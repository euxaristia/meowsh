/*
 * meowsh — POSIX-compliant shell
 * main.c — Entry point, main loop, startup files
 */

#define _POSIX_C_SOURCE 200809L

#include "shell.h"
#include "input.h"
#include "options.h"
#include "sh_error.h"
#include "memalloc.h"
#include "mystring.h"
#include "compat.h"
#include "lexer.h"
#include "parser.h"
#include "exec.h"
#include "var.h"
#include "trap.h"
#include "jobs.h"
#include "alias.h"
#include "lineedit.h"

#include <locale.h>
#include <sys/ioctl.h>
#include <stdarg.h>

static FILE *main_debug_fp;
static int main_debug_inited;

static void
main_debugf(const char *fmt, ...)
{
	const char *enabled;
	va_list ap;

	if (!main_debug_inited) {
		main_debug_inited = 1;
		enabled = getenv("MEOWSH_DEBUG_LINEEDIT");
		if (enabled && *enabled)
			main_debug_fp = fopen("/tmp/meowsh-lineedit.log", "a");
	}
	if (!main_debug_fp)
		return;

	va_start(ap, fmt);
	vfprintf(main_debug_fp, fmt, ap);
	va_end(ap);
	fputc('\n', main_debug_fp);
	fflush(main_debug_fp);
}

static void
shell_init(void)
{
	memset(&sh, 0, sizeof(sh));
	sh.shell_pid = getpid();
	sh.last_status = 0;
	sh.terminal_fd = -1;
	sh.next_job_id = 1;

	arena_init(&parse_arena);
	input_init();
	var_init();
	trap_init();
	alias_init();
	lineedit_init();
}

static void
save_history_atexit(void)
{
	if (sh.history_file) {
		history_save(sh.history_file);
	}
}

static void
setup_interactive(void)
{
	if (!sh.interactive)
		return;

	sh.opts |= OPT_INTERACTIVE;

	/* Take control of the terminal if job control */
	if (isatty(STDIN_FILENO)) {
		sh.terminal_fd = STDIN_FILENO;
		sh.opts |= OPT_MONITOR;
		jobs_init();
	}

	/* History */
	{
		const char *home = var_get("HOME");
		if (home) {
			size_t len = strlen(home) + 32;
			sh.history_file = sh_malloc(len);
			snprintf(sh.history_file, len, "%s/.meowsh_history", home);
			history_load(sh.history_file);
			atexit(save_history_atexit);
		}
	}
}

static void
source_profile(void)
{
	if (sh.login_shell) {
		if (access("/etc/profile", R_OK) == 0)
			input_push_file("/etc/profile");
		/* $HOME/.profile */
		{
			const char *home = var_get("HOME");
			if (home) {
				char path[PATH_MAX];
				snprintf(path, sizeof(path), "%s/.profile", home);
				if (access(path, R_OK) == 0)
					input_push_file(path);
			}
		}
	}

	/* $ENV for interactive shells */
	if (sh.interactive) {
		const char *env = var_get("ENV");
		if (env && *env && access(env, R_OK) == 0)
			input_push_file(env);
	}
}

static int
command_in_path(const char *name)
{
	const char *path = var_get("PATH");
	const char *p, *end;
	char fullpath[PATH_MAX];

	if (!name || !*name)
		return 0;

	if (strchr(name, '/'))
		return access(name, X_OK) == 0;

	if (!path || !*path)
		return 0;

	for (p = path; ; p = end + 1) {
		end = strchr(p, ':');
		if (!end)
			end = p + strlen(p);
		if (end == p)
			snprintf(fullpath, sizeof(fullpath), "./%s", name);
		else
			snprintf(fullpath, sizeof(fullpath), "%.*s/%s",
			    (int)(end - p), p, name);
		if (access(fullpath, X_OK) == 0)
			return 1;
		if (*end == '\0')
			break;
	}

	return 0;
}

static int
starship_disabled_by_env(const char *v)
{
	if (!v || !*v)
		return 0;
	return strcmp(v, "0") == 0 ||
	    strcmp(v, "off") == 0 ||
	    strcmp(v, "OFF") == 0 ||
	    strcmp(v, "no") == 0 ||
	    strcmp(v, "NO") == 0 ||
	    strcmp(v, "false") == 0 ||
	    strcmp(v, "FALSE") == 0;
}

static void
maybe_init_starship(void)
{
	const char *ps1;
	const char *opt;

	if (!sh.interactive)
		return;

	opt = var_get("MEOWSH_STARSHIP");
	if (starship_disabled_by_env(opt))
		return;

	ps1 = var_get("PS1");
	if (ps1 && strcmp(ps1, "meowsh % ") != 0)
		return;

	if (!command_in_path("starship"))
		return;

	sh.starship_enabled = 1;
	var_set("STARSHIP_SHELL", "bash", 1);
}

static int
terminal_width(void)
{
	struct winsize ws;
	const char *columns;
	long v;
	char *endp;

	if (isatty(STDOUT_FILENO) &&
	    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
	    ws.ws_col > 0) {
		return (int)ws.ws_col;
	}

	columns = var_get("COLUMNS");
	if (!columns || !*columns)
		return 80;

	errno = 0;
	v = strtol(columns, &endp, 10);
	if (errno != 0 || endp == columns || *endp != '\0' || v <= 0 || v > 10000)
		return 80;

	return (int)v;
}

static char *
build_starship_prompt(int status)
{
	int pipefd[2];
	pid_t pid;
	struct strbuf sb = STRBUF_INIT;
	char status_arg[32];
	char shlvl_arg[32];
	char width_arg[32];
	const char *shlvl = var_get("SHLVL");
	char *result;
	int wstatus;

	snprintf(status_arg, sizeof(status_arg), "%d", status);
	snprintf(shlvl_arg, sizeof(shlvl_arg), "%s", shlvl && *shlvl ? shlvl : "1");
	snprintf(width_arg, sizeof(width_arg), "%d", terminal_width());

	if (pipe(pipefd) < 0)
		return NULL;

	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return NULL;
	}

	if (pid == 0) {
		int devnull;

		close(pipefd[0]);
		if (dup2(pipefd[1], STDOUT_FILENO) < 0)
			_exit(127);
		close(pipefd[1]);

		devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, STDERR_FILENO);
			close(devnull);
		}

		execlp("starship", "starship", "prompt",
		    "--status", status_arg,
		    "--shlvl", shlvl_arg,
		    "--terminal-width", width_arg,
		    (char *)NULL);
		_exit(127);
	}

	close(pipefd[1]);
	for (;;) {
		char buf[512];
		ssize_t n = read(pipefd[0], buf, sizeof(buf));
		if (n <= 0)
			break;
		strbuf_addmem(&sb, buf, (size_t)n);
	}
	close(pipefd[0]);

	if (waitpid(pid, &wstatus, 0) < 0) {
		strbuf_free(&sb);
		return NULL;
	}
	if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0 || sb.len == 0) {
		strbuf_free(&sb);
		return NULL;
	}

	while (sb.len > 0 && sb.buf[sb.len - 1] == '\n')
		sb.buf[--sb.len] = '\0';

	result = strbuf_detach(&sb);
	if (!result[0]) {
		free(result);
		return NULL;
	}
	return result;
}

static char *
with_meow_marker(const char *prompt)
{
	const char *marker = "🐱 ";
	const char *insert_at;
	const char *style_prefix_end;
	const char *p;
	const char *nl;
	size_t marker_len;
	size_t prefix_len;
	size_t style_prefix_len;
	size_t suffix_len;
	char *out;
	int matched = 0;

	if (!prompt)
		return NULL;

	insert_at = prompt;
	for (nl = prompt; *nl; nl++) {
		if (*nl == '\n')
			insert_at = nl + 1;
	}

	style_prefix_end = insert_at;
	while (*style_prefix_end == '\x1b' && style_prefix_end[1] == '[') {
		const char *q = style_prefix_end + 2;
		while (*q && !(*q >= '@' && *q <= '~'))
			q++;
		if (!*q)
			break;
		style_prefix_end = q + 1;
	}

	p = style_prefix_end;
	if ((unsigned char)p[0] == 0xE2 &&
	    (unsigned char)p[1] == 0x9D &&
	    (unsigned char)p[2] == 0xAF) {
		p += 3;
		matched = 1;
	} else if (*p == '>' || *p == '$') {
		p++;
		matched = 1;
	}
	if (matched) {
		while (*p == ' ' || *p == '\t')
			p++;
	}

	marker_len = strlen(marker);
	prefix_len = (size_t)(insert_at - prompt);
	style_prefix_len = (size_t)(style_prefix_end - insert_at);
	suffix_len = strlen(matched ? p : insert_at);

	out = sh_malloc(prefix_len + style_prefix_len + marker_len + suffix_len + 1);
	memcpy(out, prompt, prefix_len);
	memcpy(out + prefix_len, insert_at, style_prefix_len);
	memcpy(out + prefix_len + style_prefix_len, marker, marker_len);
	if (matched) {
		memcpy(out + prefix_len + style_prefix_len + marker_len, p, suffix_len + 1);
	} else {
		memcpy(out + prefix_len + style_prefix_len + marker_len, insert_at, suffix_len + 1);
	}
	return out;
}

static void
main_loop(void)
{
	struct node *tree;
	char ps1_buf[PATH_MAX + 64];
	const char *ps2;
	char *starship_ps1 = NULL;

	for (;;) {
		free(starship_ps1);
		starship_ps1 = NULL;

		/* Process any pending traps */
		trap_check();

		/* Check for dead children */
		if (sh.interactive)
			jobs_reap();

		/* Parse one complete command */
		arena_free(&parse_arena);

		if (sh.interactive) {
			const char *cfg_ps1 = var_get("PS1");
			const char *cfg_ps2 = var_get("PS2");
			const char *pwd = var_get("PWD");
			const char *user = var_get("USER");
			char short_pwd[PATH_MAX];
			if (!user) user = "meow";
			if (!pwd) pwd = "?";
			
			if ((!cfg_ps1 || strcmp(cfg_ps1, "meowsh % ") == 0) &&
			    sh.starship_enabled) {
				starship_ps1 = build_starship_prompt(sh.last_status);
				if (starship_ps1) {
					char *marked = with_meow_marker(starship_ps1);
					free(starship_ps1);
					starship_ps1 = marked;
					sh.ps1 = starship_ps1;
				} else {
						shorten_path(short_pwd, pwd, sizeof(short_pwd));
						snprintf(ps1_buf, sizeof(ps1_buf),
						    "\x1b[32m%s\x1b[0m \x1b[34m%s\x1b[0m 🐱 ",
						    user, short_pwd);
						sh.ps1 = ps1_buf;
					}
				} else if (!cfg_ps1 || strcmp(cfg_ps1, "meowsh % ") == 0) {
					shorten_path(short_pwd, pwd, sizeof(short_pwd));

					/* Fish-style prompt: [user] /s/p/path $ */
					snprintf(ps1_buf, sizeof(ps1_buf),
					    "\x1b[32m%s\x1b[0m \x1b[34m%s\x1b[0m 🐱 ",
					    user, short_pwd);
					sh.ps1 = ps1_buf;
				} else {
					sh.ps1 = cfg_ps1;
				}
				ps2 = cfg_ps2 && *cfg_ps2 ? cfg_ps2 : "🐱 ";
			sh.ps2 = ps2;
			sh.cur_prompt = sh.ps1;
		} else {
			ps1_buf[0] = '\0';
			sh.ps1 = NULL;
			sh.ps2 = NULL;
			sh.cur_prompt = NULL;
		}

		tree = parse_command(sh.interactive ? sh.ps1 : NULL, sh.ps2);
		if (!tree) {
			main_debugf("parse_command -> NULL interactive=%d eof=%d",
			    sh.interactive, (sh.input ? sh.input->eof : -1));
			/* Empty line / interrupted line / EOF. Avoid probing input in interactive
			 * mode because that can trigger extra prompt reads and visual artifacts. */
			if (sh.input && sh.input->eof)
				break;
			if (!sh.interactive) {
				int c = input_getc();
				if (c < 0)
					break;
				input_ungetc(c);
			}
			continue;
			}

		/* Execute */
		if (!option_is_set(OPT_NOEXEC))
			exec_node(tree, 0);
	}

	free(starship_ps1);
}

int
main(int argc, char **argv)
{
	int optind;
	char **envp;

	setlocale(LC_ALL, "");

	shell_init();
	sh.argv0 = argv[0];

	/* Import environment */
	{
		extern char **environ;
		envp = environ;
		if (envp) {
			for (; *envp; envp++)
				var_import(*envp);
		}
	}

	/* Parse command-line options */
	optind = options_parse(argc, argv);

	if (optind < 0) {
		/* -c mode */
		int idx = -optind;
		const char *cmd = argv[idx];

		if (idx + 1 < argc)
			sh.argv0 = argv[idx + 1];

		/* Set positional params from remaining args */
		if (idx + 2 < argc)
			var_set_posparams(argc - idx - 2, argv + idx + 2);

		input_push_string(cmd);
		main_loop();
		input_pop();
		return sh.last_status;
	}

	if (optind < argc) {
		/* Script file mode */
		char *script = argv[optind];
		sh.argv0 = script;

		if (optind + 1 < argc)
			var_set_posparams(argc - optind - 1, argv + optind + 1);

		input_push_file(script);
		if (!sh.input)
			return 127;
		main_loop();
		input_pop();
		return sh.last_status;
	}

	/* Interactive / stdin mode */
	if (isatty(STDIN_FILENO))
		sh.interactive = 1;

	setup_interactive();
	source_profile();
	maybe_init_starship();

	if (sh.interactive) {
		fprintf(stderr, "meowsh — welcome! (type 'exit' to quit)\n");
	}

	input_push_fd(STDIN_FILENO);
	main_loop();
	input_pop();

	if (sh.interactive)
		fputc('\n', stderr);

	return sh.last_status;
}

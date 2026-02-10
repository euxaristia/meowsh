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

/* Global shell state */
struct shell_state sh;

unsigned int
hash_string(const char *s)
{
	unsigned int h = 5381;

	while (*s)
		h = ((h << 5) + h) + (unsigned char)*s++;
	return h % HASH_SIZE;
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

static void
main_loop(void)
{
	struct node *tree;
	const char *ps1, *ps2;

	for (;;) {
		/* Process any pending traps */
		trap_check();

		/* Check for dead children */
		if (sh.interactive)
			jobs_reap();

		/* Parse one complete command */
		arena_free(&parse_arena);

		if (sh.interactive) {
			ps1 = var_get("PS1");
			ps2 = var_get("PS2");
			if (!ps1)
				ps1 = "$ ";
			if (!ps2)
				ps2 = "> ";
		} else {
			ps1 = NULL;
			ps2 = NULL;
		}

		tree = parse_command(ps1, ps2);
		if (!tree) {
			if (input_getc() < 0) {
				/* EOF */
				break;
			}
			input_ungetc(0); /* shouldn't happen */
			continue;
		}

		/* Execute */
		if (!option_is_set(OPT_NOEXEC))
			exec_node(tree, 0);
	}
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

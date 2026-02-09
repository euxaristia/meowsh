/*
 * meowsh — POSIX-compliant shell
 * options.c — Shell option flags
 */

#define _POSIX_C_SOURCE 200809L

#include "shell.h"
#include "options.h"
#include "error.h"
#include "memalloc.h"

#include <stdio.h>
#include <string.h>

struct optmap {
	char letter;
	const char *name;
	unsigned int flag;
};

static const struct optmap optmap[] = {
	{ 'a', "allexport",  OPT_ALLEXPORT },
	{ 'e', "errexit",    OPT_ERREXIT },
	{ 'f', "noglob",     OPT_NOGLOB },
	{ 'h', "hashall",    OPT_HASHALL },
	{ 'i', "interactive",OPT_INTERACTIVE },
	{ 'm', "monitor",    OPT_MONITOR },
	{ 'n', "noexec",     OPT_NOEXEC },
	{ 'u', "nounset",    OPT_NOUNSET },
	{ 'v', "verbose",    OPT_VERBOSE },
	{ 'x', "xtrace",     OPT_XTRACE },
	{ 'C', "noclobber",  OPT_NOCLOBBER },
	{ 0, NULL, 0 }
};

int
option_set_flag(char c, int set)
{
	const struct optmap *om;

	for (om = optmap; om->letter; om++) {
		if (om->letter == c) {
			if (set)
				sh.opts |= om->flag;
			else
				sh.opts &= ~om->flag;
			return 0;
		}
	}
	return -1;
}

int
option_is_set(unsigned int flag)
{
	return (sh.opts & flag) != 0;
}

int
options_set(const char *s)
{
	int set;
	const char *p;

	if (!s || !*s)
		return -1;
	if (*s == '-')
		set = 1;
	else if (*s == '+')
		set = 0;
	else
		return -1;

	for (p = s + 1; *p; p++) {
		if (option_set_flag(*p, set) < 0) {
			sh_error("illegal option -%c", *p);
			return -1;
		}
	}
	return 0;
}

static int
option_by_name(const char *name, int set)
{
	const struct optmap *om;

	for (om = optmap; om->letter; om++) {
		if (strcmp(om->name, name) == 0) {
			if (set)
				sh.opts |= om->flag;
			else
				sh.opts &= ~om->flag;
			return 0;
		}
	}
	sh_error("unknown option: %s", name);
	return -1;
}

void
options_print(int verbose)
{
	const struct optmap *om;

	if (verbose) {
		for (om = optmap; om->letter; om++) {
			fprintf(stdout, "set %co %s\n",
			    (sh.opts & om->flag) ? '-' : '+',
			    om->name);
		}
	} else {
		for (om = optmap; om->letter; om++) {
			if (sh.opts & om->flag)
				fprintf(stdout, "-%c ", om->letter);
		}
		fputc('\n', stdout);
	}
}

char *
options_to_string(void)
{
	char buf[32];
	char *p = buf;
	const struct optmap *om;

	*p++ = '-';
	for (om = optmap; om->letter; om++) {
		if (sh.opts & om->flag)
			*p++ = om->letter;
	}
	*p = '\0';
	return sh_strdup(buf);
}

int
options_parse(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++) {
		if (argv[i][0] == '-' || argv[i][0] == '+') {
			int set = (argv[i][0] == '-');

			if (argv[i][1] == '\0') {
				/* bare "-": turn off -x and -v */
				if (set) {
					sh.opts &= ~(OPT_XTRACE | OPT_VERBOSE);
					continue;
				}
			}

			if (strcmp(argv[i], "--") == 0) {
				i++;
				break;
			}

			if (argv[i][1] == 'c' && set) {
				/* -c string */
				i++;
				if (i >= argc)
					sh_fatal("-c requires an argument");
				return -(i); /* negative signals -c mode */
			}

			if (argv[i][1] == 's' && set) {
				/* -s: read from stdin */
				continue;
			}

			if (argv[i][1] == 'o' && set) {
				/* -o name or +o name */
				i++;
				if (i >= argc)
					sh_fatal("-o requires an argument");
				option_by_name(argv[i], set);
				continue;
			}

			if (argv[i][1] == 'o' && !set) {
				i++;
				if (i >= argc)
					sh_fatal("+o requires an argument");
				option_by_name(argv[i], set);
				continue;
			}

			/* Normal option flags */
			{
				const char *p;
				for (p = argv[i] + 1; *p; p++) {
					if (option_set_flag(*p, set) < 0) {
						if (set && *p == 'l') {
							sh.login_shell = 1;
							continue;
						}
						sh_error("illegal option %c%c",
						    argv[i][0], *p);
					}
				}
			}
		} else {
			/* Script file argument */
			break;
		}
	}
	return i;
}

#define _POSIX_C_SOURCE 200809L

#include "crt.h"
#include "shell.h"
#include "var.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void slow_print(const char *s, int ms) {
  while (*s) {
    fputc(*s++, stdout);
    fflush(stdout);
    if (ms > 0) {
      struct timespec ts = {0, ms * 1000000L};
      nanosleep(&ts, NULL);
    }
  }
}

void crt_boot_sequence(void) {
  const char *crt = var_get("MEOWSH_CRT");
  if (!crt || strcmp(crt, "1") != 0)
    return;

  /* Clear screen and go to top */
  printf("\x1b[2J\x1b[H");
  fflush(stdout);

  slow_print("\x1b[32mMEOWSH(tm) BIOS v1.0.3 (c) 1982-2026\n", 5);
  slow_print("CPU: CAT-8086 @ 4.77MHz\n", 10);
  slow_print("MEMORY: 640KB RAM OK\n\n", 20);

  slow_print("CHECKING DRIVES...\n", 50);
  slow_print("  FD0: 3.5\" FLOPPY - OK\n", 100);
  slow_print("  HD0: 20MB Winchester - OK\n\n", 100);

  slow_print("LOADING SYSTEM MODULES:\n", 50);
  slow_print("  LEXER.SYS  [##########] 100%\n", 20);
  slow_print("  PARSER.SYS [##########] 100%\n", 20);
  slow_print("  EXEC.SYS   [##########] 100%\n", 20);
  slow_print("  VAR.SYS    [##########] 100%\n\n", 20);

  slow_print("INITIALIZING MEOW-SUBSYSTEM...\n", 100);
  slow_print("BOOTING MEOWSH INTERACTIVE SHELL...\n\n", 150);

  struct timespec ts = {1, 0};
  nanosleep(&ts, NULL);

  /* Clear screen and reset colors */
  printf("\x1b[2J\x1b[H\x1b[0m");
  fflush(stdout);

  const char *user = var_get("USER");
  const char *pwd = var_get("PWD");
  char short_pwd[PATH_MAX]; // flawfinder: ignore
  if (!user)
    user = "meow";
  if (!pwd)
    pwd = "?";
  shorten_path(short_pwd, pwd, sizeof(short_pwd));
  printf("\x1b[32m[SYSTEM READY] %s @ %s\x1b[0m\n\n", user, short_pwd);
  fflush(stdout);
}

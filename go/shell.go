package main

import (
	"fmt"
	"os"
	"strings"
	"syscall"
	"unicode"
	"unsafe"
)

func shellInit() {
	sh.ShellPid = os.Getpid()
	sh.LastStatus = 0
	sh.NextJobId = 1
	sh.Vars = make(map[string]Var)
	sh.Aliases = make(map[string]string)
	sh.Functions = make(map[string]*FuncDef)
	sh.Trap = make(map[string]string)

	sh.Vars["HOME"] = Var{Value: getEnv("HOME", ""), Flags: 0}
	sh.Vars["PATH"] = Var{Value: getEnv("PATH", "/usr/local/bin:/usr/bin:/bin"), Flags: VAR_EXPORT}
	sh.Vars["USER"] = Var{Value: getEnv("USER", "meow"), Flags: 0}
	sh.Vars["PWD"] = Var{Value: getEnv("PWD", "/"), Flags: 0}
	sh.Vars["SHELL"] = Var{Value: "/bin/meowsh", Flags: 0}
	sh.Vars["TERM"] = Var{Value: getEnv("TERM", "dumb"), Flags: 0}
	sh.Vars["PS1"] = Var{Value: "𓃠 ", Flags: 0}
	sh.Vars["PS2"] = Var{Value: "𓃠 ", Flags: 0}
	sh.Vars["SHLVL"] = Var{Value: "1", Flags: 0}

	jobsInit()
}

func getEnv(key, def string) string {
	if val := os.Getenv(key); val != "" {
		return val
	}
	return def
}

func varGet(name string) string {
	switch name {
	case "$":
		return fmt.Sprintf("%d", sh.ShellPid)
	case "?":
		return fmt.Sprintf("%d", sh.LastStatus)
	case "!":
		return fmt.Sprintf("%d", sh.LastBgPid)
	case "0":
		return sh.Argv1()
	case "#":
		return fmt.Sprintf("%d", len(sh.PosParams))
	case "@", "*":
		return strings.Join(sh.PosParams, " ")
	case "-":
		flags := ""
		if sh.Opts&OPT_ALLEXPORT != 0 {
			flags += "a"
		}
		if sh.Opts&OPT_ERREXIT != 0 {
			flags += "e"
		}
		if sh.Opts&OPT_NOGLOB != 0 {
			flags += "f"
		}
		if sh.Opts&OPT_HASHALL != 0 {
			flags += "h"
		}
		if sh.Opts&OPT_INTERACTIVE != 0 {
			flags += "i"
		}
		if sh.Opts&OPT_MONITOR != 0 {
			flags += "m"
		}
		if sh.Opts&OPT_NOEXEC != 0 {
			flags += "n"
		}
		if sh.Opts&OPT_NOUNSET != 0 {
			flags += "u"
		}
		if sh.Opts&OPT_VERBOSE != 0 {
			flags += "v"
		}
		if sh.Opts&OPT_XTRACE != 0 {
			flags += "x"
		}
		return flags
	}

	if len(name) > 1 && name[0] == '-' {
		return varGet(name[1:])
	}

	if v, ok := sh.Vars[name]; ok {
		return v.Value
	}
	return ""
}

func varSet(name, value string, export bool) {
	flags := sh.Vars[name].Flags
	if export {
		flags |= VAR_EXPORT
	}
	sh.Vars[name] = Var{Value: value, Flags: flags}
	os.Setenv(name, value)
}

func varUnset(name string) error {
	if v, ok := sh.Vars[name]; ok {
		if v.Flags&VAR_READONLY != 0 {
			return fmt.Errorf("cannot unset readonly variable %s", name)
		}
	}
	delete(sh.Vars, name)
	os.Unsetenv(name)
	return nil
}

func (s *Shell) Argv1() string {
	if len(s.PosParams) > 0 {
		return s.PosParams[0]
	}
	return s.Argv0
}

func shortenPath(p string) string {
	home := varGet("HOME")
	if home != "" && strings.HasPrefix(p, home) {
		return "~" + strings.TrimPrefix(p, home)
	}

	parts := strings.Split(p, "/")
	if len(parts) <= 3 {
		return p
	}

	var result []string
	for i, part := range parts {
		if i == 0 && part == "" {
			result = append(result, "/")
		} else if i == len(parts)-1 {
			result = append(result, part)
		} else if part != "" {
			result = append(result, string(part[0]))
		}
	}
	return strings.Join(result, "/")
}

func buildPrompt() string {
	user := varGet("USER")
	if user == "" {
		user = "meow"
	}
	pwd := varGet("PWD")
	if pwd == "" {
		pwd = "?"
	}

	shortPwd := shortenPath(pwd)

	return fmt.Sprintf("\033[32m%s\033[0m \033[34m%s\033[0m 𓃠  ", user, shortPwd)
}

func isatty(fd uintptr) bool {
	var termios syscall.Termios
	_, _, err := syscall.Syscall(syscall.SYS_IOCTL, fd, uintptr(syscall.TCGETS), uintptr(unsafe.Pointer(&termios)))
	return err == 0
}

func isAssignment(s string) bool {
	for i, c := range s {
		if c == '=' {
			return i > 0
		}
		if !unicode.IsLetter(c) && !unicode.IsDigit(c) && c != '_' {
			return false
		}
	}
	return false
}

package main

import (
	"bufio"
	"fmt"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
)

func builtinCd(args []string) int {
	dir := "/"
	if len(args) > 0 && args[0] != "" {
		if args[0] == "-" {
			dir = os.Getenv("OLDPWD")
			if dir == "" {
				fmt.Println("meowsh: cd: OLDPWD not set")
				return 1
			}
			fmt.Println(dir)
		} else if args[0] == "~" || strings.HasPrefix(args[0], "~/") {
			home := varGet("HOME")
			dir = filepath.Join(home, strings.TrimPrefix(args[0], "~/"))
		} else {
			dir = args[0]
		}
	} else {
		dir = varGet("HOME")
	}

	if !filepath.IsAbs(dir) {
		dir = filepath.Join(varGet("PWD"), dir)
	}

	info, err := os.Stat(dir)
	if err != nil {
		fmt.Printf("meowsh: cd: %s: no such file or directory\n", args[0])
		return 1
	}
	if !info.IsDir() {
		fmt.Printf("meowsh: cd: %s: not a directory\n", args[0])
		return 1
	}

	absDir, err := filepath.Abs(dir)
	if err != nil {
		return 1
	}

	os.Setenv("OLDPWD", varGet("PWD"))
	varSet("PWD", absDir, false)
	os.Setenv("PWD", absDir)

	return 0
}

func builtinSet(args []string) int {
	if len(args) == 0 {
		for name, v := range sh.Vars {
			if v.Flags&VAR_EXPORT != 0 {
				fmt.Printf("export %s=%q\n", name, v.Value)
			} else {
				fmt.Printf("%s=%q\n", name, v.Value)
			}
		}
		return 0
	}

	for _, arg := range args {
		if strings.HasPrefix(arg, "-") {
			for _, c := range arg[1:] {
				switch c {
				case 'a':
					sh.Opts |= OPT_ALLEXPORT
				case 'e':
					sh.Opts |= OPT_ERREXIT
				case 'f':
					sh.Opts |= OPT_NOGLOB
				case 'h':
					sh.Opts |= OPT_HASHALL
				case 'i':
					sh.Opts |= OPT_INTERACTIVE
				case 'm':
					sh.Opts |= OPT_MONITOR
				case 'n':
					sh.Opts |= OPT_NOEXEC
				case 'u':
					sh.Opts |= OPT_NOUNSET
				case 'v':
					sh.Opts |= OPT_VERBOSE
				case 'x':
					sh.Opts |= OPT_XTRACE
				case 'C':
					sh.Opts |= OPT_NOCLOBBER
				}
			}
		} else if strings.Contains(arg, "=") {
			parts := strings.SplitN(arg, "=", 2)
			varSet(parts[0], parts[1], false)
		}
	}
	return 0
}

func builtinExport(args []string) int {
	if len(args) == 0 {
		for name, v := range sh.Vars {
			if v.Flags&VAR_EXPORT != 0 {
				fmt.Printf("export %s=%q\n", name, v.Value)
			}
		}
		return 0
	}

	for _, arg := range args {
		parts := strings.SplitN(arg, "=", 2)
		name := parts[0]
		if len(parts) == 2 {
			varSet(name, parts[1], true)
		} else {
			if v, ok := sh.Vars[name]; ok {
				sh.Vars[name] = Var{Value: v.Value, Flags: v.Flags | VAR_EXPORT}
			}
		}
	}
	return 0
}

func builtinUnset(args []string) int {
	for _, name := range args {
		err := varUnset(name)
		if err != nil {
			fmt.Printf("meowsh: unset: %v\n", err)
		}
	}
	return 0
}

func builtinAlias(args []string) int {
	if len(args) == 0 {
		for name, val := range sh.Aliases {
			fmt.Printf("alias %s=%q\n", name, val)
		}
		return 0
	}

	for _, arg := range args {
		parts := strings.SplitN(arg, "=", 2)
		if len(parts) == 2 {
			sh.Aliases[parts[0]] = parts[1]
		}
	}
	return 0
}

func builtinUnalias(args []string) int {
	for _, name := range args {
		delete(sh.Aliases, name)
	}
	return 0
}

func builtinRead(args []string) int {
	reader := bufio.NewReader(os.Stdin)
	line, err := reader.ReadString('\n')
	if err != nil {
		return 1
	}
	line = strings.TrimSpace(line)
	fields := strings.Fields(line)

	if len(args) == 0 {
		if len(fields) > 0 {
			varSet("REPLY", fields[0], false)
		}
		return 0
	}

	for i, name := range args {
		if i < len(fields) {
			varSet(name, fields[i], false)
		} else {
			varSet(name, "", false)
		}
	}
	return 0
}

func builtinShift(args []string) int {
	n := 1
	if len(args) > 0 {
		fmt.Sscanf(args[0], "%d", &n)
	}
	if n > len(sh.PosParams) {
		fmt.Println("shift: cannot shift: not enough arguments")
		return 1
	}
	sh.PosParams = sh.PosParams[n:]
	return 0
}

func builtinLocal(args []string) int {
	for _, arg := range args {
		parts := strings.SplitN(arg, "=", 2)
		if len(parts) == 2 {
			varSet(parts[0], parts[1], false)
		}
	}
	return 0
}

func builtinReadonly(args []string) int {
	for name, v := range sh.Vars {
		if v.Flags&VAR_READONLY != 0 {
			fmt.Printf("readonly %s=%q\n", name, v.Value)
		}
	}
	return 0
}

func builtinEval(args []string) int {
	if len(args) == 0 {
		return 0
	}

	line := strings.Join(args, " ")
	tokens := tokenize(line)
	_ = tokens // tokens is unused by the new parser architecture unless we wrap it
	node := NewParser(NewLexer(line)).Parse()
	if node != nil {
		return execNode(node)
	}
	return 0
}

func builtinSource(args []string) int {
	if len(args) == 0 {
		fmt.Println("source: filename argument required")
		return 1
	}

	filename := args[0]
	if !filepath.IsAbs(filename) {
		filename = filepath.Join(varGet("PWD"), filename)
	}

	file, err := os.Open(filename)
	if err != nil {
		fmt.Printf("source: %s: %v\n", args[0], err)
		return 1
	}
	defer file.Close()

	scanner := bufio.NewScanner(file)
	for scanner.Scan() {
		line := scanner.Text()
		node := NewParser(NewLexer(line)).Parse()
		if node != nil {
			execNode(node)
		}
	}

	return 0
}

func builtinType(args []string) int {
	if len(args) == 0 {
		return 1
	}

	name := args[0]

	if _, ok := sh.Aliases[name]; ok {
		fmt.Printf("%s is an alias\n", name)
		return 0
	}

	if _, ok := sh.Functions[name]; ok {
		fmt.Printf("%s is a function\n", name)
		return 0
	}

	builtins := map[string]bool{
		"alias": true, "bg": true, "cd": true, "continue": true,
		"echo": true, "eval": true, "exit": true, "export": true,
		"false": true, "fg": true, "for": true, "function": true,
		"if": true, "jobs": true, "kill": true, "local": true,
		"read": true, "readonly": true, "return": true, "set": true,
		"shift": true, "source": true, "then": true, "trap": true,
		"true": true, "type": true, "ulimit": true, "umask": true,
		"unalias": true, "unset": true, "wait": true, "while": true,
		"until": true, "case": true, "esac": true, "do": true, "done": true,
		"fi": true, "else": true, "elif": true, "in": true, "{": true, "}": true,
	}

	if builtins[name] {
		fmt.Printf("%s is a shell builtin\n", name)
		return 0
	}

	pathDirs := strings.Split(varGet("PATH"), ":")
	for _, dir := range pathDirs {
		exePath := filepath.Join(dir, name)
		if _, err := os.Stat(exePath); err == nil {
			fmt.Printf("%s is %s\n", name, exePath)
			return 0
		}
	}

	fmt.Printf("%s: not found\n", name)
	return 1
}

func builtinTest(args []string) int {
	if len(args) == 0 {
		return 1
	}

	argslen := len(args)
	if args[argslen-1] == "]" {
		args = args[:argslen-1]
	}

	if len(args) == 1 {
		s := args[0]
		if s == "" || s == "0" || s == "false" {
			return 1
		}
		return 0
	}

	if len(args) == 2 {
		op := args[0]
		s := args[1]
		switch op {
		case "!":
			if s == "" || s == "0" || s == "false" {
				return 0
			}
			return 1
		case "-z":
			if s == "" {
				return 0
			}
			return 1
		case "-n":
			if s != "" {
				return 0
			}
			return 1
		case "-d":
			if info, err := os.Stat(s); err == nil && info.IsDir() {
				return 0
			}
			return 1
		case "-f":
			if info, err := os.Stat(s); err == nil && info.Mode().IsRegular() {
				return 0
			}
			return 1
		case "-e":
			if _, err := os.Stat(s); err == nil {
				return 0
			}
			return 1
		}
		return 1
	}

	if len(args) == 3 {
		a := args[0]
		op := args[1]
		b := args[2]

		a = expandAll(a)
		b = expandAll(b)

		switch op {
		case "-eq":
			ai, _ := strconv.Atoi(a)
			bi, _ := strconv.Atoi(b)
			if ai == bi {
				return 0
			}
			return 1
		case "-ne":
			ai, _ := strconv.Atoi(a)
			bi, _ := strconv.Atoi(b)
			if ai != bi {
				return 0
			}
			return 1
		case "-lt":
			ai, _ := strconv.Atoi(a)
			bi, _ := strconv.Atoi(b)
			if ai < bi {
				return 0
			}
			return 1
		case "-le":
			ai, _ := strconv.Atoi(a)
			bi, _ := strconv.Atoi(b)
			if ai <= bi {
				return 0
			}
			return 1
		case "-gt":
			ai, _ := strconv.Atoi(a)
			bi, _ := strconv.Atoi(b)
			if ai > bi {
				return 0
			}
			return 1
		case "-ge":
			ai, _ := strconv.Atoi(a)
			bi, _ := strconv.Atoi(b)
			if ai >= bi {
				return 0
			}
			return 1
		case "=":
			if a == b {
				return 0
			}
			return 1
		case "!=":
			if a != b {
				return 0
			}
			return 1
		}
		return 1
	}

	return 1
}

func builtinJobs(args []string) int {
	for _, job := range sh.Jobs {
		fmt.Printf("[%d] %d %s\n", job.Id, job.Pid, job.Cmd)
	}
	return 0
}

func builtinFg(args []string) int {
	return 0
}

func builtinBg(args []string) int {
	return 0
}

func builtinKill(args []string) int {
	if len(args) < 1 {
		fmt.Println("kill: usage: kill pid")
		return 1
	}

	signal := "SIGTERM"
	pidStr := args[0]

	if strings.HasPrefix(args[0], "-") {
		if len(args) < 2 {
			fmt.Println("kill: usage: kill [-signal] pid")
			return 1
		}
		signal = strings.TrimPrefix(args[0], "-")
		pidStr = args[1]
	}

	pid, err := strconv.Atoi(pidStr)
	if err != nil {
		fmt.Printf("kill: %s: invalid pid\n", pidStr)
		return 1
	}

	sysSig := syscall.SIGTERM
	if signal == "KILL" {
		sysSig = syscall.SIGKILL
	} else if signal == "INT" {
		sysSig = syscall.SIGINT
	} else if signal == "TERM" {
		sysSig = syscall.SIGTERM
	}

	err = syscall.Kill(pid, sysSig)
	if err != nil {
		fmt.Printf("kill: %s: %v\n", pidStr, err)
		return 1
	}
	return 0
}

func builtinWait(args []string) int {
	return 0
}

func builtinTrap(args []string) int {
	if len(args) == 0 {
		for sig, cmd := range sh.Trap {
			fmt.Printf("trap -- '%s' %s\n", cmd, sig)
		}
		return 0
	}

	if len(args) == 1 {
		return 0
	}

	cmd := args[0]
	sig := args[1]
	sh.Trap[sig] = cmd

	sigNum := signalNum(sig)
	if sigNum != nil {
		c := make(chan os.Signal, 1)
		signal.Notify(c, sigNum)
		go func() {
			for range c {
				if cmd != "" && cmd != "-" {
					execNode(&ASTNode{Type: "simple", Args: strings.Fields(cmd)})
				}
			}
		}()
	}

	return 0
}

func signalNum(s string) os.Signal {
	switch s {
	case "HUP", "SIGHUP":
		return syscall.SIGHUP
	case "INT", "SIGINT":
		return syscall.SIGINT
	case "QUIT", "SIGQUIT":
		return syscall.SIGQUIT
	case "TERM", "SIGTERM":
		return syscall.SIGTERM
	case "USR1", "SIGUSR1":
		return syscall.SIGUSR1
	case "USR2", "SIGUSR2":
		return syscall.SIGUSR2
	}
	return nil
}

func builtinUmask(args []string) int {
	if len(args) == 0 {
		oldMask := syscall.Umask(0)
		syscall.Umask(oldMask)
		fmt.Printf("%04o\n", oldMask)
		return 0
	}

	mask, err := strconv.ParseInt(args[0], 8, 32)
	if err != nil {
		fmt.Printf("umask: %s: invalid umask\n", args[0])
		return 1
	}
	syscall.Umask(int(mask))
	return 0
}

func builtinUlimit(args []string) int {
	fmt.Println("unlimited")
	return 0
}

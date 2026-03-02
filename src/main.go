package main

import (
	"fmt"
	"io"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
	"time"
	"unsafe"
)

var sh Shell

func mainLoop() {
	le := NewLineEditor()
	if sh.Interactive {
		le.LoadHistory(sh.HistoryFile)
	}
	for {
		prompt := ""
		if sh.Interactive {
			jobsNotify()
			prompt = buildPrompt()
		}

		// Split multi-line prompts: print header lines directly,
		// only pass the last line to ReadLine for in-place editing.
		if idx := strings.LastIndex(prompt, "\n"); idx >= 0 {
			fmt.Print(prompt[:idx+1])
			prompt = prompt[idx+1:]
		}

		line, err := le.ReadLine(prompt)
		if err != nil {
			if err == io.EOF {
				break
			}
			// Prevent busy loop on errors
			time.Sleep(100 * time.Millisecond)
			continue
		}

		if strings.TrimSpace(line) == "" {
			continue
		}

		if sh.Interactive {
			le.AddHistory(line)
			le.SaveHistory(sh.HistoryFile)
		}

		sh.Lineno++
		line = expandAliasLine(line)

		lexer := NewLexerWithPromptReader(line+"\n", le)
		parser := NewParser(lexer)
		node := parser.Parse()

		if node != nil {
			status := execNode(node)
			sh.LastStatus = status
		}
	}

	if sh.Interactive {
		fmt.Println()
	}
}

func main() {
	shellInit()

	sh.Argv0 = os.Args[0]
	if len(os.Args) > 0 && strings.HasPrefix(os.Args[0], "-") {
		sh.LoginShell = true
	}

	for _, env := range os.Environ() {
		parts := strings.SplitN(env, "=", 2)
		if len(parts) == 2 {
			varSet(parts[0], parts[1], true)
		}
	}

	// Simple CLI parsing for prototype parity
	isCmode := false
	cmdLine := ""
	scriptFile := ""

	for i := 1; i < len(os.Args); i++ {
		arg := os.Args[i]
		if arg == "-c" {
			isCmode = true
			if i+1 < len(os.Args) {
				cmdLine = os.Args[i+1]
			}
			break
		} else if !strings.HasPrefix(arg, "-") {
			scriptFile = arg
			break
		}
	}

	if isCmode {
		lexer := NewLexer(cmdLine)
		parser := NewParser(lexer)
		node := parser.Parse()
		if node != nil {
			os.Exit(execNode(node))
		}
		os.Exit(0)
	}

	if scriptFile != "" {
		data, err := os.ReadFile(scriptFile)
		if err != nil {
			fmt.Fprintf(os.Stderr, "meowsh: %s: %v\n", scriptFile, err)
			os.Exit(127)
		}
		lexer := NewLexer(string(data))
		parser := NewParser(lexer)
		node := parser.Parse()
		if node != nil {
			os.Exit(execNode(node))
		}
		os.Exit(0)
	}

	if isatty(os.Stdin.Fd()) && os.Getenv("TERM") != "" && os.Getenv("TERM") != "dumb" {
		sh.Interactive = true
		sh.Opts |= OPT_INTERACTIVE
		sh.Opts |= OPT_MONITOR
	}

	if sh.Interactive {
		// Ignore terminal control signals
		signal.Ignore(syscall.SIGINT, syscall.SIGTERM, syscall.SIGQUIT, syscall.SIGTSTP, syscall.SIGTTIN, syscall.SIGTTOU)

		sh.ShellPid = os.Getpid()
		if pgrp, _ := syscall.Getpgid(sh.ShellPid); pgrp != sh.ShellPid {
			if err := syscall.Setpgid(sh.ShellPid, sh.ShellPid); err != nil {
				fmt.Fprintf(os.Stderr, "meowsh: setpgid: %v\n", err)
			}
		}

		// Take control of the terminal
		if isatty(os.Stdin.Fd()) {
			syscall.Syscall(syscall.SYS_IOCTL, os.Stdin.Fd(), uintptr(syscall.TIOCSPGRP), uintptr(unsafe.Pointer(&sh.ShellPid)))
		}

		fmt.Fprintf(os.Stderr, "meowsh — welcome! (type 'exit' to quit)\n")
	}

	if sh.LoginShell {
		home := varGet("HOME")
		rcPath := filepath.Join(home, ".meowshrc")
		if _, err := os.Stat(rcPath); err == nil {
			builtinSource([]string{rcPath})
		}
	}

	mainLoop()
}

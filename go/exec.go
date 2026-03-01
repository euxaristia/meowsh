package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
)

func execNode(node *ASTNode) int {
	if node == nil || node.Type == "empty" {
		return 0
	}

	switch node.Type {
	case "simple":
		return execSimple(node)
	case "pipeline":
		return execPipeline(node)
	case "and_or":
		return execAndOr(node)
	case "list":
		return execList(node)
	case "if":
		return execIf(node)
	case "while":
		return execWhile(node)
	case "until":
		return execUntil(node)
	case "for":
		return execFor(node)
	case "brace_group", "subshell":
		return execNode(node.Body)
	case "function":
		return execFunction(node)
	case "case":
		return 0
	}

	return 0
}

func execSimple(node *ASTNode) int {
	if len(node.Args) == 0 && len(node.Assigns) == 0 {
		return 0
	}

	for name, value := range node.Assigns {
		varSet(name, expandAll(value), false)
	}

	expandedArgs := make([]string, len(node.Args))
	for i, arg := range node.Args {
		expandedArgs[i] = expandAll(arg)
	}

	if len(expandedArgs) == 0 {
		return 0
	}

	name := node.Args[0]
	if alias, ok := sh.Aliases[name]; ok {
		parts := strings.Fields(alias)
		expandedArgs = append(parts, expandedArgs[1:]...)
		name = expandedArgs[0]
	}

	if fn, ok := sh.Functions[name]; ok {
		oldPosParams := sh.PosParams
		sh.PosParams = expandedArgs[1:]
		node := NewParser(NewLexer(fn.Body)).Parse()
		if node != nil {
			execNode(node)
		}
		sh.PosParams = oldPosParams
		return sh.LastStatus
	}

	saved := handleRedirections(node.Redirs)
	defer restoreFDs(saved)

	status := execCommand(expandedArgs)
	sh.LastStatus = status
	return status
}

type SavedFDs struct {
	Stdin  *os.File
	Stdout *os.File
	Stderr *os.File
	Opened []*os.File
}

func handleRedirections(redirs []Redir) *SavedFDs {
	saved := &SavedFDs{
		Stdin:  os.Stdin,
		Stdout: os.Stdout,
		Stderr: os.Stderr,
	}

	for _, redir := range redirs {
		if redir.Op == "<&" || redir.Op == ">&" {
			var targetFd int
			if redir.File == "-" {
				continue
			}
			fmt.Sscanf(redir.File, "%d", &targetFd)

			var targetFile *os.File
			if targetFd == 0 {
				targetFile = os.Stdin
			} else if targetFd == 1 {
				targetFile = os.Stdout
			} else if targetFd == 2 {
				targetFile = os.Stderr
			}

			if redir.Fd == 0 || (redir.Fd == -1 && redir.Op == "<&") {
				os.Stdin = targetFile
			} else if redir.Fd == 1 || (redir.Fd == -1 && redir.Op == ">&") {
				os.Stdout = targetFile
			} else if redir.Fd == 2 {
				os.Stderr = targetFile
			}
			continue
		}

		target := expandAll(redir.File)
		var f *os.File
		var err error

		switch redir.Op {
		case ">", ">|":
			f, err = os.Create(target)
		case ">>":
			f, err = os.OpenFile(target, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0644)
		case "<":
			f, err = os.Open(target)
		case "<>":
			f, err = os.OpenFile(target, os.O_RDWR|os.O_CREATE, 0644)
		}

		if err == nil && f != nil {
			saved.Opened = append(saved.Opened, f)
			if redir.Fd == 0 || (redir.Fd == -1 && (redir.Op == "<" || redir.Op == "<>")) {
				os.Stdin = f
			} else if redir.Fd == 1 || (redir.Fd == -1 && (redir.Op == ">" || redir.Op == ">>" || redir.Op == ">|")) {
				os.Stdout = f
			} else if redir.Fd == 2 {
				os.Stderr = f
			}
		} else if err != nil {
			fmt.Fprintf(os.Stderr, "meowsh: %s: %v\n", target, err)
		}
	}
	return saved
}

func restoreFDs(saved *SavedFDs) {
	if saved == nil {
		return
	}
	os.Stdin = saved.Stdin
	os.Stdout = saved.Stdout
	os.Stderr = saved.Stderr
	for _, f := range saved.Opened {
		f.Close()
	}
}

func execPipeline(node *ASTNode) int {
	if len(node.Pipes) == 0 {
		return 0
	}

	if len(node.Pipes) == 1 {
		return execNode(node.Pipes[0])
	}

	var cmdStrs []string
	for _, n := range node.Pipes {
		cmdStrs = append(cmdStrs, nodeToString(n))
	}

	pipeCmd := exec.Command("sh", "-c", strings.Join(cmdStrs, " | "))
	pipeCmd.Stdin = os.Stdin
	pipeCmd.Stdout = os.Stdout
	pipeCmd.Stderr = os.Stderr

	err := pipeCmd.Run()
	if err != nil {
		if exit, ok := err.(*exec.ExitError); ok {
			if status, ok := exit.Sys().(syscall.WaitStatus); ok {
				return status.ExitStatus()
			}
		}
		return 1
	}
	return 0
}

func nodeToString(node *ASTNode) string {
	if node == nil {
		return ""
	}
	if node.Type == "simple" {
		return strings.Join(node.Args, " ")
	}
	return strings.Join(node.Args, " ")
}

func execAndOr(node *ASTNode) int {
	leftStatus := execNode(node.Left)

	if node.Conn == "&&" {
		if leftStatus != 0 {
			return leftStatus
		}
		return execNode(node.Right)
	} else if node.Conn == "||" {
		if leftStatus == 0 {
			return leftStatus
		}
		return execNode(node.Right)
	}

	return execNode(node.Right)
}

func execList(node *ASTNode) int {
	for _, n := range node.Pipes {
		status := execNode(n)
		sh.LastStatus = status
	}
	return sh.LastStatus
}

func execIf(node *ASTNode) int {
	status := execNode(node.Cond)
	if status == 0 {
		return execNode(node.Body)
	}
	return execNode(node.Else)
}

func execWhile(node *ASTNode) int {
	for {
		status := execNode(node.Cond)
		if node.Type == "while" && status != 0 {
			return 0
		}
		if node.Type == "until" && status == 0 {
			return 0
		}
		bodyStatus := execNode(node.Body)
		if bodyStatus == 2 {
			return 0
		}
		if bodyStatus == 3 {
			continue
		}
	}
	return 0
}

func execUntil(node *ASTNode) int {
	return execWhile(node)
}

func execFor(node *ASTNode) int {
	words := node.LoopWords

	if len(words) == 0 {
		words = sh.PosParams
	}

	for _, word := range words {
		sh.Vars[node.LoopVar] = Var{Value: expandAll(word), Flags: 0}
		execNode(node.Body)
	}
	return 0
}

func execFunction(node *ASTNode) int {
	if node.FuncBody != nil {
		sh.Functions[node.FuncName] = &FuncDef{Body: nodeToString(node.FuncBody)}
	}
	return 0
}

func execCommand(args []string) int {
	if len(args) == 0 {
		return 0
	}

	name := args[0]

	if name == "exit" {
		code := 0
		if len(args) > 1 {
			fmt.Sscanf(args[1], "%d", &code)
		}
		fmt.Println("logout")
		os.Exit(code)
		return 0
	}

	if name == "cd" {
		return builtinCd(args[1:])
	}

	if name == "pwd" {
		fmt.Println(varGet("PWD"))
		return 0
	}

	if name == "echo" {
		for i, arg := range args[1:] {
			fmt.Print(expandAll(arg))
			if i < len(args)-2 {
				fmt.Print(" ")
			}
		}
		fmt.Println()
		return 0
	}

	if name == "printf" {
		if len(args) > 1 {
			format := args[1]
			if len(args) == 2 {
				fmt.Print(format)
			} else {
				values := make([]any, len(args)-2)
				for i, v := range args[2:] {
					values[i] = v
				}
				fmt.Printf(format, values...)
			}
		}
		return 0
	}

	if name == "true" {
		return 0
	}

	if name == "false" {
		return 1
	}

	if name == "test" || name == "[" {
		return builtinTest(args[1:])
	}

	if name == "set" {
		return builtinSet(args[1:])
	}

	if name == "export" {
		return builtinExport(args[1:])
	}

	if name == "unset" {
		return builtinUnset(args[1:])
	}

	if name == "alias" {
		return builtinAlias(args[1:])
	}

	if name == "unalias" {
		return builtinUnalias(args[1:])
	}

	if name == "read" {
		return builtinRead(args[1:])
	}

	if name == "shift" {
		return builtinShift(args[1:])
	}

	if name == "local" {
		return builtinLocal(args[1:])
	}

	if name == "readonly" {
		return builtinReadonly(args[1:])
	}

	if name == "return" {
		return sh.LastStatus
	}

	if name == "break" {
		return 2
	}

	if name == "continue" {
		return 3
	}

	if name == "eval" {
		return builtinEval(args[1:])
	}

	if name == "exec" {
		if len(args) > 1 {
			proc := exec.Command(args[1], args[2:]...)
			proc.Stdin = os.Stdin
			proc.Stdout = os.Stdout
			proc.Stderr = os.Stderr
			proc.Run()
			os.Exit(0)
		}
		return 0
	}

	if name == "source" || name == "." {
		return builtinSource(args[1:])
	}

	if name == "type" {
		return builtinType(args[1:])
	}

	if name == "hash" {
		return 0
	}

	if name == "jobs" {
		return builtinJobs(args[1:])
	}

	if name == "fg" {
		return builtinFg(args[1:])
	}

	if name == "bg" {
		return builtinBg(args[1:])
	}

	if name == "kill" {
		return builtinKill(args[1:])
	}

	if name == "wait" {
		return builtinWait(args[1:])
	}

	if name == "trap" {
		return builtinTrap(args[1:])
	}

	if name == "umask" {
		return builtinUmask(args[1:])
	}

	if name == "ulimit" {
		return builtinUlimit(args[1:])
	}

	if name == "times" {
		return 0
	}

	if name == "getopts" {
		return 0
	}

	if name == "newgrp" {
		return 0
	}

	if name == ":" {
		return 0
	}

	if name == "meow" {
		fmt.Println("🐱 Meow!")
		return 0
	}

	pathDirs := strings.Split(varGet("PATH"), ":")
	for _, dir := range pathDirs {
		if dir == "" {
			continue
		}
		exePath := filepath.Join(dir, name)
		if _, err := os.Stat(exePath); err == nil {
			proc := exec.Command(exePath, args[1:]...)
			proc.Stdin = os.Stdin
			proc.Stdout = os.Stdout
			proc.Stderr = os.Stderr
			err := proc.Run()
			if err != nil {
				if exit, ok := err.(*exec.ExitError); ok {
					if status, ok := exit.Sys().(syscall.WaitStatus); ok {
						return status.ExitStatus()
					}
				}
				return 1
			}
			return 0
		}
	}

	if strings.Contains(name, "/") {
		proc := exec.Command(name, args[1:]...)
		proc.Stdin = os.Stdin
		proc.Stdout = os.Stdout
		proc.Stderr = os.Stderr
		err := proc.Run()
		if err != nil {
			if exit, ok := err.(*exec.ExitError); ok {
				if status, ok := exit.Sys().(syscall.WaitStatus); ok {
					return status.ExitStatus()
				}
			}
			return 1
		}
		return 0
	}

	fmt.Printf("meowsh: %s: command not found\n", name)
	return 127
}

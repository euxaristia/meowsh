package main

import (
	"bufio"
	"fmt"
	"io"
	"os"
	"strings"
)

var sh Shell

func mainLoop() {
	for {
		if sh.Interactive {
			prompt := buildPrompt()
			fmt.Print(prompt)
		}

		reader := bufio.NewReader(os.Stdin)
		line, err := reader.ReadString('\n')
		if err != nil {
			if err == io.EOF {
				break
			}
			continue
		}

		line = strings.TrimRight(line, "\r\n")
		if strings.TrimSpace(line) == "" {
			continue
		}

		sh.Lineno++
		line = expandAliasLine(line)

		lexer := NewLexer(line)
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

	if os.Getenv("TERM") != "" && os.Getenv("TERM") != "dumb" {
		sh.Interactive = true
	}

	if sh.Interactive {
		fmt.Fprintf(os.Stderr, "meowsh — welcome! (type 'exit' to quit)\n")
	}

	mainLoop()
}

package main

import (
	"bufio"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"unicode"
	"unsafe"
)

func isatty(fd uintptr) bool {
	var termios syscall.Termios
	_, _, err := syscall.Syscall(syscall.SYS_IOCTL, fd, uintptr(syscall.TCGETS), uintptr(unsafe.Pointer(&termios)))
	return err == 0
}

var (
	sh Shell
)

type Shell struct {
	Opts            uint
	LastStatus      int
	ShellPid        int
	LastBgPid       int
	Argv0           string
	Interactive     bool
	LoginShell      bool
	Vars            map[string]Var
	PosParams       []string
	Jobs            []*Job
	NextJobId       int
	Ps1             string
	Ps2             string
	CurPrompt       string
	HistoryFile     string
	Input           *InputSource
	StarshipEnabled bool
}

type Var struct {
	Value string
	Flags uint
}

type Job struct {
	Id   int
	Pid  int
	Cmd  string
	Done bool
}

type InputSource struct {
	Name   string
	File   *os.File
	Reader *bufio.Reader
	Eof    bool
	Prev   *InputSource
}

const (
	OPT_ALLEXPORT   uint = 1 << 0
	OPT_ERREXIT     uint = 1 << 1
	OPT_NOGLOB      uint = 1 << 2
	OPT_HASHALL     uint = 1 << 3
	OPT_INTERACTIVE uint = 1 << 4
	OPT_MONITOR     uint = 1 << 5
	OPT_NOEXEC      uint = 1 << 6
	OPT_NOUNSET     uint = 1 << 7
	OPT_VERBOSE     uint = 1 << 8
	OPT_XTRACE      uint = 1 << 9
	OPT_NOCLOBBER   uint = 1 << 10
)

const VAR_EXPORT uint = 1 << 0
const VAR_READONLY uint = 1 << 1

func shellInit() {
	sh.ShellPid = os.Getpid()
	sh.LastStatus = 0
	sh.NextJobId = 1
	sh.Vars = make(map[string]Var)

	sh.Vars["HOME"] = Var{Value: getEnv("HOME", ""), Flags: 0}
	sh.Vars["PATH"] = Var{Value: getEnv("PATH", "/usr/local/bin:/usr/bin:/bin"), Flags: VAR_EXPORT}
	sh.Vars["USER"] = Var{Value: getEnv("USER", "meow"), Flags: 0}
	sh.Vars["PWD"] = Var{Value: getEnv("PWD", "/"), Flags: 0}
	sh.Vars["SHELL"] = Var{Value: "/bin/meowsh", Flags: 0}
	sh.Vars["TERM"] = Var{Value: getEnv("TERM", "dumb"), Flags: 0}
	sh.Vars["PS1"] = Var{Value: "🐱 ", Flags: 0}
	sh.Vars["PS2"] = Var{Value: "🐱 ", Flags: 0}
}

func getEnv(key, def string) string {
	if val := os.Getenv(key); val != "" {
		return val
	}
	return def
}

func varGet(name string) string {
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

	return fmt.Sprintf("\033[32m%s\033[0m \033[34m%s\033[0m 🐱 ", user, shortPwd)
}

func parseCommand() []string {
	return nil
}

type Command struct {
	Args      []string
	Redirects []Redirect
	Pipes     []int
}

type Redirect struct {
	Op   string
	File string
	Fd   int
}

func parsePipeline(line string) [][]string {
	var pipelines [][]string

	statements := strings.Split(line, ";")

	for _, stmt := range statements {
		stmt = strings.TrimSpace(stmt)
		if stmt == "" {
			continue
		}

		var current []string
		var currentCmd strings.Builder

		inSingleQuote := false
		inDoubleQuote := false

		for i := 0; i < len(stmt); i++ {
			ch := rune(stmt[i])

			if ch == '\\' && i+1 < len(stmt) {
				currentCmd.WriteRune(ch)
				currentCmd.WriteRune(rune(stmt[i+1]))
				i++
				continue
			}

			if ch == '\'' {
				inSingleQuote = !inSingleQuote
				currentCmd.WriteRune(ch)
				continue
			}

			if ch == '"' {
				inDoubleQuote = !inDoubleQuote
				currentCmd.WriteRune(ch)
				continue
			}

			if ch == '|' && !inSingleQuote && !inDoubleQuote {
				cmd := strings.TrimSpace(currentCmd.String())
				if cmd != "" {
					current = parseLine(cmd)
					current = expandAlias(current)
					pipelines = append(pipelines, current)
				}
				currentCmd.Reset()
				continue
			}

			currentCmd.WriteRune(ch)
		}

		cmd := strings.TrimSpace(currentCmd.String())
		if cmd != "" {
			current = parseLine(cmd)
			current = expandAlias(current)
			pipelines = append(pipelines, current)
		}
	}

	return pipelines
}

func handleRedirections(cmd []string) ([]string, map[int]string, error) {
	var newArgs []string
	redirs := make(map[int]string)

	for i := 0; i < len(cmd); i++ {
		arg := cmd[i]

		if arg == ">" || arg == ">>" || arg == "<" || arg == "2>" || arg == "2>>" || arg == ">&" || arg == "<&" {
			if i+1 >= len(cmd) {
				return nil, nil, fmt.Errorf("missing redirect target")
			}

			target := cmd[i+1]

			switch arg {
			case ">":
				redirs[1] = ">" + target
			case ">>":
				redirs[1] = ">>" + target
			case "<":
				redirs[0] = "<" + target
			case "2>":
				redirs[2] = ">" + target
			case "2>>":
				redirs[2] = ">>" + target
			case ">&":
				redirs[1] = ">&" + target
			case "<&":
				redirs[0] = "<&" + target
			}

			i++
			continue
		}

		newArgs = append(newArgs, arg)
	}

	return newArgs, redirs, nil
}

func execPipeline(pipelines [][]string) int {
	if len(pipelines) == 0 {
		return 0
	}

	if len(pipelines) == 1 {
		cmd := pipelines[0]
		if len(cmd) == 0 {
			return 0
		}
		newArgs, redirs, err := handleRedirections(cmd)
		if err != nil {
			fmt.Printf("meowsh: %v\n", err)
			return 1
		}
		return execCommandRedir(newArgs, redirs)
	}

	return execPipelineSimple(pipelines)
}

func execPipelineSimple(pipelines [][]string) int {
	if len(pipelines) < 2 {
		return 0
	}

	cmdStrs := make([]string, len(pipelines))
	for i, cmd := range pipelines {
		newArgs, redirs, _ := handleRedirections(cmd)
		redirectStr := ""
		for fd, op := range redirs {
			_ = fd
			redirectStr += " " + op
		}
		cmdStrs[i] = strings.Join(newArgs, " ") + redirectStr
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

func execCommandRedir(cmd []string, redirs map[int]string) int {
	if len(cmd) == 0 {
		return 0
	}

	redirectStr := ""
	for fd, op := range redirs {
		_ = fd
		redirectStr += " " + op
	}

	if redirectStr != "" {
		fullCmd := strings.Join(cmd, " ") + redirectStr
		shCmd := exec.Command("sh", "-c", fullCmd)
		shCmd.Stdin = os.Stdin
		shCmd.Stdout = os.Stdout
		shCmd.Stderr = os.Stderr

		err := shCmd.Run()
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

	name := cmd[0]

	if name == "exit" {
		if len(cmd) > 1 {
			fmt.Println("meowsh: exit: too many arguments")
			return 1
		}
		fmt.Println("logout")
		os.Exit(0)
	}

	if name == "cd" {
		return builtinCd(cmd[1:])
	}

	if name == "pwd" {
		fmt.Println(varGet("PWD"))
		return 0
	}

	if name == "echo" {
		fmt.Println(strings.Join(cmd[1:], " "))
		return 0
	}

	if name == "true" {
		return 0
	}

	if name == "false" {
		return 1
	}

	if name == "set" {
		return builtinSet(cmd[1:])
	}

	if name == "export" {
		return builtinExport(cmd[1:])
	}

	if name == "unset" {
		return builtinUnset(cmd[1:])
	}

	if name == "test" || name == "[" {
		return builtinTest(cmd[1:])
	}

	if name == "shift" {
		return builtinShift(cmd[1:])
	}

	if name == "readonly" {
		return builtinReadonly(cmd[1:])
	}

	if name == "local" {
		return builtinLocal(cmd[1:])
	}

	if name == "alias" {
		return builtinAlias(cmd[1:])
	}

	if name == "unalias" {
		return builtinUnalias(cmd[1:])
	}

	if name == "source" || name == "." {
		return builtinSource(cmd[1:])
	}

	if name == "exit" {
		return builtinExit(cmd[1:])
	}

	if name == "eval" {
		return builtinEval(cmd[1:])
	}

	if name == ":" {
		return 0
	}

	pathDirs := strings.Split(varGet("PATH"), ":")
	for _, dir := range pathDirs {
		if dir == "" {
			continue
		}
		exePath := filepath.Join(dir, name)
		if _, err := os.Stat(exePath); err == nil {
			return runExternal(cmd, exePath)
		}
	}

	if strings.Contains(name, "/") {
		return runExternal(cmd, name)
	}

	fmt.Printf("meowsh: %s: command not found\n", name)
	return 127
}

func execCommand(cmd []string) int {
	if len(cmd) == 0 {
		return 0
	}

	name := cmd[0]

	if name == "exit" {
		if len(cmd) > 1 {
			fmt.Println("meowsh: exit: too many arguments")
			return 1
		}
		fmt.Println("logout")
		os.Exit(0)
	}

	if name == "cd" {
		return builtinCd(cmd[1:])
	}

	if name == "pwd" {
		fmt.Println(varGet("PWD"))
		return 0
	}

	if name == "echo" {
		fmt.Println(strings.Join(cmd[1:], " "))
		return 0
	}

	if name == "true" {
		return 0
	}

	if name == "false" {
		return 1
	}

	if name == "set" {
		return builtinSet(cmd[1:])
	}

	if name == "export" {
		return builtinExport(cmd[1:])
	}

	if name == "unset" {
		return builtinUnset(cmd[1:])
	}

	if name == "test" || name == "[" {
		return builtinTest(cmd[1:])
	}

	if name == "shift" {
		return builtinShift(cmd[1:])
	}

	if name == "readonly" {
		return builtinReadonly(cmd[1:])
	}

	if name == "local" {
		return builtinLocal(cmd[1:])
	}

	if name == "alias" {
		return builtinAlias(cmd[1:])
	}

	if name == "unalias" {
		return builtinUnalias(cmd[1:])
	}

	if name == "source" || name == "." {
		return builtinSource(cmd[1:])
	}

	if name == "exit" {
		return builtinExit(cmd[1:])
	}

	if name == "eval" {
		return builtinEval(cmd[1:])
	}

	if name == ":" {
		return 0
	}

	pathDirs := strings.Split(varGet("PATH"), ":")
	for _, dir := range pathDirs {
		if dir == "" {
			continue
		}
		exePath := filepath.Join(dir, name)
		if _, err := os.Stat(exePath); err == nil {
			return runExternal(cmd, exePath)
		}
	}

	if strings.Contains(name, "/") {
		return runExternal(cmd, name)
	}

	fmt.Printf("meowsh: %s: command not found\n", name)
	return 127
}

func runExternal(cmd []string, path string) int {
	proc := exec.Command(path, cmd[1:]...)
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
		} else if args[0] == "~" {
			dir = varGet("HOME")
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
		parts := strings.SplitN(arg, "=", 2)
		if len(parts) == 2 {
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
		if v, ok := sh.Vars[name]; ok {
			sh.Vars[name] = Var{Value: v.Value, Flags: v.Flags | VAR_EXPORT}
		}
	}
	return 0
}

func builtinUnset(args []string) int {
	for _, name := range args {
		delete(sh.Vars, name)
	}
	return 0
}

func builtinTest(args []string) int {
	if len(args) == 0 {
		return 1
	}

	if len(args) == 1 {
		return testUnary(args[0])
	}

	if len(args) == 2 {
		op := args[0]
		if op == "!" {
			return testUnary(args[1])
		}
		return 1
	}

	if len(args) == 3 {
		a := args[0]
		op := args[1]
		b := args[2]
		return testBinary(a, op, b)
	}

	return 1
}

func testUnary(s string) int {
	if s == "" {
		return 1
	}
	if s == "0" {
		return 1
	}
	return 0
}

func testBinary(a, op, b string) int {
	switch op {
	case "-eq":
		if toInt(a) == toInt(b) {
			return 0
		}
		return 1
	case "-ne":
		if toInt(a) != toInt(b) {
			return 0
		}
		return 1
	case "-lt":
		if toInt(a) < toInt(b) {
			return 0
		}
		return 1
	case "-le":
		if toInt(a) <= toInt(b) {
			return 0
		}
		return 1
	case "-gt":
		if toInt(a) > toInt(b) {
			return 0
		}
		return 1
	case "-ge":
		if toInt(a) >= toInt(b) {
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

func toInt(s string) int {
	var n int
	fmt.Sscanf(s, "%d", &n)
	return n
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

func builtinReadonly(args []string) int {
	for name, v := range sh.Vars {
		if v.Flags&VAR_READONLY != 0 {
			fmt.Printf("readonly %s=%q\n", name, v.Value)
		}
	}
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

var aliases = make(map[string]string)

func builtinAlias(args []string) int {
	if len(args) == 0 {
		for name, val := range aliases {
			fmt.Printf("alias %s=%q\n", name, val)
		}
		return 0
	}

	for _, arg := range args {
		parts := strings.SplitN(arg, "=", 2)
		if len(parts) == 2 {
			aliases[parts[0]] = parts[1]
		}
	}
	return 0
}

func builtinUnalias(args []string) int {
	for _, name := range args {
		delete(aliases, name)
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
		args := parseLine(line)
		if len(args) > 0 {
			execCommand(args)
		}
	}

	return 0
}

func builtinExit(args []string) int {
	code := 0
	if len(args) > 0 {
		fmt.Sscanf(args[0], "%d", &code)
	}
	os.Exit(code)
	return 0
}

func builtinEval(args []string) int {
	if len(args) == 0 {
		return 0
	}

	line := strings.Join(args, " ")
	args2 := parseLine(line)
	if len(args2) > 0 {
		return execCommand(args2)
	}
	return 0
}

func expandAlias(cmd []string) []string {
	if len(cmd) == 0 {
		return cmd
	}

	if alias, ok := aliases[cmd[0]]; ok {
		parts := strings.Fields(alias)
		return append(parts, cmd[1:]...)
	}
	return cmd
}

func parseLine(line string) []string {
	var args []string
	var current strings.Builder
	inSingleQuote := false
	inDoubleQuote := false
	backslash := false

	i := 0
	for i < len(line) {
		ch := rune(line[i])

		if backslash {
			current.WriteRune(ch)
			backslash = false
			i++
			continue
		}

		if ch == '\\' && !inSingleQuote {
			backslash = true
			i++
			continue
		}

		if ch == '\'' && !inSingleQuote {
			inSingleQuote = !inSingleQuote
			i++
			continue
		}

		if ch == '"' && !inSingleQuote {
			inDoubleQuote = !inDoubleQuote
			i++
			continue
		}

		if ch == '$' && !inSingleQuote && i+1 < len(line) {
			nextCh := rune(line[i+1])
			if nextCh == '{' {
				end := i + 2
				for end < len(line) && line[end] != '}' {
					end++
				}
				if end < len(line) {
					varName := line[i+2 : end]
					current.WriteString(expandVar(varName))
					i = end + 1
					continue
				}
			} else if nextCh == '$' {
				current.WriteString(fmt.Sprintf("%d", sh.ShellPid))
				i += 2
				continue
			} else if nextCh == '?' {
				current.WriteString(fmt.Sprintf("%d", sh.LastStatus))
				i += 2
				continue
			} else if unicode.IsLetter(nextCh) {
				end := i + 1
				for end < len(line) && unicode.IsLetter(rune(line[end])) {
					end++
				}
				varName := line[i+1 : end]
				current.WriteString(expandVar(varName))
				i = end
				continue
			}
		}

		if unicode.IsSpace(ch) && !inSingleQuote && !inDoubleQuote {
			if current.Len() > 0 {
				args = append(args, current.String())
				current.Reset()
			}
			i++
			continue
		}

		current.WriteRune(ch)
		i++
	}

	if current.Len() > 0 {
		args = append(args, current.String())
	}

	for idx, arg := range args {
		if !strings.HasPrefix(arg, "'") {
			args[idx] = expandVars(arg)
		}
	}

	return args
}

func expandVar(name string) string {
	switch name {
	case "$":
		return fmt.Sprintf("%d", sh.ShellPid)
	case "?":
		return fmt.Sprintf("%d", sh.LastStatus)
	case "!":
		return fmt.Sprintf("%d", sh.LastBgPid)
	case "0":
		return sh.Argv0
	}
	return varGet(name)
}

func expandVars(s string) string {
	if strings.HasPrefix(s, "'") && strings.HasSuffix(s, "'") {
		return s[1 : len(s)-1]
	}

	var result strings.Builder
	i := 0
	for i < len(s) {
		if s[i] == '$' && i+1 < len(s) {
			nextCh := s[i+1]
			if nextCh == '{' {
				end := strings.Index(s[i+2:], "}")
				if end >= 0 {
					varName := s[i+2 : i+2+end]
					result.WriteString(expandVar(varName))
					i = i + end + 3
					continue
				}
			} else if nextCh == '$' {
				result.WriteString(fmt.Sprintf("%d", sh.ShellPid))
				i += 2
				continue
			} else if nextCh == '?' {
				result.WriteString(fmt.Sprintf("%d", sh.LastStatus))
				i += 2
				continue
			} else if unicode.IsLetter(rune(nextCh)) {
				end := i + 1
				for end < len(s) && unicode.IsLetter(rune(s[end])) {
					end++
				}
				varName := s[i+1 : end]
				result.WriteString(expandVar(varName))
				i = end
				continue
			}
		}
		result.WriteByte(s[i])
		i++
	}
	return result.String()
}

func mainLoop() {
	reader := bufio.NewReader(os.Stdin)

	for {
		if sh.Interactive {
			prompt := buildPrompt()
			fmt.Print(prompt)
		}

		line, err := reader.ReadString('\n')
		if err != nil {
			break
		}

		line = strings.TrimRight(line, "\r\n")

		if strings.TrimSpace(line) == "" {
			continue
		}

		cmds := strings.Split(line, ";")
		for _, cmd := range cmds {
			cmd = strings.TrimSpace(cmd)
			if cmd == "" {
				continue
			}

			pipelines := parsePipeline(cmd)
			if len(pipelines) == 0 {
				continue
			}

			status := execPipeline(pipelines)
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

	isScript := false
	isCmode := false
	scriptFile := ""
	cmdLine := ""

	for i := 1; i < len(os.Args); i++ {
		arg := os.Args[i]
		if arg == "-c" {
			isCmode = true
			if i+1 < len(os.Args) {
				cmdLine = os.Args[i+1]
			}
			break
		} else if arg == "-s" {
			isScript = true
		} else if !strings.HasPrefix(arg, "-") {
			isScript = true
			scriptFile = arg
			break
		}
	}

	if isCmode {
		statements := strings.Split(cmdLine, ";")
		for _, stmt := range statements {
			stmt = strings.TrimSpace(stmt)
			if stmt == "" {
				continue
			}
			pipelines := parsePipeline(stmt)
			for _, pipeline := range pipelines {
				status := execPipeline([][]string{pipeline})
				sh.LastStatus = status
			}
		}
		os.Exit(sh.LastStatus)
	}

	if scriptFile != "" {
		data, err := os.ReadFile(scriptFile)
		if err != nil {
			fmt.Fprintf(os.Stderr, "meowsh: %s: %v\n", scriptFile, err)
			os.Exit(127)
		}
		lines := strings.Split(string(data), "\n")
		for _, line := range lines {
			line = strings.TrimSpace(line)
			if line == "" {
				continue
			}
			pipelines := parsePipeline(line)
			if len(pipelines) > 0 {
				status := execPipeline(pipelines)
				sh.LastStatus = status
			}
		}
		os.Exit(sh.LastStatus)
	}

	if !isScript && os.Getenv("TERM") != "" && os.Getenv("TERM") != "dumb" {
		sh.Interactive = true
	}

	if sh.Interactive {
		fmt.Fprintf(os.Stderr, "meowsh — welcome! (type 'exit' to quit)\n")
	}

	mainLoop()
}

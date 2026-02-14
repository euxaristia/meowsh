package main

import (
	"bufio"
	"bytes"
	"fmt"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"syscall"
	"unicode"
	"unsafe"
)

var sh Shell

type Shell struct {
	Opts            uint
	LastStatus      int
	ShellPid        int
	LastBgPid       int
	Argv0           string
	Interactive     bool
	LoginShell      bool
	Vars            map[string]Var
	Aliases         map[string]string
	PosParams       []string
	Jobs            []*Job
	NextJobId       int
	Ps1             string
	Ps2             string
	CurPrompt       string
	HistoryFile     string
	Input           *InputSource
	StarshipEnabled bool
	Trap            map[string]string
	Functions       map[string]*FuncDef
	Lineno          int
}

type Var struct {
	Value string
	Flags uint
}

type Job struct {
	Id     int
	Pid    int
	Cmd    string
	Done   bool
	Status int
}

type FuncDef struct {
	Body string
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

const (
	VAR_EXPORT   uint = 1 << 0
	VAR_READONLY uint = 1 << 1
	VAR_SPECIAL  uint = 1 << 2
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
	sh.Vars["PS1"] = Var{Value: "🐱 ", Flags: 0}
	sh.Vars["PS2"] = Var{Value: "🐱 ", Flags: 0}
	sh.Vars["SHLVL"] = Var{Value: "1", Flags: 0}
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

	return fmt.Sprintf("\033[32m%s\033[0m \033[34m%s\033[0m 🐱 ", user, shortPwd)
}

func isatty(fd uintptr) bool {
	var termios syscall.Termios
	_, _, err := syscall.Syscall(syscall.SYS_IOCTL, fd, uintptr(syscall.TCGETS), uintptr(unsafe.Pointer(&termios)))
	return err == 0
}

func tokenize(line string) []Token {
	var tokens []Token
	var current strings.Builder
	inSingleQuote := false
	inDoubleQuote := false
	backslash := false

	for i := 0; i < len(line); i++ {
		ch := rune(line[i])

		if backslash {
			current.WriteRune(ch)
			backslash = false
			continue
		}

		if ch == '\\' && !inSingleQuote {
			backslash = true
			continue
		}

		if ch == '\'' && !inDoubleQuote {
			current.WriteRune(ch)
			inSingleQuote = !inSingleQuote
			continue
		}

		if ch == '"' && !inSingleQuote {
			current.WriteRune(ch)
			inDoubleQuote = !inDoubleQuote
			continue
		}

		if inSingleQuote || inDoubleQuote {
			current.WriteRune(ch)
			continue
		}

		if ch == '$' && i+1 < len(line) {
			nextCh := line[i+1]
			if nextCh == '(' {
				end := findMatchingParen(line, i+1)
				if end > i+1 {
					tokens = append(tokens, Token{Type: TOK_WORD, Value: line[i+1 : end+1]})
					i = end
					continue
				}
			} else if nextCh == '{' {
				end := strings.Index(line[i+2:], "}")
				if end >= 0 {
					tokens = append(tokens, Token{Type: TOK_WORD, Value: line[i+1 : i+3+end]})
					i = i + 2 + end
					continue
				}
			} else if nextCh == '`' {
				end := findMatchingBacktick(line, i+1)
				if end > i+1 {
					tokens = append(tokens, Token{Type: TOK_WORD, Value: line[i+1 : end+1]})
					i = end
					continue
				}
			}
		}

		if unicode.IsSpace(ch) {
			if current.Len() > 0 {
				tokens = append(tokens, Token{Type: TOK_WORD, Value: current.String()})
				current.Reset()
			}
			continue
		}

		if ch == '|' {
			if current.Len() > 0 {
				tokens = append(tokens, Token{Type: TOK_WORD, Value: current.String()})
				current.Reset()
			}
			if i+1 < len(line) && line[i+1] == '|' {
				tokens = append(tokens, Token{Type: TOK_OR_IF, Value: "||"})
				i++
				continue
			}
			tokens = append(tokens, Token{Type: TOK_PIPE, Value: "|"})
			continue
		}

		if ch == '&' {
			if current.Len() > 0 {
				tokens = append(tokens, Token{Type: TOK_WORD, Value: current.String()})
				current.Reset()
			}
			if i+1 < len(line) && line[i+1] == '&' {
				tokens = append(tokens, Token{Type: TOK_AND_IF, Value: "&&"})
				i++
				continue
			}
			tokens = append(tokens, Token{Type: TOK_AMP, Value: "&"})
			continue
		}

		if ch == ';' {
			if current.Len() > 0 {
				tokens = append(tokens, Token{Type: TOK_WORD, Value: current.String()})
				current.Reset()
			}
			if i+1 < len(line) && line[i+1] == ';' {
				tokens = append(tokens, Token{Type: TOK_DSEMI, Value: ";;"})
				i++
				continue
			}
			tokens = append(tokens, Token{Type: TOK_SEMI, Value: ";"})
			continue
		}

		if ch == '>' {
			if current.Len() > 0 {
				tokens = append(tokens, Token{Type: TOK_WORD, Value: current.String()})
				current.Reset()
			}
			if i+1 < len(line) && line[i+1] == '>' {
				tokens = append(tokens, Token{Type: TOK_DGREAT, Value: ">>"})
				i++
				continue
			}
			if i+1 < len(line) && line[i+1] == '|' {
				tokens = append(tokens, Token{Type: TOK_CLOBBER, Value: ">|"})
				i++
				continue
			}
			if i+1 < len(line) && line[i+1] == '&' {
				tokens = append(tokens, Token{Type: TOK_GREATAND, Value: ">&"})
				i++
				continue
			}
			tokens = append(tokens, Token{Type: TOK_GREAT, Value: ">"})
			continue
		}

		if ch == '<' {
			if current.Len() > 0 {
				tokens = append(tokens, Token{Type: TOK_WORD, Value: current.String()})
				current.Reset()
			}
			if i+1 < len(line) && line[i+1] == '<' {
				if i+2 < len(line) && line[i+2] == '-' {
					tokens = append(tokens, Token{Type: TOK_DLESSDASH, Value: "<<-"})
					i += 2
					continue
				}
				tokens = append(tokens, Token{Type: TOK_DLESS, Value: "<<"})
				i++
				continue
			}
			if i+1 < len(line) && line[i+1] == '&' {
				tokens = append(tokens, Token{Type: TOK_LESSAND, Value: "<&"})
				i++
				continue
			}
			if i+1 < len(line) && line[i+1] == '>' {
				tokens = append(tokens, Token{Type: TOK_LESSGREAT, Value: "<>"})
				i++
				continue
			}
			tokens = append(tokens, Token{Type: TOK_LESS, Value: "<"})
			continue
		}

		if ch == '(' {
			if current.Len() > 0 {
				tokens = append(tokens, Token{Type: TOK_WORD, Value: current.String()})
				current.Reset()
			}
			tokens = append(tokens, Token{Type: TOK_LPAREN, Value: "("})
			continue
		}

		if ch == ')' {
			if current.Len() > 0 {
				tokens = append(tokens, Token{Type: TOK_WORD, Value: current.String()})
				current.Reset()
			}
			tokens = append(tokens, Token{Type: TOK_RPAREN, Value: ")"})
			continue
		}

		if ch == '{' {
			if current.Len() > 0 {
				tokens = append(tokens, Token{Type: TOK_WORD, Value: current.String()})
				current.Reset()
			}
			tokens = append(tokens, Token{Type: TOK_LBRACE, Value: "{"})
			continue
		}

		if ch == '}' {
			if current.Len() > 0 {
				tokens = append(tokens, Token{Type: TOK_WORD, Value: current.String()})
				current.Reset()
			}
			tokens = append(tokens, Token{Type: TOK_RBRACE, Value: "}"})
			continue
		}

		if ch == '!' {
			if current.Len() > 0 {
				tokens = append(tokens, Token{Type: TOK_WORD, Value: current.String()})
				current.Reset()
			}
			tokens = append(tokens, Token{Type: TOK_BANG, Value: "!"})
			continue
		}

		current.WriteRune(ch)
	}

	if current.Len() > 0 {
		tokens = append(tokens, Token{Type: TOK_WORD, Value: current.String()})
	}

	return tokens
}

func findMatchingParen(s string, start int) int {
	depth := 0
	for i := start; i < len(s); i++ {
		if s[i] == '(' {
			depth++
		} else if s[i] == ')' {
			depth--
			if depth == 0 {
				return i
			}
		}
	}
	return -1
}

func findMatchingBacktick(s string, start int) int {
	for i := start; i < len(s); i++ {
		if s[i] == '`' {
			return i
		}
	}
	return -1
}

type TokenType int

const (
	TOK_WORD TokenType = iota
	TOK_ASSIGNMENT
	TOK_IO_NUMBER
	TOK_PIPE
	TOK_AND_IF
	TOK_OR_IF
	TOK_SEMI
	TOK_AMP
	TOK_DSEMI
	TOK_LPAREN
	TOK_RPAREN
	TOK_LESS
	TOK_GREAT
	TOK_DLESS
	TOK_DGREAT
	TOK_LESSAND
	TOK_GREATAND
	TOK_LESSGREAT
	TOK_CLOBBER
	TOK_DLESSDASH
	TOK_IF
	TOK_THEN
	TOK_ELSE
	TOK_ELIF
	TOK_FI
	TOK_DO
	TOK_DONE
	TOK_CASE
	TOK_ESAC
	TOK_WHILE
	TOK_UNTIL
	TOK_FOR
	TOK_IN
	TOK_LBRACE
	TOK_RBRACE
	TOK_BANG
	TOK_NEWLINE
	TOK_EOF
)

type Token struct {
	Type  TokenType
	Value string
}

type ASTNode struct {
	Type     string
	Value    string
	Args     []string
	Assigns  map[string]string
	Redirs   []Redir
	Cond     *ASTNode
	Body     *ASTNode
	Else     *ASTNode
	LoopVar  string
	LoopBody string
	Cases    []CaseItem
	FuncName string
	FuncBody *ASTNode
	Left     *ASTNode
	Right    *ASTNode
	Conn     string
	Bang     bool
	Pipes    []*ASTNode
}

type Redir struct {
	Op   string
	File string
	Fd   int
}

type CaseItem struct {
	Pattern string
	Body    *ASTNode
}

func parseCommand(tokens []Token, pos int) (*ASTNode, int) {
	if pos >= len(tokens) {
		return nil, pos
	}

	node := &ASTNode{Type: "simple", Args: []string{}}

	for pos < len(tokens) {
		tok := tokens[pos]

		if tok.Type == TOK_SEMI || tok.Type == TOK_NEWLINE {
			break
		}

		if tok.Type == TOK_AMP {
			node.Conn = "&"
			pos++
			break
		}

		if tok.Type == TOK_PIPE {
			pos++
			rightNode, newPos := parseCommand(tokens, pos)
			if rightNode != nil {
				pipeNode := &ASTNode{Type: "pipeline", Pipes: []*ASTNode{node, rightNode}}
				node = pipeNode
			}
			pos = newPos
			break
		}

		if tok.Type == TOK_AND_IF {
			pos++
			rightNode, newPos := parseCommand(tokens, pos)
			if rightNode != nil {
				node = &ASTNode{Type: "and_or", Left: node, Right: rightNode, Conn: "&&"}
			}
			pos = newPos
			break
		}

		if tok.Type == TOK_OR_IF {
			pos++
			rightNode, newPos := parseCommand(tokens, pos)
			if rightNode != nil {
				node = &ASTNode{Type: "and_or", Left: node, Right: rightNode, Conn: "||"}
			}
			pos = newPos
			break
		}

		if tok.Value == ">" || tok.Value == ">>" || tok.Value == "<" || tok.Value == "2>" || tok.Value == "2>>" {
			if pos+1 < len(tokens) {
				node.Redirs = append(node.Redirs, Redir{Op: tok.Value, File: tokens[pos+1].Value})
				pos += 2
				continue
			}
		}

		if tok.Value == "if" {
			return parseIf(tokens, pos)
		}

		if tok.Value == "while" || tok.Value == "until" {
			return parseWhile(tokens, pos)
		}

		if tok.Value == "for" {
			return parseFor(tokens, pos)
		}

		if tok.Value == "case" {
			return parseCase(tokens, pos)
		}

		if tok.Value == "function" {
			return parseFunction(tokens, pos)
		}

		if strings.HasSuffix(tok.Value, "()") {
			name := strings.TrimSuffix(tok.Value, "()")
			bodyNode, newPos := parseFunctionBody(tokens, pos+1)
			return &ASTNode{Type: "function", FuncName: name, FuncBody: bodyNode}, newPos
		}

		if tok.Type == TOK_LBRACE {
			bodyNode, newPos := parseCompoundBody(tokens, pos+1)
			return &ASTNode{Type: "brace_group", Body: bodyNode}, newPos
		}

		if tok.Type == TOK_LPAREN {
			bodyNode, newPos := parseSubshell(tokens, pos+1)
			return &ASTNode{Type: "subshell", Body: bodyNode}, newPos
		}

		if isAssignment(tok.Value) {
			if node.Assigns == nil {
				node.Assigns = make(map[string]string)
			}
			parts := strings.SplitN(tok.Value, "=", 2)
			node.Assigns[parts[0]] = parts[1]
			pos++
			continue
		}

		node.Args = append(node.Args, tok.Value)
		pos++
	}

	return node, pos
}

func parseIf(tokens []Token, pos int) (*ASTNode, int) {
	condNode, pos := parseCommand(tokens, pos+1)

	thenPos := pos
	for thenPos < len(tokens) && tokens[thenPos].Value != "then" {
		thenPos++
	}
	bodyNode, pos := parseCommandList(tokens, thenPos+1)

	elseNode := &ASTNode{Type: "empty"}
	elifPos := pos
	for elifPos < len(tokens) && tokens[elifPos].Value != "else" && tokens[elifPos].Value != "fi" {
		elifPos++
	}

	if elifPos < len(tokens) && tokens[elifPos].Value == "else" {
		elseNode, pos = parseCommandList(tokens, elifPos+1)
	} else {
		pos = elifPos
	}

	for pos < len(tokens) && tokens[pos].Value != "fi" {
		pos++
	}

	return &ASTNode{Type: "if", Cond: condNode, Body: bodyNode, Else: elseNode}, pos + 1
}

func parseWhile(tokens []Token, pos int) (*ASTNode, int) {
	keyword := tokens[pos].Value
	condNode, pos := parseCommand(tokens, pos+1)

	doPos := pos
	for doPos < len(tokens) && tokens[doPos].Value != "do" {
		doPos++
	}
	bodyNode, pos := parseCommandList(tokens, doPos+1)

	for pos < len(tokens) && tokens[pos].Value != "done" {
		pos++
	}

	return &ASTNode{Type: keyword, Cond: condNode, Body: bodyNode}, pos + 1
}

func parseFor(tokens []Token, pos int) (*ASTNode, int) {
	varName := tokens[pos+1].Value

	inPos := pos + 2
	for inPos < len(tokens) && tokens[inPos].Value != "in" {
		inPos++
	}

	var words []string
	if inPos < len(tokens) && tokens[inPos].Value == "in" {
		wordsStart := inPos + 1
		for wordsStart < len(tokens) && tokens[wordsStart].Value != "do" && tokens[wordsStart].Value != ";" {
			if tokens[wordsStart].Type == TOK_WORD {
				words = append(words, tokens[wordsStart].Value)
			}
			wordsStart++
		}
		inPos = wordsStart
	} else {
		words = []string{"$@"}
	}

	doPos := inPos
	for doPos < len(tokens) && tokens[doPos].Value != "do" {
		doPos++
	}
	bodyNode, pos := parseCommandList(tokens, doPos+1)

	for pos < len(tokens) && tokens[pos].Value != "done" {
		pos++
	}

	return &ASTNode{Type: "for", LoopVar: varName, Body: bodyNode}, pos + 1
}

func parseCase(tokens []Token, pos int) (*ASTNode, int) {
	word := tokens[pos+1].Value

	casePos := pos + 2
	for casePos < len(tokens) && tokens[casePos].Value != "esac" {
		casePos++
	}

	return &ASTNode{Type: "case", Value: word}, casePos + 1
}

func parseFunction(tokens []Token, pos int) (*ASTNode, int) {
	name := tokens[pos+1].Value
	name = strings.TrimSuffix(name, "()")
	bodyNode, newPos := parseFunctionBody(tokens, pos+2)
	return &ASTNode{Type: "function", FuncName: name, FuncBody: bodyNode}, newPos
}

func parseFunctionBody(tokens []Token, pos int) (*ASTNode, int) {
	if pos < len(tokens) && tokens[pos].Type == TOK_LBRACE {
		return parseCompoundBody(tokens, pos+1)
	}
	return parseCommand(tokens, pos)
}

func parseCompoundBody(tokens []Token, pos int) (*ASTNode, int) {
	return parseCommandList(tokens, pos)
}

func parseSubshell(tokens []Token, pos int) (*ASTNode, int) {
	depth := 1
	start := pos
	for pos < len(tokens) {
		if tokens[pos].Type == TOK_LPAREN {
			depth++
		} else if tokens[pos].Type == TOK_RPAREN {
			depth--
			if depth == 0 {
				break
			}
		}
		pos++
	}
	node, _ := parseCommandList(tokens, start)
	return node, pos + 1
}

func parseCommandList(tokens []Token, pos int) (*ASTNode, int) {
	if pos >= len(tokens) {
		return nil, pos
	}

	var nodes []*ASTNode
	var conns []string

	for pos < len(tokens) {
		tok := tokens[pos]
		if tok.Value == "done" || tok.Value == "fi" || tok.Value == "esac" || tok.Value == "else" || tok.Value == "elif" {
			break
		}

		node, newPos := parseCommand(tokens, pos)
		if node != nil && node.Type != "empty" {
			nodes = append(nodes, node)
		}
		pos = newPos

		if pos < len(tokens) {
			if tokens[pos].Type == TOK_SEMI {
				conns = append(conns, ";")
				pos++
			} else if tokens[pos].Type == TOK_AMP {
				conns = append(conns, "&")
				pos++
			}
		}
	}

	if len(nodes) == 0 {
		return &ASTNode{Type: "empty"}, pos
	}

	if len(nodes) == 1 {
		return nodes[0], pos
	}

	listNode := &ASTNode{Type: "list", Pipes: nodes}
	return listNode, pos
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

func expandCommandSubstitution(s string) string {
	var result strings.Builder
	i := 0

	for i < len(s) {
		if i+1 < len(s) && s[i] == '$' && s[i+1] == '(' {
			end := findMatchingParen(s, i+1)
			if end > i+1 {
				inner := s[i+2 : end]
				out := runCommandOutput(inner)
				result.WriteString(out)
				i = end + 1
				continue
			}
		}

		if i+1 < len(s) && s[i] == '$' && s[i+1] == '`' {
			end := findMatchingBacktick(s, i+1)
			if end > i+1 {
				inner := s[i+2 : end]
				out := runCommandOutput(inner)
				result.WriteString(out)
				i = end + 1
				continue
			}
		}

		if i+2 < len(s) && s[i] == '$' && s[i+1] == '(' && s[i+2] == '(' {
			end := findDoubleParen(s, i+2)
			if end > i+2 {
				inner := s[i+3 : end]
				out := evaluateArithmetic(inner)
				result.WriteString(out)
				i = end + 2
				continue
			}
		}

		result.WriteByte(s[i])
		i++
	}

	return result.String()
}

func findDoubleParen(s string, start int) int {
	depth := 0
	for i := start; i < len(s); i++ {
		if i+1 < len(s) && s[i] == '(' && s[i+1] == '(' {
			depth++
		} else if i+1 < len(s) && s[i] == ')' && s[i+1] == ')' {
			depth--
			if depth == 0 {
				return i
			}
		}
	}
	return -1
}

func evaluateArithmetic(s string) string {
	s = strings.TrimSpace(s)

	re := regexp.MustCompile(`(\d+)\s*\*\s*(\d+)`)
	s = re.ReplaceAllStringFunc(s, func(m string) string {
		parts := regexp.MustCompile(`\d+`).FindAllString(m, -1)
		a, _ := strconv.Atoi(parts[0])
		b, _ := strconv.Atoi(parts[1])
		return strconv.Itoa(a * b)
	})

	re = regexp.MustCompile(`(\d+)\s*\+\s*(\d+)`)
	s = re.ReplaceAllStringFunc(s, func(m string) string {
		parts := regexp.MustCompile(`\d+`).FindAllString(m, -1)
		a, _ := strconv.Atoi(parts[0])
		b, _ := strconv.Atoi(parts[1])
		return strconv.Itoa(a + b)
	})

	re = regexp.MustCompile(`(\d+)\s*-\s*(\d+)`)
	s = re.ReplaceAllStringFunc(s, func(m string) string {
		parts := regexp.MustCompile(`\d+`).FindAllString(m, -1)
		a, _ := strconv.Atoi(parts[0])
		b, _ := strconv.Atoi(parts[1])
		return strconv.Itoa(a - b)
	})

	re = regexp.MustCompile(`(\d+)\s*/\s*(\d+)`)
	s = re.ReplaceAllStringFunc(s, func(m string) string {
		parts := regexp.MustCompile(`\d+`).FindAllString(m, -1)
		a, _ := strconv.Atoi(parts[0])
		b, _ := strconv.Atoi(parts[1])
		if b != 0 {
			return strconv.Itoa(a / b)
		}
		return "0"
	})

	num, err := strconv.Atoi(strings.TrimSpace(s))
	if err == nil {
		return strconv.Itoa(num)
	}

	return "0"
}

func runCommandOutput(cmd string) string {
	cmd = strings.TrimSpace(cmd)
	parts := strings.Fields(cmd)
	if len(parts) == 0 {
		return ""
	}

	proc := exec.Command(parts[0], parts[1:]...)
	var out bytes.Buffer
	proc.Stdout = &out
	proc.Run()
	return strings.TrimSpace(out.String())
}

func expandGlob(s string) string {
	if strings.ContainsAny(s, "*?[]") && !strings.HasPrefix(s, "'") {
		matches, err := filepath.Glob(s)
		if err == nil && len(matches) > 0 {
			return strings.Join(matches, " ")
		}
	}
	return s
}

func expandVariable(s string) string {
	if strings.HasPrefix(s, "'") && strings.HasSuffix(s, "'") {
		return s[1 : len(s)-1]
	}

	if strings.HasPrefix(s, "\"") && strings.HasSuffix(s, "\"") {
		inner := s[1 : len(s)-1]
		return expandVariables(inner)
	}

	var result strings.Builder
	i := 0

	for i < len(s) {
		if i+1 < len(s) && s[i] == '$' {
			nextCh := s[i+1]
			if nextCh == '{' {
				end := strings.Index(s[i+2:], "}")
				if end >= 0 {
					varName := s[i+2 : i+2+end]
					result.WriteString(expandVarValue(varName))
					i = i + 3 + end
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
			} else if nextCh == '(' {
				end := findMatchingParen(s, i+1)
				if end > i+1 {
					inner := s[i+2 : end]
					out := runCommandOutput(inner)
					result.WriteString(out)
					i = end + 1
					continue
				}
			} else if nextCh == '`' {
				end := findMatchingBacktick(s, i+1)
				if end > i+1 {
					inner := s[i+2 : end]
					out := runCommandOutput(inner)
					result.WriteString(out)
					i = end + 1
					continue
				}
			} else if unicode.IsLetter(rune(nextCh)) {
				end := i + 1
				for end < len(s) && (unicode.IsLetter(rune(s[end])) || unicode.IsDigit(rune(s[end]))) {
					end++
				}
				varName := s[i+1 : end]
				result.WriteString(expandVarValue(varName))
				i = end
				continue
			}
		}
		result.WriteByte(s[i])
		i++
	}

	return result.String()
}

func expandVarValue(name string) string {
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
	}
	if v, ok := sh.Vars[name]; ok {
		return v.Value
	}
	return ""
}

func expandVariables(s string) string {
	var result strings.Builder
	i := 0

	for i < len(s) {
		if i+1 < len(s) && s[i] == '$' {
			nextCh := s[i+1]
			if nextCh == '{' {
				end := strings.Index(s[i+2:], "}")
				if end >= 0 {
					varName := s[i+2 : i+2+end]
					result.WriteString(expandVarValue(varName))
					i = i + 3 + end
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
			} else if nextCh == '(' {
				end := findMatchingParen(s, i+1)
				if end > i+1 {
					inner := s[i+2 : end]
					out := runCommandOutput(inner)
					result.WriteString(out)
					i = end + 1
					continue
				}
			} else if unicode.IsLetter(rune(nextCh)) {
				end := i + 1
				for end < len(s) && (unicode.IsLetter(rune(s[end])) || unicode.IsDigit(rune(s[end]))) {
					end++
				}
				varName := s[i+1 : end]
				result.WriteString(expandVarValue(varName))
				i = end
				continue
			}
		}
		result.WriteByte(s[i])
		i++
	}

	return result.String()
}

func expandAll(s string) string {
	s = expandVariable(s)
	s = expandGlob(s)
	return s
}

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
	if len(node.Args) == 0 {
		return 0
	}

	for name, value := range node.Assigns {
		varSet(name, value, false)
	}

	expandedArgs := make([]string, len(node.Args))
	for i, arg := range node.Args {
		expandedArgs[i] = expandAll(arg)
	}

	if len(expandedArgs) == 0 {
		return 0
	}

	name := expandedArgs[0]

	if alias, ok := sh.Aliases[name]; ok {
		parts := strings.Fields(alias)
		expandedArgs = append(parts, expandedArgs[1:]...)
		name = expandedArgs[0]
	}

	if fn, ok := sh.Functions[name]; ok {
		oldPosParams := sh.PosParams
		sh.PosParams = expandedArgs[1:]
		tokens := tokenize(fn.Body)
		bodyNode, _ := parseCommand(tokens, 0)
		if bodyNode != nil {
			execNode(bodyNode)
		}
		sh.PosParams = oldPosParams
		return sh.LastStatus
	}

	handleRedirections(node.Redirs)

	status := execCommand(expandedArgs)
	sh.LastStatus = status
	return status
}

func handleRedirections(redirs []Redir) {
	for _, redir := range redirs {
		target := expandAll(redir.File)
		switch redir.Op {
		case ">":
			f, _ := os.Create(target)
			os.Stdout = f
		case ">>":
			f, _ := os.OpenFile(target, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0644)
			os.Stdout = f
		case "<":
			f, _ := os.Open(target)
			os.Stdin = f
		case "2>":
			f, _ := os.Create(target)
			os.Stderr = f
		case "2>>":
			f, _ := os.OpenFile(target, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0644)
			os.Stderr = f
		}
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
	var words []string
	if len(node.Body.Pipes) > 0 && node.Body.Pipes[0].Type == "simple" {
		words = node.Body.Pipes[0].Args
	}

	if len(words) == 0 {
		words = sh.PosParams
	}

	for _, word := range words {
		sh.Vars[node.LoopVar] = Var{Value: word, Flags: 0}
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
	node, _ := parseCommand(tokens, 0)
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
		tokens := tokenize(line)
		node, _ := parseCommand(tokens, 0)
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

		sh.Lineno++

		tokens := tokenize(line)
		node, _ := parseCommand(tokens, 0)

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
			tokens := tokenize(stmt)
			node, _ := parseCommand(tokens, 0)
			if node != nil {
				status := execNode(node)
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
			if line == "" || strings.HasPrefix(line, "#") {
				continue
			}
			tokens := tokenize(line)
			node, _ := parseCommand(tokens, 0)
			if node != nil {
				status := execNode(node)
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

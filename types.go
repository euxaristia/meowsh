package main

import (
	"bufio"
	"os"
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
	Id         int
	Pgid       int
	Procs      []*Process
	State      JobState
	Notified   bool
	Foreground bool
	CmdText    string
}

type Process struct {
	Pid    int
	Status int
	State  ProcState
	Cmd    string
}

type JobState int

const (
	JOB_RUNNING JobState = iota
	JOB_STOPPED
	JOB_DONE
)

type ProcState int

const (
	PROC_RUNNING ProcState = iota
	PROC_STOPPED
	PROC_DONE
)

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
	Type      string
	Value     string
	Args      []string
	Assigns   map[string]string
	Redirs    []Redir
	Cond      *ASTNode
	Body      *ASTNode
	Else      *ASTNode
	LoopVar   string
	LoopWords []string
	Cases     []CaseItem
	FuncName  string
	FuncBody  *ASTNode
	Left      *ASTNode
	Right     *ASTNode
	Conn      string
	Bang      bool
	Pipes     []*ASTNode
}

type Redir struct {
	Op          string
	File        string
	Fd          int
	HeredocBody string
}

type CaseItem struct {
	Pattern string
	Body    *ASTNode
}

use std::collections::HashMap;
use std::sync::Mutex;

pub const OPT_ALLEXPORT: u32 = 1 << 0;
pub const OPT_ERREXIT: u32 = 1 << 1;
pub const OPT_NOGLOB: u32 = 1 << 2;
pub const OPT_HASHALL: u32 = 1 << 3;
pub const OPT_INTERACTIVE: u32 = 1 << 4;
pub const OPT_MONITOR: u32 = 1 << 5;
pub const OPT_NOEXEC: u32 = 1 << 6;
pub const OPT_NOUNSET: u32 = 1 << 7;
pub const OPT_VERBOSE: u32 = 1 << 8;
pub const OPT_XTRACE: u32 = 1 << 9;
pub const OPT_NOCLOBBER: u32 = 1 << 10;

pub const VAR_EXPORT: u32 = 1 << 0;
pub const VAR_READONLY: u32 = 1 << 1;
pub const VAR_SPECIAL: u32 = 1 << 2;

#[derive(Clone, Copy, PartialEq)]
pub enum JobState {
    Running,
    Stopped,
    Done,
}

#[derive(Clone, Copy, PartialEq)]
pub enum ProcState {
    Running,
    Stopped,
    Done,
}

#[derive(Clone)]
pub struct Var {
    pub value: String,
    pub flags: u32,
}

#[derive(Clone)]
pub struct Job {
    pub id: i32,
    pub pgid: i32,
    pub procs: Vec<Process>,
    pub state: JobState,
    pub notified: bool,
    pub foreground: bool,
    pub cmd_text: String,
}

#[derive(Clone)]
pub struct Process {
    pub pid: i32,
    pub status: i32,
    pub state: ProcState,
    pub cmd: String,
}

#[derive(Clone)]
pub struct FuncDef {
    pub body: String,
}

pub struct Shell {
    pub opts: u32,
    pub last_status: i32,
    pub shell_pid: i32,
    pub last_bg_pid: i32,
    pub argv0: String,
    pub interactive: bool,
    pub login_shell: bool,
    pub vars: HashMap<String, Var>,
    pub aliases: HashMap<String, String>,
    pub pos_params: Vec<String>,
    pub jobs: Mutex<Vec<Job>>,
    pub history: Mutex<Vec<String>>,
    pub next_job_id: Mutex<i32>,
    pub ps1: String,
    pub ps2: String,
    pub cur_prompt: String,
    pub history_file: String,
    pub trap: HashMap<String, String>,
    pub functions: HashMap<String, FuncDef>,
    pub lineno: i32,
}

impl Shell {
    pub fn new() -> Self {
        Shell {
            opts: 0,
            last_status: 0,
            shell_pid: 0,
            last_bg_pid: 0,
            argv0: String::new(),
            interactive: false,
            login_shell: false,
            vars: HashMap::new(),
            aliases: HashMap::new(),
            pos_params: Vec::new(),
            jobs: Mutex::new(Vec::new()),
            history: Mutex::new(Vec::new()),
            next_job_id: Mutex::new(1),
            ps1: String::from("meowsh> "),
            ps2: String::from("meowsh... "),
            cur_prompt: String::new(),
            history_file: String::new(),
            trap: HashMap::new(),
            functions: HashMap::new(),
            lineno: 0,
        }
    }
}

impl Default for Shell {
    fn default() -> Self {
        Self::new()
    }
}

#[derive(Clone, Copy, PartialEq)]
pub enum TokenType {
    Word,
    Assignment,
    IoNumber,
    Pipe,
    AndIf,
    OrIf,
    Semi,
    Ampersand,
    Dsemi,
    Lparen,
    Rparen,
    Less,
    Great,
    Dless,
    Dgreat,
    LessAnd,
    GreatAnd,
    LessGreat,
    Clobber,
    DlessDash,
    If,
    Then,
    Else,
    Elif,
    Fi,
    Do,
    Done,
    Case,
    Esac,
    While,
    Until,
    For,
    In,
    Lbrace,
    Rbrace,
    Bang,
    Newline,
    Eof,
}

#[derive(Clone)]
pub struct Token {
    pub token_type: TokenType,
    pub value: String,
}

#[derive(Clone)]
pub struct Redir {
    pub op: String,
    pub file: String,
    pub fd: i32,
    pub heredoc_body: String,
}

#[derive(Clone)]
pub struct CaseItem {
    pub pattern: String,
    pub body: Option<Box<ASTNode>>,
}

#[derive(Clone)]
pub struct ASTNode {
    pub node_type: String,
    pub value: String,
    pub args: Vec<String>,
    pub assigns: HashMap<String, String>,
    pub redirs: Vec<Redir>,
    pub cond: Option<Box<ASTNode>>,
    pub body: Option<Box<ASTNode>>,
    pub else_body: Option<Box<ASTNode>>,
    pub loop_var: String,
    pub loop_words: Vec<String>,
    pub cases: Vec<CaseItem>,
    pub func_name: String,
    pub func_body: Option<Box<ASTNode>>,
    pub left: Option<Box<ASTNode>>,
    pub right: Option<Box<ASTNode>>,
    pub conn: String,
    pub bang: bool,
    pub pipes: Vec<ASTNode>,
}

impl ASTNode {
    pub fn new(node_type: &str) -> Self {
        ASTNode {
            node_type: node_type.to_string(),
            value: String::new(),
            args: Vec::new(),
            assigns: HashMap::new(),
            redirs: Vec::new(),
            cond: None,
            body: None,
            else_body: None,
            loop_var: String::new(),
            loop_words: Vec::new(),
            cases: Vec::new(),
            func_name: String::new(),
            func_body: None,
            left: None,
            right: None,
            conn: String::new(),
            bang: false,
            pipes: Vec::new(),
        }
    }
}

use std::collections::HashMap;
use std::sync::Mutex;

pub const OPT_INTERACTIVE: u32 = 1 << 4;
pub const VAR_EXPORT: u32 = 1 << 0;

#[derive(Clone)]
pub struct Var {
    pub value: String,
    pub flags: u32,
}

pub struct Shell {
    pub opts: u32,
    pub last_status: i32,
    pub shell_pid: i32,
    pub last_bg_pid: i32,
    pub argv0: String,
    pub interactive: bool,
    pub vars: HashMap<String, Var>,
    pub aliases: HashMap<String, String>,
    pub pos_params: Vec<String>,
    pub history: Mutex<Vec<String>>,
    pub next_job_id: Mutex<i32>,
    pub ps1: String,
    pub ps2: String,
    pub history_file: String,
    pub functions: HashMap<String, String>,
}

impl Default for Shell {
    fn default() -> Self {
        Self::new()
    }
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
            vars: HashMap::new(),
            aliases: HashMap::new(),
            pos_params: Vec::new(),
            history: Mutex::new(Vec::new()),
            next_job_id: Mutex::new(1),
            ps1: String::from("meowsh> "),
            ps2: String::from("meowsh... "),
            history_file: String::new(),
            functions: HashMap::new(),
        }
    }
}

#[derive(Clone)]
pub struct Token {
    pub token_type: TokenType,
    pub value: String,
}

#[derive(Clone, Copy, PartialEq)]
pub enum TokenType {
    Word,
    Newline,
    Eof,
}

#[derive(Clone)]
pub struct ASTNode {
    pub node_type: String,
    pub args: Vec<String>,
    pub assigns: HashMap<String, String>,
}

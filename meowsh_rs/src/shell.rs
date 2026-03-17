use crate::types::{Shell, Var, OPT_INTERACTIVE, VAR_EXPORT};
use std::collections::HashMap;
use std::env;
use std::path::PathBuf;
use std::sync::Mutex;

pub struct ShellState {
    pub shell: Mutex<Shell>,
}

impl Default for ShellState {
    fn default() -> Self {
        Self::new()
    }
}

impl ShellState {
    pub fn new() -> Self {
        ShellState {
            shell: Mutex::new(Shell::new()),
        }
    }
}

lazy_static::lazy_static! {
    pub static ref SHELL: ShellState = ShellState::new();
}

pub fn shell_init() {
    let mut shell = SHELL.shell.lock().unwrap();
    shell.shell_pid = std::process::id() as i32;
    shell.last_status = 0;
    *shell.next_job_id.lock().unwrap() = 1;

    shell.vars.insert(
        "HOME".to_string(),
        Var {
            value: get_env("HOME", ""),
            flags: 0,
        },
    );
    shell.vars.insert(
        "PATH".to_string(),
        Var {
            value: get_env("PATH", "/usr/local/bin:/usr/bin:/bin"),
            flags: VAR_EXPORT,
        },
    );
    shell.vars.insert(
        "USER".to_string(),
        Var {
            value: get_env("USER", "meow"),
            flags: 0,
        },
    );
    shell.vars.insert(
        "PWD".to_string(),
        Var {
            value: get_env("PWD", "/"),
            flags: 0,
        },
    );
    shell.vars.insert(
        "SHELL".to_string(),
        Var {
            value: "/bin/meowsh".to_string(),
            flags: 0,
        },
    );
    shell.vars.insert(
        "TERM".to_string(),
        Var {
            value: get_env("TERM", "dumb"),
            flags: 0,
        },
    );
    shell.vars.insert(
        "PS1".to_string(),
        Var {
            value: "meowsh> ".to_string(),
            flags: 0,
        },
    );
    shell.vars.insert(
        "PS2".to_string(),
        Var {
            value: "meowsh... ".to_string(),
            flags: 0,
        },
    );

    if let Some(home) = shell.vars.get("HOME") {
        shell.history_file = PathBuf::from(&home.value)
            .join(".meowsh_history")
            .to_string_lossy()
            .to_string();
    }
}

fn get_env(key: &str, def: &str) -> String {
    env::var(key).unwrap_or_else(|_| def.to_string())
}

pub fn var_get(name: &str) -> String {
    let shell = SHELL.shell.lock().unwrap();
    match name {
        "$" => return format!("{}", shell.shell_pid),
        "?" => return format!("{}", shell.last_status),
        "!" => return format!("{}", shell.last_bg_pid),
        "0" => return shell.argv0.clone(),
        "#" => return format!("{}", shell.pos_params.len()),
        "@" | "*" => return shell.pos_params.join(" "),
        "-" => {
            let mut flags = String::new();
            if shell.opts & OPT_INTERACTIVE != 0 {
                flags.push('i');
            }
            return flags;
        }
        _ => {}
    }

    if name.len() > 1 && name.starts_with('-') {
        return var_get(&name[1..]);
    }

    if let Some(v) = shell.vars.get(name) {
        return v.value.clone();
    }
    String::new()
}

pub fn var_set(name: &str, value: &str, _export: bool) {
    let mut shell = SHELL.shell.lock().unwrap();
    shell.vars.insert(
        name.to_string(),
        Var {
            value: value.to_string(),
            flags: 0,
        },
    );
    env::set_var(name, value);
}

pub fn is_assignment(s: &str) -> bool {
    if s.is_empty() {
        return false;
    }
    for (i, c) in s.chars().enumerate() {
        if c == '=' {
            return i > 0;
        }
        if !c.is_alphabetic() && !c.is_ascii_digit() && c != '_' {
            return false;
        }
    }
    false
}

pub fn shorten_path(p: &str) -> String {
    let home = var_get("HOME");
    if !home.is_empty() && p.starts_with(&home) {
        return format!("~{}", p.strip_prefix(&home).unwrap_or(""));
    }
    p.to_string()
}

pub fn build_prompt() -> String {
    let user = var_get("USER");
    let user = if user.is_empty() { "meow" } else { &user };
    let pwd = var_get("PWD");
    let pwd = if pwd.is_empty() { "?" } else { &pwd };
    let short_pwd = shorten_path(&pwd);
    format!("\x1b[32m{}\x1b[0m \x1b[34m{}\x1b[0m ", user, short_pwd)
}

use crate::types::{
    Shell, Var, OPT_ALLEXPORT, OPT_ERREXIT, OPT_HASHALL, OPT_INTERACTIVE, OPT_MONITOR, OPT_NOEXEC,
    OPT_NOGLOB, OPT_NOUNSET, OPT_VERBOSE, OPT_XTRACE, VAR_EXPORT,
};
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

use std::sync::LazyLock;

pub static SHELL: LazyLock<ShellState> = LazyLock::new(ShellState::new);

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
    shell.vars.insert(
        "SHLVL".to_string(),
        Var {
            value: "1".to_string(),
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
            if shell.opts & OPT_ALLEXPORT != 0 {
                flags.push('a');
            }
            if shell.opts & OPT_ERREXIT != 0 {
                flags.push('e');
            }
            if shell.opts & OPT_NOGLOB != 0 {
                flags.push('f');
            }
            if shell.opts & OPT_HASHALL != 0 {
                flags.push('h');
            }
            if shell.opts & OPT_INTERACTIVE != 0 {
                flags.push('i');
            }
            if shell.opts & OPT_MONITOR != 0 {
                flags.push('m');
            }
            if shell.opts & OPT_NOEXEC != 0 {
                flags.push('n');
            }
            if shell.opts & OPT_NOUNSET != 0 {
                flags.push('u');
            }
            if shell.opts & OPT_VERBOSE != 0 {
                flags.push('v');
            }
            if shell.opts & OPT_XTRACE != 0 {
                flags.push('x');
            }
            return flags;
        }
        _ => {}
    }

    if name.len() > 1 && name.starts_with('-') {
        return var_get(&name[1..]);
    }

    // Numeric positional parameter lookup ($1, $2, ...).
    if !name.is_empty() && name.chars().all(|c| c.is_ascii_digit()) {
        if let Ok(idx) = name.parse::<usize>() {
            if idx == 0 {
                return shell.argv0.clone();
            }
            if let Some(p) = shell.pos_params.get(idx - 1) {
                return p.clone();
            }
            return String::new();
        }
    }

    if let Some(arr) = shell.arrays.get(name) {
        return arr.join(" ");
    }
    if let Some(v) = shell.vars.get(name) {
        return v.value.clone();
    }
    String::new()
}

pub fn var_get_if_exists(name: &str) -> Option<i64> {
    let shell = SHELL.shell.lock().unwrap();
    if let Some(v) = shell.vars.get(name) {
        if let Ok(n) = v.value.parse::<i64>() {
            return Some(n);
        }
    }
    None
}

pub fn var_set(name: &str, value: &str, _export: bool) {
    let mut shell = SHELL.shell.lock().unwrap();
    // Setting a scalar with the same name as an existing array clears the
    // array — they share a namespace.
    shell.arrays.remove(name);
    shell.vars.insert(
        name.to_string(),
        Var {
            value: value.to_string(),
            flags: 0,
        },
    );
    env::set_var(name, value);
}

pub fn array_set(name: &str, items: Vec<String>) {
    let mut shell = SHELL.shell.lock().unwrap();
    shell.vars.remove(name);
    let joined = items.join(" ");
    env::set_var(name, &joined);
    shell.arrays.insert(name.to_string(), items);
}

pub fn array_append(name: &str, items: Vec<String>) {
    let mut shell = SHELL.shell.lock().unwrap();
    let existing = shell.arrays.entry(name.to_string()).or_default();
    existing.extend(items);
    let joined = existing.join(" ");
    shell.vars.remove(name);
    env::set_var(name, &joined);
}

pub fn array_get(name: &str) -> Option<Vec<String>> {
    SHELL.shell.lock().unwrap().arrays.get(name).cloned()
}

pub fn is_array(name: &str) -> bool {
    SHELL.shell.lock().unwrap().arrays.contains_key(name)
}

pub fn push_scope() {
    let mut shell = SHELL.shell.lock().unwrap();
    shell.local_scopes.push(HashMap::new());
}

pub fn pop_scope() {
    let mut shell = SHELL.shell.lock().unwrap();
    if let Some(scope) = shell.local_scopes.pop() {
        for (name, prev) in scope {
            match prev {
                Some(var) => {
                    env::set_var(&name, &var.value);
                    shell.vars.insert(name, var);
                }
                None => {
                    shell.vars.remove(&name);
                    env::remove_var(&name);
                }
            }
        }
    }
}

pub fn in_function_scope() -> bool {
    !SHELL.shell.lock().unwrap().local_scopes.is_empty()
}

// Snapshot the current value of `name` into the innermost scope so it will
// be restored on scope exit. Returns true if a scope existed (i.e. we are
// inside a function), false otherwise. The first call for a given name
// wins; subsequent calls in the same scope are no-ops to preserve the
// original prior value.
pub fn declare_local(name: &str) -> bool {
    let mut shell = SHELL.shell.lock().unwrap();
    let prev = shell.vars.get(name).cloned();
    if let Some(scope) = shell.local_scopes.last_mut() {
        scope.entry(name.to_string()).or_insert(prev);
        true
    } else {
        false
    }
}

pub fn shorten_path(p: &str) -> String {
    let home = var_get("HOME");
    if !home.is_empty() && p.starts_with(&home) {
        return format!("~{}", p.strip_prefix(&home).unwrap_or(""));
    }
    p.to_string()
}

pub fn build_prompt() -> String {
    let prompt_str = {
        let p = var_get("PROMPT");
        if !p.is_empty() {
            p
        } else {
            var_get("PS1")
        }
    };
    if prompt_str.is_empty() {
        let user = var_get("USER");
        let user = if user.is_empty() { "meow" } else { &user };
        let pwd = var_get("PWD");
        let pwd = if pwd.is_empty() { "?" } else { &pwd };
        let short_pwd = shorten_path(pwd);
        return format!("\x1b[32m{}\x1b[0m \x1b[34m{}\x1b[0m 𓃠  ", user, short_pwd);
    }
    expand_prompt(&prompt_str)
}

pub fn expand_prompt(s: &str) -> String {
    let mut out = String::new();
    let mut chars = s.chars().peekable();
    while let Some(c) = chars.next() {
        if c != '%' {
            out.push(c);
            continue;
        }
        match chars.next() {
            None => out.push('%'),
            Some('%') => out.push('%'),
            Some('n') => out.push_str(&var_get("USER")),
            Some('m') => out.push_str(&hostname_short()),
            Some('M') => out.push_str(&hostname_full()),
            Some('~') => out.push_str(&shorten_path(&var_get("PWD"))),
            Some('d') | Some('/') => out.push_str(&var_get("PWD")),
            Some('c') | Some('.') | Some('C') => out.push_str(&pwd_basename()),
            Some('#') => out.push(if is_root() { '#' } else { '%' }),
            Some('T') => out.push_str(&time_fmt("%H:%M")),
            Some('t') | Some('@') => out.push_str(&time_fmt("%I:%M%p")),
            Some('*') => out.push_str(&time_fmt("%H:%M:%S")),
            Some('D') => {
                if chars.peek() == Some(&'{') {
                    chars.next();
                    let fmt = read_until_brace(&mut chars);
                    out.push_str(&time_fmt(&fmt));
                } else {
                    out.push_str(&time_fmt("%y-%m-%d"));
                }
            }
            Some('w') => out.push_str(&time_fmt("%a %d")),
            Some('W') => out.push_str(&time_fmt("%m/%d/%y")),
            Some('!') | Some('h') => out.push_str(&history_count()),
            Some('?') => out.push_str(&format!(
                "{}",
                SHELL.shell.lock().unwrap().last_status
            )),
            Some('j') => out.push_str(&jobs_count()),
            Some('B') => out.push_str("\x1b[1m"),
            Some('b') => out.push_str("\x1b[22m"),
            Some('U') => out.push_str("\x1b[4m"),
            Some('u') => out.push_str("\x1b[24m"),
            Some('S') => out.push_str("\x1b[7m"),
            Some('s') => out.push_str("\x1b[27m"),
            Some('F') => {
                if chars.peek() == Some(&'{') {
                    chars.next();
                    let name = read_until_brace(&mut chars);
                    out.push_str(&color_fg(&name));
                } else {
                    out.push_str("\x1b[39m");
                }
            }
            Some('f') => out.push_str("\x1b[39m"),
            Some('K') => {
                if chars.peek() == Some(&'{') {
                    chars.next();
                    let name = read_until_brace(&mut chars);
                    out.push_str(&color_bg(&name));
                } else {
                    out.push_str("\x1b[49m");
                }
            }
            Some('k') => out.push_str("\x1b[49m"),
            // %{ raw %} — content is emitted verbatim. We don't track
            // visual width, so the markers are effectively no-ops.
            Some('{') => loop {
                match chars.next() {
                    None => break,
                    Some('%') => {
                        if chars.peek() == Some(&'}') {
                            chars.next();
                            break;
                        }
                        out.push('%');
                    }
                    Some(ch) => out.push(ch),
                }
            },
            Some(other) => {
                out.push('%');
                out.push(other);
            }
        }
    }
    out
}

fn read_until_brace(chars: &mut std::iter::Peekable<std::str::Chars<'_>>) -> String {
    let mut s = String::new();
    while let Some(&ch) = chars.peek() {
        chars.next();
        if ch == '}' {
            break;
        }
        s.push(ch);
    }
    s
}

fn hostname_full() -> String {
    let mut buf = vec![0u8; 256];
    unsafe {
        if libc::gethostname(buf.as_mut_ptr() as *mut libc::c_char, buf.len()) == 0 {
            let cstr = std::ffi::CStr::from_ptr(buf.as_ptr() as *const libc::c_char);
            return cstr.to_string_lossy().into_owned();
        }
    }
    String::new()
}

fn hostname_short() -> String {
    let h = hostname_full();
    h.split('.').next().unwrap_or(&h).to_string()
}

fn pwd_basename() -> String {
    let pwd = var_get("PWD");
    if pwd.is_empty() {
        return String::new();
    }
    PathBuf::from(&pwd)
        .file_name()
        .map(|s| s.to_string_lossy().into_owned())
        .unwrap_or_else(|| pwd.clone())
}

fn is_root() -> bool {
    unsafe { libc::geteuid() == 0 }
}

fn time_fmt(fmt: &str) -> String {
    let cfmt = match std::ffi::CString::new(fmt) {
        Ok(c) => c,
        Err(_) => return String::new(),
    };
    let mut buf = vec![0u8; 128];
    unsafe {
        let mut t: libc::time_t = 0;
        libc::time(&mut t);
        let mut tm: libc::tm = std::mem::zeroed();
        if libc::localtime_r(&t, &mut tm).is_null() {
            return String::new();
        }
        let n = libc::strftime(
            buf.as_mut_ptr() as *mut libc::c_char,
            buf.len(),
            cfmt.as_ptr(),
            &tm,
        );
        if n == 0 {
            return String::new();
        }
        buf.truncate(n);
        String::from_utf8_lossy(&buf).into_owned()
    }
}

fn history_count() -> String {
    let shell = SHELL.shell.lock().unwrap();
    let n = shell.history.lock().unwrap().len();
    n.to_string()
}

fn jobs_count() -> String {
    let shell = SHELL.shell.lock().unwrap();
    let n = shell.jobs.lock().unwrap().len();
    n.to_string()
}

fn color_code(name: &str) -> Option<u16> {
    match name.to_ascii_lowercase().as_str() {
        "black" => Some(0),
        "red" => Some(1),
        "green" => Some(2),
        "yellow" => Some(3),
        "blue" => Some(4),
        "magenta" => Some(5),
        "cyan" => Some(6),
        "white" => Some(7),
        "default" => Some(9),
        _ => name.parse::<u16>().ok(),
    }
}

fn color_fg(name: &str) -> String {
    match color_code(name) {
        Some(9) => "\x1b[39m".to_string(),
        Some(n) if n < 8 => format!("\x1b[{}m", 30 + n),
        Some(n) => format!("\x1b[38;5;{}m", n),
        None => String::new(),
    }
}

fn color_bg(name: &str) -> String {
    match color_code(name) {
        Some(9) => "\x1b[49m".to_string(),
        Some(n) if n < 8 => format!("\x1b[{}m", 40 + n),
        Some(n) => format!("\x1b[48;5;{}m", n),
        None => String::new(),
    }
}

pub fn find_command(name: &str) -> Option<String> {
    if name.contains('/') {
        let p = std::path::Path::new(name);
        if p.is_file() && is_executable(p) {
            return Some(name.to_string());
        }
        return None;
    }

    let path = var_get("PATH");
    for dir in path.split(':') {
        let dir = if dir.is_empty() { "." } else { dir };
        let p = std::path::Path::new(dir).join(name);
        if p.is_file() && is_executable(&p) {
            return Some(p.to_string_lossy().to_string());
        }
    }
    None
}

fn is_executable(path: &std::path::Path) -> bool {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        if let Ok(metadata) = path.metadata() {
            let mode = metadata.permissions().mode();
            return metadata.is_file() && (mode & 0o111 != 0);
        }
    }
    path.is_file()
}

pub fn command_exists(name: &str) -> bool {
    let name = crate::strip_ansi(name);
    let name = name.as_str();
    {
        let shell = SHELL.shell.lock().unwrap();
        if shell.aliases.contains_key(name) || shell.functions.contains_key(name) {
            return true;
        }
    }

    let builtins = [
        "exit", "cd", "source", ".", "pwd", "echo", "true", "false", "test", "[", "jobs", "fg",
        "bg", "export", "set", "setopt", "unsetopt", "unset", "alias", "unalias", "read", "shift",
        "local", "type", "kill", "wait", "umask", "return", "eval", "trap", "ulimit", "readonly",
        "history", "zstyle", "add-zsh-hook", "autoload", "compinit", "compdef", "compaudit",
        "bindkey", "zmodload",
    ];
    if builtins.contains(&name) {
        return true;
    }

    find_command(name).is_some()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::TEST_LOCK;

    #[test]
    fn test_expand_prompt_literal_percent() {
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        shell_init();
        assert_eq!(expand_prompt("%%"), "%");
    }

    #[test]
    fn test_expand_prompt_color_named() {
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        shell_init();
        let s = expand_prompt("%F{red}x%f");
        assert!(s.contains("\x1b[31m"), "missing red SGR in {:?}", s);
        assert!(s.contains('x'));
        assert!(s.contains("\x1b[39m"), "missing default fg reset in {:?}", s);
    }

    #[test]
    fn test_expand_prompt_color_numeric() {
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        shell_init();
        let s = expand_prompt("%F{208}orange%f");
        assert!(s.contains("\x1b[38;5;208m"));
    }

    #[test]
    fn test_expand_prompt_bold_underline() {
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        shell_init();
        let s = expand_prompt("%B%Utext%u%b");
        assert!(s.contains("\x1b[1m"));
        assert!(s.contains("\x1b[4m"));
        assert!(s.contains("\x1b[24m"));
        assert!(s.contains("\x1b[22m"));
    }

    #[test]
    fn test_expand_prompt_user_and_pwd() {
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        shell_init();
        var_set("USER", "alice", false);
        var_set("HOME", "/home/alice", false);
        var_set("PWD", "/home/alice/projects", false);
        let s = expand_prompt("%n at %~");
        assert_eq!(s, "alice at ~/projects");
    }

    #[test]
    fn test_expand_prompt_passes_through_unknown() {
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        shell_init();
        let s = expand_prompt("%Z");
        assert_eq!(s, "%Z");
    }

    #[test]
    fn test_expand_prompt_raw_braces() {
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        shell_init();
        let s = expand_prompt("%{\x1b[31m%}red%{\x1b[0m%}");
        assert_eq!(s, "\x1b[31mred\x1b[0m");
    }

    #[test]
    fn test_diagnostic_commands() {
        let _lock = TEST_LOCK.lock().unwrap();
        shell_init();
        let path = var_get("PATH");
        println!("PATH in meowsh: {}", path);
        
        let ls_exists = command_exists("ls");
        println!("ls exists: {}", ls_exists);
        assert!(ls_exists, "ls should exist in PATH");

        // Try cargo if it exists in the outer environment
        if let Ok(cargo_path) = std::process::Command::new("which").arg("cargo").output() {
            if cargo_path.status.success() {
                let cargo_exists = command_exists("cargo");
                println!("cargo exists: {}", cargo_exists);
                assert!(cargo_exists, "cargo should exist in PATH if it exists in environment");
            }
        }
    }
}

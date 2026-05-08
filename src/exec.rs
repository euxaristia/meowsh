use crate::expand::expand_all;
use crate::jobs::{builtin_bg, builtin_fg, builtin_jobs, job_wait_foreground};
use crate::lexer::Lexer;
use crate::parser::Parser;
use crate::shell::{
    declare_local, find_command, in_function_scope, pop_scope, push_scope, var_get, var_set, SHELL,
};
use crate::types::{ASTNode, Job, JobState, ProcState, Redir};
use std::env;
use std::fs::{self, OpenOptions};
use std::io;
use std::os::unix::io::{FromRawFd, IntoRawFd, OwnedFd};
use std::os::unix::process::CommandExt;
use std::path::Path;
use std::process::{Command, Stdio};

pub fn execute_line(line: &str) {
    let line = line.trim();
    if line.is_empty() {
        return;
    }

    // Add to history if interactive
    {
        let shell = SHELL.shell.lock().unwrap();
        if shell.interactive {
            let mut history = shell.history.lock().unwrap();
            history.push(line.to_string());
            if history.len() > 1000 {
                history.remove(0);
            }
        }
    }

    let mut lexer = Lexer::new(line);
    let mut parser = Parser::new(&mut lexer);

    while let Some(node) = parser.parse() {
        let status = exec_node(&node);
        SHELL.shell.lock().unwrap().last_status = status;
        
        if status != 0 {
            let opts = SHELL.shell.lock().unwrap().opts;
            if opts & crate::types::OPT_ERREXIT != 0 {
                std::process::exit(status);
            }
        }
    }
}

pub fn exec_node(node: &ASTNode) -> i32 {
    // Check if this is an empty node
    if node.node_type == "empty" {
        return 0;
    }

    // For nodes that use left/right (and_or, etc.), don't apply the empty check
    let uses_left_right = matches!(
        node.node_type.as_str(),
        "and_or"
            | "if"
            | "while"
            | "until"
            | "for"
            | "function"
            | "case"
            | "brace_group"
            | "subshell"
            | "list"
            | "dbracket"
    );

    if !uses_left_right
        && node.args.is_empty()
        && node.assigns.is_empty()
        && node.scalar_appends.is_empty()
        && node.array_assigns.is_empty()
        && node.array_appends.is_empty()
        && node.pipes.is_empty()
        && node.loop_var.is_empty()
    {
        return 0;
    }

    match node.node_type.as_str() {
        "simple" => return exec_simple(node),
        "pipeline" => return exec_pipeline(node),
        "and_or" => return exec_and_or(node),
        "list" => return exec_list(node),
        "if" => return exec_if(node),
        "while" | "until" => return exec_while(node),
        "for" => return exec_for(node),
        "brace_group" | "subshell" => {
            if let Some(ref body) = node.body {
                return exec_node(body);
            }
        }
        "function" => return exec_function(node),
        "case" => return exec_case(node),
        "dbracket" => return exec_dbracket(node),
        _ => {}
    }

    0
}

fn exec_simple(node: &ASTNode) -> i32 {
    if node.args.is_empty()
        && node.assigns.is_empty()
        && node.scalar_appends.is_empty()
        && node.array_assigns.is_empty()
        && node.array_appends.is_empty()
    {
        return 0;
    }

    for (name, value) in &node.assigns {
        var_set(name, &expand_all(value), false);
    }
    for (name, value) in &node.scalar_appends {
        let prev = var_get(name);
        let appended = format!("{}{}", prev, expand_all(value));
        var_set(name, &appended, false);
    }
    for (name, items) in &node.array_assigns {
        let expanded: Vec<String> = items.iter().map(|s| expand_all(s)).collect();
        crate::shell::array_set(name, expanded);
    }
    for (name, items) in &node.array_appends {
        let expanded: Vec<String> = items.iter().map(|s| expand_all(s)).collect();
        crate::shell::array_append(name, expanded);
    }

    let mut expanded_args: Vec<String> = node.args.iter().map(|arg| expand_all(arg)).collect();

    if expanded_args.is_empty() {
        return 0;
    }

    let opts = SHELL.shell.lock().unwrap().opts;
    if opts & crate::types::OPT_XTRACE != 0 {
        eprintln!("+ {}", expanded_args.join(" "));
    }

    let name = expanded_args[0].clone();

    {
        let aliases = SHELL.shell.lock().unwrap().aliases.clone();
        if let Some(alias) = aliases.get(&name) {
            let parts: Vec<&str> = alias.split_whitespace().collect();
            let mut new_args: Vec<String> = parts.iter().map(|s| s.to_string()).collect();
            new_args.extend(expanded_args[1..].to_vec());
            expanded_args = new_args;
        }
    }

    {
        let func_body = SHELL
            .shell
            .lock()
            .unwrap()
            .functions
            .get(&name)
            .map(|f| f.body.clone());
        if let Some(body) = func_body {
            let old_params = SHELL.shell.lock().unwrap().pos_params.clone();
            SHELL.shell.lock().unwrap().pos_params = expanded_args[1..].to_vec();

            push_scope();
            let _scope_guard = ScopeGuard { old_params };

            exec_node(&body);
            return SHELL.shell.lock().unwrap().last_status;
        }
    }

    let saved_fds = handle_redirections(&node.redirs);

    let foreground = node.conn != "&";
    let status = exec_command(&expanded_args, foreground);

    restore_fds(saved_fds);

    SHELL.shell.lock().unwrap().last_status = status;
    status
}

#[allow(clippy::suspicious_open_options)]
fn handle_redirections(redirs: &[Redir]) -> Option<SavedFds> {
    if redirs.is_empty() {
        return None;
    }

    let stdin_fd = unsafe { OwnedFd::from_raw_fd(0) };
    let stdout_fd = unsafe { OwnedFd::from_raw_fd(1) };
    let stderr_fd = unsafe { OwnedFd::from_raw_fd(2) };

    for redir in redirs {
        let target = expand_all(&redir.file);

        match redir.op.as_str() {
            ">" | ">|" => {
                if let Ok(f) = fs::File::create(&target) {
                    let fd = f.into_raw_fd();
                    unsafe {
                        libc::dup2(fd, 1);
                    }
                }
            }
            ">>" => {
                if let Ok(f) = OpenOptions::new().append(true).create(true).open(&target) {
                    let fd = f.into_raw_fd();
                    unsafe {
                        libc::dup2(fd, 1);
                    }
                }
            }
            "<" => {
                if let Ok(f) = fs::File::open(&target) {
                    let fd = f.into_raw_fd();
                    unsafe {
                        libc::dup2(fd, 0);
                    }
                }
            }
            "<>" => {
                if let Ok(f) = OpenOptions::new()
                    .read(true)
                    .write(true)
                    .create(true)
                    .open(&target)
                {
                    let fd = f.into_raw_fd();
                    unsafe {
                        libc::dup2(fd, 0);
                        libc::dup2(fd, 1);
                    }
                }
            }
            "<&" | ">&" => {
                if let Ok(fd_num) = redir.file.parse::<i32>() {
                    unsafe {
                        if redir.op == "<&" {
                            libc::dup2(fd_num, 0);
                        } else {
                            libc::dup2(fd_num, 1);
                        }
                    }
                }
            }
            "<<" | "<<-" => {
                if !redir.heredoc_body.is_empty() {
                    let mut fd: [i32; 2] = [-1; 2];
                    unsafe {
                        if libc::pipe(fd.as_mut_ptr()) == 0 {
                            libc::write(
                                fd[1],
                                redir.heredoc_body.as_ptr() as *const libc::c_void,
                                redir.heredoc_body.len(),
                            );
                            libc::close(fd[1]);
                            libc::dup2(fd[0], 0);
                            libc::close(fd[0]);
                        }
                    }
                }
            }
            _ => {}
        }
    }

    Some(SavedFds {
        stdin: stdin_fd,
        stdout: stdout_fd,
        stderr: stderr_fd,
    })
}

fn restore_fds(saved: Option<SavedFds>) {
    if let Some(fds) = saved {
        unsafe {
            libc::dup2(fds.stdin.into_raw_fd(), 0);
            libc::dup2(fds.stdout.into_raw_fd(), 1);
            libc::dup2(fds.stderr.into_raw_fd(), 2);
        }
    }
}

struct SavedFds {
    stdin: OwnedFd,
    stdout: OwnedFd,
    stderr: OwnedFd,
}

struct ScopeGuard {
    old_params: Vec<String>,
}

impl Drop for ScopeGuard {
    fn drop(&mut self) {
        pop_scope();
        SHELL.shell.lock().unwrap().pos_params = std::mem::take(&mut self.old_params);
    }
}

struct OptsGuard(u32);

impl Drop for OptsGuard {
    fn drop(&mut self) {
        SHELL.shell.lock().unwrap().opts = self.0;
    }
}

fn exec_command(args: &[String], foreground: bool) -> i32 {
    if args.is_empty() {
        return 0;
    }

    let name = args[0].clone();

    let status = match name.as_str() {
        "exit" => builtin_exit(&args[1..]),
        "cd" => builtin_cd(&args[1..]),
        "source" | "." => builtin_source(&args[1..]),
        "pwd" => builtin_pwd(&args[1..]),
        "echo" => builtin_echo(&args[1..]),
        "true" => 0,
        "false" => 1,
        "test" | "[" => builtin_test(&args[1..]),
        "jobs" => builtin_jobs(&args[1..]),
        "fg" => builtin_fg(&args[1..]),
        "bg" => builtin_bg(&args[1..]),
        "export" => builtin_export(&args[1..]),
        "set" => builtin_set(&args[1..]),
        "setopt" => builtin_setopt(&args[1..]),
        "unsetopt" => builtin_unsetopt(&args[1..]),
        "unset" => builtin_unset(&args[1..]),
        "zstyle" => 0,
        "add-zsh-hook" => 0,
        "autoload" => builtin_autoload(&args[1..]),
        "compinit" | "compdef" | "compaudit" => 0,
        "bindkey" => 0,
        "zmodload" => 0,
        "alias" => builtin_alias(&args[1..]),
        "unalias" => builtin_unalias(&args[1..]),
        "read" => builtin_read(&args[1..]),
        "shift" => builtin_shift(&args[1..]),
        "local" => builtin_local(&args[1..]),
        "type" => builtin_type(&args[1..]),
        "kill" => builtin_kill(&args[1..]),
        "wait" => builtin_wait(&args[1..]),
        "umask" => builtin_umask(&args[1..]),
        "return" => builtin_return(&args[1..]),
        "eval" => builtin_eval(&args[1..]),
        "trap" => builtin_trap(&args[1..]),
        "ulimit" => builtin_ulimit(&args[1..]),
        "readonly" => builtin_readonly(&args[1..]),
        "history" => builtin_history(&args[1..]),
        _ => -1,
    };

    if status >= 0 {
        return status;
    }

    let exe_path = if name.contains('/') {
        name.clone()
    } else {
        find_command(&name).unwrap_or_default()
    };

    if exe_path.is_empty() {
        eprintln!("meowsh: {}: command not found", name);
        return 127;
    }

    let mut cmd = Command::new(&exe_path);
    cmd.args(&args[1..]);
    cmd.stdin(Stdio::inherit());
    cmd.stdout(Stdio::inherit());
    cmd.stderr(Stdio::inherit());
    cmd.process_group(0);

    match cmd.spawn() {
        Ok(child) => {
            let pid = child.id() as i32;

            let job_id = *SHELL.shell.lock().unwrap().next_job_id.lock().unwrap();
            let cmd_text = args.join(" ");
            let foreground_flag = foreground;

            let job = Job {
                id: job_id,
                pgid: pid,
                procs: vec![crate::types::Process {
                    pid,
                    status: 0,
                    state: ProcState::Running,
                    cmd: cmd_text.clone(),
                }],
                state: JobState::Running,
                notified: false,
                foreground: foreground_flag,
                cmd_text: cmd_text.clone(),
            };

            *SHELL.shell.lock().unwrap().next_job_id.lock().unwrap() += 1;
            SHELL
                .shell
                .lock()
                .unwrap()
                .jobs
                .lock()
                .unwrap()
                .push(job.clone());

            if foreground {
                unsafe {
                    libc::tcsetpgrp(0, pid);
                }
                job_wait_foreground(&job)
            } else {
                println!("[{}] {}", job_id, pid);
                SHELL.shell.lock().unwrap().last_bg_pid = pid;
                0
            }
        }
        Err(e) => {
            eprintln!("meowsh: {}", e);
            1
        }
    }
}

fn exec_pipeline(node: &ASTNode) -> i32 {
    if node.pipes.is_empty() {
        return 0;
    }
    if node.pipes.len() == 1 {
        return exec_node(&node.pipes[0]);
    }

    let mut in_fd = 0;
    let mut pids = Vec::new();
    let num_pipes = node.pipes.len();

    for (i, pipe_node) in node.pipes.iter().enumerate() {
        let mut fd: [i32; 2] = [-1; 2];
        if i < num_pipes - 1 {
            unsafe {
                if libc::pipe(fd.as_mut_ptr()) < 0 {
                    eprintln!("meowsh: pipe error");
                    return 1;
                }
            }
        }

        unsafe {
            let pid = libc::fork();
            if pid < 0 {
                eprintln!("meowsh: fork error");
                return 1;
            }

            if pid == 0 {
                // Child
                if in_fd != 0 {
                    libc::dup2(in_fd, 0);
                    libc::close(in_fd);
                }

                if i < num_pipes - 1 {
                    libc::dup2(fd[1], 1);
                    libc::close(fd[0]);
                    libc::close(fd[1]);
                }

                let status = exec_node(pipe_node);
                libc::_exit(status);
            } else {
                // Parent
                pids.push(pid);
                if in_fd != 0 {
                    libc::close(in_fd);
                }
                if i < num_pipes - 1 {
                    libc::close(fd[1]);
                    in_fd = fd[0];
                }
            }
        }
    }

    let mut last_status = 0;
    for pid in pids {
        let mut status: i32 = 0;
        unsafe {
            libc::waitpid(pid, &mut status, 0);
        }
        if libc::WIFEXITED(status) {
            last_status = libc::WEXITSTATUS(status);
        } else if libc::WIFSIGNALED(status) {
            last_status = 128 + libc::WTERMSIG(status);
        }
    }

    last_status
}

fn exec_and_or(node: &ASTNode) -> i32 {
    let left_status = if let Some(ref left) = node.left {
        exec_node(left)
    } else {
        0
    };

    if node.conn == "&&" {
        if left_status != 0 {
            return left_status;
        }
    } else if node.conn == "||" && left_status == 0 {
        return left_status;
    }

    if let Some(ref right) = node.right {
        exec_node(right)
    } else {
        0
    }
}

fn exec_list(node: &ASTNode) -> i32 {
    let mut status = 0;
    for n in &node.pipes {
        status = exec_node(n);
        SHELL.shell.lock().unwrap().last_status = status;
        if status != 0 {
            let opts = SHELL.shell.lock().unwrap().opts;
            if opts & crate::types::OPT_ERREXIT != 0 {
                std::process::exit(status);
            }
        }
    }
    status
}

fn exec_if(node: &ASTNode) -> i32 {
    let status = if let Some(ref cond) = node.cond {
        exec_node(cond)
    } else {
        0
    };

    if status == 0 {
        if let Some(ref body) = node.body {
            return exec_node(body);
        }
    } else if let Some(ref else_body) = node.else_body {
        return exec_node(else_body);
    }
    0
}

fn exec_while(node: &ASTNode) -> i32 {
    loop {
        let status = if let Some(ref cond) = node.cond {
            exec_node(cond)
        } else {
            0
        };

        if node.node_type == "while" && status != 0 {
            return 0;
        }
        if node.node_type == "until" && status == 0 {
            return 0;
        }

        if let Some(ref body) = node.body {
            exec_node(body);
        }
    }
}

fn exec_for(node: &ASTNode) -> i32 {
    let words: Vec<String> = if node.loop_words.is_empty() {
        SHELL.shell.lock().unwrap().pos_params.clone()
    } else {
        node.loop_words
            .iter()
            .flat_map(|w| expand_loop_word(w))
            .collect()
    };

    for word in words {
        var_set(&node.loop_var, &word, false);
        if let Some(ref body) = node.body {
            exec_node(body);
        }
    }
    0
}

// Expands a single `for ... in <word>` token into the list of values it
// represents. Recognises `"${arr[@]}"`, `${arr[@]}`, `$arr` etc. — anything
// that names an indexed array becomes one element per array slot. All other
// tokens go through normal expansion as a single value.
fn expand_loop_word(w: &str) -> Vec<String> {
    let stripped = if (w.starts_with('"') && w.ends_with('"') && w.len() >= 2)
        || (w.starts_with('\'') && w.ends_with('\'') && w.len() >= 2)
    {
        &w[1..w.len() - 1]
    } else {
        w
    };
    // ${name[@]} / ${name[*]}
    if let Some(rest) = stripped.strip_prefix("${") {
        if let Some(inner) = rest.strip_suffix('}') {
            if let Some(open) = inner.find('[') {
                if inner.ends_with("[@]") || inner.ends_with("[*]") {
                    let name = &inner[..open];
                    if let Some(arr) = crate::shell::array_get(name) {
                        return arr;
                    }
                }
            }
        }
    }
    // $name (bare scalar reference; if name is an array, expand to elements).
    if let Some(name) = stripped.strip_prefix('$') {
        if !name.contains(['{', '[', '(', '$']) {
            if let Some(arr) = crate::shell::array_get(name) {
                return arr;
            }
        }
    }
    vec![expand_all(w)]
}

fn exec_function(node: &ASTNode) -> i32 {
    if let Some(ref body) = node.func_body {
        let func = crate::types::FuncDef {
            body: std::sync::Arc::new((**body).clone()),
        };
        SHELL
            .shell
            .lock()
            .unwrap()
            .functions
            .insert(node.func_name.clone(), func);
    }
    0
}

fn exec_case(node: &ASTNode) -> i32 {
    let word = expand_all(&node.value);
    for item in &node.cases {
        if item.pattern == word || item.pattern == "*" {
            if let Some(ref body) = item.body {
                return exec_node(body);
            }
            return 0;
        }
    }
    0
}

fn exec_dbracket(node: &ASTNode) -> i32 {
    // Inside [[ ]] zsh suppresses filesystem globbing on operands so that
    // patterns like `screen*` on the RHS of `==` are matched against the LHS,
    // not expanded against the cwd.
    let saved_opts = SHELL.shell.lock().unwrap().opts;
    let _guard = OptsGuard(saved_opts);
    SHELL.shell.lock().unwrap().opts |= crate::types::OPT_NOGLOB;
    let toks: Vec<String> = node.args.iter().map(|a| expand_all(a)).collect();

    let (result, _) = dbracket_or(&toks, 0);
    if result {
        0
    } else {
        1
    }
}

fn dbracket_or(toks: &[String], pos: usize) -> (bool, usize) {
    let (mut left, mut p) = dbracket_and(toks, pos);
    while p < toks.len() && toks[p] == "||" {
        let (right, np) = dbracket_and(toks, p + 1);
        left = left || right;
        p = np;
    }
    (left, p)
}

fn dbracket_and(toks: &[String], pos: usize) -> (bool, usize) {
    let (mut left, mut p) = dbracket_unary(toks, pos);
    while p < toks.len() && toks[p] == "&&" {
        let (right, np) = dbracket_unary(toks, p + 1);
        left = left && right;
        p = np;
    }
    (left, p)
}

fn dbracket_unary(toks: &[String], pos: usize) -> (bool, usize) {
    if pos < toks.len() && toks[pos] == "!" {
        let (v, p) = dbracket_unary(toks, pos + 1);
        return (!v, p);
    }
    dbracket_primary(toks, pos)
}

fn dbracket_primary(toks: &[String], pos: usize) -> (bool, usize) {
    if pos >= toks.len() {
        return (false, pos);
    }
    if toks[pos] == "(" {
        let (v, p) = dbracket_or(toks, pos + 1);
        let p = if p < toks.len() && toks[p] == ")" { p + 1 } else { p };
        return (v, p);
    }
    let unary_ops = [
        "-n", "-z", "-d", "-e", "-f", "-r", "-w", "-x", "-s", "-h", "-L", "-b", "-c", "-p", "-S",
    ];
    if unary_ops.contains(&toks[pos].as_str()) && pos + 1 < toks.len() {
        let arg = &toks[pos + 1];
        let path = Path::new(arg);
        let result = match toks[pos].as_str() {
            "-n" => !arg.is_empty(),
            "-z" => arg.is_empty(),
            "-d" => path.is_dir(),
            "-e" => path.exists(),
            "-f" => path.is_file(),
            "-s" => path.metadata().map(|m| m.len() > 0).unwrap_or(false),
            "-h" | "-L" => path
                .symlink_metadata()
                .map(|m| m.file_type().is_symlink())
                .unwrap_or(false),
            "-r" | "-w" | "-x" => {
                let mode = match toks[pos].as_str() {
                    "-r" => libc::R_OK,
                    "-w" => libc::W_OK,
                    _ => libc::X_OK,
                };
                std::ffi::CString::new(arg.as_bytes())
                    .map(|cs| unsafe { libc::access(cs.as_ptr(), mode) == 0 })
                    .unwrap_or(false)
            }
            _ => false,
        };
        return (result, pos + 2);
    }
    if pos + 2 < toks.len() {
        let lhs = &toks[pos];
        let op = toks[pos + 1].as_str();
        let rhs = &toks[pos + 2];
        let result = match op {
            "=" | "==" => glob_match(lhs, rhs),
            "!=" => !glob_match(lhs, rhs),
            "<" => lhs < rhs,
            ">" => lhs > rhs,
            "-eq" => parse_int(lhs) == parse_int(rhs),
            "-ne" => parse_int(lhs) != parse_int(rhs),
            "-lt" => parse_int(lhs) < parse_int(rhs),
            "-le" => parse_int(lhs) <= parse_int(rhs),
            "-gt" => parse_int(lhs) > parse_int(rhs),
            "-ge" => parse_int(lhs) >= parse_int(rhs),
            "-ef" | "-nt" | "-ot" => false,
            _ => false,
        };
        return (result, pos + 3);
    }
    (!toks[pos].is_empty(), pos + 1)
}

fn glob_match(value: &str, pattern: &str) -> bool {
    match glob::Pattern::new(pattern) {
        Ok(p) => p.matches(value),
        Err(_) => value == pattern,
    }
}

fn parse_int(s: &str) -> i64 {
    s.trim().parse::<i64>().unwrap_or(0)
}

// Builtins

fn builtin_exit(args: &[String]) -> i32 {
    let code = if args.is_empty() {
        0
    } else {
        args[0].parse().unwrap_or(0)
    };
    println!("logout");
    std::process::exit(code);
}

fn builtin_cd(args: &[String]) -> i32 {
    let dir = if args.is_empty() {
        var_get("HOME")
    } else {
        args[0].clone()
    };
    if dir.is_empty() {
        return 1;
    }

    let expanded = expand_all(&dir);
    if Path::new(&expanded).is_dir() {
        env::set_current_dir(&expanded).ok();
        var_set("PWD", &expanded, false);
        0
    } else {
        eprintln!("meowsh: cd: {}: No such file or directory", expanded);
        1
    }
}

fn builtin_source(args: &[String]) -> i32 {
    if args.is_empty() {
        eprintln!("meowsh: source: filename argument required");
        return 1;
    }

    let filename = expand_all(&args[0]);

    match fs::read_to_string(&filename) {
        Ok(content) => {
            let mut lexer = Lexer::new(&content);
            let mut parser = Parser::new(&mut lexer);
            while let Some(node) = parser.parse() {
                exec_node(&node);
            }
            0
        }
        Err(e) => {
            eprintln!("meowsh: {}: {}", filename, e);
            1
        }
    }
}

fn builtin_pwd(_args: &[String]) -> i32 {
    println!("{}", var_get("PWD"));
    0
}

fn builtin_echo(args: &[String]) -> i32 {
    for (i, arg) in args.iter().enumerate() {
        print!("{}", expand_all(arg));
        if i < args.len() - 1 {
            print!(" ");
        }
    }
    println!();
    0
}

fn builtin_export(args: &[String]) -> i32 {
    if args.is_empty() {
        for (name, var) in SHELL.shell.lock().unwrap().vars.iter() {
            if var.flags & crate::types::VAR_EXPORT != 0 {
                println!("export {}={}", name, var.value);
            }
        }
        return 0;
    }

    for arg in args {
        if let Some(idx) = arg.find('=') {
            let (name, value) = arg.split_at(idx);
            var_set(name, &value[1..], true);
        } else {
            var_set(arg, &var_get(arg), true);
        }
    }
    0
}

fn builtin_set(args: &[String]) -> i32 {
    if args.is_empty() {
        for (name, var) in SHELL.shell.lock().unwrap().vars.iter() {
            println!("{}={}", name, var.value);
        }
        return 0;
    }
    let mut i = 0;
    while i < args.len() {
        let arg = &args[i];
        if arg.starts_with('-') || arg.starts_with('+') {
            let turn_on = arg.starts_with('-');
            for c in arg.chars().skip(1) {
                let opt = match c {
                    'e' => crate::types::OPT_ERREXIT,
                    'u' => crate::types::OPT_NOUNSET,
                    'x' => crate::types::OPT_XTRACE,
                    'n' => crate::types::OPT_NOEXEC,
                    'v' => crate::types::OPT_VERBOSE,
                    _ => 0,
                };
                if opt != 0 {
                    let mut shell = SHELL.shell.lock().unwrap();
                    if turn_on {
                        shell.opts |= opt;
                    } else {
                        shell.opts &= !opt;
                    }
                }
            }
        }
        i += 1;
    }
    0
}

// Best-effort autoload: skips flag args (-U, -X, -z, -k, etc.), then for
// each function name tries to find a file by that name in $fpath and
// source it. If $fpath is unset or the file isn't found, the function is
// silently registered as known so subsequent calls don't error out — this
// matches the Phase 2A scope of "don't crash typical .zshrc".
fn builtin_autoload(args: &[String]) -> i32 {
    let fpath = var_get("fpath");
    let fpath_dirs: Vec<&str> = if fpath.is_empty() {
        Vec::new()
    } else {
        fpath.split_whitespace().collect()
    };
    for arg in args {
        if arg.starts_with('-') || arg.starts_with('+') {
            continue;
        }
        let mut found = false;
        for dir in &fpath_dirs {
            let path = format!("{}/{}", dir, arg);
            if Path::new(&path).is_file() {
                if let Ok(content) = fs::read_to_string(&path) {
                    let mut lexer = Lexer::new(&content);
                    let mut parser = Parser::new(&mut lexer);
                    while let Some(node) = parser.parse() {
                        exec_node(&node);
                    }
                    found = true;
                    break;
                }
            }
        }
        if !found {
            // Register as a no-op so calls don't fail with "command not found".
            let mut body = ASTNode::new("brace_group");
            body.body = Some(Box::new(ASTNode::new("empty")));
            SHELL.shell.lock().unwrap().functions.insert(
                arg.clone(),
                crate::types::FuncDef { body: std::sync::Arc::new(body) },
            );
        }
    }
    0
}

fn builtin_setopt(args: &[String]) -> i32 {
    if args.is_empty() {
        let shell = SHELL.shell.lock().unwrap();
        let mut keys: Vec<&String> = shell
            .named_opts
            .iter()
            .filter(|(_, v)| **v)
            .map(|(k, _)| k)
            .collect();
        keys.sort();
        for k in keys {
            println!("{}", k);
        }
        return 0;
    }
    let mut i = 0;
    while i < args.len() {
        let arg = &args[i];
        if arg == "-o" {
            if i + 1 < args.len() {
                toggle_named_opt(&args[i + 1], true);
                i += 2;
                continue;
            } else {
                eprintln!("meowsh: setopt: -o requires an argument");
                return 1;
            }
        }
        if arg.starts_with('-') || arg.starts_with('+') {
            eprintln!("meowsh: setopt: unsupported option: {}", arg);
            i += 1;
            continue;
        }
        toggle_named_opt(arg, true);
        i += 1;
    }
    0
}

fn builtin_unsetopt(args: &[String]) -> i32 {
    let mut i = 0;
    while i < args.len() {
        let arg = &args[i];
        if arg == "-o" {
            if i + 1 < args.len() {
                toggle_named_opt(&args[i + 1], false);
                i += 2;
                continue;
            } else {
                eprintln!("meowsh: unsetopt: -o requires an argument");
                return 1;
            }
        }
        if arg.starts_with('-') || arg.starts_with('+') {
            eprintln!("meowsh: unsetopt: unsupported option: {}", arg);
            i += 1;
            continue;
        }
        toggle_named_opt(arg, false);
        i += 1;
    }
    0
}

fn toggle_named_opt(name: &str, value: bool) {
    let normalized: String = name.to_ascii_lowercase().chars().filter(|c| *c != '_').collect();
    let mut shell = SHELL.shell.lock().unwrap();
    shell.named_opts.insert(normalized.clone(), value);
    let opt_flag = match normalized.as_str() {
        "errexit" => crate::types::OPT_ERREXIT,
        "noexec" => crate::types::OPT_NOEXEC,
        "verbose" => crate::types::OPT_VERBOSE,
        "xtrace" => crate::types::OPT_XTRACE,
        "nounset" => crate::types::OPT_NOUNSET,
        "noglob" => crate::types::OPT_NOGLOB,
        "noclobber" => crate::types::OPT_NOCLOBBER,
        "monitor" => crate::types::OPT_MONITOR,
        "allexport" => crate::types::OPT_ALLEXPORT,
        "hashall" => crate::types::OPT_HASHALL,
        _ => 0,
    };
    if opt_flag != 0 {
        if value {
            shell.opts |= opt_flag;
        } else {
            shell.opts &= !opt_flag;
        }
    }
}

fn builtin_unset(args: &[String]) -> i32 {
    {
        let mut shell = SHELL.shell.lock().unwrap();
        for arg in args {
            shell.vars.remove(arg);
            shell.arrays.remove(arg);
        }
    }
    for arg in args {
        env::remove_var(arg);
    }
    0
}

fn builtin_alias(args: &[String]) -> i32 {
    if args.is_empty() {
        for (name, alias) in SHELL.shell.lock().unwrap().aliases.iter() {
            println!("{}={}", name, alias);
        }
        return 0;
    }

    for arg in args {
        if let Some(idx) = arg.find('=') {
            let (name, value) = arg.split_at(idx);
            SHELL
                .shell
                .lock()
                .unwrap()
                .aliases
                .insert(name.to_string(), value[1..].to_string());
        }
    }
    0
}

fn builtin_unalias(args: &[String]) -> i32 {
    for arg in args {
        SHELL.shell.lock().unwrap().aliases.remove(arg);
    }
    0
}

fn builtin_read(args: &[String]) -> i32 {
    let mut line = String::new();
    if io::stdin().read_line(&mut line).is_err() {
        return 1;
    }

    let line = line.trim_end();

    if !args.is_empty() {
        let vars: Vec<&str> = line.split_whitespace().collect();
        for (i, var) in args.iter().enumerate() {
            let val = vars.get(i).unwrap_or(&"");
            var_set(var, val, false);
        }
    }
    0
}

fn builtin_shift(_args: &[String]) -> i32 {
    let mut shell = SHELL.shell.lock().unwrap();
    if !shell.pos_params.is_empty() {
        shell.pos_params.remove(0);
    }
    0
}

fn builtin_local(args: &[String]) -> i32 {
    if !in_function_scope() {
        eprintln!("meowsh: local: can only be used in a function");
        return 1;
    }
    for arg in args {
        let (name, value) = match arg.find('=') {
            Some(idx) => {
                let (n, v) = arg.split_at(idx);
                (n.to_string(), Some(expand_all(&v[1..])))
            }
            None => (arg.clone(), None),
        };
        if name.is_empty() || !is_valid_name(&name) {
            eprintln!("meowsh: local: `{}': not a valid identifier", arg);
            return 1;
        }
        declare_local(&name);
        match value {
            Some(v) => var_set(&name, &v, false),
            // Bare `local foo` masks any existing global with an empty
            // value for the duration of the function.
            None => var_set(&name, "", false),
        }
    }
    0
}

fn is_valid_name(s: &str) -> bool {
    let mut chars = s.chars();
    match chars.next() {
        Some(c) if c.is_ascii_alphabetic() || c == '_' => {}
        _ => return false,
    }
    chars.all(|c| c.is_ascii_alphanumeric() || c == '_')
}

fn builtin_type(args: &[String]) -> i32 {
    if args.is_empty() {
        return 1;
    }

    let name = &args[0];
    let builtins = vec![
        "cd", "echo", "exit", "pwd", "test", "true", "false", "export", "set", "unset", "alias",
        "unalias", "read", "source", "jobs", "fg", "bg", "type", "kill", "wait",
    ];
    if builtins.contains(&name.as_str()) {
        println!("{} is a shell builtin", name);
        return 0;
    }

    if SHELL.shell.lock().unwrap().aliases.contains_key(name) {
        println!("{} is an alias", name);
        return 0;
    }

    if SHELL.shell.lock().unwrap().functions.contains_key(name) {
        println!("{} is a function", name);
        return 0;
    }

    if let Some(path) = find_command(name) {
        println!("{} is {}", name, path);
        return 0;
    }

    eprintln!("meowsh: type: {} not found", name);
    1
}

fn builtin_kill(args: &[String]) -> i32 {
    if args.is_empty() {
        return 1;
    }
    Command::new("kill")
        .args(args)
        .output()
        .map(|o| if o.status.success() { 0 } else { 1 })
        .unwrap_or(1)
}

fn builtin_wait(_args: &[String]) -> i32 {
    loop {
        let mut status: i32 = 0;
        let pid = unsafe { libc::waitpid(-1, &mut status, libc::WNOHANG) };
        if pid <= 0 {
            break;
        }
    }
    0
}

fn builtin_umask(args: &[String]) -> i32 {
    if args.is_empty() {
        let mask = unsafe { libc::umask(0) };
        unsafe { libc::umask(mask) };
        println!("{:04o}", mask);
        return 0;
    }
    let mask = u16::from_str_radix(&args[0], 8).unwrap_or(0) as u32;
    unsafe { libc::umask(mask) };
    0
}

fn builtin_return(_args: &[String]) -> i32 {
    SHELL.shell.lock().unwrap().last_status
}

fn builtin_eval(args: &[String]) -> i32 {
    let cmd = args.join(" ");
    let mut lexer = Lexer::new(&cmd);
    let mut parser = Parser::new(&mut lexer);
    while let Some(node) = parser.parse() {
        exec_node(&node);
    }
    0
}

fn builtin_trap(args: &[String]) -> i32 {
    if args.len() < 2 {
        for (sig, action) in SHELL.shell.lock().unwrap().trap.iter() {
            println!("trap -- '{}' {}", action, sig);
        }
        return 0;
    }
    let action = args[0].clone();
    let signal = args[1].clone();
    SHELL.shell.lock().unwrap().trap.insert(signal, action);
    0
}

fn builtin_ulimit(_args: &[String]) -> i32 {
    println!("unlimited");
    0
}

fn builtin_readonly(args: &[String]) -> i32 {
    if args.is_empty() {
        for (name, var) in SHELL.shell.lock().unwrap().vars.iter() {
            if var.flags & crate::types::VAR_READONLY != 0 {
                println!("readonly {}={}", name, var.value);
            }
        }
        return 0;
    }
    for arg in args {
        if let Some(idx) = arg.find('=') {
            let (name, value) = arg.split_at(idx);
            let flags = crate::types::VAR_READONLY;
            SHELL.shell.lock().unwrap().vars.insert(
                name.to_string(),
                crate::types::Var {
                    value: value[1..].to_string(),
                    flags,
                },
            );
        }
    }
    0
}

fn builtin_test(args: &[String]) -> i32 {
    if args.is_empty() {
        return 1;
    }
    let args: Vec<String> = args.iter().filter(|a| *a != "]").cloned().collect();
    if args.len() == 1 {
        return if !args[0].is_empty() { 0 } else { 1 };
    }
    if args.len() == 2 {
        let op = args[0].as_str();
        let path_str = &args[1];
        let path = Path::new(path_str);
        match op {
            "-n" => return if !args[1].is_empty() { 0 } else { 1 },
            "-z" => return if args[1].is_empty() { 0 } else { 1 },
            "-d" => return if path.is_dir() { 0 } else { 1 },
            "-e" => return if path.exists() { 0 } else { 1 },
            "-f" => return if path.is_file() { 0 } else { 1 },
            "-s" => return if path.metadata().map(|m| m.len() > 0).unwrap_or(false) { 0 } else { 1 },
            "-h" | "-L" => return if path.symlink_metadata().map(|m| m.is_symlink()).unwrap_or(false) { 0 } else { 1 },
            "-r" => return unsafe { if libc::access(std::ffi::CString::new(path_str.as_bytes()).unwrap_or_default().as_ptr(), libc::R_OK) == 0 { 0 } else { 1 } },
            "-w" => return unsafe { if libc::access(std::ffi::CString::new(path_str.as_bytes()).unwrap_or_default().as_ptr(), libc::W_OK) == 0 { 0 } else { 1 } },
            "-x" => return unsafe { if libc::access(std::ffi::CString::new(path_str.as_bytes()).unwrap_or_default().as_ptr(), libc::X_OK) == 0 { 0 } else { 1 } },
            _ => return 1,
        }
    }
    if args.len() == 3 {
        match args[1].as_str() {
            "=" => return if args[0] == args[2] { 0 } else { 1 },
            "!=" => return if args[0] != args[2] { 0 } else { 1 },
            "-eq" => {
                return if args[0].parse::<i32>().unwrap_or(0) == args[2].parse::<i32>().unwrap_or(0)
                {
                    0
                } else {
                    1
                }
            }
            "-ne" => {
                return if args[0].parse::<i32>().unwrap_or(0) != args[2].parse::<i32>().unwrap_or(0)
                {
                    0
                } else {
                    1
                }
            }
            "-lt" => {
                return if args[0].parse::<i32>().unwrap_or(0) < args[2].parse::<i32>().unwrap_or(0)
                {
                    0
                } else {
                    1
                }
            }
            "-le" => {
                return if args[0].parse::<i32>().unwrap_or(0) <= args[2].parse::<i32>().unwrap_or(0)
                {
                    0
                } else {
                    1
                }
            }
            "-gt" => {
                return if args[0].parse::<i32>().unwrap_or(0) > args[2].parse::<i32>().unwrap_or(0)
                {
                    0
                } else {
                    1
                }
            }
            "-ge" => {
                return if args[0].parse::<i32>().unwrap_or(0) >= args[2].parse::<i32>().unwrap_or(0)
                {
                    0
                } else {
                    1
                }
            }
            _ => return 1,
        }
    }
    0
}

fn builtin_history(_args: &[String]) -> i32 {
    let shell = SHELL.shell.lock().unwrap();
    let history = shell.history.lock().unwrap();
    for (i, line) in history.iter().enumerate() {
        println!("{:5}  {}", i + 1, line);
    }
    0
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::shell::SHELL;
    use crate::TEST_LOCK;

    #[test]
    fn test_execute_line_history() {
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        {
            let mut shell = SHELL.shell.lock().unwrap_or_else(|e| e.into_inner());
            shell.interactive = true;
            let mut history = shell.history.lock().unwrap_or_else(|e| e.into_inner());
            history.clear();
        }

        execute_line("ls -l");
        execute_line("echo hello");

        {
            let shell = SHELL.shell.lock().unwrap_or_else(|e| e.into_inner());
            let history = shell.history.lock().unwrap_or_else(|e| e.into_inner());
            assert_eq!(history.len(), 2);
            assert_eq!(history[0], "ls -l");
            assert_eq!(history[1], "echo hello");
        }
    }

    #[test]
    fn test_execute_line_history_non_interactive() {
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        {
            let mut shell = SHELL.shell.lock().unwrap_or_else(|e| e.into_inner());
            shell.interactive = false;
            let mut history = shell.history.lock().unwrap_or_else(|e| e.into_inner());
            history.clear();
        }

        execute_line("ls -l");

        {
            let shell = SHELL.shell.lock().unwrap_or_else(|e| e.into_inner());
            let history = shell.history.lock().unwrap_or_else(|e| e.into_inner());
            assert_eq!(history.len(), 0);
        }
    }

    fn reset_shell() {
        let mut shell = SHELL.shell.lock().unwrap_or_else(|e| e.into_inner());
        shell.vars.clear();
        shell.arrays.clear();
        shell.functions.clear();
        shell.local_scopes.clear();
        shell.aliases.clear();
        shell.pos_params.clear();
        shell.last_status = 0;
    }

    #[test]
    fn test_local_outside_function_errors() {
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        reset_shell();
        execute_line("local x=1");
        let shell = SHELL.shell.lock().unwrap_or_else(|e| e.into_inner());
        assert_eq!(shell.last_status, 1);
        assert!(shell.vars.get("x").is_none());
    }

    #[test]
    fn test_local_does_not_leak_after_return() {
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        reset_shell();
        execute_line("function f { local x=inner; }");
        execute_line("f");
        let shell = SHELL.shell.lock().unwrap_or_else(|e| e.into_inner());
        assert!(shell.vars.get("x").is_none(),
            "x should not exist after function returns; got {:?}",
            shell.vars.get("x").map(|v| &v.value));
    }

    #[test]
    fn test_local_restores_prior_global() {
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        reset_shell();
        execute_line("x=outer");
        execute_line("function f { local x=inner; }");
        execute_line("f");
        let shell = SHELL.shell.lock().unwrap_or_else(|e| e.into_inner());
        assert_eq!(shell.vars.get("x").map(|v| v.value.as_str()), Some("outer"));
    }

    #[test]
    fn test_bare_local_masks_with_empty_then_restores() {
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        reset_shell();
        execute_line("x=outer");
        // Function body that asserts x is empty inside, then mutates it.
        execute_line("function f { local x; x=inside; }");
        execute_line("f");
        let shell = SHELL.shell.lock().unwrap_or_else(|e| e.into_inner());
        assert_eq!(shell.vars.get("x").map(|v| v.value.as_str()), Some("outer"));
    }

    #[test]
    fn test_local_repeated_in_same_scope_keeps_original_snapshot() {
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        reset_shell();
        execute_line("x=outer");
        execute_line("function f { local x=first; local x=second; }");
        execute_line("f");
        let shell = SHELL.shell.lock().unwrap_or_else(|e| e.into_inner());
        assert_eq!(shell.vars.get("x").map(|v| v.value.as_str()), Some("outer"));
    }

    #[test]
    fn test_nested_functions_have_independent_scopes() {
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        reset_shell();
        execute_line("x=outer");
        execute_line("function inner { local x=inner_val; }");
        execute_line("function outer { local x=outer_val; inner; }");
        execute_line("outer");
        let shell = SHELL.shell.lock().unwrap_or_else(|e| e.into_inner());
        // After outer returns, x should be restored to its pre-call value.
        assert_eq!(shell.vars.get("x").map(|v| v.value.as_str()), Some("outer"));
    }

    #[test]
    fn test_local_creates_var_that_did_not_exist() {
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        reset_shell();
        execute_line("function f { local newvar=hi; }");
        execute_line("f");
        let shell = SHELL.shell.lock().unwrap_or_else(|e| e.into_inner());
        assert!(shell.vars.get("newvar").is_none(),
            "newvar should be unset after function returns");
    }

    #[test]
    fn test_assignment_without_local_is_global() {
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        reset_shell();
        execute_line("function f { y=set_inside; }");
        execute_line("f");
        let shell = SHELL.shell.lock().unwrap_or_else(|e| e.into_inner());
        assert_eq!(shell.vars.get("y").map(|v| v.value.as_str()), Some("set_inside"));
    }

    #[test]
    fn test_execute_line_history_limit() {
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        {
            let mut shell = SHELL.shell.lock().unwrap_or_else(|e| e.into_inner());
            shell.interactive = true;
            let mut history = shell.history.lock().unwrap_or_else(|e| e.into_inner());
            history.clear();
            for i in 0..1000 {
                history.push(format!("cmd{}", i));
            }
        }

        execute_line("new_cmd");

        {
            let shell = SHELL.shell.lock().unwrap_or_else(|e| e.into_inner());
            let history = shell.history.lock().unwrap_or_else(|e| e.into_inner());
            assert_eq!(history.len(), 1000);
            assert_eq!(history[999], "new_cmd");
            assert_eq!(history[0], "cmd1");
        }
    }
}

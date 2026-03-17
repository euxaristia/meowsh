use crate::expand::{expand_all, run_command_output};
use crate::jobs::{builtin_bg, builtin_fg, builtin_jobs, job_wait_foreground};
use crate::lexer::Lexer;
use crate::parser::Parser;
use crate::shell::{var_get, var_set, SHELL};
use crate::types::{ASTNode, Job, JobState, ProcState, Redir};
use std::collections::HashMap;
use std::env;
use std::fs::{self, File, OpenOptions};
use std::io::{self, Read, Write};
use std::os::unix::io::{FromRawFd, IntoRawFd, OwnedFd};
use std::os::unix::process::CommandExt;
use std::path::Path;
use std::process::{Command, Stdio};

pub fn execute_line(line: &str) {
    let line = line.trim();
    if line.is_empty() {
        return;
    }

    let mut lexer = Lexer::new(line);
    let mut parser = Parser::new(&mut lexer);

    while let Some(node) = parser.parse() {
        let status = exec_node(&node);
        SHELL.shell.lock().unwrap().last_status = status;
    }
}

pub fn exec_node(node: &ASTNode) -> i32 {
    if node.node_type == "empty"
        || (node.args.is_empty() && node.assigns.is_empty() && node.pipes.is_empty())
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
        _ => {}
    }

    0
}

fn exec_simple(node: &ASTNode) -> i32 {
    if node.args.is_empty() && node.assigns.is_empty() {
        return 0;
    }

    for (name, value) in &node.assigns {
        var_set(name, &expand_all(value), false);
    }

    let mut expanded_args: Vec<String> = node.args.iter().map(|arg| expand_all(arg)).collect();

    if expanded_args.is_empty() {
        return 0;
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
        let functions = SHELL.shell.lock().unwrap().functions.clone();
        if let Some(func) = functions.get(&name) {
            let old_params = SHELL.shell.lock().unwrap().pos_params.clone();
            SHELL.shell.lock().unwrap().pos_params = expanded_args[1..].to_vec();

            let mut lexer = Lexer::new(&func.body);
            let mut parser = Parser::new(&mut lexer);
            if let Some(func_node) = parser.parse() {
                exec_node(&func_node);
            }

            SHELL.shell.lock().unwrap().pos_params = old_params;
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
                if let Ok(f) = OpenOptions::new()
                    .append(true)
                    .create(true)
                    .write(true)
                    .open(&target)
                {
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
        "unset" => builtin_unset(&args[1..]),
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
        find_command(&name)
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
                return job_wait_foreground(&job);
            } else {
                println!("[{}] {}", job_id, pid);
                SHELL.shell.lock().unwrap().last_bg_pid = pid;
                return 0;
            }
        }
        Err(e) => {
            eprintln!("meowsh: {}", e);
            return 1;
        }
    }
}

fn find_command(name: &str) -> String {
    let path = var_get("PATH");
    for dir in path.split(':') {
        if dir.is_empty() {
            continue;
        }
        let p = Path::new(dir).join(name);
        if p.exists() {
            return p.to_string_lossy().to_string();
        }
    }
    String::new()
}

fn exec_pipeline(node: &ASTNode) -> i32 {
    if node.pipes.is_empty() {
        return 0;
    }
    if node.pipes.len() == 1 {
        return exec_node(&node.pipes[0]);
    }

    let cmd_strs: Vec<String> = node
        .pipes
        .iter()
        .map(|n| {
            if n.node_type == "simple" {
                n.args.join(" ")
            } else {
                String::new()
            }
        })
        .collect();

    let result = Command::new("sh")
        .arg("-c")
        .arg(cmd_strs.join(" | "))
        .status();

    match result {
        Ok(s) => {
            if s.success() {
                0
            } else {
                s.code().unwrap_or(1)
            }
        }
        Err(_) => 1,
    }
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
    } else if node.conn == "||" {
        if left_status == 0 {
            return left_status;
        }
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
    let words = if node.loop_words.is_empty() {
        SHELL.shell.lock().unwrap().pos_params.clone()
    } else {
        node.loop_words.clone()
    };

    for word in words {
        var_set(&node.loop_var, &expand_all(&word), false);
        if let Some(ref body) = node.body {
            exec_node(body);
        }
    }
    0
}

fn exec_function(node: &ASTNode) -> i32 {
    if let Some(ref body) = node.func_body {
        let func = crate::types::FuncDef {
            body: body.args.join(" "),
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
    0
}

fn builtin_unset(args: &[String]) -> i32 {
    for arg in args {
        SHELL.shell.lock().unwrap().vars.remove(arg);
    }
    if !args.is_empty() {
        env::remove_var(&args[0]);
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
    for arg in args {
        if let Some(idx) = arg.find('=') {
            let (name, value) = arg.split_at(idx);
            var_set(name, &value[1..], false);
        }
    }
    0
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

    let path = find_command(name);
    if !path.is_empty() {
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
    let args: Vec<String> = args
        .iter()
        .map(|a| if a == "]" { String::new() } else { a.clone() })
        .collect();
    if args.len() == 1 {
        return if !args[0].is_empty() { 0 } else { 1 };
    }
    if args.len() == 2 {
        match args[0].as_str() {
            "-n" => return if !args[1].is_empty() { 0 } else { 1 },
            "-z" => return if args[1].is_empty() { 0 } else { 1 },
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

use crate::expand::{expand_all, run_command};
use crate::shell::{var_get, var_set, SHELL};

pub fn execute_line(line: &str) {
    let line = line.trim();
    if line.is_empty() {
        return;
    }

    // Check for alias expansion
    let words: Vec<&str> = line.split_whitespace().collect();
    let mut cmd_line = line.to_string();

    if let Some(first) = words.first() {
        let aliases = SHELL.shell.lock().unwrap().aliases.clone();
        if let Some(alias) = aliases.get(*first) {
            let rest = words[1..].join(" ");
            cmd_line = if rest.is_empty() {
                alias.clone()
            } else {
                format!("{} {}", alias, rest)
            };
        }
    }

    // Execute the line directly as a command
    execute_command(&cmd_line);
}

fn execute_command(cmd: &str) {
    let cmd = cmd.trim();
    if cmd.is_empty() {
        return;
    }

    // Check if it's a builtin
    if cmd == "exit" {
        println!("logout");
        std::process::exit(0);
    }
    if cmd == "cd" {
        builtin_cd("");
        return;
    }
    if cmd == "pwd" {
        println!("{}", var_get("PWD"));
        return;
    }
    if cmd == "true" || cmd == ":" {
        SHELL.shell.lock().unwrap().last_status = 0;
        return;
    }
    if cmd == "false" {
        SHELL.shell.lock().unwrap().last_status = 1;
        return;
    }
    if cmd == "alias" {
        let aliases = SHELL.shell.lock().unwrap().aliases.clone();
        for (name, alias) in aliases {
            println!("{}={}", name, alias);
        }
        return;
    }

    // Handle echo specially
    if cmd.starts_with("echo ") || cmd == "echo" {
        let args = if cmd.len() > 5 { &cmd[5..] } else { "" };
        println!("{}", expand_all(args));
        return;
    }

    // Handle assignments
    if cmd.contains('=') && !cmd.starts_with('=') && !cmd.contains(' ') {
        if let Some(idx) = cmd.find('=') {
            let (name, value) = cmd.split_at(idx);
            let value = &value[1..];
            var_set(name, value, false);
            return;
        }
    }

    // Handle cd with argument
    if cmd.starts_with("cd ") {
        let dir = cmd.strip_prefix("cd ").unwrap_or("");
        builtin_cd(dir);
        return;
    }

    // External command
    let output = run_command(cmd);

    {
        let mut shell = SHELL.shell.lock().unwrap();
        if output.status.success() {
            shell.last_status = 0;
        } else {
            shell.last_status = output.status.code().unwrap_or(1) as i32;
        }
    }

    // Print stdout
    print!("{}", String::from_utf8_lossy(&output.stdout));
    // Print stderr
    eprint!("{}", String::from_utf8_lossy(&output.stderr));
}

fn builtin_cd(dir: &str) {
    let dir = if dir.is_empty() {
        var_get("HOME")
    } else {
        expand_all(dir)
    };

    if dir.is_empty() {
        SHELL.shell.lock().unwrap().last_status = 1;
        return;
    }

    if std::path::Path::new(&dir).is_dir() {
        std::env::set_current_dir(&dir).ok();
        var_set("PWD", &dir, false);
        SHELL.shell.lock().unwrap().last_status = 0;
    } else {
        eprintln!("meowsh: cd: {}: No such file or directory", dir);
        SHELL.shell.lock().unwrap().last_status = 1;
    }
}

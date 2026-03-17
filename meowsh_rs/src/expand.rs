use crate::shell::{var_get, SHELL};
use std::process::Command;

pub fn expand_all(s: &str) -> String {
    // Simple expansion - just expand $VAR for now
    let mut result = String::new();
    let mut chars = s.chars().peekable();

    while let Some(c) = chars.next() {
        if c == '$' {
            if let Some(&next) = chars.peek() {
                if next == '{' {
                    chars.next(); // consume {
                    let mut name = String::new();
                    while let Some(ch) = chars.next() {
                        if ch == '}' {
                            break;
                        }
                        name.push(ch);
                    }
                    result.push_str(&var_get(&name));
                    continue;
                } else if next.is_alphabetic() || next == '_' {
                    let mut name = String::new();
                    while let Some(&ch) = chars.peek() {
                        if ch.is_alphanumeric() || ch == '_' {
                            name.push(chars.next().unwrap());
                        } else {
                            break;
                        }
                    }
                    result.push_str(&var_get(&name));
                    continue;
                }
            }
        }
        result.push(c);
    }
    result
}

pub fn run_command(cmd: &str) -> std::process::Output {
    Command::new("sh")
        .arg("-c")
        .arg(cmd)
        .output()
        .expect("failed to execute command")
}

use crate::shell::{var_get, var_set, SHELL};
use std::process::Command;

pub fn expand_all(s: &str) -> String {
    let s = expand_arithmetic(s);
    let s = expand_tilde(&s);
    let s = expand_command_substitution(&s);
    let s = expand_variable(&s);
    let s = remove_quotes(&s);
    s
}

pub fn expand_arithmetic(s: &str) -> String {
    if s.starts_with("$(") && s.ends_with(')') {
        let inner = &s[2..s.len() - 1];
        if inner.starts_with('(') && inner.ends_with(')') {
            let val = eval_arithmetic(&inner[1..inner.len() - 1]);
            return val.to_string();
        }
    }

    let mut result = String::new();
    let mut chars = s.chars().peekable();

    while let Some(c) = chars.next() {
        if c == '$' {
            if chars.peek() == Some(&'(') {
                chars.next();
                if chars.peek() == Some(&'(') {
                    chars.next();
                    let mut depth = 1;
                    let mut inner = String::new();
                    while let Some(ch) = chars.next() {
                        if ch == '(' && chars.peek() == Some(&'(') {
                            chars.next();
                            inner.push(ch);
                            depth += 1;
                        } else if ch == ')' && chars.peek() == Some(&')') {
                            chars.next();
                            depth -= 1;
                            if depth == 0 {
                                break;
                            }
                            inner.push(ch);
                        } else {
                            inner.push(ch);
                        }
                    }
                    let val = eval_arithmetic(&inner);
                    result.push_str(&val.to_string());
                    continue;
                } else {
                    let mut inner = String::new();
                    let mut depth = 1;
                    while let Some(ch) = chars.next() {
                        if ch == '(' {
                            depth += 1;
                        } else if ch == ')' {
                            depth -= 1;
                            if depth == 0 {
                                break;
                            }
                        }
                        inner.push(ch);
                    }
                    let out = run_command_output(&inner);
                    result.push_str(&out);
                    continue;
                }
            }
        }
        result.push(c);
    }
    result
}

pub fn eval_arithmetic(s: &str) -> i64 {
    let s = s.trim();
    if s.is_empty() {
        return 0;
    }

    let expanded = expand_variable(s);
    let expanded = expanded.trim();

    if let Ok(n) = expanded.parse::<i64>() {
        return n;
    }

    let mut result = 0i64;
    let mut current = 0i64;
    let mut op = '+';
    let mut num_buf = String::new();

    for c in expanded.chars() {
        if c.is_ascii_digit() {
            num_buf.push(c);
            continue;
        }

        if !num_buf.is_empty() {
            current = num_buf.parse().unwrap_or(0);
            num_buf.clear();

            match op {
                '+' => result += current,
                '-' => result -= current,
                '*' => result *= current,
                '/' => {
                    if current != 0 {
                        result /= current;
                    }
                }
                _ => {}
            }
        }

        if c == '+' || c == '-' || c == '*' || c == '/' {
            op = c;
        }
    }

    if !num_buf.is_empty() {
        current = num_buf.parse().unwrap_or(0);
        match op {
            '+' => result += current,
            '-' => result -= current,
            '*' => result *= current,
            '/' => {
                if current != 0 {
                    result /= current;
                }
            }
            _ => {}
        }
    }

    result
}

pub fn expand_tilde(s: &str) -> String {
    if !s.starts_with('~') {
        return s.to_string();
    }

    let slash_idx = s.find('/').unwrap_or(s.len());
    let username = &s[1..slash_idx];
    let rest = &s[slash_idx..];

    let home_dir = if username.is_empty() {
        var_get("HOME")
    } else {
        // For now, just return the original string for other users
        // Full ~username support would need proper user lookup
        return s.to_string();
    };

    if !home_dir.is_empty() {
        format!("{}{}", home_dir, rest)
    } else {
        s.to_string()
    }
}

pub fn expand_command_substitution(s: &str) -> String {
    let mut result = String::new();
    let mut chars = s.chars().peekable();

    while let Some(c) = chars.next() {
        if c == '$' {
            if chars.peek() == Some(&'(') {
                chars.next();
                if chars.peek() == Some(&'(') {
                    result.push('$');
                    result.push('(');
                    result.push('(');
                    while let Some(ch) = chars.next() {
                        result.push(ch);
                        if ch == ')' && chars.peek() == Some(&')') {
                            result.push(chars.next().unwrap());
                            break;
                        }
                    }
                } else {
                    let mut inner = String::new();
                    let mut depth = 1;
                    while let Some(ch) = chars.next() {
                        if ch == '(' {
                            depth += 1;
                        } else if ch == ')' {
                            depth -= 1;
                            if depth == 0 {
                                break;
                            }
                        }
                        inner.push(ch);
                    }
                    let out = run_command_output(&inner);
                    result.push_str(&out);
                }
                continue;
            }
        }
        if c == '`' {
            let mut inner = String::new();
            while let Some(ch) = chars.next() {
                if ch == '`' {
                    break;
                }
                inner.push(ch);
            }
            let out = run_command_output(&inner);
            result.push_str(&out);
            continue;
        }
        result.push(c);
    }
    result
}

pub fn run_command_output(cmd: &str) -> String {
    if cmd.is_empty() {
        return String::new();
    }
    match Command::new("sh").arg("-c").arg(cmd).output() {
        Ok(output) => {
            let s = String::from_utf8_lossy(&output.stdout);
            s.trim_end_matches('\n').to_string()
        }
        Err(_) => String::new(),
    }
}

pub fn expand_variable(s: &str) -> String {
    let mut result = String::new();
    let mut chars = s.chars().peekable();

    while let Some(c) = chars.next() {
        if c == '$' {
            if chars.peek() == Some(&'{') {
                chars.next();
                let mut expr = String::new();
                while let Some(ch) = chars.next() {
                    if ch == '}' {
                        break;
                    }
                    expr.push(ch);
                }
                result.push_str(&expand_var_expr(&expr));
                continue;
            } else {
                let mut name = String::new();
                while let Some(&ch) = chars.peek() {
                    if ch.is_alphanumeric()
                        || ch == '_'
                        || ch == '?'
                        || ch == '$'
                        || ch == '!'
                        || ch == '#'
                        || ch == '@'
                        || ch == '*'
                    {
                        name.push(chars.next().unwrap());
                    } else {
                        break;
                    }
                }
                if !name.is_empty() {
                    result.push_str(&var_get(&name));
                } else {
                    result.push('$');
                }
                continue;
            }
        }
        result.push(c);
    }

    let opts = SHELL.shell.lock().unwrap().opts;
    if opts & crate::types::OPT_NOGLOB == 0 {
        result = expand_glob(&result);
    }

    result
}

pub fn expand_var_expr(expr: &str) -> String {
    if expr.starts_with('#') && expr.len() > 1 {
        let val = var_get(&expr[1..]);
        return val.len().to_string();
    }

    let ops = vec![
        ":-", ":=", ":?", ":-", "-", "=", "?", "+", "%%", "%", "##", "#",
    ];

    for op in ops {
        if let Some(idx) = expr.find(op) {
            let param = &expr[..idx];
            let word = &expr[idx + op.len()..];
            let val = var_get(param);

            match op {
                ":-" | "-" => {
                    if val.is_empty() {
                        return expand_variable(word);
                    }
                    return val;
                }
                ":=" | "=" => {
                    if val.is_empty() {
                        let expanded = expand_variable(word);
                        var_set(param, &expanded, false);
                        return expanded;
                    }
                    return val;
                }
                ":?" | "?" => {
                    if val.is_empty() {
                        let msg = if word.is_empty() {
                            "parameter null or not set"
                        } else {
                            word
                        };
                        eprintln!("meowsh: {}: {}", param, msg);
                        return String::new();
                    }
                    return val;
                }
                ":+" | "+" => {
                    if !val.is_empty() {
                        return expand_variable(word);
                    }
                    return String::new();
                }
                _ => {}
            }
        }
    }

    var_get(expr)
}

pub fn expand_glob(s: &str) -> String {
    if !s.contains('*') && !s.contains('?') && !s.contains('[') {
        return s.to_string();
    }
    match glob::glob(s) {
        Ok(entries) => {
            let matches: Vec<String> = entries
                .filter_map(|e| e.ok())
                .filter_map(|e| e.to_str().map(|s| s.to_string()))
                .collect();
            if matches.is_empty() {
                s.to_string()
            } else {
                matches.join(" ")
            }
        }
        Err(_) => s.to_string(),
    }
}

pub fn remove_quotes(s: &str) -> String {
    let mut result = String::new();
    let mut chars = s.chars().peekable();
    let mut in_double = false;
    let mut in_single = false;

    while let Some(c) = chars.next() {
        if c == '\\' && !in_single {
            if let Some(&next) = chars.peek() {
                result.push(next);
                chars.next();
            }
            continue;
        }
        if c == '"' && !in_single {
            in_double = !in_double;
            continue;
        }
        if c == '\'' && !in_double {
            in_single = !in_single;
            continue;
        }
        result.push(c);
    }
    result
}

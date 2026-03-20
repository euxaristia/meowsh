use crate::shell::{var_get, var_get_if_exists, var_set, SHELL};
use std::process::Command;

pub fn expand_all(s: &str) -> String {
    let s = expand_arithmetic(s);
    let s = expand_tilde(&s);
    let s = expand_command_substitution(&s);
    let s = expand_variable(&s);

    remove_quotes(&s)
}

pub fn expand_arithmetic(s: &str) -> String {
    // Handle $((...)) arithmetic expansion
    if s.starts_with("$(((") && s.ends_with("))") {
        // Pattern: $((...)) -> extract "..."
        let inner = &s[4..s.len() - 2];
        let val = eval_arithmetic(inner);
        return val.to_string();
    }

    // Handle $(...) command substitution
    if s.starts_with("$(") && s.ends_with(')') {
        let inner = &s[2..s.len() - 1];
        if inner.starts_with('(') && inner.ends_with(')') {
            // It's $((...)), evaluate the content (remove the extra parentheses)
            let val = eval_arithmetic(&inner[1..inner.len() - 1]);
            return val.to_string();
        } else {
            // It's $(...), run command substitution
            let output = run_command_output(inner);
            return output.trim().to_string();
        }
    }

    let mut result = String::new();
    let mut chars = s.chars().peekable();

    while let Some(c) = chars.next() {
        if c == '$' && chars.peek() == Some(&'(') {
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
                for ch in chars.by_ref() {
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
        result.push(c);
    }
    result
}

pub fn eval_arithmetic(s: &str) -> i64 {
    let s = s.trim();
    if s.is_empty() {
        return 0;
    }

    // Check if it's just a number
    if let Ok(n) = s.parse::<i64>() {
        return n;
    }

    // Check if it's a variable reference (without $)
    if let Some(val) = var_get_if_exists(s) {
        return val;
    }

    // Handle operators recursively
    if s.contains('+') {
        let parts: Vec<&str> = s.splitn(2, '+').collect();
        return eval_arithmetic(parts[0]) + eval_arithmetic(parts[1]);
    }
    if s.contains('-') {
        let parts: Vec<&str> = s.splitn(2, '-').collect();
        return eval_arithmetic(parts[0]) - eval_arithmetic(parts[1]);
    }
    if s.contains('*') {
        let parts: Vec<&str> = s.splitn(2, '*').collect();
        return eval_arithmetic(parts[0]) * eval_arithmetic(parts[1]);
    }
    if s.contains('/') {
        let parts: Vec<&str> = s.splitn(2, '/').collect();
        let divisor = eval_arithmetic(parts[1]);
        if divisor == 0 {
            return 0;
        }
        return eval_arithmetic(parts[0]) / divisor;
    }

    // Try variable expansion with $
    let expanded = expand_variable(&format!("${}", s));
    if let Ok(n) = expanded.parse::<i64>() {
        return n;
    }

    0
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
        if c == '$' && chars.peek() == Some(&'(') {
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
                for ch in chars.by_ref() {
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
        if c == '`' {
            let mut inner = String::new();
            for ch in chars.by_ref() {
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
                for ch in chars.by_ref() {
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
                    let val = var_get(&name);
                    if val.is_empty() {
                        let is_unset = {
                            let shell = SHELL.shell.lock().unwrap();
                            if name == "$" || name == "?" || name == "!" || name == "0" || name == "#" || name == "@" || name == "*" || name == "-" {
                                false
                            } else {
                                !shell.vars.contains_key(&name)
                            }
                        };
                        if is_unset {
                            let opts = SHELL.shell.lock().unwrap().opts;
                            if opts & crate::types::OPT_NOUNSET != 0 {
                                eprintln!("meowsh: {}: unbound variable", name);
                                std::process::exit(1);
                            }
                        }
                    }
                    result.push_str(&val);
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

    fn expand_pattern(word: &str) -> String {
        let old_opts = { SHELL.shell.lock().unwrap().opts };
        { SHELL.shell.lock().unwrap().opts |= crate::types::OPT_NOGLOB; }
        let res = expand_variable(word);
        { SHELL.shell.lock().unwrap().opts = old_opts; }
        res
    }

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
                "%" => {
                    let pattern = expand_pattern(word);
                    return strip_suffix(&val, &pattern, false);
                }
                "%%" => {
                    let pattern = expand_pattern(word);
                    return strip_suffix(&val, &pattern, true);
                }
                "#" => {
                    let pattern = expand_pattern(word);
                    return strip_prefix(&val, &pattern, false);
                }
                "##" => {
                    let pattern = expand_pattern(word);
                    return strip_prefix(&val, &pattern, true);
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

fn match_pattern(s: &str, p: &str) -> bool {
    let s_chars: Vec<char> = s.chars().collect();
    let p_chars: Vec<char> = p.chars().collect();
    match_chars(&s_chars, &p_chars, 0, 0)
}

fn match_chars(s: &[char], p: &[char], i: usize, j: usize) -> bool {
    if j == p.len() {
        return i == s.len();
    }
    if p[j] == '*' {
        if match_chars(s, p, i, j + 1) {
            return true;
        }
        if i < s.len() && match_chars(s, p, i + 1, j) {
            return true;
        }
        return false;
    }
    if p[j] == '\\' && j + 1 < p.len() {
        if i < s.len() && p[j + 1] == s[i] {
            return match_chars(s, p, i + 1, j + 2);
        }
        return false;
    }
    if i < s.len() && (p[j] == '?' || p[j] == s[i]) {
        return match_chars(s, p, i + 1, j + 1);
    }
    false
}

fn strip_suffix(val: &str, pattern: &str, longest: bool) -> String {
    if longest {
        for i in 0..=val.len() {
            if match_pattern(&val[i..], pattern) {
                return val[..i].to_string();
            }
        }
    } else {
        for i in (0..=val.len()).rev() {
            if match_pattern(&val[i..], pattern) {
                return val[..i].to_string();
            }
        }
    }
    val.to_string()
}

fn strip_prefix(val: &str, pattern: &str, longest: bool) -> String {
    if longest {
        for i in (0..=val.len()).rev() {
            if match_pattern(&val[..i], pattern) {
                return val[i..].to_string();
            }
        }
    } else {
        for i in 0..=val.len() {
            if match_pattern(&val[..i], pattern) {
                return val[i..].to_string();
            }
        }
    }
    val.to_string()
}


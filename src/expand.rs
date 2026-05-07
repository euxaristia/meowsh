use crate::shell::{array_get, var_get, var_get_if_exists, var_set, SHELL};
use std::process::Command;

pub fn expand_all(s: &str) -> String {
    let s = expand_arithmetic(s);
    let s = expand_tilde(&s);
    let s = expand_command_substitution(&s);
    let s = expand_variable(&s);
    let s = expand_braces(&s);

    remove_quotes(&s)
}

pub fn expand_braces(s: &str) -> String {
    let mut brace_start = None;
    let mut brace_level = 0;
    let chars: Vec<char> = s.chars().collect();

    for i in 0..chars.len() {
        match chars[i] {
            '{' => {
                if brace_level == 0 {
                    brace_start = Some(i);
                }
                brace_level += 1;
            }
            '}' => {
                if brace_level > 0 {
                    brace_level -= 1;
                    if brace_level == 0 {
                        if let Some(start) = brace_start {
                            let prefix = &s[..start];
                            let suffix = &s[i + 1..];
                            let inner = &s[start + 1..i];

                            // Check for top-level commas in the inner part
                            let mut parts = Vec::new();
                            let mut current_part = String::new();
                            let mut inner_brace_level = 0;
                            for c in inner.chars() {
                                if c == ',' && inner_brace_level == 0 {
                                    parts.push(current_part);
                                    current_part = String::new();
                                } else {
                                    if c == '{' {
                                        inner_brace_level += 1;
                                    } else if c == '}' {
                                        inner_brace_level -= 1;
                                    }
                                    current_part.push(c);
                                }
                            }
                            parts.push(current_part);

                            if parts.len() > 1 {
                                let mut result = Vec::new();
                                for part in parts {
                                    let expanded = format!("{}{}{}", prefix, part, suffix);
                                    result.push(expand_braces(&expanded));
                                }
                                return result.join(" ");
                            } else {
                                // If it's just {a}, we should NOT expand it, but we MUST
                                // check if there are OTHER braces in the suffix.
                                // We can just reconstruct and continue searching after this brace.
                                let reconstructed = format!("{}{}{}{}{}", prefix, '{', inner, '}', expand_braces(suffix));
                                return reconstructed;
                            }
                        }
                    }
                }
            }
            _ => {}
        }
    }

    s.to_string()
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
        let inner = &expr[1..];
        // ${#arr[@]} or ${#arr[*]} — element count.
        if let Some((name, sub)) = split_subscript(inner) {
            if matches!(sub, "@" | "*") {
                if let Some(arr) = array_get(name) {
                    return arr.len().to_string();
                }
            }
            // ${#arr[N]} — length of nth element.
            return read_subscript(name, sub).chars().count().to_string();
        }
        let val = var_get(inner);
        return val.chars().count().to_string();
    }

    // ${arr[subscript]} — subscripted read.
    if let Some((name, sub)) = split_subscript(expr) {
        return read_subscript(name, sub);
    }

    // ${var/old/new} and ${var//old/new} — pattern substitution.
    if let Some(result) = try_substitution(expr) {
        return result;
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

// Splits `name[subscript]` into (name, subscript) if the expression has the
// shape `name[...]` with no operator characters in `name`. Returns None if
// the bracket form isn't present or if `name` would be empty.
fn split_subscript(expr: &str) -> Option<(&str, &str)> {
    let open = expr.find('[')?;
    if !expr.ends_with(']') {
        return None;
    }
    let name = &expr[..open];
    if name.is_empty() {
        return None;
    }
    if name.contains(|c: char| matches!(c, ':' | '#' | '%' | '/' | '?' | '+' | '-' | '=')) {
        return None;
    }
    let sub = &expr[open + 1..expr.len() - 1];
    Some((name, sub))
}

fn read_subscript(name: &str, sub: &str) -> String {
    let arr = match array_get(name) {
        Some(a) => a,
        None => {
            // Fall back to scalar — `${var[1]}` on a scalar returns the
            // whole value for [1]/[@]/[*], empty otherwise.
            let val = var_get(name);
            return match sub {
                "@" | "*" | "1" => val,
                _ => String::new(),
            };
        }
    };
    if matches!(sub, "@" | "*") {
        return arr.join(" ");
    }
    if let Ok(i) = sub.parse::<isize>() {
        let len = arr.len() as isize;
        let idx = if i > 0 {
            i - 1
        } else if i < 0 {
            len + i
        } else {
            return String::new();
        };
        if idx >= 0 && (idx as usize) < arr.len() {
            return arr[idx as usize].clone();
        }
    }
    String::new()
}

// `${var/old/new}` (first match) and `${var//old/new}` (all matches).
// Pattern is treated as a glob via the `glob` crate. Returns None if the
// expression doesn't have this shape. Backslash-escaped slashes inside the
// pattern or replacement are preserved as literal `/`.
fn try_substitution(expr: &str) -> Option<String> {
    let first_slash = find_unescaped_slash(expr, 0)?;
    let name = &expr[..first_slash];
    if name.is_empty() {
        return None;
    }
    if name.contains(|c: char| matches!(c, ':' | '#' | '%' | '?' | '+' | '-' | '=' | '[')) {
        return None;
    }
    let mut rest_start = first_slash + 1;
    let mut global = false;
    if expr[rest_start..].starts_with('/') {
        global = true;
        rest_start += 1;
    }
    let mut anchor_start = false;
    let mut anchor_end = false;
    if expr[rest_start..].starts_with('#') {
        anchor_start = true;
        rest_start += 1;
    } else if expr[rest_start..].starts_with('%') {
        anchor_end = true;
        rest_start += 1;
    }
    let (pat_raw, repl_raw) = match find_unescaped_slash(expr, rest_start) {
        Some(i) => (&expr[rest_start..i], &expr[i + 1..]),
        None => (&expr[rest_start..], ""),
    };
    let pat = unescape_slashes(pat_raw);
    let repl = unescape_slashes(repl_raw);
    let value = var_get(name);
    let result = substitute(&value, &pat, &repl, global, anchor_start, anchor_end);
    Some(result)
}

fn find_unescaped_slash(s: &str, start: usize) -> Option<usize> {
    let bytes = s.as_bytes();
    let mut i = start;
    while i < bytes.len() {
        if bytes[i] == b'\\' && i + 1 < bytes.len() {
            i += 2;
            continue;
        }
        if bytes[i] == b'/' {
            return Some(i);
        }
        i += 1;
    }
    None
}

fn unescape_slashes(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    let mut chars = s.chars().peekable();
    while let Some(c) = chars.next() {
        if c == '\\' {
            if let Some(&nc) = chars.peek() {
                if nc == '/' || nc == '\\' {
                    chars.next();
                    out.push(nc);
                    continue;
                }
            }
        }
        out.push(c);
    }
    out
}

fn substitute(
    value: &str,
    pat: &str,
    repl: &str,
    global: bool,
    anchor_start: bool,
    anchor_end: bool,
) -> String {
    if pat.is_empty() {
        return value.to_string();
    }
    let glob_pat = match glob::Pattern::new(pat) {
        Ok(p) => Some(p),
        Err(_) => None,
    };
    let has_meta = pat.contains(['*', '?', '[']);
    let matches_at = |s: &str| -> Option<usize> {
        if !has_meta {
            // Literal substring match — much faster than per-substring glob.
            return s.find(pat).map(|_| pat.len());
        }
        // Glob path: try increasing lengths starting from `s`.
        let glob_pat = glob_pat.as_ref()?;
        let opts = glob::MatchOptions {
            case_sensitive: true,
            require_literal_separator: false,
            require_literal_leading_dot: false,
        };
        for end in 0..=s.len() {
            if let Some(slice) = s.get(..end) {
                if glob_pat.matches_with(slice, opts) {
                    return Some(end);
                }
            }
        }
        None
    };
    if anchor_start {
        if let Some(len) = matches_at(value) {
            let mut out = String::with_capacity(value.len());
            out.push_str(repl);
            out.push_str(&value[len..]);
            return out;
        }
        return value.to_string();
    }
    if anchor_end {
        // Try suffixes from longest to shortest.
        for start in 0..=value.len() {
            let suffix = &value[start..];
            if let Some(len) = matches_at(suffix) {
                if start + len == value.len() {
                    let mut out = String::with_capacity(value.len());
                    out.push_str(&value[..start]);
                    out.push_str(repl);
                    return out;
                }
            }
        }
        return value.to_string();
    }
    // Unanchored. Walk byte positions; for literal pattern we can use find.
    if !has_meta {
        if global {
            return value.replace(pat, repl);
        }
        return value.replacen(pat, repl, 1);
    }
    // Glob unanchored: scan positions.
    let mut out = String::new();
    let mut i = 0;
    let bytes = value.as_bytes();
    while i <= bytes.len() {
        let suffix = &value[i..];
        if let Some(len) = matches_at(suffix) {
            if len == 0 {
                if i < bytes.len() {
                    out.push(bytes[i] as char);
                }
                i += 1;
                continue;
            }
            out.push_str(repl);
            i += len;
            if !global {
                out.push_str(&value[i..]);
                return out;
            }
        } else {
            if i < bytes.len() {
                out.push(bytes[i] as char);
            }
            i += 1;
        }
    }
    out
}

pub fn expand_glob(s: &str) -> String {
    if !s.contains('*') && !s.contains('?') && !s.contains('[') {
        return s.to_string();
    }

    // If it contains **, use globwalk for recursive globbing
    if s.contains("**") {
        let mut matches = Vec::new();
        // globwalk expects patterns relative to a base directory or absolute
        // We'll try to use current directory as base if it's a relative pattern
        let base = if s.starts_with('/') { "/" } else { "." };
        let pattern = if s.starts_with('/') { &s[1..] } else { s };

        if let Ok(walker) = globwalk::GlobWalkerBuilder::from_patterns(base, &[pattern])
            .follow_links(true)
            .build()
        {
            for entry in walker.filter_map(|e| e.ok()) {
                if let Some(path_str) = entry.path().to_str() {
                    let p = if path_str.starts_with("./") {
                        &path_str[2..]
                    } else {
                        path_str
                    };
                    matches.push(p.to_string());
                }
            }
        }

        if matches.is_empty() {
            return s.to_string();
        }
        matches.sort();
        return matches.join(" ");
    }

    match glob::glob(s) {
        Ok(entries) => {
            let mut matches: Vec<String> = entries
                .filter_map(|e| e.ok())
                .filter_map(|e| e.to_str().map(|s| s.to_string()))
                .collect();
            if matches.is_empty() {
                s.to_string()
            } else {
                matches.sort();
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


use crate::shell::build_prompt;
use rustyline::completion::Completer;
use rustyline::config::Configurer;
use rustyline::error::ReadlineError;
use rustyline::highlight::Highlighter;
use rustyline::hint::Hinter;
use rustyline::history::FileHistory;
use rustyline::validate::Validator;
use rustyline::{ColorMode, Editor, Helper};
use std::borrow::Cow;

pub struct MeowshHelper;

impl MeowshHelper {
    fn command_exists(&self, name: &str) -> bool {
        let shell = crate::shell::SHELL.shell.lock().unwrap();

        if shell.aliases.contains_key(name) || shell.functions.contains_key(name) {
            return true;
        }

        let builtins = [
            "exit", "cd", "source", "pwd", "echo", "true", "false", "test", "[", "jobs", "fg",
            "bg", "export", "set", "unset", "alias", "unalias", "read", "shift", "local",
            "type", "kill", "wait", "umask", "return", "eval", "trap", "ulimit", "readonly",
            "history",
        ];
        if builtins.contains(&name) {
            return true;
        }

        if name.contains('/') {
            return std::path::Path::new(name).is_file();
        }

        let path = shell
            .vars
            .get("PATH")
            .map(|v| v.value.as_str())
            .unwrap_or("");
        for dir in path.split(':') {
            if dir.is_empty() {
                continue;
            }
            if std::path::Path::new(dir).join(name).is_file() {
                return true;
            }
        }

        false
    }

    fn get_completions(&self, line: &str) -> Vec<rustyline::completion::Pair> {
        let mut completions = Vec::new();
        let shell = crate::shell::SHELL.shell.lock().unwrap();

        let parts: Vec<&str> = line.split_whitespace().collect();
        let is_cmd_position = parts.is_empty() || (parts.len() == 1 && !parts[0].contains('/'));

        if is_cmd_position {
            let current = parts.last().unwrap_or(&"");

            for name in shell.aliases.keys() {
                if name.starts_with(current) {
                    completions.push(rustyline::completion::Pair {
                        display: name.clone(),
                        replacement: name.clone(),
                    });
                }
            }

            for name in shell.functions.keys() {
                if name.starts_with(current) {
                    completions.push(rustyline::completion::Pair {
                        display: name.clone(),
                        replacement: name.clone(),
                    });
                }
            }

            let builtins = [
                "exit", "cd", "source", "pwd", "echo", "true", "false", "test", "[", "jobs", "fg",
                "bg", "export", "set", "unset", "alias", "unalias", "read", "shift", "local",
                "type", "kill", "wait", "umask", "return", "eval", "trap", "ulimit", "readonly",
            ];
            for b in builtins {
                if b.starts_with(current) {
                    completions.push(rustyline::completion::Pair {
                        display: b.to_string(),
                        replacement: b.to_string(),
                    });
                }
            }

            let path = shell
                .vars
                .get("PATH")
                .map(|v| v.value.as_str())
                .unwrap_or("");
            for dir in path.split(':') {
                if dir.is_empty() {
                    continue;
                }
                if let Ok(entries) = std::fs::read_dir(dir) {
                    for entry in entries.flatten() {
                        if let Ok(name) = entry.file_name().into_string() {
                            if name.starts_with(current) {
                                completions.push(rustyline::completion::Pair {
                                    display: name.clone(),
                                    replacement: name,
                                });
                            }
                        }
                    }
                }
            }
        }

        completions
    }
}

impl Completer for MeowshHelper {
    type Candidate = rustyline::completion::Pair;

    fn complete(
        &self,
        line: &str,
        pos: usize,
        _ctx: &rustyline::Context<'_>,
    ) -> Result<(usize, Vec<rustyline::completion::Pair>), ReadlineError> {
        Ok((pos, self.get_completions(line)))
    }
}

impl Highlighter for MeowshHelper {
    fn highlight<'l>(&self, line: &'l str, _pos: usize) -> Cow<'l, str> {
        if line.is_empty() {
            return Cow::Borrowed(line);
        }

        let mut colored = String::with_capacity(line.len() * 2);
        let mut i = 0;
        let chars: Vec<char> = line.chars().collect();
        let mut is_start_of_command = true;

        while i < chars.len() {
            match chars[i] {
                ' ' | '\t' => {
                    colored.push(chars[i]);
                    i += 1;
                }
                '#' if is_start_of_command => {
                    colored.push_str("\x1b[90m");
                    colored.extend(&chars[i..]);
                    colored.push_str("\x1b[0m");
                    break;
                }
                ';' | '|' | '&' => {
                    colored.push_str("\x1b[36m");
                    colored.push(chars[i]);
                    if i + 1 < chars.len() && chars[i + 1] == chars[i] && (chars[i] == '|' || chars[i] == '&') {
                        colored.push(chars[i + 1]);
                        i += 1;
                    }
                    colored.push_str("\x1b[0m");
                    is_start_of_command = true;
                    i += 1;
                }
                '\'' | '"' => {
                    let quote = chars[i];
                    colored.push_str("\x1b[33m");
                    colored.push(quote);
                    i += 1;
                    while i < chars.len() && chars[i] != quote {
                        if chars[i] == '\\' && i + 1 < chars.len() {
                            colored.push('\\');
                            colored.push(chars[i+1]);
                            i += 2;
                        } else {
                            colored.push(chars[i]);
                            i += 1;
                        }
                    }
                    if i < chars.len() {
                        colored.push(quote);
                        i += 1;
                    }
                    colored.push_str("\x1b[0m");
                    is_start_of_command = false;
                }
                '$' => {
                    colored.push_str("\x1b[35m");
                    colored.push('$');
                    i += 1;
                    while i < chars.len() && (chars[i].is_alphanumeric() || chars[i] == '_') {
                        colored.push(chars[i]);
                        i += 1;
                    }
                    colored.push_str("\x1b[0m");
                    is_start_of_command = false;
                }
                _ => {
                    let start = i;
                    while i < chars.len() && !chars[i].is_whitespace() && !";|&'\"$".contains(chars[i]) {
                        i += 1;
                    }
                    let word: String = chars[start..i].iter().collect();
                    if is_start_of_command {
                        if self.command_exists(&word) {
                            colored.push_str("\x1b[32m");
                        } else {
                            colored.push_str("\x1b[31m");
                        }
                        colored.push_str(&word);
                        colored.push_str("\x1b[0m");
                        is_start_of_command = false;
                    } else {
                        colored.push_str(&word);
                    }
                }
            }
        }

        Cow::Owned(colored)
    }

    fn highlight_prompt<'b, 's: 'b, 'p: 'b>(
        &self,
        prompt: &'p str,
        _default: bool,
    ) -> Cow<'b, str> {
        // Ensure trailing space for input separation
        if prompt.ends_with(' ') {
            Cow::Borrowed(prompt)
        } else {
            Cow::Owned(format!("{} ", prompt))
        }
    }
}

#[derive(Hash, Debug, PartialEq, Eq)]
pub struct MeowshHint {
    display: String,
    completion: String,
}

impl rustyline::hint::Hint for MeowshHint {
    fn display(&self) -> &str {
        &self.display
    }

    fn completion(&self) -> Option<&str> {
        Some(&self.completion)
    }
}

impl Hinter for MeowshHelper {
    type Hint = MeowshHint;

    fn hint(&self, line: &str, pos: usize, _ctx: &rustyline::Context<'_>) -> Option<MeowshHint> {
        // Only show hint if cursor is at the end of the line
        if pos < line.len() {
            return None;
        }

        let trimmed_input = line.trim_start();
        if trimmed_input.is_empty() {
            return None;
        }

        let shell = crate::shell::SHELL.shell.lock().unwrap();
        let history = shell.history.lock().unwrap();

        let lower_input = trimmed_input.to_lowercase();
        for hist in history.iter().rev() {
            let lower_hist = hist.to_lowercase();
            if lower_hist.starts_with(&lower_input) && hist.chars().count() > trimmed_input.chars().count() {
                // Return what's left of the history entry after the input
                // We skip the number of characters in the input to get the hint suffix
                let char_count = trimmed_input.chars().count();
                let hint_text: String = hist.chars().skip(char_count).collect();
                
                return Some(MeowshHint {
                    display: format!("\x1b[90m{}\x1b[0m", hint_text),
                    completion: hint_text,
                });
            }
        }
        None
    }
}

impl Validator for MeowshHelper {}

impl Helper for MeowshHelper {}

pub fn run_interactive() {
    let mut rl: Editor<MeowshHelper, FileHistory> = match Editor::new() {
        Ok(rl) => rl,
        Err(_) => {
            return run_noninteractive();
        }
    };

    rl.set_color_mode(ColorMode::Forced);
    rl.set_helper(Some(MeowshHelper));

    let shell = crate::shell::SHELL.shell.lock().unwrap();
    let history_file = shell.history_file.clone();
    drop(shell);

    if !history_file.is_empty() {
        let _ = rl.load_history(&history_file);
        if let Ok(content) = std::fs::read_to_string(&history_file) {
            let shell = crate::shell::SHELL.shell.lock().unwrap();
            let mut history = shell.history.lock().unwrap();
            *history = content.lines().map(|s| s.to_string()).collect();
        }
    }

    loop {
        let prompt = build_prompt();

        let readline = rl.readline(&prompt);
        match readline {
            Ok(line) => {
                let trimmed = line.trim();
                if trimmed.is_empty() {
                    continue;
                }

                if !line.is_empty() {
                    rl.add_history_entry(line.as_str()).ok();
                }

                crate::exec::execute_line(trimmed);
            }
            Err(ReadlineError::Interrupted) => {
                println!("^C");
                continue;
            }
            Err(ReadlineError::Eof) => {
                println!("logout");
                break;
            }
            Err(err) => {
                eprintln!("Error: {:?}", err);
                break;
            }
        }
    }

    if !history_file.is_empty() {
        let _ = rl.save_history(&history_file);
    }
}

pub fn run_noninteractive() {
    use std::io::{self, Read};

    let mut buffer = String::new();
    io::stdin().read_to_string(&mut buffer).ok();

    for line in buffer.lines() {
        if !line.trim().is_empty() {
            crate::exec::execute_line(line);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::shell::SHELL;
    use crate::TEST_LOCK;
    use rustyline::hint::Hinter;

    #[test]
    fn test_hinter_no_input() {
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let helper = MeowshHelper;
        let history = FileHistory::new();
        let ctx = rustyline::Context::new(&history);
        assert_eq!(helper.hint("", 0, &ctx), None);
        assert_eq!(helper.hint("   ", 3, &ctx), None);
    }

    #[test]
    fn test_hinter_mid_line() {
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let helper = MeowshHelper;
        let history = FileHistory::new();
        let ctx = rustyline::Context::new(&history);
        assert_eq!(helper.hint("echo hello", 4, &ctx), None);
    }

    #[test]
    fn test_hinter_match() {
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        {
            let shell = SHELL.shell.lock().unwrap_or_else(|e| e.into_inner());
            let mut history = shell.history.lock().unwrap_or_else(|e| e.into_inner());
            history.clear();
            history.push("ls -l".to_string());
            history.push("echo hello".to_string());
        }

        let helper = MeowshHelper;
        let history = FileHistory::new();
        let ctx = rustyline::Context::new(&history);
        
        // Case insensitive match and prefix match
        let hint = helper.hint("ec", 2, &ctx);
        assert!(hint.is_some());
        use rustyline::hint::Hint;
        assert!(hint.as_ref().unwrap().display().contains("ho hello"));
        assert_eq!(hint.as_ref().unwrap().completion(), Some("ho hello"));

        let hint = helper.hint("LS", 2, &ctx);
        assert!(hint.is_some());
        assert!(hint.as_ref().unwrap().display().contains(" -l"));
        assert_eq!(hint.as_ref().unwrap().completion(), Some(" -l"));
    }

    #[test]
    fn test_hinter_no_match() {
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        {
            let shell = SHELL.shell.lock().unwrap_or_else(|e| e.into_inner());
            let mut history = shell.history.lock().unwrap_or_else(|e| e.into_inner());
            history.clear();
            history.push("ls -l".to_string());
        }

        let helper = MeowshHelper;
        let history = FileHistory::new();
        let ctx = rustyline::Context::new(&history);
        assert_eq!(helper.hint("grep", 4, &ctx), None);
    }

    #[test]
    fn test_highlight_command() {
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let helper = MeowshHelper;
        
        // Known builtin
        let h = helper.highlight("echo hello", 0);
        assert!(h.contains("\x1b[32mecho\x1b[0m"));
        
        // Unknown command
        let h = helper.highlight("nonexistent_command", 0);
        assert!(h.contains("\x1b[31mnonexistent_command\x1b[0m"));
    }

    #[test]
    fn test_highlight_operators() {
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let helper = MeowshHelper;
        
        let h = helper.highlight("ls | grep foo && echo bar", 0);
        assert!(h.contains("\x1b[36m|\x1b[0m"));
        assert!(h.contains("\x1b[36m&&\x1b[0m"));
        // Second command 'grep' should be highlighted too
        assert!(h.contains("\x1b[32mgrep\x1b[0m") || h.contains("\x1b[31mgrep\x1b[0m"));
    }

    #[test]
    fn test_highlight_strings() {
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let helper = MeowshHelper;
        
        let h = helper.highlight("echo \"hello world\"", 0);
        assert!(h.contains("\x1b[33m\"hello world\"\x1b[0m"));
    }

    #[test]
    fn test_highlight_comments() {
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let helper = MeowshHelper;
        
        let h = helper.highlight("# this is a comment", 0);
        assert!(h.contains("\x1b[90m# this is a comment\x1b[0m"));
    }
}

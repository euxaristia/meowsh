use crate::shell::build_prompt;
use crate::strip_ansi;
use rustyline::completion::Completer;
use rustyline::config::Configurer;
use rustyline::error::ReadlineError;
use rustyline::highlight::{CmdKind, Highlighter};
use rustyline::hint::Hinter;
use rustyline::history::FileHistory;
use rustyline::validate::Validator;
use rustyline::{ColorMode, Editor, Helper};
use std::borrow::Cow;

pub struct MeowshHelper;

impl MeowshHelper {
    pub fn command_exists(&self, name: &str) -> bool {
        crate::shell::command_exists(name)
    }

    fn fuzzy_match(&self, query: &str, candidate: &str) -> bool {
        if query.is_empty() {
            return true;
        }
        let mut query_chars = query.chars().peekable();
        for c in candidate.chars() {
            if let Some(&q) = query_chars.peek() {
                if c.to_lowercase().next() == q.to_lowercase().next() {
                    query_chars.next();
                }
            } else {
                return true;
            }
        }
        query_chars.peek().is_none()
    }

    fn get_completions(&self, line: &str) -> Vec<rustyline::completion::Pair> {
        let mut completions = Vec::new();
        let shell = crate::shell::SHELL.shell.lock().unwrap();

        let parts: Vec<&str> = line.split_whitespace().collect();
        let is_cmd_position = parts.is_empty() || (parts.len() == 1 && !parts[0].contains('/'));

        if is_cmd_position {
            let current = parts.last().unwrap_or(&"");

            for name in shell.aliases.keys() {
                if self.fuzzy_match(current, name) {
                    completions.push(rustyline::completion::Pair {
                        display: name.clone(),
                        replacement: name.clone(),
                    });
                }
            }

            for name in shell.functions.keys() {
                if self.fuzzy_match(current, name) {
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
                "history",
            ];
            for b in builtins {
                if self.fuzzy_match(current, b) {
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
                            if self.fuzzy_match(current, &name) {
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

#[derive(Clone, Debug)]
enum CharOrAnsi {
    Char(char),
    Ansi(String),
}

fn get_char_or_ansi(s: &str) -> Vec<CharOrAnsi> {
    let mut out = Vec::new();
    let mut chars = s.char_indices().peekable();
    while let Some((_, c)) = chars.next() {
        if c == '\x1b' {
            let mut ansi = String::new();
            ansi.push(c);
            if let Some(&(_, next_c)) = chars.peek() {
                match next_c {
                    '[' => { // CSI
                        let (_, c2) = chars.next().unwrap();
                        ansi.push(c2);
                        while let Some((_, c)) = chars.next() {
                            ansi.push(c);
                            if ('@'..='~').contains(&c) {
                                break;
                            }
                        }
                    }
                    ']' => { // OSC
                        let (_, c2) = chars.next().unwrap();
                        ansi.push(c2);
                        while let Some((_, c)) = chars.next() {
                            ansi.push(c);
                            if c == '\x07' { // BEL
                                break;
                            }
                            if c == '\x1b' {
                                if let Some(&(_, next_next_c)) = chars.peek() {
                                    if next_next_c == '\\' { // ST
                                        let (_, c3) = chars.next().unwrap();
                                        ansi.push(c3);
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    '(' | ')' | '#' | '*' | '+' | '-' | '.' | '/' => { // G0/G1 charset or other 2-char ESC
                        let (_, c2) = chars.next().unwrap();
                        ansi.push(c2);
                        if let Some((_, c3)) = chars.next() {
                            ansi.push(c3);
                        }
                    }
                    _ => {
                        if let Some((_, c2)) = chars.next() {
                            ansi.push(c2);
                        }
                    }
                }
            }
            out.push(CharOrAnsi::Ansi(ansi));
        } else {
            out.push(CharOrAnsi::Char(c));
        }
    }
    out
}

impl Completer for MeowshHelper {
    type Candidate = rustyline::completion::Pair;

    fn complete(
        &self,
        line: &str,
        pos: usize,
        _ctx: &rustyline::Context<'_>,
    ) -> Result<(usize, Vec<rustyline::completion::Pair>), ReadlineError> {
        let (clean_line, mappings) = crate::get_clean_and_mappings(line);

        // Map raw pos to clean_pos
        let mut clean_pos = 0;
        for (i, &raw_idx) in mappings.iter().enumerate() {
            if raw_idx >= pos {
                clean_pos = i;
                break;
            }
        }

        // Find the start of the word being completed in the clean line
        let word_start_in_clean = clean_line[..clean_pos]
            .rfind(|c: char| c.is_whitespace() || ";|&".contains(c))
            .map(|i| i + 1)
            .unwrap_or(0);

        // Map word_start back to original line
        let start = mappings[word_start_in_clean];

        Ok((
            start,
            self.get_completions(&clean_line[..clean_pos.min(clean_line.len())]),
        ))
    }
}

impl Highlighter for MeowshHelper {
    fn highlight<'l>(&self, line: &'l str, _pos: usize) -> Cow<'l, str> {
        if line.is_empty() {
            return Cow::Borrowed(line);
        }

        let parts = get_char_or_ansi(line);
        let mut colored = String::with_capacity(line.len() * 2);
        let mut i = 0;
        let mut is_start_of_command = true;

        while i < parts.len() {
            match &parts[i] {
                CharOrAnsi::Ansi(ansi) => {
                    colored.push_str(ansi);
                    i += 1;
                }
                CharOrAnsi::Char(c) => match *c {
                    ' ' | '\t' => {
                        colored.push(*c);
                        i += 1;
                    }
                    '#' if is_start_of_command => {
                        colored.push_str("\x1b[90m");
                        while i < parts.len() {
                            match &parts[i] {
                                CharOrAnsi::Char(c) => colored.push(*c),
                                CharOrAnsi::Ansi(ansi) => colored.push_str(ansi),
                            }
                            i += 1;
                        }
                        colored.push_str("\x1b[0m");
                        break;
                    }
                    ';' | '|' | '&' => {
                        colored.push_str("\x1b[36m");
                        colored.push(*c);
                        i += 1;
                        if i < parts.len() {
                            if let CharOrAnsi::Char(next_c) = &parts[i] {
                                if *next_c == *c && (*c == '|' || *c == '&') {
                                    colored.push(*next_c);
                                    i += 1;
                                }
                            }
                        }
                        colored.push_str("\x1b[0m");
                        is_start_of_command = true;
                    }
                    '\'' | '"' => {
                        let quote = *c;
                        colored.push_str("\x1b[33m");
                        colored.push(quote);
                        i += 1;
                        while i < parts.len() {
                            match &parts[i] {
                                CharOrAnsi::Char(c) if *c == quote => break,
                                CharOrAnsi::Char(c) if *c == '\\' => {
                                    colored.push('\\');
                                    i += 1;
                                    if i < parts.len() {
                                        if let CharOrAnsi::Char(c2) = &parts[i] {
                                            colored.push(*c2);
                                            i += 1;
                                        }
                                    }
                                }
                                CharOrAnsi::Char(c) => {
                                    colored.push(*c);
                                    i += 1;
                                }
                                CharOrAnsi::Ansi(ansi) => {
                                    colored.push_str(ansi);
                                    i += 1;
                                }
                            }
                        }
                        if i < parts.len() {
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
                        while i < parts.len() {
                            if let CharOrAnsi::Char(c) = &parts[i] {
                                if c.is_alphanumeric() || *c == '_' {
                                    colored.push(*c);
                                    i += 1;
                                    continue;
                                }
                            }
                            break;
                        }
                        colored.push_str("\x1b[0m");
                        is_start_of_command = false;
                    }
                    _ => {
                        let mut word_parts = Vec::new();
                        let mut visible_word = String::new();
                        while i < parts.len() {
                            match &parts[i] {
                                CharOrAnsi::Char(c)
                                    if c.is_whitespace() || ";|&'\"$".contains(*c) =>
                                {
                                    break;
                                }
                                CharOrAnsi::Char(c) => {
                                    visible_word.push(*c);
                                    word_parts.push(&parts[i]);
                                }
                                CharOrAnsi::Ansi(_) => {
                                    word_parts.push(&parts[i]);
                                }
                            }
                            i += 1;
                        }

                        if is_start_of_command {
                            if self.command_exists(&visible_word) {
                                colored.push_str("\x1b[32m");
                            } else {
                                colored.push_str("\x1b[31m");
                            }
                            for p in word_parts {
                                match p {
                                    CharOrAnsi::Char(c) => colored.push(*c),
                                    CharOrAnsi::Ansi(ansi) => colored.push_str(ansi),
                                }
                            }
                            colored.push_str("\x1b[0m");
                            is_start_of_command = false;
                        } else {
                            for p in word_parts {
                                match p {
                                    CharOrAnsi::Char(c) => colored.push(*c),
                                    CharOrAnsi::Ansi(ansi) => colored.push_str(ansi),
                                }
                            }
                        }
                    }
                },
            }
        }

        Cow::Owned(colored)
    }

    fn highlight_hint<'h>(&self, hint: &'h str) -> Cow<'h, str> {
        Cow::Owned(format!("\x1b[90m{}\x1b[0m", hint))
    }

    fn highlight_char(&self, _line: &str, _pos: usize, _kind: CmdKind) -> bool {
        // Always re-render the full line so that command validity coloring
        // updates for the entire word as the user types each character.
        true
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

        let clean_line = strip_ansi(line);
        let trimmed_input = clean_line.trim_start();
        if trimmed_input.is_empty() {
            return None;
        }

        let shell = crate::shell::SHELL.shell.lock().unwrap();
        let history = shell.history.lock().unwrap();

        let lower_input = trimmed_input.to_lowercase();
        for hist in history.iter().rev() {
            let clean_hist = strip_ansi(hist);
            let lower_hist = clean_hist.to_lowercase();
            if lower_hist.starts_with(&lower_input) && clean_hist.chars().count() > trimmed_input.chars().count() {
                // Return what's left of the history entry after the input
                // We skip the number of characters in the input to get the hint suffix
                let char_count = trimmed_input.chars().count();
                let hint_text: String = clean_hist.chars().skip(char_count).collect();
                
                return Some(MeowshHint {
                    display: hint_text.clone(),
                    completion: hint_text, // hist is already clean
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
        if let Ok(content) = std::fs::read_to_string(&history_file) {
            let shell = crate::shell::SHELL.shell.lock().unwrap();
            let mut history = shell.history.lock().unwrap();
            history.clear();
            for line in content.lines() {
                let clean = strip_ansi(line);
                if !clean.is_empty() {
                    rl.add_history_entry(&clean).ok();
                    history.push(clean);
                }
            }
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
                    let clean_line = strip_ansi(&line);
                    rl.add_history_entry(clean_line.as_str()).ok();
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
        assert_eq!(hint.as_ref().unwrap().display(), "ho hello");
        assert_eq!(hint.as_ref().unwrap().completion(), Some("ho hello"));

        let hint = helper.hint("LS", 2, &ctx);
        assert!(hint.is_some());
        assert_eq!(hint.as_ref().unwrap().display(), " -l");
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

    #[test]
    fn test_fuzzy_match() {
        let helper = MeowshHelper;
        assert!(helper.fuzzy_match("msh", "meowsh"));
        assert!(helper.fuzzy_match("echo", "echo"));
        assert!(helper.fuzzy_match("eo", "echo"));
        assert!(helper.fuzzy_match("E", "echo"));
        assert!(!helper.fuzzy_match("abc", "echo"));
        assert!(helper.fuzzy_match("", "anything"));
    }

    #[test]
    fn test_fuzzy_completions() {
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let helper = MeowshHelper;
        
        // Test builtins fuzzy matching
        let comps = helper.get_completions("eo");
        assert!(comps.iter().any(|c| c.display == "echo"));
        
        let comps = helper.get_completions("rd");
        assert!(comps.iter().any(|c| c.display == "read"));
    }

    #[test]
    fn test_highlight_char_always_true() {
        let helper = MeowshHelper;
        // highlight_char must return true for all inputs so rustyline
        // re-renders the full line after every keystroke, preventing
        // stale red coloring on partial command words.
        assert!(helper.highlight_char("cargo", 0, CmdKind::MoveCursor));
        assert!(helper.highlight_char("cargo", 3, CmdKind::Other));
        assert!(helper.highlight_char("", 0, CmdKind::ForcedRefresh));
    }

    #[test]
    fn test_highlight_command_with_ansi() {
        let _lock = crate::TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let helper = MeowshHelper;

        // "ec\x1b[90mho" should be seen as "echo" which is a builtin (green)
        let h = helper.highlight("ec\x1b[90mho hello", 0);
        assert!(h.contains("\x1b[32m")); // Green start
        assert!(h.contains("ec\x1b[90mho")); // Original text preserved
        assert!(h.contains("\x1b[0m")); // Reset at end of word
    }

    #[test]
    fn test_complete_with_ansi() {
        let _lock = crate::TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let helper = MeowshHelper;
        let history = FileHistory::new();
        let ctx = rustyline::Context::new(&history);

        // "\x1b[31mec" -> complete should find "echo" and offset should be at "ec" (index 5)
        use rustyline::completion::Completer;
        let (offset, candidates) = helper.complete("\x1b[31mec", 7, &ctx).unwrap();
        assert_eq!(offset, 5);
        assert!(candidates.iter().any(|c| c.display == "echo"));
    }
}

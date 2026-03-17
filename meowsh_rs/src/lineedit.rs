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

impl Hinter for MeowshHelper {
    type Hint = String;

    fn hint(&self, line: &str, _pos: usize, _ctx: &rustyline::Context<'_>) -> Option<String> {
        let shell = crate::shell::SHELL.shell.lock().unwrap();
        let history = shell.history.lock().unwrap();

        for hist in history.iter().rev() {
            if hist.starts_with(line) && hist.len() > line.len() {
                return Some(hist[line.len()..].to_string());
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

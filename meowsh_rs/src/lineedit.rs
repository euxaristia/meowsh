use crate::shell::build_prompt;
use rustyline::{history::FileHistory, Editor};

pub fn run_interactive() {
    let mut rl: Editor<(), FileHistory> = Editor::new().unwrap();

    loop {
        let prompt = build_prompt();

        match rl.readline(&prompt) {
            Ok(line) => {
                if line.trim().is_empty() {
                    continue;
                }

                rl.add_history_entry(&line);

                crate::exec::execute_line(&line);
            }
            Err(rustyline::error::ReadlineError::Interrupted) => {
                println!("^C");
                continue;
            }
            Err(rustyline::error::ReadlineError::Eof) => {
                println!("logout");
                break;
            }
            Err(err) => {
                eprintln!("Error: {:?}", err);
                break;
            }
        }
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

use crate::shell::build_prompt;
use std::io::{self, Read, Write};

pub fn run_interactive() {
    let mut stdin = io::stdin();

    loop {
        let prompt = build_prompt();
        print!("{}", prompt);
        io::Write::flush(&mut io::stdout()).unwrap();

        let mut line = String::new();
        match stdin.read_line(&mut line) {
            Ok(0) => {
                println!("logout");
                break;
            }
            Ok(_) => {
                line = line.trim_end().to_string();
                if line.is_empty() {
                    continue;
                }

                crate::exec::execute_line(&line);
            }
            Err(err) => {
                eprintln!("Error: {:?}", err);
                break;
            }
        }
    }
}

pub fn run_noninteractive() {
    let mut buffer = String::new();
    io::stdin().read_to_string(&mut buffer).ok();

    for line in buffer.lines() {
        if !line.trim().is_empty() {
            crate::exec::execute_line(line);
        }
    }
}

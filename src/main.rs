use libc::{isatty, signal, SIGINT, SIGQUIT, SIGTERM, SIGTSTP, SIGTTIN, SIGTTOU, SIG_IGN};
use meowsh::exec::execute_line;
use meowsh::lineedit::{run_interactive, run_noninteractive};
use meowsh::shell::{shell_init, SHELL};
use meowsh::types::OPT_INTERACTIVE;
use std::env;

fn main() {
    shell_init();

    // Ignore terminal control signals (like the Go version)
    unsafe {
        signal(SIGINT, SIG_IGN);
        signal(SIGTERM, SIG_IGN);
        signal(SIGQUIT, SIG_IGN);
        signal(SIGTSTP, SIG_IGN);
        signal(SIGTTIN, SIG_IGN);
        signal(SIGTTOU, SIG_IGN);
    }

    // Parse arguments
    let args: Vec<String> = env::args().collect();
    let mut interactive = unsafe { isatty(0) != 0 };
    let mut script_to_run = None;

    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "-i" => {
                interactive = true;
            }
            "-c" => {
                if i + 1 < args.len() {
                    script_to_run = Some(args[i + 1].clone());
                }
                break;
            }
            _ => {
                // Positional arguments or unknown flags
                break;
            }
        }
        i += 1;
    }

    SHELL.shell.lock().unwrap().interactive = interactive;

    if interactive {
        SHELL.shell.lock().unwrap().opts |= OPT_INTERACTIVE;
    }

    if !args.is_empty() {
        SHELL.shell.lock().unwrap().argv0 = args[0].clone();
    }

    // Handle -c option
    if let Some(script) = script_to_run {
        execute_line(&script);
        return;
    }

    if SHELL.shell.lock().unwrap().interactive {
        run_interactive();
    } else {
        run_noninteractive();
    }
}

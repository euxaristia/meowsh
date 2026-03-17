use libc::isatty;
use meowsh_rs::exec::execute_line;
use meowsh_rs::lineedit::{run_interactive, run_noninteractive};
use meowsh_rs::shell::{shell_init, SHELL};
use meowsh_rs::types::OPT_INTERACTIVE;
use std::env;

fn main() {
    shell_init();

    // Check if interactive
    let interactive = unsafe { isatty(0) != 0 };
    unsafe { SHELL.shell.lock().unwrap().interactive = interactive };

    if interactive {
        unsafe { SHELL.shell.lock().unwrap().opts |= OPT_INTERACTIVE };
    }

    // Parse arguments
    let args: Vec<String> = env::args().collect();
    if !args.is_empty() {
        unsafe { SHELL.shell.lock().unwrap().argv0 = args[0].clone() };
    }

    // Handle -c option
    if args.len() > 1 && args[1] == "-c" {
        if args.len() > 2 {
            execute_line(&args[2]);
        }
        return;
    }

    if unsafe { SHELL.shell.lock().unwrap().interactive } {
        run_interactive();
    } else {
        run_noninteractive();
    }
}

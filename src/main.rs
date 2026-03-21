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

    let args: Vec<String> = env::args().collect();
    let (interactive_flag, script_to_run) = parse_args(&args);
    
    let interactive = interactive_flag.unwrap_or_else(|| unsafe { isatty(0) != 0 });

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

fn parse_args(args: &[String]) -> (Option<bool>, Option<String>) {
    let mut interactive = None;
    let mut script_to_run = None;

    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "-i" => {
                interactive = Some(true);
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
    (interactive, script_to_run)
}

#[cfg(test)]
mod tests {
    use super::*;
    use meowsh::TEST_LOCK;

    #[test]
    fn test_parse_args_interactive() {
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let args = vec!["meowsh".to_string(), "-i".to_string()];
        let (interactive, script) = parse_args(&args);
        assert_eq!(interactive, Some(true));
        assert_eq!(script, None);
    }

    #[test]
    fn test_parse_args_script() {
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let args = vec!["meowsh".to_string(), "-c".to_string(), "echo hello".to_string()];
        let (interactive, script) = parse_args(&args);
        assert_eq!(interactive, None);
        assert_eq!(script, Some("echo hello".to_string()));
    }

    #[test]
    fn test_parse_args_both() {
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let args = vec!["meowsh".to_string(), "-i".to_string(), "-c".to_string(), "ls".to_string()];
        let (interactive, script) = parse_args(&args);
        assert_eq!(interactive, Some(true));
        assert_eq!(script, Some("ls".to_string()));
    }

    #[test]
    fn test_parse_args_none() {
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let args = vec!["meowsh".to_string()];
        let (interactive, script) = parse_args(&args);
        assert_eq!(interactive, None);
        assert_eq!(script, None);
    }
}

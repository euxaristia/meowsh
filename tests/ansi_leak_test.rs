use meowsh::lineedit::MeowshHelper;
use meowsh::shell::SHELL;
use meowsh::TEST_LOCK;
use rustyline::hint::Hinter;
use rustyline::history::FileHistory;

#[test]
fn test_ansi_leak_in_hint() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    {
        let shell = SHELL.shell.lock().unwrap_or_else(|e| e.into_inner());
        let mut history = shell.history.lock().unwrap_or_else(|e| e.into_inner());
        history.clear();
        // Add a history entry with ANSI codes
        history.push("\x1b[31mcargo\x1b[0m run".to_string());
    }

    let helper = MeowshHelper;
    let history = FileHistory::new();
    let ctx = rustyline::Context::new(&history);
    
    // Type "\x1b[31mca" which should match the history entry
    let input = "\x1b[31mca";
    let hint = helper.hint(input, input.len(), &ctx);
    
    assert!(hint.is_some(), "Hint should be found even with ANSI in input if history has it");
    let h = hint.unwrap();
    
    use rustyline::hint::Hint;
    println!("Hint display: {:?}", h.display());
    println!("Hint completion: {:?}", h.completion());
    
    assert!(!h.completion().unwrap().contains("\x1b[0m"), "Hint completion should NOT contain ANSI codes");
}

#[test]
fn test_command_exists_path() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let helper = MeowshHelper;
    
    // Create a temporary directory and an executable file
    let temp = tempfile::tempdir().unwrap();
    let bin_path = temp.path().join("mycmd");
    {
        use std::io::Write;
        let mut f = std::fs::File::create(&bin_path).unwrap();
        f.write_all(b"#!/bin/sh\necho hi\n").unwrap();
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let mut perms = f.metadata().unwrap().permissions();
            perms.set_mode(0o755);
            f.set_permissions(perms).unwrap();
        }
    }

    {
        let mut shell = SHELL.shell.lock().unwrap_or_else(|e| e.into_inner());
        // Set PATH to include the temp dir
        shell.vars.insert("PATH".to_string(), meowsh::types::Var {
            value: temp.path().to_str().unwrap().to_string(),
            flags: meowsh::types::VAR_EXPORT,
        });
    }

    // It should exist now
    assert!(helper.command_exists("mycmd"), "Command should be found in PATH");

    // Test empty PATH entry (should mean current directory)
    {
        let mut shell = SHELL.shell.lock().unwrap_or_else(|e| e.into_inner());
        shell.vars.insert("PATH".to_string(), meowsh::types::Var {
            value: format!(":{}", temp.path().to_str().unwrap()),
            flags: meowsh::types::VAR_EXPORT,
        });
    }
    
    // Create 'mycmd2' in current directory
    let cwd_cmd = std::env::current_dir().unwrap().join("mycmd2");
    {
        use std::io::Write;
        let mut f = std::fs::File::create(&cwd_cmd).unwrap();
        f.write_all(b"#!/bin/sh\necho hi\n").unwrap();
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let mut perms = f.metadata().unwrap().permissions();
            perms.set_mode(0o755);
            f.set_permissions(perms).unwrap();
        }
    }
    
    let exists = helper.command_exists("mycmd2");
    // Cleanup first
    let _ = std::fs::remove_file(&cwd_cmd);
    
    assert!(exists, "Command should be found in current directory via empty PATH entry");
}

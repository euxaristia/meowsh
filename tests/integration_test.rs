use std::env;
use std::fs;
use std::io::{Read, Write};
use std::process::{Command, Stdio};
use tempfile::TempDir;

fn run_script(script: &str) -> (i32, String, String) {
    let child = Command::new(env!("CARGO_BIN_EXE_meowsh"))
        .arg("-c")
        .arg(script)
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .spawn()
        .expect("Failed to spawn meowsh");

    let output = child.wait_with_output().expect("Failed to read output");
    (
        output.status.code().unwrap_or(127),
        String::from_utf8_lossy(&output.stdout).to_string(),
        String::from_utf8_lossy(&output.stderr).to_string(),
    )
}

#[test]
fn test_native_pipes() {
    let (status, stdout, _) = run_script("echo 'hello world' | grep 'hello' | wc -w | tr -d ' '");
    assert_eq!(status, 0);
    assert_eq!(stdout.trim(), "2"); // 'hello world' has 2 words
}

#[test]
fn test_file_operators() {
    let temp_dir = TempDir::new().unwrap();
    let dir_path = temp_dir.path().join("mydir");
    let file_path = temp_dir.path().join("myfile.txt");

    fs::create_dir(&dir_path).unwrap();
    fs::File::create(&file_path).unwrap();

    let script = format!(
        r#"
        if [ -d "{}" ]; then echo "is_dir"; fi
        if [ -f "{}" ]; then echo "is_file"; fi
        if [ -e "{}" ]; then echo "exists"; fi
        if [ -s "{}" ]; then echo "not_empty"; else echo "empty"; fi
        "#,
        dir_path.display(),
        file_path.display(),
        file_path.display(),
        file_path.display()
    );

    let (status, stdout, _) = run_script(&script);
    assert_eq!(status, 0);
    assert!(stdout.contains("is_dir"));
    assert!(stdout.contains("is_file"));
    assert!(stdout.contains("exists"));
    assert!(stdout.contains("empty"));
}

#[test]
fn test_shell_option_errexit() {
    let script = r#"
        set -e
        echo "start"
        false
        echo "end"
    "#;
    let (status, stdout, _) = run_script(script);
    assert_ne!(status, 0);
    assert!(stdout.contains("start"));
    assert!(!stdout.contains("end"));
}

#[test]
fn test_shell_option_nounset() {
    let script = r#"
        set -u
        echo "value: $UNDEFINED_VAR"
        echo "end"
    "#;
    let (status, stdout, stderr) = run_script(script);
    assert_ne!(status, 0);
    assert!(!stdout.contains("end"));
    assert!(stderr.contains("unbound variable"));
}

#[test]
fn test_shell_option_xtrace() {
    let script = r#"
        set -x
        echo "trace this"
    "#;
    let (status, stdout, stderr) = run_script(script);
    assert_eq!(status, 0);
    assert!(stdout.contains("trace this"));
    assert!(stderr.contains("+ echo trace this"));
}

#[test]
fn test_here_documents() {
    let script = r#"
cat << EOF
line1
line2
EOF
"#;
    let (status, stdout, _) = run_script(script);
    assert_eq!(status, 0);
    assert!(stdout.contains("line1"));
    assert!(stdout.contains("line2"));
}

#[test]
fn test_parameter_expansion_remove_suffix() {
    let script = r#"
        set -f
        file="example.tar.gz"
        echo "${file%.*}"
        echo "${file%%.*}"
    "#;
    let (status, stdout, _) = run_script(script);
    assert_eq!(status, 0);
    let mut lines = stdout.lines();
    assert_eq!(lines.next().unwrap(), "example.tar");
    assert_eq!(lines.next().unwrap(), "example");
}

#[test]
fn test_parameter_expansion_remove_prefix() {
    let script = r#"
        set -f
        path="/usr/local/bin"
        echo "${path#*/}"
        echo "${path##*/}"
    "#;
    let (status, stdout, _) = run_script(script);
    assert_eq!(status, 0);
    let mut lines = stdout.lines();
    assert_eq!(lines.next().unwrap(), "usr/local/bin");
    assert_eq!(lines.next().unwrap(), "bin");
}

#[test]
fn test_history_builtin() {
    let mut child = Command::new(env!("CARGO_BIN_EXE_meowsh"))
        .stdin(std::process::Stdio::piped())
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .spawn()
        .expect("Failed to spawn meowsh");

    let mut stdin = child.stdin.take().expect("Failed to open stdin");
    writeln!(stdin, "echo first").unwrap();
    writeln!(stdin, "echo second").unwrap();
    writeln!(stdin, "history").unwrap();
    writeln!(stdin, "exit").unwrap();
    drop(stdin);

    let mut stdout = String::new();
    child.stdout.unwrap().read_to_string(&mut stdout).unwrap();

    assert!(stdout.contains("first"));
    assert!(stdout.contains("second"));
    assert!(stdout.contains("history"));
}

#[test]
fn test_recursive_globbing() {
    let mut child = Command::new(env!("CARGO_BIN_EXE_meowsh"))
        .stdin(std::process::Stdio::piped())
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .spawn()
        .expect("Failed to spawn meowsh");

    let mut stdin = child.stdin.take().expect("Failed to open stdin");
    writeln!(stdin, "echo src/**/*.rs").unwrap();
    writeln!(stdin, "exit").unwrap();
    drop(stdin);

    let mut stdout = String::new();
    child.stdout.unwrap().read_to_string(&mut stdout).unwrap();

    assert!(stdout.contains("src/main.rs"));
    assert!(stdout.contains("src/lib.rs"));
    assert!(stdout.contains("src/expand.rs"));
}

#[test]
fn test_brace_expansion() {
    let mut child = Command::new(env!("CARGO_BIN_EXE_meowsh"))
        .stdin(std::process::Stdio::piped())
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .spawn()
        .expect("Failed to spawn meowsh");

    let mut stdin = child.stdin.take().expect("Failed to open stdin");
    writeln!(stdin, "echo {{a,b,c}}").unwrap();
    writeln!(stdin, "echo file_{{1,2}}.txt").unwrap();
    writeln!(stdin, "exit").unwrap();
    drop(stdin);

    let mut stdout = String::new();
    child.stdout.unwrap().read_to_string(&mut stdout).unwrap();

    assert!(stdout.contains("a b c"));
    assert!(stdout.contains("file_1.txt file_2.txt"));
}

#[test]
fn test_brace_expansion_advanced() {
    let (status, stdout, _) = run_script("echo {a,b}_{1,2}");
    assert_eq!(status, 0);
    assert!(stdout.contains("a_1 a_2 b_1 b_2"));

    let (status, stdout, _) = run_script("echo {a,{b,c}}");
    assert_eq!(status, 0);
    assert!(stdout.contains("a b c"));

    let (status, stdout, _) = run_script("echo {a}");
    assert_eq!(status, 0);
    assert!(stdout.trim() == "{a}");
}

#[test]
fn test_glob_no_match() {
    let (status, stdout, _) = run_script("echo non_existent_file_*.txt");
    assert_eq!(status, 0);
    assert_eq!(stdout.trim(), "non_existent_file_*.txt");
}

#[test]
fn test_history_limit() {
    let mut script = String::new();
    for i in 0..2000 {
        script.push_str(&format!("echo cmd{}\n", i));
    }
    script.push_str("history\n");
    
    let (status, stdout, _) = run_script(&script);
    assert_eq!(status, 0);
    // cmd0 should have been rotated out as we have 2000 commands and limit is 1000
    assert!(!stdout.contains("cmd0 "));
    // cmd1999 should be there
    assert!(stdout.contains("cmd1999"));
}

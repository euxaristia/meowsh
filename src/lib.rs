pub mod types;
pub mod lexer;
pub mod parser;
pub mod shell;
pub mod expand;
pub mod exec;
pub mod jobs;
pub mod lineedit;

pub use types::*;
pub use lexer::*;
pub use parser::*;
pub use shell::*;
pub use expand::*;
pub use exec::*;
pub use jobs::*;
pub use lineedit::*;

pub fn get_clean_and_mappings(s: &str) -> (String, Vec<usize>) {
    let mut out = String::with_capacity(s.len());
    let mut mappings = Vec::new();
    let mut chars = s.char_indices().peekable();
    while let Some((idx, c)) = chars.next() {
        if c == '\x1b' {
            if let Some(&(_, next_c)) = chars.peek() {
                match next_c {
                    '[' => { // CSI
                        chars.next();
                        while let Some((_, c)) = chars.next() {
                            if ('@'..='~').contains(&c) {
                                break;
                            }
                        }
                    }
                    ']' => { // OSC
                        chars.next();
                        while let Some((_, c)) = chars.next() {
                            if c == '\x07' { // BEL
                                break;
                            }
                            if c == '\x1b' {
                                if let Some(&(_, next_next_c)) = chars.peek() {
                                    if next_next_c == '\\' { // ST
                                        chars.next();
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    '(' | ')' | '#' | '*' | '+' | '-' | '.' | '/' => { // G0/G1 charset or other 2-char ESC
                        chars.next(); // Consume the qualifier
                        chars.next(); // Consume the character
                    }
                    _ => {
                        // Other ESC sequences are typically 2 characters total (ESC + something)
                        chars.next();
                    }
                }
            }
        } else {
            mappings.push(idx);
            out.push(c);
        }
    }
    mappings.push(s.len());
    (out, mappings)
}

pub fn strip_ansi(s: &str) -> String {
    get_clean_and_mappings(s).0
}

pub static TEST_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_strip_ansi() {
        assert_eq!(strip_ansi("\x1b[31mhello\x1b[0m"), "hello");
        assert_eq!(strip_ansi("plain text"), "plain text");
        assert_eq!(strip_ansi("\x1b[1;32mgreen bold\x1b[0m and \x1b[90mgrey\x1b[0m"), "green bold and grey");
        // Test OSC
        assert_eq!(strip_ansi("\x1b]0;terminal title\x07hello"), "hello");
        assert_eq!(strip_ansi("\x1b]0;title\x1b\\world"), "world");
        // Test other ESC
        assert_eq!(strip_ansi("\x1b(Bcharset"), "charset");
        assert_eq!(strip_ansi("\x1bAcursor up"), "cursor up");
    }
}

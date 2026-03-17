use crate::types::{Token, TokenType};

fn is_operator(c: char) -> bool {
    matches!(c, '|' | '&' | ';' | '<' | '>' | '(' | ')')
}

fn is_assignment(s: &str) -> bool {
    if s.is_empty() {
        return false;
    }
    for (i, c) in s.chars().enumerate() {
        if c == '=' {
            return i > 0;
        }
        if !c.is_alphabetic() && !c.is_ascii_digit() && c != '_' {
            return false;
        }
    }
    false
}

pub struct Lexer {
    input: String,
    pos: usize,
}

impl Clone for Lexer {
    fn clone(&self) -> Self {
        Lexer {
            input: self.input.clone(),
            pos: self.pos,
        }
    }
}

impl Lexer {
    pub fn new(input: &str) -> Self {
        Lexer {
            input: input.to_string(),
            pos: 0,
        }
    }

    pub fn peek_token(&mut self) -> Token {
        let mut clone = self.clone();
        clone.next_token()
    }

    pub fn next_token(&mut self) -> Token {
        self.skip_blanks();

        if self.pos >= self.input.len() {
            return Token {
                token_type: TokenType::Eof,
                value: String::new(),
            };
        }

        let c = self.input[self.pos..].chars().next().unwrap();

        if c == '\\'
            && self.pos + 1 < self.input.len()
            && self.input[self.pos + 1..].starts_with('\n')
        {
            self.pos += 2;
            return self.next_token();
        }

        if c == '#' && (self.pos == 0 || self.input[self.pos - 1..].starts_with('\n')) {
            while self.pos < self.input.len() && !self.input[self.pos..].starts_with('\n')
            {
                self.pos += 1;
            }
            return self.next_token();
        }

        if c == '\n' {
            self.pos += 1;
            return Token {
                token_type: TokenType::Newline,
                value: "\n".to_string(),
            };
        }

        if c.is_ascii_digit() && self.pos + 1 < self.input.len() {
            let next_char = self.input[self.pos + 1..].chars().next().unwrap_or(' ');
            if next_char == '<' || next_char == '>' {
                return self.read_io_number();
            }
        }

        if is_operator(c) {
            return self.read_operator();
        }

        self.read_word()
    }

    fn skip_blanks(&mut self) {
        while self.pos < self.input.len() {
            let c = self.input[self.pos..].chars().next().unwrap();
            if c == ' ' || c == '\t' {
                self.pos += 1;
            } else {
                break;
            }
        }
    }

    fn read_io_number(&mut self) -> Token {
        let start = self.pos;
        while self.pos < self.input.len() {
            let c = self.input[self.pos..].chars().next().unwrap();
            if c.is_ascii_digit() {
                self.pos += 1;
            } else {
                break;
            }
        }

        if self.pos < self.input.len() {
            let c = self.input[self.pos..].chars().next().unwrap();
            if c == '<' || c == '>' {
                return Token {
                    token_type: TokenType::IoNumber,
                    value: self.input[start..self.pos].to_string(),
                };
            }
        }

        self.pos = start;
        self.read_word()
    }

    fn read_operator(&mut self) -> Token {
        let c = self.input[self.pos..].chars().next().unwrap();
        self.pos += 1;

        match c {
            '|' => {
                if self.pos < self.input.len() && self.input[self.pos..].starts_with('|') {
                    self.pos += 1;
                    return Token {
                        token_type: TokenType::OrIf,
                        value: "||".to_string(),
                    };
                }
                Token {
                    token_type: TokenType::Pipe,
                    value: "|".to_string(),
                }
            }
            '&' => {
                if self.pos < self.input.len() && self.input[self.pos..].starts_with('&') {
                    self.pos += 1;
                    return Token {
                        token_type: TokenType::AndIf,
                        value: "&&".to_string(),
                    };
                }
                Token {
                    token_type: TokenType::Ampersand,
                    value: "&".to_string(),
                }
            }
            ';' => {
                if self.pos < self.input.len() && self.input[self.pos..].starts_with(';') {
                    self.pos += 1;
                    return Token {
                        token_type: TokenType::Dsemi,
                        value: ";;".to_string(),
                    };
                }
                Token {
                    token_type: TokenType::Semi,
                    value: ";".to_string(),
                }
            }
            '<' => {
                if self.pos < self.input.len() {
                    let next = self.input[self.pos..].chars().next().unwrap();
                    if next == '<' {
                        self.pos += 1;
                        if self.pos < self.input.len() && self.input[self.pos..].starts_with('-') {
                            self.pos += 1;
                            return Token {
                                token_type: TokenType::DlessDash,
                                value: "<<-".to_string(),
                            };
                        }
                        return Token {
                            token_type: TokenType::Dless,
                            value: "<<".to_string(),
                        };
                    }
                    if next == '&' {
                        self.pos += 1;
                        return Token {
                            token_type: TokenType::LessAnd,
                            value: "<&".to_string(),
                        };
                    }
                    if next == '>' {
                        self.pos += 1;
                        return Token {
                            token_type: TokenType::LessGreat,
                            value: "<>".to_string(),
                        };
                    }
                }
                Token {
                    token_type: TokenType::Less,
                    value: "<".to_string(),
                }
            }
            '>' => {
                if self.pos < self.input.len() {
                    let next = self.input[self.pos..].chars().next().unwrap();
                    if next == '>' {
                        self.pos += 1;
                        return Token {
                            token_type: TokenType::Dgreat,
                            value: ">>".to_string(),
                        };
                    }
                    if next == '&' {
                        self.pos += 1;
                        return Token {
                            token_type: TokenType::GreatAnd,
                            value: ">&".to_string(),
                        };
                    }
                    if next == '|' {
                        self.pos += 1;
                        return Token {
                            token_type: TokenType::Clobber,
                            value: ">|".to_string(),
                        };
                    }
                }
                Token {
                    token_type: TokenType::Great,
                    value: ">".to_string(),
                }
            }
            '(' => Token {
                token_type: TokenType::Lparen,
                value: "(".to_string(),
            },
            ')' => Token {
                token_type: TokenType::Rparen,
                value: ")".to_string(),
            },
            _ => Token {
                token_type: TokenType::Word,
                value: c.to_string(),
            },
        }
    }

    fn read_word(&mut self) -> Token {
        let start = self.pos;

        while self.pos < self.input.len() {
            let c = self.input[self.pos..].chars().next().unwrap();

            if c == '\\' {
                self.pos += 1;
                if self.pos < self.input.len() {
                    self.pos += 1;
                }
                continue;
            }

            if c == '\'' {
                self.pos += 1;
                while self.pos < self.input.len()
                    && !self.input[self.pos..].starts_with('\'')
                {
                    self.pos += 1;
                }
                if self.pos < self.input.len() {
                    self.pos += 1;
                }
                continue;
            }

            if c == '"' {
                self.pos += 1;
                while self.pos < self.input.len() {
                    let nc = self.input[self.pos..].chars().next().unwrap();
                    if nc == '\\' {
                        self.pos += 1;
                        if self.pos < self.input.len() {
                            self.pos += 1;
                        }
                        continue;
                    }
                    if nc == '"' {
                        self.pos += 1;
                        break;
                    }
                    self.pos += 1;
                }
                continue;
            }

            if c == '`' {
                self.pos += 1;
                while self.pos < self.input.len()
                    && !self.input[self.pos..].starts_with('`')
                {
                    self.pos += 1;
                }
                if self.pos < self.input.len() {
                    self.pos += 1;
                }
                continue;
            }

            if c == '$' {
                self.pos += 1;
                if self.pos < self.input.len() {
                    let nc = self.input[self.pos..].chars().next().unwrap();
                    if nc == '(' {
                        self.pos += 1;
                        if self.pos < self.input.len()
                            && self.input[self.pos..].starts_with('(')
                        {
                            self.pos += 1;
                            let mut depth = 1;
                            while self.pos < self.input.len() && depth > 0 {
                                let ch = self.input[self.pos..].chars().next().unwrap();
                                self.pos += 1;
                                if ch == '(' {
                                    if self.pos < self.input.len()
                                        && self.input[self.pos..].starts_with('(')
                                    {
                                        self.pos += 1;
                                        depth += 1;
                                    }
                                } else if ch == ')'
                                    && self.pos < self.input.len()
                                        && self.input[self.pos..].starts_with(')')
                                    {
                                        self.pos += 1;
                                        depth -= 1;
                                    }
                            }
                            continue;
                        }

                        let mut depth = 1;
                        while self.pos < self.input.len() && depth > 0 {
                            let ch = self.input[self.pos..].chars().next().unwrap();
                            self.pos += 1;
                            if ch == '(' {
                                depth += 1;
                            } else if ch == ')' {
                                depth -= 1;
                            }
                        }
                        continue;
                    }
                    if nc == '{' {
                        while self.pos < self.input.len()
                            && !self.input[self.pos..].starts_with('}')
                        {
                            self.pos += 1;
                        }
                        if self.pos < self.input.len() {
                            self.pos += 1;
                        }
                        continue;
                    }
                }
                continue;
            }

            if c.is_whitespace() || is_operator(c) {
                break;
            }

            self.pos += 1;
        }

        let value = self.input[start..self.pos].to_string();
        let token_type = if is_assignment(&value) {
            TokenType::Assignment
        } else {
            TokenType::Word
        };

        Token { token_type, value }
    }
}

pub fn tokenize(line: &str) -> Vec<Token> {
    let mut lexer = Lexer::new(line);
    let mut tokens = Vec::new();
    loop {
        let tok = lexer.next_token();
        if tok.token_type == TokenType::Eof {
            break;
        }
        tokens.push(tok);
    }
    tokens
}

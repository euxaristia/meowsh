use crate::types::Token;
use crate::types::TokenType;

pub struct Lexer {
    input: String,
    pos: usize,
}

impl Lexer {
    pub fn new(input: &str) -> Self {
        Lexer {
            input: input.to_string(),
            pos: 0,
        }
    }

    pub fn next_token(&mut self) -> Token {
        // Skip whitespace
        while self.pos < self.input.len() && self.input[self.pos..].starts_with(' ') {
            self.pos += 1;
        }

        if self.pos >= self.input.len() {
            return Token {
                token_type: TokenType::Eof,
                value: String::new(),
            };
        }

        // Check for newline
        if self.input[self.pos..].starts_with('\n') {
            self.pos += 1;
            return Token {
                token_type: TokenType::Newline,
                value: "\n".to_string(),
            };
        }

        // Read word (everything until whitespace)
        let start = self.pos;
        while self.pos < self.input.len()
            && !self.input[self.pos..].starts_with(' ')
            && !self.input[self.pos..].starts_with('\n')
        {
            self.pos += 1;
        }

        Token {
            token_type: TokenType::Word,
            value: self.input[start..self.pos].to_string(),
        }
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

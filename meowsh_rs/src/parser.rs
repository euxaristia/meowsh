use crate::lexer::Lexer;
use crate::types::{ASTNode, CaseItem, Redir, Token, TokenType};

pub struct Parser<'a> {
    lexer: &'a mut Lexer,
}

impl<'a> Parser<'a> {
    pub fn new(lexer: &'a mut Lexer) -> Self {
        Parser { lexer }
    }

    pub fn parse(&mut self) -> Option<ASTNode> {
        self.parse_command_list(true).0
    }

    fn parse_command_list(&mut self, top_level: bool) -> (Option<ASTNode>, bool) {
        let mut nodes = Vec::new();

        loop {
            let tok = self.peek_token();
            if tok.token_type == TokenType::Eof
                || tok.token_type == TokenType::Rparen
                || tok.token_type == TokenType::Rbrace
            {
                break;
            }
            if Self::is_reserved_end(&tok.value) {
                break;
            }

            if let Some(node) = self.parse_command() {
                nodes.push(node);
            }

            let tok = self.peek_token();
            match tok.token_type {
                TokenType::Semi => {
                    self.lexer.next_token();
                }
                TokenType::Newline => {
                    self.lexer.next_token();
                    if top_level {
                        break;
                    }
                }
                TokenType::Ampersand => {
                    self.lexer.next_token();
                    if let Some(ref mut node) = nodes.last_mut() {
                        node.conn = "&".to_string();
                    }
                }
                _ => break,
            }
        }

        if nodes.is_empty() {
            return (None, false);
        }
        if nodes.len() == 1 {
            return (Some(nodes.remove(0)), true);
        }

        let pipes = nodes;
        let mut node = ASTNode::new("list");
        node.pipes = pipes;
        (Some(node), true)
    }

    fn is_reserved_end(value: &str) -> bool {
        matches!(
            value,
            "done" | "fi" | "esac" | "else" | "elif" | "then" | "do"
        )
    }

    fn peek_token(&mut self) -> Token {
        // Simplified - we'd need position saving for proper implementation
        self.lexer.next_token()
    }

    fn parse_command(&mut self) -> Option<ASTNode> {
        let tok = self.peek_token();

        if tok.token_type == TokenType::Eof
            || tok.token_type == TokenType::Newline
            || tok.token_type == TokenType::Semi
        {
            return None;
        }

        match tok.value.as_str() {
            "if" => Some(self.parse_if()),
            "while" | "until" => Some(self.parse_while()),
            "for" => Some(self.parse_for()),
            "case" => Some(self.parse_case()),
            "{" => {
                self.lexer.next_token();
                let (body, _) = self.parse_command_list(false);
                self.expect_token_type(TokenType::Rbrace);
                let mut node = ASTNode::new("brace_group");
                node.body = body.map(Box::new);
                Some(node)
            }
            "(" => {
                self.lexer.next_token();
                let (body, _) = self.parse_command_list(false);
                self.expect_token_type(TokenType::Rparen);
                let mut node = ASTNode::new("subshell");
                node.body = body.map(Box::new);
                Some(node)
            }
            "!" => {
                self.lexer.next_token();
                let mut node = self.parse_command()?;
                node.bang = true;
                Some(node)
            }
            "function" => self.parse_function(),
            _ => self.parse_simple_command(),
        }
    }

    fn parse_simple_command(&mut self) -> Option<ASTNode> {
        let mut node = ASTNode::new("simple");

        loop {
            let tok = self.peek_token();
            if Self::is_terminator(&tok) {
                break;
            }

            let mut fd = -1;
            if tok.token_type == TokenType::IoNumber {
                let fd_str = self.lexer.next_token().value;
                fd = fd_str.parse().unwrap_or(-1);
            }

            if Self::is_redirection(&tok) {
                let op = self.lexer.next_token().value;
                if fd == -1 {
                    if matches!(op.as_str(), "<" | "<<" | "<<-" | "<&" | "<>") {
                        fd = 0;
                    } else {
                        fd = 1;
                    }
                }
                let file_tok = self.lexer.next_token();
                let redir = Redir {
                    op,
                    file: file_tok.value,
                    fd,
                    heredoc_body: String::new(),
                };
                node.redirs.push(redir);
                continue;
            }

            if tok.token_type == TokenType::Assignment {
                if node.args.is_empty() {
                    if node.assigns.is_empty() {
                        node.assigns = std::collections::HashMap::new();
                    }
                    let parts: Vec<&str> = tok.value.splitn(2, '=').collect();
                    if parts.len() == 2 {
                        node.assigns
                            .insert(parts[0].to_string(), parts[1].to_string());
                    }
                    self.lexer.next_token();
                    continue;
                } else {
                    node.args.push(tok.value.clone());
                    self.lexer.next_token();
                    continue;
                }
            }

            if tok.token_type == TokenType::Word {
                node.args.push(tok.value.clone());
                self.lexer.next_token();
                continue;
            }

            break;
        }

        let tok = self.peek_token();
        if tok.token_type == TokenType::Pipe {
            self.lexer.next_token();
            if let Some(right) = self.parse_command() {
                let left = node;
                let mut pipe_node = ASTNode::new("pipeline");
                pipe_node.pipes = vec![left, right];
                return Some(pipe_node);
            }
        }
        if tok.token_type == TokenType::AndIf {
            self.lexer.next_token();
            if let Some(right) = self.parse_command() {
                let mut and_or = ASTNode::new("and_or");
                and_or.left = Some(Box::new(node));
                and_or.right = Some(Box::new(right));
                and_or.conn = "&&".to_string();
                return Some(and_or);
            }
        }
        if tok.token_type == TokenType::OrIf {
            self.lexer.next_token();
            if let Some(right) = self.parse_command() {
                let mut and_or = ASTNode::new("and_or");
                and_or.left = Some(Box::new(node));
                and_or.right = Some(Box::new(right));
                and_or.conn = "||".to_string();
                return Some(and_or);
            }
        }

        Some(node)
    }

    fn is_terminator(tok: &Token) -> bool {
        matches!(
            tok.token_type,
            TokenType::Eof
                | TokenType::Semi
                | TokenType::Newline
                | TokenType::Ampersand
                | TokenType::Pipe
                | TokenType::AndIf
                | TokenType::OrIf
                | TokenType::Rparen
                | TokenType::Rbrace
        )
    }

    fn is_redirection(tok: &Token) -> bool {
        matches!(
            tok.token_type,
            TokenType::Less
                | TokenType::Great
                | TokenType::Dless
                | TokenType::Dgreat
                | TokenType::LessAnd
                | TokenType::GreatAnd
                | TokenType::LessGreat
                | TokenType::Clobber
                | TokenType::DlessDash
        )
    }

    fn parse_if(&mut self) -> ASTNode {
        self.lexer.next_token(); // if
        let (cond, _) = self.parse_command_list(false);

        self.expect_value("then");
        let (body, _) = self.parse_command_list(false);

        let else_node = {
            let tok = self.peek_token();
            match tok.value.as_str() {
                "else" => {
                    self.lexer.next_token();
                    self.parse_command_list(false).0
                }
                "elif" => Some(Box::new(self.parse_if())),
                _ => None,
            }
        };

        self.expect_value("fi");

        let mut node = ASTNode::new("if");
        node.cond = cond.map(Box::new);
        node.body = body.map(Box::new);
        node.else_body = else_node;
        node
    }

    fn parse_while(&mut self) -> ASTNode {
        let keyword = self.lexer.next_token().value;
        let (cond, _) = self.parse_command_list(false);
        self.expect_value("do");
        let (body, _) = self.parse_command_list(false);
        self.expect_value("done");

        let mut node = ASTNode::new(&keyword);
        node.cond = cond.map(Box::new);
        node.body = body.map(Box::new);
        node
    }

    fn parse_for(&mut self) -> ASTNode {
        self.lexer.next_token(); // for
        let var_name = self.lexer.next_token().value;

        self.expect_value("in");

        let mut words = Vec::new();
        loop {
            let tok = self.peek_token();
            if tok.value == "do"
                || tok.token_type == TokenType::Semi
                || tok.token_type == TokenType::Newline
            {
                break;
            }
            words.push(self.lexer.next_token().value);
        }

        if self.peek_token().token_type == TokenType::Semi {
            self.lexer.next_token();
        }

        self.expect_value("do");
        let (body, _) = self.parse_command_list(false);
        self.expect_value("done");

        let mut node = ASTNode::new("for");
        node.loop_var = var_name;
        node.loop_words = words;
        node.body = body.map(Box::new);
        node
    }

    fn parse_case(&mut self) -> ASTNode {
        self.lexer.next_token(); // case
        let word = self.lexer.next_token().value;

        self.skip_newlines();
        self.expect_value("in");
        self.skip_newlines();

        let mut node = ASTNode::new("case");
        node.value = word;
        node.cases = Vec::new();

        loop {
            let tok = self.peek_token();
            if tok.value == "esac" || tok.token_type == TokenType::Eof {
                break;
            }

            if tok.token_type == TokenType::Newline {
                self.lexer.next_token();
                continue;
            }

            if tok.value == "(" {
                self.lexer.next_token();
            }

            let pattern = self.lexer.next_token().value;
            self.expect_value(")");

            let (body, _) = self.parse_command_list(false);
            node.cases.push(CaseItem {
                pattern,
                body: body.map(Box::new),
            });

            let tok = self.peek_token();
            if tok.value == ";;" {
                self.lexer.next_token();
            }
            self.skip_newlines();
        }

        self.expect_value("esac");
        node
    }

    fn parse_function(&mut self) -> Option<ASTNode> {
        self.lexer.next_token(); // function
        let name = self.lexer.next_token().value;

        let body = self.parse_command();

        let mut node = ASTNode::new("function");
        node.func_name = name;
        node.func_body = body.map(Box::new);
        Some(node)
    }

    fn skip_newlines(&mut self) {
        loop {
            let tok = self.peek_token();
            if tok.token_type != TokenType::Newline {
                break;
            }
            self.lexer.next_token();
        }
    }

    fn expect_token_type(&mut self, _token_type: TokenType) {
        self.lexer.next_token();
    }

    fn expect_value(&mut self, val: &str) {
        let tok = self.lexer.next_token();
        if tok.value != val {
            // Error handling
        }
    }
}

pub fn parse(line: &str) -> Option<ASTNode> {
    let mut lexer = Lexer::new(line);
    let mut parser = Parser::new(&mut lexer);
    parser.parse()
}

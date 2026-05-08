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

        // Skip blank lines at the start of a top-level parse so they don't
        // make `parse()` return None and falsely terminate the script.
        if top_level {
            while self.peek_token().token_type == TokenType::Newline {
                self.lexer.next_token();
            }
        }

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
        // The lexer doesn't emit a dedicated Rbrace token for `}` — it
        // comes back as a Word — so we treat the literal value as a
        // command-list terminator alongside the other reserved words.
        matches!(
            value,
            "done" | "fi" | "esac" | "else" | "elif" | "then" | "do" | "}"
        )
    }

    fn peek_token(&mut self) -> Token {
        self.lexer.peek_token()
    }

    fn parse_command(&mut self) -> Option<ASTNode> {
        let tok = self.peek_token();

        if tok.token_type == TokenType::Eof
            || tok.token_type == TokenType::Newline
            || tok.token_type == TokenType::Semi
        {
            return None;
        }

        // POSIX function definition: name ( )  followed by compound command.
        if tok.token_type == TokenType::Word && self.is_posix_func_def() {
            return self.parse_posix_function();
        }

        if tok.value == "[[" {
            return Some(self.parse_dbracket());
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

    fn is_posix_func_def(&mut self) -> bool {
        let mut clone = self.lexer.clone();
        let name = clone.next_token();
        if name.token_type != TokenType::Word {
            return false;
        }
        // Reserved words and the "function" keyword don't form POSIX defs.
        if matches!(
            name.value.as_str(),
            "if" | "while"
                | "until"
                | "for"
                | "case"
                | "function"
                | "then"
                | "else"
                | "elif"
                | "fi"
                | "do"
                | "done"
                | "esac"
                | "in"
                | "{"
                | "}"
                | "!"
        ) {
            return false;
        }
        let lparen = clone.next_token();
        if lparen.token_type != TokenType::Lparen {
            return false;
        }
        let rparen = clone.next_token();
        rparen.token_type == TokenType::Rparen
    }

    fn parse_posix_function(&mut self) -> Option<ASTNode> {
        let name = self.lexer.next_token().value;
        self.lexer.next_token(); // (
        self.lexer.next_token(); // )
        self.skip_newlines();
        let body = self.parse_command();
        let mut node = ASTNode::new("function");
        node.func_name = name;
        node.func_body = body.map(Box::new);
        Some(node)
    }

    // [[ expr ]] conditional. The lexer hands us each inner token as-is
    // (including `&&`, `||`, `(`, `)`, and operator/operand words). We
    // collect raw values until `]]` and let the executor evaluate.
    fn parse_dbracket(&mut self) -> ASTNode {
        self.lexer.next_token(); // [[
        let mut args: Vec<String> = Vec::new();
        loop {
            let tok = self.lexer.next_token();
            if tok.token_type == TokenType::Eof {
                eprintln!("meowsh: syntax error: unexpected end of file in [[ ... ]]");
                break;
            }
            if tok.value == "]]" {
                break;
            }
            if tok.token_type == TokenType::Newline {
                continue;
            }
            args.push(tok.value);
        }
        let mut node = ASTNode::new("dbracket");
        node.args = args;
        node
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
                let mut redir = Redir {
                    op: op.clone(),
                    file: file_tok.value.clone(),
                    fd,
                    heredoc_body: String::new(),
                };

                if op == "<<" || op == "<<-" {
                    redir.heredoc_body = self.lexer.read_heredoc(&file_tok.value, op == "<<-");
                }

                node.redirs.push(redir);
                continue;
            }

            if tok.token_type == TokenType::Assignment {
                if node.args.is_empty() {
                    let raw = tok.value.clone();
                    self.lexer.next_token();
                    let (name, value, is_append) = split_assignment(&raw);

                    // Array literal / append: `name=(...)` or `name+=(...)`.
                    if value.is_empty()
                        && self.peek_token().token_type == TokenType::Lparen
                    {
                        self.lexer.next_token(); // (
                        let mut items: Vec<String> = Vec::new();
                        loop {
                            let t = self.peek_token();
                            if t.token_type == TokenType::Rparen
                                || t.token_type == TokenType::Eof
                            {
                                break;
                            }
                            if t.token_type == TokenType::Newline
                                || t.token_type == TokenType::Semi
                            {
                                self.lexer.next_token();
                                continue;
                            }
                            items.push(self.lexer.next_token().value);
                        }
                        if self.peek_token().token_type == TokenType::Rparen {
                            self.lexer.next_token();
                        }
                        if is_append {
                            node.array_appends.insert(name, items);
                        } else {
                            node.array_assigns.insert(name, items);
                        }
                        continue;
                    }

                    if is_append {
                        node.scalar_appends.insert(name, value);
                    } else {
                        node.assigns.insert(name, value);
                    }
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
                    self.parse_command_list(false).0.map(Box::new)
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

// Splits an Assignment-token value `name=val` or `name+=val` into
// (name, val, is_append).
fn split_assignment(raw: &str) -> (String, String, bool) {
    if let Some(idx) = raw.find("+=") {
        return (raw[..idx].to_string(), raw[idx + 2..].to_string(), true);
    }
    if let Some(idx) = raw.find('=') {
        return (raw[..idx].to_string(), raw[idx + 1..].to_string(), false);
    }
    (raw.to_string(), String::new(), false)
}

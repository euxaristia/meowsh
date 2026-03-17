use crate::lexer::Lexer;
use crate::types::ASTNode;

pub struct Parser<'a> {
    lexer: &'a mut Lexer,
}

impl<'a> Parser<'a> {
    pub fn new(lexer: &'a mut Lexer) -> Self {
        Parser { lexer }
    }

    pub fn parse(&mut self) -> Option<ASTNode> {
        // Simplified parser - just return a node with the command
        let tok = self.lexer.next_token();

        if tok.value.is_empty() {
            return None;
        }

        Some(ASTNode {
            node_type: "simple".to_string(),
            args: vec![tok.value],
            assigns: std::collections::HashMap::new(),
        })
    }
}

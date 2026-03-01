package main

import "fmt"

type Parser struct {
	lexer *Lexer
}

func NewParser(lexer *Lexer) *Parser {
	return &Parser{lexer: lexer}
}

func (p *Parser) Parse() *ASTNode {
	node, _ := p.parseCommandList(true)
	return node
}

func (p *Parser) parseCommandList(topLevel bool) (*ASTNode, bool) {
	var nodes []*ASTNode
	for {
		tok := p.lexer.PeekToken()
		if tok.Type == TOK_EOF || tok.Type == TOK_RPAREN || tok.Type == TOK_RBRACE {
			break
		}
		// Reserved words that end command lists
		if tok.Value == "done" || tok.Value == "fi" || tok.Value == "esac" || tok.Value == "else" || tok.Value == "elif" || tok.Value == "then" || tok.Value == "do" {
			break
		}

		node := p.parseCommand()
		if node != nil {
			nodes = append(nodes, node)
		}

		tok = p.lexer.PeekToken()
		if tok.Type == TOK_SEMI {
			p.lexer.NextToken()
		} else if tok.Type == TOK_NEWLINE {
			p.lexer.NextToken()
			if topLevel {
				break
			}
		} else if tok.Type == TOK_AMP {
			p.lexer.NextToken()
			if node != nil {
				node.Conn = "&"
			}
		} else {
			// If not a separator, we might have reached the end of a line/command
			break
		}
	}

	if len(nodes) == 0 {
		return nil, false
	}
	if len(nodes) == 1 {
		return nodes[0], true
	}
	return &ASTNode{Type: "list", Pipes: nodes}, true
}

func (p *Parser) parseCommand() *ASTNode {
	tok := p.lexer.PeekToken()
	if tok.Type == TOK_EOF || tok.Type == TOK_NEWLINE || tok.Type == TOK_SEMI {
		return nil
	}

	switch tok.Value {
	case "if":
		return p.parseIf()
	case "while", "until":
		return p.parseWhile()
	case "for":
		return p.parseFor()
	case "case":
		return p.parseCase()
	case "{":
		p.lexer.NextToken()
		node, _ := p.parseCommandList(false)
		p.expect(TOK_RBRACE, "}")
		return &ASTNode{Type: "brace_group", Body: node}
	case "(":
		p.lexer.NextToken()
		node, _ := p.parseCommandList(false)
		p.expect(TOK_RPAREN, ")")
		return &ASTNode{Type: "subshell", Body: node}
	}

	return p.parseSimpleCommand()
}

func (p *Parser) parseSimpleCommand() *ASTNode {
	node := &ASTNode{Type: "simple", Args: []string{}}
	for {
		tok := p.lexer.PeekToken()
		if tok.Type == TOK_EOF || tok.Type == TOK_SEMI || tok.Type == TOK_NEWLINE || tok.Type == TOK_AMP || tok.Type == TOK_PIPE || tok.Type == TOK_AND_IF || tok.Type == TOK_OR_IF || tok.Type == TOK_RPAREN || tok.Type == TOK_RBRACE {
			break
		}

		fd := -1
		if tok.Type == TOK_IO_NUMBER {
			fdStr := p.lexer.NextToken().Value
			fmt.Sscanf(fdStr, "%d", &fd)
			tok = p.lexer.PeekToken()
		}

		if isRedirection(tok) {
			op := p.lexer.NextToken().Value
			if fd == -1 {
				if op == "<" || op == "<<" || op == "<<-" || op == "<&" || op == "<>" {
					fd = 0
				} else {
					fd = 1
				}
			}
			fileTok := p.lexer.NextToken()
			redir := Redir{Op: op, File: fileTok.Value, Fd: fd}
			node.Redirs = append(node.Redirs, redir)
			
			if op == "<<" || op == "<<-" {
				p.lexer.QueueHeredoc(&node.Redirs[len(node.Redirs)-1], fileTok.Value)
			}
			continue
		}

		if tok.Type == TOK_ASSIGNMENT && len(node.Args) == 0 {
			if node.Assigns == nil {
				node.Assigns = make(map[string]string)
			}
			parts := splitAssignment(tok.Value)
			node.Assigns[parts[0]] = parts[1]
			p.lexer.NextToken()
			continue
		}

		if tok.Type == TOK_WORD {
			node.Args = append(node.Args, tok.Value)
			p.lexer.NextToken()
			continue
		}

		break
	}

	// Handle pipeline and and/or logic
	tok := p.lexer.PeekToken()
	if tok.Type == TOK_PIPE {
		p.lexer.NextToken()
		right := p.parseCommand()
		return &ASTNode{Type: "pipeline", Pipes: []*ASTNode{node, right}}
	}
	if tok.Type == TOK_AND_IF {
		p.lexer.NextToken()
		right := p.parseCommand()
		return &ASTNode{Type: "and_or", Left: node, Right: right, Conn: "&&"}
	}
	if tok.Type == TOK_OR_IF {
		p.lexer.NextToken()
		right := p.parseCommand()
		return &ASTNode{Type: "and_or", Left: node, Right: right, Conn: "||"}
	}

	return node
}

func (p *Parser) parseIf() *ASTNode {
	p.lexer.NextToken() // if
	cond, _ := p.parseCommandList(false)
	p.expectValue("then")
	body, _ := p.parseCommandList(false)

	var elseNode *ASTNode
	tok := p.lexer.PeekToken()
	if tok.Value == "else" {
		p.lexer.NextToken()
		elseNode, _ = p.parseCommandList(false)
	} else if tok.Value == "elif" {
		elseNode = p.parseIf()
	}

	p.expectValue("fi")
	return &ASTNode{Type: "if", Cond: cond, Body: body, Else: elseNode}
}

func (p *Parser) parseWhile() *ASTNode {
	tok := p.lexer.NextToken()
	keyword := tok.Value
	cond, _ := p.parseCommandList(false)
	p.expectValue("do")
	body, _ := p.parseCommandList(false)
	p.expectValue("done")
	return &ASTNode{Type: keyword, Cond: cond, Body: body}
}

func (p *Parser) parseFor() *ASTNode {
	p.lexer.NextToken() // for
	varName := p.lexer.NextToken().Value
	p.expectValue("in")

	var words []string
	for {
		tok := p.lexer.PeekToken()
		if tok.Value == "do" || tok.Type == TOK_SEMI || tok.Type == TOK_NEWLINE {
			break
		}
		words = append(words, p.lexer.NextToken().Value)
	}

	if p.lexer.PeekToken().Type == TOK_SEMI {
		p.lexer.NextToken()
	}

	p.expectValue("do")
	body, _ := p.parseCommandList(false)
	p.expectValue("done")
	return &ASTNode{Type: "for", LoopVar: varName, LoopWords: words, Body: body}
}

func (p *Parser) parseCase() *ASTNode {
	p.lexer.NextToken() // case
	word := p.lexer.NextToken().Value
	p.skipNewlines()
	p.expectValue("in")
	p.skipNewlines()

	node := &ASTNode{Type: "case", Value: word, Cases: []CaseItem{}}

	for {
		tok := p.lexer.PeekToken()
		if tok.Value == "esac" || tok.Type == TOK_EOF {
			break
		}

		if tok.Type == TOK_NEWLINE {
			p.lexer.NextToken()
			continue
		}

		// (pattern) or pattern)
		if tok.Value == "(" {
			p.lexer.NextToken()
		}

		pattern := p.lexer.NextToken().Value
		p.expectValue(")")

		body, _ := p.parseCommandList(false)
		node.Cases = append(node.Cases, CaseItem{Pattern: pattern, Body: body})

		tok = p.lexer.PeekToken()
		if tok.Value == ";;" {
			p.lexer.NextToken()
		}
		p.skipNewlines()
	}

	p.expectValue("esac")
	return node
}

func (p *Parser) skipNewlines() {
	for p.lexer.PeekToken().Type == TOK_NEWLINE {
		p.lexer.NextToken()
	}
}

func (p *Parser) expect(t TokenType, val string) {
	tok := p.lexer.NextToken()
	if tok.Type != t {
		// Panic for simplicity in prototype
	}
}

func (p *Parser) expectValue(val string) {
	tok := p.lexer.NextToken()
	if tok.Value != val {
		// Panic for simplicity
	}
}

func isRedirection(t Token) bool {
	return t.Type == TOK_LESS || t.Type == TOK_GREAT || t.Type == TOK_DLESS || t.Type == TOK_DGREAT || t.Type == TOK_LESSAND || t.Type == TOK_GREATAND || t.Type == TOK_LESSGREAT || t.Type == TOK_CLOBBER || t.Type == TOK_DLESSDASH
}

func splitAssignment(s string) []string {
	idx := 0
	for i, c := range s {
		if c == '=' {
			idx = i
			break
		}
	}
	return []string{s[:idx], s[idx+1:]}
}

package main

import (
	"bufio"
	"fmt"
	"strings"
	"unicode"
)

type HeredocPending struct {
	Redir *Redir
	Delim string
}

type PromptReader interface {
	ReadLine(prompt string) (string, error)
}

type Lexer struct {
	input           string
	pos             int
	reader          *bufio.Reader
	promptReader    PromptReader
	pendingHeredocs []*HeredocPending
}

func NewLexer(input string) *Lexer {
	return &Lexer{input: input, pos: 0}
}

func NewLexerWithReader(input string, reader *bufio.Reader) *Lexer {
	return &Lexer{input: input, pos: 0, reader: reader}
}

func NewLexerWithPromptReader(input string, pr PromptReader) *Lexer {
	return &Lexer{input: input, pos: 0, promptReader: pr}
}

func (l *Lexer) QueueHeredoc(redir *Redir, delim string) {
	l.pendingHeredocs = append(l.pendingHeredocs, &HeredocPending{Redir: redir, Delim: delim})
}

func (l *Lexer) readPendingHeredocs() {
	for _, h := range l.pendingHeredocs {
		var sb strings.Builder
		for {
			var line string
			var err error
			if l.promptReader != nil {
				line, err = l.promptReader.ReadLine(varGet("PS2"))
				if err == nil {
					line += "\n"
				}
			} else if l.reader != nil {
				if sh.Interactive {
					fmt.Print(varGet("PS2"))
				}
				line, err = l.reader.ReadString('\n')
			} else {
				break
			}

			if err != nil || line == "" {
				break
			}

			trimmed := strings.TrimRight(line, "\r\n")
			if trimmed == h.Delim {
				break
			}
			sb.WriteString(line)
		}
		h.Redir.HeredocBody = sb.String()
	}
	l.pendingHeredocs = nil
}

func (l *Lexer) readMore() bool {
	if l.promptReader != nil {
		line, err := l.promptReader.ReadLine(varGet("PS2"))
		if err == nil {
			l.input += line + "\n"
			return true
		}
	} else if l.reader != nil {
		if sh.Interactive {
			fmt.Print(varGet("PS2"))
		}
		line, err := l.reader.ReadString('\n')
		if err == nil || len(line) > 0 {
			l.input += line
			return true
		}
	}
	return false
}

func (l *Lexer) NextRune() rune {
	if l.pos >= len(l.input) {
		if !l.readMore() {
			return -1
		}
	}
	r := rune(l.input[l.pos])
	l.pos++
	return r
}

func (l *Lexer) backup() {
	if l.pos > 0 {
		l.pos--
	}
}

func (l *Lexer) PeekRune() rune {
	r := l.NextRune()
	if r != -1 {
		l.backup()
	}
	return r
}

func (l *Lexer) PeekToken() Token {
	oldPos := l.pos
	tok := l.readToken()
	l.pos = oldPos
	return tok
}

func (l *Lexer) NextToken() Token {
	return l.readToken()
}

func (l *Lexer) skipBlanks() {
	for l.pos < len(l.input) {
		r := rune(l.input[l.pos])
		if r == ' ' || r == '\t' {
			l.pos++
			continue
		}
		break
	}
}

func (l *Lexer) Tokenize() []Token {
	var tokens []Token
	for {
		l.skipBlanks()
		tok := l.readToken()
		if tok.Type == TOK_EOF {
			break
		}
		tokens = append(tokens, tok)
	}
	return tokens
}

func (l *Lexer) readToken() Token {
	l.skipBlanks()
	r := l.NextRune()
	if r == -1 {
		return Token{Type: TOK_EOF}
	}

	if r == '\\' {
		nr := l.NextRune()
		if nr == '\n' {
			return l.readToken()
		}
		if nr != -1 {
			l.backup()
		}
	}

	if r == '#' {
		for {
			r = l.NextRune()
			if r == -1 || r == '\n' {
				break
			}
		}
		if r == '\n' {
			l.backup()
			return l.readToken()
		}
		return Token{Type: TOK_EOF}
	}

	if r == '\n' {
		l.readPendingHeredocs()
		return Token{Type: TOK_NEWLINE, Value: "\n"}
	}

	if unicode.IsDigit(r) {
		oldPos := l.pos
		var isIO bool
		var sb strings.Builder
		sb.WriteRune(r)
		for {
			nr := l.NextRune()
			if nr != -1 && unicode.IsDigit(nr) {
				sb.WriteRune(nr)
			} else if nr == '<' || nr == '>' {
				if nr != -1 {
					l.backup()
				}
				isIO = true
				break
			} else {
				if nr != -1 {
					l.backup()
				}
				break
			}
		}
		if isIO {
			return Token{Type: TOK_IO_NUMBER, Value: sb.String()}
		}
		l.pos = oldPos
	}

	if isOperator(r) {
		return l.readOperator(r)
	}

	return l.readWord(r)
}

func isOperator(r rune) bool {
	return r == '|' || r == '&' || r == ';' || r == '<' || r == '>' || r == '(' || r == ')'
}

func (l *Lexer) readOperator(r rune) Token {
	switch r {
	case '|':
		if l.PeekRune() == '|' {
			l.NextRune()
			return Token{Type: TOK_OR_IF, Value: "||"}
		}
		return Token{Type: TOK_PIPE, Value: "|"}
	case '&':
		if l.PeekRune() == '&' {
			l.NextRune()
			return Token{Type: TOK_AND_IF, Value: "&&"}
		}
		return Token{Type: TOK_AMP, Value: "&"}
	case ';':
		if l.PeekRune() == ';' {
			l.NextRune()
			return Token{Type: TOK_DSEMI, Value: ";;"}
		}
		return Token{Type: TOK_SEMI, Value: ";"}
	case '<':
		nr := l.NextRune()
		if nr == '<' {
			if l.PeekRune() == '-' {
				l.NextRune()
				return Token{Type: TOK_DLESSDASH, Value: "<<-"}
			}
			return Token{Type: TOK_DLESS, Value: "<<"}
		}
		if nr == '&' {
			return Token{Type: TOK_LESSAND, Value: "<&"}
		}
		if nr == '>' {
			return Token{Type: TOK_LESSGREAT, Value: "<>"}
		}
		if nr != -1 {
			l.backup()
		}
		return Token{Type: TOK_LESS, Value: "<"}
	case '>':
		nr := l.NextRune()
		if nr == '>' {
			return Token{Type: TOK_DGREAT, Value: ">>"}
		}
		if nr == '&' {
			return Token{Type: TOK_GREATAND, Value: ">&"}
		}
		if nr == '|' {
			return Token{Type: TOK_CLOBBER, Value: ">|"}
		}
		if nr != -1 {
			l.backup()
		}
		return Token{Type: TOK_GREAT, Value: ">"}
	case '(':
		return Token{Type: TOK_LPAREN, Value: "("}
	case ')':
		return Token{Type: TOK_RPAREN, Value: ")"}
	}
	return Token{Type: TOK_WORD, Value: string(r)}
}

func (l *Lexer) readWord(r rune) Token {
	var sb strings.Builder
	var depth int

	for {
		switch r {
		case -1:
			goto done
		case '\\':
			nr := l.NextRune()
			if nr != -1 {
				sb.WriteRune('\\')
				sb.WriteRune(nr)
			}
		case '\'':
			sb.WriteRune('\'')
			for {
				r = l.NextRune()
				if r == -1 {
					break
				}
				sb.WriteRune(r)
				if r == '\'' {
					break
				}
			}
		case '"':
			sb.WriteRune('"')
			for {
				r = l.NextRune()
				if r == -1 {
					break
				}
				if r == '\\' {
					nr := l.NextRune()
					sb.WriteRune('\\')
					if nr != -1 {
						sb.WriteRune(nr)
					}
					continue
				}
				if r == '"' {
					sb.WriteRune('"')
					break
				}
				sb.WriteRune(r)
				if r == '`' {
					for {
						r = l.NextRune()
						if r == -1 {
							break
						}
						sb.WriteRune(r)
						if r == '\\' {
							nr := l.NextRune()
							if nr != -1 {
								sb.WriteRune(nr)
							}
							continue
						}
						if r == '`' {
							break
						}
					}
				}
				if r == '$' {
					nr := l.NextRune()
					if nr == '(' {
						nnc := l.NextRune()
						if nnc == '(' {
							sb.WriteRune('(')
							sb.WriteRune('(')
							depth = 2
							for depth > 0 {
								r = l.NextRune()
								if r == -1 {
									break
								}
								sb.WriteRune(r)
								if r == '(' {
									depth++
								} else if r == ')' {
									depth--
								}
							}
							continue
						}
						l.backup()
						sb.WriteRune('(')
						depth = 1
						for depth > 0 {
							r = l.NextRune()
							if r == -1 {
								break
							}
							sb.WriteRune(r)
							if r == '(' {
								depth++
							} else if r == ')' {
								depth--
							} else if r == '\'' {
								for {
									r = l.NextRune()
									if r == -1 {
										break
									}
									sb.WriteRune(r)
									if r == '\'' {
										break
									}
								}
							} else if r == '"' {
								for {
									r = l.NextRune()
									if r == -1 {
										break
									}
									sb.WriteRune(r)
									if r == '\\' {
										nr := l.NextRune()
										if nr != -1 {
											sb.WriteRune(nr)
										}
										continue
									}
									if r == '"' {
										break
									}
								}
							}
						}
						continue
					} else if nr == '{' {
						sb.WriteRune('{')
						for {
							r = l.NextRune()
							if r == -1 || r == '}' {
								if r == '}' {
									sb.WriteRune('}')
								}
								break
							}
							sb.WriteRune(r)
						}
					} else {
						if nr != -1 {
							l.backup()
						}
					}
				}
			}
		case '`':
			sb.WriteRune('`')
			for {
				r = l.NextRune()
				if r == -1 {
					break
				}
				sb.WriteRune(r)
				if r == '\\' {
					nr := l.NextRune()
					if nr != -1 {
						sb.WriteRune(nr)
					}
					continue
				}
				if r == '`' {
					break
				}
			}
		case '$':
			sb.WriteRune('$')
			r = l.NextRune()
			if r == '(' {
				nr := l.NextRune()
				if nr == '(' {
					sb.WriteRune('(')
					sb.WriteRune('(')
					depth = 2
					for depth > 0 {
						r = l.NextRune()
						if r == -1 {
							break
						}
						sb.WriteRune(r)
						if r == '(' {
							depth++
						} else if r == ')' {
							depth--
						}
					}
					r = l.NextRune()
					continue
				}
				l.backup()
				sb.WriteRune('(')
				depth = 1
				for depth > 0 {
					r = l.NextRune()
					if r == -1 {
						break
					}
					sb.WriteRune(r)
					if r == '(' {
						depth++
					} else if r == ')' {
						depth--
					} else if r == '\'' {
						for {
							r = l.NextRune()
							if r == -1 {
								break
							}
							sb.WriteRune(r)
							if r == '\'' {
								break
							}
						}
					} else if r == '"' {
						for {
							r = l.NextRune()
							if r == -1 {
								break
							}
							sb.WriteRune(r)
							if r == '\\' {
								nr := l.NextRune()
								if nr != -1 {
									sb.WriteRune(nr)
								}
								continue
							}
							if r == '"' {
								break
							}
						}
					}
				}
				r = l.NextRune()
				continue
			} else if r == '{' {
				sb.WriteRune('{')
				depth = 1
				for {
					r = l.NextRune()
					if r == -1 {
						break
					}
					sb.WriteRune(r)
					if r == '{' {
						depth++
					} else if r == '}' {
						depth--
						if depth == 0 {
							break
						}
					}
				}
			} else {
				if r != -1 {
					l.backup()
				}
			}
		default:
			if unicode.IsSpace(r) || isOperator(r) || r == '#' {
				l.backup()
				goto done
			}
			sb.WriteRune(r)
		}
		r = l.NextRune()
	}

done:
	word := sb.String()
	t := TOK_WORD
	if isAssignment(word) {
		t = TOK_ASSIGNMENT
	}
	return Token{Type: t, Value: word}
}

func findMatchingParen(s string, start int) int {
	depth := 0
	for i := start; i < len(s); i++ {
		if s[i] == '(' {
			depth++
		} else if s[i] == ')' {
			depth--
			if depth == 0 {
				return i
			}
		}
	}
	return -1
}

func findMatchingBacktick(s string, start int) int {
	for i := start; i < len(s); i++ {
		if s[i] == '`' {
			return i
		}
	}
	return -1
}

func tokenize(line string) []Token {
	lexer := NewLexer(line)
	return lexer.Tokenize()
}

package main

import (
	"bytes"
	"fmt"
	"os"
	"os/exec"
	"os/user"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"unicode"
)

func expandCommandSubstitution(s string) string {
	var result strings.Builder
	i := 0

	for i < len(s) {
		if i+1 < len(s) && s[i] == '$' && s[i+1] == '(' {
			end := findMatchingParen(s, i+1)
			if end > i+1 {
				inner := s[i+2 : end]
				out := runCommandOutput(inner)
				result.WriteString(out)
				i = end + 1
				continue
			}
		}

		if i+1 < len(s) && s[i] == '$' && s[i+1] == '`' {
			end := findMatchingBacktick(s, i+1)
			if end > i+1 {
				inner := s[i+2 : end]
				out := runCommandOutput(inner)
				result.WriteString(out)
				i = end + 1
				continue
			}
		}

		if i+2 < len(s) && s[i] == '$' && s[i+1] == '(' && s[i+2] == '(' {
			end := findDoubleParen(s, i+2)
			if end > i+2 {
				inner := s[i+3 : end]
				out := evaluateArithmetic(inner)
				result.WriteString(out)
				i = end + 2
				continue
			}
		}

		result.WriteByte(s[i])
		i++
	}

	return result.String()
}

func findDoubleParen(s string, start int) int {
	depth := 0
	for i := start; i < len(s); i++ {
		if i+1 < len(s) && s[i] == '(' && s[i+1] == '(' {
			depth++
		} else if i+1 < len(s) && s[i] == ')' && s[i+1] == ')' {
			depth--
			if depth == 0 {
				return i
			}
		}
	}
	return -1
}

func evaluateArithmetic(s string) string {
	s = strings.TrimSpace(s)

	re := regexp.MustCompile(`(\d+)\s*\*\s*(\d+)`)
	s = re.ReplaceAllStringFunc(s, func(m string) string {
		parts := regexp.MustCompile(`\d+`).FindAllString(m, -1)
		a, _ := strconv.Atoi(parts[0])
		b, _ := strconv.Atoi(parts[1])
		return strconv.Itoa(a * b)
	})

	re = regexp.MustCompile(`(\d+)\s*\+\s*(\d+)`)
	s = re.ReplaceAllStringFunc(s, func(m string) string {
		parts := regexp.MustCompile(`\d+`).FindAllString(m, -1)
		a, _ := strconv.Atoi(parts[0])
		b, _ := strconv.Atoi(parts[1])
		return strconv.Itoa(a + b)
	})

	re = regexp.MustCompile(`(\d+)\s*-\s*(\d+)`)
	s = re.ReplaceAllStringFunc(s, func(m string) string {
		parts := regexp.MustCompile(`\d+`).FindAllString(m, -1)
		a, _ := strconv.Atoi(parts[0])
		b, _ := strconv.Atoi(parts[1])
		return strconv.Itoa(a - b)
	})

	re = regexp.MustCompile(`(\d+)\s*/\s*(\d+)`)
	s = re.ReplaceAllStringFunc(s, func(m string) string {
		parts := regexp.MustCompile(`\d+`).FindAllString(m, -1)
		a, _ := strconv.Atoi(parts[0])
		b, _ := strconv.Atoi(parts[1])
		if b != 0 {
			return strconv.Itoa(a / b)
		}
		return "0"
	})

	num, err := strconv.Atoi(strings.TrimSpace(s))
	if err == nil {
		return strconv.Itoa(num)
	}

	return "0"
}

func runCommandOutput(cmd string) string {
	cmd = strings.TrimSpace(cmd)
	parts := strings.Fields(cmd)
	if len(parts) == 0 {
		return ""
	}

	proc := exec.Command(parts[0], parts[1:]...)
	var out bytes.Buffer
	proc.Stdout = &out
	proc.Run()
	return strings.TrimSpace(out.String())
}

func expandGlob(s string) string {
	if strings.ContainsAny(s, "*?[]") && !strings.HasPrefix(s, "'") {
		matches, err := filepath.Glob(s)
		if err == nil && len(matches) > 0 {
			return strings.Join(matches, " ")
		}
	}
	return s
}

func expandVariable(s string) string {
	if len(s) >= 2 && s[0] == '\'' && s[len(s)-1] == '\'' {
		return s[1 : len(s)-1]
	}

	doubleQuoted := false
	if len(s) >= 2 && s[0] == '"' && s[len(s)-1] == '"' {
		doubleQuoted = true
		s = s[1 : len(s)-1]
	}

	var result strings.Builder
	i := 0

	for i < len(s) {
		if s[i] == '$' && i+1 < len(s) {
			nextCh := s[i+1]
			if nextCh == '{' {
				end := strings.Index(s[i+2:], "}")
				if end >= 0 {
					varExpr := s[i+2 : i+2+end]
					result.WriteString(expandVarExpr(varExpr))
					i = i + 3 + end
					continue
				}
			} else if nextCh == '$' {
				result.WriteString(fmt.Sprintf("%d", sh.ShellPid))
				i += 2
				continue
			} else if nextCh == '?' {
				result.WriteString(fmt.Sprintf("%d", sh.LastStatus))
				i += 2
				continue
			} else if nextCh == '!' {
				result.WriteString(fmt.Sprintf("%d", sh.LastBgPid))
				i += 2
				continue
			} else if unicode.IsLetter(rune(nextCh)) || unicode.IsDigit(rune(nextCh)) || nextCh == '_' {
				end := i + 1
				for end < len(s) && (unicode.IsLetter(rune(s[end])) || unicode.IsDigit(rune(s[end])) || s[end] == '_') {
					end++
				}
				varName := s[i+1 : end]
				result.WriteString(varGetSimple(varName))
				i = end
				continue
			}
		}
		result.WriteByte(s[i])
		i++
	}

	res := result.String()
	if !doubleQuoted {
		res = expandGlob(res)
	}
	return res
}

func expandVarExpr(expr string) string {
	if strings.HasPrefix(expr, "#") && len(expr) > 1 {
		val := varGetSimple(expr[1:])
		return strconv.Itoa(len(val))
	}

	ops := []string{":-", ":=", ":?", ":+", "-", "=", "?", "+", "%%", "%", "##", "#"}
	for _, op := range ops {
		idx := strings.Index(expr, op)
		if idx != -1 {
			param := expr[:idx]
			word := expr[idx+len(op):]
			val := varGetSimple(param)

			switch op {
			case ":-", "-":
				if val == "" {
					return expandVariable(word)
				}
				return val
			case ":=", "=":
				if val == "" {
					expandedWord := expandVariable(word)
					varSet(param, expandedWord, false)
					return expandedWord
				}
				return val
			case ":?", "?":
				if val == "" {
					msg := word
					if msg == "" {
						msg = "parameter null or not set"
					}
					fmt.Fprintf(os.Stderr, "meowsh: %s: %s\n", param, msg)
					return ""
				}
				return val
			case ":+", "+":
				if val != "" {
					return expandVariable(word)
				}
				return ""
			case "##":
				pattern := expandVariable(word)
				for i := len(val); i >= 1; i-- {
					matched, _ := filepath.Match(pattern, val[:i])
					if matched {
						return val[i:]
					}
				}
				return val
			case "#":
				pattern := expandVariable(word)
				for i := 1; i <= len(val); i++ {
					matched, _ := filepath.Match(pattern, val[:i])
					if matched {
						return val[i:]
					}
				}
				return val
			case "%%":
				pattern := expandVariable(word)
				for i := 0; i < len(val); i++ {
					matched, _ := filepath.Match(pattern, val[i:])
					if matched {
						return val[:i]
					}
				}
				return val
			case "%":
				pattern := expandVariable(word)
				for i := len(val) - 1; i >= 0; i-- {
					matched, _ := filepath.Match(pattern, val[i:])
					if matched {
						return val[:i]
					}
				}
				return val
			}
		}
	}
	return varGetSimple(expr)
}

func varGetSimple(name string) string {
	switch name {
	case "$":
		return fmt.Sprintf("%d", sh.ShellPid)
	case "?":
		return fmt.Sprintf("%d", sh.LastStatus)
	case "!":
		return fmt.Sprintf("%d", sh.LastBgPid)
	case "0":
		return sh.Argv1()
	case "#":
		return fmt.Sprintf("%d", len(sh.PosParams))
	case "@", "*":
		return strings.Join(sh.PosParams, " ")
	}
	if v, ok := sh.Vars[name]; ok {
		return v.Value
	}
	return ""
}

func expandTilde(s string) string {
	if !strings.HasPrefix(s, "~") {
		return s
	}

	slashIdx := strings.Index(s, "/")
	if slashIdx == -1 {
		slashIdx = len(s)
	}

	username := s[1:slashIdx]
	var homeDir string

	if username == "" {
		homeDir = varGet("HOME")
	} else {
		u, err := user.Lookup(username)
		if err == nil {
			homeDir = u.HomeDir
		} else {
			return s
		}
	}

	if homeDir != "" {
		return homeDir + s[slashIdx:]
	}
	return s
}

func expandAll(s string) string {
	// 0. Tilde expansion
	s = expandTilde(s)
	// 1. Command substitution
	s = expandCommandSubstitution(s)
	// 2. Variable expansion & Globbing & Quote removal (handled inside expandVariable for now)
	s = expandVariable(s)
	return s
}

func expandAliasLine(line string) string {
	words := strings.Fields(line)
	if len(words) == 0 {
		return line
	}

	firstWord := words[0]
	if alias, ok := sh.Aliases[firstWord]; ok {
		rest := strings.Join(words[1:], " ")
		if rest != "" {
			return alias + " " + rest
		}
		return alias
	}

	return line
}

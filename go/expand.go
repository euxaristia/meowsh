package main

import (
	"fmt"
	"os"
	"os/exec"
	"os/user"
	"path/filepath"
	"strconv"
	"strings"
	"unicode"
)

func expandCommandSubstitution(s string) string {
	var result strings.Builder
	i := 0

	for i < len(s) {
		if i+2 < len(s) && s[i] == '$' && s[i+1] == '(' && s[i+2] == '(' {
			result.WriteByte('$')
			result.WriteByte('(')
			result.WriteByte('(')
			i += 3
			continue
		}
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

		if s[i] == '`' {
			end := findMatchingBacktick(s, i+1)
			if end > i {
				inner := s[i+1 : end]
				out := runCommandOutput(inner)
				result.WriteString(out)
				i = end + 1
				continue
			}
		}

		result.WriteByte(s[i])
		i++
	}
	return result.String()
}

func runCommandOutput(cmd string) string {
	c := exec.Command("sh", "-c", cmd)
	out, err := c.Output()
	if err != nil {
		return ""
	}
	return strings.TrimRight(string(out), "\n")
}

func expandVariable(s string) string {
	var result strings.Builder
	i := 0
	for i < len(s) {
		if i+2 < len(s) && s[i] == '$' && s[i+1] == '(' && s[i+2] == '(' {
			result.WriteString("$")
			i++
			continue
		}
		if s[i] == '$' {
			if i+1 < len(s) && s[i+1] == '{' {
				end := strings.Index(s[i:], "}")
				if end != -1 {
					expr := s[i+2 : i+end]
					result.WriteString(expandVarExpr(expr))
					i += end + 1
					continue
				}
			} else {
				j := i + 1
				for j < len(s) && (unicode.IsLetter(rune(s[j])) || unicode.IsDigit(rune(s[j])) || s[j] == '_' || s[j] == '?' || s[j] == '$' || s[j] == '!' || s[j] == '#' || s[j] == '@' || s[j] == '*') {
					j++
				}
				if j > i+1 {
					name := s[i+1 : j]
					result.WriteString(varGet(name))
					i = j
					continue
				}
			}
		}
		result.WriteByte(s[i])
		i++
	}

	res := result.String()
	if !sh.Interactive || (sh.Opts&OPT_NOGLOB == 0) {
		res = expandGlob(res)
	}
	return res
}

func expandGlob(s string) string {
	if !strings.ContainsAny(s, "*?[]") {
		return s
	}
	matches, err := filepath.Glob(s)
	if err == nil && len(matches) > 0 {
		return strings.Join(matches, " ")
	}
	return s
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

func expandArithmetic(s string) string {
	if strings.HasPrefix(s, "$(") && strings.HasSuffix(s, ")") {
		inner := s[2 : len(s)-1]
		if strings.HasPrefix(inner, "(") && strings.HasSuffix(inner, ")") {
			val := evalArithmetic(inner[1 : len(inner)-1])
			return strconv.Itoa(val)
		}
	}

	var result strings.Builder
	i := 0
	for i < len(s) {
		if i+2 < len(s) && s[i] == '$' && s[i+1] == '(' && s[i+2] == '(' {
			end := findMatchingDoubleParen(s, i+1)
			if end != -1 {
				inner := s[i+3 : end-1]
				val := evalArithmetic(inner)
				result.WriteString(strconv.Itoa(val))
				i = end + 1
				continue
			}
		}
		result.WriteByte(s[i])
		i++
	}
	return result.String()
}

func findMatchingDoubleParen(s string, start int) int {
	depth := 0
	for i := start; i < len(s)-1; i++ {
		if s[i] == '(' && s[i+1] == '(' {
			depth++
			i++
		} else if s[i] == ')' && s[i+1] == ')' {
			depth--
			if depth == 0 {
				return i + 1
			}
			i++
		}
	}
	return -1
}

func evalArithmetic(s string) int {
	s = strings.TrimSpace(s)
	if s == "" {
		return 0
	}

	// Resolve variables inside math (handle both $VAR and VAR)
	if strings.Contains(s, "$") {
		s = expandVariable(s)
	}

	if strings.Contains(s, "+") {
		parts := strings.SplitN(s, "+", 2)
		return evalArithmetic(parts[0]) + evalArithmetic(parts[1])
	}
	if strings.Contains(s, "-") {
		parts := strings.SplitN(s, "-", 2)
		return evalArithmetic(parts[0]) - evalArithmetic(parts[1])
	}
	if strings.Contains(s, "*") {
		parts := strings.SplitN(s, "*", 2)
		return evalArithmetic(parts[0]) * evalArithmetic(parts[1])
	}
	if strings.Contains(s, "/") {
		parts := strings.SplitN(s, "/", 2)
		b := evalArithmetic(parts[1])
		if b == 0 {
			return 0
		}
		return evalArithmetic(parts[0]) / b
	}

	s = strings.TrimSpace(s)
	// If it's not a number, try looking it up as a variable
	if val, err := strconv.Atoi(s); err == nil {
		return val
	}
	
	v := varGet(s)
	if v != "" {
		iv, _ := strconv.Atoi(v)
		return iv
	}

	return 0
}

func removeQuotes(s string) string {
	var result strings.Builder
	i := 0
	inDouble := false
	inSingle := false
	for i < len(s) {
		c := s[i]
		if c == '\\' && !inSingle {
			if i+1 < len(s) {
				result.WriteByte(s[i+1])
				i += 2
				continue
			}
		}
		if c == '"' && !inSingle {
			inDouble = !inDouble
			i++
			continue
		}
		if c == '\'' && !inDouble {
			inSingle = !inSingle
			i++
			continue
		}
		result.WriteByte(c)
		i++
	}
	return result.String()
}

func expandAll(s string) string {
	s = expandArithmetic(s)
	s = expandTilde(s)
	s = expandCommandSubstitution(s)
	s = expandVariable(s)
	s = removeQuotes(s)
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

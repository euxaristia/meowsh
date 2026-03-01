package main

import (
	"bufio"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"unsafe"
)

type LineEditor struct {
	prompt      string
	line        []rune
	pos         int
	history     []string
	historyIdx  int
	origTermios syscall.Termios
	rawMode     bool
	reader      *bufio.Reader

	lastMatches []string
	matchIdx    int
	matchDir    string
	lastTabPos  int
	prefixLen   int
}

func NewLineEditor() *LineEditor {
	return &LineEditor{
		history:    []string{},
		historyIdx: -1,
		reader:     bufio.NewReader(os.Stdin),
		matchIdx:   -1,
	}
}

func (le *LineEditor) enableRawMode() error {
	if _, _, err := syscall.Syscall(syscall.SYS_IOCTL, os.Stdin.Fd(), uintptr(syscall.TCGETS), uintptr(unsafe.Pointer(&le.origTermios))); err != 0 {
		return err
	}

	raw := le.origTermios
	// Disable echo, canonical mode, signals, and extended processing
	raw.Lflag &^= syscall.ECHO | syscall.ICANON | syscall.ISIG | syscall.IEXTEN
	// Disable break, CR to NL, parity, and flow control
	raw.Iflag &^= syscall.BRKINT | syscall.ICRNL | syscall.INPCK | syscall.ISTRIP | syscall.IXON
	raw.Cflag |= syscall.CS8
	raw.Cc[syscall.VMIN] = 1
	raw.Cc[syscall.VTIME] = 0

	if _, _, err := syscall.Syscall(syscall.SYS_IOCTL, os.Stdin.Fd(), uintptr(syscall.TCSETS), uintptr(unsafe.Pointer(&raw))); err != 0 {
		return err
	}
	le.rawMode = true
	return nil
}

func (le *LineEditor) disableRawMode() {
	if le.rawMode {
		syscall.Syscall(syscall.SYS_IOCTL, os.Stdin.Fd(), uintptr(syscall.TCSETS), uintptr(unsafe.Pointer(&le.origTermios)))
		le.rawMode = false
	}
}

func (le *LineEditor) refreshLine() {
	// Move to start of line (\r), clear to end of screen (\x1b[J)
	fmt.Print("\r\x1b[J")
	fmt.Print(le.prompt)
	fmt.Print(string(le.line))
	
	// Move cursor back to the correct position
	if le.pos < len(le.line) {
		moveBack := len(le.line) - le.pos
		fmt.Printf("\x1b[%dD", moveBack)
	}
}

func (le *LineEditor) ReadLine(prompt string) (string, error) {
	le.prompt = prompt
	le.line = []rune{}
	le.pos = 0
	le.resetCompletion()
	
	if sh.Interactive && isatty(os.Stdin.Fd()) {
		if err := le.enableRawMode(); err != nil {
			fmt.Print(prompt)
			return le.readLineCooked()
		}
		defer le.disableRawMode()
	} else {
		fmt.Print(prompt)
		return le.readLineCooked()
	}

	le.refreshLine()

	buf := make([]byte, 16)
	for {
		n, err := os.Stdin.Read(buf)
		if err != nil {
			return "", err
		}
		if n == 0 {
			continue
		}

		for i := 0; i < n; i++ {
			b := buf[i]

			if b != 9 {
				le.resetCompletion()
			}

			switch b {
			case 3: // Ctrl-C
				fmt.Print("^C\r\n")
				return "", nil
			case 4: // Ctrl-D
				if len(le.line) == 0 {
					fmt.Print("\r\n")
					return "", io.EOF
				}
				// Delete at cursor
				if le.pos < len(le.line) {
					le.line = append(le.line[:le.pos], le.line[le.pos+1:]...)
					le.refreshLine()
				}
			case 13, 10: // Enter
				fmt.Print("\r\n")
				return string(le.line), nil
			case 127, 8: // Backspace
				if le.pos > 0 {
					le.line = append(le.line[:le.pos-1], le.line[le.pos:]...)
					le.pos--
					le.refreshLine()
				}
			case 9: // Tab
				le.handleTab()
				le.refreshLine()
			case 27: // Escape sequence
				if i+2 < n && buf[i+1] == '[' {
					switch buf[i+2] {
					case 'A': // Up
						// History up (todo)
					case 'B': // Down
						// History down (todo)
					case 'C': // Right
						if le.pos < len(le.line) {
							le.pos++
							fmt.Print("\x1b[1C")
						}
					case 'D': // Left
						if le.pos > 0 {
							le.pos--
							fmt.Print("\x1b[1D")
						}
					case 'H': // Home
						if le.pos > 0 {
							fmt.Printf("\r\x1b[%dC", le.promptWidth())
							le.pos = 0
						}
					case 'F': // End
						if le.pos < len(le.line) {
							fmt.Printf("\x1b[%dC", len(le.line)-le.pos)
							le.pos = len(le.line)
						}
					}
					i += 2
				}
			default:
				if b >= 32 {
					char := rune(b)
					if le.pos == len(le.line) {
						le.line = append(le.line, char)
						fmt.Print(string(char))
					} else {
						newP := make([]rune, len(le.line)+1)
						copy(newP, le.line[:le.pos])
						newP[le.pos] = char
						copy(newP[le.pos+1:], le.line[le.pos:])
						le.line = newP
						le.refreshLine()
					}
					le.pos++
				}
			}
		}
	}
}

func (le *LineEditor) resetCompletion() {
	le.lastMatches = nil
	le.matchIdx = -1
	le.lastTabPos = -1
}

func (le *LineEditor) promptWidth() int {
	// Crude estimate: strip ANSI codes
	w := 0
	inEsc := false
	for _, r := range le.prompt {
		if r == '\x1b' {
			inEsc = true
			continue
		}
		if inEsc {
			if (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') {
				inEsc = false
			}
			continue
		}
		// Emoji/Hieroglyph width handling
		if r > 0xFFFF {
			w += 2
		} else {
			w++
		}
	}
	return w
}

func (le *LineEditor) readLineCooked() (string, error) {
	if le.prompt != "" {
		fmt.Print(le.prompt)
	}
	line, err := le.reader.ReadString('\n')
	return strings.TrimRight(line, "\r\n"), err
}

func (le *LineEditor) handleTab() {
	if len(le.lastMatches) > 0 && le.lastTabPos == le.pos {
		// Cycle
		currentMatch := le.lastMatches[le.matchIdx]
		fullPath := filepath.Join(le.matchDir, currentMatch)
		suffix := " "
		if info, err := os.Stat(fullPath); err == nil && info.IsDir() {
			suffix = "/"
		}
		toDelete := len([]rune(currentMatch[le.prefixLen:] + suffix))
		le.line = append(le.line[:le.pos-toDelete], le.line[le.pos:]...)
		le.pos -= toDelete

		le.matchIdx = (le.matchIdx + 1) % len(le.lastMatches)
		nextMatch := le.lastMatches[le.matchIdx]
		fullPath = filepath.Join(le.matchDir, nextMatch)
		suffix = " "
		if info, err := os.Stat(fullPath); err == nil && info.IsDir() {
			suffix = "/"
		}
		le.insertAtCursor(nextMatch[le.prefixLen:] + suffix)
		le.lastTabPos = le.pos
		return
	}

	start := le.pos
	for start > 0 && le.line[start-1] != ' ' {
		start--
	}
	word := string(le.line[start:le.pos])

	dir := "."
	prefix := word
	if i := strings.LastIndex(word, "/"); i != -1 {
		dir = word[:i+1]
		prefix = word[i+1:]
	}

	matches := []string{}
	files, err := os.ReadDir(dir)
	if err == nil {
		for _, f := range files {
			if strings.HasPrefix(f.Name(), prefix) {
				matches = append(matches, f.Name())
			}
		}
	}

	if len(matches) == 0 {
		return
	}

	if len(matches) == 1 {
		fullPath := filepath.Join(dir, matches[0])
		completed := matches[0][len(prefix):]
		if info, err := os.Stat(fullPath); err == nil && info.IsDir() {
			completed += "/"
		} else {
			completed += " "
		}
		le.insertAtCursor(completed)
	} else {
		// Longest common prefix
		cp := matches[0]
		for _, m := range matches[1:] {
			i := 0
			for i < len(cp) && i < len(m) && cp[i] == m[i] {
				i++
			}
			cp = cp[:i]
		}

		if len(cp) > len(prefix) {
			le.insertAtCursor(cp[len(prefix):])
		} else {
			// Start cycling
			le.lastMatches = matches
			le.matchIdx = 0
			le.matchDir = dir
			le.prefixLen = len(prefix)

			nextMatch := matches[0]
			fullPath := filepath.Join(dir, nextMatch)
			suffix := " "
			if info, err := os.Stat(fullPath); err == nil && info.IsDir() {
				suffix = "/"
			}
			le.insertAtCursor(nextMatch[len(prefix):] + suffix)
		}
	}
	le.lastTabPos = le.pos
}

func (le *LineEditor) insertAtCursor(s string) {
	runes := []rune(s)
	newP := make([]rune, len(le.line)+len(runes))
	copy(newP, le.line[:le.pos])
	copy(newP[le.pos:], runes)
	copy(newP[le.pos+len(runes):], le.line[le.pos:])
	le.line = newP
	le.pos += len(runes)
}

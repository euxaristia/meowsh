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

	// Completion state
	lastMatches []string
	matchIdx    int
	matchDir    string
	baseLine    []rune
	basePos     int
	prefixLen   int
	menuRows    int
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
	raw.Lflag &^= syscall.ECHO | syscall.ICANON | syscall.ISIG | syscall.IEXTEN
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

func (le *LineEditor) getTermWidth() int {
	var ws struct {
		Row    uint16
		Col    uint16
		Xpixel uint16
		Ypixel uint16
	}
	_, _, err := syscall.Syscall(syscall.SYS_IOCTL, os.Stdin.Fd(), uintptr(syscall.TIOCGWINSZ), uintptr(unsafe.Pointer(&ws)))
	if err != 0 {
		return 80
	}
	return int(ws.Col)
}

func (le *LineEditor) refreshLine() {
	// Clear from cursor down to remove old menu if any
	fmt.Print("\r\x1b[J")
	fmt.Print(le.prompt)
	fmt.Print(string(le.line))
	
	if le.pos < len(le.line) {
		moveBack := len(le.line) - le.pos
		fmt.Printf("\x1b[%dD", moveBack)
	}

	if len(le.lastMatches) > 0 {
		le.renderMenu()
	}
}

func (le *LineEditor) renderMenu() {
	fmt.Print("\x1b[s") // Save cursor
	fmt.Print("\n")
	
	width := le.getTermWidth()
	maxLen := 0
	for _, m := range le.lastMatches {
		if len(m) > maxLen {
			maxLen = len(m)
		}
	}
	colWidth := maxLen + 2
	cols := width / colWidth
	if cols == 0 {
		cols = 1
	}
	
	le.menuRows = (len(le.lastMatches) + cols - 1) / cols

	for r := 0; r < le.menuRows; r++ {
		for c := 0; c < cols; c++ {
			idx := r + c*le.menuRows
			if idx < len(le.lastMatches) {
				m := le.lastMatches[idx]
				if idx == le.matchIdx {
					fmt.Printf("\x1b[7m%s\x1b[0m", m) // Highlight
				} else {
					// Add color based on type if we had it, for now just print
					fullPath := filepath.Join(le.matchDir, m)
					if info, err := os.Stat(fullPath); err == nil && info.IsDir() {
						fmt.Printf("\033[34m%s\033[0m", m)
					} else {
						fmt.Print(m)
					}
				}
				if c < cols-1 {
					fmt.Print(strings.Repeat(" ", colWidth-len(m)))
				}
			}
		}
		if r < le.menuRows-1 {
			fmt.Print("\n")
		}
	}
	
	fmt.Print("\x1b[u") // Restore cursor
}

func (le *LineEditor) clearMenu() {
	if le.menuRows > 0 {
		// Moving down and clearing is hard with \x1b[J if we are at the bottom of the screen.
		// refreshLine already clears from cursor down.
		le.menuRows = 0
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

			// Completion keys: Tab, Esc, Arrows
			isTab := b == 9
			isEsc := b == 27
			
			if !isTab && !isEsc {
				le.resetCompletion()
			}

			switch b {
			case 3: // Ctrl-C
				le.clearMenu()
				fmt.Print("^C\r\n")
				return "", nil
			case 4: // Ctrl-D
				if len(le.line) == 0 {
					fmt.Print("\r\n")
					return "", io.EOF
				}
				if le.pos < len(le.line) {
					le.line = append(le.line[:le.pos], le.line[le.pos+1:]...)
					le.refreshLine()
				}
			case 13, 10: // Enter
				le.clearMenu()
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
					key := buf[i+2]
					if len(le.lastMatches) > 0 {
						switch key {
						case 'A': // Up
							le.moveMenu(-1, 0)
							le.applyMatch()
							le.refreshLine()
							i += 2
							continue
						case 'B': // Down
							le.moveMenu(1, 0)
							le.applyMatch()
							le.refreshLine()
							i += 2
							continue
						case 'C': // Right
							le.moveMenu(0, 1)
							le.applyMatch()
							le.refreshLine()
							i += 2
							continue
						case 'D': // Left
							le.moveMenu(0, -1)
							le.applyMatch()
							le.refreshLine()
							i += 2
							continue
						}
					}
					// Normal arrow handling
					switch key {
					case 'A': // Up
					case 'B': // Down
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
				} else {
					le.resetCompletion()
					le.refreshLine()
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
	le.baseLine = nil
	le.menuRows = 0
}

func (le *LineEditor) moveMenu(dRow, dCol int) {
	if len(le.lastMatches) == 0 {
		return
	}
	
	width := le.getTermWidth()
	maxLen := 0
	for _, m := range le.lastMatches {
		if len(m) > maxLen {
			maxLen = len(m)
		}
	}
	colWidth := maxLen + 2
	cols := width / colWidth
	if cols == 0 {
		cols = 1
	}
	rows := (len(le.lastMatches) + cols - 1) / cols

	if le.matchIdx == -1 {
		le.matchIdx = 0
		return
	}

	r := le.matchIdx % rows
	c := le.matchIdx / rows

	if dRow != 0 {
		r = (r + dRow + rows) % rows
	}
	if dCol != 0 {
		c = (c + dCol + cols) % cols
	}

	newIdx := r + c*rows
	if newIdx >= len(le.lastMatches) {
		if dCol > 0 {
			newIdx = r // Wrap to first col
		} else if dCol < 0 {
			newIdx = r + (cols-2)*rows // Previous col might still be too far
			for newIdx >= len(le.lastMatches) {
				newIdx -= rows
			}
		} else {
			newIdx = 0 // Fallback
		}
	}
	le.matchIdx = newIdx
}

func (le *LineEditor) applyMatch() {
	if le.matchIdx == -1 {
		return
	}
	m := le.lastMatches[le.matchIdx]
	fullPath := filepath.Join(le.matchDir, m)
	suffix := " "
	if info, err := os.Stat(fullPath); err == nil && info.IsDir() {
		suffix = "/"
	}
	
	le.line = make([]rune, len(le.baseLine))
	copy(le.line, le.baseLine)
	le.pos = le.basePos
	le.insertAtCursor(m[le.prefixLen:] + suffix)
}

func (le *LineEditor) promptWidth() int {
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
	if len(le.lastMatches) > 0 {
		le.moveMenu(0, 1)
		le.applyMatch()
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
			// Start menu
			le.lastMatches = matches
			le.matchIdx = -1 // No selection yet, but will be 0 on next move
			le.matchDir = dir
			le.prefixLen = len(prefix)
			le.baseLine = make([]rune, len(le.line))
			copy(le.baseLine, le.line)
			le.basePos = le.pos
			
			le.moveMenu(0, 1)
			le.applyMatch()
		}
	}
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

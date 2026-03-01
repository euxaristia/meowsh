package main

import (
	"bufio"
	"fmt"
	"io"
	"os"
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
}

func NewLineEditor() *LineEditor {
	return &LineEditor{
		history:    []string{},
		historyIdx: -1,
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
	// We need to account for prompt width here eventually, but for now simple:
	if le.pos < len(le.line) {
		moveBack := len(le.line) - le.pos
		fmt.Printf("\x1b[%dD", moveBack)
	}
}

func (le *LineEditor) ReadLine(prompt string) (string, error) {
	le.prompt = prompt
	le.line = []rune{}
	le.pos = 0
	
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

		b := buf[0]

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
			if n >= 3 && buf[1] == '[' {
				switch buf[2] {
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
			}
		default:
			if b >= 32 {
				char := []rune(string(buf[:n]))[0]
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
	reader := bufio.NewReader(os.Stdin)
	line, err := reader.ReadString('\n')
	return strings.TrimRight(line, "\r\n"), err
}

func (le *LineEditor) handleTab() {
	start := le.pos
	for start > 0 && le.line[start-1] != ' ' {
		start--
	}
	word := string(le.line[start:le.pos])
	
	matches := []string{}
	files, err := os.ReadDir(".")
	if err == nil {
		for _, f := range files {
			if strings.HasPrefix(f.Name(), word) {
				matches = append(matches, f.Name())
			}
		}
	}

	if len(matches) == 1 {
		completed := matches[0][len(word):]
		if info, err := os.Stat(matches[0]); err == nil && info.IsDir() {
			completed += "/"
		} else {
			completed += " "
		}
		
		newP := make([]rune, len(le.line)+len(completed))
		copy(newP, le.line[:le.pos])
		copy(newP[le.pos:], []rune(completed))
		copy(newP[le.pos+len(completed):], le.line[le.pos:])
		le.line = newP
		le.pos += len(completed)
	} else if len(matches) > 1 {
		fmt.Print("\r\n")
		for _, m := range matches {
			fmt.Printf("%s  ", m)
		}
		fmt.Print("\r\n")
	}
}

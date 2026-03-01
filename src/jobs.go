package main

import (
	"fmt"
	"os"
	"os/signal"
	"syscall"
	"unsafe"
)

func jobsInit() {
	sh.Jobs = []*Job{}
	sh.NextJobId = 1

	if sh.Interactive && (sh.Opts&OPT_MONITOR != 0) {
		// In Go, we use a channel for signals
		sigCh := make(chan os.Signal, 10)
		signal.Notify(sigCh, syscall.SIGCHLD)
		go func() {
			for range sigCh {
				jobReap()
			}
		}()
	}
}

func jobReap() {
	for {
		var wstatus syscall.WaitStatus
		pid, err := syscall.Wait4(-1, &wstatus, syscall.WNOHANG|syscall.WUNTRACED|syscall.WCONTINUED, nil)
		if err != nil || pid <= 0 {
			break
		}
		jobUpdateStatus(pid, wstatus)
	}
}

func jobUpdateStatus(pid int, status syscall.WaitStatus) {
	for _, j := range sh.Jobs {
		for _, p := range j.Procs {
			if p.Pid == pid {
				p.Status = int(status)
				if status.Stopped() {
					p.State = PROC_STOPPED
				} else if status.Continued() {
					p.State = PROC_RUNNING
				} else {
					p.State = PROC_DONE
				}

				// Update job state
				allDone := true
				anyStopped := false
				for _, pp := range j.Procs {
					if pp.State == PROC_RUNNING {
						allDone = false
					}
					if pp.State == PROC_STOPPED {
						anyStopped = true
					}
				}

				if allDone {
					j.State = JOB_DONE
				} else if anyStopped {
					j.State = JOB_STOPPED
				} else {
					j.State = JOB_RUNNING
				}
				return
			}
		}
	}
}

func jobWaitFg(j *Job) int {
	for j.State == JOB_RUNNING {
		// In interactive mode with MONITOR, SIGCHLD handler will update status.
		// We still need to wait if not using SIGCHLD or to block.
		var wstatus syscall.WaitStatus
		pid, err := syscall.Wait4(-j.Pgid, &wstatus, syscall.WUNTRACED, nil)
		if err != nil {
			if err == syscall.ECHILD {
				break
			}
			if err == syscall.EINTR {
				continue
			}
			break
		}
		jobUpdateStatus(pid, wstatus)
	}

	if j.State == JOB_STOPPED {
		fmt.Printf("\n[%d]+  Stopped\t%s\n", j.Id, j.CmdText)
		j.Notified = true
	}

	status := 0
	if len(j.Procs) > 0 {
		lastProc := j.Procs[len(j.Procs)-1]
		status = syscall.WaitStatus(lastProc.Status).ExitStatus()
		if syscall.WaitStatus(lastProc.Status).Signaled() {
			status = 128 + int(syscall.WaitStatus(lastProc.Status).Signal())
		}
	}

	// Give terminal back to shell
	if sh.Interactive {
		syscall.Syscall(syscall.SYS_IOCTL, os.Stdin.Fd(), uintptr(syscall.TIOCSPGRP), uintptr(unsafe.Pointer(&sh.ShellPid)))
	}

	return status
}

func jobsCleanup() {
	newJobs := []*Job{}
	for _, j := range sh.Jobs {
		if j.State == JOB_DONE && (j.Foreground || j.Notified) {
			continue
		}
		newJobs = append(newJobs, j)
	}
	sh.Jobs = newJobs
}

func jobsNotify() {
	for _, j := range sh.Jobs {
		if j.State == JOB_DONE && !j.Notified && !j.Foreground {
			fmt.Printf("[%d]+  Done\t\t%s\n", j.Id, j.CmdText)
			j.Notified = true
		}
	}
	jobsCleanup()
}

func builtinJobs(args []string) int {
	for _, j := range sh.Jobs {
		stateStr := "Running"
		if j.State == JOB_STOPPED {
			stateStr = "Stopped"
		} else if j.State == JOB_DONE {
			stateStr = "Done"
		}
		fmt.Printf("[%d]+  %s\t\t%s\n", j.Id, stateStr, j.CmdText)
	}
	return 0
}

func builtinFg(args []string) int {
	if len(sh.Jobs) == 0 {
		fmt.Println("meowsh: fg: no current job")
		return 1
	}
	j := sh.Jobs[len(sh.Jobs)-1]
	
	fmt.Printf("%s\n", j.CmdText)
	j.State = JOB_RUNNING
	j.Foreground = true
	
	// Give terminal to job
	syscall.Syscall(syscall.SYS_IOCTL, os.Stdin.Fd(), uintptr(syscall.TIOCSPGRP), uintptr(unsafe.Pointer(&j.Pgid)))
	
	// Continue job
	syscall.Kill(-j.Pgid, syscall.SIGCONT)
	
	return jobWaitFg(j)
}

func builtinBg(args []string) int {
	if len(sh.Jobs) == 0 {
		fmt.Println("meowsh: bg: no current job")
		return 1
	}
	j := sh.Jobs[len(sh.Jobs)-1]
	
	j.State = JOB_RUNNING
	j.Foreground = false
	
	// Continue job
	syscall.Kill(-j.Pgid, syscall.SIGCONT)
	
	return 0
}

use crate::shell::SHELL;
use crate::types::{Job, JobState, ProcState};

pub fn jobs_init() {}

pub fn job_update_status(pid: i32, status: i32) {
    let shell = SHELL.shell.lock().unwrap();
    let mut jobs = shell.jobs.lock().unwrap();

    for job in jobs.iter_mut() {
        for proc in job.procs.iter_mut() {
            if proc.pid == pid {
                proc.status = status;
                let stopped = libc::WIFSTOPPED(status);
                let exited = libc::WIFEXITED(status);
                let signaled = libc::WIFSIGNALED(status);

                if stopped {
                    proc.state = ProcState::Stopped;
                } else if exited || signaled {
                    proc.state = ProcState::Done;
                } else {
                    proc.state = ProcState::Running;
                }
                let all_done = job.procs.iter().all(|p| p.state == ProcState::Done);
                let any_stopped = job.procs.iter().any(|p| p.state == ProcState::Stopped);

                if all_done {
                    job.state = JobState::Done;
                } else if any_stopped {
                    job.state = JobState::Stopped;
                } else {
                    job.state = JobState::Running;
                }
                return;
            }
        }
    }
}

pub fn job_wait_foreground(job: &Job) -> i32 {
    loop {
        let mut status: i32 = 0;
        let pid = unsafe { libc::waitpid(-job.pgid, &mut status, libc::WUNTRACED) };

        if pid < 0 {
            break;
        }

        job_update_status(pid, status);

        if libc::WIFSTOPPED(status) {
            println!("\n[{}]+  Stopped\t{}", job.id, job.cmd_text);
            let shell = SHELL.shell.lock().unwrap();
            let mut jobs = shell.jobs.lock().unwrap();
            if let Some(j) = jobs.iter_mut().find(|j| j.id == job.id) {
                j.state = JobState::Stopped;
                j.notified = true;
            }
            break;
        }

        let job_state = {
            let shell = SHELL.shell.lock().unwrap();
            let jobs = shell.jobs.lock().unwrap();
            jobs.iter()
                .find(|j| j.id == job.id)
                .map(|j| j.state)
                .unwrap_or(JobState::Done)
        };
        if job_state != JobState::Running {
            break;
        }
    }

    let status = if let Some(proc) = job.procs.last() {
        let ws = proc.status;
        if libc::WIFEXITED(ws) {
            libc::WEXITSTATUS(ws)
        } else if libc::WIFSIGNALED(ws) {
            128 + libc::WTERMSIG(ws) as i32
        } else {
            0
        }
    } else {
        0
    };

    unsafe {
        let shell_pgid = SHELL.shell.lock().unwrap().shell_pid;
        let _ = libc::tcsetpgrp(0, shell_pgid);
    }

    status
}

pub fn builtin_jobs(_args: &[String]) -> i32 {
    let shell = SHELL.shell.lock().unwrap();
    let jobs = shell.jobs.lock().unwrap();

    for job in jobs.iter() {
        let state_str = match job.state {
            JobState::Running => "Running",
            JobState::Stopped => "Stopped",
            JobState::Done => "Done",
        };
        println!("[{}]+  {}\t\t{}", job.id, state_str, job.cmd_text);
    }
    0
}

pub fn builtin_fg(_args: &[String]) -> i32 {
    let shell = SHELL.shell.lock().unwrap();
    let mut jobs = shell.jobs.lock().unwrap();

    if jobs.is_empty() {
        eprintln!("meowsh: fg: no current job");
        return 1;
    }

    let job = jobs.last_mut().unwrap();
    println!("{}", job.cmd_text);
    job.state = JobState::Running;
    job.foreground = true;

    let pgid = job.pgid;
    let job_id = job.id;
    let cmd_text = job.cmd_text.clone();

    drop(jobs);
    drop(shell);

    unsafe { libc::kill(-pgid, libc::SIGCONT) };

    loop {
        let mut status: i32 = 0;
        let pid = unsafe { libc::waitpid(-pgid, &mut status, libc::WUNTRACED) };
        if pid < 0 {
            break;
        }
        job_update_status(pid, status);
        if libc::WIFSTOPPED(status) {
            println!("\n[{}]+  Stopped\t{}", job_id, cmd_text);
            break;
        }
    }

    0
}

pub fn builtin_bg(_args: &[String]) -> i32 {
    let shell = SHELL.shell.lock().unwrap();
    let mut jobs = shell.jobs.lock().unwrap();

    if jobs.is_empty() {
        eprintln!("meowsh: bg: no current job");
        return 1;
    }

    let job = jobs.last_mut().unwrap();
    job.state = JobState::Running;
    job.foreground = false;

    let pgid = job.pgid;

    drop(jobs);
    drop(shell);

    unsafe { libc::kill(-pgid, libc::SIGCONT) };

    0
}

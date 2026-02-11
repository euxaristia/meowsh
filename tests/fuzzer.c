#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>
#include <fcntl.h>

char *commands[] = {
    "ls", "echo", "pwd", "cd", "export", "alias", "meow", "exit",
    "if true; then echo hi; fi",
    "while false; do echo loop; done",
    "(echo subshell)",
    "{ echo brace; }",
    "ls | grep .",
    "echo $HOME",
    "export FOO=BAR",
    "alias cat='cat -n'",
    "ls > /dev/null",
    "cat <<EOF\nhello\nEOF",
    "ls && echo ok || echo fail",
    "$(echo echo cmdsub)",
};

char *special_chars = "|&;()<> \t\n$\"'\\`#*?[]!{}";

void generate_random_cmd(char *buf, int max_len) {
    int len = rand() % max_len;
    for (int i = 0; i < len; i++) {
        if (rand() % 5 == 0) {
            buf[i] = special_chars[rand() % strlen(special_chars)];
        } else {
            buf[i] = (rand() % 94) + 32;
        }
    }
    buf[len] = '\0';
}

void run_fuzz_iteration(int iter) {
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        exit(1);
    }

    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[1]);
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);

        int devnull = open("/dev/null", O_WRONLY);
        if (devnull != -1) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        char *args[] = {"./meowsh", NULL};
        execv(args[0], args);
        exit(1);
    } else {
        close(pipefd[0]);
        
        char buf[8192];
        if (iter % 10 == 0) {
            int parts = (rand() % 5) + 1;
            buf[0] = '\0';
            for(int p=0; p<parts; p++) {
                strcat(buf, commands[rand() % (sizeof(commands)/sizeof(char*))]);
                strcat(buf, " ");
            }
        } else {
            generate_random_cmd(buf, 1024);
        }
        strcat(buf, "\nexit\n");

        if (write(pipefd[1], buf, strlen(buf)) < 0) {
            // ignore
        }
        close(pipefd[1]);

        int status;
        waitpid(pid, &status, 0);
        if (WIFSIGNALED(status)) {
            printf("\n[!] CRASH DETECTED at iteration %d\n", iter);
            printf("[!] Input was: \n---\n%s\n---\n", buf);
            printf("[!] Signal: %d\n", WTERMSIG(status));
            exit(1);
        }
    }
}

int main(int argc, char **argv) {
    int iterations = 1000;
    if (argc > 1) iterations = atoi(argv[1]);

    srand(time(NULL));
    printf("Starting heavy fuzzing on ./meowsh for %d iterations...\n", iterations);

    for (int i = 0; i < iterations; i++) {
        if (i % 100 == 0) {
            printf("Iteration %d...\n", i);
            fflush(stdout);
        }
        run_fuzz_iteration(i);
    }

    printf("Fuzzing complete. No crashes detected.\n");
    return 0;
}
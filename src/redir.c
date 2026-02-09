/*
 * meowsh — POSIX-compliant shell
 * redir.c — I/O redirection setup/teardown
 */

#define _POSIX_C_SOURCE 200809L

#include "shell.h"
#include "redir.h"
#include "expand.h"
#include "error.h"
#include "memalloc.h"
#include "compat.h"

#include <fcntl.h>
#include <string.h>

static struct saved_fd *
save_fd(int fd, struct saved_fd *list)
{
	struct saved_fd *sf;
	int saved;

	saved = fcntl(fd, F_DUPFD, 10);
	if (saved < 0)
		saved = -1;
	else
		sh_cloexec(saved, 1);

	sf = sh_malloc(sizeof(*sf));
	sf->orig_fd = fd;
	sf->saved_fd = saved;
	sf->next = list;
	return sf;
}

struct saved_fd *
redir_apply(struct redirect *redirs)
{
	struct redirect *r;
	struct saved_fd *saved = NULL;
	int fd, newfd;
	char *filename;

	for (r = redirs; r; r = r->next) {
		fd = r->fd;

		/* Save the original fd */
		saved = save_fd(fd, saved);

		switch (r->type) {
		case REDIR_INPUT:
			filename = expand_word(r->filename, 0);
			newfd = open(filename, O_RDONLY);
			if (newfd < 0) {
				sh_errorf("%s", filename);
				free(filename);
				goto error;
			}
			free(filename);
			if (newfd != fd) {
				if (dup2(newfd, fd) < 0) {
					close(newfd);
					goto error;
				}
				close(newfd);
			}
			break;

		case REDIR_OUTPUT:
			filename = expand_word(r->filename, 0);
			{
				int flags = O_WRONLY | O_CREAT | O_TRUNC;
				if (option_is_set(OPT_NOCLOBBER))
					flags = O_WRONLY | O_CREAT | O_EXCL;
				newfd = open(filename, flags, 0666);
			}
			if (newfd < 0) {
				sh_errorf("%s", filename);
				free(filename);
				goto error;
			}
			free(filename);
			if (newfd != fd) {
				dup2(newfd, fd);
				close(newfd);
			}
			break;

		case REDIR_CLOBBER:
			filename = expand_word(r->filename, 0);
			newfd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
			if (newfd < 0) {
				sh_errorf("%s", filename);
				free(filename);
				goto error;
			}
			free(filename);
			if (newfd != fd) {
				dup2(newfd, fd);
				close(newfd);
			}
			break;

		case REDIR_APPEND:
			filename = expand_word(r->filename, 0);
			newfd = open(filename,
			    O_WRONLY | O_CREAT | O_APPEND, 0666);
			if (newfd < 0) {
				sh_errorf("%s", filename);
				free(filename);
				goto error;
			}
			free(filename);
			if (newfd != fd) {
				dup2(newfd, fd);
				close(newfd);
			}
			break;

		case REDIR_RDWR:
			filename = expand_word(r->filename, 0);
			newfd = open(filename, O_RDWR | O_CREAT, 0666);
			if (newfd < 0) {
				sh_errorf("%s", filename);
				free(filename);
				goto error;
			}
			free(filename);
			if (newfd != fd) {
				dup2(newfd, fd);
				close(newfd);
			}
			break;

		case REDIR_DUP_INPUT:
		case REDIR_DUP_OUTPUT:
			filename = expand_word(r->filename, 0);
			if (strcmp(filename, "-") == 0) {
				close(fd);
			} else {
				newfd = atoi(filename);
				if (dup2(newfd, fd) < 0) {
					sh_errorf("dup2");
					free(filename);
					goto error;
				}
			}
			free(filename);
			break;

		case REDIR_HEREDOC:
		case REDIR_HEREDOC_STRIP:
			{
				char *body;
				int pfd[2];

				if (r->heredoc_quoted)
					body = sh_strdup(r->heredoc_body ?
					    r->heredoc_body : "");
				else
					body = expand_heredoc(r->heredoc_body ?
					    r->heredoc_body : "");

				if (pipe(pfd) < 0) {
					free(body);
					goto error;
				}
				{
					size_t len = strlen(body);
					ssize_t nw = write(pfd[1], body, len);
					(void)nw;
				}
				close(pfd[1]);
				free(body);

				if (dup2(pfd[0], fd) < 0) {
					close(pfd[0]);
					goto error;
				}
				close(pfd[0]);
			}
			break;
		}
	}

	return saved;

error:
	redir_restore(saved);
	sh.last_status = 1;
	return NULL;
}

void
redir_restore(struct saved_fd *saved)
{
	struct saved_fd *sf, *next;

	for (sf = saved; sf; sf = next) {
		next = sf->next;
		if (sf->saved_fd >= 0) {
			dup2(sf->saved_fd, sf->orig_fd);
			close(sf->saved_fd);
		} else {
			close(sf->orig_fd);
		}
		free(sf);
	}
}

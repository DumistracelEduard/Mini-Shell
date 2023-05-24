// SPDX-License-Identifier: BSD-3-Clause
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <fcntl.h>
#include <unistd.h>

#include "cmd.h"
#include "utils.h"

#define READ 0
#define WRITE 1

/**
 * Internal change-directory command.
 */
static bool shell_cd(word_t *dir)
{
	if (dir == NULL)
		return -1;

	char *path = get_word(dir);

	int value = chdir(path);

	free(path);

	return value;
}

/**
 * Internal exit/quit command.
 */
static int shell_exit(void)
{
	exit(0);
	return SHELL_EXIT;
}

/**
 * Parse a simple command (internal, environment variable assignment,
 * external command).
 */
static int parse_simple(simple_command_t *s, int level, command_t *father)
{
	if (s == NULL || s->verb == NULL)
		return 0;

	char *command = get_word(s->verb);

	if (strcmp(command, "cd") == 0) {
		char *filename = get_word(s->out);

		int output_fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);

		close(output_fd);

		int data = shell_cd(s->params);

		free(filename);
		free(command);

		return data;
	} else if (strcmp(command, "exit") == 0 || strcmp(command, "quit") == 0) {
		free(command);
		return shell_exit();
	}

	if (strchr(command, '=') != NULL) {
		char *variable = strtok(command, "=");
		char *value = strtok(NULL, "=");

		free(command);
		return setenv(variable, value, 1);
	}

	pid_t pid = fork();
	int value = 0;

	if (pid == 0) {
		if (s->in) { // < input
			char *file_in = get_word(s->in);

			int input_fd = open(file_in, O_RDONLY);

			dup2(input_fd, STDIN_FILENO);
			close(input_fd);

			free(file_in);
		}
		if (s->out && (s->io_flags & IO_OUT_APPEND)) { // >> filename
			char *file_out = get_word(s->out);

			int output_fd = open(file_out, O_WRONLY | O_CREAT | O_APPEND, 0644);

			dup2(output_fd, STDOUT_FILENO);
			close(output_fd);

			free(file_out);
		} else if (s->err && (s->io_flags & IO_ERR_APPEND)) { // 2>> filename
			char *file_err = get_word(s->err);

			int error_fd = open(file_err, O_WRONLY | O_CREAT | O_APPEND, 0644);

			dup2(error_fd, STDERR_FILENO);
			close(error_fd);

			free(file_err);
		} else if (s->out && s->err && s->out->string == s->err->string) { // &> filename
			char *file_out = get_word(s->out);

			int output_fd = open(file_out, O_WRONLY | O_CREAT | O_TRUNC, 0644);

			dup2(output_fd, STDOUT_FILENO);
			dup2(output_fd, STDERR_FILENO);
			close(output_fd);

			free(file_out);
		} else if (s->out && s->err && s->out->string != s->err->string) { // &> filename
			char *file_out = get_word(s->out);

			int output_fd = open(file_out, O_WRONLY | O_CREAT | O_TRUNC, 0644);

			dup2(output_fd, STDOUT_FILENO);
			free(file_out);

			file_out = get_word(s->err);
			output_fd = open(file_out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			dup2(output_fd, STDERR_FILENO);

			close(output_fd);
			free(file_out);
		} else if (s->out && !(s->io_flags & IO_OUT_APPEND) && !s->err) { // > filename
			char *file_out = get_word(s->out);

			int output_fd = open(file_out, O_WRONLY | O_CREAT | O_TRUNC, 0644);

			dup2(output_fd, STDOUT_FILENO);
			close(output_fd);

			free(file_out);
		} else if (s->err && !(s->io_flags & IO_ERR_APPEND) && !s->out) { // 2> filename
			char *file_err = get_word(s->err);

			int error_fd = open(file_err, O_WRONLY | O_CREAT | O_TRUNC, 0644);

			dup2(error_fd, STDERR_FILENO);
			close(error_fd);

			free(file_err);
		}

		char **data = get_argv(s, &value);

		execvp(command, data);
		for (int i = 0; i < value; i++)
			free(data[i]);

		free(data);
	} else {
		int status;

		waitpid(pid, &status, 0);

		free(command);
		if (WIFEXITED(status))
			return WEXITSTATUS(status);
	}
	free(command);
	return value;
}

/**
 * Process two commands in parallel, by creating two children.
 */
static bool run_in_parallel(command_t *cmd1, command_t *cmd2, int level,
							command_t *father)
{
	pid_t pid = fork();
	int value = 0;

	if (pid == 0) {
		value = parse_command(cmd1, level + 1, father);
	} else {
		int status;

		value = parse_command(cmd2, level + 1, father);
		waitpid(pid, &status, 0);

		if (WIFEXITED(status))
			return WEXITSTATUS(status);
	}
	return value;
}

/**
 * Run commands by creating an anonymous pipe (cmd1 | cmd2).
 */
static bool run_on_pipe(command_t *cmd1, command_t *cmd2, int level,
						command_t *father)
{
	/* TODO: Redirect the output of cmd1 to the input of cmd2. */
	int fd[2], ret;

	pipe(fd);

	pid_t pid = fork();

	if (pid == 0) {
		close(fd[0]);
		ret = dup2(fd[1], STDOUT_FILENO);

		exit(parse_command(cmd1, level + 1, father));
	}

	pid_t pid_2 = fork();

	if (pid_2 == 0) {
		close(fd[1]);
		ret = dup2(fd[0], STDIN_FILENO);

		exit(parse_command(cmd2, level + 1, father));
	}
	close(fd[0]);
	close(fd[1]);

	int status;

	waitpid(pid, &status, 0);
	ret = waitpid(pid_2, &status, 0);

	if (WIFEXITED(status))
		return WEXITSTATUS(status);

	return ret; /* TODO: Replace with actual exit status. */
}

/**
 * Parse and execute a command.
 */
int parse_command(command_t *c, int level, command_t *father)
{
	int ret = 0;

	if (c == NULL)
		return -1;

	if (c->op == OP_NONE)
		return parse_simple(c->scmd, level, father);

	switch (c->op) {
	case OP_SEQUENTIAL:
		ret = parse_command(c->cmd1, level + 1, c);
		ret = parse_command(c->cmd2, level + 1, c);
		break;

	case OP_PARALLEL:
		ret = run_in_parallel(c->cmd1, c->cmd2, level + 1, c);
		break;

	case OP_CONDITIONAL_NZERO:
		ret = parse_command(c->cmd1, level + 1, c);

		if (ret != 0)
			ret = parse_command(c->cmd2, level + 1, c);
		break;

	case OP_CONDITIONAL_ZERO:
		ret = parse_command(c->cmd1, level + 1, c);

		if (ret == 0)
			ret = parse_command(c->cmd2, level + 1, c);
		break;

	case OP_PIPE:
		ret = run_on_pipe(c->cmd1, c->cmd2, level + 1, c);
		break;

	default:
		return SHELL_EXIT;
	}

	return ret;
}

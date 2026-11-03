#include "log_viewer/process.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

long process_seconds(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return now.tv_sec;
}

bool process_spawn(pid_t *pid, char *const argv[], int output, int diagnostic) {
  posix_spawn_file_actions_t actions;
  int error = posix_spawn_file_actions_init(&actions);
  if (error) {
    errno = error;
    return false;
  }

  error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  if (!error) error = posix_spawn_file_actions_adddup2(&actions, output, STDOUT_FILENO);
  if (!error) error = posix_spawn_file_actions_adddup2(&actions, diagnostic, STDERR_FILENO);
  if (!error) error = posix_spawnp(pid, argv[0], &actions, nullptr, argv, environ);
  posix_spawn_file_actions_destroy(&actions);
  errno = error;
  return !error;
}

static int temporary(void) {
  char path[] = "/tmp/log-viewer-command-XXXXXX";
  int fd = mkstemp(path);
  if (fd >= 0) {
    unlink(path);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
  }
  return fd;
}

bool process_capture(ManagedProcess *process, char *const argv[]) {
  *process = (ManagedProcess){ .output = temporary(), .diagnostic = temporary(), .started = process_seconds() };
  if (process->output >= 0 && process->diagnostic >= 0 && process_spawn(&process->pid, argv, process->output, process->diagnostic)) return true;

  process_close(process);
  return false;
}

int process_poll(ManagedProcess *process) {
  int status;
  pid_t result = waitpid(process->pid, &status, WNOHANG);
  if (!result && process_seconds() - process->started < 10) return 0;
  if (!result) {
    kill(process->pid, SIGKILL);
    while (waitpid(process->pid, &status, 0) < 0 && errno == EINTR) {}
    process->pid = 0;
    return -1;
  }
  if (result < 0 && errno == EINTR) return 0;

  process->pid = 0;
  return result > 0 && WIFEXITED(status) && !WEXITSTATUS(status) ? 1 : -1;
}

char *process_output(int fd) {
  struct stat identity;
  if (fstat(fd, &identity) < 0 || identity.st_size > 16 * 1024 * 1024) return nullptr;

  size_t length = (size_t)identity.st_size;
  char *text = calloc(length + 1, 1);
  if (!text) return nullptr;

  size_t read_bytes = 0;
  while (read_bytes < length) {
    ssize_t n = pread(fd, text + read_bytes, length - read_bytes, (off_t)read_bytes);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      free(text);
      return nullptr;
    }
    read_bytes += (size_t)n;
  }
  return text;
}

void process_close(ManagedProcess *process) {
  if (process->pid > 0) {
    kill(process->pid, SIGKILL);
    while (waitpid(process->pid, nullptr, 0) < 0 && errno == EINTR) {}
  }
  if (process->output >= 0) close(process->output);
  if (process->diagnostic >= 0) close(process->diagnostic);
  *process = (ManagedProcess){ .output = -1, .diagnostic = -1 };
}

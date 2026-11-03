#ifndef LOG_VIEWER_PROCESS_H
#define LOG_VIEWER_PROCESS_H
#include <sys/types.h>
#include <stdbool.h>

typedef struct {
  pid_t pid;
  int output, diagnostic;
  long started;
} ManagedProcess;

bool process_spawn(pid_t *pid, char *const argv[], int output, int diagnostic);
bool process_capture(ManagedProcess *process, char *const argv[]);
/* 0 while running, 1 on success, -1 on failure or ten-second timeout. */
int process_poll(ManagedProcess *process);
char *process_output(int fd);
void process_close(ManagedProcess *process);
long process_seconds(void);
#endif

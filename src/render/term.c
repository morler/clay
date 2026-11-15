#include "clay/term.h"
#include "clay/color.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#ifdef _WIN32
/* MinGW exposes the CRT's secure rand_s declaration behind this switch. */
#define _CRT_RAND_S
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <conio.h>
#include <direct.h>
#include <io.h>
#include <aclapi.h>
#include <shellapi.h>
#include <sys/stat.h>
#include <windows.h>
#define CLAY_PATH_MAX MAX_PATH
#else
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#ifdef __linux__
#include <sys/xattr.h>
#endif
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#define CLAY_PATH_MAX 4096
#endif

#define CLAY_TERM_NOTIFICATION_LIMIT 512

struct ClayTermHttpServer {
#ifdef _WIN32
  SOCKET listener;
  SOCKET peer;
#else
  int listener;
  int peer;
#endif
  unsigned short port;
};

static volatile sig_atomic_t g_interrupted = 0;
static volatile sig_atomic_t g_resized = 0;
static int g_pending_escape = 0;
static int g_pending_escape_action = 0;
static int g_pending_think_toggle = 0;
static int g_noninteractive = 0;

#ifndef _WIN32
static void handle_sigint(int signal_number) {
  (void)signal_number;
  g_interrupted = 1;
}

static void handle_sigwinch(int signal_number) {
  (void)signal_number;
  g_resized = 1;
}
#endif

void clay_term_init(void) {
#ifdef _WIN32
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD mode = 0;
  if (GetConsoleMode(out, &mode)) {
    /* The UI strings are UTF-8. cmd.exe commonly starts with an OEM
       code page, which renders each UTF-8 byte as a separate glyph. */
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
  }
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
  if (GetConsoleMode(in, &mode))
    SetConsoleCP(CP_UTF8);
#else
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = handle_sigint;
  sigemptyset(&action.sa_mask);
  sigaction(SIGINT, &action, NULL);

  struct sigaction winch_action;
  memset(&winch_action, 0, sizeof(winch_action));
  winch_action.sa_handler = handle_sigwinch;
  sigemptyset(&winch_action.sa_mask);
  sigaction(SIGWINCH, &winch_action, NULL);
#endif
}

void clay_term_hide_cursor(void) { fputs("\x1b[?25l", stdout); }
void clay_term_show_cursor(void) { fputs("\x1b[?25h", stdout); }
void clay_term_clear_line(void) { fputs("\r\x1b[2K", stdout); }

void clay_term_cursor_up(int n) {
  if (n > 0)
    printf("\x1b[%dA", n);
}

void clay_term_cursor_down(int n) {
  if (n > 0)
    printf("\x1b[%dB", n);
}

void clay_term_cursor_save(void) { fputs("\x1b" "7", stdout); }

void clay_term_cursor_restore(void) { fputs("\x1b" "8", stdout); }

void clay_term_cursor_col(int col) { printf("\x1b[%dG", col + 1); }

void clay_term_row_enter(int row, int *established) {
  if (row < *established) {
    clay_term_cursor_down(1);
  } else {
    fputc('\n', stdout);
    *established = row + 1;
  }
  fputc('\r', stdout);
}

void clay_term_sleep_ms(int ms) {
#ifdef _WIN32
  Sleep((DWORD)ms);
#else
  usleep((unsigned int)ms * 1000);
#endif
}

static void clay_term_notification_text(ClayStr *out, const char *text) {
  if (!text) return;
  for (size_t i = 0; text[i] && out->len < CLAY_TERM_NOTIFICATION_LIMIT; i++) {
    unsigned char ch = (unsigned char)text[i];
    if (ch < 0x20 || ch == 0x7f || ch == ';')
      clay_str_push_char(out, ' ');
    else
      clay_str_push_char(out, (char)ch);
  }
}

#if !defined(_WIN32) && defined(__APPLE__)
static void clay_term_applescript_quote(ClayStr *out, const char *text) {
  clay_str_push_char(out, '"');
  for (const char *p = text; *p; p++) {
    if (*p == '\\' || *p == '"') clay_str_push_char(out, '\\');
    clay_str_push_char(out, *p);
  }
  clay_str_push_char(out, '"');
}
#endif

#ifndef _WIN32
static void clay_term_spawn_notification(const char *title, const char *message) {
  pid_t child = fork();
  if (child < 0) return;
  if (child == 0) {
    pid_t detached = fork();
    if (detached > 0) _exit(0);
    if (detached < 0) _exit(0);
#ifdef __APPLE__
    ClayStr script;
    clay_str_init(&script);
    clay_str_push(&script, "display notification ");
    clay_term_applescript_quote(&script, message);
    clay_str_push(&script, " with title ");
    clay_term_applescript_quote(&script, title);
    execlp("osascript", "osascript", "-e", script.data, (char *)NULL);
    clay_str_free(&script);
#else
    execlp("notify-send", "notify-send", "--app-name", title, message,
           (char *)NULL);
#endif
    _exit(0);
  }
  waitpid(child, NULL, 0);
}
#endif

void clay_term_notify(const char *title, const char *message) {
  if (!clay_term_is_interactive()) return;

  ClayStr safe_title;
  ClayStr safe_message;
  clay_str_init(&safe_title);
  clay_str_init(&safe_message);
  clay_term_notification_text(&safe_title, title ? title : "Clay");
  clay_term_notification_text(&safe_message, message ? message : "Attention required");

  fputc('\a', stdout);
  printf("\x1b]9;%s: %s\a", safe_title.data, safe_message.data);
  fflush(stdout);
#ifndef _WIN32
  clay_term_spawn_notification(safe_title.data, safe_message.data);
#endif
  clay_str_free(&safe_title);
  clay_str_free(&safe_message);
}

/* Shared by both platform paths below: resolves the caller's timeout and
   hands each chunk to the streaming hook before it is buffered. */
static int exec_timeout_seconds(const ClayExecOptions *options) {
  int seconds = options ? options->timeout_seconds : 0;
  if (seconds <= 0) seconds = CLAY_SHELL_DEFAULT_TIMEOUT_SECONDS;
  if (seconds > CLAY_SHELL_MAX_TIMEOUT_SECONDS)
    seconds = CLAY_SHELL_MAX_TIMEOUT_SECONDS;
  return seconds;
}

static int exec_should_stop(const ClayExecOptions *options) {
  return options && options->should_stop &&
         options->should_stop(options->user_data);
}

static void exec_stream(const ClayExecOptions *options, const char *data,
                        size_t len) {
  if (options && options->on_output)
    options->on_output(data, len, options->user_data);
}

char *clay_term_display_cwd(void) {
  char *cwd = clay_term_cwd();
  if (!cwd) return strdup(".");
  const char *home = getenv("HOME");
#ifdef _WIN32
  if (!home || !*home) home = getenv("USERPROFILE");
#endif
  size_t length = home ? strlen(home) : 0;
  if (length && strncmp(cwd, home, length) == 0 &&
      (cwd[length] == '\0' || cwd[length] == '/' || cwd[length] == '\\')) {
    ClayStr display;
    clay_str_init(&display);
    clay_str_push_char(&display, '~');
    clay_str_push(&display, cwd + length);
    free(cwd);
    return display.data;
  }
  return cwd;
}

int clay_term_shell_exec(const char *command, ClayStr *output,
                         size_t output_limit, const ClayExecOptions *options,
                         ClayExecResult *result) {
  result->exit_code = -1;
  result->output_truncated = 0;
  result->timed_out = 0;
  result->stopped = 0;
  int timeout_seconds = exec_timeout_seconds(options);
#ifdef _WIN32
  SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
  HANDLE read_handle = NULL, write_handle = NULL;
  if (!CreatePipe(&read_handle, &write_handle, &security, 0) ||
      !SetHandleInformation(read_handle, HANDLE_FLAG_INHERIT, 0)) {
    if (read_handle) CloseHandle(read_handle);
    if (write_handle) CloseHandle(write_handle);
    return -1;
  }

  ClayStr invocation;
  clay_str_init(&invocation);
  clay_str_printf(&invocation, "cmd.exe /d /s /c \"%s\"", command);
  STARTUPINFOA startup = {0};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = write_handle;
  startup.hStdError = write_handle;
  PROCESS_INFORMATION process = {0};
  BOOL started = CreateProcessA(NULL, invocation.data, NULL, NULL, TRUE, 0,
                                NULL, NULL, &startup, &process);
  clay_str_free(&invocation);
  CloseHandle(write_handle);
  if (!started) {
    CloseHandle(read_handle);
    return -1;
  }

  /* Killing only cmd.exe leaves grandchildren (and their inherited pipe
     handle) alive. A kill-on-close job gives this one-shot command the same
     process-group semantics as the POSIX implementation below. */
  HANDLE job = CreateJobObjectA(NULL, NULL);
  if (job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits)) ||
        !AssignProcessToJobObject(job, process.hProcess)) {
      CloseHandle(job);
      job = NULL;
    }
  }

  int truncated = 0;
  int timed_out = 0;
  int stopped = 0;
  ULONGLONG deadline = GetTickCount64() + (ULONGLONG)timeout_seconds * 1000;
  for (;;) {
    DWORD available = 0;
    if (!PeekNamedPipe(read_handle, NULL, 0, NULL, &available, NULL)) break;
    while (available > 0) {
      char buffer[4096];
      DWORD requested = available < sizeof(buffer) ? available : sizeof(buffer);
      DWORD count = 0;
      if (!ReadFile(read_handle, buffer, requested, &count, NULL) || count == 0)
        break;
      for (DWORD i = 0; i < count; i++) {
        if (buffer[i] == '\0') buffer[i] = '?';
      }
      exec_stream(options, buffer, (size_t)count);
      size_t kept = output->len < output_limit ? output_limit - output->len : 0;
      if ((size_t)count > kept) truncated = 1;
      if (kept > (size_t)count) kept = count;
      if (kept) clay_str_push_n(output, buffer, kept);
      available -= count;
    }

    DWORD state = WaitForSingleObject(process.hProcess, 50);
    if (state == WAIT_OBJECT_0) break;
    if (state == WAIT_FAILED) {
      if (job) TerminateJobObject(job, 1);
      else TerminateProcess(process.hProcess, 1);
      WaitForSingleObject(process.hProcess, INFINITE);
      CloseHandle(read_handle);
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
      if (job) CloseHandle(job);
      return -1;
    }
    if (exec_should_stop(options)) {
      stopped = 1;
      if (job) TerminateJobObject(job, 143);
      else TerminateProcess(process.hProcess, 143);
      WaitForSingleObject(process.hProcess, INFINITE);
      break;
    }
    if (GetTickCount64() >= deadline) {
      timed_out = 1;
      if (job) TerminateJobObject(job, 124);
      else TerminateProcess(process.hProcess, 124);
      WaitForSingleObject(process.hProcess, INFINITE);
      break;
    }
  }

  /* Drain output after process termination. The job has closed inherited
     writers, so PeekNamedPipe can no longer leave us blocked indefinitely. */
  for (;;) {
    DWORD available = 0;
    if (!PeekNamedPipe(read_handle, NULL, 0, NULL, &available, NULL) || !available)
      break;
    char buffer[4096];
    DWORD requested = available < sizeof(buffer) ? available : sizeof(buffer);
    DWORD count = 0;
    if (!ReadFile(read_handle, buffer, requested, &count, NULL) || count == 0)
      break;
    for (DWORD i = 0; i < count; i++) if (buffer[i] == '\0') buffer[i] = '?';
    exec_stream(options, buffer, (size_t)count);
    size_t kept = output->len < output_limit ? output_limit - output->len : 0;
    if ((size_t)count > kept) truncated = 1;
    if (kept > (size_t)count) kept = count;
    if (kept) clay_str_push_n(output, buffer, kept);
  }
  DWORD status = 1;
  GetExitCodeProcess(process.hProcess, &status);
  CloseHandle(read_handle);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  if (job) CloseHandle(job);
  if (timed_out) {
    static const char message[] = "\nclay: command timed out\n";
    size_t kept = output->len < output_limit ? output_limit - output->len : 0;
    if (kept > sizeof(message) - 1) kept = sizeof(message) - 1;
    if (kept < sizeof(message) - 1) truncated = 1;
    if (kept) clay_str_push_n(output, message, kept);
  }
  result->exit_code = timed_out ? 124 : (int)status;
  result->output_truncated = truncated;
  result->timed_out = timed_out;
  result->stopped = stopped;
  return 0;
#else
  int pipe_fds[2];
  if (pipe(pipe_fds) != 0) return -1;
  /* Anything still sitting in stdout's buffer would be flushed into the
     child's pipe and come back as command output. */
  fflush(NULL);
  pid_t pid = fork();
  if (pid < 0) {
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return -1;
  }
  if (pid == 0) {
    setpgid(0, 0);
    dup2(pipe_fds[1], STDOUT_FILENO);
    dup2(pipe_fds[1], STDERR_FILENO);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    execl("/bin/sh", "sh", "-c", command, (char *)NULL);
    _exit(127);
  }
  close(pipe_fds[1]);
  setpgid(pid, pid);
  int flags = fcntl(pipe_fds[0], F_GETFL);
  if (flags < 0 || fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK) != 0) {
    close(pipe_fds[0]);
    kill(-pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
  }

  int truncated = 0, timed_out = 0, stopped = 0, killed = 0, child_done = 0,
      pipe_open = 1;
  int status = 0;
  long long stopped_at = 0;
  long long drain_since = 0;
  struct timespec started;
  clock_gettime(CLOCK_MONOTONIC, &started);
  while (pipe_open || !child_done) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long elapsed = (long long)(now.tv_sec - started.tv_sec) * 1000 +
                        (now.tv_nsec - started.tv_nsec) / 1000000;
    if (!child_done && waitpid(pid, &status, WNOHANG) == pid) {
      child_done = 1;
      drain_since = elapsed;
    }
    if (!child_done && !stopped && exec_should_stop(options)) {
      kill(-pid, SIGTERM);
      stopped = 1;
      stopped_at = elapsed;
    }
    /* SIGTERM gives the command a chance to flush; SIGKILL is the backstop
       for one that ignores it, and for whatever it left running after it -
       once the shell is reaped there is nothing left to exit gracefully. */
    if (stopped && !killed &&
        (child_done || elapsed - stopped_at >= CLAY_SHELL_STOP_GRACE_MS)) {
      kill(-pid, SIGKILL);
      killed = 1;
    }
    if (!child_done && !timed_out &&
        elapsed >= (long long)timeout_seconds * 1000) {
      kill(-pid, SIGKILL);
      killed = 1;
      timed_out = 1;
      if (waitpid(pid, &status, 0) == pid) {
        child_done = 1;
        drain_since = elapsed;
      }
    }
    /* A background child of the command can hold the pipe open long after
       the command itself is gone: read what it has, then stop waiting on an
       EOF that may never come. */
    if (pipe_open && child_done && (!stopped || killed) &&
        elapsed - drain_since >= CLAY_SHELL_DRAIN_MS) {
      close(pipe_fds[0]);
      pipe_open = 0;
      continue;
    }
    struct pollfd descriptor = {pipe_fds[0], POLLIN | POLLHUP, 0};
    int polled = pipe_open ? poll(&descriptor, 1, 20) : poll(NULL, 0, 20);
    if (polled < 0) {
      if (errno == EINTR) continue;
      close(pipe_fds[0]);
      if (!child_done) kill(-pid, SIGKILL);
      waitpid(pid, NULL, 0);
      return -1;
    }
    if (pipe_open && (polled > 0 || child_done)) {
      for (;;) {
        char buffer[4096];
        ssize_t count = read(pipe_fds[0], buffer, sizeof(buffer));
        if (count > 0) {
          for (ssize_t i = 0; i < count; i++) if (buffer[i] == '\0') buffer[i] = '?';
          exec_stream(options, buffer, (size_t)count);
          size_t kept = output->len < output_limit ? output_limit - output->len : 0;
          if ((size_t)count > kept) truncated = 1;
          if (kept > (size_t)count) kept = count;
          if (kept) clay_str_push_n(output, buffer, kept);
          continue;
        }
        if (count == 0) {
          close(pipe_fds[0]);
          pipe_open = 0;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
          close(pipe_fds[0]);
          pipe_open = 0;
        }
        break;
      }
    }
  }
  if (timed_out) {
    static const char message[] = "\nclay: command timed out\n";
    size_t kept = output->len < output_limit ? output_limit - output->len : 0;
    if (kept > sizeof(message) - 1) kept = sizeof(message) - 1;
    if (kept < sizeof(message) - 1) truncated = 1;
    if (kept) clay_str_push_n(output, message, kept);
  }
  result->exit_code =
      timed_out ? 124 : (WIFEXITED(status) ? WEXITSTATUS(status) : -1);
  result->output_truncated = truncated;
  result->timed_out = timed_out;
  result->stopped = stopped;
  return 0;
#endif
}

void clay_term_shell_quote(ClayStr *out, const char *value) {
  clay_str_push_char(out, '\'');
  for (const char *p = value; *p; p++) {
    if (*p == '\'')
      clay_str_push(out, "'\\''");
    else
      clay_str_push_char(out, *p);
  }
  clay_str_push_char(out, '\'');
}

void clay_term_setenv(const char *name, const char *value) {
#ifdef _WIN32
  _putenv_s(name, value ? value : "");
#else
  if (value)
    setenv(name, value, 1);
  else
    unsetenv(name);
#endif
}

#ifdef _WIN32
struct ClayProcess {
  HANDLE process_handle;
  HANDLE write_handle;
  HANDLE read_handle;
  ClayStr read_buffer; /* bytes read past the last line boundary returned */
};

ClayProcess *clay_term_process_start(const char *command, char *const argv[]) {
  (void)command;
  SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
  HANDLE child_stdin_read, child_stdin_write;
  HANDLE child_stdout_read, child_stdout_write;
  if (!CreatePipe(&child_stdin_read, &child_stdin_write, &sa, 0))
    return NULL;
  if (!SetHandleInformation(child_stdin_write, HANDLE_FLAG_INHERIT, 0)) {
    CloseHandle(child_stdin_read);
    CloseHandle(child_stdin_write);
    return NULL;
  }
  if (!CreatePipe(&child_stdout_read, &child_stdout_write, &sa, 0)) {
    CloseHandle(child_stdin_read);
    CloseHandle(child_stdin_write);
    return NULL;
  }
  if (!SetHandleInformation(child_stdout_read, HANDLE_FLAG_INHERIT, 0)) {
    CloseHandle(child_stdin_read);
    CloseHandle(child_stdin_write);
    CloseHandle(child_stdout_read);
    CloseHandle(child_stdout_write);
    return NULL;
  }

  ClayStr cmdline;
  clay_str_init(&cmdline);
  for (int i = 0; argv[i]; i++) {
    if (i > 0)
      clay_str_push_char(&cmdline, ' ');
    clay_str_push_char(&cmdline, '"');
    for (const char *p = argv[i]; *p; p++) {
      if (*p == '"')
        clay_str_push(&cmdline, "\\\"");
      else
        clay_str_push_char(&cmdline, *p);
    }
    clay_str_push_char(&cmdline, '"');
  }

  STARTUPINFO si;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags |= STARTF_USESTDHANDLES;
  si.hStdInput = child_stdin_read;
  si.hStdOutput = child_stdout_write;
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION pi;
  ZeroMemory(&pi, sizeof(pi));

  BOOL started = CreateProcess(NULL, cmdline.data, NULL, NULL, TRUE, 0, NULL,
                               NULL, &si, &pi);
  clay_str_free(&cmdline);
  CloseHandle(child_stdin_read);
  CloseHandle(child_stdout_write);
  if (!started) {
    CloseHandle(child_stdin_write);
    CloseHandle(child_stdout_read);
    return NULL;
  }
  CloseHandle(pi.hThread);

  ClayProcess *process = malloc(sizeof(ClayProcess));
  process->process_handle = pi.hProcess;
  process->write_handle = child_stdin_write;
  process->read_handle = child_stdout_read;
  clay_str_init(&process->read_buffer);
  return process;
}

int clay_term_process_write(ClayProcess *process, const char *data,
                            size_t len) {
  size_t written = 0;
  while (written < len) {
    DWORD n = 0;
    if (!WriteFile(process->write_handle, data + written,
                   (DWORD)(len - written), &n, NULL))
      return -1;
    written += n;
  }
  return 0;
}

char *clay_term_process_read_line(ClayProcess *process) {
  for (;;) {
    for (size_t i = 0; i < process->read_buffer.len; i++) {
      if (process->read_buffer.data[i] != '\n')
        continue;
      size_t line_len = i;
      if (line_len > 0 && process->read_buffer.data[line_len - 1] == '\r')
        line_len--;
      char *line = malloc(line_len + 1);
      memcpy(line, process->read_buffer.data, line_len);
      line[line_len] = '\0';
      clay_str_remove_n(&process->read_buffer, 0, i + 1);
      return line;
    }
    char chunk[4096];
    DWORD n = 0;
    BOOL ok = ReadFile(process->read_handle, chunk, sizeof(chunk), &n, NULL);
    if (!ok || n == 0) {
      if (process->read_buffer.len == 0)
        return NULL;
      char *line = strdup(process->read_buffer.data);
      clay_str_clear(&process->read_buffer);
      return line;
    }
    clay_str_push_n(&process->read_buffer, chunk, n);
  }
}

void clay_term_process_stop(ClayProcess *process) {
  if (!process)
    return;
  CloseHandle(process->write_handle);
  TerminateProcess(process->process_handle, 0);
  WaitForSingleObject(process->process_handle, 2000);
  CloseHandle(process->read_handle);
  CloseHandle(process->process_handle);
  clay_str_free(&process->read_buffer);
  free(process);
}
#else
struct ClayProcess {
  pid_t pid;
  int write_fd;
  FILE *read_file;
};

ClayProcess *clay_term_process_start(const char *command, char *const argv[]) {
  int stdin_pipe[2];
  int stdout_pipe[2];
  if (pipe(stdin_pipe) != 0)
    return NULL;
  if (pipe(stdout_pipe) != 0) {
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    return NULL;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    return NULL;
  }
  if (pid == 0) {
    dup2(stdin_pipe[0], STDIN_FILENO);
    dup2(stdout_pipe[1], STDOUT_FILENO);
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    execvp(command, argv);
    _exit(127);
  }

  close(stdin_pipe[0]);
  close(stdout_pipe[1]);
  ClayProcess *process = malloc(sizeof(ClayProcess));
  process->pid = pid;
  process->write_fd = stdin_pipe[1];
  process->read_file = fdopen(stdout_pipe[0], "r");
  if (!process->read_file) {
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    free(process);
    return NULL;
  }
  return process;
}

int clay_term_process_write(ClayProcess *process, const char *data,
                            size_t len) {
  size_t written = 0;
  while (written < len) {
    ssize_t n = write(process->write_fd, data + written, len - written);
    if (n <= 0)
      return -1;
    written += (size_t)n;
  }
  return 0;
}

char *clay_term_process_read_line(ClayProcess *process) {
  char *line = NULL;
  size_t cap = 0;
  ssize_t n = getline(&line, &cap, process->read_file);
  if (n < 0) {
    free(line);
    return NULL;
  }
  while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
    line[--n] = '\0';
  return line;
}

void clay_term_process_stop(ClayProcess *process) {
  if (!process)
    return;
  close(process->write_fd);
  fclose(process->read_file);
  kill(process->pid, SIGTERM);
  int status = 0;
  waitpid(process->pid, &status, 0);
  free(process);
}
#endif

int clay_term_change_dir(const char *path) {
#ifdef _WIN32
  return _chdir(path);
#else
  return chdir(path);
#endif
}

int clay_term_width(void) {
#ifdef _WIN32
  CONSOLE_SCREEN_BUFFER_INFO info;
  if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
    return info.srWindow.Right - info.srWindow.Left + 1;
  }
  return 80;
#else
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
    return ws.ws_col;
  }
  return 80;
#endif
}

int clay_term_supports_color(void) {
#ifdef _WIN32
  return _isatty(_fileno(stdout));
#else
  if (!isatty(STDOUT_FILENO))
    return 0;
  const char *term = getenv("TERM");
  return term != NULL && strcmp(term, "dumb") != 0;
#endif
}

int clay_term_is_interactive(void) {
  if (g_noninteractive)
    return 0;
#ifdef _WIN32
  return _isatty(_fileno(stdin)) && _isatty(_fileno(stdout));
#else
  return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
#endif
}

void clay_term_set_noninteractive(int noninteractive) {
  g_noninteractive = noninteractive != 0;
}

static int g_color_enabled = 1;
static int g_color_initialized = 0;

static void ensure_color_initialized(void) {
  if (g_color_initialized)
    return;
  g_color_initialized = 1;
  if (getenv("NO_COLOR") != NULL)
    g_color_enabled = 0;
}

void clay_term_set_color_enabled(int enabled) {
  g_color_initialized = 1;
  g_color_enabled = enabled;
}

int clay_term_color_enabled(void) {
  ensure_color_initialized();
  return g_color_enabled;
}

const char *clay_color(const char *code) {
  return clay_term_color_enabled() ? code : "";
}

/* Unicode 17.0.0 tables; ambiguous-width characters remain single-column. */
struct width_range { uint32_t first, last; };
static const struct width_range zero_width[] = {
  {0xad, 0xad},
  {0x300, 0x36f},
  {0x483, 0x489},
  {0x591, 0x5bd},
  {0x5bf, 0x5bf},
  {0x5c1, 0x5c2},
  {0x5c4, 0x5c5},
  {0x5c7, 0x5c7},
  {0x600, 0x605},
  {0x610, 0x61a},
  {0x61c, 0x61c},
  {0x64b, 0x65f},
  {0x670, 0x670},
  {0x6d6, 0x6dd},
  {0x6df, 0x6e4},
  {0x6e7, 0x6e8},
  {0x6ea, 0x6ed},
  {0x70f, 0x70f},
  {0x711, 0x711},
  {0x730, 0x74a},
  {0x7a6, 0x7b0},
  {0x7eb, 0x7f3},
  {0x7fd, 0x7fd},
  {0x816, 0x819},
  {0x81b, 0x823},
  {0x825, 0x827},
  {0x829, 0x82d},
  {0x859, 0x85b},
  {0x890, 0x891},
  {0x897, 0x89f},
  {0x8ca, 0x902},
  {0x93a, 0x93a},
  {0x93c, 0x93c},
  {0x941, 0x948},
  {0x94d, 0x94d},
  {0x951, 0x957},
  {0x962, 0x963},
  {0x981, 0x981},
  {0x9bc, 0x9bc},
  {0x9c1, 0x9c4},
  {0x9cd, 0x9cd},
  {0x9e2, 0x9e3},
  {0x9fe, 0x9fe},
  {0xa01, 0xa02},
  {0xa3c, 0xa3c},
  {0xa41, 0xa42},
  {0xa47, 0xa48},
  {0xa4b, 0xa4d},
  {0xa51, 0xa51},
  {0xa70, 0xa71},
  {0xa75, 0xa75},
  {0xa81, 0xa82},
  {0xabc, 0xabc},
  {0xac1, 0xac5},
  {0xac7, 0xac8},
  {0xacd, 0xacd},
  {0xae2, 0xae3},
  {0xafa, 0xaff},
  {0xb01, 0xb01},
  {0xb3c, 0xb3c},
  {0xb3f, 0xb3f},
  {0xb41, 0xb44},
  {0xb4d, 0xb4d},
  {0xb55, 0xb56},
  {0xb62, 0xb63},
  {0xb82, 0xb82},
  {0xbc0, 0xbc0},
  {0xbcd, 0xbcd},
  {0xc00, 0xc00},
  {0xc04, 0xc04},
  {0xc3c, 0xc3c},
  {0xc3e, 0xc40},
  {0xc46, 0xc48},
  {0xc4a, 0xc4d},
  {0xc55, 0xc56},
  {0xc62, 0xc63},
  {0xc81, 0xc81},
  {0xcbc, 0xcbc},
  {0xcbf, 0xcbf},
  {0xcc6, 0xcc6},
  {0xccc, 0xccd},
  {0xce2, 0xce3},
  {0xd00, 0xd01},
  {0xd3b, 0xd3c},
  {0xd41, 0xd44},
  {0xd4d, 0xd4d},
  {0xd62, 0xd63},
  {0xd81, 0xd81},
  {0xdca, 0xdca},
  {0xdd2, 0xdd4},
  {0xdd6, 0xdd6},
  {0xe31, 0xe31},
  {0xe34, 0xe3a},
  {0xe47, 0xe4e},
  {0xeb1, 0xeb1},
  {0xeb4, 0xebc},
  {0xec8, 0xece},
  {0xf18, 0xf19},
  {0xf35, 0xf35},
  {0xf37, 0xf37},
  {0xf39, 0xf39},
  {0xf71, 0xf7e},
  {0xf80, 0xf84},
  {0xf86, 0xf87},
  {0xf8d, 0xf97},
  {0xf99, 0xfbc},
  {0xfc6, 0xfc6},
  {0x102d, 0x1030},
  {0x1032, 0x1037},
  {0x1039, 0x103a},
  {0x103d, 0x103e},
  {0x1058, 0x1059},
  {0x105e, 0x1060},
  {0x1071, 0x1074},
  {0x1082, 0x1082},
  {0x1085, 0x1086},
  {0x108d, 0x108d},
  {0x109d, 0x109d},
  {0x135d, 0x135f},
  {0x1712, 0x1714},
  {0x1732, 0x1733},
  {0x1752, 0x1753},
  {0x1772, 0x1773},
  {0x17b4, 0x17b5},
  {0x17b7, 0x17bd},
  {0x17c6, 0x17c6},
  {0x17c9, 0x17d3},
  {0x17dd, 0x17dd},
  {0x180b, 0x180f},
  {0x1885, 0x1886},
  {0x18a9, 0x18a9},
  {0x1920, 0x1922},
  {0x1927, 0x1928},
  {0x1932, 0x1932},
  {0x1939, 0x193b},
  {0x1a17, 0x1a18},
  {0x1a1b, 0x1a1b},
  {0x1a56, 0x1a56},
  {0x1a58, 0x1a5e},
  {0x1a60, 0x1a60},
  {0x1a62, 0x1a62},
  {0x1a65, 0x1a6c},
  {0x1a73, 0x1a7c},
  {0x1a7f, 0x1a7f},
  {0x1ab0, 0x1add},
  {0x1ae0, 0x1aeb},
  {0x1b00, 0x1b03},
  {0x1b34, 0x1b34},
  {0x1b36, 0x1b3a},
  {0x1b3c, 0x1b3c},
  {0x1b42, 0x1b42},
  {0x1b6b, 0x1b73},
  {0x1b80, 0x1b81},
  {0x1ba2, 0x1ba5},
  {0x1ba8, 0x1ba9},
  {0x1bab, 0x1bad},
  {0x1be6, 0x1be6},
  {0x1be8, 0x1be9},
  {0x1bed, 0x1bed},
  {0x1bef, 0x1bf1},
  {0x1c2c, 0x1c33},
  {0x1c36, 0x1c37},
  {0x1cd0, 0x1cd2},
  {0x1cd4, 0x1ce0},
  {0x1ce2, 0x1ce8},
  {0x1ced, 0x1ced},
  {0x1cf4, 0x1cf4},
  {0x1cf8, 0x1cf9},
  {0x1dc0, 0x1dff},
  {0x200b, 0x200f},
  {0x202a, 0x202e},
  {0x2060, 0x2064},
  {0x2066, 0x206f},
  {0x20d0, 0x20f0},
  {0x2cef, 0x2cf1},
  {0x2d7f, 0x2d7f},
  {0x2de0, 0x2dff},
  {0x302a, 0x302d},
  {0x3099, 0x309a},
  {0xa66f, 0xa672},
  {0xa674, 0xa67d},
  {0xa69e, 0xa69f},
  {0xa6f0, 0xa6f1},
  {0xa802, 0xa802},
  {0xa806, 0xa806},
  {0xa80b, 0xa80b},
  {0xa825, 0xa826},
  {0xa82c, 0xa82c},
  {0xa8c4, 0xa8c5},
  {0xa8e0, 0xa8f1},
  {0xa8ff, 0xa8ff},
  {0xa926, 0xa92d},
  {0xa947, 0xa951},
  {0xa980, 0xa982},
  {0xa9b3, 0xa9b3},
  {0xa9b6, 0xa9b9},
  {0xa9bc, 0xa9bd},
  {0xa9e5, 0xa9e5},
  {0xaa29, 0xaa2e},
  {0xaa31, 0xaa32},
  {0xaa35, 0xaa36},
  {0xaa43, 0xaa43},
  {0xaa4c, 0xaa4c},
  {0xaa7c, 0xaa7c},
  {0xaab0, 0xaab0},
  {0xaab2, 0xaab4},
  {0xaab7, 0xaab8},
  {0xaabe, 0xaabf},
  {0xaac1, 0xaac1},
  {0xaaec, 0xaaed},
  {0xaaf6, 0xaaf6},
  {0xabe5, 0xabe5},
  {0xabe8, 0xabe8},
  {0xabed, 0xabed},
  {0xfb1e, 0xfb1e},
  {0xfe00, 0xfe0f},
  {0xfe20, 0xfe2f},
  {0xfeff, 0xfeff},
  {0xfff9, 0xfffb},
  {0x101fd, 0x101fd},
  {0x102e0, 0x102e0},
  {0x10376, 0x1037a},
  {0x10a01, 0x10a03},
  {0x10a05, 0x10a06},
  {0x10a0c, 0x10a0f},
  {0x10a38, 0x10a3a},
  {0x10a3f, 0x10a3f},
  {0x10ae5, 0x10ae6},
  {0x10d24, 0x10d27},
  {0x10d69, 0x10d6d},
  {0x10eab, 0x10eac},
  {0x10efa, 0x10eff},
  {0x10f46, 0x10f50},
  {0x10f82, 0x10f85},
  {0x11001, 0x11001},
  {0x11038, 0x11046},
  {0x11070, 0x11070},
  {0x11073, 0x11074},
  {0x1107f, 0x11081},
  {0x110b3, 0x110b6},
  {0x110b9, 0x110ba},
  {0x110bd, 0x110bd},
  {0x110c2, 0x110c2},
  {0x110cd, 0x110cd},
  {0x11100, 0x11102},
  {0x11127, 0x1112b},
  {0x1112d, 0x11134},
  {0x11173, 0x11173},
  {0x11180, 0x11181},
  {0x111b6, 0x111be},
  {0x111c9, 0x111cc},
  {0x111cf, 0x111cf},
  {0x1122f, 0x11231},
  {0x11234, 0x11234},
  {0x11236, 0x11237},
  {0x1123e, 0x1123e},
  {0x11241, 0x11241},
  {0x112df, 0x112df},
  {0x112e3, 0x112ea},
  {0x11300, 0x11301},
  {0x1133b, 0x1133c},
  {0x11340, 0x11340},
  {0x11366, 0x1136c},
  {0x11370, 0x11374},
  {0x113bb, 0x113c0},
  {0x113ce, 0x113ce},
  {0x113d0, 0x113d0},
  {0x113d2, 0x113d2},
  {0x113e1, 0x113e2},
  {0x11438, 0x1143f},
  {0x11442, 0x11444},
  {0x11446, 0x11446},
  {0x1145e, 0x1145e},
  {0x114b3, 0x114b8},
  {0x114ba, 0x114ba},
  {0x114bf, 0x114c0},
  {0x114c2, 0x114c3},
  {0x115b2, 0x115b5},
  {0x115bc, 0x115bd},
  {0x115bf, 0x115c0},
  {0x115dc, 0x115dd},
  {0x11633, 0x1163a},
  {0x1163d, 0x1163d},
  {0x1163f, 0x11640},
  {0x116ab, 0x116ab},
  {0x116ad, 0x116ad},
  {0x116b0, 0x116b5},
  {0x116b7, 0x116b7},
  {0x1171d, 0x1171d},
  {0x1171f, 0x1171f},
  {0x11722, 0x11725},
  {0x11727, 0x1172b},
  {0x1182f, 0x11837},
  {0x11839, 0x1183a},
  {0x1193b, 0x1193c},
  {0x1193e, 0x1193e},
  {0x11943, 0x11943},
  {0x119d4, 0x119d7},
  {0x119da, 0x119db},
  {0x119e0, 0x119e0},
  {0x11a01, 0x11a0a},
  {0x11a33, 0x11a38},
  {0x11a3b, 0x11a3e},
  {0x11a47, 0x11a47},
  {0x11a51, 0x11a56},
  {0x11a59, 0x11a5b},
  {0x11a8a, 0x11a96},
  {0x11a98, 0x11a99},
  {0x11b60, 0x11b60},
  {0x11b62, 0x11b64},
  {0x11b66, 0x11b66},
  {0x11c30, 0x11c36},
  {0x11c38, 0x11c3d},
  {0x11c3f, 0x11c3f},
  {0x11c92, 0x11ca7},
  {0x11caa, 0x11cb0},
  {0x11cb2, 0x11cb3},
  {0x11cb5, 0x11cb6},
  {0x11d31, 0x11d36},
  {0x11d3a, 0x11d3a},
  {0x11d3c, 0x11d3d},
  {0x11d3f, 0x11d45},
  {0x11d47, 0x11d47},
  {0x11d90, 0x11d91},
  {0x11d95, 0x11d95},
  {0x11d97, 0x11d97},
  {0x11ef3, 0x11ef4},
  {0x11f00, 0x11f01},
  {0x11f36, 0x11f3a},
  {0x11f40, 0x11f40},
  {0x11f42, 0x11f42},
  {0x11f5a, 0x11f5a},
  {0x13430, 0x13440},
  {0x13447, 0x13455},
  {0x1611e, 0x16129},
  {0x1612d, 0x1612f},
  {0x16af0, 0x16af4},
  {0x16b30, 0x16b36},
  {0x16f4f, 0x16f4f},
  {0x16f8f, 0x16f92},
  {0x16fe4, 0x16fe4},
  {0x1bc9d, 0x1bc9e},
  {0x1bca0, 0x1bca3},
  {0x1cf00, 0x1cf2d},
  {0x1cf30, 0x1cf46},
  {0x1d167, 0x1d169},
  {0x1d173, 0x1d182},
  {0x1d185, 0x1d18b},
  {0x1d1aa, 0x1d1ad},
  {0x1d242, 0x1d244},
  {0x1da00, 0x1da36},
  {0x1da3b, 0x1da6c},
  {0x1da75, 0x1da75},
  {0x1da84, 0x1da84},
  {0x1da9b, 0x1da9f},
  {0x1daa1, 0x1daaf},
  {0x1e000, 0x1e006},
  {0x1e008, 0x1e018},
  {0x1e01b, 0x1e021},
  {0x1e023, 0x1e024},
  {0x1e026, 0x1e02a},
  {0x1e08f, 0x1e08f},
  {0x1e130, 0x1e136},
  {0x1e2ae, 0x1e2ae},
  {0x1e2ec, 0x1e2ef},
  {0x1e4ec, 0x1e4ef},
  {0x1e5ee, 0x1e5ef},
  {0x1e6e3, 0x1e6e3},
  {0x1e6e6, 0x1e6e6},
  {0x1e6ee, 0x1e6ef},
  {0x1e6f5, 0x1e6f5},
  {0x1e8d0, 0x1e8d6},
  {0x1e944, 0x1e94a},
  {0xe0001, 0xe0001},
  {0xe0020, 0xe007f},
  {0xe0100, 0xe01ef},
};
static const struct width_range wide_width[] = {
  {0x1100, 0x115f},
  {0x231a, 0x231b},
  {0x2329, 0x232a},
  {0x23e9, 0x23ec},
  {0x23f0, 0x23f0},
  {0x23f3, 0x23f3},
  {0x25fd, 0x25fe},
  {0x2614, 0x2615},
  {0x2630, 0x2637},
  {0x2648, 0x2653},
  {0x267f, 0x267f},
  {0x268a, 0x268f},
  {0x2693, 0x2693},
  {0x26a1, 0x26a1},
  {0x26aa, 0x26ab},
  {0x26bd, 0x26be},
  {0x26c4, 0x26c5},
  {0x26ce, 0x26ce},
  {0x26d4, 0x26d4},
  {0x26ea, 0x26ea},
  {0x26f2, 0x26f3},
  {0x26f5, 0x26f5},
  {0x26fa, 0x26fa},
  {0x26fd, 0x26fd},
  {0x2705, 0x2705},
  {0x270a, 0x270b},
  {0x2728, 0x2728},
  {0x274c, 0x274c},
  {0x274e, 0x274e},
  {0x2753, 0x2755},
  {0x2757, 0x2757},
  {0x2795, 0x2797},
  {0x27b0, 0x27b0},
  {0x27bf, 0x27bf},
  {0x2b1b, 0x2b1c},
  {0x2b50, 0x2b50},
  {0x2b55, 0x2b55},
  {0x2e80, 0x2e99},
  {0x2e9b, 0x2ef3},
  {0x2f00, 0x2fd5},
  {0x2ff0, 0x303e},
  {0x3041, 0x3096},
  {0x3099, 0x30ff},
  {0x3105, 0x312f},
  {0x3131, 0x318e},
  {0x3190, 0x31e5},
  {0x31ef, 0x321e},
  {0x3220, 0x3247},
  {0x3250, 0xa48c},
  {0xa490, 0xa4c6},
  {0xa960, 0xa97c},
  {0xac00, 0xd7a3},
  {0xf900, 0xfa6d},
  {0xfa70, 0xfad9},
  {0xfe10, 0xfe19},
  {0xfe30, 0xfe52},
  {0xfe54, 0xfe66},
  {0xfe68, 0xfe6b},
  {0xff01, 0xff60},
  {0xffe0, 0xffe6},
  {0x16fe0, 0x16fe4},
  {0x16ff0, 0x16ff6},
  {0x17000, 0x18cd5},
  {0x18cff, 0x18d1e},
  {0x18d80, 0x18df2},
  {0x1aff0, 0x1aff3},
  {0x1aff5, 0x1affb},
  {0x1affd, 0x1affe},
  {0x1b000, 0x1b122},
  {0x1b132, 0x1b132},
  {0x1b150, 0x1b152},
  {0x1b155, 0x1b155},
  {0x1b164, 0x1b167},
  {0x1b170, 0x1b2fb},
  {0x1d300, 0x1d356},
  {0x1d360, 0x1d376},
  {0x1f004, 0x1f004},
  {0x1f0cf, 0x1f0cf},
  {0x1f18e, 0x1f18e},
  {0x1f191, 0x1f19a},
  {0x1f200, 0x1f202},
  {0x1f210, 0x1f23b},
  {0x1f240, 0x1f248},
  {0x1f250, 0x1f251},
  {0x1f260, 0x1f265},
  {0x1f300, 0x1f320},
  {0x1f32d, 0x1f335},
  {0x1f337, 0x1f37c},
  {0x1f37e, 0x1f393},
  {0x1f3a0, 0x1f3ca},
  {0x1f3cf, 0x1f3d3},
  {0x1f3e0, 0x1f3f0},
  {0x1f3f4, 0x1f3f4},
  {0x1f3f8, 0x1f43e},
  {0x1f440, 0x1f440},
  {0x1f442, 0x1f4fc},
  {0x1f4ff, 0x1f53d},
  {0x1f54b, 0x1f54e},
  {0x1f550, 0x1f567},
  {0x1f57a, 0x1f57a},
  {0x1f595, 0x1f596},
  {0x1f5a4, 0x1f5a4},
  {0x1f5fb, 0x1f64f},
  {0x1f680, 0x1f6c5},
  {0x1f6cc, 0x1f6cc},
  {0x1f6d0, 0x1f6d2},
  {0x1f6d5, 0x1f6d8},
  {0x1f6dc, 0x1f6df},
  {0x1f6eb, 0x1f6ec},
  {0x1f6f4, 0x1f6fc},
  {0x1f7e0, 0x1f7eb},
  {0x1f7f0, 0x1f7f0},
  {0x1f90c, 0x1f93a},
  {0x1f93c, 0x1f945},
  {0x1f947, 0x1f9ff},
  {0x1fa70, 0x1fa7c},
  {0x1fa80, 0x1fa8a},
  {0x1fa8e, 0x1fac6},
  {0x1fac8, 0x1fac8},
  {0x1facd, 0x1fadc},
  {0x1fadf, 0x1faea},
  {0x1faef, 0x1faf8},
  {0x20000, 0x2a6df},
  {0x2a700, 0x2b81d},
  {0x2b820, 0x2cead},
  {0x2ceb0, 0x2ebe0},
  {0x2ebf0, 0x2ee5d},
  {0x2f800, 0x2fa1d},
  {0x30000, 0x3134a},
  {0x31350, 0x33479},
};

static int in_width_ranges(uint32_t c, const struct width_range *ranges,
                           size_t count) {
  size_t lo = 0, hi = count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (c < ranges[mid].first) hi = mid;
    else if (c > ranges[mid].last) lo = mid + 1;
    else return 1;
  }
  return 0;
}

size_t clay_utf8_char_width(const char *s, size_t *bytes) {
  const unsigned char *p = (const unsigned char *)s;
  uint32_t c;
  size_t n;
  if (!p || !*p) { if (bytes) *bytes = 0; return 0; }
  if (*p < 0x80) { c = *p; n = 1; }
  else {
    if (*p >= 0xc2 && *p <= 0xdf) { c = *p & 0x1f; n = 2; }
    else if (*p >= 0xe0 && *p <= 0xef) { c = *p & 0x0f; n = 3; }
    else if (*p >= 0xf0 && *p <= 0xf4) { c = *p & 0x07; n = 4; }
    else goto invalid;
    for (size_t i = 1; i < n; i++) {
      if (!p[i] || (p[i] & 0xc0) != 0x80) goto invalid;
      c = (c << 6) | (p[i] & 0x3f);
    }
    if ((n == 3 && c < 0x800) || (n == 4 && c < 0x10000) ||
        (c >= 0xd800 && c <= 0xdfff) || c > 0x10ffff) goto invalid;
  }
  if (bytes) *bytes = n;
  if (in_width_ranges(c, zero_width, sizeof(zero_width) / sizeof(*zero_width)))
    return 0;
  return in_width_ranges(c, wide_width, sizeof(wide_width) / sizeof(*wide_width)) ? 2 : 1;
invalid:
  if (bytes) *bytes = 1;
  return 1;
}

static const unsigned char *skip_ansi(const unsigned char *p) {
  p++;
  if (*p == '[') {
    p++;
    while (*p && (*p < 0x40 || *p > 0x7e)) p++;
    if (*p) p++;
  } else if (*p == ']') {
    p++;
    while (*p && *p != 0x07 && !(*p == 0x1b && p[1] == '\\')) p++;
    if (*p == 0x07) p++;
    else if (*p == 0x1b) p += 2;
  } else if (*p) p++;
  return p;
}

size_t clay_utf8_width(const char *s) {
  size_t width = 0;
  if (!s) return 0;
  const unsigned char *p = (const unsigned char *)s;
  while (*p) {
    if (*p == 0x1b) p = skip_ansi(p);
    else {
      size_t bytes;
      width += clay_utf8_char_width((const char *)p, &bytes);
      p += bytes;
    }
  }
  return width;
}

size_t clay_term_write_clipped(const char *text, int columns) {
  if (!text || columns <= 0) return 0;
  size_t width = 0;
  const unsigned char *p = (const unsigned char *)text;
  while (*p) {
    if (*p == 0x1b) {
      const unsigned char *start = p;
      p = skip_ansi(p);
      fwrite(start, 1, (size_t)(p - start), stdout);
    } else {
      size_t bytes;
      size_t cells = clay_utf8_char_width((const char *)p, &bytes);
      if (cells > (size_t)columns - width) break;
      fwrite(p, 1, bytes, stdout);
      p += bytes;
      width += cells;
    }
  }
  /* A clipped coloured row must not leak its style into the next row. */
  if (*p) fputs(clay_color(CLAY_RESET), stdout);
  return width;
}

void clay_term_hyperlink(const char *url, const char *text) {
  if (clay_term_supports_color()) {
    printf("\x1b]8;;%s\x1b\\%s\x1b]8;;\x1b\\", url, text);
  } else {
    fputs(text, stdout);
  }
}

void clay_term_hyperlink_file(const char *path) {
  if (!clay_term_supports_color()) {
    fputs(path, stdout);
    return;
  }

  char resolved[CLAY_PATH_MAX];
  static char url[CLAY_PATH_MAX + 16];
  const char *link_target = path;

#ifdef _WIN32
  if (_fullpath(resolved, path, sizeof(resolved)) != NULL) {
    snprintf(url, sizeof(url), "file:///%s", resolved);
    for (char *p = url; *p; p++) {
      if (*p == '\\')
        *p = '/';
    }
    link_target = url;
  }
#else
  if (realpath(path, resolved) != NULL) {
    snprintf(url, sizeof(url), "file://%s", resolved);
    link_target = url;
  }
#endif

  printf("\x1b]8;;%s\x1b\\%s\x1b]8;;\x1b\\", link_target, path);
}

#ifndef _WIN32
static struct termios g_orig_termios;
static int g_raw_active = 0;
#endif

void clay_term_bracketed_paste_enable(void) {
#ifndef _WIN32
  fputs("\x1b[?2004h", stdout);
  fflush(stdout);
#endif
}

void clay_term_bracketed_paste_disable(void) {
#ifndef _WIN32
  fputs("\x1b[?2004l", stdout);
  fflush(stdout);
#endif
}

ClayPasteResult clay_term_read_bracketed_paste(ClayStr *out) {
#ifdef _WIN32
  (void)out;
  return CLAY_PASTE_EOF;
#else
  char pending[6];
  size_t pending_len = 0;
  unsigned char c;
  const char terminator[] = "[201~";
  ClayPasteResult result = CLAY_PASTE_COMPLETE;
  for (;;) {
    /* Keep draining until the terminator after a failed paste; input cannot be resynchronized by time. */
    if (result == CLAY_PASTE_COMPLETE && out->len + pending_len >= 16 * 1024 * 1024)
      result = CLAY_PASTE_LIMIT;
    if (g_interrupted) {
      if (result == CLAY_PASTE_COMPLETE) result = CLAY_PASTE_INTERRUPTED;
      clay_term_take_interrupt();
    }
    struct pollfd fd = {STDIN_FILENO, POLLIN, 0};
    int ready = poll(&fd, 1, -1);
    if (ready < 0 && errno == EINTR) continue;
    if (ready < 0) {
      result = CLAY_PASTE_EOF;
      break;
    }
    ssize_t count = read(STDIN_FILENO, &c, 1);
    if (count < 0 && errno == EINTR) continue;
    if (count != 1) {
      result = CLAY_PASTE_EOF;
      break;
    }
    if (c == 0 && result == CLAY_PASTE_COMPLETE) result = CLAY_PASTE_INVALID;
    if (pending_len == 0) {
      if (c == 27) pending[pending_len++] = (char)c;
      else if (result == CLAY_PASTE_COMPLETE) clay_str_push_char(out, (char)c);
      continue;
    }
    if (c == (unsigned char)terminator[pending_len - 1]) {
      pending[pending_len++] = (char)c;
      if (pending_len == sizeof(pending)) return result;
      continue;
    }
    if (result == CLAY_PASTE_COMPLETE)
      for (size_t i = 0; i < pending_len; i++) clay_str_push_char(out, pending[i]);
    pending_len = 0;
    if (c == 27) pending[pending_len++] = (char)c;
    else if (result == CLAY_PASTE_COMPLETE) clay_str_push_char(out, (char)c);
  }
  return result;
#endif
}

void clay_term_raw_enable(void) {
#ifndef _WIN32
  if (g_raw_active)
    return;
  tcgetattr(STDIN_FILENO, &g_orig_termios);
  struct termios raw = g_orig_termios;
  raw.c_lflag &= ~(ECHO | ICANON);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
  g_raw_active = 1;
#endif
  /* _getch() on Windows already reads unbuffered, unechoed input. */
}

void clay_term_raw_disable(void) {
#ifndef _WIN32
  if (!g_raw_active)
    return;
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
  g_raw_active = 0;
#endif
}

int clay_term_take_interrupt(void) {
  if (!g_interrupted)
    return 0;
  g_interrupted = 0;
  return 1;
}

ClayKey clay_term_read_key(char *ch_out) {
  if (g_pending_escape) {
    g_pending_escape = 0;
    return CLAY_KEY_ESCAPE;
  }
#ifdef _WIN32
  int c = _getch();
  if (c == EOF)
    return CLAY_KEY_EOF;
  if (c == '\r' || c == '\n')
    return CLAY_KEY_ENTER;
  if (c == 8 || c == 127)
    return CLAY_KEY_BACKSPACE;
  if (c == 3)
    return CLAY_KEY_INTERRUPT;
  if (c == 0x0c)
    return CLAY_KEY_CLEAR_SCREEN;
  if (c == 0x0f)
    return CLAY_KEY_THINK_TOGGLE;
  if (c == 0x12)
    return CLAY_KEY_HISTORY_SEARCH;
  if (c == 27)
    return CLAY_KEY_ESCAPE;
  if (c == 0 || c == 0xE0) {
    int ext = _getch();
    switch (ext) {
    case 72:
      return CLAY_KEY_UP;
    case 80:
      return CLAY_KEY_DOWN;
    case 75:
      return CLAY_KEY_LEFT;
    case 77:
      return CLAY_KEY_RIGHT;
    case 15:
      return CLAY_KEY_CYCLE_SANDBOX;
    default:
      return CLAY_KEY_UNKNOWN;
    }
  }
  if (ch_out)
    *ch_out = (char)c;
  return CLAY_KEY_CHAR;
#else
  unsigned char c;
  ssize_t n = read(STDIN_FILENO, &c, 1);
  if (n < 0 && errno == EINTR && g_interrupted)
    return CLAY_KEY_INTERRUPT;
  if (n < 0 && errno == EINTR && g_resized) {
    g_resized = 0;
    return CLAY_KEY_RESIZE;
  }
  if (n <= 0)
    return CLAY_KEY_EOF;
  if (c == '\r' || c == '\n')
    return CLAY_KEY_ENTER;
  if (c == 127 || c == 8)
    return CLAY_KEY_BACKSPACE;
  if (c == 3)
    return CLAY_KEY_INTERRUPT;
  if (c == 0x0c)
    return CLAY_KEY_CLEAR_SCREEN;
  if (c == 0x0f)
    return CLAY_KEY_THINK_TOGGLE;
  if (c == 0x12)
    return CLAY_KEY_HISTORY_SEARCH;

  if (c == 0x1b) {
    unsigned char seq[64] = {0};
    size_t seq_len = 0;
    fd_set fds;
    struct timeval timeout = {0, 20000};
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &timeout) <= 0 ||
        read(STDIN_FILENO, &seq[seq_len], 1) <= 0) {
      return CLAY_KEY_ESCAPE;
    }
    seq_len++;
    if (seq[0] == 0x1b) {
      g_pending_escape = 1;
      return CLAY_KEY_ESCAPE;
    }
    while (seq_len < sizeof(seq) - 1) {
      timeout.tv_usec = 20000;
      FD_ZERO(&fds);
      FD_SET(STDIN_FILENO, &fds);
      if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &timeout) <= 0 ||
          read(STDIN_FILENO, &seq[seq_len], 1) <= 0)
        break;
      if ((seq[seq_len] >= 'A' && seq[seq_len] <= 'Z') ||
          (seq[seq_len] >= 'a' && seq[seq_len] <= 'z') || seq[seq_len] == '~') {
        seq_len++;
        break;
      }
      seq_len++;
    }
    seq[seq_len] = '\0';
    if (strcmp((char *)seq, "[200~") == 0) return CLAY_KEY_PASTE_BEGIN;
    if (strcmp((char *)seq, "[A") == 0) return CLAY_KEY_UP;
    if (strcmp((char *)seq, "[B") == 0) return CLAY_KEY_DOWN;
    if (strcmp((char *)seq, "[C") == 0) return CLAY_KEY_RIGHT;
    if (strcmp((char *)seq, "[D") == 0) return CLAY_KEY_LEFT;
    /* Shift+Tab is standardized as CSI Z by xterm-compatible terminals. */
    if (strcmp((char *)seq, "[Z") == 0)
      return CLAY_KEY_CYCLE_SANDBOX;
    return CLAY_KEY_UNKNOWN;
  }

  if (ch_out)
    *ch_out = (char)c;
  return CLAY_KEY_CHAR;
#endif
}

ClayKey clay_term_read_key_timeout(char *ch_out, int timeout_ms) {
  if (g_pending_escape)
    return clay_term_read_key(ch_out);
#ifdef _WIN32
  int elapsed = 0;
  while (!_kbhit()) {
    if (elapsed >= timeout_ms)
      return CLAY_KEY_NONE;
    Sleep(10);
    elapsed += 10;
  }
#else
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(STDIN_FILENO, &fds);
  struct timeval timeout = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  int ready = select(STDIN_FILENO + 1, &fds, NULL, NULL, &timeout);
  if (ready == 0)
    return CLAY_KEY_NONE;
  if (ready < 0 && errno == EINTR && g_interrupted)
    return CLAY_KEY_INTERRUPT;
  if (ready < 0 && errno == EINTR && g_resized) {
    g_resized = 0;
    return CLAY_KEY_RESIZE;
  }
  if (ready < 0)
    return CLAY_KEY_NONE;
#endif
  return clay_term_read_key(ch_out);
}

int clay_term_take_escape(void) {
  if (g_pending_escape_action) {
    g_pending_escape_action = 0;
    return 1;
  }
  if (g_pending_think_toggle)
    return 0;
  if (!clay_term_input_pending())
    return 0;
  ClayKey key = clay_term_read_key(NULL);
  if (key == CLAY_KEY_THINK_TOGGLE) {
    g_pending_think_toggle = 1;
    return 0;
  }
  if (key == CLAY_KEY_ESCAPE) {
    return 1;
  }
  if (key == CLAY_KEY_INTERRUPT)
    g_interrupted = 1;
  return 0;
}

int clay_term_take_think_toggle(void) {
  if (g_pending_think_toggle) {
    g_pending_think_toggle = 0;
    return 1;
  }
  if (g_pending_escape_action || !clay_term_input_pending())
    return 0;
  ClayKey key = clay_term_read_key(NULL);
  if (key == CLAY_KEY_THINK_TOGGLE)
    return 1;
  if (key == CLAY_KEY_ESCAPE) {
    g_pending_escape_action = 1;
    return 0;
  }
  if (key == CLAY_KEY_INTERRUPT)
    g_interrupted = 1;
  return 0;
}

int clay_term_input_pending(void) {
#ifdef _WIN32
  return _kbhit() != 0;
#else
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(STDIN_FILENO, &fds);
  struct timeval tv = {0, 0};
  return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
#endif
}

char *clay_term_home_dir(void) {
#ifdef _WIN32
  const char *home = getenv("USERPROFILE");
#else
  const char *home = getenv("HOME");
#endif
  return home ? strdup(home) : NULL;
}

char *clay_term_platform_name(void) {
#ifdef _WIN32
  return strdup("Windows");
#elif defined(__APPLE__)
  return strdup("macOS");
#else
  /* PRETTY_NAME on a distro that ships /etc/os-release, else a bare "Linux". */
  FILE *file = fopen("/etc/os-release", "r");
  if (!file)
    return strdup("Linux");
  ClayStr text;
  clay_str_init(&text);
  int ch;
  while ((ch = fgetc(file)) != EOF)
    clay_str_push_char(&text, (char)ch);
  fclose(file);
  const char *marker = "PRETTY_NAME=";
  const char *line = strstr(text.data, marker);
  if (!line || (line != text.data && line[-1] != '\n')) {
    clay_str_free(&text);
    return strdup("Linux");
  }
  const char *value = line + strlen(marker);
  const char *end = strchr(value, '\n');
  size_t len = end ? (size_t)(end - value) : strlen(value);
  if (len >= 2 && value[0] == '"' && value[len - 1] == '"') {
    value++;
    len -= 2;
  }
  char *name = malloc(len + 1);
  memcpy(name, value, len);
  name[len] = '\0';
  clay_str_free(&text);
  return name;
#endif
}

char *clay_term_cwd(void) {
#ifdef _WIN32
  return _getcwd(NULL, 0);
#else
  return getcwd(NULL, 0);
#endif
}

char *clay_term_resolve_path(const char *path) {
  char resolved[CLAY_PATH_MAX];
#ifdef _WIN32
  if (_fullpath(resolved, path, sizeof(resolved)) == NULL) return NULL;
#else
  if (realpath(path, resolved) == NULL) return NULL;
#endif
  return strdup(resolved);
}

int clay_term_mkdir(const char *path) {
#ifdef _WIN32
  if (_mkdir(path) != 0) return errno == EEXIST ? 0 : -1;
  clay_term_restrict_file(path);
  return 0;
#else
  return mkdir(path, 0700) == 0 || errno == EEXIST ? 0 : -1;
#endif
}

void clay_term_windows_parent_dir(const char *path, ClayStr *out) {
  clay_str_init(out);
  const char *slash = strrchr(path, '/');
  const char *backslash = strrchr(path, '\\');
  if (backslash && (!slash || backslash > slash)) slash = backslash;
  if (!slash) clay_str_push(out, ".");
  else {
    size_t n = (size_t)(slash - path);
    if (n == 2 && path[1] == ':') n++;
    else if (!n) n++;
    clay_str_push_n(out, path, n);
  }
}

static int write_file_atomic(const char *path, const void *data, size_t len,
                             int preserve_mode) {
  if (!path || (!data && len > 0)) return -1;
#ifdef _WIN32
  ClayStr parent;
  clay_term_windows_parent_dir(path, &parent);
  if (parent.len >= MAX_PATH) { clay_str_free(&parent); return -1; }
  char directory[MAX_PATH];
  memcpy(directory, parent.data, parent.len + 1);
  clay_str_free(&parent);
  char temporary[MAX_PATH];
  if (GetTempFileNameA(directory, "clay", 0, temporary) == 0) return -1;
  FILE *file = fopen(temporary, "wb");
  if (!file) {
    DeleteFileA(temporary);
    return -1;
  }
  int ok = fwrite(data, 1, len, file) == len;
  if (ok && fflush(file) != 0) ok = 0;
  if (ok && _commit(_fileno(file)) != 0) ok = 0;
  if (fclose(file) != 0) ok = 0;
  if (!ok) {
    DeleteFileA(temporary);
    return -1;
  }
  if (preserve_mode) {
    char backup[MAX_PATH];
    if (!GetTempFileNameA(directory, "clb", 0, backup)) {
      DeleteFileA(temporary);
      return -1;
    }
    if (!DeleteFileA(backup)) {
      DeleteFileA(temporary);
      return -1;
    }
    if (ReplaceFileA(path, temporary, backup, 0, NULL, NULL)) {
      DeleteFileA(backup);
    } else {
      DWORD error = GetLastError();
      if (error == ERROR_FILE_NOT_FOUND &&
          MoveFileExA(temporary, path, MOVEFILE_WRITE_THROUGH)) return 0;
      /* ReplaceFile may move the original to the backup even on failure. */
      if (error == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2 &&
          !MoveFileExA(backup, path, MOVEFILE_WRITE_THROUGH))
        fprintf(stderr, "clay: original file preserved at %s; restore it manually\n", backup);
      DeleteFileA(temporary);
      return -1;
    }
  } else if (!MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileA(temporary);
    return -1;
  }
  if (!preserve_mode) clay_term_restrict_file(path);
  return 0;
#else
  struct stat existing;
  int have_mode = 0;
  if (preserve_mode) {
    if (stat(path, &existing) == 0) {
#ifdef __linux__
      /* shortcut: reject ACLs rather than lose them; use an ACL API to preserve them. */
      if (getxattr(path, "system.posix_acl_access", NULL, 0) >= 0 || errno != ENODATA) return -1;
      have_mode = 1;
#else
      /* shortcut: ACL detection is unavailable; use a native ACL API to enable replacement. */
      return -1;
#endif
    }
    else if (errno != ENOENT) return -1;
  }
  ClayStr temporary;
  clay_str_init(&temporary);
  clay_str_printf(&temporary, "%s.tmp.XXXXXX", path);
  int descriptor = mkstemp(temporary.data);
  if (descriptor < 0) {
    clay_str_free(&temporary);
    return -1;
  }
  FILE *file = fdopen(descriptor, "wb");
  if (!file) {
    close(descriptor);
    remove(temporary.data);
    clay_str_free(&temporary);
    return -1;
  }
  int ok = fwrite(data, 1, len, file) == len;
  if (ok && fflush(file) != 0) ok = 0;
#ifdef __linux__
  if (ok && have_mode &&
      (fgetxattr(fileno(file), "system.posix_acl_access", NULL, 0) >= 0 || errno != ENODATA)) ok = 0;
#endif
  if (ok && have_mode && fchmod(fileno(file), existing.st_mode & 0777) != 0) ok = 0;
  if (ok && fsync(fileno(file)) != 0) ok = 0;
  if (fclose(file) != 0) ok = 0;
  if (!ok) {
    remove(temporary.data);
    clay_str_free(&temporary);
    return -1;
  }
  if (rename(temporary.data, path) != 0) {
    remove(temporary.data);
    clay_str_free(&temporary);
    return -1;
  }
  clay_str_free(&temporary);
  return 0;
#endif
}

int clay_term_write_file_atomic(const char *path, const void *data, size_t len) {
  return write_file_atomic(path, data, len, 0);
}

int clay_term_write_project_file_atomic(const char *path, const void *data, size_t len) {
  return write_file_atomic(path, data, len, 1);
}

int clay_term_read_file(const char *path, size_t max_bytes, ClayStr *out) {
  if (!path || !out) return -1;
  FILE *file = fopen(path, "rb");
  if (!file) return -1;
  clay_str_init(out);
  char buffer[4096];
  for (;;) {
    size_t count = fread(buffer, 1, sizeof(buffer), file);
    if (count > 0) {
      if (max_bytes > 0 && (out->len > max_bytes || count > max_bytes - out->len)) {
        fclose(file);
        clay_str_free(out);
        errno = EFBIG;
        return -1;
      }
      clay_str_push_n(out, buffer, count);
    }
    if (count < sizeof(buffer)) {
      if (ferror(file)) {
        fclose(file);
        clay_str_free(out);
        errno = EIO;
        return -1;
      }
      break;
    }
  }
  int ok = fclose(file) == 0;
  if (!ok) {
    clay_str_free(out);
    return -1;
  }
  return 0;
}

int clay_term_list_dir(const char *path, ClayArray *names) {
  clay_array_init(names, sizeof(char *));
#ifdef _WIN32
  ClayStr pattern;
  clay_str_init(&pattern);
  clay_str_printf(&pattern, "%s\\*", path);
  WIN32_FIND_DATA data;
  HANDLE handle = FindFirstFile(pattern.data, &data);
  clay_str_free(&pattern);
  if (handle == INVALID_HANDLE_VALUE)
    return -1;
  do {
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        strcmp(data.cFileName, ".") != 0 && strcmp(data.cFileName, "..") != 0) {
      char *name = strdup(data.cFileName);
      clay_array_push_val(names, &name);
    }
  } while (FindNextFile(handle, &data));
  FindClose(handle);
#else
  DIR *directory = opendir(path);
  if (!directory)
    return -1;
  struct dirent *entry;
  while ((entry = readdir(directory))) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    ClayStr child;
    clay_str_init(&child);
    clay_str_printf(&child, "%s/%s", path, entry->d_name);
    struct stat info;
    int is_dir = stat(child.data, &info) == 0 && S_ISDIR(info.st_mode);
    clay_str_free(&child);
    if (is_dir) {
      char *name = strdup(entry->d_name);
      clay_array_push_val(names, &name);
    }
  }
  closedir(directory);
#endif
  return 0;
}

int clay_term_list_entries(const char *path, ClayArray *names) {
  clay_array_init(names, sizeof(char *));
#ifdef _WIN32
  ClayStr pattern;
  clay_str_init(&pattern);
  clay_str_printf(&pattern, "%s\\*", path);
  WIN32_FIND_DATA data;
  HANDLE handle = FindFirstFile(pattern.data, &data);
  clay_str_free(&pattern);
  if (handle == INVALID_HANDLE_VALUE)
    return -1;
  do {
    if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0)
      continue;
    char *name = strdup(data.cFileName);
    clay_array_push_val(names, &name);
  } while (FindNextFile(handle, &data));
  FindClose(handle);
#else
  DIR *directory = opendir(path);
  if (!directory)
    return -1;
  struct dirent *entry;
  while ((entry = readdir(directory))) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    char *name = strdup(entry->d_name);
    clay_array_push_val(names, &name);
  }
  closedir(directory);
#endif
  return 0;
}

int clay_term_is_dir(const char *path) {
#ifdef _WIN32
  struct _stat info;
  return _stat(path, &info) == 0 && (info.st_mode & _S_IFDIR) != 0;
#else
  struct stat info;
  return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
#endif
}

long long clay_term_file_modified_at(const char *path) {
#ifdef _WIN32
  struct _stat info;
  return _stat(path, &info) == 0 ? (long long)info.st_mtime : 0;
#else
  struct stat info;
  return stat(path, &info) == 0 ? (long long)info.st_mtime : 0;
#endif
}

int clay_term_random_bytes(unsigned char *bytes, size_t count) {
  if (!bytes && count > 0)
    return -1;
  if (count == 0)
    return 0;
#ifdef _WIN32
  size_t index = 0;
  while (index < count) {
    unsigned int value;
    if (rand_s(&value) != 0)
      return -1;
    for (size_t offset = 0; offset < sizeof(value) && index < count;
         offset++, index++) {
      bytes[index] = (unsigned char)(value >> (offset * 8));
    }
  }
  return 0;
#else
  FILE *file = fopen("/dev/urandom", "rb");
  if (!file)
    return -1;
  size_t read_count = 0;
  while (read_count < count) {
    size_t read_now = fread(bytes + read_count, 1, count - read_count, file);
    if (read_now == 0)
      break;
    read_count += read_now;
  }
  int closed = fclose(file) == 0;
  return read_count == count && closed ? 0 : -1;
#endif
}

int clay_term_open_browser(const char *url) {
  if (!url || !*url)
    return -1;
#ifdef _WIN32
  return (intptr_t)ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL) >
                 32
             ? 0
             : -1;
#else
  pid_t pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    execlp("xdg-open", "xdg-open", url, (char *)NULL);
    _exit(127);
  }
  return 0;
#endif
}

ClayTermHttpServer *clay_term_http_server_create(unsigned short port) {
#ifdef _WIN32
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
    return NULL;
#endif
  ClayTermHttpServer *server = calloc(1, sizeof(*server));
  if (!server)
    goto failed;
#ifdef _WIN32
  server->listener = INVALID_SOCKET;
  server->peer = INVALID_SOCKET;
#else
  server->listener = -1;
  server->peer = -1;
#endif
  server->listener = socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
  if (server->listener == INVALID_SOCKET)
    goto failed;
#else
  if (server->listener < 0)
    goto failed;
#endif
  int yes = 1;
  setsockopt(server->listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes,
             sizeof(yes));
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  if (bind(server->listener, (struct sockaddr *)&address, sizeof(address)) !=
          0 ||
      listen(server->listener, 1) != 0)
    goto failed;
#ifdef _WIN32
  int address_len = sizeof(address);
#else
  socklen_t address_len = sizeof(address);
#endif
  if (getsockname(server->listener, (struct sockaddr *)&address,
                  &address_len) != 0)
    goto failed;
  server->port = ntohs(address.sin_port);
  return server;
failed:
  clay_term_http_server_destroy(server);
  return NULL;
}

unsigned short clay_term_http_server_port(const ClayTermHttpServer *server) {
  return server ? server->port : 0;
}

int clay_term_http_server_receive(ClayTermHttpServer *server, ClayStr *request,
                                  int timeout_ms) {
  if (!server || !request)
    return -1;
  fd_set ready;
  FD_ZERO(&ready);
  FD_SET(server->listener, &ready);
  struct timeval timeout = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  int rc = select((int)server->listener + 1, &ready, NULL, NULL, &timeout);
  if (rc == 0)
    return 0;
  if (rc < 0)
    return -1;
  /* A preflight can arrive before the real callback. */
#ifdef _WIN32
  if (server->peer != INVALID_SOCKET) {
    closesocket(server->peer);
    server->peer = INVALID_SOCKET;
  }
#else
  if (server->peer >= 0) {
    close(server->peer);
    server->peer = -1;
  }
#endif
  server->peer = accept(server->listener, NULL, NULL);
#ifdef _WIN32
  if (server->peer == INVALID_SOCKET)
    return -1;
#else
  if (server->peer < 0)
    return -1;
#endif
  clay_str_clear(request);
  char chunk[1024];
  for (;;) {
    int received = recv(server->peer, chunk, sizeof(chunk), 0);
    if (received <= 0)
      return -1;
    clay_str_push_n(request, chunk, (size_t)received);
    if (strstr(request->data, "\r\n\r\n") ||
        strstr(request->data, "\n\n"))
      return 1;
    if (request->len > 16384)
      return -1;
  }
}

int clay_term_http_server_reply(ClayTermHttpServer *server, int status,
                                const char *body) {
  return clay_term_http_server_reply_with_headers(server, status, body, NULL);
}

int clay_term_http_server_reply_with_headers(ClayTermHttpServer *server,
                                             int status, const char *body,
                                             const char *extra_headers) {
  if (!server || !body)
    return -1;
#ifdef _WIN32
  if (server->peer == INVALID_SOCKET)
    return -1;
#else
  if (server->peer < 0)
    return -1;
#endif
  ClayStr response;
  clay_str_init(&response);
  clay_str_printf(
      &response,
      "HTTP/1.1 %d %s\r\nContent-Type: text/html; "
      "charset=utf-8\r\nContent-Length: %zu\r\n%sConnection: close\r\n\r\n%s",
      status, status == 200 ? "OK" : (status == 204 ? "No Content" : "Bad Request"),
      strlen(body), extra_headers ? extra_headers : "", body);
  size_t sent = 0;
  while (sent < response.len) {
    int count =
        send(server->peer, response.data + sent, (int)(response.len - sent), 0);
    if (count <= 0) {
      clay_str_free(&response);
      return -1;
    }
    sent += (size_t)count;
  }
  clay_str_free(&response);
  return 0;
}

void clay_term_http_server_destroy(ClayTermHttpServer *server) {
  if (!server)
    return;
#ifdef _WIN32
  if (server->peer != INVALID_SOCKET)
    closesocket(server->peer);
  if (server->listener != INVALID_SOCKET)
    closesocket(server->listener);
  WSACleanup();
#else
  if (server->peer >= 0)
    close(server->peer);
  if (server->listener >= 0)
    close(server->listener);
#endif
  free(server);
}

void clay_term_restrict_file(const char *path) {
#ifndef _WIN32
  chmod(path, 0600);
#else
  HANDLE token = NULL;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return;
  DWORD token_size = 0;
  GetTokenInformation(token, TokenUser, NULL, 0, &token_size);
  if (!token_size) {
    CloseHandle(token);
    return;
  }
  PTOKEN_USER user = (PTOKEN_USER)malloc(token_size);
  PACL acl = NULL;
  if (!user || !GetTokenInformation(token, TokenUser, user, token_size, &token_size)) {
    free(user);
    CloseHandle(token);
    return;
  }
  EXPLICIT_ACCESSA access = {0};
  access.grfAccessPermissions = GENERIC_ALL;
  access.grfAccessMode = SET_ACCESS;
  access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
  BuildTrusteeWithSidA(&access.Trustee, user->User.Sid);
  DWORD result = SetEntriesInAclA(1, &access, NULL, &acl);
  if (result == ERROR_SUCCESS) {
    SetNamedSecurityInfoA((LPSTR)path, SE_FILE_OBJECT,
                          DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                          NULL, NULL, acl, NULL);
  }
  if (acl) LocalFree(acl);
  free(user);
  CloseHandle(token);
#endif
}

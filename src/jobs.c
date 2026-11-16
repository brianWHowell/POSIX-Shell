#include "shell.h"

static pid_t done_pids[MAX_JOBS];
static int done_status[MAX_JOBS];
static int done_next = 0;
bool wait_interruptible = false;

// Converts a waitpid status to a shell status: the exit code, or 128 + signal.
int status_of(int ws) {
  if (WIFEXITED(ws)) {
    return WEXITSTATUS(ws);
  }
  if (WIFSIGNALED(ws)) {
    return 128 + WTERMSIG(ws);
  }
  return 1;
}

// Prints why a foreground command was killed, such as "Killed", like other shells do.
static void report_signal(int ws) {
  if (!interactive || !WIFSIGNALED(ws)) {
    return;
  }
  int sig = WTERMSIG(ws);
  if (sig == SIGINT) {
    printf("\n");
  } else if (sig != SIGPIPE) {
    printf("%s%s\n", strsignal(sig), WCOREDUMP(ws) ? " (core dumped)" : "");
  }
}

// Returns the lowest-numbered signal that has arrived but not been handled.
int first_pending_signal(void) {
  for (int sig = 1; sig < NSIG; sig++) {
    if (pending_sig[sig]) {
      return sig;
    }
  }
  return SIGINT;
}

// Waits for every process in a pipeline to finish. Returns the status of the last one.
int wait_job(Job *j) {
  for (int i = 0; i < j->npids; i++) {
    int ws;
    pid_t r = waitpid(j->pids[i], &ws, 0);
    while (r < 0 && errno == EINTR) {
      if (wait_interruptible && (pending_any || got_sigint)) {
        return 128 + first_pending_signal();
      }
      r = waitpid(j->pids[i], &ws, 0);
    }
    if (r > 0 && i == j->npids - 1) {
      j->status = status_of(ws);
      report_signal(ws);
    }
  }
  return j->status;
}

// Remembers a background process, so wait can find it later.
void add_background(pid_t pid) {
  for (int i = 0; i < MAX_JOBS; i++) {
    if (background_pids[i] == 0) {
      background_pids[i] = pid;
      return;
    }
  }
  fprintf(stderr, "too many background jobs\n");
}

// Saves a finished background process's status so a later wait can still report it.
void remember_status(pid_t pid, int status) {
  done_pids[done_next] = pid;
  done_status[done_next] = status;
  done_next = (done_next + 1) % MAX_JOBS;
}

// Looks up and forgets a saved status. Returns false if there is none.
bool recall_status(pid_t pid, int *status) {
  for (int i = 0; i < MAX_JOBS; i++) {
    if (done_pids[i] == pid && pid != 0) {
      *status = done_status[i];
      done_pids[i] = 0;
      return true;
    }
  }
  return false;
}

// Records that a background process ended, and saves its status for wait.
static void background_done(pid_t pid, int ws) {
  for (int i = 0; i < MAX_JOBS; i++) {
    if (background_pids[i] == pid) {
      background_pids[i] = 0;
      remember_status(pid, status_of(ws));
      return;
    }
  }
}

// Collects every background process that has ended, without waiting.
void reap_background(void) {
  int ws;
  pid_t pid;
  while ((pid = waitpid(-1, &ws, WNOHANG)) > 0) {
    background_done(pid, ws);
  }
}

// Waits for one background process and returns its status.
// Returns 128 + signal if a trapped signal interrupts the wait.
int wait_background(int slot) {
  pid_t pid = background_pids[slot];
  int ws;
  pid_t r = waitpid(pid, &ws, 0);
  while (r < 0 && errno == EINTR) {
    if (pending_any || got_sigint) {
      return 128 + first_pending_signal();
    }
    r = waitpid(pid, &ws, 0);
  }
  background_pids[slot] = 0;
  return r > 0 ? status_of(ws) : 127;
}

// Returns the background table slot holding pid, or -1 if it isn't there.
int background_slot(pid_t pid) {
  for (int i = 0; i < MAX_JOBS; i++) {
    if (background_pids[i] == pid && pid != 0) {
      return i;
    }
  }
  return -1;
}

// Signal handler. Only records that the signal arrived.
// The trap runs later, between commands.
void on_signal(int sig) {
  pending_sig[sig] = 1;
  pending_any = 1;
  if (sig == SIGINT) {
    got_sigint = 1;
  }
}

// Installs on_signal for a signal, without restarting interrupted calls.
void catch_signal(int sig) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(sig, &sa, NULL);
}

// Notes which signals were already ignored when the shell started.
void record_ignored_signals(void) {
  for (int sig = 1; sig < NSIG; sig++) {
    struct sigaction sa;
    if (sigaction(sig, NULL, &sa) == 0 && sa.sa_handler == SIG_IGN) {
      ignored_on_entry[sig] = true;
    }
  }
}

// Puts a signal back to how the shell normally handles it, after trap -.
void shell_default_signal(int sig) {
  if (ignored_on_entry[sig] && !interactive) {
    signal(sig, SIG_IGN);
    return;
  }
  if (interactive && sig == SIGINT) {
    catch_signal(sig);
    return;
  }
  if (interactive && (sig == SIGQUIT || sig == SIGTERM || sig == SIGTSTP)) {
    signal(sig, SIG_IGN);
    return;
  }
  signal(sig, SIG_DFL);
}

// Sets up the signals an interactive shell catches or ignores.
void setup_signals(void) {
  catch_signal(SIGINT);
  signal(SIGQUIT, SIG_IGN);
  signal(SIGTERM, SIG_IGN);
  signal(SIGTSTP, SIG_IGN);
}

// Puts signals back to their defaults in a child process,
// keeping any that must stay ignored. SIGTSTP is left alone, so children of
// an interactive shell keep ignoring Ctrl-Z: there is no job control to resume them.
void reset_signals(void) {
  int plain[] = {SIGTERM, SIGTTIN, SIGTTOU};
  signal(SIGINT, in_background || ignored_on_entry[SIGINT] ? SIG_IGN : SIG_DFL);
  signal(SIGQUIT, in_background || ignored_on_entry[SIGQUIT] ? SIG_IGN : SIG_DFL);
  for (int i = 0; i < 3; i++) {
    signal(plain[i], ignored_on_entry[plain[i]] ? SIG_IGN : SIG_DFL);
  }
  for (int sig = 1; sig < NSIG; sig++) {
    if (trap_action[sig] == NULL) {
      continue;
    }
    if (trap_action[sig][0] == '\0') {
      signal(sig, SIG_IGN);
    } else if (sig != SIGKILL && sig != SIGSTOP) {
      signal(sig, SIG_DFL);
    }
  }
}

// Makes a forked child a subshell: not interactive, and traps reset.
void enter_subshell(void) {
  interactive = false;
  reset_signals();
  for (int sig = 0; sig < NSIG; sig++) {
    if (trap_action[sig] != NULL && trap_action[sig][0] != '\0') {
      free(trap_action[sig]);
      trap_action[sig] = NULL;
    }
    pending_sig[sig] = 0;
  }
  pending_any = 0;
  got_sigint = 0;
}

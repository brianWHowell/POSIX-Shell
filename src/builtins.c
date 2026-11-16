#include "shell.h"

static int dot_depth = 0;

static const char *reserved_words[] = {"if", "then", "else", "elif", "fi",
                                "do", "done", "case", "esac", "while",
                                "until", "for", "{", "}", "!", "in"};

// True if the word is a reserved word such as if or done.
static bool is_reserved_word(const char *name) {
  for (int i = 0; i < 16; i++) {
    if (strcmp(reserved_words[i], name) == 0) {
      return true;
    }
  }
  return false;
}

// Counts the arguments in a NULL-terminated list.
static int count_args(char **args) {
  int n = 0;
  while (args[n] != NULL) {
    n++;
  }
  return n;
}

// The : and true builtins. Do nothing and succeed.
static int bi_colon(char **args) {
  (void)args;
  return 0;
}

// The false builtin. Does nothing and fails.
static int bi_false(char **args) {
  (void)args;
  return 1;
}

// Shared code for break and continue: checks n and sets the unwind counter.
static int loop_control(char **args, bool is_break) {
  long n = 1;
  if (args[1] != NULL && (!parse_int(args[1], &n) || n < 1)) {
    shell_error("%s: %s: bad number", args[0], args[1]);
    special_error = true;
    return 1;
  }
  if (loop_depth == 0) {
    return 0;
  }
  if (n > loop_depth) {
    n = loop_depth;
  }
  if (is_break) {
    loop_break = (int)n;
  } else {
    loop_continue = (int)n;
  }
  return 0;
}

// The break [n] builtin. Leaves n enclosing loops.
static int bi_break(char **args) {
  return loop_control(args, true);
}

// The continue [n] builtin. Starts the next pass of the nth enclosing loop.
static int bi_continue(char **args) {
  return loop_control(args, false);
}

// Finds the file for . to run: as given if it has a /, otherwise on PATH.
static char *find_dot_file(const char *name) {
  if (strchr(name, '/') != NULL) {
    return xstrdup(name);
  }
  const char *path = get_var("PATH");
  if (path == NULL) {
    return NULL;
  }
  const char *dir = path;
  while (true) {
    const char *end = strchr(dir, ':');
    size_t dir_len = end != NULL ? (size_t)(end - dir) : strlen(dir);
    Str full = {0};
    str_add_len(&full, dir_len == 0 ? "." : dir, dir_len == 0 ? 1 : dir_len);
    str_add_char(&full, '/');
    str_add(&full, name);
    struct stat st;
    if (stat(full.data, &st) == 0 && S_ISREG(st.st_mode) &&
        access(full.data, R_OK) == 0) {
      return str_take(&full);
    }
    str_free(&full);
    if (end == NULL) {
      return NULL;
    }
    dir = end + 1;
  }
}

// Runs a file in the current shell. Used by . and the startup files.
// new_params, if given, replaces $1... for the duration.
int run_file(const char *path, char **new_params, int count) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    shell_error("%s: %s", path, strerror(errno));
    return -1;
  }
  Source src;
  memset(&src, 0, sizeof(src));
  src.fd = move_fd_high(fd);
  SavedParams saved;
  bool swap = new_params != NULL;
  if (swap) {
    saved = save_params();
    set_params(new_params, count);
  }
  dot_depth++;
  int status = run_source(&src);
  dot_depth--;
  if (returning) {
    returning = false;
    status = return_status;
  }
  if (swap) {
    restore_params(saved);
  }
  close(src.fd);
  return status;
}

// The . file [args] builtin. Runs a file in the current shell.
static int bi_dot(char **args) {
  if (args[1] == NULL) {
    shell_error(".: filename argument required", NULL, NULL);
    special_error = true;
    return 2;
  }
  char *path = find_dot_file(args[1]);
  if (path == NULL) {
    shell_error(".: %s: not found", args[1], NULL);
    special_error = true;
    return 2;
  }
  int count = count_args(args + 2);
  int status = run_file(path, count > 0 ? args + 2 : NULL, count);
  free(path);
  if (status < 0) {
    special_error = true;
    return 2;
  }
  return status;
}

// The eval builtin. Joins its arguments and runs them as a command.
static int bi_eval(char **args) {
  Str text = {0};
  for (int i = 1; args[i] != NULL; i++) {
    if (i > 1) {
      str_add_char(&text, ' ');
    }
    str_add(&text, args[i]);
  }
  int status = 0;
  if (text.len > 0) {
    status = run_string(text.data);
  }
  str_free(&text);
  return status;
}

// The exec builtin. Replaces the shell with a command,
// or with no command keeps its redirections permanently.
static int bi_exec(char **args) {
  if (args[1] == NULL) {
    return 0;
  }
  char *path = find_command(args[1]);
  if (path == NULL) {
    shell_error("%s: not found", args[1], NULL);
    if (!interactive) {
      exit_shell(127);
    }
    return 127;
  }
  bool was_interactive = interactive;
  in_background = false;
  reset_signals();
  char **env = build_env();
  execve(path, args + 1, env);
  int err = errno;
  if (err == ENOEXEC) {
    enter_subshell();
    run_script_child(path, args + 1);
  }
  shell_error("%s: %s", args[1], strerror(err));
  free(path);
  if (!was_interactive) {
    exit_shell(err == ENOENT ? 127 : 126);
  }
  setup_signals();
  return err == ENOENT ? 127 : 126;
}

// The exit [n] builtin.
static int bi_exit(char **args) {
  int code = last_status;
  if (args[1] != NULL) {
    long n;
    if (!parse_int(args[1], &n)) {
      shell_error("exit: %s: numeric argument required", args[1], NULL);
      n = 2;
    }
    code = (int)(n & 0xFF);
  }
  exit_shell(code);
  return code;
}

// Prints variables in a form that can be pasted back, sorted by name.
// prefix is "export" or "readonly" for the -p listings.
static void print_vars(const char *prefix, bool only_exported,
                       bool only_readonly) {
  sort_vars();
  for (int i = 0; i < nvars; i++) {
    Var *v = &vars[i];
    if ((only_exported && !v->exported) || (only_readonly && !v->readonly)) {
      continue;
    }
    if (prefix == NULL && v->value == NULL) {
      continue;
    }
    Str line = {0};
    if (prefix != NULL) {
      str_add(&line, prefix);
      str_add_char(&line, ' ');
    }
    str_add(&line, v->name);
    if (v->value != NULL) {
      str_add_char(&line, '=');
      quote_value(&line, v->value);
    }
    printf("%s\n", str_cstr(&line));
    str_free(&line);
  }
}

// Shared code for export and readonly: marks or sets each name, or lists with -p.
static int declare_vars(char **args, bool exported) {
  int i = 1;
  if (args[i] != NULL && strcmp(args[i], "-p") == 0) {
    i++;
  }
  if (args[i] != NULL && strcmp(args[i], "--") == 0) {
    i++;
  }
  if (args[i] == NULL) {
    print_vars(exported ? "export" : "readonly", exported, !exported);
    return 0;
  }
  int status = 0;
  for (; args[i] != NULL; i++) {
    char *name = xstrdup(args[i]);
    char *eq = strchr(name, '=');
    if (eq != NULL) {
      *eq = '\0';
    }
    if (!is_valid_name(name)) {
      shell_error("%s: %s: bad variable name", args[0], name);
      special_error = true;
      status = 1;
      free(name);
      continue;
    }
    if (eq != NULL && !set_var(name, eq + 1)) {
      special_error = true;
      status = 1;
      free(name);
      continue;
    }
    Var *v = add_var(name);
    if (exported) {
      v->exported = true;
    } else {
      v->readonly = true;
    }
    free(name);
  }
  return status;
}

// The export builtin.
static int bi_export(char **args) {
  return declare_vars(args, true);
}

// The readonly builtin.
static int bi_readonly(char **args) {
  return declare_vars(args, false);
}

// The return [n] builtin. Leaves the current function or . file.
static int bi_return(char **args) {
  long n = last_status;
  if (args[1] != NULL && !parse_int(args[1], &n)) {
    shell_error("return: %s: bad number", args[1], NULL);
    n = 2;
  }
  if (func_depth == 0 && dot_depth == 0) {
    shell_error("return: not in a function", NULL, NULL);
    special_error = true;
    return 1;
  }
  returning = true;
  return_status = (int)(n & 0xFF);
  return return_status;
}

// Prints every option for set -o, or as set commands for set +o.
static void print_options(bool as_commands) {
  for (int i = 0; i < option_count; i++) {
    bool on = options[i].on;
    if (as_commands) {
      printf("set %co %s\n", on ? '-' : '+', options[i].name);
    } else {
      printf("%-15s %s\n", options[i].name, on ? "on" : "off");
    }
  }
}

// The set builtin. Changes options, sets $1..., or lists variables.
static int bi_set(char **args) {
  if (args[1] == NULL) {
    print_vars(NULL, false, false);
    return 0;
  }
  int i = 1;
  bool set_positional = false;
  while (args[i] != NULL) {
    const char *a = args[i];
    if (strcmp(a, "--") == 0) {
      set_positional = true;
      i++;
      break;
    }
    if (strcmp(a, "-") == 0) {
      find_option_letter('x')->on = false;
      find_option_letter('v')->on = false;
      set_positional = true;
      i++;
      break;
    }
    if (a[0] != '-' && a[0] != '+') {
      set_positional = true;
      break;
    }
    bool on = a[0] == '-';
    if (strcmp(a + 1, "o") == 0) {
      if (args[i + 1] == NULL) {
        print_options(!on);
        i++;
        continue;
      }
      Option *o = find_option_name(args[i + 1]);
      if (o == NULL) {
        shell_error("set: %s: invalid option name", args[i + 1], NULL);
        special_error = true;
        return 2;
      }
      set_option(o, on);
      i += 2;
      continue;
    }
    for (int k = 1; a[k] != '\0'; k++) {
      Option *o = find_option_letter(a[k]);
      if (o == NULL) {
        char letter[2] = {a[k], '\0'};
        shell_error("set: -%s: invalid option", letter, NULL);
        special_error = true;
        return 2;
      }
      set_option(o, on);
    }
    i++;
  }
  if (set_positional) {
    set_params(args + i, count_args(args + i));
  }
  return 0;
}

// The shift [n] builtin. Drops the first n positional parameters.
static int bi_shift(char **args) {
  long n = 1;
  if (args[1] != NULL && (!parse_int(args[1], &n) || n < 0)) {
    shell_error("shift: %s: bad number", args[1], NULL);
    special_error = true;
    return 1;
  }
  if (n > nparams) {
    shell_error("shift: can't shift that many", NULL, NULL);
    special_error = true;
    return 1;
  }
  for (int i = 0; i < n; i++) {
    free(params[i]);
  }
  memmove(params, params + n, (nparams - n + 1) * sizeof(char *));
  nparams -= (int)n;
  return 0;
}

// Prints a time in the 0m0.000s format used by times.
static void print_time(clock_t ticks, long hz) {
  double seconds = (double)ticks / (double)hz;
  int minutes = (int)(seconds / 60);
  printf("%dm%.3fs", minutes, seconds - minutes * 60);
}

// The times builtin. Prints the CPU time used by the shell and its children.
static int bi_times(char **args) {
  (void)args;
  struct tms t;
  times(&t);
  long hz = sysconf(_SC_CLK_TCK);
  print_time(t.tms_utime, hz);
  printf(" ");
  print_time(t.tms_stime, hz);
  printf("\n");
  print_time(t.tms_cutime, hz);
  printf(" ");
  print_time(t.tms_cstime, hz);
  printf("\n");
  return 0;
}

static const char *signal_names[] = {
    "HUP",  "INT",  "QUIT", "ILL",  "TRAP", "ABRT", "BUS",  "FPE",
    "KILL", "USR1", "SEGV", "USR2", "PIPE", "ALRM", "TERM", "CHLD",
    "CONT", "STOP", "TSTP", "TTIN", "TTOU", "URG",  "XCPU", "XFSZ",
    "VTALRM", "PROF", "WINCH", "SYS"};

static const int signal_numbers[] = {
    SIGHUP,  SIGINT,  SIGQUIT, SIGILL,  SIGTRAP, SIGABRT, SIGBUS,  SIGFPE,
    SIGKILL, SIGUSR1, SIGSEGV, SIGUSR2, SIGPIPE, SIGALRM, SIGTERM, SIGCHLD,
    SIGCONT, SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU, SIGURG,  SIGXCPU, SIGXFSZ,
    SIGVTALRM, SIGPROF, SIGWINCH, SIGSYS};

// Returns how many signals are in the signal name table.
static int signal_count(void) {
  return sizeof(signal_numbers) / sizeof(signal_numbers[0]);
}

// Converts a signal name (INT, SIGINT, int) or number to its number.
// Returns -1 if it isn't one.
static int parse_signal(const char *s) {
  if (isdigit((unsigned char)s[0])) {
    long n;
    return parse_int(s, &n) && n >= 0 && n < NSIG ? (int)n : -1;
  }
  char upper[32];
  size_t k = 0;
  for (; s[k] != '\0' && k < sizeof(upper) - 1; k++) {
    upper[k] = (char)toupper((unsigned char)s[k]);
  }
  upper[k] = '\0';
  const char *name = upper;
  if (strncmp(name, "SIG", 3) == 0) {
    name += 3;
  }
  for (int i = 0; i < signal_count(); i++) {
    if (strcmp(name, signal_names[i]) == 0) {
      return signal_numbers[i];
    }
  }
  return -1;
}

// Returns the name of a signal number, such as "INT", or NULL.
static const char *signal_name(int sig) {
  for (int i = 0; i < signal_count(); i++) {
    if (signal_numbers[i] == sig) {
      return signal_names[i];
    }
  }
  return NULL;
}

// Like parse_signal, but also accepts EXIT and 0 for the exit trap.
static int trap_number(const char *s) {
  if (strcmp(s, "EXIT") == 0 || strcmp(s, "0") == 0) {
    return 0;
  }
  return parse_signal(s);
}

// The trap builtin. Sets, resets, ignores or lists trap actions.
static int bi_trap(char **args) {
  int i = 1;
  if (args[i] != NULL && strcmp(args[i], "--") == 0) {
    i++;
  }
  if (args[i] == NULL || strcmp(args[i], "-p") == 0) {
    for (int sig = 0; sig < NSIG; sig++) {
      if (trap_action[sig] == NULL) {
        continue;
      }
      Str line = {0};
      str_add(&line, "trap -- ");
      quote_value(&line, trap_action[sig]);
      str_add_char(&line, ' ');
      const char *name = sig == 0 ? "EXIT" : signal_name(sig);
      if (name != NULL) {
        str_add(&line, name);
      } else {
        char num[16];
        snprintf(num, sizeof(num), "%d", sig);
        str_add(&line, num);
      }
      printf("%s\n", str_cstr(&line));
      str_free(&line);
    }
    return 0;
  }
  const char *action = args[i];
  bool reset = strcmp(action, "-") == 0;
  long n;
  if (parse_int(action, &n) && n >= 0) {
    reset = true;
  } else {
    i++;
  }
  if (args[i] == NULL) {
    shell_error("trap: usage: trap [action condition...]", NULL, NULL);
    special_error = true;
    return 2;
  }
  int status = 0;
  for (; args[i] != NULL; i++) {
    int sig = trap_number(args[i]);
    if (sig < 0 || sig == SIGKILL || sig == SIGSTOP) {
      shell_error("trap: %s: bad trap", args[i], NULL);
      status = 1;
      continue;
    }
    if (sig != 0 && ignored_on_entry[sig] && !interactive) {
      continue;
    }
    free(trap_action[sig]);
    trap_action[sig] = reset ? NULL : xstrdup(action);
    if (sig == 0) {
      continue;
    }
    if (reset) {
      shell_default_signal(sig);
    } else if (action[0] == '\0') {
      signal(sig, SIG_IGN);
    } else {
      catch_signal(sig);
    }
  }
  return status;
}

// The unset [-v|-f] builtin. Removes variables or functions.
static int bi_unset(char **args) {
  bool functions = false;
  int i = 1;
  while (args[i] != NULL && args[i][0] == '-') {
    if (strcmp(args[i], "-f") == 0) {
      functions = true;
    } else if (strcmp(args[i], "-v") == 0) {
      functions = false;
    } else if (strcmp(args[i], "--") == 0) {
      i++;
      break;
    } else {
      shell_error("unset: %s: invalid option", args[i], NULL);
      special_error = true;
      return 2;
    }
    i++;
  }
  int status = 0;
  for (; args[i] != NULL; i++) {
    if (functions) {
      remove_func(args[i]);
    } else if (!unset_var(args[i])) {
      special_error = true;
      status = 1;
    }
  }
  return status;
}

// The alias builtin. Defines aliases, or lists them.
static int bi_alias(char **args) {
  if (args[1] == NULL) {
    for (int i = 0; i < naliases; i++) {
      Str line = {0};
      str_add(&line, alias_names[i]);
      str_add_char(&line, '=');
      quote_value(&line, alias_values[i]);
      printf("%s\n", str_cstr(&line));
      str_free(&line);
    }
    return 0;
  }
  int status = 0;
  for (int i = 1; args[i] != NULL; i++) {
    char *eq = strchr(args[i], '=');
    if (eq != NULL) {
      char *name = xstrndup(args[i], eq - args[i]);
      set_alias(name, eq + 1);
      free(name);
      continue;
    }
    const char *value = get_alias(args[i]);
    if (value == NULL) {
      shell_error("alias: %s: not found", args[i], NULL);
      status = 1;
      continue;
    }
    Str line = {0};
    str_add(&line, args[i]);
    str_add_char(&line, '=');
    quote_value(&line, value);
    printf("%s\n", str_cstr(&line));
    str_free(&line);
  }
  return status;
}

// The unalias [-a] builtin. Removes aliases.
static int bi_unalias(char **args) {
  if (args[1] != NULL && strcmp(args[1], "-a") == 0) {
    while (naliases > 0) {
      remove_alias(alias_names[0]);
    }
    return 0;
  }
  int status = 0;
  for (int i = 1; args[i] != NULL; i++) {
    if (!remove_alias(args[i])) {
      shell_error("unalias: %s: not found", args[i], NULL);
      status = 1;
    }
  }
  return status;
}

// Joins dir onto base and resolves . and .. by text,
// the way cd does without -P.
static char *logical_path(const char *base, const char *dir) {
  Str joined = {0};
  if (dir[0] != '/') {
    str_add(&joined, base);
    str_add_char(&joined, '/');
  }
  str_add(&joined, dir);
  char **parts = xmalloc((joined.len + 1) * sizeof(char *));
  int n = 0;
  char *save = NULL;
  for (char *tok = strtok_r(joined.data, "/", &save); tok != NULL;
       tok = strtok_r(NULL, "/", &save)) {
    if (strcmp(tok, ".") == 0) {
      continue;
    }
    if (strcmp(tok, "..") == 0) {
      if (n > 0) {
        n--;
      }
      continue;
    }
    parts[n++] = tok;
  }
  Str out = {0};
  for (int i = 0; i < n; i++) {
    str_add_char(&out, '/');
    str_add(&out, parts[i]);
  }
  if (n == 0) {
    str_add_char(&out, '/');
  }
  free(parts);
  str_free(&joined);
  return str_take(&out);
}

// True if the path is a directory.
static bool is_directory(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// Returns the current directory: $PWD if it is still right, otherwise getcwd.
static char *current_pwd(void) {
  const char *pwd = get_var("PWD");
  struct stat a;
  struct stat b;
  if (pwd != NULL && pwd[0] == '/' && stat(pwd, &a) == 0 && stat(".", &b) == 0 &&
      a.st_dev == b.st_dev && a.st_ino == b.st_ino) {
    return xstrdup(pwd);
  }
  char buf[PATH_MAX];
  if (getcwd(buf, sizeof(buf)) != NULL) {
    return xstrdup(buf);
  }
  return NULL;
}

// Sets a variable and exports it.
static void set_exported(const char *name, const char *value) {
  if (set_var(name, value)) {
    find_var(name)->exported = true;
  }
}

// The cd [-L|-P] [dir|-] builtin. Uses CDPATH and updates PWD and OLDPWD.
static int bi_cd(char **args) {
  bool physical = false;
  int i = 1;
  while (args[i] != NULL && args[i][0] == '-' && args[i][1] != '\0') {
    if (strcmp(args[i], "-L") == 0) {
      physical = false;
    } else if (strcmp(args[i], "-P") == 0) {
      physical = true;
    } else if (strcmp(args[i], "--") == 0) {
      i++;
      break;
    } else {
      shell_error("cd: %s: invalid option", args[i], NULL);
      return 2;
    }
    i++;
  }
  const char *dir = args[i];
  bool print = false;
  if (dir == NULL) {
    dir = get_var("HOME");
    if (dir == NULL) {
      shell_error("cd: HOME not set", NULL, NULL);
      return 1;
    }
  } else if (strcmp(dir, "-") == 0) {
    dir = get_var("OLDPWD");
    if (dir == NULL) {
      shell_error("cd: OLDPWD not set", NULL, NULL);
      return 1;
    }
    print = true;
  }
  char *target = NULL;
  bool dotted = dir[0] == '.' && (dir[1] == '\0' || dir[1] == '/' ||
                                  (dir[1] == '.' && (dir[2] == '\0' || dir[2] == '/')));
  const char *cdpath = get_var("CDPATH");
  if (dir[0] != '/' && !dotted && cdpath != NULL) {
    const char *p = cdpath;
    while (target == NULL) {
      const char *end = strchr(p, ':');
      size_t n = end != NULL ? (size_t)(end - p) : strlen(p);
      Str cand = {0};
      str_add_len(&cand, n == 0 ? "." : p, n == 0 ? 1 : n);
      str_add_char(&cand, '/');
      str_add(&cand, dir);
      if (is_directory(cand.data)) {
        target = str_take(&cand);
        if (n > 0) {
          print = true;
        }
      } else {
        str_free(&cand);
      }
      if (end == NULL) {
        break;
      }
      p = end + 1;
    }
  }
  if (target == NULL) {
    target = xstrdup(dir);
  }
  char *old = current_pwd();
  char *newpwd = NULL;
  if (!physical) {
    newpwd = logical_path(old != NULL ? old : "/", target);
    if (chdir(newpwd) != 0) {
      free(newpwd);
      newpwd = NULL;
    }
  }
  if (newpwd == NULL) {
    if (chdir(target) != 0) {
      shell_error("cd: %s: %s", dir, strerror(errno));
      free(target);
      free(old);
      return 1;
    }
    char buf[PATH_MAX];
    newpwd = xstrdup(getcwd(buf, sizeof(buf)) != NULL ? buf : target);
  }
  if (old != NULL) {
    set_exported("OLDPWD", old);
  }
  set_exported("PWD", newpwd);
  if (print) {
    printf("%s\n", newpwd);
  }
  free(newpwd);
  free(old);
  free(target);
  return 0;
}

// The pwd [-L|-P] builtin.
static int bi_pwd(char **args) {
  bool physical = args[1] != NULL && strcmp(args[1], "-P") == 0;
  char *dir = NULL;
  if (!physical) {
    dir = current_pwd();
  } else {
    char buf[PATH_MAX];
    if (getcwd(buf, sizeof(buf)) != NULL) {
      dir = xstrdup(buf);
    }
  }
  if (dir == NULL) {
    shell_error("pwd: %s", strerror(errno), NULL);
    return 1;
  }
  printf("%s\n", dir);
  free(dir);
  return 0;
}

// Prints what a name would run, for type, command -v and command -V.
static int describe_command(const char *name, bool verbose) {
  const char *alias = get_alias(name);
  if (alias != NULL) {
    Str line = {0};
    if (verbose) {
      str_add(&line, name);
      str_add(&line, " is an alias for ");
      str_add(&line, alias);
    } else {
      str_add(&line, "alias ");
      str_add(&line, name);
      str_add_char(&line, '=');
      quote_value(&line, alias);
    }
    printf("%s\n", str_cstr(&line));
    str_free(&line);
    return 0;
  }
  if (is_reserved_word(name)) {
    printf(verbose ? "%s is a shell keyword\n" : "%s\n", name);
    return 0;
  }
  const Builtin *b = find_builtin(name);
  if (b != NULL && b->special) {
    printf(verbose ? "%s is a special shell builtin\n" : "%s\n", name);
    return 0;
  }
  if (find_func(name) != NULL) {
    printf(verbose ? "%s is a shell function\n" : "%s\n", name);
    return 0;
  }
  if (b != NULL) {
    printf(verbose ? "%s is a shell builtin\n" : "%s\n", name);
    return 0;
  }
  char *path = find_command(name);
  if (path == NULL || !is_executable(path)) {
    if (verbose) {
      shell_error("%s: not found", name, NULL);
    }
    free(path);
    return 127;
  }
  if (verbose) {
    printf("%s is %s\n", name, path);
  } else {
    printf("%s\n", path);
  }
  free(path);
  return 0;
}

// The type builtin. Describes each name.
static int bi_type(char **args) {
  int status = 0;
  for (int i = 1; args[i] != NULL; i++) {
    if (describe_command(args[i], true) != 0) {
      status = 127;
    }
  }
  return status;
}

// The command builtin. Runs a command while skipping functions,
// or describes it with -v or -V.
static int bi_command(char **args) {
  int i = 1;
  bool default_path = false;
  bool describe = false;
  bool verbose = false;
  while (args[i] != NULL && args[i][0] == '-' && args[i][1] != '\0') {
    if (strcmp(args[i], "--") == 0) {
      i++;
      break;
    }
    for (int k = 1; args[i][k] != '\0'; k++) {
      char c = args[i][k];
      if (c == 'p') {
        default_path = true;
      } else if (c == 'v') {
        describe = true;
      } else if (c == 'V') {
        verbose = true;
      } else {
        char letter[2] = {c, '\0'};
        shell_error("command: -%s: invalid option", letter, NULL);
        return 2;
      }
    }
    i++;
  }
  if (args[i] == NULL) {
    return 0;
  }
  if (describe || verbose) {
    int status = 0;
    for (; args[i] != NULL; i++) {
      if (describe_command(args[i], verbose) != 0) {
        status = verbose ? 127 : 1;
      }
    }
    return status;
  }
  const Builtin *b = find_builtin(args[i]);
  if (b != NULL) {
    bool saved = special_error;
    int status = b->fn(args + i);
    special_error = saved;
    return status;
  }
  char path_buf[1024];
  const char *path = NULL;
  if (default_path) {
    size_t n = confstr(_CS_PATH, path_buf, sizeof(path_buf));
    path = n > 0 && n <= sizeof(path_buf) ? path_buf : "/usr/bin:/bin";
  }
  return fork_external(args + i, NULL, NULL, 0, NULL, path);
}

// Sets OPTIND without restarting getopts.
static void set_optind(long n) {
  int pos = getopts_pos;
  set_var_int("OPTIND", n);
  getopts_pos = pos;
  getopts_optind = (int)n;
}

// The getopts builtin. Reads the next option from the arguments each time it runs.
static int bi_getopts(char **args) {
  if (args[1] == NULL || args[2] == NULL) {
    shell_error("getopts: usage: getopts optstring name [arg...]", NULL, NULL);
    return 2;
  }
  const char *optstring = args[1];
  const char *var = args[2];
  char **list = args[3] != NULL ? args + 3 : params;
  int argc = args[3] != NULL ? count_args(args + 3) : nparams;
  long optind = 1;
  const char *cur = get_var("OPTIND");
  if (cur == NULL || !parse_int(cur, &optind) || optind < 1) {
    optind = 1;
  }
  if (optind != getopts_optind) {
    getopts_pos = 1;
    getopts_optind = (int)optind;
  }
  bool silent = optstring[0] == ':';
  if (silent) {
    optstring++;
  }
  unset_var("OPTARG");
  if (optind > argc) {
    set_var(var, "?");
    set_optind(optind);
    getopts_pos = 1;
    return 1;
  }
  const char *arg = list[optind - 1];
  if (getopts_pos == 1) {
    if (arg[0] != '-' || arg[1] == '\0') {
      set_var(var, "?");
      set_optind(optind);
      return 1;
    }
    if (strcmp(arg, "--") == 0) {
      set_var(var, "?");
      set_optind(optind + 1);
      return 1;
    }
  }
  char c = arg[getopts_pos++];
  bool last_in_arg = arg[getopts_pos] == '\0';
  const char *spec = c != ':' ? strchr(optstring, c) : NULL;
  char letter[2] = {c, '\0'};
  if (spec == NULL) {
    if (silent) {
      set_var("OPTARG", letter);
    } else {
      shell_error("illegal option -- %s", letter, NULL);
    }
    set_var(var, "?");
    if (last_in_arg) {
      optind++;
      getopts_pos = 1;
    }
  } else if (spec[1] == ':') {
    if (!last_in_arg) {
      set_var("OPTARG", arg + getopts_pos);
      optind++;
    } else if (optind < argc) {
      set_var("OPTARG", list[optind]);
      optind += 2;
    } else {
      if (silent) {
        set_var("OPTARG", letter);
        set_var(var, ":");
      } else {
        shell_error("option requires an argument -- %s", letter, NULL);
        set_var(var, "?");
      }
      getopts_pos = 1;
      set_optind(optind + 1);
      return 0;
    }
    getopts_pos = 1;
    set_var(var, letter);
  } else {
    set_var(var, letter);
    if (last_in_arg) {
      optind++;
      getopts_pos = 1;
    }
  }
  set_optind(optind);
  return 0;
}

// The hash [-r] builtin. Lists, adds or clears remembered command paths.
static int bi_hash(char **args) {
  if (args[1] != NULL && strcmp(args[1], "-r") == 0) {
    clear_hash();
    return 0;
  }
  if (args[1] == NULL) {
    for (int i = 0; i < nhash; i++) {
      printf("%s\n", hash_paths[i]);
    }
    return 0;
  }
  int status = 0;
  for (int i = 1; args[i] != NULL; i++) {
    if (find_builtin(args[i]) != NULL || find_func(args[i]) != NULL) {
      continue;
    }
    char *path = find_command(args[i]);
    if (path == NULL) {
      shell_error("hash: %s: not found", args[i], NULL);
      status = 1;
    }
    free(path);
  }
  return status;
}

// Reads one byte from stdin for read. Returns false at end of input,
// or when a trapped signal arrives.
static bool read_byte(char *c, int *status) {
  while (true) {
    ssize_t r = read(STDIN_FILENO, c, 1);
    if (r == 1) {
      return true;
    }
    if (r < 0 && errno == EINTR) {
      if (pending_any || got_sigint) {
        *status = 128 + first_pending_signal();
        return false;
      }
      continue;
    }
    *status = 1;
    return false;
  }
}

// True if the character at i is an unescaped IFS character.
static bool read_is_ifs(Field *f, size_t i, const char *ifs) {
  return f->flags.data[i] == 0 && strchr(ifs, f->chars.data[i]) != NULL &&
         f->chars.data[i] != '\0';
}

// The read [-r] builtin. Reads a line and splits it into the named variables.
static int bi_read(char **args) {
  bool raw = false;
  int i = 1;
  while (args[i] != NULL && args[i][0] == '-' && args[i][1] != '\0') {
    if (strcmp(args[i], "-r") == 0) {
      raw = true;
    } else if (strcmp(args[i], "--") == 0) {
      i++;
      break;
    } else {
      shell_error("read: %s: invalid option", args[i], NULL);
      return 2;
    }
    i++;
  }
  char *reply[] = {"REPLY", NULL};
  char **names = args[i] != NULL ? args + i : reply;
  int nnames = count_args(names);
  for (int k = 0; k < nnames; k++) {
    if (!is_valid_name(names[k])) {
      shell_error("read: %s: bad variable name", names[k], NULL);
      return 2;
    }
  }
  Field line = {0};
  int status = 0;
  char c;
  while (read_byte(&c, &status)) {
    if (c == '\n') {
      break;
    }
    if (!raw && c == '\\') {
      char next;
      if (!read_byte(&next, &status)) {
        break;
      }
      if (next == '\n') {
        continue;
      }
      field_add(&line, next, Q_QUOTED);
      continue;
    }
    field_add(&line, c, 0);
  }
  if (status > 128) {
    field_free(&line);
    return status;
  }
  const char *ifs = ifs_value();
  size_t n = line.chars.len;
  size_t pos = 0;
  for (int k = 0; k < nnames; k++) {
    while (pos < n && read_is_ifs(&line, pos, ifs) &&
           is_ifs_space(line.chars.data[pos])) {
      pos++;
    }
    Str value = {0};
    if (k == nnames - 1) {
      size_t end = n;
      while (end > pos && read_is_ifs(&line, end - 1, ifs) &&
             is_ifs_space(line.chars.data[end - 1])) {
        end--;
      }
      str_add_len(&value, line.chars.data != NULL ? line.chars.data + pos : "",
                  end - pos);
      pos = n;
    } else {
      while (pos < n && !read_is_ifs(&line, pos, ifs)) {
        str_add_char(&value, line.chars.data[pos++]);
      }
      while (pos < n && read_is_ifs(&line, pos, ifs) &&
             is_ifs_space(line.chars.data[pos])) {
        pos++;
      }
      if (pos < n && read_is_ifs(&line, pos, ifs)) {
        pos++;
      }
    }
    if (!set_var(names[k], str_cstr(&value))) {
      status = 2;
    }
    str_free(&value);
  }
  field_free(&line);
  return status;
}

// The umask [-S] [mode] builtin. Shows or sets the file creation mask,
// in octal or symbolic form.
static int bi_umask(char **args) {
  int i = 1;
  bool symbolic = false;
  if (args[i] != NULL && strcmp(args[i], "-S") == 0) {
    symbolic = true;
    i++;
  }
  mode_t mask = umask(0);
  umask(mask);
  if (args[i] == NULL) {
    if (!symbolic) {
      printf("%04o\n", (unsigned)mask);
      return 0;
    }
    mode_t allowed = ~mask & 0777;
    const char *who = "ugo";
    for (int k = 0; k < 3; k++) {
      int shift = 6 - 3 * k;
      printf("%c=", who[k]);
      if (allowed & (4 << shift)) {
        printf("r");
      }
      if (allowed & (2 << shift)) {
        printf("w");
      }
      if (allowed & (1 << shift)) {
        printf("x");
      }
      printf(k < 2 ? "," : "\n");
    }
    return 0;
  }
  const char *m = args[i];
  if (isdigit((unsigned char)m[0])) {
    char *end;
    long v = strtol(m, &end, 8);
    if (*end != '\0' || v < 0 || v > 0777) {
      shell_error("umask: %s: bad mask", m, NULL);
      return 1;
    }
    umask((mode_t)v);
    return 0;
  }
  mode_t allowed = ~mask & 0777;
  const char *p = m;
  while (*p != '\0') {
    mode_t who = 0;
    while (*p == 'u' || *p == 'g' || *p == 'o' || *p == 'a') {
      if (*p == 'u') {
        who |= 0700;
      } else if (*p == 'g') {
        who |= 0070;
      } else if (*p == 'o') {
        who |= 0007;
      } else {
        who |= 0777;
      }
      p++;
    }
    if (who == 0) {
      who = 0777;
    }
    if (*p != '+' && *p != '-' && *p != '=') {
      shell_error("umask: %s: bad mask", m, NULL);
      return 1;
    }
    while (*p == '+' || *p == '-' || *p == '=') {
      char op = *p++;
      mode_t bits = 0;
      while (*p == 'r' || *p == 'w' || *p == 'x') {
        if (*p == 'r') {
          bits |= 0444;
        } else if (*p == 'w') {
          bits |= 0222;
        } else {
          bits |= 0111;
        }
        p++;
      }
      bits &= who;
      if (op == '+') {
        allowed |= bits;
      } else if (op == '-') {
        allowed &= ~bits;
      } else {
        allowed = (allowed & ~who) | bits;
      }
    }
    if (*p == ',') {
      p++;
    } else if (*p != '\0') {
      shell_error("umask: %s: bad mask", m, NULL);
      return 1;
    }
  }
  umask(~allowed & 0777);
  return 0;
}

// The echo builtin. Takes no options. Backslash escapes work as POSIX (XSI)
// describes, and \c ends the output with no newline.
static int bi_echo(char **args) {
  int i = 1;
  bool newline = true;
  Str out = {0};
  bool stop = false;
  for (int first = i; args[i] != NULL && !stop; i++) {
    if (i > first) {
      str_add_char(&out, ' ');
    }
    for (const char *p = args[i]; *p != '\0'; p++) {
      if (*p != '\\' || p[1] == '\0') {
        str_add_char(&out, *p);
        continue;
      }
      p++;
      char c = *p;
      if (c == 'a') {
        str_add_char(&out, '\a');
      } else if (c == 'b') {
        str_add_char(&out, '\b');
      } else if (c == 'c') {
        stop = true;
        newline = false;
        break;
      } else if (c == 'f') {
        str_add_char(&out, '\f');
      } else if (c == 'n') {
        str_add_char(&out, '\n');
      } else if (c == 'r') {
        str_add_char(&out, '\r');
      } else if (c == 't') {
        str_add_char(&out, '\t');
      } else if (c == 'v') {
        str_add_char(&out, '\v');
      } else if (c == '\\') {
        str_add_char(&out, '\\');
      } else if (c == '0') {
        int value = 0;
        for (int k = 0; k < 3 && p[1] >= '0' && p[1] <= '7'; k++) {
          p++;
          value = value * 8 + (*p - '0');
        }
        str_add_char(&out, (char)value);
      } else {
        str_add_char(&out, '\\');
        str_add_char(&out, c);
      }
    }
  }
  if (newline) {
    str_add_char(&out, '\n');
  }
  fwrite(str_cstr(&out), 1, out.len, stdout);
  fflush(stdout);
  str_free(&out);
  return 0;
}

// True if a signal arrived while wait was waiting.
static bool wait_interrupted(void) {
  return pending_any || got_sigint;
}

// The wait [pid...] builtin. Waits for the given background processes, or for all of them.
static int bi_wait(char **args) {
  got_sigint = 0;
  wait_interruptible = true;
  int status = 0;
  if (args[1] == NULL) {
    for (int i = 0; i < MAX_JOBS && !wait_interrupted(); i++) {
      if (background_pids[i] != 0) {
        int s = wait_background(i);
        if (wait_interrupted()) {
          status = s;
        }
      }
    }
  } else {
    for (int i = 1; args[i] != NULL && !wait_interrupted(); i++) {
      long pid;
      if (!parse_int(args[i], &pid) || pid <= 0) {
        shell_error("wait: %s: not a pid", args[i], NULL);
        status = 2;
        continue;
      }
      int slot = background_slot((pid_t)pid);
      if (slot < 0) {
        if (!recall_status((pid_t)pid, &status)) {
          status = 127;
        }
        continue;
      }
      status = wait_background(slot);
    }
  }
  wait_interruptible = false;
  if (wait_interrupted()) {
    if (got_sigint && interactive) {
      fprintf(stderr, "\n");
    }
    got_sigint = 0;
    return 128 + first_pending_signal();
  }
  return status;
}

// Handles kill -l: lists signal names, or names the signal for a status.
static int list_signals(char **args) {
  if (args[0] == NULL) {
    for (int i = 0; i < signal_count(); i++) {
      printf("%s%s", signal_names[i], i == signal_count() - 1 ? "\n" : " ");
    }
    return 0;
  }
  int status = 0;
  for (int i = 0; args[i] != NULL; i++) {
    int n = atoi(args[i]);
    if (n > 128) {
      n -= 128;
    }
    const char *name = signal_name(n);
    if (name == NULL) {
      fprintf(stderr, "kill: %s: invalid signal\n", args[i]);
      status = 1;
    } else {
      printf("%s\n", name);
    }
  }
  return status;
}

// The kill builtin. Sends a signal to processes.
static int bi_kill(char **args) {
  int sig = SIGTERM;
  int i = 1;
  if (args[i] != NULL && strcmp(args[i], "-l") == 0) {
    return list_signals(args + 2);
  }
  if (args[i] != NULL && strcmp(args[i], "-s") == 0) {
    if (args[i + 1] == NULL) {
      fprintf(stderr, "kill: -s needs a signal name\n");
      return 2;
    }
    sig = parse_signal(args[i + 1]);
    if (sig < 0) {
      fprintf(stderr, "kill: %s: invalid signal\n", args[i + 1]);
      return 2;
    }
    i += 2;
  } else if (args[i] != NULL && args[i][0] == '-' && args[i][1] != '\0' &&
             strcmp(args[i], "--") != 0) {
    sig = parse_signal(args[i] + 1);
    if (sig < 0) {
      fprintf(stderr, "kill: %s: invalid signal\n", args[i] + 1);
      return 2;
    }
    i++;
  }
  if (args[i] != NULL && strcmp(args[i], "--") == 0) {
    i++;
  }
  if (args[i] == NULL) {
    fprintf(stderr, "kill: usage: kill [-s sig | -sig] pid ...\n");
    return 2;
  }
  int status = 0;
  for (; args[i] != NULL; i++) {
    long n;
    if (!parse_int(args[i], &n)) {
      fprintf(stderr, "kill: %s: not a pid\n", args[i]);
      status = 1;
      continue;
    }
    if (kill((pid_t)n, sig) < 0) {
      fprintf(stderr, "kill: %s: %s\n", args[i], strerror(errno));
      status = 1;
    }
  }
  return status;
}

const Builtin builtin_table[] = {
    {":", bi_colon, true},         {".", bi_dot, true},
    {"break", bi_break, true},     {"continue", bi_continue, true},
    {"eval", bi_eval, true},       {"exec", bi_exec, true},
    {"exit", bi_exit, true},       {"export", bi_export, true},
    {"readonly", bi_readonly, true}, {"return", bi_return, true},
    {"set", bi_set, true},         {"shift", bi_shift, true},
    {"times", bi_times, true},     {"trap", bi_trap, true},
    {"unset", bi_unset, true},     {"alias", bi_alias, false},
    {"cd", bi_cd, false},          {"command", bi_command, false},
    {"echo", bi_echo, false},      {"false", bi_false, false},
    {"getopts", bi_getopts, false}, {"hash", bi_hash, false},
    {"kill", bi_kill, false},
    {"pwd", bi_pwd, false},        {"read", bi_read, false},
    {"true", bi_colon, false},     {"type", bi_type, false},
    {"umask", bi_umask, false},    {"unalias", bi_unalias, false},
    {"wait", bi_wait, false}};

static const int builtin_count = sizeof(builtin_table) / sizeof(builtin_table[0]);

// Finds a builtin by name, or returns NULL.
const Builtin *find_builtin(const char *name) {
  for (int i = 0; i < builtin_count; i++) {
    if (strcmp(builtin_table[i].name, name) == 0) {
      return &builtin_table[i];
    }
  }
  return NULL;
}

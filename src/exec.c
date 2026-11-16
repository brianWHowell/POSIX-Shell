#include "shell.h"

static int exec_node(Node *n);

static bool in_trap = false;
static int cmdsub_status = 0;
static int errexit_off = 0;
static bool exiting = false;

// Remembers what fd pointed at before a redirection changes it.
static void save_fd(FdSave *save, int fd) {
  if (save == NULL) {
    return;
  }
  for (int i = 0; i < save->n; i++) {
    if (save->items[i].fd == fd) {
      return;
    }
  }
  if (save->n == MAX_SAVED_FDS) {
    return;
  }
  save->items[save->n].fd = fd;
  save->items[save->n].saved = fcntl(fd, F_DUPFD_CLOEXEC, 10);
  save->n++;
}

// Puts back every file descriptor saved by save_fd.
static void restore_fds(FdSave *save) {
  for (int i = save->n - 1; i >= 0; i--) {
    int fd = save->items[i].fd;
    int saved = save->items[i].saved;
    if (saved >= 0) {
      dup2(saved, fd);
      close(saved);
    } else {
      close(fd);
    }
  }
  save->n = 0;
}

// Discards the saved copies, so the redirections stay. Used by exec.
static void forget_fds(FdSave *save) {
  for (int i = 0; i < save->n; i++) {
    if (save->items[i].saved >= 0) {
      close(save->items[i].saved);
    }
  }
  save->n = 0;
}

// Writes a here-doc body to a temporary file and returns a descriptor to read it.
static int heredoc_fd(Redir *r) {
  const char *raw = r->heredoc != NULL ? r->heredoc : "";
  char *body = r->heredoc_quoted ? xstrdup(raw) : expand_heredoc(raw);
  if (body == NULL) {
    return -1;
  }
  const char *dir = get_var("TMPDIR");
  Str path = {0};
  str_add(&path, dir != NULL && dir[0] != '\0' ? dir : "/tmp");
  str_add(&path, "/shXXXXXX");
  int fd = mkstemp(path.data);
  if (fd < 0) {
    shell_error("cannot create here-document: %s", strerror(errno), NULL);
    free(body);
    str_free(&path);
    return -1;
  }
  unlink(path.data);
  str_free(&path);
  size_t len = strlen(body);
  size_t done = 0;
  while (done < len) {
    ssize_t w = write(fd, body + done, len - done);
    if (w <= 0) {
      break;
    }
    done += (size_t)w;
  }
  free(body);
  lseek(fd, 0, SEEK_SET);
  return fd;
}

// Opens the file a redirection names, with the right mode for <, >, >>, <> or >|.
static int open_redirect_target(Redir *r, const char *target) {
  int flags = O_RDONLY;
  if (r->type == T_GREAT) {
    flags = opt('C') ? O_WRONLY | O_CREAT | O_EXCL : O_WRONLY | O_CREAT | O_TRUNC;
  } else if (r->type == T_CLOBBER) {
    flags = O_WRONLY | O_CREAT | O_TRUNC;
  } else if (r->type == T_DGREAT) {
    flags = O_WRONLY | O_CREAT | O_APPEND;
  } else if (r->type == T_LESSGREAT) {
    flags = O_RDWR | O_CREAT;
  }
  int fd = open(target, flags, 0666);
  if (fd < 0 && r->type == T_GREAT && opt('C') && errno == EEXIST) {
    struct stat st;
    if (stat(target, &st) == 0 && !S_ISREG(st.st_mode)) {
      fd = open(target, O_WRONLY);
    } else {
      shell_error("%s: cannot overwrite existing file", target, NULL);
      return -1;
    }
  }
  if (fd < 0) {
    shell_error("%s: %s", target, strerror(errno));
  }
  return fd;
}

// Performs one redirection. Saves the old descriptor first if save is given.
static int apply_redir(Redir *r, FdSave *save) {
  if (r->type == T_DLESS || r->type == T_DLESSDASH) {
    int fd = heredoc_fd(r);
    if (fd < 0) {
      return -1;
    }
    save_fd(save, r->fd);
    if (fd != r->fd) {
      dup2(fd, r->fd);
      close(fd);
    }
    return 0;
  }
  char *target = expand_string(r->target);
  if (target == NULL) {
    return -1;
  }
  if (r->type == T_LESSAND || r->type == T_GREATAND) {
    long n;
    if (strcmp(target, "-") == 0) {
      save_fd(save, r->fd);
      close(r->fd);
    } else if (!parse_int(target, &n) || n < 0 || n > INT_MAX ||
               fcntl((int)n, F_GETFD) < 0) {
      shell_error("%s: bad file descriptor", target, NULL);
      free(target);
      return -1;
    } else if ((int)n != r->fd) {
      save_fd(save, r->fd);
      dup2((int)n, r->fd);
    }
    free(target);
    return 0;
  }
  save_fd(save, r->fd);
  int fd = open_redirect_target(r, target);
  free(target);
  if (fd < 0) {
    return -1;
  }
  if (fd != r->fd) {
    dup2(fd, r->fd);
    close(fd);
  }
  return 0;
}

// Performs a list of redirections in order. Returns -1 if any fails.
static int apply_redirs(Redir *r, FdSave *save) {
  for (; r != NULL; r = r->next) {
    if (apply_redir(r, save) < 0) {
      return -1;
    }
  }
  return 0;
}

typedef struct {
  char *name;
  char *value;
  bool existed;
  bool exported;
} TempVar;

// Sets the NAME=value assignments written before a builtin or function,
// remembering the old values.
static TempVar *push_temp(char **names, char **values, int n, bool export_them) {
  TempVar *t = xmalloc((n + 1) * sizeof(TempVar));
  for (int i = 0; i < n; i++) {
    Var *v = find_var(names[i]);
    t[i].name = xstrdup(names[i]);
    t[i].existed = v != NULL;
    t[i].value = v != NULL && v->value != NULL ? xstrdup(v->value) : NULL;
    t[i].exported = v != NULL && v->exported;
    if (set_var(names[i], values[i]) && export_them) {
      find_var(names[i])->exported = true;
    }
  }
  return t;
}

// Puts back the variables changed by push_temp.
static void pop_temp(TempVar *t, int n) {
  for (int i = n - 1; i >= 0; i--) {
    Var *v = find_var(t[i].name);
    if (!t[i].existed) {
      if (v != NULL && !v->readonly) {
        unset_var(t[i].name);
      }
      free(t[i].value);
    } else {
      v = add_var(t[i].name);
      free(v->value);
      v->value = t[i].value;
      v->exported = t[i].exported;
      if (strcmp(t[i].name, "PATH") == 0) {
        clear_hash();
      }
    }
    free(t[i].name);
  }
  free(t);
}

// True if a word must be quoted to be shown correctly in set -x output.
static bool needs_quotes(const char *s) {
  if (s[0] == '\0') {
    return true;
  }
  for (; *s != '\0'; s++) {
    if (!isalnum((unsigned char)*s) && strchr("_-./=:,+%@", *s) == NULL) {
      return true;
    }
  }
  return false;
}

// Prints a command to stderr after PS4, for set -x.
static void trace_command(char **names, char **values, int n, char **args) {
  const char *ps4 = get_var("PS4");
  char *prefix = expand_prompt(ps4 != NULL ? ps4 : "+ ");
  Str line = {0};
  str_add(&line, prefix);
  free(prefix);
  bool first = true;
  for (int i = 0; i < n; i++) {
    if (!first) {
      str_add_char(&line, ' ');
    }
    first = false;
    str_add(&line, names[i]);
    str_add_char(&line, '=');
    if (needs_quotes(values[i])) {
      quote_value(&line, values[i]);
    } else {
      str_add(&line, values[i]);
    }
  }
  for (int i = 0; args != NULL && args[i] != NULL; i++) {
    if (!first) {
      str_add_char(&line, ' ');
    }
    first = false;
    if (needs_quotes(args[i])) {
      quote_value(&line, args[i]);
    } else {
      str_add(&line, args[i]);
    }
  }
  fprintf(stderr, "%s\n", str_cstr(&line));
  str_free(&line);
}

// Handles a failed expansion: a script exits, an interactive shell carries on.
static int expansion_error(void) {
  if (!interactive) {
    exit_shell(2);
  }
  return 2;
}

// Runs a file with no #! line as a shell script, in the child process.
void run_script_child(const char *path, char **args) {
  for (int i = nvars - 1; i >= 0; i--) {
    if (!vars[i].exported) {
      vars[i].readonly = false;
      unset_var(vars[i].name);
    }
  }
  while (nfuncs > 0) {
    remove_func(funcs[0]->name);
  }
  while (naliases > 0) {
    remove_alias(alias_names[0]);
  }
  int count = 0;
  while (args[count + 1] != NULL) {
    count++;
  }
  shell_name = xstrdup(path);
  set_params(args + 1, count);
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    shell_error("%s: %s", path, strerror(errno));
    _exit(126);
  }
  Source src;
  memset(&src, 0, sizeof(src));
  src.fd = move_fd_high(fd);
  last_status = 0;
  run_source(&src);
  exit_shell(last_status);
}

// Replaces the current process with an external command.
// Only called in a child, and never returns.
static void exec_external(char **args, char **names, char **values, int n,
                          Redir *redirs, const char *path_override) {
  for (int i = 0; i < n; i++) {
    if (set_var(names[i], values[i])) {
      find_var(names[i])->exported = true;
    }
  }
  if (apply_redirs(redirs, NULL) < 0) {
    _exit(1);
  }
  char *path = path_override != NULL ? search_path(args[0], path_override)
                                      : find_command(args[0]);
  if (path == NULL) {
    shell_error("%s: not found", args[0], NULL);
    _exit(127);
  }
  char **env = build_env();
  execve(path, args, env);
  int err = errno;
  if (err == ENOEXEC) {
    run_script_child(path, args);
  }
  if (err == ENOENT) {
    shell_error("%s: not found", args[0], NULL);
    _exit(127);
  }
  shell_error("%s: %s", args[0], strerror(err));
  _exit(126);
}

// Forks and runs an external command, then waits for it.
int fork_external(char **args, char **names, char **values, int n,
                  Redir *redirs, const char *path_override) {
  bool path_assigned = path_override != NULL;
  for (int i = 0; i < n; i++) {
    if (strcmp(names[i], "PATH") == 0) {
      path_assigned = true;
    }
  }
  if (!path_assigned) {
    free(find_command(args[0]));
  }
  pid_t pid = fork();
  if (pid < 0) {
    shell_error("fork: %s", strerror(errno), NULL);
    return 2;
  }
  if (pid == 0) {
    enter_subshell();
    exec_external(args, names, values, n, redirs, path_override);
  }
  Job job;
  memset(&job, 0, sizeof(job));
  job.status = 1;
  job.pids[0] = pid;
  job.npids = 1;
  return wait_job(&job);
}

// Calls a shell function with args as $1, $2... and returns its status.
static int call_function(Func *f, char **args) {
  if (func_depth >= MAX_FUNC_DEPTH) {
    shell_error("%s: function nesting too deep", f->name, NULL);
    return 2;
  }
  SavedParams saved = save_params();
  int count = 0;
  while (args[count + 1] != NULL) {
    count++;
  }
  set_params(args + 1, count);
  int saved_loop_depth = loop_depth;
  loop_depth = 0;
  func_depth++;
  f->running++;
  int status = exec_node(f->body);
  f->running--;
  func_depth--;
  loop_depth = saved_loop_depth;
  loop_break = 0;
  loop_continue = 0;
  if (returning) {
    returning = false;
    status = return_status;
  }
  restore_params(saved);
  if (f->stale && f->running == 0) {
    release_func(f);
  }
  return status;
}

// True for export and readonly, whose arguments expand like assignments.
static bool is_declaration(const char *raw) {
  return strcmp(raw, "export") == 0 || strcmp(raw, "readonly") == 0;
}

// Expands a simple command's words into its argument list.
static bool expand_command_words(Node *n, StrList *argv) {
  if (n->nwords == 0) {
    list_items(argv);
    return true;
  }
  if (!is_declaration(n->words[0])) {
    return expand_words(n->words, n->nwords, argv);
  }
  for (int i = 0; i < n->nwords; i++) {
    const char *raw = n->words[i];
    if (i > 0 && is_assignment_word(raw)) {
      const char *eq = strchr(raw, '=');
      char *value = expand_assignment(eq + 1);
      if (value == NULL) {
        return false;
      }
      Str s = {0};
      str_add_len(&s, raw, eq - raw + 1);
      str_add(&s, value);
      free(value);
      list_add(argv, str_take(&s));
    } else if (!expand_words(&n->words[i], 1, argv)) {
      return false;
    }
  }
  list_items(argv);
  return true;
}

// Runs a command that is only assignments and redirections, such as x=1.
static int exec_assign_only(Node *n, char **names, char **values) {
  int status = 0;
  if (opt('x') && n->nassigns > 0) {
    trace_command(names, values, n->nassigns, NULL);
  }
  for (int i = 0; i < n->nassigns; i++) {
    if (!set_var(names[i], values[i])) {
      status = 1;
      if (!interactive) {
        exit_shell(2);
      }
    }
  }
  FdSave save = {0};
  if (apply_redirs(n->redirs, &save) < 0) {
    status = 1;
  }
  restore_fds(&save);
  return status != 0 ? status : cmdsub_status;
}

// Frees the expanded names and values of a command's assignments.
static void free_assigns(char **names, char **values, int n) {
  for (int i = 0; i < n; i++) {
    free(names[i]);
    free(values[i]);
  }
  free(names);
  free(values);
}

// Runs a simple command. Looks it up as a special builtin, then a function,
// then a builtin, then on PATH. no_fork runs an external command in place.
static int exec_simple(Node *n, bool no_fork) {
  current_lineno = n->lineno;
  set_var_int("LINENO", n->lineno);
  cmdsub_status = 0;
  StrList argv = {0};
  if (!expand_command_words(n, &argv)) {
    list_free(&argv);
    return expansion_error();
  }
  char **names = xmalloc((n->nassigns + 1) * sizeof(char *));
  char **values = xmalloc((n->nassigns + 1) * sizeof(char *));
  for (int i = 0; i < n->nassigns; i++) {
    const char *raw = n->assigns[i];
    const char *eq = strchr(raw, '=');
    names[i] = xstrndup(raw, eq - raw);
    values[i] = expand_assignment(eq + 1);
    if (values[i] == NULL) {
      free_assigns(names, values, i + 1);
      list_free(&argv);
      return expansion_error();
    }
  }
  int na = n->nassigns;
  int status;
  if (argv.len == 0) {
    status = exec_assign_only(n, names, values);
    free_assigns(names, values, na);
    list_free(&argv);
    return status;
  }
  char **args = argv.items;
  if (opt('x')) {
    trace_command(names, values, na, args);
  }
  const Builtin *b = find_builtin(args[0]);
  Func *f = NULL;
  if (b == NULL || !b->special) {
    f = find_func(args[0]);
  }
  if (b != NULL && b->special && f == NULL) {
    bool ok = true;
    for (int i = 0; i < na; i++) {
      if (!set_var(names[i], values[i])) {
        ok = false;
      }
    }
    FdSave save = {0};
    if (!ok) {
      status = 1;
      if (!interactive) {
        exit_shell(2);
      }
    } else if (apply_redirs(n->redirs, &save) < 0) {
      restore_fds(&save);
      status = 1;
      if (!interactive) {
        exit_shell(2);
      }
    } else {
      special_error = false;
      status = b->fn(args);
      if (strcmp(args[0], "exec") == 0 && args[1] == NULL) {
        forget_fds(&save);
      } else {
        restore_fds(&save);
      }
      if (special_error && !interactive) {
        exit_shell(status != 0 ? status : 2);
      }
      special_error = false;
    }
  } else if (f != NULL || b != NULL) {
    TempVar *temp = push_temp(names, values, na, f != NULL);
    FdSave save = {0};
    if (apply_redirs(n->redirs, &save) < 0) {
      status = 1;
    } else if (f != NULL) {
      status = call_function(f, args);
    } else {
      status = b->fn(args);
    }
    restore_fds(&save);
    pop_temp(temp, na);
  } else if (no_fork) {
    exec_external(args, names, values, na, n->redirs, NULL);
    status = 127;
  } else {
    status = fork_external(args, names, values, na, n->redirs, NULL);
  }
  free_assigns(names, values, na);
  list_free(&argv);
  return status;
}

// Runs one command of a pipeline, simple or compound.
static int exec_command(Node *c, bool no_fork) {
  if (c->type == N_SIMPLE) {
    return exec_simple(c, no_fork);
  }
  return exec_node(c);
}

// Runs a pipeline of two or more commands, each in its own child process.
static int exec_multi(Node *n) {
  Job job;
  memset(&job, 0, sizeof(job));
  job.status = 1;
  int prev_read = -1;
  for (int i = 0; i < n->ncmds; i++) {
    int fds[2] = {-1, -1};
    if (i < n->ncmds - 1 && pipe(fds) < 0) {
      shell_error("pipe: %s", strerror(errno), NULL);
      break;
    }
    pid_t pid = fork();
    if (pid < 0) {
      shell_error("fork: %s", strerror(errno), NULL);
      if (fds[0] != -1) {
        close(fds[0]);
        close(fds[1]);
      }
      break;
    }
    if (pid == 0) {
      enter_subshell();
      if (prev_read != -1) {
        dup2(prev_read, STDIN_FILENO);
        close(prev_read);
      }
      if (fds[1] != -1) {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
      }
      int status = exec_command(n->cmds[i], true);
      exit_shell(status);
    }
    job.pids[job.npids++] = pid;
    if (prev_read != -1) {
      close(prev_read);
    }
    if (fds[1] != -1) {
      close(fds[1]);
    }
    prev_read = fds[0];
  }
  if (prev_read != -1) {
    close(prev_read);
  }
  if (job.npids == 0) {
    return 1;
  }
  return wait_job(&job);
}

// Runs a pipeline, applies ! to its status, and exits if set -e says so.
static int exec_pipeline(Node *n) {
  if (n->bang) {
    errexit_off++;
  }
  int status;
  if (n->ncmds == 1) {
    status = exec_command(n->cmds[0], false);
  } else {
    status = exec_multi(n);
  }
  if (n->bang) {
    errexit_off--;
    return status == 0 ? 1 : 0;
  }
  if (opt('e') && status != 0 && errexit_off == 0 && !returning &&
      loop_break == 0 && loop_continue == 0) {
    exit_shell(status);
  }
  return status;
}

// Runs a command in the background with &.
static int exec_background(Node *n) {
  pid_t pid = fork();
  if (pid < 0) {
    shell_error("fork: %s", strerror(errno), NULL);
    return 2;
  }
  if (pid == 0) {
    in_background = true;
    enter_subshell();
    int fd = open("/dev/null", O_RDONLY);
    if (fd >= 0) {
      dup2(fd, STDIN_FILENO);
      close(fd);
    }
    Node *item = n->left;
    int status;
    if (item->type == N_PIPE && item->ncmds == 1 && !item->bang) {
      status = exec_command(item->cmds[0], true);
    } else {
      status = exec_node(item);
    }
    exit_shell(status);
  }
  last_bg_pid = pid;
  add_background(pid);
  return 0;
}

// Runs ( list ) in a forked child, so changes don't affect the shell.
static int exec_subshell(Node *n) {
  pid_t pid = fork();
  if (pid < 0) {
    shell_error("fork: %s", strerror(errno), NULL);
    return 2;
  }
  if (pid == 0) {
    enter_subshell();
    int status = exec_list(n->body);
    exit_shell(status);
  }
  Job job;
  memset(&job, 0, sizeof(job));
  job.status = 1;
  job.pids[0] = pid;
  job.npids = 1;
  return wait_job(&job);
}

// True if a break, continue or return is unwinding.
bool loop_should_stop(void) {
  return loop_break > 0 || loop_continue > 0 || returning;
}

// Handles break and continue after a loop body.
// Sets stop when the loop should end.
static bool loop_after_body(bool *stop) {
  if (loop_break > 0) {
    loop_break--;
    *stop = true;
    return true;
  }
  if (loop_continue > 0) {
    loop_continue--;
    if (loop_continue > 0) {
      *stop = true;
    }
    return true;
  }
  if (returning) {
    *stop = true;
    return true;
  }
  return false;
}

// Runs a while or until loop.
static int exec_while(Node *n) {
  int status = 0;
  loop_depth++;
  while (true) {
    errexit_off++;
    int c = exec_list(n->cond);
    errexit_off--;
    bool stop = false;
    if (loop_should_stop()) {
      loop_after_body(&stop);
      if (stop) {
        break;
      }
      continue;
    }
    if ((c == 0) != (n->type == N_WHILE)) {
      break;
    }
    status = exec_list(n->body);
    if (loop_after_body(&stop) && stop) {
      break;
    }
  }
  loop_depth--;
  return status;
}

// Runs a for loop over the expanded words, or over $@ when there is no in.
static int exec_for(Node *n) {
  StrList words = {0};
  if (n->has_in) {
    if (!expand_words(n->words, n->nwords, &words)) {
      list_free(&words);
      return expansion_error();
    }
  } else {
    for (int i = 0; i < nparams; i++) {
      list_add(&words, xstrdup(params[i]));
    }
  }
  int status = 0;
  loop_depth++;
  for (int i = 0; i < words.len; i++) {
    if (!set_var(n->name, words.items[i])) {
      status = 1;
      break;
    }
    status = exec_list(n->body);
    bool stop = false;
    if (loop_after_body(&stop) && stop) {
      break;
    }
  }
  loop_depth--;
  list_free(&words);
  return status;
}

// Runs the first case branch whose pattern matches the word.
static int exec_case(Node *n) {
  char *word = expand_string(n->name);
  if (word == NULL) {
    return expansion_error();
  }
  int status = 0;
  bool matched = false;
  for (CaseItem *it = n->items; it != NULL && !matched; it = it->next) {
    for (int i = 0; i < it->npatterns && !matched; i++) {
      char *pat = expand_pattern(it->patterns[i]);
      if (pat == NULL) {
        free(word);
        return expansion_error();
      }
      if (fnmatch(pat, word, 0) == 0) {
        matched = true;
        status = it->body != NULL ? exec_list(it->body) : 0;
      }
      free(pat);
    }
  }
  free(word);
  return status;
}

// Runs an if command, including its elif and else parts.
static int exec_if(Node *n) {
  errexit_off++;
  int c = exec_list(n->cond);
  errexit_off--;
  if (loop_should_stop()) {
    return c;
  }
  if (c == 0) {
    return exec_list(n->body);
  }
  if (n->else_part == NULL) {
    return 0;
  }
  if (n->else_part->type == N_IF && n->else_part->next == NULL) {
    return exec_node(n->else_part);
  }
  return exec_list(n->else_part);
}

// Stores a function definition. The body is copied so it outlives the command.
static void define_function(Node *n) {
  Func *f = xmalloc(sizeof(Func));
  memset(f, 0, sizeof(*f));
  f->name = xstrdup(n->name);
  f->body = copy_node(&f->arena, n->body);
  f->body->next = NULL;
  add_func(f);
}

// Runs a compound command once its redirections are in place.
static int exec_compound_body(Node *n) {
  switch (n->type) {
    case N_GROUP:
      return exec_list(n->body);
    case N_SUBSHELL:
      return exec_subshell(n);
    case N_IF:
      return exec_if(n);
    case N_WHILE:
    case N_UNTIL:
      return exec_while(n);
    case N_FOR:
      return exec_for(n);
    case N_CASE:
      return exec_case(n);
    default:
      return 0;
  }
}

// Runs any node in the command tree and returns its status.
// Trap actions run afterwards.
static int exec_node(Node *n) {
  int status = 0;
  if (opt('n') && !interactive) {
    return 0;
  }
  if (n->type == N_PIPE) {
    status = exec_pipeline(n);
  } else if (n->type == N_AND || n->type == N_OR) {
    errexit_off++;
    status = exec_node(n->left);
    errexit_off--;
    if (!loop_should_stop() && ((n->type == N_AND) == (status == 0))) {
      status = exec_node(n->right);
    }
  } else if (n->type == N_BG) {
    status = exec_background(n);
  } else if (n->type == N_SIMPLE) {
    status = exec_simple(n, false);
  } else if (n->type == N_FUNC) {
    define_function(n);
    status = 0;
  } else {
    FdSave save = {0};
    if (apply_redirs(n->redirs, &save) < 0) {
      status = 1;
    } else {
      status = exec_compound_body(n);
    }
    restore_fds(&save);
  }
  last_status = status;
  run_pending_traps();
  return status;
}

// Runs a list of commands in order and returns the last status.
int exec_list(Node *n) {
  int status = 0;
  for (; n != NULL; n = n->next) {
    status = exec_node(n);
    if (loop_should_stop()) {
      break;
    }
  }
  return status;
}

// Runs body in a child process and returns everything it printed. Used for $(...).
char *command_subst(const char *body) {
  int fds[2];
  if (pipe(fds) < 0) {
    shell_error("pipe: %s", strerror(errno), NULL);
    return xstrdup("");
  }
  pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    shell_error("fork: %s", strerror(errno), NULL);
    return xstrdup("");
  }
  if (pid == 0) {
    close(fds[0]);
    if (fds[1] != STDOUT_FILENO) {
      dup2(fds[1], STDOUT_FILENO);
      close(fds[1]);
    }
    enter_subshell();
    int status = run_string(body);
    exit_shell(status);
  }
  close(fds[1]);
  Str out = {0};
  char buf[4096];
  while (true) {
    ssize_t r = read(fds[0], buf, sizeof(buf));
    if (r < 0 && errno == EINTR) {
      continue;
    }
    if (r <= 0) {
      break;
    }
    for (ssize_t i = 0; i < r; i++) {
      if (buf[i] != '\0') {
        str_add_char(&out, buf[i]);
      }
    }
  }
  close(fds[0]);
  int ws = 0;
  while (waitpid(pid, &ws, 0) < 0 && errno == EINTR) {
  }
  cmdsub_status = status_of(ws);
  last_status = cmdsub_status;
  return str_take(&out);
}

// Runs the EXIT trap, if one is set, once.
static void run_exit_trap(void) {
  if (trap_action[0] == NULL || trap_action[0][0] == '\0') {
    return;
  }
  char *action = trap_action[0];
  trap_action[0] = NULL;
  run_string(action);
  free(action);
}

// Exits the shell after running the EXIT trap and flushing output.
void exit_shell(int status) {
  if (!exiting) {
    exiting = true;
    last_status = status;
    returning = false;
    loop_break = 0;
    loop_continue = 0;
    run_exit_trap();
  }
  fflush(NULL);
  _exit(status & 0xFF);
}

// Runs the trap action for every signal that arrived since last time.
void run_pending_traps(void) {
  if (!pending_any || in_trap) {
    return;
  }
  in_trap = true;
  while (pending_any) {
    pending_any = 0;
    for (int sig = 1; sig < NSIG; sig++) {
      if (!pending_sig[sig]) {
        continue;
      }
      pending_sig[sig] = 0;
      if (trap_action[sig] == NULL || trap_action[sig][0] == '\0') {
        continue;
      }
      char *action = xstrdup(trap_action[sig]);
      int saved = last_status;
      run_string(action);
      last_status = saved;
      free(action);
    }
  }
  in_trap = false;
}

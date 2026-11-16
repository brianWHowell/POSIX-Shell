#include "shell.h"

enum { READ_LINE, READ_EOF, READ_INTERRUPT };
static int source_depth = 0;

// Reads the next line from a string source, such as eval or -c.
static int read_string_line(Source *src, Str *line) {
  if (src->text_pos >= src->text_len) {
    return READ_EOF;
  }
  size_t start = src->text_pos;
  while (src->text_pos < src->text_len && src->text[src->text_pos] != '\n') {
    src->text_pos++;
  }
  if (src->text_pos < src->text_len) {
    src->text_pos++;
  }
  str_add_len(line, src->text + start, src->text_pos - start);
  return READ_LINE;
}

// Reads one byte from a file source, buffered unless it must read
// one byte at a time.
static int read_fd_byte(Source *src, char *c) {
  while (true) {
    if (!src->unbuffered && src->buf_pos < src->buf_len) {
      *c = src->buf[src->buf_pos++];
      return READ_LINE;
    }
    ssize_t r;
    if (src->unbuffered) {
      r = read(src->fd, c, 1);
    } else {
      r = read(src->fd, src->buf, sizeof(src->buf));
    }
    if (r < 0 && errno == EINTR) {
      if (src->interactive && got_sigint) {
        return READ_INTERRUPT;
      }
      continue;
    }
    if (r <= 0) {
      src->at_eof = true;
      return READ_EOF;
    }
    if (src->unbuffered) {
      return READ_LINE;
    }
    src->buf_len = (size_t)r;
    src->buf_pos = 0;
  }
}

// Reads the next line from a file source. NUL bytes are skipped.
static int read_fd_line(Source *src, Str *line) {
  if (src->at_eof) {
    return READ_EOF;
  }
  bool any = false;
  while (true) {
    char c;
    int r = read_fd_byte(src, &c);
    if (r == READ_INTERRUPT) {
      return READ_INTERRUPT;
    }
    if (r == READ_EOF) {
      return any ? READ_LINE : READ_EOF;
    }
    any = true;
    if (c == '\0') {
      continue;
    }
    str_add_char(line, c);
    if (c == '\n') {
      return READ_LINE;
    }
  }
}

// Shows the prompt and reads a line from the user.
static int read_interactive_line(Source *src, Str *line, bool more) {
  if (!more) {
    reap_background();
  }
  run_pending_traps();
  got_sigint = 0;
  const char *ps = get_var(more ? "PS2" : "PS1");
  char *prompt = expand_prompt(ps != NULL ? ps : "");
  Str got = {0};
  fputs(prompt, stderr);
  fflush(stderr);
  int r = read_fd_line(src, &got);
  if (r == READ_INTERRUPT) {
    fputs("\n", stderr);
  } else if (r == READ_EOF) {
    src->at_eof = false;
    fputs("\n", stderr);
  }
  free(prompt);
  if (r == READ_LINE) {
    str_add_len(line, str_cstr(&got), got.len);
  }
  str_free(&got);
  return r;
}

// Reads the next line from any kind of source and appends it to line.
static int read_source_line(Source *src, Str *line, bool more) {
  size_t before = line->len;
  int r;
  if (src->text != NULL) {
    r = read_string_line(src, line);
  } else if (src->interactive) {
    r = read_interactive_line(src, line, more);
  } else {
    r = read_fd_line(src, line);
  }
  if (r == READ_LINE) {
    src->lineno++;
    if (opt('v')) {
      fputs(line->data + before, stderr);
      fflush(stderr);
    }
  }
  return r;
}

// Decides what Ctrl-D does: honour ignoreeof, or let the shell exit.
static bool keep_reading_at_eof(Source *src) {
  if (!src->interactive) {
    return false;
  }
  Option *ignoreeof = find_option_name("ignoreeof");
  if (ignoreeof->on) {
    fprintf(stderr, "Use \"exit\" to leave the shell.\n");
    src->at_eof = false;
    return true;
  }
  return false;
}

// Prints a syntax error message.
static void report_syntax_error(Parser *p) {
  current_lineno = p->lineno;
  if (p->incomplete) {
    shell_error("syntax error: unexpected end of file", NULL, NULL);
  } else {
    shell_error("syntax error: \"%s\" unexpected", p->error_token, NULL);
  }
}

// Decides whether to stop reading after a command because of break,
// continue or return.
static bool after_command(void) {
  if (!loop_should_stop()) {
    return false;
  }
  if (source_depth > 1) {
    return true;
  }
  loop_break = 0;
  loop_continue = 0;
  returning = false;
  return false;
}

// The main loop: reads, parses and runs commands from a source until it ends.
// Reads more lines while a command is unfinished.
int run_source(Source *src) {
  int status = 0;
  source_depth++;
  Str text = {0};
  bool stop = false;
  while (!stop) {
    str_clear(&text);
    Arena arena = {0};
    Parser p;
    memset(&p, 0, sizeof(p));
    Node *tree = NULL;
    bool more = false;
    int start_line = src->lineno + 1;
    int r;
    while (true) {
      r = read_source_line(src, &text, more);
      if (r == READ_INTERRUPT || (r == READ_EOF && text.len == 0)) {
        break;
      }
      arena_free(&arena);
      parser_init(&p, str_cstr(&text), text.len, &arena);
      p.lineno = start_line;
      tree = parse_program(&p);
      if (p.incomplete && r == READ_LINE) {
        more = true;
        continue;
      }
      break;
    }
    if (r == READ_INTERRUPT) {
      arena_free(&arena);
      last_status = 130;
      got_sigint = 0;
      continue;
    }
    if (r == READ_EOF && text.len == 0) {
      arena_free(&arena);
      if (keep_reading_at_eof(src)) {
        continue;
      }
      break;
    }
    if (src->interactive && strspn(str_cstr(&text), "\n") < text.len) {
      history_count++;
    }
    if (p.error || p.incomplete) {
      report_syntax_error(&p);
      arena_free(&arena);
      last_status = 2;
      status = 2;
      if (!interactive) {
        exit_shell(2);
      }
      if (r == READ_EOF && !src->interactive) {
        break;
      }
      continue;
    }
    if (tree != NULL && !(opt('n') && !interactive)) {
      status = exec_list(tree);
      stop = after_command();
    }
    arena_free(&arena);
    if (src->interactive) {
      got_sigint = 0;
      fflush(stdout);
    }
  }
  str_free(&text);
  source_depth--;
  return status;
}

// Runs a string as shell commands. Used by eval, traps and $(...).
int run_string(const char *text) {
  Source src;
  memset(&src, 0, sizeof(src));
  src.text = text;
  src.text_len = strlen(text);
  int saved_line = current_lineno;
  if (current_lineno > 0) {
    src.lineno = current_lineno - 1;
  }
  int status = run_source(&src);
  current_lineno = saved_line;
  return status;
}

// Runs a startup file if it exists and can be read.
static void source_if_readable(const char *path) {
  if (path != NULL && path[0] != '\0' && access(path, R_OK) == 0) {
    run_file(path, NULL, 0);
  }
}

// Runs the file named by $ENV, for interactive shells.
static void run_startup_files(void) {
  if (!interactive) {
    return;
  }
  const char *env = get_var("ENV");
  if (env != NULL && env[0] != '\0') {
    char *path = expand_string(env);
    if (path != NULL) {
      source_if_readable(path);
      free(path);
    }
  }
}

// Sets the variables every shell starts with: IFS, PS1, PS2, PS4,
// OPTIND, PPID and PWD.
static void set_default_vars(void) {
  set_var("IFS", " \t\n");
  if (get_var("PS1") == NULL) {
    set_var("PS1", geteuid() == 0 ? "# " : "$ ");
  }
  if (get_var("PS2") == NULL) {
    set_var("PS2", "> ");
  }
  if (get_var("PS4") == NULL) {
    set_var("PS4", "+ ");
  }
  set_var("OPTIND", "1");
  set_var_int("PPID", (long)getppid());
  const char *pwd = get_var("PWD");
  struct stat a;
  struct stat b;
  bool pwd_ok = pwd != NULL && pwd[0] == '/' && stat(pwd, &a) == 0 &&
                stat(".", &b) == 0 && a.st_dev == b.st_dev &&
                a.st_ino == b.st_ino;
  if (!pwd_ok) {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
      set_var("PWD", cwd);
      find_var("PWD")->exported = true;
    }
  }
}

// Prints a bad command-line option error and exits.
static void usage_error(const char *what, const char *arg) {
  fprintf(stderr, "%s: %s%s\n", shell_name, what, arg);
  exit(2);
}

// Starts the shell: reads the options, sets up an interactive session,
// runs the startup files, then runs the commands.
int main(int argc, char **argv) {
  setbuf(stdout, NULL);
  shell_pid = getpid();
  shell_name = argv[0];
  record_ignored_signals();
  import_environment();
  set_default_vars();

  bool command_mode = false;
  bool read_stdin = false;
  bool force_interactive = false;
  int i = 1;
  while (i < argc) {
    const char *a = argv[i];
    if (strcmp(a, "--") == 0 || strcmp(a, "-") == 0) {
      i++;
      break;
    }
    if ((a[0] != '-' && a[0] != '+') || a[1] == '\0') {
      break;
    }
    bool on = a[0] == '-';
    i++;
    for (int k = 1; a[k] != '\0'; k++) {
      char c = a[k];
      if (c == 'c' && on) {
        command_mode = true;
      } else if (c == 's' && on) {
        read_stdin = true;
      } else if (c == 'i' && on) {
        force_interactive = true;
      } else if (c == 'o') {
        if (i >= argc) {
          usage_error("-o needs an option name", "");
        }
        Option *o = find_option_name(argv[i++]);
        if (o == NULL) {
          usage_error("illegal option -o ", argv[i - 1]);
        }
        o->on = on;
      } else {
        Option *o = find_option_letter(c);
        if (o == NULL) {
          char bad[2] = {c, '\0'};
          usage_error("illegal option -", bad);
        }
        o->on = on;
      }
    }
  }

  const char *command = NULL;
  const char *script = NULL;
  if (command_mode) {
    if (i >= argc) {
      usage_error("-c requires an argument", "");
    }
    command = argv[i++];
    if (i < argc) {
      shell_name = argv[i++];
    }
  } else if (!read_stdin && i < argc) {
    script = argv[i++];
    shell_name = (char *)script;
  }
  set_params(argv + i, argc - i);

  interactive = force_interactive ||
                (command == NULL && script == NULL && isatty(STDIN_FILENO) &&
                 isatty(STDERR_FILENO));
  if (interactive) {
    setup_signals();
  }

  run_startup_files();

  if (command != NULL) {
    run_string(command);
    exit_shell(last_status);
  }
  Source src;
  memset(&src, 0, sizeof(src));
  if (script != NULL) {
    int fd = open(script, O_RDONLY);
    if (fd < 0) {
      fprintf(stderr, "%s: %s: %s\n", argv[0], script, strerror(errno));
      exit(127);
    }
    src.fd = move_fd_high(fd);
  } else {
    src.fd = STDIN_FILENO;
    src.unbuffered = true;
    src.interactive = interactive;
  }
  run_source(&src);
  exit_shell(last_status);
  return last_status;
}

#include "shell.h"

static bool expand_failed = false;
enum { CTX_NONE, CTX_DQ, CTX_BRACE_DQ, CTX_HEREDOC };
enum { MODE_FIELDS, MODE_STRING, MODE_PATTERN };

typedef struct {
  Field cur;
  Field *raw;
  int nraw;
  int mode;
  bool assignment;
  bool error;
  bool at_zero;
} Expander;

static void expand_text(Expander *e, const char *s, size_t len, int ctx);

// Appends one character and its quoting flags to a field.
void field_add(Field *f, char c, char flag) {
  str_add_char(&f->chars, c);
  str_add_char(&f->flags, flag);
}

// Appends a string to a field, giving every character the same flags.
static void field_add_str(Field *f, const char *s, char flag) {
  for (; *s != '\0'; s++) {
    field_add(f, *s, flag);
  }
}

// Frees a field's text and flags.
void field_free(Field *f) {
  str_free(&f->chars);
  str_free(&f->flags);
  f->quoted_empty = false;
}

// Gets an expander ready, in the given mode.
static void expander_init(Expander *e, int mode) {
  memset(e, 0, sizeof(*e));
  e->mode = mode;
}

// Ends the current field and starts a new one. Used between "$@" arguments.
static void expander_break(Expander *e) {
  e->raw = xrealloc(e->raw, (e->nraw + 1) * sizeof(Field));
  e->raw[e->nraw++] = e->cur;
  memset(&e->cur, 0, sizeof(Field));
}

// Frees everything an expander holds.
static void expander_free(Expander *e) {
  for (int i = 0; i < e->nraw; i++) {
    field_free(&e->raw[i]);
  }
  free(e->raw);
  field_free(&e->cur);
}

// Returns the flag for text that comes out of an expansion: it may be
// split when unquoted, and is protected inside quotes.
static char value_flag(int ctx) {
  return ctx == CTX_NONE ? Q_SPLIT : Q_QUOTED;
}

// Marks the expansion as failed, so the command won't run.
static void expand_fail(Expander *e) {
  e->error = true;
  expand_failed = true;
}

// Builds the value of $-: the letters of every option that is on.
static const char *option_letters(char *buf) {
  int n = 0;
  for (int i = 0; i < option_count; i++) {
    if (options[i].letter != 0 && options[i].on) {
      buf[n++] = options[i].letter;
    }
  }
  if (interactive) {
    buf[n++] = 'i';
  }
  buf[n] = '\0';
  return buf;
}

// Returns the value of a parameter by name, including special ones
// such as $?, $#, $$ and $1. Returns NULL if it is unset.
static const char *param_value(const char *name, char *buf, size_t cap,
                               bool *is_set) {
  *is_set = true;
  if (name[0] == '?' && name[1] == '\0') {
    snprintf(buf, cap, "%d", last_status);
    return buf;
  }
  if (name[0] == '$' && name[1] == '\0') {
    snprintf(buf, cap, "%d", (int)shell_pid);
    return buf;
  }
  if (name[0] == '!' && name[1] == '\0') {
    if (last_bg_pid == 0) {
      *is_set = false;
      return NULL;
    }
    snprintf(buf, cap, "%d", (int)last_bg_pid);
    return buf;
  }
  if (name[0] == '#' && name[1] == '\0') {
    snprintf(buf, cap, "%d", nparams);
    return buf;
  }
  if (name[0] == '-' && name[1] == '\0') {
    char letters[32];
    snprintf(buf, cap, "%s", option_letters(letters));
    return buf;
  }
  if (isdigit((unsigned char)name[0])) {
    long n = atol(name);
    if (n == 0) {
      return shell_name != NULL ? shell_name : "sh";
    }
    if (n > nparams) {
      *is_set = false;
      return NULL;
    }
    return params[n - 1];
  }
  if ((name[0] == '@' || name[0] == '*') && name[1] == '\0') {
    *is_set = nparams > 0;
    Str joined = {0};
    const char *ifs = ifs_value();
    for (int i = 0; i < nparams; i++) {
      if (i > 0 && (name[0] == '@' || ifs[0] != '\0')) {
        str_add_char(&joined, name[0] == '@' ? ' ' : ifs[0]);
      }
      str_add(&joined, params[i]);
    }
    snprintf(buf, cap, "%s", str_cstr(&joined));
    str_free(&joined);
    return buf;
  }
  const char *v = get_var(name);
  *is_set = v != NULL;
  return v;
}

// Reports a set -u error for an unset parameter.
static void unset_error(Expander *e, const char *name) {
  shell_error("%s: parameter not set", name, NULL);
  expand_fail(e);
}

// Adds $@ or $* to the field. Quoted $@ gives one field per argument.
static void add_params(Expander *e, char which, int ctx) {
  if (nparams == 0) {
    if (ctx != CTX_NONE) {
      e->at_zero = true;
    }
    return;
  }
  if (ctx == CTX_NONE) {
    for (int i = 0; i < nparams; i++) {
      if (i > 0 && e->mode == MODE_FIELDS) {
        expander_break(e);
      } else if (i > 0) {
        field_add(&e->cur, ' ', Q_SPLIT);
      }
      field_add_str(&e->cur, params[i], Q_SPLIT);
    }
    return;
  }
  if (which == '*' || e->mode != MODE_FIELDS) {
    const char *ifs = ifs_value();
    for (int i = 0; i < nparams; i++) {
      if (i > 0 && (which == '@' || ifs[0] != '\0')) {
        field_add(&e->cur, which == '@' ? ' ' : ifs[0], Q_QUOTED);
      }
      field_add_str(&e->cur, params[i], Q_QUOTED);
    }
    e->cur.quoted_empty = true;
    return;
  }
  for (int i = 0; i < nparams; i++) {
    if (i > 0) {
      expander_break(e);
    }
    field_add_str(&e->cur, params[i], Q_QUOTED);
    e->cur.quoted_empty = true;
  }
}

// Expands the word inside a ${...} and returns it as one string.
static char *expand_sub(const char *s, size_t len, int mode, int ctx,
                        bool *error) {
  Expander sub;
  expander_init(&sub, mode);
  expand_text(&sub, s, len, ctx);
  *error = sub.error;
  Str out = {0};
  for (int i = 0; i < sub.nraw; i++) {
    str_add_len(&out, sub.raw[i].chars.data != NULL ? sub.raw[i].chars.data : "",
                sub.raw[i].chars.len);
    str_add_char(&out, ' ');
  }
  Field *f = &sub.cur;
  for (size_t i = 0; i < f->chars.len; i++) {
    char c = f->chars.data[i];
    bool quoted = (f->flags.data[i] & Q_QUOTED) != 0;
    if (mode == MODE_PATTERN && (quoted || c == '\\') &&
        strchr("*?[]\\", c) != NULL) {
      str_add_char(&out, '\\');
    }
    str_add_char(&out, c);
  }
  expander_free(&sub);
  return str_take(&out);
}

// Removes the shortest or longest match of a pattern from the start (#, ##)
// or end (%, %%) of value.
static void strip_pattern(const char *value, const char *pat, char op,
                          bool longest, Str *out) {
  size_t n = strlen(value);
  char *tmp = xstrdup(value);
  bool done = false;
  if (op == '#') {
    for (size_t k = 0; k <= n && !done; k++) {
      size_t i = longest ? n - k : k;
      char saved = tmp[i];
      tmp[i] = '\0';
      if (fnmatch(pat, tmp, 0) == 0) {
        str_add(out, value + i);
        done = true;
      }
      tmp[i] = saved;
    }
  } else {
    for (size_t k = 0; k <= n && !done; k++) {
      size_t i = longest ? k : n - k;
      if (fnmatch(pat, value + i, 0) == 0) {
        str_add_len(out, value, i);
        done = true;
      }
    }
  }
  if (!done) {
    str_add(out, value);
  }
  free(tmp);
}

// Expands a ${...} form, such as ${x:-default}, ${#x} or ${x%.c}.
static void expand_braced(Expander *e, const char *c, size_t clen, int ctx) {
  char name[256];
  size_t i = 0;
  bool length = false;
  if (clen > 1 && c[0] == '#' &&
      (is_name_start(c[1]) || isdigit((unsigned char)c[1]) ||
       (strchr("@*#?-$!", c[1]) != NULL && clen == 2))) {
    length = true;
    i = 1;
  }
  size_t ns = i;
  if (i < clen && is_name_start(c[i])) {
    while (i < clen && is_name_char(c[i])) {
      i++;
    }
  } else if (i < clen && isdigit((unsigned char)c[i])) {
    while (i < clen && isdigit((unsigned char)c[i])) {
      i++;
    }
  } else if (i < clen && strchr("@*#?-$!", c[i]) != NULL) {
    i++;
  }
  if (i == ns || i - ns >= sizeof(name)) {
    char *text = xstrndup(c, clen);
    shell_error("${%s}: bad substitution", text, NULL);
    free(text);
    expand_fail(e);
    return;
  }
  memcpy(name, c + ns, i - ns);
  name[i - ns] = '\0';

  char buf[4096];
  bool is_set;
  const char *value = param_value(name, buf, sizeof(buf), &is_set);
  bool is_at = strcmp(name, "@") == 0 || strcmp(name, "*") == 0;

  if (length) {
    if (i != clen) {
      shell_error("bad substitution", NULL, NULL);
      expand_fail(e);
      return;
    }
    if (!is_set && opt('u') && !is_at) {
      unset_error(e, name);
      return;
    }
    char num[32];
    snprintf(num, sizeof(num), "%zu", value != NULL ? strlen(value) : (size_t)0);
    field_add_str(&e->cur, num, value_flag(ctx));
    return;
  }

  if (i == clen) {
    if (!is_set && opt('u') && !is_at) {
      unset_error(e, name);
      return;
    }
    if (is_at) {
      add_params(e, name[0], ctx);
    } else if (value != NULL) {
      field_add_str(&e->cur, value, value_flag(ctx));
    }
    return;
  }

  bool colon = false;
  if (c[i] == ':') {
    colon = true;
    i++;
  }
  if (i >= clen) {
    shell_error("bad substitution", NULL, NULL);
    expand_fail(e);
    return;
  }
  char op = c[i++];
  bool doubled = false;
  if ((op == '%' || op == '#') && !colon && i < clen && c[i] == op) {
    doubled = true;
    i++;
  }
  const char *word = c + i;
  size_t wlen = clen - i;
  bool null = !is_set || (value != NULL && value[0] == '\0') ||
              (is_at && nparams == 0);
  bool use_word_cond = colon ? null : !is_set;
  int word_ctx = ctx == CTX_NONE ? CTX_NONE : CTX_BRACE_DQ;

  if (op == '-' || op == '+') {
    bool use_word = op == '-' ? use_word_cond : !use_word_cond;
    if (use_word) {
      expand_text(e, word, wlen, word_ctx);
    } else if (op == '-') {
      if (is_at) {
        add_params(e, name[0], ctx);
      } else if (value != NULL) {
        field_add_str(&e->cur, value, value_flag(ctx));
      }
    }
    return;
  }
  if (op == '=' || op == '?') {
    if (!use_word_cond) {
      if (is_at) {
        add_params(e, name[0], ctx);
      } else if (value != NULL) {
        field_add_str(&e->cur, value, value_flag(ctx));
      }
      return;
    }
    bool err = false;
    char *w = expand_sub(word, wlen, MODE_STRING, word_ctx, &err);
    if (err) {
      free(w);
      expand_fail(e);
      return;
    }
    if (op == '?') {
      if (w[0] != '\0') {
        shell_error("%s: %s", name, w);
      } else {
        shell_error("%s: parameter %s", name, colon ? "null or not set" : "not set");
      }
      free(w);
      expand_fail(e);
      return;
    }
    if (!is_valid_name(name)) {
      shell_error("%s: cannot assign in this way", name, NULL);
      free(w);
      expand_fail(e);
      return;
    }
    if (!set_var(name, w)) {
      free(w);
      expand_fail(e);
      return;
    }
    field_add_str(&e->cur, w, value_flag(ctx));
    free(w);
    return;
  }
  if (op == '%' || op == '#') {
    if (colon) {
      shell_error("bad substitution", NULL, NULL);
      expand_fail(e);
      return;
    }
    if (!is_set && opt('u') && !is_at) {
      unset_error(e, name);
      return;
    }
    bool err = false;
    char *pat = expand_sub(word, wlen, MODE_PATTERN, CTX_NONE, &err);
    if (err) {
      free(pat);
      expand_fail(e);
      return;
    }
    Str out = {0};
    strip_pattern(value != NULL ? value : "", pat, op, doubled, &out);
    field_add_str(&e->cur, str_cstr(&out), value_flag(ctx));
    str_free(&out);
    free(pat);
    return;
  }
  shell_error("bad substitution", NULL, NULL);
  expand_fail(e);
}

// Runs a $(...) command and adds its output to the field,
// without the trailing newlines.
static void add_command_output(Expander *e, const char *body, int ctx) {
  char *out = command_subst(body);
  size_t n = strlen(out);
  while (n > 0 && out[n - 1] == '\n') {
    n--;
  }
  out[n] = '\0';
  field_add_str(&e->cur, out, value_flag(ctx));
  free(out);
}

// Removes the extra backslashes that `...` needs, before running its contents.
static char *unescape_backquote(const char *s, size_t len, int ctx) {
  Str out = {0};
  for (size_t i = 0; i < len; i++) {
    if (s[i] == '\\' && i + 1 < len &&
        (s[i + 1] == '$' || s[i + 1] == '`' || s[i + 1] == '\\' ||
         (ctx != CTX_NONE && s[i + 1] == '"'))) {
      i++;
    }
    str_add_char(&out, s[i]);
  }
  return str_take(&out);
}

// Expands the $ construct at i and returns the index just past it.
static size_t expand_dollar(Expander *e, const char *s, size_t len, size_t i,
                            int ctx) {
  if (i + 1 >= len) {
    field_add(&e->cur, '$', ctx == CTX_NONE ? 0 : Q_QUOTED);
    return i + 1;
  }
  char c = s[i + 1];
  if (c == '{') {
    long end = scan_braces(s, len, i, ctx != CTX_NONE);
    if (end < 0) {
      shell_error("bad substitution", NULL, NULL);
      expand_fail(e);
      return len;
    }
    expand_braced(e, s + i + 2, (size_t)end - i - 3, ctx);
    return (size_t)end;
  }
  if (c == '(') {
    if (i + 2 < len && s[i + 2] == '(') {
      long end = scan_arith(s, len, i);
      if (end < 0) {
        shell_error("bad arithmetic", NULL, NULL);
        expand_fail(e);
        return len;
      }
      bool err = false;
      char *expr = expand_sub(s + i + 3, (size_t)end - i - 5, MODE_STRING,
                              CTX_HEREDOC, &err);
      long value = 0;
      if (err || !arith_eval(expr, &value)) {
        free(expr);
        expand_fail(e);
        return len;
      }
      free(expr);
      char num[32];
      snprintf(num, sizeof(num), "%ld", value);
      field_add_str(&e->cur, num, value_flag(ctx));
      return (size_t)end;
    }
    long end = find_cmdsub_end(s, len, i + 2);
    if (end < 0) {
      shell_error("bad command substitution", NULL, NULL);
      expand_fail(e);
      return len;
    }
    char *body = xstrndup(s + i + 2, (size_t)end - i - 3);
    add_command_output(e, body, ctx);
    free(body);
    return (size_t)end;
  }
  char name[256];
  size_t j = i + 1;
  if (is_name_start(c)) {
    while (j < len && is_name_char(s[j]) && j - i - 1 < sizeof(name) - 1) {
      j++;
    }
  } else if (isdigit((unsigned char)c) || strchr("@*#?-$!", c) != NULL) {
    j++;
  } else {
    field_add(&e->cur, '$', ctx == CTX_NONE ? 0 : Q_QUOTED);
    return i + 1;
  }
  memcpy(name, s + i + 1, j - i - 1);
  name[j - i - 1] = '\0';
  if (strcmp(name, "@") == 0 || strcmp(name, "*") == 0) {
    add_params(e, name[0], ctx);
    return j;
  }
  char buf[4096];
  bool is_set;
  const char *value = param_value(name, buf, sizeof(buf), &is_set);
  if (!is_set && opt('u')) {
    unset_error(e, name);
    return len;
  }
  if (value != NULL) {
    field_add_str(&e->cur, value, value_flag(ctx));
  }
  return j;
}

// Expands ~ or ~user at i to a home directory. Returns the index past it.
static size_t expand_tilde(Expander *e, const char *s, size_t len, size_t i) {
  size_t j = i + 1;
  while (j < len && s[j] != '/' && !(e->assignment && s[j] == ':')) {
    if (strchr("'\"\\$`", s[j]) != NULL) {
      field_add(&e->cur, '~', 0);
      return i + 1;
    }
    j++;
  }
  char *user = xstrndup(s + i + 1, j - i - 1);
  const char *home = NULL;
  if (user[0] == '\0') {
    home = get_var("HOME");
    if (home == NULL) {
      struct passwd *pw = getpwuid(getuid());
      home = pw != NULL ? pw->pw_dir : NULL;
    }
  } else {
    struct passwd *pw = getpwnam(user);
    home = pw != NULL ? pw->pw_dir : NULL;
  }
  if (home == NULL) {
    field_add(&e->cur, '~', 0);
    field_add_str(&e->cur, user, 0);
  } else {
    field_add_str(&e->cur, home, Q_QUOTED);
  }
  free(user);
  return j;
}

// Walks a word and expands it into fields, recording for each character
// whether it was quoted and whether it may be split.
static void expand_text(Expander *e, const char *s, size_t len, int ctx) {
  size_t i = 0;
  bool dq_like = ctx != CTX_NONE;
  while (i < len && !e->error) {
    char c = s[i];
    if (c == '\\') {
      if (i + 1 >= len) {
        field_add(&e->cur, '\\', dq_like ? Q_QUOTED : 0);
        i++;
        continue;
      }
      char next = s[i + 1];
      if (next == '\n') {
        i += 2;
        continue;
      }
      if (ctx == CTX_NONE) {
        field_add(&e->cur, next, Q_QUOTED);
        i += 2;
        continue;
      }
      const char *special = ctx == CTX_HEREDOC ? "$`\\" : "$`\"\\";
      if (strchr(special, next) != NULL) {
        field_add(&e->cur, next, Q_QUOTED);
        i += 2;
      } else {
        field_add(&e->cur, '\\', Q_QUOTED);
        i++;
      }
      continue;
    }
    if (c == '\'' && ctx == CTX_NONE) {
      long end = scan_squote(s, len, i);
      if (end < 0) {
        end = (long)len;
      }
      for (size_t k = i + 1; k + 1 < (size_t)end; k++) {
        field_add(&e->cur, s[k], Q_QUOTED);
      }
      e->cur.quoted_empty = true;
      i = (size_t)end;
      continue;
    }
    if (c == '"' && (ctx == CTX_NONE || ctx == CTX_BRACE_DQ)) {
      long end = scan_dquote(s, len, i);
      if (end < 0) {
        end = (long)len + 1;
      }
      bool saved_zero = e->at_zero;
      e->at_zero = false;
      expand_text(e, s + i + 1, (size_t)end - i - 2, CTX_DQ);
      if (!e->at_zero) {
        e->cur.quoted_empty = true;
      }
      e->at_zero = saved_zero;
      i = (size_t)end;
      continue;
    }
    if (c == '`') {
      long end = scan_backquote(s, len, i);
      if (end < 0) {
        end = (long)len + 1;
      }
      char *body = unescape_backquote(s + i + 1, (size_t)end - i - 2, ctx);
      add_command_output(e, body, ctx);
      free(body);
      i = (size_t)end;
      continue;
    }
    if (c == '$') {
      i = expand_dollar(e, s, len, i, ctx);
      continue;
    }
    if (c == '~' && ctx == CTX_NONE &&
        (i == 0 || (e->assignment && (s[i - 1] == ':' || s[i - 1] == '=')))) {
      i = expand_tilde(e, s, len, i);
      continue;
    }
    field_add(&e->cur, c, dq_like ? Q_QUOTED : 0);
    i++;
  }
}

// True if the character at i can split the field.
static bool is_ifs_char(Field *f, size_t i, const char *ifs) {
  return (f->flags.data[i] & Q_SPLIT) != 0 && f->chars.data[i] != '\0' &&
         strchr(ifs, f->chars.data[i]) != NULL;
}

// True for space, tab or newline.
bool is_ifs_space(char c) {
  return c == ' ' || c == '\t' || c == '\n';
}

// Matches a field against file names when it has unquoted * ? or [.
// Adds the matches, or the field itself if nothing matches.
static void glob_field(Field *f, StrList *out) {
  bool has_magic = false;
  for (size_t i = 0; i < f->chars.len; i++) {
    char c = f->chars.data[i];
    if ((f->flags.data[i] & Q_QUOTED) == 0 && (c == '*' || c == '?' || c == '[')) {
      has_magic = true;
    }
  }
  const char *plain = f->chars.data != NULL ? f->chars.data : "";
  if (!has_magic || opt('f')) {
    list_add(out, xstrndup(plain, f->chars.len));
    return;
  }
  Str pat = {0};
  for (size_t i = 0; i < f->chars.len; i++) {
    char c = f->chars.data[i];
    bool quoted = (f->flags.data[i] & Q_QUOTED) != 0;
    if ((quoted || c == '\\') && strchr("*?[]\\", c) != NULL) {
      str_add_char(&pat, '\\');
    }
    str_add_char(&pat, c);
  }
  glob_t g;
  memset(&g, 0, sizeof(g));
  if (glob(str_cstr(&pat), 0, NULL, &g) == 0 && g.gl_pathc > 0) {
    for (size_t i = 0; i < g.gl_pathc; i++) {
      list_add(out, xstrdup(g.gl_pathv[i]));
    }
  } else {
    list_add(out, xstrndup(plain, f->chars.len));
  }
  globfree(&g);
  str_free(&pat);
}

// Finishes one split piece: globs it, then empties the piece for the next one.
static void emit_field(Field *part, StrList *out) {
  glob_field(part, out);
  str_clear(&part->chars);
  str_clear(&part->flags);
}

// Splits a field on IFS following the POSIX rules, then globs each piece.
static void split_field(Field *f, StrList *out) {
  const char *ifs = ifs_value();
  size_t n = f->chars.len;
  if (n == 0) {
    if (f->quoted_empty) {
      list_add(out, xstrdup(""));
    }
    return;
  }
  Field part = {0};
  bool started = false;
  size_t i = 0;
  int before = out->len;
  while (i < n && is_ifs_char(f, i, ifs) && is_ifs_space(f->chars.data[i])) {
    i++;
  }
  while (i < n) {
    if (!is_ifs_char(f, i, ifs)) {
      field_add(&part, f->chars.data[i], f->flags.data[i]);
      started = true;
      i++;
      continue;
    }
    bool hard = false;
    while (i < n && is_ifs_char(f, i, ifs) && is_ifs_space(f->chars.data[i])) {
      i++;
    }
    if (i < n && is_ifs_char(f, i, ifs) && !is_ifs_space(f->chars.data[i])) {
      hard = true;
      i++;
      while (i < n && is_ifs_char(f, i, ifs) && is_ifs_space(f->chars.data[i])) {
        i++;
      }
    }
    if (started || hard) {
      emit_field(&part, out);
    }
    started = false;
  }
  if (started) {
    emit_field(&part, out);
  }
  if (out->len == before && f->quoted_empty) {
    list_add(out, xstrdup(""));
  }
  field_free(&part);
}

// Fully expands one word and adds the resulting arguments to out.
static bool expand_word_into(const char *raw, StrList *out) {
  Expander e;
  expander_init(&e, MODE_FIELDS);
  expand_text(&e, raw, strlen(raw), CTX_NONE);
  if (e.error) {
    expander_free(&e);
    return false;
  }
  expander_break(&e);
  for (int i = 0; i < e.nraw; i++) {
    split_field(&e.raw[i], out);
  }
  expander_free(&e);
  return true;
}

// Fully expands a list of words into arguments: expansion, splitting and globbing.
bool expand_words(char **raw, int n, StrList *out) {
  for (int i = 0; i < n; i++) {
    if (!expand_word_into(raw[i], out)) {
      return false;
    }
  }
  list_items(out);
  return true;
}

// Expands a word into one string, without splitting or globbing.
static char *expand_with(const char *raw, int mode, int ctx, bool assignment) {
  Expander e;
  expander_init(&e, mode);
  e.assignment = assignment;
  expand_text(&e, raw, strlen(raw), ctx);
  if (e.error) {
    expander_free(&e);
    return NULL;
  }
  Str out = {0};
  Field *f = &e.cur;
  for (size_t i = 0; i < f->chars.len; i++) {
    char c = f->chars.data[i];
    bool quoted = (f->flags.data[i] & Q_QUOTED) != 0;
    if (mode == MODE_PATTERN && (quoted || c == '\\') &&
        strchr("*?[]\\", c) != NULL) {
      str_add_char(&out, '\\');
    }
    str_add_char(&out, c);
  }
  expander_free(&e);
  return str_take(&out);
}

// Expands a word into one string. Used for redirection targets and case words.
char *expand_string(const char *raw) {
  return expand_with(raw, MODE_STRING, CTX_NONE, false);
}

// Expands the value of NAME=value. Also expands ~ after : and =.
char *expand_assignment(const char *raw) {
  return expand_with(raw, MODE_STRING, CTX_NONE, true);
}

// Expands a pattern, keeping quoted characters literal for matching.
char *expand_pattern(const char *raw) {
  return expand_with(raw, MODE_PATTERN, CTX_NONE, false);
}

// Expands $ and ` in a here-doc body.
char *expand_heredoc(const char *body) {
  return expand_with(body, MODE_STRING, CTX_HEREDOC, false);
}

// Expands PS1, PS2 or PS4. ! becomes the history number, and set -u is ignored.
char *expand_prompt(const char *raw) {
  Str pre = {0};
  for (const char *p = raw; *p != '\0'; p++) {
    if (*p == '!' && p[1] == '!') {
      str_add_char(&pre, '!');
      p++;
    } else if (*p == '!') {
      char num[32];
      snprintf(num, sizeof(num), "%d", history_count + 1);
      str_add(&pre, num);
    } else {
      str_add_char(&pre, *p);
    }
  }
  bool saved_u = opt('u');
  find_option_letter('u')->on = false;
  bool saved_failed = expand_failed;
  char *out = expand_with(str_cstr(&pre), MODE_STRING, CTX_HEREDOC, false);
  expand_failed = saved_failed;
  find_option_letter('u')->on = saved_u;
  str_free(&pre);
  return out != NULL ? out : xstrdup(raw);
}

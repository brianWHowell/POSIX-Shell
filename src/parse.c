#include "shell.h"

static const char *op_texts[] = {"&&", "||", ";;", "<<-", "<<", ">>",
                                 "<&", ">&", "<>", ">|",  "&",  "|",
                                 ";",  "(",  ")",  "<",   ">"};

static const int op_types[] = {
    T_AND_IF,  T_OR_IF,    T_DSEMI,   T_DLESSDASH, T_DLESS, T_DGREAT,
    T_LESSAND, T_GREATAND, T_LESSGREAT, T_CLOBBER, T_AMP,   T_PIPE,
    T_SEMI,    T_LPAREN,   T_RPAREN,  T_LESS,      T_GREAT};

// Returns how many operators are in the operator table.
static int op_count(void) {
  return sizeof(op_types) / sizeof(op_types[0]);
}

// Returns the text of an operator token type, such as "&&".
static const char *op_text(int type) {
  for (int i = 0; i < op_count(); i++) {
    if (op_types[i] == type) {
      return op_texts[i];
    }
  }
  return "?";
}

// True if the token type is a redirection operator such as > or <<.
static bool is_redir_op(int type) {
  return type == T_LESS || type == T_GREAT || type == T_DGREAT ||
         type == T_LESSAND || type == T_GREATAND || type == T_LESSGREAT ||
         type == T_DLESS || type == T_DLESSDASH || type == T_CLOBBER;
}

static long scan_dollar(const char *s, size_t len, size_t i, bool in_dq);

// Skips a '...' string starting at i. Returns the index past the closing quote,
// or -1 if the text ends first.
long scan_squote(const char *s, size_t len, size_t i) {
  size_t j = i + 1;
  while (j < len && s[j] != '\'') {
    j++;
  }
  return j < len ? (long)(j + 1) : -1;
}

// Skips a `...` command starting at i. Returns the index past the closing
// backquote, or -1 if the text ends first.
long scan_backquote(const char *s, size_t len, size_t i) {
  size_t j = i + 1;
  while (j < len) {
    if (s[j] == '\\' && j + 1 < len) {
      j += 2;
      continue;
    }
    if (s[j] == '`') {
      return (long)(j + 1);
    }
    j++;
  }
  return -1;
}

// Skips a "..." string starting at i, including anything nested in it.
// Returns the index past the closing quote, or -1 if the text ends first.
long scan_dquote(const char *s, size_t len, size_t i) {
  size_t j = i + 1;
  while (j < len) {
    char c = s[j];
    long k = -3;
    if (c == '"') {
      return (long)(j + 1);
    }
    if (c == '\\') {
      j += 2;
      continue;
    }
    if (c == '$') {
      k = scan_dollar(s, len, j, true);
    } else if (c == '`') {
      k = scan_backquote(s, len, j);
    }
    if (k == -3) {
      j++;
      continue;
    }
    if (k < 0) {
      return k;
    }
    j = (size_t)k;
  }
  return -1;
}

// Skips a quote, $ construct or backquote at j if there is one there.
// Sets handled to false when the character at j isn't one of those.
static long scan_nested(const char *s, size_t len, size_t j, bool in_dq,
                        bool *handled) {
  char c = s[j];
  *handled = true;
  if (c == '\'' && !in_dq) {
    return scan_squote(s, len, j);
  }
  if (c == '"') {
    return scan_dquote(s, len, j);
  }
  if (c == '$') {
    return scan_dollar(s, len, j, in_dq);
  }
  if (c == '`') {
    return scan_backquote(s, len, j);
  }
  *handled = false;
  return (long)j;
}

// Skips a ${...} starting at i. Returns the index past the }, or -1 if the text ends first.
long scan_braces(const char *s, size_t len, size_t i, bool in_dq) {
  size_t j = i + 2;
  while (j < len) {
    if (s[j] == '}') {
      return (long)(j + 1);
    }
    if (s[j] == '\\') {
      j += 2;
      continue;
    }
    bool handled;
    long k = scan_nested(s, len, j, in_dq, &handled);
    if (!handled) {
      j++;
      continue;
    }
    if (k < 0) {
      return k;
    }
    j = (size_t)k;
  }
  return -1;
}

// Skips a $((...)) starting at i. Returns the index past the )), or -1 if the text ends first.
long scan_arith(const char *s, size_t len, size_t i) {
  size_t j = i + 3;
  int depth = 0;
  while (j < len) {
    char c = s[j];
    if (c == '(') {
      depth++;
    } else if (c == ')') {
      if (depth == 0) {
        if (j + 1 >= len) {
          return -1;
        }
        return s[j + 1] == ')' ? (long)(j + 2) : -2;
      }
      depth--;
    } else if (c == '\\') {
      j += 2;
      continue;
    } else {
      bool handled;
      long k = scan_nested(s, len, j, false, &handled);
      if (handled) {
        if (k < 0) {
          return k;
        }
        j = (size_t)k;
        continue;
      }
    }
    j++;
  }
  return -1;
}

// Skips a $ construct starting at i: $name, ${...}, $(...) or $((...)).
// Returns the index just past it, -1 if the text ends first, or -2 on an error.
static long scan_dollar(const char *s, size_t len, size_t i, bool in_dq) {
  if (i + 1 >= len) {
    return (long)(i + 1);
  }
  if (s[i + 1] == '{') {
    return scan_braces(s, len, i, in_dq);
  }
  if (s[i + 1] == '(') {
    if (i + 2 < len && s[i + 2] == '(') {
      return scan_arith(s, len, i);
    }
    return find_cmdsub_end(s, len, i + 2);
  }
  return (long)(i + 1);
}

// Returns the input frame being read now: an alias, or the main text.
static Frame *top_frame(Parser *p) {
  return &p->frames[p->depth - 1];
}

// Adds the newlines between from and to to the parser's line number.
static void count_lines(Parser *p, const char *s, size_t from, size_t to) {
  for (size_t i = from; i < to; i++) {
    if (s[i] == '\n') {
      p->lineno++;
    }
  }
}

// Works out a here-doc's end word from what followed <<.
// Removes any quotes and sets quoted if there were some.
static char *heredoc_delim(Arena *a, const char *raw, bool *quoted) {
  Str out = {0};
  *quoted = false;
  for (size_t i = 0; raw[i] != '\0'; i++) {
    char c = raw[i];
    if (c == '\'') {
      *quoted = true;
      i++;
      while (raw[i] != '\0' && raw[i] != '\'') {
        str_add_char(&out, raw[i++]);
      }
      if (raw[i] == '\0') {
        break;
      }
    } else if (c == '"') {
      *quoted = true;
      i++;
      while (raw[i] != '\0' && raw[i] != '"') {
        if (raw[i] == '\\' && raw[i + 1] != '\0' &&
            strchr("\\\"$`", raw[i + 1]) != NULL) {
          i++;
        }
        str_add_char(&out, raw[i++]);
      }
      if (raw[i] == '\0') {
        break;
      }
    } else if (c == '\\' && raw[i + 1] != '\0') {
      *quoted = true;
      str_add_char(&out, raw[++i]);
    } else {
      str_add_char(&out, c);
    }
  }
  char *result = arena_strdup(a, str_cstr(&out));
  str_free(&out);
  return result;
}

// Reads the bodies of the here-docs seen on the line that just ended.
static void read_heredocs(Parser *p) {
  Frame *f = &p->frames[0];
  for (int h = 0; h < p->npending; h++) {
    Redir *r = p->pending[h];
    bool quoted;
    char *delim = heredoc_delim(p->arena, r->target, &quoted);
    size_t dlen = strlen(delim);
    Str body = {0};
    bool found = false;
    while (f->pos < f->len) {
      size_t start = f->pos;
      size_t end = start;
      while (end < f->len && f->text[end] != '\n') {
        end++;
      }
      if (end >= f->len) {
        break;
      }
      size_t ls = start;
      if (r->strip_tabs) {
        while (ls < end && f->text[ls] == '\t') {
          ls++;
        }
      }
      f->pos = end + 1;
      p->lineno++;
      if (end - ls == dlen && strncmp(f->text + ls, delim, dlen) == 0) {
        found = true;
        break;
      }
      str_add_len(&body, f->text + ls, end + 1 - ls);
    }
    if (!found) {
      str_free(&body);
      p->incomplete = true;
      p->npending = 0;
      return;
    }
    r->heredoc = arena_strdup(p->arena, str_cstr(&body));
    r->heredoc_quoted = quoted;
    str_free(&body);
  }
  p->npending = 0;
}

// Reads one word token, skipping over quotes and $ constructs as single units.
static void read_word(Parser *p, Token *t) {
  Frame *f = top_frame(p);
  const char *s = f->text;
  size_t len = f->len;
  size_t j = f->pos;
  Str w = {0};
  while (j < len) {
    char c = s[j];
    if (c == '\\') {
      if (j + 1 < len && s[j + 1] == '\n') {
        j += 2;
        p->lineno++;
        if (j >= len && p->depth == 1) {
          p->incomplete = true;
        }
        continue;
      }
      size_t n = j + 1 < len ? 2 : 1;
      str_add_len(&w, s + j, n);
      j += n;
      continue;
    }
    if (is_blank(c) || c == '\n' || c == '\0' || strchr("|&;<>()", c) != NULL) {
      break;
    }
    long k = -3;
    if (c == '\'') {
      k = scan_squote(s, len, j);
    } else if (c == '"') {
      k = scan_dquote(s, len, j);
    } else if (c == '`') {
      k = scan_backquote(s, len, j);
    } else if (c == '$') {
      k = scan_dollar(s, len, j, false);
    }
    if (k == -3) {
      str_add_char(&w, c);
      j++;
      continue;
    }
    if (k == -1) {
      if (p->depth == 1) {
        p->incomplete = true;
      } else {
        p->error = true;
        p->error_token = "end of alias";
      }
      t->type = T_EOF;
      str_free(&w);
      f->pos = len;
      return;
    }
    if (k < 0) {
      p->error = true;
      p->error_token = "(";
      t->type = T_EOF;
      str_free(&w);
      f->pos = len;
      return;
    }
    count_lines(p, s, j, (size_t)k);
    str_add_len(&w, s + j, (size_t)k - j);
    j = (size_t)k;
  }
  f->pos = j;
  t->type = T_WORD;
  t->text = arena_strdup(p->arena, str_cstr(&w));
  str_free(&w);
}

// Reads the next token from the input: a word, number, operator, newline or end.
static void lex(Parser *p, Token *t) {
  t->text = NULL;
  t->fd = -1;
  while (true) {
    Frame *f = top_frame(p);
    if (f->pos >= f->len) {
      if (p->depth > 1) {
        p->depth--;
        continue;
      }
      if (p->npending > 0) {
        p->incomplete = true;
      }
      t->type = T_EOF;
      return;
    }
    char c = f->text[f->pos];
    if (is_blank(c)) {
      f->pos++;
      continue;
    }
    if (c == '\\' && f->pos + 1 < f->len && f->text[f->pos + 1] == '\n') {
      f->pos += 2;
      p->lineno++;
      if (p->depth == 1 && f->pos >= f->len) {
        p->incomplete = true;
      }
      continue;
    }
    if (c == '#') {
      while (f->pos < f->len && f->text[f->pos] != '\n') {
        f->pos++;
      }
      continue;
    }
    break;
  }

  Frame *f = top_frame(p);
  const char *s = f->text;
  size_t i = f->pos;
  if (s[i] == '\n') {
    f->pos++;
    p->lineno++;
    t->type = T_NEWLINE;
    if (p->depth == 1 && p->npending > 0) {
      read_heredocs(p);
    }
    return;
  }

  for (int k = 0; k < op_count(); k++) {
    size_t n = strlen(op_texts[k]);
    if (i + n <= f->len && strncmp(s + i, op_texts[k], n) == 0) {
      f->pos += n;
      t->type = op_types[k];
      return;
    }
  }

  size_t j = i;
  while (j < f->len && isdigit((unsigned char)s[j])) {
    j++;
  }
  if (j > i && j < f->len && (s[j] == '<' || s[j] == '>')) {
    t->type = T_IONUM;
    t->fd = atoi(s + i);
    f->pos = j;
    return;
  }

  read_word(p, t);
}

// Returns the next token without using it up.
static Token *peek(Parser *p) {
  if (p->nqueue == 0) {
    lex(p, &p->queue[0]);
    p->nqueue = 1;
  }
  return &p->queue[0];
}

// Returns the token after the next one without using either up.
static Token *peek2(Parser *p) {
  peek(p);
  if (p->nqueue == 1) {
    lex(p, &p->queue[1]);
    p->nqueue = 2;
  }
  return &p->queue[1];
}

// Uses up the next token and returns it.
static Token advance(Parser *p) {
  peek(p);
  Token t = p->queue[0];
  p->queue[0] = p->queue[1];
  p->nqueue--;
  return t;
}

// True if the token is the plain word kw, such as "then".
static bool is_kw(Token *t, const char *kw) {
  return t->type == T_WORD && strcmp(t->text, kw) == 0;
}

// Records a syntax error at token t.
// At the end of input it is recorded as incomplete instead, so another line can be read.
static void syntax_error(Parser *p, Token *t) {
  if (p->error || p->incomplete) {
    return;
  }
  if (t->type == T_EOF) {
    p->incomplete = true;
    return;
  }
  p->error = true;
  if (t->type == T_NEWLINE) {
    p->error_token = "newline";
  } else if (t->type == T_WORD) {
    p->error_token = t->text;
  } else if (t->type == T_IONUM) {
    p->error_token = "number";
  } else {
    p->error_token = op_text(t->type);
  }
}

// True once the parse has hit an error or run out of input.
static bool failed(Parser *p) {
  return p->error || p->incomplete;
}

// Skips any newline tokens.
static void skip_newlines(Parser *p) {
  while (!failed(p) && peek(p)->type == T_NEWLINE) {
    advance(p);
  }
}

// Uses up the keyword kw, or records a syntax error if it isn't next.
static bool expect_kw(Parser *p, const char *kw) {
  if (failed(p)) {
    return false;
  }
  Token *t = peek(p);
  if (!is_kw(t, kw)) {
    syntax_error(p, t);
    return false;
  }
  advance(p);
  return true;
}

// Records a syntax error if a command that needs a body, such as then, has none.
static bool require_body(Parser *p, Node *body) {
  if (failed(p)) {
    return false;
  }
  if (body == NULL) {
    syntax_error(p, peek(p));
    return false;
  }
  return true;
}

// True if the next token ends a list, such as fi, done, esac, } or ).
static bool is_list_end(Parser *p) {
  Token *t = peek(p);
  if (t->type == T_EOF || t->type == T_RPAREN || t->type == T_DSEMI) {
    return true;
  }
  static const char *enders[] = {"then", "else", "elif", "fi",
                                 "do",   "done", "esac", "}"};
  for (int i = 0; i < 8; i++) {
    if (is_kw(t, enders[i])) {
      return true;
    }
  }
  return false;
}

// Allocates a new, empty tree node of the given type.
static Node *new_node(Parser *p, int type) {
  Node *n = arena_alloc(p->arena, sizeof(Node));
  n->type = type;
  n->lineno = p->lineno;
  return n;
}

// Moves a list of strings into the arena as a NULL-terminated array.
static char **arena_list(Arena *a, StrList *l) {
  char **v = arena_alloc(a, (l->len + 1) * sizeof(char *));
  for (int i = 0; i < l->len; i++) {
    v[i] = l->items[i];
  }
  v[l->len] = NULL;
  free(l->items);
  l->items = NULL;
  return v;
}

// If the next word is an alias, switches input to the alias text.
// Returns true if it did.
static bool try_alias(Parser *p) {
  if (p->no_alias) {
    return false;
  }
  Token *t = peek(p);
  if (p->nqueue != 1 || t->type != T_WORD) {
    return false;
  }
  const char *value = get_alias(t->text);
  if (value == NULL || p->depth >= MAX_ALIAS_DEPTH) {
    return false;
  }
  for (int d = 0; d < p->depth; d++) {
    if (p->frames[d].alias != NULL && strcmp(p->frames[d].alias, t->text) == 0) {
      return false;
    }
  }
  p->nqueue = 0;
  Frame *f = &p->frames[p->depth++];
  f->text = arena_strdup(p->arena, value);
  f->len = strlen(value);
  f->pos = 0;
  f->alias = arena_strdup(p->arena, t->text);
  size_t n = strlen(value);
  p->arm_alias = n > 0 && is_blank(value[n - 1]);
  return true;
}

static Node *parse_list(Parser *p);
static Node *parse_command(Parser *p);

// Parses one redirection, such as 2>file or <<EOF.
static Redir *parse_redirect(Parser *p) {
  int fd = -1;
  if (peek(p)->type == T_IONUM) {
    fd = advance(p).fd;
  }
  Token op = advance(p);
  Token *t = peek(p);
  if (t->type != T_WORD) {
    syntax_error(p, t);
    return NULL;
  }
  Redir *r = arena_alloc(p->arena, sizeof(Redir));
  r->type = op.type;
  if (fd >= 0) {
    r->fd = fd;
  } else if (op.type == T_LESS || op.type == T_LESSAND ||
             op.type == T_LESSGREAT || op.type == T_DLESS ||
             op.type == T_DLESSDASH) {
    r->fd = 0;
  } else {
    r->fd = 1;
  }
  r->target = advance(p).text;
  if (op.type == T_DLESS || op.type == T_DLESSDASH) {
    r->strip_tabs = op.type == T_DLESSDASH;
    if (p->npending < MAX_HEREDOCS) {
      p->pending[p->npending++] = r;
    }
  }
  return r;
}

// Parses a simple command: assignments, words and redirections.
static Node *parse_simple(Parser *p) {
  Node *n = new_node(p, N_SIMPLE);
  StrList words = {0};
  StrList assigns = {0};
  Redir **rtail = &n->redirs;
  bool ok = true;
  while (ok && !failed(p)) {
    Token *t = peek(p);
    if (t->type == T_IONUM || is_redir_op(t->type)) {
      Redir *r = parse_redirect(p);
      if (r == NULL) {
        ok = false;
        break;
      }
      *rtail = r;
      rtail = &r->next;
      continue;
    }
    if (t->type != T_WORD) {
      break;
    }
    if (words.len == 0 && is_assignment_word(t->text)) {
      list_add(&assigns, advance(p).text);
      continue;
    }
    if ((words.len == 0 || p->alias_next) && try_alias(p)) {
      p->alias_next = false;
      continue;
    }
    p->alias_next = p->arm_alias;
    p->arm_alias = false;
    list_add(&words, advance(p).text);
  }
  p->alias_next = false;
  p->arm_alias = false;
  n->nwords = words.len;
  n->words = arena_list(p->arena, &words);
  n->nassigns = assigns.len;
  n->assigns = arena_list(p->arena, &assigns);
  if (!ok || failed(p)) {
    return NULL;
  }
  if (n->nwords == 0 && n->nassigns == 0 && n->redirs == NULL) {
    syntax_error(p, peek(p));
    return NULL;
  }
  return n;
}

// Parses any redirections written after a compound command, such as done > file.
static Node *finish_compound(Parser *p, Node *n) {
  if (n == NULL || failed(p)) {
    return NULL;
  }
  Redir **rtail = &n->redirs;
  while (true) {
    Token *t = peek(p);
    if (t->type != T_IONUM && !is_redir_op(t->type)) {
      break;
    }
    Redir *r = parse_redirect(p);
    if (r == NULL) {
      return NULL;
    }
    *rtail = r;
    rtail = &r->next;
  }
  return n;
}

// Parses a { list; } group.
static Node *parse_brace(Parser *p) {
  Node *n = new_node(p, N_GROUP);
  advance(p);
  n->body = parse_list(p);
  if (!require_body(p, n->body) || !expect_kw(p, "}")) {
    return NULL;
  }
  return n;
}

// Parses a ( list ) subshell.
static Node *parse_subshell(Parser *p) {
  Node *n = new_node(p, N_SUBSHELL);
  advance(p);
  n->body = parse_list(p);
  if (!require_body(p, n->body)) {
    return NULL;
  }
  Token *t = peek(p);
  if (t->type != T_RPAREN) {
    syntax_error(p, t);
    return NULL;
  }
  advance(p);
  return n;
}

// Parses if ... then ... [elif ... then ...] [else ...] fi.
static Node *parse_if(Parser *p) {
  Node *n = new_node(p, N_IF);
  advance(p);
  n->cond = parse_list(p);
  if (!require_body(p, n->cond) || !expect_kw(p, "then")) {
    return NULL;
  }
  n->body = parse_list(p);
  if (!require_body(p, n->body)) {
    return NULL;
  }
  Token *t = peek(p);
  if (is_kw(t, "elif")) {
    n->else_part = parse_if(p);
    return n->else_part != NULL ? n : NULL;
  }
  if (is_kw(t, "else")) {
    advance(p);
    n->else_part = parse_list(p);
    if (!require_body(p, n->else_part)) {
      return NULL;
    }
  }
  if (!expect_kw(p, "fi")) {
    return NULL;
  }
  return n;
}

// Parses while or until: the condition, then do ... done.
static Node *parse_loop(Parser *p, int type) {
  Node *n = new_node(p, type);
  advance(p);
  n->cond = parse_list(p);
  if (!require_body(p, n->cond) || !expect_kw(p, "do")) {
    return NULL;
  }
  n->body = parse_list(p);
  if (!require_body(p, n->body) || !expect_kw(p, "done")) {
    return NULL;
  }
  return n;
}

// Parses for name [in words]; do ... done.
static Node *parse_for(Parser *p) {
  Node *n = new_node(p, N_FOR);
  advance(p);
  Token *t = peek(p);
  if (t->type != T_WORD || !is_valid_name(t->text)) {
    syntax_error(p, t);
    return NULL;
  }
  n->name = advance(p).text;
  skip_newlines(p);
  t = peek(p);
  if (is_kw(t, "in")) {
    advance(p);
    n->has_in = true;
    StrList words = {0};
    while (peek(p)->type == T_WORD) {
      list_add(&words, advance(p).text);
    }
    n->nwords = words.len;
    n->words = arena_list(p->arena, &words);
    t = peek(p);
    if (t->type != T_SEMI && t->type != T_NEWLINE) {
      syntax_error(p, t);
      return NULL;
    }
    advance(p);
    skip_newlines(p);
  } else if (t->type == T_SEMI) {
    advance(p);
    skip_newlines(p);
  }
  if (!expect_kw(p, "do")) {
    return NULL;
  }
  n->body = parse_list(p);
  if (!require_body(p, n->body) || !expect_kw(p, "done")) {
    return NULL;
  }
  return n;
}

// Parses case word in pattern) list ;; ... esac.
static Node *parse_case(Parser *p) {
  Node *n = new_node(p, N_CASE);
  advance(p);
  Token *t = peek(p);
  if (t->type != T_WORD) {
    syntax_error(p, t);
    return NULL;
  }
  n->name = advance(p).text;
  skip_newlines(p);
  if (!expect_kw(p, "in")) {
    return NULL;
  }
  skip_newlines(p);
  CaseItem **tail = &n->items;
  while (!failed(p)) {
    t = peek(p);
    if (is_kw(t, "esac")) {
      advance(p);
      return n;
    }
    if (t->type == T_LPAREN) {
      advance(p);
    }
    StrList pats = {0};
    bool ok = true;
    while (true) {
      t = peek(p);
      if (t->type != T_WORD) {
        syntax_error(p, t);
        ok = false;
        break;
      }
      list_add(&pats, advance(p).text);
      t = peek(p);
      if (t->type == T_PIPE) {
        advance(p);
        continue;
      }
      if (t->type == T_RPAREN) {
        advance(p);
        break;
      }
      syntax_error(p, t);
      ok = false;
      break;
    }
    CaseItem *item = arena_alloc(p->arena, sizeof(CaseItem));
    item->npatterns = pats.len;
    item->patterns = arena_list(p->arena, &pats);
    if (!ok) {
      return NULL;
    }
    item->body = parse_list(p);
    if (failed(p)) {
      return NULL;
    }
    *tail = item;
    tail = &item->next;
    t = peek(p);
    if (t->type == T_DSEMI) {
      advance(p);
      skip_newlines(p);
      continue;
    }
    if (!is_kw(t, "esac")) {
      syntax_error(p, t);
      return NULL;
    }
  }
  return NULL;
}

// True if the token starts a compound command, such as if or {.
static bool is_compound_start(Token *t) {
  return t->type == T_LPAREN || is_kw(t, "{") || is_kw(t, "if") ||
         is_kw(t, "while") || is_kw(t, "until") || is_kw(t, "for") ||
         is_kw(t, "case");
}

// Parses a function definition: name() followed by a compound command.
static Node *parse_function(Parser *p) {
  Node *n = new_node(p, N_FUNC);
  n->name = advance(p).text;
  advance(p);
  Token *t = peek(p);
  if (t->type != T_RPAREN) {
    syntax_error(p, t);
    return NULL;
  }
  advance(p);
  skip_newlines(p);
  t = peek(p);
  if (!is_compound_start(t)) {
    syntax_error(p, t);
    return NULL;
  }
  n->body = parse_command(p);
  return n->body != NULL ? n : NULL;
}

// Parses one command of any kind: compound, function definition or simple.
static Node *parse_command(Parser *p) {
  while (!failed(p)) {
    Token *t = peek(p);
    if (t->type == T_LPAREN) {
      return finish_compound(p, parse_subshell(p));
    }
    if (t->type == T_IONUM || is_redir_op(t->type)) {
      return parse_simple(p);
    }
    if (t->type != T_WORD) {
      syntax_error(p, t);
      return NULL;
    }
    if (is_kw(t, "{")) {
      return finish_compound(p, parse_brace(p));
    }
    if (is_kw(t, "if")) {
      return finish_compound(p, parse_if(p));
    }
    if (is_kw(t, "while")) {
      return finish_compound(p, parse_loop(p, N_WHILE));
    }
    if (is_kw(t, "until")) {
      return finish_compound(p, parse_loop(p, N_UNTIL));
    }
    if (is_kw(t, "for")) {
      return finish_compound(p, parse_for(p));
    }
    if (is_kw(t, "case")) {
      return finish_compound(p, parse_case(p));
    }
    if (is_list_end(p) || is_kw(t, "!")) {
      syntax_error(p, t);
      return NULL;
    }
    if (try_alias(p)) {
      continue;
    }
    if (is_valid_name(t->text) && peek2(p)->type == T_LPAREN) {
      return parse_function(p);
    }
    return parse_simple(p);
  }
  return NULL;
}

// Parses a pipeline: [!] cmd | cmd | ...
static Node *parse_pipeline(Parser *p) {
  Node *n = new_node(p, N_PIPE);
  Token *t = peek(p);
  if (is_kw(t, "!")) {
    advance(p);
    n->bang = true;
  }
  Node *cmds[MAX_PIPE];
  int count = 0;
  while (true) {
    Node *c = parse_command(p);
    if (c == NULL) {
      return NULL;
    }
    if (count == MAX_PIPE) {
      p->error = true;
      p->error_token = "| (pipeline too long)";
      return NULL;
    }
    cmds[count++] = c;
    if (peek(p)->type != T_PIPE) {
      break;
    }
    advance(p);
    skip_newlines(p);
  }
  n->ncmds = count;
  n->cmds = arena_alloc(p->arena, count * sizeof(Node *));
  for (int i = 0; i < count; i++) {
    n->cmds[i] = cmds[i];
  }
  return n;
}

// Parses pipelines joined by && and ||, left to right.
static Node *parse_and_or(Parser *p) {
  Node *left = parse_pipeline(p);
  while (left != NULL) {
    Token *t = peek(p);
    if (t->type != T_AND_IF && t->type != T_OR_IF) {
      break;
    }
    int type = t->type == T_AND_IF ? N_AND : N_OR;
    advance(p);
    skip_newlines(p);
    Node *right = parse_pipeline(p);
    if (right == NULL) {
      return NULL;
    }
    Node *n = new_node(p, type);
    n->left = left;
    n->right = right;
    left = n;
  }
  return left;
}

// Parses a list of commands separated by ;, & or newlines.
static Node *parse_list(Parser *p) {
  Node *head = NULL;
  Node **tail = &head;
  skip_newlines(p);
  while (!failed(p) && !is_list_end(p)) {
    Node *item = parse_and_or(p);
    if (item == NULL) {
      return NULL;
    }
    Token *t = peek(p);
    if (t->type == T_AMP) {
      Node *bg = new_node(p, N_BG);
      bg->left = item;
      item = bg;
      advance(p);
    } else if (t->type == T_SEMI) {
      advance(p);
    } else if (t->type != T_NEWLINE && !is_list_end(p)) {
      syntax_error(p, t);
      return NULL;
    }
    *tail = item;
    tail = &item->next;
    skip_newlines(p);
  }
  if (failed(p)) {
    return NULL;
  }
  return head;
}

// Gets a parser ready to read the given text.
void parser_init(Parser *p, const char *text, size_t len, Arena *arena) {
  memset(p, 0, sizeof(*p));
  p->frames[0].text = text;
  p->frames[0].len = len;
  p->depth = 1;
  p->arena = arena;
  p->lineno = 1;
}

// Parses all of the text. Returns the command tree, or NULL on an error
// or incomplete input.
Node *parse_program(Parser *p) {
  Node *list = parse_list(p);
  if (failed(p)) {
    return NULL;
  }
  Token *t = peek(p);
  if (t->type != T_EOF) {
    syntax_error(p, t);
    return NULL;
  }
  if (p->incomplete) {
    return NULL;
  }
  return list;
}

// Finds the ) that closes a $( starting at start, by parsing what is inside.
// Returns the index just past it, -1 if the text ends first, or -2 on an error.
long find_cmdsub_end(const char *s, size_t len, size_t start) {
  Arena a = {0};
  Parser sub;
  parser_init(&sub, s, len, &a);
  sub.frames[0].pos = start;
  sub.no_alias = true;
  parse_list(&sub);
  long result = -2;
  if (sub.incomplete) {
    result = -1;
  } else if (!sub.error) {
    Token *t = peek(&sub);
    if (t->type == T_RPAREN) {
      result = (long)sub.frames[0].pos;
    } else if (t->type == T_EOF) {
      result = -1;
    }
  }
  arena_free(&a);
  return result;
}

// Copies an array of strings into the arena.
static char **copy_strs(Arena *a, char **v, int n) {
  char **out = arena_alloc(a, (n + 1) * sizeof(char *));
  for (int i = 0; i < n; i++) {
    out[i] = arena_strdup(a, v[i]);
  }
  out[n] = NULL;
  return out;
}

// Copies a string into the arena. NULL stays NULL.
static char *copy_str(Arena *a, const char *s) {
  return s != NULL ? arena_strdup(a, s) : NULL;
}

// Copies a list of redirections into the arena.
static Redir *copy_redirs(Arena *a, Redir *r) {
  Redir *head = NULL;
  Redir **tail = &head;
  for (; r != NULL; r = r->next) {
    Redir *c = arena_alloc(a, sizeof(Redir));
    *c = *r;
    c->target = copy_str(a, r->target);
    c->heredoc = copy_str(a, r->heredoc);
    c->next = NULL;
    *tail = c;
    tail = &c->next;
  }
  return head;
}

// Deep-copies a command tree into the arena, so a function body outlives
// the command that defined it.
Node *copy_node(Arena *a, Node *n) {
  if (n == NULL) {
    return NULL;
  }
  Node *c = arena_alloc(a, sizeof(Node));
  *c = *n;
  c->next = copy_node(a, n->next);
  c->left = copy_node(a, n->left);
  c->right = copy_node(a, n->right);
  c->cond = copy_node(a, n->cond);
  c->body = copy_node(a, n->body);
  c->else_part = copy_node(a, n->else_part);
  if (n->cmds != NULL) {
    c->cmds = arena_alloc(a, n->ncmds * sizeof(Node *));
    for (int i = 0; i < n->ncmds; i++) {
      c->cmds[i] = copy_node(a, n->cmds[i]);
    }
  }
  c->words = n->words != NULL ? copy_strs(a, n->words, n->nwords) : NULL;
  c->assigns = n->assigns != NULL ? copy_strs(a, n->assigns, n->nassigns) : NULL;
  c->redirs = copy_redirs(a, n->redirs);
  c->name = copy_str(a, n->name);
  CaseItem *head = NULL;
  CaseItem **tail = &head;
  for (CaseItem *it = n->items; it != NULL; it = it->next) {
    CaseItem *ci = arena_alloc(a, sizeof(CaseItem));
    ci->npatterns = it->npatterns;
    ci->patterns = copy_strs(a, it->patterns, it->npatterns);
    ci->body = copy_node(a, it->body);
    *tail = ci;
    tail = &ci->next;
  }
  c->items = head;
  return c;
}

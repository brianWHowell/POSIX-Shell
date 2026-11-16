#include "shell.h"

int last_status = 0;
int loop_depth = 0;
int loop_break = 0;
int loop_continue = 0;
int return_status = 0;
int func_depth = 0;
int current_lineno = 0;
int naliases = 0;
int history_count = 0;
int getopts_pos = 1;
int getopts_optind = 1;
int nparams = 0;
int nvars = 0;
int nfuncs = 0;
int nhash = 0;
pid_t last_bg_pid = 0;
pid_t shell_pid = 0;
bool special_error = false;
bool returning = false;
bool interactive = false;
bool in_background = false;
char *shell_name = NULL;
char **params = NULL;
char **alias_names = NULL;
char **alias_values = NULL;
char **hash_paths = NULL;
Var *vars = NULL;
Func **funcs = NULL;
char *trap_action[NSIG];
bool ignored_on_entry[NSIG];
pid_t background_pids[MAX_JOBS];

volatile sig_atomic_t got_sigint = 0;
volatile sig_atomic_t pending_any = 0;
volatile sig_atomic_t pending_sig[NSIG];

Option options[] = {
    {'a', "allexport", false}, {'b', "notify", false},
    {'C', "noclobber", false}, {'e', "errexit", false},
    {'f', "noglob", false},    {'h', "hashall", false},
    {'n', "noexec", false},    {'u', "nounset", false},
    {'v', "verbose", false},   {'x', "xtrace", false},
    {0, "ignoreeof", false},   {0, "nolog", false}};

const int option_count = sizeof(options) / sizeof(options[0]);

// Allocates memory, and exits the shell if there is none left.
void *xmalloc(size_t n) {
  void *p = malloc(n == 0 ? 1 : n);
  if (p == NULL) {
    fprintf(stderr, "out of memory\n");
    exit(2);
  }
  return p;
}

// Resizes a block of memory, and exits the shell if there is none left.
void *xrealloc(void *p, size_t n) {
  void *q = realloc(p, n == 0 ? 1 : n);
  if (q == NULL) {
    fprintf(stderr, "out of memory\n");
    exit(2);
  }
  return q;
}

// Returns a newly allocated copy of a string.
char *xstrdup(const char *s) {
  size_t n = strlen(s);
  char *p = xmalloc(n + 1);
  memcpy(p, s, n + 1);
  return p;
}

// Returns a newly allocated copy of the first n characters of a string.
char *xstrndup(const char *s, size_t n) {
  char *p = xmalloc(n + 1);
  memcpy(p, s, n);
  p[n] = '\0';
  return p;
}

// Makes sure the string has room for extra more characters.
void str_reserve(Str *s, size_t extra) {
  if (s->len + extra + 1 <= s->cap) {
    return;
  }
  size_t cap = s->cap == 0 ? 64 : s->cap;
  while (cap < s->len + extra + 1) {
    cap *= 2;
  }
  s->data = xrealloc(s->data, cap);
  s->cap = cap;
}

// Appends one character to the string.
void str_add_char(Str *s, char c) {
  str_reserve(s, 1);
  s->data[s->len++] = c;
  s->data[s->len] = '\0';
}

// Appends n characters from t to the string.
void str_add_len(Str *s, const char *t, size_t n) {
  str_reserve(s, n);
  memcpy(s->data + s->len, t, n);
  s->len += n;
  s->data[s->len] = '\0';
}

// Appends a C string to the string.
void str_add(Str *s, const char *t) {
  str_add_len(s, t, strlen(t));
}

// Empties the string but keeps its memory for reuse.
void str_clear(Str *s) {
  s->len = 0;
  if (s->data != NULL) {
    s->data[0] = '\0';
  }
}

// Frees the string's memory and resets it to empty.
void str_free(Str *s) {
  free(s->data);
  s->data = NULL;
  s->len = 0;
  s->cap = 0;
}

// Returns the string's text, or "" if nothing was ever added.
const char *str_cstr(Str *s) {
  return s->data != NULL ? s->data : "";
}

// Hands the string's memory to the caller and resets the Str.
// The caller must free the result.
char *str_take(Str *s) {
  char *p = s->data != NULL ? s->data : xstrdup("");
  s->data = NULL;
  s->len = 0;
  s->cap = 0;
  return p;
}

// Appends an item to the list and keeps it NULL-terminated.
void list_add(StrList *l, char *item) {
  if (l->len + 2 > l->cap) {
    l->cap = l->cap == 0 ? 8 : l->cap * 2;
    l->items = xrealloc(l->items, l->cap * sizeof(char *));
  }
  l->items[l->len++] = item;
  l->items[l->len] = NULL;
}

// Frees every item in the list, then the list itself.
void list_free(StrList *l) {
  for (int i = 0; i < l->len; i++) {
    free(l->items[i]);
  }
  free(l->items);
  l->items = NULL;
  l->len = 0;
  l->cap = 0;
}

// Returns the list as a NULL-terminated array, even when it is empty.
char **list_items(StrList *l) {
  if (l->items == NULL) {
    list_add(l, NULL);
    l->len = 0;
  }
  return l->items;
}

// Allocates zeroed memory from the arena, adding a new block when full.
// Nothing is freed until arena_free.
void *arena_alloc(Arena *a, size_t n) {
  n = (n + 15) & ~(size_t)15;
  if (a->head == NULL || a->head->used + n > a->head->size) {
    size_t size = n > 8192 ? n : 8192;
    ArenaBlock *b = xmalloc(sizeof(ArenaBlock) + size);
    b->next = a->head;
    b->used = 0;
    b->size = size;
    a->head = b;
  }
  void *p = a->head->data + a->head->used;
  a->head->used += n;
  memset(p, 0, n);
  return p;
}

// Copies the first n characters of a string into the arena.
char *arena_strndup(Arena *a, const char *s, size_t n) {
  char *p = arena_alloc(a, n + 1);
  memcpy(p, s, n);
  p[n] = '\0';
  return p;
}

// Copies a string into the arena.
char *arena_strdup(Arena *a, const char *s) {
  return arena_strndup(a, s, strlen(s));
}

// Frees every block in the arena at once.
void arena_free(Arena *a) {
  while (a->head != NULL) {
    ArenaBlock *next = a->head->next;
    free(a->head);
    a->head = next;
  }
}

// True for a space or a tab.
bool is_blank(char c) {
  return c == ' ' || c == '\t';
}

// True if c can start a variable name: a letter or underscore.
bool is_name_start(char c) {
  return isalpha((unsigned char)c) || c == '_';
}

// True if c can appear in a variable name: a letter, digit or underscore.
bool is_name_char(char c) {
  return isalnum((unsigned char)c) || c == '_';
}

// True if the whole string is a valid variable name.
bool is_valid_name(const char *s) {
  if (!is_name_start(s[0])) {
    return false;
  }
  for (int i = 1; s[i] != '\0'; i++) {
    if (!is_name_char(s[i])) {
      return false;
    }
  }
  return true;
}

// True if the word looks like NAME=value.
bool is_assignment_word(const char *s) {
  if (!is_name_start(s[0])) {
    return false;
  }
  int i = 1;
  while (is_name_char(s[i])) {
    i++;
  }
  return s[i] == '=';
}

// Converts a whole string to a number. Returns false if it isn't one.
bool parse_int(const char *s, long *out) {
  char *end;
  errno = 0;
  long n = strtol(s, &end, 10);
  if (end == s || *end != '\0' || errno != 0) {
    return false;
  }
  *out = n;
  return true;
}

// Prints an error to stderr as "shell: line: message".
// The line number is only shown when running a script.
void shell_error(const char *fmt, const char *a, const char *b) {
  fprintf(stderr, "%s: ", shell_name != NULL ? shell_name : "sh");
  if (current_lineno > 0 && !interactive) {
    fprintf(stderr, "%d: ", current_lineno);
  }
  fprintf(stderr, fmt, a, b);
  fprintf(stderr, "\n");
}

// Appends value wrapped in single quotes so it can be pasted back into the shell.
void quote_value(Str *out, const char *value) {
  str_add_char(out, '\'');
  for (const char *p = value; *p != '\0'; p++) {
    if (*p == '\'') {
      str_add(out, "'\\''");
    } else {
      str_add_char(out, *p);
    }
  }
  str_add_char(out, '\'');
}

// Moves a file descriptor the shell uses for itself to 10 or above,
// so it can't clash with the user's redirections.
int move_fd_high(int fd) {
  if (fd < 0) {
    return fd;
  }
  int high = fcntl(fd, F_DUPFD_CLOEXEC, 10);
  if (high < 0) {
    return fd;
  }
  close(fd);
  return high;
}

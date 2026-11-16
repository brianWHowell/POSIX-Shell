#ifndef SHELL_H
#define SHELL_H

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <glob.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_JOBS 64
#define MAX_PIPE 64
#define MAX_ALIAS_DEPTH 32
#define MAX_HEREDOCS 32
#define MAX_SAVED_FDS 32
#define MAX_FUNC_DEPTH 1000

extern char **environ;

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} Str;

typedef struct {
  char **items;
  int len;
  int cap;
} StrList;

typedef struct ArenaBlock {
  struct ArenaBlock *next;
  size_t used;
  size_t size;
  char data[];
} ArenaBlock;

typedef struct {
  ArenaBlock *head;
} Arena;

enum {
  T_WORD,
  T_IONUM,
  T_NEWLINE,
  T_EOF,
  T_AND_IF,
  T_OR_IF,
  T_DSEMI,
  T_SEMI,
  T_AMP,
  T_PIPE,
  T_LPAREN,
  T_RPAREN,
  T_LESS,
  T_GREAT,
  T_DGREAT,
  T_LESSAND,
  T_GREATAND,
  T_LESSGREAT,
  T_DLESS,
  T_DLESSDASH,
  T_CLOBBER
};

enum {
  N_SIMPLE,
  N_PIPE,
  N_AND,
  N_OR,
  N_BG,
  N_GROUP,
  N_SUBSHELL,
  N_IF,
  N_WHILE,
  N_UNTIL,
  N_FOR,
  N_CASE,
  N_FUNC
};

typedef struct Redir {
  int type;
  int fd;
  char *target;
  char *heredoc;
  bool heredoc_quoted;
  bool strip_tabs;
  struct Redir *next;
} Redir;

typedef struct CaseItem {
  char **patterns;
  int npatterns;
  struct Node *body;
  struct CaseItem *next;
} CaseItem;

typedef struct Node {
  int type;
  int lineno;
  struct Node *next;
  struct Node *left;
  struct Node *right;
  struct Node *cond;
  struct Node *body;
  struct Node *else_part;
  struct Node **cmds;
  int ncmds;
  char **words;
  int nwords;
  char **assigns;
  int nassigns;
  Redir *redirs;
  CaseItem *items;
  char *name;
  bool bang;
  bool has_in;
} Node;

typedef struct {
  char *name;
  Node *body;
  Arena arena;
  int running;
  bool stale;
} Func;

typedef struct {
  char *name;
  char *value;
  bool exported;
  bool readonly;
} Var;

typedef struct {
  pid_t pids[MAX_PIPE];
  int npids;
  int status;
} Job;

typedef struct {
  int fd;
  int saved;
} SavedFd;

typedef struct {
  SavedFd items[MAX_SAVED_FDS];
  int n;
} FdSave;

typedef struct {
  int fd;
  const char *text;
  size_t text_len;
  size_t text_pos;
  char buf[4096];
  size_t buf_len;
  size_t buf_pos;
  bool unbuffered;
  bool interactive;
  bool at_eof;
  int lineno;
} Source;

typedef struct {
  const char *name;
  int (*fn)(char **args);
  bool special;
} Builtin;

typedef struct {
  char letter;
  const char *name;
  bool on;
} Option;

typedef struct {
  char **params;
  int nparams;
} SavedParams;

typedef struct {
  int type;
  int fd;
  char *text;
} Token;

typedef struct {
  const char *text;
  size_t len;
  size_t pos;
  const char *alias;
} Frame;

typedef struct {
  Frame frames[MAX_ALIAS_DEPTH];
  int depth;
  Arena *arena;
  Token queue[2];
  int nqueue;
  Redir *pending[MAX_HEREDOCS];
  int npending;
  bool incomplete;
  bool error;
  bool no_alias;
  bool alias_next;
  bool arm_alias;
  int lineno;
  const char *error_token;
} Parser;

enum { Q_QUOTED = 1, Q_SPLIT = 2 };

typedef struct {
  Str chars;
  Str flags;
  bool quoted_empty;
} Field;

extern int last_status;
extern bool interactive;
extern bool in_background;
extern pid_t last_bg_pid;
extern pid_t shell_pid;
extern volatile sig_atomic_t got_sigint;
extern volatile sig_atomic_t pending_any;
extern volatile sig_atomic_t pending_sig[NSIG];
extern char *trap_action[NSIG];
extern bool ignored_on_entry[NSIG];
extern bool special_error;
extern int loop_depth;
extern int loop_break;
extern int loop_continue;
extern bool returning;
extern int return_status;
extern int func_depth;
extern int current_lineno;
extern char *shell_name;
extern char **params;
extern int nparams;
extern Var *vars;
extern int nvars;
extern Func **funcs;
extern int nfuncs;
extern char **alias_names;
extern char **alias_values;
extern int naliases;
extern char **hash_paths;
extern int nhash;
extern pid_t background_pids[MAX_JOBS];
extern int history_count;
extern int getopts_pos;
extern int getopts_optind;
extern Option options[];
extern const int option_count;

extern bool wait_interruptible;

extern const Builtin builtin_table[];

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
void str_reserve(Str *s, size_t extra);
void str_add_char(Str *s, char c);
void str_add_len(Str *s, const char *t, size_t n);
void str_add(Str *s, const char *t);
void str_clear(Str *s);
void str_free(Str *s);
const char *str_cstr(Str *s);
char *str_take(Str *s);
void list_add(StrList *l, char *item);
void list_free(StrList *l);
char **list_items(StrList *l);
void *arena_alloc(Arena *a, size_t n);
char *arena_strndup(Arena *a, const char *s, size_t n);
char *arena_strdup(Arena *a, const char *s);
void arena_free(Arena *a);
bool is_blank(char c);
bool is_name_start(char c);
bool is_name_char(char c);
bool is_valid_name(const char *s);
bool is_assignment_word(const char *s);
bool parse_int(const char *s, long *out);
void shell_error(const char *fmt, const char *a, const char *b);
void quote_value(Str *out, const char *value);
int move_fd_high(int fd);

Option *find_option_letter(char c);
Option *find_option_name(const char *name);
bool opt(char c);
void set_option(Option *o, bool on);
Var *find_var(const char *name);
Var *add_var(const char *name);
const char *get_var(const char *name);
void clear_hash(void);
bool set_var(const char *name, const char *value);
bool unset_var(const char *name);
void set_var_int(const char *name, long n);
char **build_env(void);
void import_environment(void);
void sort_vars(void);
const char *ifs_value(void);
void set_params(char **args, int n);
SavedParams save_params(void);
void restore_params(SavedParams s);
const char *get_alias(const char *name);
void set_alias(const char *name, const char *value);
bool remove_alias(const char *name);
Func *find_func(const char *name);
void release_func(Func *f);
void remove_func(const char *name);
void add_func(Func *f);
bool is_executable(const char *file);
char *search_path(const char *name, const char *path);
char *find_command(const char *name);

long scan_squote(const char *s, size_t len, size_t i);
long scan_backquote(const char *s, size_t len, size_t i);
long scan_dquote(const char *s, size_t len, size_t i);
long scan_braces(const char *s, size_t len, size_t i, bool in_dq);
long scan_arith(const char *s, size_t len, size_t i);
void parser_init(Parser *p, const char *text, size_t len, Arena *arena);
Node *parse_program(Parser *p);
long find_cmdsub_end(const char *s, size_t len, size_t start);
Node *copy_node(Arena *a, Node *n);

bool arith_eval(const char *expr, long *result);

void field_add(Field *f, char c, char flag);
void field_free(Field *f);
bool is_ifs_space(char c);
bool expand_words(char **raw, int n, StrList *out);
char *expand_string(const char *raw);
char *expand_assignment(const char *raw);
char *expand_pattern(const char *raw);
char *expand_heredoc(const char *body);
char *expand_prompt(const char *raw);

int status_of(int ws);
int first_pending_signal(void);
int wait_job(Job *j);
void add_background(pid_t pid);
void remember_status(pid_t pid, int status);
bool recall_status(pid_t pid, int *status);
void reap_background(void);
int wait_background(int slot);
int background_slot(pid_t pid);
void on_signal(int sig);
void catch_signal(int sig);
void record_ignored_signals(void);
void shell_default_signal(int sig);
void setup_signals(void);
void reset_signals(void);
void enter_subshell(void);

void run_script_child(const char *path, char **args);
int fork_external(char **args, char **names, char **values, int n,
                 Redir *redirs, const char *path_override);
bool loop_should_stop(void);
int exec_list(Node *n);
char *command_subst(const char *body);
void exit_shell(int status);
void run_pending_traps(void);

int run_file(const char *path, char **new_params, int count);
const Builtin *find_builtin(const char *name);

int run_source(Source *src);
int run_string(const char *text);

#endif

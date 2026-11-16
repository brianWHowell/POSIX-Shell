#include "shell.h"

static int cap_vars = 0;
static char **hash_names = NULL;

// Finds an option by its letter, such as 'e' for set -e.
Option *find_option_letter(char c) {
  for (int i = 0; i < option_count; i++) {
    if (options[i].letter == c && c != 0) {
      return &options[i];
    }
  }
  return NULL;
}

// Finds an option by its long name, such as "errexit".
Option *find_option_name(const char *name) {
  for (int i = 0; i < option_count; i++) {
    if (strcmp(options[i].name, name) == 0) {
      return &options[i];
    }
  }
  return NULL;
}

// True if the option with this letter is on.
bool opt(char c) {
  Option *o = find_option_letter(c);
  return o != NULL && o->on;
}

// Turns an option on or off.
void set_option(Option *o, bool on) {
  o->on = on;
}

// Finds a variable by name, or returns NULL.
Var *find_var(const char *name) {
  for (int i = 0; i < nvars; i++) {
    if (strcmp(vars[i].name, name) == 0) {
      return &vars[i];
    }
  }
  return NULL;
}

// Returns the variable with this name, creating an empty one if needed.
Var *add_var(const char *name) {
  Var *v = find_var(name);
  if (v != NULL) {
    return v;
  }
  if (nvars == cap_vars) {
    cap_vars = cap_vars == 0 ? 64 : cap_vars * 2;
    vars = xrealloc(vars, cap_vars * sizeof(Var));
  }
  v = &vars[nvars++];
  v->name = xstrdup(name);
  v->value = NULL;
  v->exported = false;
  v->readonly = false;
  return v;
}

// Returns a variable's value, or NULL if it is unset.
const char *get_var(const char *name) {
  Var *v = find_var(name);
  return v != NULL ? v->value : NULL;
}

// Forgets every remembered command path.
void clear_hash(void) {
  for (int i = 0; i < nhash; i++) {
    free(hash_names[i]);
    free(hash_paths[i]);
  }
  nhash = 0;
}

// Sets a variable. Fails on readonly variables.
// Also handles set -a, and the side effects of setting PATH and OPTIND.
bool set_var(const char *name, const char *value) {
  Var *v = find_var(name);
  if (v != NULL && v->readonly) {
    shell_error("%s: is read only", name, NULL);
    return false;
  }
  v = add_var(name);
  char *copy = xstrdup(value);
  free(v->value);
  v->value = copy;
  if (opt('a')) {
    v->exported = true;
  }
  if (strcmp(name, "PATH") == 0) {
    clear_hash();
  }
  if (strcmp(name, "OPTIND") == 0) {
    getopts_pos = 1;
    long n;
    getopts_optind = parse_int(value, &n) ? (int)n : 1;
  }
  return true;
}

// Removes a variable. Fails on readonly variables.
bool unset_var(const char *name) {
  Var *v = find_var(name);
  if (v == NULL) {
    return true;
  }
  if (v->readonly) {
    shell_error("%s: is read only", name, NULL);
    return false;
  }
  if (strcmp(name, "PATH") == 0) {
    clear_hash();
  }
  free(v->name);
  free(v->value);
  nvars--;
  *v = vars[nvars];
  return true;
}

// Sets a variable to a number.
void set_var_int(const char *name, long n) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%ld", n);
  set_var(name, buf);
}

// Builds the NAME=value list of exported variables to pass to a new program.
char **build_env(void) {
  char **env = xmalloc((nvars + 1) * sizeof(char *));
  int n = 0;
  for (int i = 0; i < nvars; i++) {
    if (vars[i].exported && vars[i].value != NULL) {
      size_t len = strlen(vars[i].name) + strlen(vars[i].value) + 2;
      env[n] = xmalloc(len);
      snprintf(env[n], len, "%s=%s", vars[i].name, vars[i].value);
      n++;
    }
  }
  env[n] = NULL;
  return env;
}

// Copies the environment the shell started with into exported variables.
void import_environment(void) {
  for (char **e = environ; *e != NULL; e++) {
    char *eq = strchr(*e, '=');
    if (eq == NULL) {
      continue;
    }
    char *name = xstrndup(*e, eq - *e);
    if (is_valid_name(name)) {
      Var *v = add_var(name);
      free(v->value);
      v->value = xstrdup(eq + 1);
      v->exported = true;
    }
    free(name);
  }
}

// Orders two variables by name, for qsort.
static int compare_vars(const void *a, const void *b) {
  const Var *x = a;
  const Var *y = b;
  return strcmp(x->name, y->name);
}

// Sorts the variable table by name, so listings come out in order.
void sort_vars(void) {
  qsort(vars, nvars, sizeof(Var), compare_vars);
}

// Returns IFS, or space-tab-newline when IFS is unset.
const char *ifs_value(void) {
  const char *ifs = get_var("IFS");
  return ifs != NULL ? ifs : " \t\n";
}

// Replaces the positional parameters $1, $2... with copies of args.
void set_params(char **args, int n) {
  for (int i = 0; i < nparams; i++) {
    free(params[i]);
  }
  free(params);
  params = xmalloc((n + 1) * sizeof(char *));
  for (int i = 0; i < n; i++) {
    params[i] = xstrdup(args[i]);
  }
  params[n] = NULL;
  nparams = n;
}

// Takes the current positional parameters out so they can be restored later.
SavedParams save_params(void) {
  SavedParams s;
  s.params = params;
  s.nparams = nparams;
  params = NULL;
  nparams = 0;
  return s;
}

// Frees the current positional parameters and puts saved ones back.
void restore_params(SavedParams s) {
  for (int i = 0; i < nparams; i++) {
    free(params[i]);
  }
  free(params);
  params = s.params;
  nparams = s.nparams;
}

// Returns an alias's value, or NULL if there is no such alias.
const char *get_alias(const char *name) {
  for (int i = 0; i < naliases; i++) {
    if (strcmp(alias_names[i], name) == 0) {
      return alias_values[i];
    }
  }
  return NULL;
}

// Creates an alias, or replaces it if it already exists.
void set_alias(const char *name, const char *value) {
  for (int i = 0; i < naliases; i++) {
    if (strcmp(alias_names[i], name) == 0) {
      free(alias_values[i]);
      alias_values[i] = xstrdup(value);
      return;
    }
  }
  alias_names = xrealloc(alias_names, (naliases + 1) * sizeof(char *));
  alias_values = xrealloc(alias_values, (naliases + 1) * sizeof(char *));
  alias_names[naliases] = xstrdup(name);
  alias_values[naliases] = xstrdup(value);
  naliases++;
}

// Deletes an alias. Returns false if it didn't exist.
bool remove_alias(const char *name) {
  for (int i = 0; i < naliases; i++) {
    if (strcmp(alias_names[i], name) == 0) {
      free(alias_names[i]);
      free(alias_values[i]);
      naliases--;
      alias_names[i] = alias_names[naliases];
      alias_values[i] = alias_values[naliases];
      return true;
    }
  }
  return false;
}

// Finds a shell function by name, or returns NULL.
Func *find_func(const char *name) {
  for (int i = 0; i < nfuncs; i++) {
    if (strcmp(funcs[i]->name, name) == 0) {
      return funcs[i];
    }
  }
  return NULL;
}

// Frees a function and its body, or marks it to be freed later if it is still running.
void release_func(Func *f) {
  if (f->running > 0) {
    f->stale = true;
    return;
  }
  arena_free(&f->arena);
  free(f->name);
  free(f);
}

// Removes a function by name, if there is one.
void remove_func(const char *name) {
  for (int i = 0; i < nfuncs; i++) {
    if (strcmp(funcs[i]->name, name) == 0) {
      Func *f = funcs[i];
      nfuncs--;
      funcs[i] = funcs[nfuncs];
      release_func(f);
      return;
    }
  }
}

// Adds a function, replacing any existing one with the same name.
void add_func(Func *f) {
  remove_func(f->name);
  funcs = xrealloc(funcs, (nfuncs + 1) * sizeof(Func *));
  funcs[nfuncs++] = f;
}

// True if the path is a regular file we are allowed to run.
bool is_executable(const char *file) {
  struct stat st;
  if (stat(file, &st) != 0 || !S_ISREG(st.st_mode)) {
    return false;
  }
  return access(file, X_OK) == 0;
}

// Looks for a command in each directory of path.
// Returns the full path, or NULL if not found.
char *search_path(const char *name, const char *path) {
  if (path == NULL) {
    return NULL;
  }
  const char *dir = path;
  while (true) {
    const char *end = strchr(dir, ':');
    size_t dir_len = end != NULL ? (size_t)(end - dir) : strlen(dir);
    Str full = {0};
    if (dir_len == 0) {
      str_add(&full, ".");
    } else {
      str_add_len(&full, dir, dir_len);
    }
    str_add_char(&full, '/');
    str_add(&full, name);
    if (is_executable(full.data)) {
      return str_take(&full);
    }
    str_free(&full);
    if (end == NULL) {
      return NULL;
    }
    dir = end + 1;
  }
}

// Returns the remembered path for a command, or NULL.
static const char *hash_lookup(const char *name) {
  for (int i = 0; i < nhash; i++) {
    if (strcmp(hash_names[i], name) == 0) {
      return hash_paths[i];
    }
  }
  return NULL;
}

// Remembers where a command was found.
static void hash_add(const char *name, const char *path) {
  for (int i = 0; i < nhash; i++) {
    if (strcmp(hash_names[i], name) == 0) {
      free(hash_paths[i]);
      hash_paths[i] = xstrdup(path);
      return;
    }
  }
  hash_names = xrealloc(hash_names, (nhash + 1) * sizeof(char *));
  hash_paths = xrealloc(hash_paths, (nhash + 1) * sizeof(char *));
  hash_names[nhash] = xstrdup(name);
  hash_paths[nhash] = xstrdup(path);
  nhash++;
}

// Returns the full path of a command, using the remembered path if it still works.
char *find_command(const char *name) {
  if (strchr(name, '/') != NULL) {
    return xstrdup(name);
  }
  const char *cached = hash_lookup(name);
  if (cached != NULL && is_executable(cached)) {
    return xstrdup(cached);
  }
  char *found = search_path(name, get_var("PATH"));
  if (found != NULL) {
    hash_add(name, found);
  }
  return found;
}

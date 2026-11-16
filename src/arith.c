#include "shell.h"

typedef struct {
  const char *s;
  size_t pos;
  bool error;
  char msg[128];
} Arith;

static long arith_assign(Arith *a, bool eval);

// Records an arithmetic error, keeping only the first one.
static void arith_fail(Arith *a, const char *msg) {
  if (!a->error) {
    a->error = true;
    snprintf(a->msg, sizeof(a->msg), "%s", msg);
  }
}

// Skips spaces, tabs and newlines in the expression.
static void arith_skip(Arith *a) {
  while (isspace((unsigned char)a->s[a->pos])) {
    a->pos++;
  }
}

// True if the expression continues with the operator op.
static bool arith_peek(Arith *a, const char *op) {
  arith_skip(a);
  return strncmp(a->s + a->pos, op, strlen(op)) == 0;
}

// True if the expression continues with op, but not when op is followed
// by a character in not_followed. This tells < apart from <= and <<.
static bool arith_peek_exact(Arith *a, const char *op, const char *not_followed) {
  if (!arith_peek(a, op)) {
    return false;
  }
  char next = a->s[a->pos + strlen(op)];
  return next == '\0' || strchr(not_followed, next) == NULL;
}

// Converts decimal, 0x hex or 0 octal text to a number.
static bool arith_number(const char *s, long *out) {
  while (isspace((unsigned char)*s)) {
    s++;
  }
  if (*s == '\0') {
    *out = 0;
    return true;
  }
  char *end;
  errno = 0;
  long n = strtol(s, &end, 0);
  while (isspace((unsigned char)*end)) {
    end++;
  }
  if (end == s || *end != '\0' || errno != 0) {
    return false;
  }
  *out = n;
  return true;
}

// Returns a variable's value as a number. Unset or empty counts as 0.
static long arith_var(Arith *a, const char *name) {
  const char *v = get_var(name);
  if (v == NULL) {
    if (opt('u')) {
      char msg[128];
      snprintf(msg, sizeof(msg), "%s: parameter not set", name);
      arith_fail(a, msg);
    }
    return 0;
  }
  long n;
  if (!arith_number(v, &n)) {
    char msg[128];
    snprintf(msg, sizeof(msg), "%s: bad number '%s'", name, v);
    arith_fail(a, msg);
    return 0;
  }
  return n;
}

// Parses a number, a variable or a (bracketed) expression.
static long arith_primary(Arith *a, bool eval) {
  arith_skip(a);
  const char *s = a->s;
  if (s[a->pos] == '(') {
    a->pos++;
    long v = arith_assign(a, eval);
    arith_skip(a);
    if (s[a->pos] != ')') {
      arith_fail(a, "missing )");
      return 0;
    }
    a->pos++;
    return v;
  }
  if (isdigit((unsigned char)s[a->pos])) {
    char *end;
    errno = 0;
    long n = strtol(s + a->pos, &end, 0);
    if (errno != 0 || is_name_char(*end)) {
      arith_fail(a, "bad number");
      return 0;
    }
    a->pos = end - s;
    return n;
  }
  if (is_name_start(s[a->pos])) {
    size_t start = a->pos;
    while (is_name_char(s[a->pos])) {
      a->pos++;
    }
    char *name = xstrndup(s + start, a->pos - start);
    long v = eval ? arith_var(a, name) : 0;
    free(name);
    return v;
  }
  if (s[a->pos] == '\0') {
    arith_fail(a, "missing operand");
  } else {
    arith_fail(a, "syntax error");
  }
  return 0;
}

// Parses the unary operators + - ! ~.
static long arith_unary(Arith *a, bool eval) {
  arith_skip(a);
  char c = a->s[a->pos];
  if (c == '+' || c == '-' || c == '!' || c == '~') {
    a->pos++;
    long v = arith_unary(a, eval);
    if (c == '-') {
      return -v;
    }
    if (c == '!') {
      return !v;
    }
    if (c == '~') {
      return ~v;
    }
    return v;
  }
  return arith_primary(a, eval);
}

// Parses * / %. Division by zero is an error.
static long arith_mul(Arith *a, bool eval) {
  long left = arith_unary(a, eval);
  while (!a->error) {
    char op;
    if (arith_peek_exact(a, "*", "=")) {
      op = '*';
    } else if (arith_peek_exact(a, "/", "=")) {
      op = '/';
    } else if (arith_peek_exact(a, "%", "=")) {
      op = '%';
    } else {
      break;
    }
    a->pos++;
    long right = arith_unary(a, eval);
    if (op == '*') {
      left = left * right;
    } else if (right == 0) {
      if (eval) {
        arith_fail(a, "division by zero");
      }
      left = 0;
    } else if (op == '/') {
      left = left / right;
    } else {
      left = left % right;
    }
  }
  return left;
}

// Parses + and -.
static long arith_add(Arith *a, bool eval) {
  long left = arith_mul(a, eval);
  while (!a->error) {
    if (arith_peek_exact(a, "+", "=")) {
      a->pos++;
      left = left + arith_mul(a, eval);
    } else if (arith_peek_exact(a, "-", "=")) {
      a->pos++;
      left = left - arith_mul(a, eval);
    } else {
      break;
    }
  }
  return left;
}

// Parses << and >>.
static long arith_shift(Arith *a, bool eval) {
  long left = arith_add(a, eval);
  while (!a->error) {
    if (arith_peek_exact(a, "<<", "=")) {
      a->pos += 2;
      left = left << arith_add(a, eval);
    } else if (arith_peek_exact(a, ">>", "=")) {
      a->pos += 2;
      left = left >> arith_add(a, eval);
    } else {
      break;
    }
  }
  return left;
}

// Parses < <= > >=.
static long arith_rel(Arith *a, bool eval) {
  long left = arith_shift(a, eval);
  while (!a->error) {
    if (arith_peek(a, "<=")) {
      a->pos += 2;
      left = left <= arith_shift(a, eval);
    } else if (arith_peek(a, ">=")) {
      a->pos += 2;
      left = left >= arith_shift(a, eval);
    } else if (arith_peek_exact(a, "<", "<")) {
      a->pos++;
      left = left < arith_shift(a, eval);
    } else if (arith_peek_exact(a, ">", ">")) {
      a->pos++;
      left = left > arith_shift(a, eval);
    } else {
      break;
    }
  }
  return left;
}

// Parses == and !=.
static long arith_eq(Arith *a, bool eval) {
  long left = arith_rel(a, eval);
  while (!a->error) {
    if (arith_peek(a, "==")) {
      a->pos += 2;
      left = left == arith_rel(a, eval);
    } else if (arith_peek(a, "!=")) {
      a->pos += 2;
      left = left != arith_rel(a, eval);
    } else {
      break;
    }
  }
  return left;
}

// Parses bitwise &.
static long arith_band(Arith *a, bool eval) {
  long left = arith_eq(a, eval);
  while (!a->error && arith_peek_exact(a, "&", "&=")) {
    a->pos++;
    left = left & arith_eq(a, eval);
  }
  return left;
}

// Parses bitwise ^.
static long arith_bxor(Arith *a, bool eval) {
  long left = arith_band(a, eval);
  while (!a->error && arith_peek_exact(a, "^", "=")) {
    a->pos++;
    left = left ^ arith_band(a, eval);
  }
  return left;
}

// Parses bitwise |.
static long arith_bor(Arith *a, bool eval) {
  long left = arith_bxor(a, eval);
  while (!a->error && arith_peek_exact(a, "|", "|=")) {
    a->pos++;
    left = left | arith_bxor(a, eval);
  }
  return left;
}

// Parses &&. The right side isn't evaluated when the left is 0.
static long arith_land(Arith *a, bool eval) {
  long left = arith_bor(a, eval);
  while (!a->error && arith_peek(a, "&&")) {
    a->pos += 2;
    long right = arith_bor(a, eval && left != 0);
    left = left != 0 && right != 0;
  }
  return left;
}

// Parses ||. The right side isn't evaluated when the left is not 0.
static long arith_lor(Arith *a, bool eval) {
  long left = arith_land(a, eval);
  while (!a->error && arith_peek(a, "||")) {
    a->pos += 2;
    long right = arith_land(a, eval && left == 0);
    left = left != 0 || right != 0;
  }
  return left;
}

// Parses cond ? a : b, evaluating only the side that is chosen.
static long arith_ternary(Arith *a, bool eval) {
  long cond = arith_lor(a, eval);
  if (a->error || !arith_peek(a, "?")) {
    return cond;
  }
  a->pos++;
  long yes = arith_assign(a, eval && cond != 0);
  arith_skip(a);
  if (a->s[a->pos] != ':') {
    arith_fail(a, "missing :");
    return 0;
  }
  a->pos++;
  long no = arith_ternary(a, eval && cond == 0);
  return cond != 0 ? yes : no;
}

static const char *assign_ops[] = {"<<=", ">>=", "+=", "-=", "*=", "/=",
                                   "%=",  "&=",  "^=", "|=", "="};

// Works out the new value for an assignment such as += or <<=.
static long apply_assign_op(Arith *a, const char *op, long left, long right,
                            bool eval) {
  if (strcmp(op, "=") == 0) {
    return right;
  }
  if ((strcmp(op, "/=") == 0 || strcmp(op, "%=") == 0) && right == 0) {
    if (eval) {
      arith_fail(a, "division by zero");
    }
    return 0;
  }
  if (strcmp(op, "+=") == 0) {
    return left + right;
  }
  if (strcmp(op, "-=") == 0) {
    return left - right;
  }
  if (strcmp(op, "*=") == 0) {
    return left * right;
  }
  if (strcmp(op, "/=") == 0) {
    return left / right;
  }
  if (strcmp(op, "%=") == 0) {
    return left % right;
  }
  if (strcmp(op, "<<=") == 0) {
    return left << right;
  }
  if (strcmp(op, ">>=") == 0) {
    return left >> right;
  }
  if (strcmp(op, "&=") == 0) {
    return left & right;
  }
  if (strcmp(op, "^=") == 0) {
    return left ^ right;
  }
  return left | right;
}

// Parses an assignment such as x = 5 or x += 1, or else a ternary expression.
static long arith_assign(Arith *a, bool eval) {
  arith_skip(a);
  size_t start = a->pos;
  if (is_name_start(a->s[a->pos])) {
    size_t end = a->pos;
    while (is_name_char(a->s[end])) {
      end++;
    }
    size_t after = end;
    while (isspace((unsigned char)a->s[after])) {
      after++;
    }
    for (int i = 0; i < 11; i++) {
      const char *op = assign_ops[i];
      size_t n = strlen(op);
      if (strncmp(a->s + after, op, n) != 0) {
        continue;
      }
      if (strcmp(op, "=") == 0 && a->s[after + 1] == '=') {
        break;
      }
      char *name = xstrndup(a->s + start, end - start);
      a->pos = after + n;
      long right = arith_assign(a, eval);
      long left = 0;
      if (strcmp(op, "=") != 0 && eval) {
        left = arith_var(a, name);
      }
      long value = apply_assign_op(a, op, left, right, eval);
      if (eval && !a->error) {
        set_var_int(name, value);
      }
      free(name);
      return value;
    }
  }
  a->pos = start;
  return arith_ternary(a, eval);
}

// Evaluates the text inside $((...)). Prints an error and returns false
// if the expression is invalid.
bool arith_eval(const char *expr, long *result) {
  Arith a;
  a.s = expr;
  a.pos = 0;
  a.error = false;
  a.msg[0] = '\0';
  long v = arith_assign(&a, true);
  arith_skip(&a);
  if (!a.error && a.s[a.pos] != '\0') {
    arith_fail(&a, "syntax error");
  }
  if (a.error) {
    shell_error("arithmetic expression: %s: \"%s\"", a.msg, expr);
    return false;
  }
  *result = v;
  return true;
}

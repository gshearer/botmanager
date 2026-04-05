#ifndef BM_MATH_EXPR_H
#define BM_MATH_EXPR_H

// Evaluate a mathematical expression string.
// Supports: +, -, *, /, %, ^ (power), parentheses, and built-in
// functions/constants (abs, acos, asin, atan, atan2, ceil, cos, cosh,
// e, exp, fac, floor, ln, log, log10, ncr, npr, pi, pow, sin, sinh,
// sqrt, tan, tanh).
// returns: result as double, or NAN on parse/evaluation error
// expression: NUL-terminated expression string
// error: if non-NULL, receives 0 on success or 1-based position of
//        the character where parsing failed
double math_eval(const char *expression, int *error);

#ifdef MATH_EXPR_INTERNAL

#include <stdbool.h>
#include <stddef.h>

// -----------------------------------------------------------------------
// Token types
// -----------------------------------------------------------------------

enum {
  TOK_END,
  TOK_NUM,
  TOK_IDENT,
  TOK_PLUS,
  TOK_MINUS,
  TOK_STAR,
  TOK_SLASH,
  TOK_PERCENT,
  TOK_CARET,
  TOK_LPAREN,
  TOK_RPAREN,
  TOK_COMMA,
  TOK_ERR,
};

// -----------------------------------------------------------------------
// Safety limits
// -----------------------------------------------------------------------

// Maximum expression length in bytes.  Prevents callers without their
// own length cap from feeding arbitrarily large input.
#define MATH_INPUT_MAX  4096

// Maximum recursion depth.  Limits stack growth from deeply nested
// parentheses, chained function calls (e.g. sin sin sin ... 1), or
// exponent chains (e.g. 2^2^2^...^2).
#define MATH_DEPTH_MAX  64

// -----------------------------------------------------------------------
// Parser state
// -----------------------------------------------------------------------

#define IDENT_MAX 32

typedef struct {
  const char *start;              // beginning of expression
  const char *pos;                // current scan position
  int         tok;                // current token type
  int         depth;              // current recursion depth
  double      num;                // numeric value (TOK_NUM)
  char        ident[IDENT_MAX];   // identifier text (TOK_IDENT)
} parser_t;

// -----------------------------------------------------------------------
// Built-in function table
// -----------------------------------------------------------------------

typedef double (*fn0_t)(void);
typedef double (*fn1_t)(double);
typedef double (*fn2_t)(double, double);

typedef struct {
  const char *name;
  int         arity;
  union {
    fn0_t f0;
    fn1_t f1;
    fn2_t f2;
  };
} builtin_t;

#define BUILTIN_COUNT (sizeof(builtins) / sizeof(builtins[0]))

// Built-in helpers.
static double              const_pi(void);
static double              const_e(void);
static double              fn_fac(double a);
static double              fn_ncr(double n, double r);
static double              fn_npr(double n, double r);

// Builtin lookup.
static const builtin_t    *find_builtin(const char *name);

// Tokenizer.
static void                advance(parser_t *p);

// Recursive descent parser — direct evaluation.
static double              parse_expr(parser_t *p);
static double              parse_term(parser_t *p);
static double              parse_unary(parser_t *p);
static double              parse_power(parser_t *p);
static double              parse_call(parser_t *p);
static double              parse_primary(parser_t *p);

#endif // MATH_EXPR_INTERNAL

#endif // BM_MATH_EXPR_H

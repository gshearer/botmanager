// botmanager — MIT
// Recursive-descent math expression evaluator: single-pass, no AST, no heap.

#define MATH_EXPR_INTERNAL
#include "math_expr.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef NAN
#define NAN (0.0/0.0)
#endif

#ifndef INFINITY
#define INFINITY (1.0/0.0)
#endif

// Built-in constants and helper functions

static double
const_pi(void)
{
  return 3.14159265358979323846;
}

static double
const_e(void)
{
  return 2.71828182845904523536;
}

static double
fn_fac(double a)
{
  unsigned int  n;
  unsigned long result = 1;

  if(a < 0.0)
    return NAN;

  if(a > (double)UINT_MAX)
    return INFINITY;

  n = (unsigned int)a;

  for(unsigned int i = 1; i <= n; i++)
  {
    if(i > ULONG_MAX / result)
      return INFINITY;

    result *= i;
  }

  return (double)result;
}

// r: items chosen (non-negative, must not exceed n)
static double
fn_ncr(double n, double r)
{
  unsigned int  un;
  unsigned int  ur;
  unsigned long result = 1;

  if(n < 0.0 || r < 0.0 || n < r)
    return NAN;

  if(n > (double)UINT_MAX || r > (double)UINT_MAX)
    return INFINITY;

  un = (unsigned int)n;
  ur = (unsigned int)r;

  if(ur > un / 2)
    ur = un - ur;

  for(unsigned int i = 1; i <= ur; i++)
  {
    if(result > ULONG_MAX / (un - ur + i))
      return INFINITY;

    result *= un - ur + i;
    result /= i;
  }

  return (double)result;
}

// r: items chosen (non-negative, must not exceed n)
static double
fn_npr(double n, double r)
{
  return fn_ncr(n, r) * fn_fac(r);
}

// Built-in function/constant table.  Must be sorted alphabetically
// for binary search.
static const builtin_t builtins[] = {
  { "abs",   1, { .f1 = fabs    } },
  { "acos",  1, { .f1 = acos    } },
  { "asin",  1, { .f1 = asin    } },
  { "atan",  1, { .f1 = atan    } },
  { "atan2", 2, { .f2 = atan2   } },
  { "ceil",  1, { .f1 = ceil    } },
  { "cos",   1, { .f1 = cos     } },
  { "cosh",  1, { .f1 = cosh    } },
  { "e",     0, { .f0 = const_e } },
  { "exp",   1, { .f1 = exp     } },
  { "fac",   1, { .f1 = fn_fac  } },
  { "floor", 1, { .f1 = floor   } },
  { "ln",    1, { .f1 = log     } },
  { "log",   1, { .f1 = log10   } },
  { "log10", 1, { .f1 = log10   } },
  { "ncr",   2, { .f2 = fn_ncr  } },
  { "npr",   2, { .f2 = fn_npr  } },
  { "pi",    0, { .f0 = const_pi } },
  { "pow",   2, { .f2 = pow     } },
  { "sin",   1, { .f1 = sin     } },
  { "sinh",  1, { .f1 = sinh    } },
  { "sqrt",  1, { .f1 = sqrt    } },
  { "tan",   1, { .f1 = tan     } },
  { "tanh",  1, { .f1 = tanh    } },
};

static const builtin_t *
find_builtin(const char *name)
{
  int lo = 0;
  int hi = (int)BUILTIN_COUNT - 1;

  while(lo <= hi)
  {
    int mid = lo + (hi - lo) / 2;
    int cmp = strncmp(name, builtins[mid].name, IDENT_MAX);

    if(cmp == 0)
      return &builtins[mid];

    if(cmp < 0)
      hi = mid - 1;
    else
      lo = mid + 1;
  }

  return NULL;
}

// Tokenizer

static void
advance(parser_t *p)
{
  while(*p->pos == ' ' || *p->pos == '\t'
      || *p->pos == '\n' || *p->pos == '\r')
    p->pos++;

  if(*p->pos == '\0')
  {
    p->tok = TOK_END;
    return;
  }

  // Numeric literal.
  if((*p->pos >= '0' && *p->pos <= '9') || *p->pos == '.')
  {
    char *end;

    p->num = strtod(p->pos, &end);

    if(end == p->pos)
    {
      p->tok = TOK_ERR;
      return;
    }

    p->pos = end;
    p->tok = TOK_NUM;
    return;
  }

  // Identifier (function name or constant).
  if(isalpha((unsigned char)*p->pos) || *p->pos == '_')
  {
    size_t len = 0;

    while(isalnum((unsigned char)*p->pos) || *p->pos == '_')
    {
      if(len < IDENT_MAX - 1)
        p->ident[len++] = *p->pos;

      p->pos++;
    }

    p->ident[len] = '\0';
    p->tok = TOK_IDENT;
    return;
  }

  // Single-character operator or delimiter.
  switch(*p->pos)
  {
    case '+': p->tok = TOK_PLUS;    p->pos++; return;
    case '-': p->tok = TOK_MINUS;   p->pos++; return;
    case '*': p->tok = TOK_STAR;    p->pos++; return;
    case '/': p->tok = TOK_SLASH;   p->pos++; return;
    case '%': p->tok = TOK_PERCENT; p->pos++; return;
    case '^': p->tok = TOK_CARET;   p->pos++; return;
    case '(': p->tok = TOK_LPAREN;  p->pos++; return;
    case ')': p->tok = TOK_RPAREN;  p->pos++; return;
    case ',': p->tok = TOK_COMMA;   p->pos++; return;
    default:  p->tok = TOK_ERR;              return;
  }
}

// Recursive-descent parser — direct evaluation.
//
// Grammar:
//   expr    = term (('+' | '-') term)*
//   term    = unary (('*' | '/' | '%') unary)*
//   unary   = ('-' | '+')* power
//   power   = call ('^' power)?          // right-associative
//   call    = IDENT '(' args ')' | IDENT power | primary
//   primary = NUMBER | '(' expr ')'
//   args    = expr (',' expr)*

// p: parser state (tok must be primed by advance())
static double
parse_expr(parser_t *p)
{
  double val;

  if(++p->depth > MATH_DEPTH_MAX)
  {
    p->tok = TOK_ERR;
    return NAN;
  }

  val = parse_term(p);

  while(p->tok == TOK_PLUS || p->tok == TOK_MINUS)
  {
    int    op = p->tok;
    double rhs;

    advance(p);
    rhs = parse_term(p);

    if(op == TOK_PLUS)
      val += rhs;
    else
      val -= rhs;
  }

  p->depth--;
  return val;
}

static double
parse_term(parser_t *p)
{
  double val = parse_unary(p);

  while(p->tok == TOK_STAR || p->tok == TOK_SLASH
      || p->tok == TOK_PERCENT)
  {
    int    op = p->tok;
    double rhs;

    advance(p);
    rhs = parse_unary(p);

    if(op == TOK_STAR)
      val *= rhs;

    else if(op == TOK_SLASH)
      val /= rhs;

    else
      val = fmod(val, rhs);
  }

  return val;
}

static double
parse_unary(parser_t *p)
{
  int    sign = 1;
  double val;

  while(p->tok == TOK_PLUS || p->tok == TOK_MINUS)
  {
    if(p->tok == TOK_MINUS)
      sign = -sign;

    advance(p);
  }

  val = parse_power(p);

  return sign < 0 ? -val : val;
}

static double
parse_power(parser_t *p)
{
  double base;

  if(++p->depth > MATH_DEPTH_MAX)
  {
    p->tok = TOK_ERR;
    return NAN;
  }

  base = parse_call(p);

  if(p->tok == TOK_CARET)
  {
    double exponent;

    advance(p);
    exponent = parse_power(p);
    p->depth--;
    return pow(base, exponent);
  }

  p->depth--;
  return base;
}

static double
parse_call(parser_t *p)
{
  char             name[IDENT_MAX];
  const builtin_t *fn;
  double           a0;
  double           a1;

  if(p->tok != TOK_IDENT)
    return parse_primary(p);

  memcpy(name, p->ident, IDENT_MAX);

  fn = find_builtin(name);

  if(fn == NULL)
  {
    p->tok = TOK_ERR;
    return NAN;
  }

  advance(p);

  // Zero-arity: constant with optional empty parens.
  if(fn->arity == 0)
  {
    if(p->tok == TOK_LPAREN)
    {
      advance(p);

      if(p->tok != TOK_RPAREN)
      {
        p->tok = TOK_ERR;
        return NAN;
      }

      advance(p);
    }

    return fn->f0();
  }

  // Single-argument: fn(expr) or fn power (implicit parens).
  if(fn->arity == 1)
  {
    if(p->tok == TOK_LPAREN)
    {
      double arg;

      advance(p);
      arg = parse_expr(p);

      if(p->tok != TOK_RPAREN)
      {
        p->tok = TOK_ERR;
        return NAN;
      }

      advance(p);
      return fn->f1(arg);
    }

    return fn->f1(parse_power(p));
  }

  // Multi-argument: require parenthesised, comma-separated args.
  if(p->tok != TOK_LPAREN)
  {
    p->tok = TOK_ERR;
    return NAN;
  }

  advance(p);

  a0 = parse_expr(p);

  for(int i = 1; i < fn->arity; i++)
  {
    if(p->tok != TOK_COMMA)
    {
      p->tok = TOK_ERR;
      return NAN;
    }

    advance(p);
  }

  a1 = parse_expr(p);

  if(p->tok != TOK_RPAREN)
  {
    p->tok = TOK_ERR;
    return NAN;
  }

  advance(p);
  return fn->f2(a0, a1);
}

static double
parse_primary(parser_t *p)
{
  if(p->tok == TOK_NUM)
  {
    double val = p->num;

    advance(p);
    return val;
  }

  if(p->tok == TOK_LPAREN)
  {
    double val;

    advance(p);
    val = parse_expr(p);

    if(p->tok != TOK_RPAREN)
    {
      p->tok = TOK_ERR;
      return NAN;
    }

    advance(p);
    return val;
  }

  p->tok = TOK_ERR;
  return NAN;
}

// Public API

double
math_eval(const char *expression, int *error)
{
  parser_t p;
  double   result;

  if(expression == NULL || *expression == '\0')
  {
    if(error)
      *error = 1;

    return NAN;
  }

  // Reject oversized input to prevent abuse from callers that lack
  // their own length limits.
  if(strnlen(expression, MATH_INPUT_MAX + 1) > MATH_INPUT_MAX)
  {
    if(error)
      *error = 1;

    return NAN;
  }

  p.start = expression;
  p.pos   = expression;
  p.depth = 0;
  advance(&p);

  result = parse_expr(&p);

  if(p.tok == TOK_ERR || p.tok != TOK_END)
  {
    if(error)
    {
      *error = (int)(p.pos - p.start);

      if(*error == 0)
        *error = 1;
    }

    return NAN;
  }

  if(error)
    *error = 0;

  return result;
}

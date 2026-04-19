#include "te.h"
#include <math.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

/* Recursive-descent parser — grammar:
 *   expr   = term   { +|- term }
 *   term   = factor { *|/ factor }
 *   factor = base   { ^ factor }    (right-associative)
 *   base   = -base | number | x | pi | e | func(expr) | (expr) */

typedef struct {
    const char *p;
    double      x;
    te_err_t    err;
} te_state_t;

static double parse_expr  (te_state_t *s);
static double parse_term  (te_state_t *s);
static double parse_factor(te_state_t *s);
static double parse_base  (te_state_t *s);

static void skip_ws(te_state_t *s) {
    while (*s->p && isspace((unsigned char)*s->p)) s->p++;
}
static int peek  (te_state_t *s, char c) { skip_ws(s); return *s->p == c; }
static int accept(te_state_t *s, char c) {
    skip_ws(s);
    if (*s->p == c) { s->p++; return 1; }
    return 0;
}

/* Returns match length or 0 — ensures keyword isn't a prefix of a longer name */
static int match_kw(const char *p, const char *kw)
{
    size_t n = strlen(kw);
    if (strncmp(p, kw, n) == 0 && !isalnum((unsigned char)p[n]) && p[n] != '_')
        return (int)n;
    return 0;
}

static double parse_expr(te_state_t *s)
{
    double v = parse_term(s);
    while (s->err == TE_OK) {
        skip_ws(s);
        if      (accept(s, '+')) v += parse_term(s);
        else if (accept(s, '-')) v -= parse_term(s);
        else break;
    }
    return v;
}

static double parse_term(te_state_t *s)
{
    double v = parse_factor(s);
    while (s->err == TE_OK) {
        skip_ws(s);
        if (accept(s, '*')) {
            v *= parse_factor(s);
        } else if (accept(s, '/')) {
            double d = parse_factor(s);
            if (d == 0.0) { s->err = TE_DIV_ZERO; return 0.0; }
            v /= d;
        } else break;
    }
    return v;
}

static double parse_factor(te_state_t *s)
{
    double v = parse_base(s);
    if (s->err != TE_OK) return 0.0;
    skip_ws(s);
    if (accept(s, '^')) v = pow(v, parse_factor(s));   /* right-assoc: recurse */
    return v;
}

static double parse_base(te_state_t *s)
{
    if (s->err != TE_OK) return 0.0;
    skip_ws(s);

    if (accept(s, '-')) return -parse_base(s);
    if (accept(s, '+')) return  parse_base(s);

    if (accept(s, '(')) {
        double v = parse_expr(s);
        if (!accept(s, ')')) s->err = TE_BAD_EXPR;
        return v;
    }

    if (isalpha((unsigned char)*s->p)) {
        int n;
        if ((n = match_kw(s->p, "x")))  { s->p += n; return s->x;  }
        if ((n = match_kw(s->p, "pi"))) { s->p += n; return M_PI;  }
        if ((n = match_kw(s->p, "e")))  { s->p += n; return M_E;   }

        /* Macro expands each single-arg function — match log10 before log */
        #define FN1(name, fn) \
            if ((n = match_kw(s->p, name))) { \
                s->p += n; skip_ws(s); \
                if (!accept(s, '(')) { s->err = TE_BAD_EXPR; return 0.0; } \
                double arg = parse_expr(s); \
                if (!accept(s, ')')) { s->err = TE_BAD_EXPR; return 0.0; } \
                return fn(arg); \
            }
        FN1("sqrt",  sqrt)  FN1("abs",   fabs)
        FN1("sin",   sin)   FN1("cos",   cos)   FN1("tan",   tan)
        FN1("asin",  asin)  FN1("acos",  acos)  FN1("atan",  atan)
        FN1("log10", log10) FN1("log",   log)
        FN1("exp",   exp)   FN1("floor", floor) FN1("ceil",  ceil)
        FN1("round", round)
        #undef FN1

        s->err = TE_BAD_EXPR;
        return 0.0;
    }

    if (isdigit((unsigned char)*s->p) || *s->p == '.') {
        char *end;
        double v = strtod(s->p, &end);
        if (end == s->p) { s->err = TE_BAD_EXPR; return 0.0; }
        s->p = end;
        return v;
    }

    s->err = TE_BAD_EXPR;
    return 0.0;
}

te_err_t te_eval(const char *expr, double x, double *out)
{
    if (!expr || !*expr || !out) return TE_BAD_EXPR;

    te_state_t s = { .p = expr, .x = x, .err = TE_OK };
    double result = parse_expr(&s);

    if (s.err != TE_OK) return s.err;

    /* Reject trailing garbage */
    skip_ws(&s);
    if (*s.p != '\0') return TE_BAD_EXPR;

    if (!isfinite(result)) return TE_OVERFLOW;

    *out = result;
    return TE_OK;
}

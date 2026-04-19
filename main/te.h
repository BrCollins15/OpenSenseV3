#pragma once
#include <stddef.h>

/* Operators: + - * / ^ ( )
 * Functions: sqrt abs sin cos tan asin acos atan log log10 exp floor ceil round
 * Constants: pi  e
 * Variable:  x  (set by caller — typically raw sensor ADC value) */

typedef enum {
    TE_OK        = 0,
    TE_DIV_ZERO  = 1,
    TE_BAD_EXPR  = 2,   /* parse error / unknown symbol */
    TE_OVERFLOW  = 3,   /* result is inf or nan */
} te_err_t;

te_err_t te_eval(const char *expr, double x, double *out);

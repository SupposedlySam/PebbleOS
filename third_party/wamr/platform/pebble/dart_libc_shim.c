/* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
/* libc functions WAMR needs that PebbleOS's pblibc does not provide. The
   symbols are absent (not just undeclared), so we implement them here. These
   are simple, correct-enough implementations; the float-parsing ones are not
   on any path the on-device modules currently exercise. */

#include <math.h>
#include <stddef.h>
#include <string.h>

static int dart_tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* pblibc lacks qsort/bsearch. When CONFIG_MODDABLE_XS is enabled the Moddable
   build vendors its own qsort/bsearch, so only provide ours when XS is off (to
   avoid duplicate symbols). Small-N usage in WAMR (export tables), so a simple
   insertion sort is fine. */
#ifndef CONFIG_MODDABLE_XS
void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *)) {
  char *a = (char *)base;
  for (size_t i = 1; i < nmemb; i++) {
    for (size_t j = i; j > 0; j--) {
      char *x = a + (j - 1) * size, *y = a + j * size;
      if (compar(x, y) <= 0) {
        break;
      }
      for (size_t k = 0; k < size; k++) {
        char t = x[k];
        x[k] = y[k];
        y[k] = t;
      }
    }
  }
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *)) {
  size_t lo = 0, hi = nmemb;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    const char *p = (const char *)base + mid * size;
    int c = compar(key, p);
    if (c == 0) {
      return (void *)p;
    } else if (c < 0) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return NULL;
}
#endif /* !CONFIG_MODDABLE_XS */

int strncasecmp(const char *a, const char *b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    int ca = dart_tolower((unsigned char)a[i]);
    int cb = dart_tolower((unsigned char)b[i]);
    if (ca != cb) {
      return ca - cb;
    }
    if (ca == 0) {
      return 0;
    }
  }
  return 0;
}

char *strtok_r(char *str, const char *delim, char **saveptr) {
  if (str == NULL) {
    str = *saveptr;
  }
  str += strspn(str, delim);
  if (*str == '\0') {
    *saveptr = str;
    return NULL;
  }
  char *end = str + strcspn(str, delim);
  if (*end != '\0') {
    *end++ = '\0';
  }
  *saveptr = end;
  return str;
}

unsigned long long strtoull(const char *s, char **endptr, int base) {
  while (*s == ' ' || *s == '\t') {
    s++;
  }
  if (base == 0) {
    base = 10;
  }
  unsigned long long v = 0;
  for (;;) {
    char c = *s;
    int d;
    if (c >= '0' && c <= '9') {
      d = c - '0';
    } else if (c >= 'a' && c <= 'z') {
      d = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'Z') {
      d = c - 'A' + 10;
    } else {
      break;
    }
    if (d >= base) {
      break;
    }
    v = v * (unsigned long long)base + (unsigned long long)d;
    s++;
  }
  if (endptr) {
    *endptr = (char *)s;
  }
  return v;
}

long long strtoll(const char *s, char **endptr, int base) {
  while (*s == ' ' || *s == '\t') {
    s++;
  }
  int neg = 0;
  if (*s == '-') {
    neg = 1;
    s++;
  } else if (*s == '+') {
    s++;
  }
  unsigned long long v = strtoull(s, endptr, base);
  return neg ? -(long long)v : (long long)v;
}

double strtod(const char *s, char **endptr) {
  while (*s == ' ' || *s == '\t') {
    s++;
  }
  int neg = 0;
  if (*s == '-') {
    neg = 1;
    s++;
  } else if (*s == '+') {
    s++;
  }
  double v = 0.0;
  while (*s >= '0' && *s <= '9') {
    v = v * 10.0 + (*s - '0');
    s++;
  }
  if (*s == '.') {
    s++;
    double f = 0.1;
    while (*s >= '0' && *s <= '9') {
      v += (*s - '0') * f;
      f *= 0.1;
      s++;
    }
  }
  if (endptr) {
    *endptr = (char *)s;
  }
  return neg ? -v : v;
}

float strtof(const char *s, char **endptr) { return (float)strtod(s, endptr); }

double trunc(double x) { return x < 0.0 ? ceil(x) : floor(x); }
float truncf(float x) { return (float)trunc((double)x); }

double rint(double x) {
  double f = floor(x);
  double d = x - f;
  if (d < 0.5) {
    return f;
  }
  if (d > 0.5) {
    return f + 1.0;
  }
  /* tie: round to even */
  return (fmod(f, 2.0) == 0.0) ? f : f + 1.0;
}
float rintf(float x) { return (float)rint((double)x); }

/* WAMR's libc-builtin ctype wrappers need these; pblibc provides only a subset
   on some boards. Defined weak so a board's own (strong) versions win when
   present, and these fill the gap otherwise. ASCII semantics. */
#define DART_WEAK __attribute__((weak))
DART_WEAK int isalpha(int c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
DART_WEAK int isdigit(int c) { return c >= '0' && c <= '9'; }
DART_WEAK int isalnum(int c) { return isalpha(c) || isdigit(c); }
DART_WEAK int isupper(int c) { return c >= 'A' && c <= 'Z'; }
DART_WEAK int islower(int c) { return c >= 'a' && c <= 'z'; }
DART_WEAK int isspace(int c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}
DART_WEAK int iscntrl(int c) { return (c >= 0 && c < 0x20) || c == 0x7f; }
DART_WEAK int isprint(int c) { return c >= 0x20 && c < 0x7f; }
DART_WEAK int isgraph(int c) { return c > 0x20 && c < 0x7f; }
DART_WEAK int ispunct(int c) { return isgraph(c) && !isalnum(c); }
DART_WEAK int isxdigit(int c) {
  return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* WAMR's C-API trap path (wasm_runtime_invoke_c_api_native) is compiled but
   never reached - our natives use the simple NativeSymbol ABI, not wasm_func_t.
   Stub the one C-API symbol it references so the link resolves. */
void wasm_trap_delete(void *trap) { (void)trap; }

/* newlib (freestanding) omits the double fmin/fmax; the AOT reloc symbol table
   (aot_reloc.h) references them so they must link. Weak: a real libm wins if
   present. NaN handling per C99 (return the non-NaN operand). */
DART_WEAK double fmin(double a, double b) {
  if (a != a) return b;
  if (b != b) return a;
  return a < b ? a : b;
}
DART_WEAK double fmax(double a, double b) {
  if (a != a) return b;
  if (b != b) return a;
  return a > b ? a : b;
}

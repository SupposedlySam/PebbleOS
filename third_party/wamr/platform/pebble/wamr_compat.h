/* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
/* Force-included into the WAMR engine TUs. The firmware ships a reduced libc
   header set (and builds -ffreestanding), so a number of standard functions
   WAMR uses are not declared even though the symbols exist in newlib's libc.
   We declare them here with their standard signatures so compilation (-Werror)
   succeeds; the linker resolves them against libc. */
#ifndef WAMR_COMPAT_H
#define WAMR_COMPAT_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* stdlib */
void abort(void) __attribute__((__noreturn__));
void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));
int atoi(const char *nptr);
long strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
long long strtoll(const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);
double strtod(const char *nptr, char **endptr);
float strtof(const char *nptr, char **endptr);

/* strings.h */
int strncasecmp(const char *s1, const char *s2, size_t n);
char *strtok_r(char *str, const char *delim, char **saveptr);

/* math */
double trunc(double x);
float truncf(float x);
double rint(double x);
float rintf(float x);
double sqrt(double x);
float sqrtf(float x);
double ceil(double x);
double floor(double x);

#ifdef __cplusplus
}
#endif
#endif /* WAMR_COMPAT_H */

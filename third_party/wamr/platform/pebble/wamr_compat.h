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

/* AOT: the .aot target string the loader validates against (check_machine_info,
   prefix-matched). wamrc emits arch "thumbv8m.main" (--target=thumbv8m.main
   --cpu=cortex-m33); "thumbv8m" is a matching prefix. aot_reloc_thumb.c reads
   BUILD_TARGET; WAMR's cmake normally sets it but our waf build does not. */
#ifndef BUILD_TARGET
#define BUILD_TARGET "thumbv8m"
#endif

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
float ceilf(float x);
double floor(double x);
float floorf(float x);
/* Registered in the AOT reloc symbol table (aot_reloc.h REG_COMMON_SYMBOLS) so
   AOT-compiled code can call them; newlib's freestanding math.h omits these. */
double fmin(double x, double y);
float fminf(float x, float y);
double fmax(double x, double y);
float fmaxf(float x, float y);

#ifdef __cplusplus
}
#endif
#endif /* WAMR_COMPAT_H */

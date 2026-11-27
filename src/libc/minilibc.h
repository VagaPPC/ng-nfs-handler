/* minilibc.h -- libc minima per nfs-handler.  NG-nfs-handler -- LGPL-2.1 */
#ifndef NG_MINILIBC_H
#define NG_MINILIBC_H

#include <stddef.h>
#include <stdarg.h>

int   mini_init(void);          /* crea il memory pool: chiamare per prima */
void  mini_exit(void);          /* distrugge il pool (libera TUTTO) */
const char *mini_errstr(int e);
extern void (*mini_assert_hook)(const char *file, int line, const char *expr);

void *malloc(size_t);
void *calloc(size_t, size_t);
void *realloc(void *, size_t);
void  free(void *);
char *strdup(const char *);
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
int   memcmp(const void *, const void *, size_t);
size_t strlen(const char *);
int   strcmp(const char *, const char *);
int   strncmp(const char *, const char *, size_t);
char *strcpy(char *, const char *);
char *strncpy(char *, const char *, size_t);
char *strcat(char *, const char *);
char *strchr(const char *, int);
char *strrchr(const char *, int);
int   vsnprintf(char *, size_t, const char *, va_list);
int   snprintf(char *, size_t, const char *, ...);

#endif

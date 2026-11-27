/*
 * minilibc.c -- libc minima per nfs-handler (niente startup code di libnix)
 *
 * Fornisce solo quello che serve a libnfs e all'handler:
 *   - memoria: malloc/calloc/realloc/free/strdup su un memory pool exec
 *     (DeletePool all'uscita libera tutto, anche eventuali leak)
 *   - stringhe: mem*, str*, bcopy, atoi, strtol, strerror
 *   - formattazione: vsnprintf/snprintf/sprintf (niente float)
 *   - varie: time, errno, get*id, pipe/dup2 (falliscono), __assert_func
 *
 * Tutto e' pensato per UN solo task (il processo dell'handler).
 *
 * NG-nfs-handler -- LGPL-2.1
 * Copyright (C) 2026 VagaPPC (Benedetto Lorello)
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "minilibc.h"

int errno;

/* ------------------------------------------------------------------ pool */

static APTR mem_pool;

/* Header di 8 byte davanti a ogni blocco: dimensione utile (per realloc/free)
 * e allineamento a 8 dei dati. */
struct mhdr { ULONG size; ULONG magic; };
#define MAGIC 0x4E465321UL      /* 'NFS!' */

int mini_init(void)
{
    if (mem_pool) return 0;
    /* MEMF_PUBLIC: lock e FileInfoBlock vengono letti anche da altri task */
    mem_pool = CreatePool(MEMF_PUBLIC, 32768, 8192);
    return mem_pool ? 0 : -1;
}

void mini_exit(void)
{
    if (mem_pool) {
        DeletePool(mem_pool);
        mem_pool = NULL;
    }
}

void *malloc(size_t n)
{
    struct mhdr *h;
    if (!mem_pool) return NULL;
    h = AllocPooled(mem_pool, n + sizeof(*h));
    if (!h) { errno = 12; /* ENOMEM */ return NULL; }
    h->size  = n;
    h->magic = MAGIC;
    return h + 1;
}

void free(void *p)
{
    struct mhdr *h;
    if (!p || !mem_pool) return;
    h = (struct mhdr *)p - 1;
    if (h->magic != MAGIC) return;      /* blocco non nostro: ignora */
    h->magic = 0;
    FreePooled(mem_pool, h, h->size + sizeof(*h));
}

void *calloc(size_t a, size_t b)
{
    size_t n = a * b;
    void *p = malloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void *realloc(void *p, size_t n)
{
    struct mhdr *h;
    void *q;
    if (!p) return malloc(n);
    if (!n) { free(p); return NULL; }
    h = (struct mhdr *)p - 1;
    if (h->size >= n) return p;
    q = malloc(n);
    if (!q) return NULL;
    memcpy(q, p, h->size);
    free(p);
    return q;
}

char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

/* --------------------------------------------------------------- memoria */

void *memcpy(void *d, const void *s, size_t n)
{
    CopyMem((APTR)s, d, n);
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *dp = d;
    const unsigned char *sp = s;
    if (dp == sp || !n) return d;
    if (dp < sp || dp >= sp + n) {
        CopyMem((APTR)s, d, n);
    } else {
        dp += n; sp += n;
        while (n--) *--dp = *--sp;
    }
    return d;
}

void bcopy(const void *s, void *d, size_t n) { memmove(d, s, n); }

void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y) return *x - *y;
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    for (; n; n--, p++)
        if (*p == (unsigned char)c) return (void *)p;
    return NULL;
}

/* --------------------------------------------------------------- stringhe */

size_t strlen(const char *s) { const char *p = s; while (*p) p++; return p - s; }

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        if (!*a) return 0;
    }
    return 0;
}

char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)); return r; }

char *strncpy(char *d, const char *s, size_t n)
{
    char *r = d;
    while (n && *s) { *d++ = *s++; n--; }
    while (n--) *d++ = 0;
    return r;
}

char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return NULL;
    }
}

char *strrchr(const char *s, int c)
{
    const char *r = NULL;
    for (;; s++) {
        if (*s == (char)c) r = s;
        if (!*s) return (char *)r;
    }
}

char *strstr(const char *h, const char *n)
{
    size_t l = strlen(n);
    if (!l) return (char *)h;
    for (; *h; h++)
        if (*h == *n && !strncmp(h, n, l)) return (char *)h;
    return NULL;
}

long strtol(const char *s, char **end, int base)
{
    long v = 0;
    int neg = 0, d;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2; base = 16;
    } else if (base == 0) {
        base = (*s == '0') ? 8 : 10;
    }
    for (;; s++) {
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
    }
    if (end) *end = (char *)s;
    return neg ? -v : v;
}

int atoi(const char *s) { return (int)strtol(s, NULL, 10); }

const char *mini_errstr(int e)
{
    switch (e) {
    case 0:   return "Success";
    case 1:   return "Operation not permitted";
    case 2:   return "No such file or directory";
    case 4:   return "Interrupted";
    case 5:   return "I/O error";
    case 9:   return "Bad file descriptor";
    case 12:  return "Out of memory";
    case 13:  return "Permission denied";
    case 17:  return "File exists";
    case 20:  return "Not a directory";
    case 21:  return "Is a directory";
    case 22:  return "Invalid argument";
    case 28:  return "No space left on device";
    case 30:  return "Read-only file system";
    case 32:  return "Broken pipe";
    case 35:  return "Resource temporarily unavailable";
    case 36:  return "Operation now in progress";
    case 54:  return "Connection reset by peer";
    case 57:  return "Socket is not connected";
    case 60:  return "Operation timed out";
    case 61:  return "Connection refused";
    case 63:  return "File name too long";
    case 64:  return "Host is down";
    case 65:  return "No route to host";
    case 66:  return "Directory not empty";
    case 70:  return "Stale NFS file handle";
    case 78:  return "Function not implemented";
    }
    return "Unknown error";
}

char *strerror(int e) { return (char *)mini_errstr(e); }

/* ---------------------------------------------------------- vsnprintf */

struct out { char *p; size_t left; size_t total; };

static void put(struct out *o, char c)
{
    if (o->left > 1) { *o->p++ = c; o->left--; }
    o->total++;
}

static void put_num(struct out *o, unsigned long long v, int base, int upper,
                    int neg, int width, int prec, int zero, int left,
                    char sign, int alt)
{
    char buf[24];
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0, len, pad;
    char pre[3];
    int npre = 0;

    if (v == 0 && prec != 0) buf[n++] = '0';
    while (v) { buf[n++] = dig[v % base]; v /= base; }

    if (neg) pre[npre++] = '-';
    else if (sign) pre[npre++] = sign;
    if (alt && base == 16 && n) { pre[npre++] = '0'; pre[npre++] = upper ? 'X' : 'x'; }

    len = (prec > n ? prec : n) + npre;
    pad = width > len ? width - len : 0;

    if (!left && !(zero && prec < 0)) while (pad-- > 0) put(o, ' ');
    { int i; for (i = 0; i < npre; i++) put(o, pre[i]); }
    if (!left && zero && prec < 0) while (pad-- > 0) put(o, '0');
    { int z = prec - n; while (z-- > 0) put(o, '0'); }
    while (n) put(o, buf[--n]);
    if (left) while (pad-- > 0) put(o, ' ');
}

int vsnprintf(char *dst, size_t size, const char *f, va_list ap)
{
    struct out o;
    o.p = dst; o.left = size; o.total = 0;

    for (; *f; f++) {
        int left = 0, zero = 0, alt = 0, width = 0, prec = -1, lng = 0;
        char sign = 0;

        if (*f != '%') { put(&o, *f); continue; }
        f++;
        for (;; f++) {
            if (*f == '-') left = 1;
            else if (*f == '0') zero = 1;
            else if (*f == '+') sign = '+';
            else if (*f == ' ') { if (!sign) sign = ' '; }
            else if (*f == '#') alt = 1;
            else break;
        }
        if (*f == '*') { width = va_arg(ap, int); if (width < 0) { left = 1; width = -width; } f++; }
        else while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == '.') {
            f++; prec = 0;
            if (*f == '*') { prec = va_arg(ap, int); f++; }
            else while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0');
        }
        for (;; f++) {
            if (*f == 'l') lng++;
            else if (*f == 'h') ;
            else if (*f == 'z' || *f == 'j' || *f == 't') lng = (sizeof(size_t) > 4) ? 2 : 1;
            else break;
        }

        switch (*f) {
        case 'd': case 'i': {
            long long v = lng >= 2 ? va_arg(ap, long long)
                        : lng == 1 ? va_arg(ap, long) : va_arg(ap, int);
            int neg = v < 0;
            put_num(&o, neg ? -(unsigned long long)v : (unsigned long long)v,
                    10, 0, neg, width, prec, zero, left, sign, 0);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            unsigned long long v = lng >= 2 ? va_arg(ap, unsigned long long)
                                 : lng == 1 ? va_arg(ap, unsigned long)
                                 : va_arg(ap, unsigned int);
            int base = *f == 'u' ? 10 : *f == 'o' ? 8 : 16;
            put_num(&o, v, base, *f == 'X', 0, width, prec, zero, left, 0, alt);
            break;
        }
        case 'p':
            put_num(&o, (unsigned long)va_arg(ap, void *), 16, 0, 0,
                    width, prec, zero, left, 0, 1);
            break;
        case 'c': {
            int pad = width > 1 ? width - 1 : 0;
            if (!left) while (pad-- > 0) put(&o, ' ');
            put(&o, (char)va_arg(ap, int));
            if (left) while (pad-- > 0) put(&o, ' ');
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            int n, pad;
            if (!s) s = "(null)";
            n = strlen(s);
            if (prec >= 0 && n > prec) n = prec;
            pad = width > n ? width - n : 0;
            if (!left) while (pad-- > 0) put(&o, ' ');
            while (n--) put(&o, *s++);
            if (left) while (pad-- > 0) put(&o, ' ');
            break;
        }
        case '%':
            put(&o, '%');
            break;
        case 0:
            f--;
            break;
        default:
            put(&o, '%'); put(&o, *f);
            break;
        }
    }
    if (size) *o.p = 0;
    return (int)o.total;
}

int snprintf(char *d, size_t n, const char *f, ...)
{
    va_list ap; int r;
    va_start(ap, f); r = vsnprintf(d, n, f, ap); va_end(ap);
    return r;
}

int sprintf(char *d, const char *f, ...)
{
    va_list ap; int r;
    va_start(ap, f); r = vsnprintf(d, 0x7fffffff, f, ap); va_end(ap);
    return r;
}

/* ----------------------------------------------------------------- varie */

/* Secondi dal 1970 (ora locale: libnfs lo usa solo per i timeout) */
long time(long *t)
{
    struct DateStamp ds;
    long s;
    DateStamp(&ds);
    s = ds.ds_Days * 86400 + ds.ds_Minute * 60 + ds.ds_Tick / TICKS_PER_SECOND
        + 252460800;
    if (t) *t = s;
    return s;
}

int getuid(void)  { return 0; }
int getgid(void)  { return 0; }
int getpid(void)  { return (int)(ULONG)FindTask(NULL); }
int pipe(int fd[2]) { (void)fd; errno = 78; return -1; }
int dup2(int a, int b) { (void)a; (void)b; errno = 78; return -1; }

void (*mini_assert_hook)(const char *file, int line, const char *expr);

void __assert_func(const char *file, int line, const char *func, const char *expr)
{
    (void)func;
    if (mini_assert_hook) mini_assert_hook(file, line, expr);
    /* Non possiamo uscire dall'handler: fermiamo il task senza crash */
    Wait(0);
}

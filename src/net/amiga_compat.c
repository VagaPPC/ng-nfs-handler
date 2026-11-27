/*
 * amiga_compat.c -- strato POSIX/socket per libnfs su AmigaOS 3.x
 *
 * NG-nfs-handler -- LGPL-2.1
 * Copyright (C) 2026 VagaPPC (Benedetto Lorello)
 */
#include "config.h"
#include "aros_compat.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <libraries/bsdsocket.h>
#include <utility/tagitem.h>

struct Library *SocketBase = NULL;
int h_errno = 0;

/* ------------------------------------------------------------------ init */

int amiga_socket_init(void)
{
    if (SocketBase) return 0;
    SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 3);
    if (!SocketBase) return -1;
    /* errno della libc = errno dello stack */
    SocketBaseTags(SBTM_SETVAL(SBTC_ERRNOLONGPTR), (ULONG)&errno,
                   SBTM_SETVAL(SBTC_HERRNOLONGPTR), (ULONG)&h_errno,
                   TAG_DONE);
    return 0;
}

int amiga_socket_ready(void)
{
    return SocketBase != NULL;
}

void amiga_socket_exit(void)
{
    if (SocketBase) {
        CloseLibrary(SocketBase);
        SocketBase = NULL;
    }
}

/* ------------------------------------------------------- risoluzione nomi */

int amiga_inet_pton(int af, const char *src, void *dst)
{
    struct in_addr a;
    const char *p;
    int dots = 0;

    if (af != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    /* inet_addr accetta anche forme abbreviate: richiedi a.b.c.d */
    for (p = src; *p; p++) {
        if (*p == '.') dots++;
        else if (*p < '0' || *p > '9') return 0;
    }
    if (dots != 3) return 0;
    a.s_addr = inet_addr((STRPTR)src);
    if (a.s_addr == INADDR_NONE && strcmp(src, "255.255.255.255") != 0)
        return 0;
    memcpy(dst, &a.s_addr, sizeof(a.s_addr));
    return 1;
}

const char *amiga_inet_ntop(int af, const void *src, char *dst, socklen_t size)
{
    const unsigned char *b = (const unsigned char *)src;
    char tmp[16];

    if (af != AF_INET) { errno = EAFNOSUPPORT; return NULL; }
    snprintf(tmp, sizeof(tmp), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    if (strlen(tmp) >= size) { errno = ENOSPC; return NULL; }
    strcpy(dst, tmp);
    return dst;
}

int amiga_getaddrinfo(const char *node, const char *service,
                      const struct addrinfo *hints, struct addrinfo **res)
{
    struct addrinfo    *ai;
    struct sockaddr_in *sin;
    struct in_addr      addr;

    if (!node) return EAI_NONAME;
    if (hints && hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET)
        return EAI_FAIL;

    if (amiga_inet_pton(AF_INET, node, &addr) != 1) {
        struct hostent *he;
        if (hints && (hints->ai_flags & AI_NUMERICHOST)) return EAI_NONAME;
        he = gethostbyname((STRPTR)node);
        if (!he || he->h_addrtype != AF_INET || !he->h_addr_list[0])
            return EAI_NONAME;
        memcpy(&addr, he->h_addr_list[0], sizeof(addr));
    }

    ai  = calloc(1, sizeof(*ai));
    sin = calloc(1, sizeof(*sin));
    if (!ai || !sin) { free(ai); free(sin); return EAI_MEMORY; }

    sin->sin_len    = sizeof(*sin);
    sin->sin_family = AF_INET;
    sin->sin_addr   = addr;
    sin->sin_port   = service ? htons(atoi(service)) : 0;

    ai->ai_family   = AF_INET;
    ai->ai_socktype = hints ? hints->ai_socktype : SOCK_STREAM;
    ai->ai_protocol = hints ? hints->ai_protocol : 0;
    ai->ai_addrlen  = sizeof(*sin);
    ai->ai_addr     = (struct sockaddr *)sin;
    *res = ai;
    return 0;
}

void amiga_freeaddrinfo(struct addrinfo *res)
{
    while (res) {
        struct addrinfo *next = res->ai_next;
        free(res->ai_addr);
        free(res->ai_canonname);
        free(res);
        res = next;
    }
}

int amiga_getnameinfo(const struct sockaddr *sa, socklen_t salen,
                      char *host, size_t hostlen,
                      char *serv, size_t servlen, int flags)
{
    const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;

    if (sa->sa_family != AF_INET) return EAI_FAIL;
    if (host && hostlen &&
        !amiga_inet_ntop(AF_INET, &sin->sin_addr, host, hostlen))
        return EAI_FAIL;
    if (serv && servlen)
        snprintf(serv, servlen, "%u", (unsigned)ntohs(sin->sin_port));
    return 0;
}

/* ------------------------------------------------------------------ poll */

int amiga_poll(struct pollfd *fds, unsigned int nfds, int timeout_ms)
{
    struct timeval tv, *tvp = NULL;
    fd_set rfds, wfds, efds;
    unsigned int i;
    int maxfd = -1, rc;

    FD_ZERO(&rfds); FD_ZERO(&wfds); FD_ZERO(&efds);
    for (i = 0; i < nfds; i++) {
        fds[i].revents = 0;
        if (fds[i].fd < 0) continue;
        if (fds[i].events & (POLLIN | POLLPRI)) FD_SET(fds[i].fd, &rfds);
        if (fds[i].events & POLLOUT)            FD_SET(fds[i].fd, &wfds);
        FD_SET(fds[i].fd, &efds);
        if (fds[i].fd > maxfd) maxfd = fds[i].fd;
    }
    if (timeout_ms >= 0) {
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        tvp = &tv;
    }

    rc = WaitSelect(maxfd + 1, &rfds, &wfds, &efds, tvp, NULL);
    if (rc <= 0) return rc;

    rc = 0;
    for (i = 0; i < nfds; i++) {
        int fd = fds[i].fd;
        if (fd < 0) continue;
        if ((fds[i].events & (POLLIN | POLLPRI)) && FD_ISSET(fd, &rfds))
            fds[i].revents |= POLLIN;
        if ((fds[i].events & POLLOUT) && FD_ISSET(fd, &wfds))
            fds[i].revents |= POLLOUT;
        if (FD_ISSET(fd, &efds))
            fds[i].revents |= POLLERR;
        if (fds[i].revents) rc++;
    }
    return rc;
}

/* ------------------------------------------------------- readv / writev */

ssize_t amiga_readv(int fd, const struct iovec *iov, int iovcnt)
{
    ssize_t total = 0;
    int i;

    for (i = 0; i < iovcnt; i++) {
        LONG n;
        if (!iov[i].iov_len) continue;
        n = recv(fd, iov[i].iov_base, iov[i].iov_len, 0);
        if (n < 0) return total ? total : -1;
        total += n;
        if (n == 0 || (size_t)n < iov[i].iov_len) break;
    }
    return total;
}

ssize_t amiga_writev(int fd, const struct iovec *iov, int iovcnt)
{
    ssize_t total = 0, len = 0;
    int i;

    for (i = 0; i < iovcnt; i++) len += iov[i].iov_len;

    /* Pacchetti piccoli: un solo send() (evita segmenti TCP spezzati).
     * Buffer statico, non sullo stack: libnfs gira in un solo task. */
    if (len <= 4096) {
        static char buf[4096];
        char *p = buf;
        for (i = 0; i < iovcnt; i++) {
            memcpy(p, iov[i].iov_base, iov[i].iov_len);
            p += iov[i].iov_len;
        }
        return send(fd, buf, len, 0);
    }

    for (i = 0; i < iovcnt; i++) {
        LONG n;
        if (!iov[i].iov_len) continue;
        n = send(fd, iov[i].iov_base, iov[i].iov_len, 0);
        if (n < 0) return total ? total : -1;
        total += n;
        if ((size_t)n < iov[i].iov_len) break;
    }
    return total;
}

/* ------------------------------------------------------------- fcntl */

#include <stdarg.h>

#define MAX_TRACKED_FD 256
static unsigned char nonblock_fd[MAX_TRACKED_FD / 8];

int amiga_fcntl(int fd, int cmd, ...)
{
    va_list ap;
    long arg = 0;
    LONG on;

    va_start(ap, cmd);
    if (cmd == F_SETFL || cmd == F_SETFD) arg = va_arg(ap, long);
    va_end(ap);

    if (fd < 0 || fd >= MAX_TRACKED_FD) { errno = EBADF; return -1; }

    switch (cmd) {
    case F_GETFL:
        return (nonblock_fd[fd >> 3] & (1 << (fd & 7))) ? O_NONBLOCK : 0;
    case F_SETFL:
        on = (arg & O_NONBLOCK) ? 1 : 0;
        if (IoctlSocket(fd, FIONBIO, (char *)&on) < 0) return -1;
        if (on) nonblock_fd[fd >> 3] |=  (1 << (fd & 7));
        else    nonblock_fd[fd >> 3] &= ~(1 << (fd & 7));
        return 0;
    default:
        errno = ENOSYS;
        return -1;
    }
}

/* ------------------------------------------------------------ varie */

char *amiga_strndup(const char *s, size_t n)
{
    size_t l = 0;
    char *d;
    while (l < n && s[l]) l++;
    d = malloc(l + 1);
    if (!d) return NULL;
    memcpy(d, s, l);
    d[l] = 0;
    return d;
}

int amiga_nanosleep(const struct timespec *req, struct timespec *rem)
{
    LONG ticks = req->tv_sec * TICKS_PER_SECOND +
                 req->tv_nsec / (1000000000 / TICKS_PER_SECOND);
    if (ticks < 1) ticks = 1;
    Delay(ticks);
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    return 0;
}

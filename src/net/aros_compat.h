/*
 * aros_compat.h -- sostituisce aros/aros_compat.h di libnfs per AmigaOS 3.x
 * (bsdsocket.library: Roadshow, AmiTCP, Miami, Genesis, bsdsocket_emu UAE).
 *
 * libnfs viene compilata con -DAROS e questa cartella in testa al path degli
 * include: cosi' i sorgenti di libnfs restano INTATTI.
 *
 * NG-nfs-handler -- LGPL-2.1
 * Copyright (C) 2026 VagaPPC (Benedetto Lorello)
 */
#ifndef AROS_COMPAT_H
#define AROS_COMPAT_H

#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/bsdsocket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>

extern struct Library *SocketBase;

/* Apre bsdsocket.library per il task CORRENTE (SocketBase e' per-processo)
 * e collega errno/h_errno. Ritorna 0 se ok, -1 se manca lo stack TCP. */
int  amiga_socket_init(void);
void amiga_socket_exit(void);
int  amiga_socket_ready(void);    /* SocketBase aperta? */

/* --- socket API --- */
#define ioctl        IoctlSocket
#define close        CloseSocket

/* --- risoluzione nomi (gli stack OS3 non hanno getaddrinfo) --- */
#ifndef EAI_FAIL
#define EAI_FAIL     4
#define EAI_NONAME   8
#define EAI_MEMORY   6
#endif
#ifndef NI_MAXHOST
#define NI_MAXHOST   1025
#define NI_MAXSERV   32
#define NI_NUMERICHOST 1
#define NI_NUMERICSERV 2
#endif
#ifndef AI_PASSIVE
struct addrinfo {
    int              ai_flags;
    int              ai_family;
    int              ai_socktype;
    int              ai_protocol;
    socklen_t        ai_addrlen;
    struct sockaddr *ai_addr;
    char            *ai_canonname;
    struct addrinfo *ai_next;
};
#define AI_PASSIVE     1
#define AI_NUMERICHOST 4
#endif

/* L'NDK 3.2 mappa queste funzioni sulle chiamate di Roadshow 4.x, assenti
 * in AmiTCP/Miami/Genesis e nel bsdsocket_emu di UAE: usiamo le nostre. */
#undef getaddrinfo
#undef freeaddrinfo
#undef getnameinfo
#undef inet_pton
#undef inet_ntop
#define getaddrinfo  amiga_getaddrinfo
#define freeaddrinfo amiga_freeaddrinfo
#define getnameinfo  amiga_getnameinfo
#define inet_pton    amiga_inet_pton
#define inet_ntop    amiga_inet_ntop

int  amiga_getaddrinfo(const char *node, const char *service,
                       const struct addrinfo *hints, struct addrinfo **res);
void amiga_freeaddrinfo(struct addrinfo *res);
int  amiga_getnameinfo(const struct sockaddr *sa, socklen_t salen,
                       char *host, size_t hostlen,
                       char *serv, size_t servlen, int flags);
int  amiga_inet_pton(int af, const char *src, void *dst);
const char *amiga_inet_ntop(int af, const void *src, char *dst, socklen_t size);

/* --- poll() su WaitSelect() --- */
#define POLLIN      0x0001
#define POLLPRI     0x0002
#define POLLOUT     0x0004
#define POLLERR     0x0008
#define POLLHUP     0x0010
#define POLLNVAL    0x0020

struct pollfd {
    int   fd;
    short events;
    short revents;
};
#define poll(f, n, t)  amiga_poll(f, n, t)
int amiga_poll(struct pollfd *fds, unsigned int nfds, int timeout_ms);

/* --- statvfs (usata solo da nfs_statvfs lato API) --- */
struct statvfs {
    unsigned long f_bsize, f_frsize;
    unsigned long long f_blocks, f_bfree, f_bavail;
    unsigned long long f_files, f_ffree, f_favail;
    unsigned long f_fsid, f_flag, f_namemax;
};

/* --- fcntl() sui socket ---
 * libnfs imposta O_NONBLOCK con fcntl(), ma quello di libnix agisce sui file
 * della libc, non sui socket bsdsocket: il socket restava BLOCCANTE e recv()
 * si bloccava per sempre dopo la prima risposta. Qui O_NONBLOCK diventa
 * IoctlSocket(FIONBIO). F_GETFD/F_SETFD falliscono: il "parcheggio" del fd
 * con pipe()+dup2() durante la riconnessione non ha senso su Amiga. */
#include <fcntl.h>
#undef fcntl
#define fcntl amiga_fcntl
int amiga_fcntl(int fd, int cmd, ...);

/* --- costanti assenti negli header NDK/libnix --- */
#ifndef SOL_TCP
#define SOL_TCP      IPPROTO_TCP
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW   0x00100000
#endif
/* La sockaddr_in6 dell'NDK non ha sin6_len. Gli stack OS3 non supportano
 * IPv6: il codice IPv6 di libnfs viene compilato ma mai eseguito, quindi
 * rimappiamo il campo su uno innocuo. */
#define sin6_len     sin6_scope_id

/* --- varie POSIX assenti --- */
struct utimbuf { long actime; long modtime; };
#ifndef major
#define major(d) (((d) >> 8) & 0xff)
#define minor(d) ((d) & 0xff)
#endif

/* --- I/O vettoriale (bsdsocket non ha readv/writev) --- */
#include <sys/uio.h>
#define readv(fd, iov, n)   amiga_readv(fd, iov, n)
#define writev(fd, iov, n)  amiga_writev(fd, iov, n)
ssize_t amiga_readv(int fd, const struct iovec *iov, int iovcnt);
ssize_t amiga_writev(int fd, const struct iovec *iov, int iovcnt);

/* --- stringhe/tempo assenti in libnix --- */
#include <time.h>
#define strndup(s, n)       amiga_strndup(s, n)
char *amiga_strndup(const char *s, size_t n);
#define nanosleep(ts, rem)  amiga_nanosleep(ts, rem)
int amiga_nanosleep(const struct timespec *req, struct timespec *rem);

#endif /* AROS_COMPAT_H */

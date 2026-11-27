/*
 * aros_compat.h -- versione AmigaOS 4.1 / clib4
 *
 * libnfs viene compilata con -DAROS (che serve solo a includere questo file
 * e a togliere NFSv4.2) e src/platform/os4 in testa al path degli include,
 * al posto di src/net. clib4 e' quasi POSIX: socket come file descriptor,
 * poll, readv/writev, getaddrinfo, fcntl(O_NONBLOCK) sui socket. Qui resta
 * solo quello che serve al nucleo dell'handler.
 *
 * NG-nfs-handler -- LGPL-2.1
 * Copyright (C) 2026 VagaPPC (Benedetto Lorello)
 */
#ifndef AROS_COMPAT_H
#define AROS_COMPAT_H

#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <unistd.h>
#include <poll.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <time.h>
#include <utime.h>

/* clib4 definisce le opzioni TCP_KEEP* di lwIP, ma Roadshow (lo stack di
 * OS4) le rifiuta con ENOPROTOOPT e libnfs annulla la connessione ("Cannot
 * enable keepalive"). Senza le macro libnfs usa solo SO_KEEPALIVE. */
#undef TCP_KEEPALIVE
#undef TCP_KEEPIDLE
#undef TCP_KEEPINTVL
#undef TCP_KEEPCNT

#ifndef SOL_TCP
#define SOL_TCP IPPROTO_TCP
#endif

/* readv/writev di clib4 passano dal livello file (lock stdio, un'azione
 * per ogni iovec) e con le letture grandi di libnfs (READ3 da 16 KB
 * direttamente nei buffer del chiamante) il flusso RPC si desincronizzava:
 * la prima Copy restava appesa. Al loro posto recv()/send(), come su OS3. */
#undef readv
#undef writev
#define readv(fd, iov, n)   amiga_readv(fd, iov, n)
#define writev(fd, iov, n)  amiga_writev(fd, iov, n)
ssize_t amiga_readv(int fd, const struct iovec *iov, int iovcnt);
ssize_t amiga_writev(int fd, const struct iovec *iov, int iovcnt);

/* Con clib4 lo stack TCP lo apre la libreria per ogni processo: nel nucleo
 * resta solo il controllo che bsdsocket.library ci sia. */
int  amiga_socket_init(void);
void amiga_socket_exit(void);
int  amiga_socket_ready(void);

#endif /* AROS_COMPAT_H */

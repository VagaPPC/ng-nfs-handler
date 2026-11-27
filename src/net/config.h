/* config.h per libnfs su AmigaOS 3.x (m68k, bebbo-gcc + libnix + NDK 3.2)
 * NG-nfs-handler -- LGPL-2.1
 * Copyright (C) 2026 VagaPPC (Benedetto Lorello) */
#ifndef NG_LIBNFS_CONFIG_H
#define NG_LIBNFS_CONFIG_H

#define HAVE_UNISTD_H        1
#define HAVE_STRINGS_H       1
#define HAVE_STRING_H        1
#define HAVE_STDLIB_H        1
#define HAVE_STDINT_H        1
#define HAVE_INTTYPES_H      1
#define HAVE_SYS_TYPES_H     1
#define HAVE_SYS_STAT_H      1
#define HAVE_SYS_TIME_H      1
#define HAVE_SYS_SOCKET_H    1
#define HAVE_SYS_IOCTL_H     1
#define HAVE_NETINET_IN_H    1
#define HAVE_NETINET_TCP_H   1
#define HAVE_ARPA_INET_H     1
#define HAVE_NETDB_H         1
#define HAVE_SOCKADDR_LEN    1

/* NON definiti di proposito: HAVE_MULTITHREADING, HAVE_LIBKRB5, HAVE_TLS,
 * HAVE_RDMA, HAVE_NFS4_2, HAVE_POLL_H (usiamo il nostro poll su WaitSelect) */

/* evita i prototipi di arpa/inet.h che collidono con le inline di bsdsocket */
#define __NO_NET_API         1

#ifndef _U_
#define _U_
#endif

#endif

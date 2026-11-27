/* config.h per libnfs su AmigaOS 4.1 (PPC, adtools gcc + clib4)
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
#define HAVE_SYS_UIO_H       1
#define HAVE_SYS_STATVFS_H   1
#define HAVE_SYS_SYSMACROS_H 1
#define HAVE_NETINET_IN_H    1
#define HAVE_NETINET_TCP_H   1
#define HAVE_ARPA_INET_H     1
#define HAVE_NETDB_H         1
#define HAVE_POLL_H          1
#define HAVE_UTIME_H         1
#define HAVE_SOCKADDR_LEN    1

/* NON definiti di proposito: HAVE_MULTITHREADING, HAVE_LIBKRB5, HAVE_TLS,
 * HAVE_RDMA, HAVE_NFS4_2 */

/* La sockaddr_storage di clib4 2.5 ha "short ss_family", ma le sockaddr
 * hanno sa_len + sa_family da un byte: libnfs leggeva len e famiglia
 * insieme (AF_INET con len 16 = 0x1002 = 4098, "Can not handle
 * AF_FAMILY:4098"). Si nasconde quella di clib4 sotto un altro nome e libnfs
 * definisce la sua (con ss_len/ss_family, HAVE_SOCKADDR_LEN). */
#define sockaddr_storage clib4_sockaddr_storage
#include <sys/socket.h>
#include <netinet/in.h>
#undef sockaddr_storage
#undef _SS_ALIGNSIZE
#undef _SS_PAD1SIZE
#undef _SS_PAD2SIZE

#ifndef _U_
#define _U_
#endif

#endif

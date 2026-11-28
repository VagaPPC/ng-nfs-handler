# NG-nfs-handler

NFSv3 client filesystem handler for AmigaOS. It mounts NFS exports (e.g. from
a Linux server) as a normal AmigaDOS volume: put `nfs-handler` in `L:`, mount
it with a mountlist, unmount it with `NFSDismount` (AmigaOS 3.x) or
`C:Dismount` (AmigaOS 4.1).

**Status: 1.0.** Works on AmigaOS 3.x (68020+) and AmigaOS 4.1. Verified on a
Vampire V4 with Roadshow, in Amiberry and on an AmigaOne X5000 (AmigaOS 4.1 FE,
clib4). Ports to MorphOS and AROS are planned.

## Features

- NFSv3 over TCP, based on [libnfs](https://github.com/sahlberg/libnfs) 8.1.0
  (vendored, unmodified)
- read/write: files, directories, rename, delete, protection, dates, links
- case-insensitive names (optional), Latin-1 ⇄ UTF-8 name conversion with
  reversible `%XX` escapes
- DOS notification (StartNotify) by polling
- lazy mount and automatic reconnection after a server restart
- clean unmount (`NFSDismount` on OS3, `C:Dismount` on OS4, also with files
  or locks still open), self-termination after `Assign ... DISMOUNT`
- AmigaOS 4.1: 64-bit packets and ExamineData (files larger than 4 GB),
  options also as tooltypes in the icon of the mountlist
- OS3: no startup code and no libnix inside the handler (own mini libc on an
  exec memory pool); OS4: clib4

User documentation: [doc/nfs-handler.txt](doc/nfs-handler.txt) (English),
[doc/nfs-handler.it.txt](doc/nfs-handler.it.txt) (Italiano).

## Building

Cross-compiled on Linux. AmigaOS 3.x: [bebbo's amiga-gcc](https://github.com/bebbo/amiga-gcc)
(m68k-amigaos-gcc 6.5), set `M68K_PREFIX` if it is not in the default location.
AmigaOS 4.1: adtools ppc-amigaos-gcc with the clib4 SDK, set `OS4_SDK` if needed.

    make                       # build/os3/: nfs-handler, NFSDismount, nfsls, NotifyTest
    make PLATFORM=os4 handler  # build/os4/nfs-handler
    make dist                  # build/NG-nfs-handler.lha: OS3 + OS4 binaries, docs, sources

## Layout

| Path | Content |
|------|---------|
| `src/dos/` | the handler: DOS packet loop, locks, files, notify, reconnection |
| `src/platform/` | per-system layer: startup, libc, timer, DosList (`os3/`, `os4/`) |
| `src/net/` | `config.h` and `aros_compat.h`: libnfs on bsdsocket.library (poll, getaddrinfo, fcntl, readv/writev) |
| `src/libc/` | minimal libc used by the handler |
| `libnfs/` | libnfs 8.1.0, unmodified (see `libnfs/VENDOR.txt`) |
| `tools/` | `NFSDismount`, `nfsls`, `NotifyTest`, `pktlog-handler` (packet logger used to study DOS behaviour) |
| `test/` | AmigaDOS test scripts and mountlists |
| `dist/` | release files: Aminet readme, `OS3/` and `OS4/` example mountlists with icons |

## License

GNU Lesser General Public License 2.1 (see `COPYING`). libnfs is LGPL 2.1 /
BSD (see `libnfs/COPYING`); clib4 (OS4) is BSD 3-Clause.

Copyright (C) 2026 VagaPPC (Benedetto Lorello)

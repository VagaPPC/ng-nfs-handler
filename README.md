# NG-nfs-handler

NFSv3 client filesystem handler for AmigaOS. It mounts NFS exports (e.g. from
a Linux server) as a normal AmigaDOS volume: put `nfs-handler` in `L:`, mount
it with a mountlist, unmount it with `NFSDismount`.

**Status: 0.9 beta.** Works on AmigaOS 3.x (68020+). Verified on a Vampire V4
with Roadshow and in Amiberry. Ports to AROS, AmigaOS 4 and MorphOS are planned.

## Features

- NFSv3 over TCP, based on [libnfs](https://github.com/sahlberg/libnfs) 8.1.0
  (vendored, unmodified)
- read/write: files, directories, rename, delete, protection, dates, links
- case-insensitive names (optional), Latin-1 ⇄ UTF-8 name conversion with
  reversible `%XX` escapes
- DOS notification (StartNotify) by polling
- lazy mount and automatic reconnection after a server restart
- clean unmount (`NFSDismount`), self-termination after `Assign ... DISMOUNT`
- no startup code and no libnix inside the handler (own mini libc on an exec
  memory pool)

User documentation: [doc/nfs-handler.txt](doc/nfs-handler.txt) (English),
[doc/nfs-handler.it.txt](doc/nfs-handler.it.txt) (Italiano).

## Building

Cross-compiled on Linux with [bebbo's amiga-gcc](https://github.com/bebbo/amiga-gcc)
(m68k-amigaos-gcc 6.5). Set `M68K_PREFIX` if the toolchain is not in the
default location used by the Makefile.

    make                 # build/os3/: nfs-handler, NFSDismount, nfsls, NotifyTest
    make dist            # build/NG-nfs-handler.lha (binaries, docs, sources)

## Layout

| Path | Content |
|------|---------|
| `src/dos/` | the handler: DOS packet loop, locks, files, notify, reconnection |
| `src/net/` | `config.h` and `aros_compat.h`: libnfs on bsdsocket.library (poll, getaddrinfo, fcntl, readv/writev) |
| `src/libc/` | minimal libc used by the handler |
| `libnfs/` | libnfs 8.1.0, unmodified (see `libnfs/VENDOR.txt`) |
| `tools/` | `NFSDismount`, `nfsls`, `NotifyTest`, `pktlog-handler` (packet logger used to study DOS behaviour) |
| `test/` | AmigaDOS test scripts and mountlists |
| `dist/` | release files (Aminet readme, example mountlist) |

## License

GNU Lesser General Public License 2.1 (see `COPYING`). libnfs is LGPL 2.1 /
BSD (see `libnfs/COPYING`).

Copyright (C) 2026 @AUTHOR@

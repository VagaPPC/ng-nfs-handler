/*
 * nfsls -- M1 di NG-nfs-handler: test di libnfs su AmigaOS 3.x
 *
 *   nfsls SERVER/A,EXPORT/A,PATH,UID/N,GID/N,WRITETEST/S
 *
 *   nfsls 192.168.1.100 /export/amiga
 *   nfsls nfs-server /export/amiga PATH=sub UID=1000 GID=1000 WRITETEST
 *
 * Usa l'API sincrona di libnfs (NFSv3 su TCP).
 *
 * Licenza: LGPL-2.1
 * Copyright (C) 2026 VagaPPC (Benedetto Lorello)
 */
#include "config.h"
#include "aros_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <nfsc/libnfs.h>

static const char verstag[] __attribute__((used)) =
    "$VER: nfsls 1.0 (28.11.2026)";

/* libnix: stack minimo del processo */
unsigned long __stack = 65536;

static const char *type_char(uint32_t mode)
{
    switch (mode & S_IFMT) {
    case S_IFDIR: return "d";
    case S_IFLNK: return "l";
    case S_IFREG: return "-";
    default:      return "?";
    }
}

static int write_test(struct nfs_context *nfs, const char *dir)
{
    static const char payload[] =
        "NG-nfs-handler M1: prova di scrittura da AmigaOS 3.x\n";
    char path[512], buf[128];
    struct nfsfh *fh;
    int n;

    snprintf(path, sizeof(path), "%s/.nfsls-writetest", dir);
    printf("\nWRITETEST su %s\n", path);

    if (nfs_creat(nfs, path, 0644, &fh)) {
        printf("  creat: ERRORE %s\n", nfs_get_error(nfs));
        return 0;
    }
    n = nfs_write(nfs, fh, payload, strlen(payload));
    nfs_close(nfs, fh);
    printf("  write: %d byte\n", n);
    if (n != (int)strlen(payload)) return 0;

    if (nfs_open(nfs, path, O_RDONLY, &fh)) {
        printf("  open: ERRORE %s\n", nfs_get_error(nfs));
        return 0;
    }
    memset(buf, 0, sizeof(buf));
    n = nfs_read(nfs, fh, buf, sizeof(buf) - 1);
    nfs_close(nfs, fh);
    printf("  read : %d byte, contenuto %s\n", n,
           (n == (int)strlen(payload) && !memcmp(buf, payload, n))
           ? "IDENTICO" : "DIVERSO!");

    if (nfs_unlink(nfs, path)) {
        printf("  unlink: ERRORE %s\n", nfs_get_error(nfs));
        return 0;
    }
    printf("  unlink: ok\n");
    return 1;
}

int main(void)
{
    LONG args[6] = { 0, 0, 0, 0, 0, 0 };
    struct RDArgs *rda;
    struct nfs_context *nfs = NULL;
    struct nfsdir *dir;
    struct nfsdirent *e;
    const char *path;
    int rc = RETURN_FAIL, count = 0;

    rda = ReadArgs((STRPTR)"SERVER/A,EXPORT/A,PATH,UID/N,GID/N,WRITETEST/S",
                   args, NULL);
    if (!rda) {
        PrintFault(IoErr(), (STRPTR)"nfsls");
        return RETURN_FAIL;
    }
    path = args[2] ? (const char *)args[2] : "/";

    if (amiga_socket_init()) {
        printf("nfsls: bsdsocket.library non disponibile (stack TCP/IP attivo?)\n");
        goto out;
    }

    nfs = nfs_init_context();
    if (!nfs) {
        printf("nfsls: nfs_init_context fallita\n");
        goto out;
    }
    nfs_set_version(nfs, 3);
    nfs_set_uid(nfs, args[3] ? *(LONG *)args[3] : 65534);
    nfs_set_gid(nfs, args[4] ? *(LONG *)args[4] : 65534);
    nfs_set_timeout(nfs, 10000);

    printf("mount %s:%s (NFSv3/TCP) ...\n",
           (char *)args[0], (char *)args[1]);
    if (nfs_mount(nfs, (const char *)args[0], (const char *)args[1])) {
        printf("nfsls: mount fallito: %s\n", nfs_get_error(nfs));
        goto out;
    }

    if (nfs_opendir(nfs, path, &dir)) {
        printf("nfsls: opendir %s: %s\n", path, nfs_get_error(nfs));
        goto out;
    }
    printf("\nContenuto di %s:\n", path);
    while ((e = nfs_readdir(nfs, dir))) {
        printf("  %s%04o %5u %5u %10llu  %s\n",
               type_char(e->mode), (unsigned)(e->mode & 07777),
               (unsigned)e->uid, (unsigned)e->gid,
               (unsigned long long)e->size, e->name);
        count++;
    }
    nfs_closedir(nfs, dir);
    printf("%d voci\n", count);
    rc = RETURN_OK;

    if (args[5] && !write_test(nfs, strcmp(path, "/") ? path : ""))
        rc = RETURN_WARN;

out:
    if (nfs) nfs_destroy_context(nfs);
    amiga_socket_exit();
    FreeArgs(rda);
    return rc;
}

/*
 * nfs-handler -- handler DOS per volumi NFS (NG-nfs-handler)
 *
 * M2: versione READ-ONLY per AmigaOS 3.x.
 *
 * Mountlist (DEVS:DOSDrivers/NFS0):
 *   Handler   = L:nfs-handler
 *   StackSize = 32768
 *   Priority  = 5
 *   GlobVec   = -1
 *   Activate  = 1
 *   Startup   = "SERVER=nfs-server EXPORT=/export/amiga VOLUME=NAS UID=1000 GID=1000"
 *
 * Opzioni Startup (ReadArgs):
 *   SERVER/A, EXPORT/A, VOLUME/K, UID/N, GID/N, TIMEOUT/N (s), RSIZE/N,
 *   CHARSET/K (LATIN1|RAW), LOG/K (file di log, per debug)
 *
 * Architettura: un solo processo; loop su WaitSelect() (socket NFS +
 * segnali porta pacchetti/timer); ogni pacchetto avvia al piu' una chiamata
 * asincrona di libnfs alla volta, la cui callback risponde al pacchetto.
 *
 * Licenza: LGPL-2.1
 * Copyright (C) 2026 VagaPPC (Benedetto Lorello)
 */

#include "config.h"
#include "aros_compat.h"

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <dos/rdargs.h>
#include <dos/notify.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdarg.h>
#include <stdint.h>
#include <errno.h>
#include <nfsc/libnfs.h>
#include <nfsc/libnfs-raw.h>

#include "minilibc.h"
#include "platform.h"

#ifdef __amigaos4__
#include <dos/obsolete.h>       /* dn_Task, dol_Task, fl_Task, ACTION_DIE */
/* OS4: struct TimeRequest al posto di timerequest (quella vecchia si
 * scontra con la timeval di clib4); fib_EntryType si chiama fib_Obsolete */
typedef struct TimeRequest timereq_t;
#define TR_IO(t)        ((t)->Request)
#define TR_SECS(t)      ((t)->Time.Seconds)
#define TR_MICRO(t)     ((t)->Time.Microseconds)
#define fib_EntryType   fib_Obsolete
#define LOG_SEEK_END(fh) ChangeFilePosition(fh, 0, OFFSET_END)
#else
typedef struct timerequest timereq_t;
#define TR_IO(t)        ((t)->tr_node)
#define TR_SECS(t)      ((t)->tr_time.tv_secs)
#define TR_MICRO(t)     ((t)->tr_time.tv_micro)
#define LOG_SEEK_END(fh) Seek(fh, 0, OFFSET_END)
#endif
#ifndef ACTION_SHUTDOWN
#define ACTION_SHUTDOWN 3000    /* OS4 (V51+): sostituisce ACTION_DIE */
#endif

#define HANDLER_VERSION "nfs-handler 0.9 (27.11.2026) beta"
static const char verstag[] __attribute__((used)) = "$VER: " HANDLER_VERSION;

/* tipi NFSv3 (libnfs-raw-nfs.h) */
#define NF3REG 1
#define NF3DIR 2
#define NF3LNK 5

#define AMIGA_EPOCH_OFFSET 252460800L   /* 1978-01-01 - 1970-01-01 */
#define HEARTBEAT_SECS     2
#define ORPHAN_CHECK_EVERY 5            /* heartbeat */

/* ------------------------------------------------------------------------ */
/* Strutture                                                                */
/* ------------------------------------------------------------------------ */

struct NLock {
    struct FileLock    fl;          /* DEVE essere il primo campo */
    struct NLock      *next;
    char              *path;        /* percorso sul server (UTF-8), "/" = root */
    struct nfs_stat_64 st;
    struct nfsdir     *dir;         /* listing per ExNext (NULL = da leggere) */
    char               owner[24];   /* task che l'ha creato (diagnosi) */
};

struct NFile {
    struct NFile      *next;
    char              *path;
    struct nfsfh      *fh;
    struct FileHandle *pubfh;       /* FileHandle del DOS (ACTION_COLLECT) */
    BOOL               dirty;       /* scritto e non ancora committato */
    BOOL               modified;    /* scritto dall'apertura (per i notify) */
    int                oflags;      /* per riaprire dopo una riconnessione */
    uint64_t           pos;
    struct nfs_stat_64 st;
};

enum { MS_IDLE, MS_MOUNTING, MS_READY };

/* Contesto di una richiesta asincrona */
struct Req {
    struct DosPacket *pkt;
    char             *path;
    struct NLock     *lock;
    struct NFile     *file;
    struct FileHandle *fh;
    UBYTE            *buf;
    LONG              want, done;
    BPTR              fib;          /* FileInfoBlock / InfoData */
    BPTR              freelock;     /* FH_FROM_LOCK: lock da liberare */
    struct nfs_stat_64 st;          /* attributi letti prima dell'open */
    struct nfsdirent *ent;          /* ExNext su symlink: voce originale */
    /* ricerca case-insensitive */
    nfs_cb            ci_cb;        /* callback finale (come una stat) */
    char             *ci_prefix;    /* parte gia' risolta (nomi reali) */
    char             *ci_cand;      /* candidato in prova */
    const char       *ci_rest;      /* componenti ancora da risolvere */
    int               ci_complen;   /* lunghezza componente corrente */
    BOOL              ci_scanned;   /* componente gia' cercato nel listing */
    char             *ci_parent;    /* ENOENT sull'ultimo componente: genitore reale */
    /* scrittura */
    int               omode;        /* OPEN_OLD / OPEN_NEW / OPEN_RW */
    int               oflags;       /* flag open() in uso */
    char             *src;          /* RENAME: sorgente reale */
    char             *dst;          /* RENAME: destinazione (percorso DOS risolto) */
    char             *dstname;      /* RENAME: nome richiesto (ultimo componente) */
    LONG              ival;         /* argomento intero (protezione, contatori) */
    int64_t           size64;       /* SET_FILE_SIZE: nuova dimensione */
    struct DateStamp  ds;           /* SET_DATE */
    BOOL              st_valid;     /* r->st contiene gli attributi di ci_prefix */
    /* I/O parallelo */
    LONG              issued;       /* byte gia' richiesti */
    LONG              inflight;     /* blocchi in volo */
    LONG              eof_at;       /* <0: nessun EOF/short; altrimenti byte validi */
    int               io_err;       /* primo errore (errno negativo) */
    BOOL              is_write;
    BOOL              is64;         /* risposta DosPacket64 (OS4) */
    APTR              ctx;          /* EXAMINEDATA_DIR: contesto del DOS */
    struct nfsdir    *xdir;         /* EXAMINEDATA_DIR: listing in corso */
    /* misure (solo con LOG=) */
    ULONG             m_rpc0;       /* contatore RPC alla partenza */
    uint64_t          m_t0;         /* EClock alla partenza */
};

enum { OPEN_OLD, OPEN_NEW, OPEN_RW };

#ifndef O_SYNC
#define O_SYNC 0x0080
#endif
#define S_IFREG_ 0100000

/* ------------------------------------------------------------------------ */
/* Stato globale                                                            */
/* ------------------------------------------------------------------------ */

static struct Process     *me;
static struct MsgPort     *pktport;
static struct DeviceNode  *devnode;
static struct DosList     *volnode;
static char                devname[64];

static struct nfs_context *nfs;
static int                 mstate = MS_IDLE;
static BOOL                nfs_reset;       /* distruggere il contesto nel loop */
static ULONG               ticks;           /* battiti del timer (HEARTBEAT_SECS) */
static ULONG               mount_tick;      /* battito di inizio del mount in corso */
static struct List         pending;         /* pacchetti in attesa del mount */

static struct NLock       *locks;
static LONG                numlocks;
static struct NFile       *files;
static LONG                numfiles;
static LONG                gmt_offset_secs; /* da locale.library */
static ULONG               rpc_calls;       /* RPC inviate (statistiche) */
static ULONG               eclock_freq;

/* callback statistiche di libnfs: conta le chiamate inviate */
static void cb_rpc_stats(struct rpc_context *rpc, struct rpc_pdu_stats *d, void *priv)
{
    (void)rpc; (void)priv;
    if (d->direction == 0 /* CALL */) rpc_calls++;
}

static LONG                notify_msgs;     /* NotifyMessage non ancora restituiti */
static LONG                active_reqs;     /* richieste asincrone in corso */
static void notify_touch(const char *path);

static BOOL                devnode_seen = TRUE;
static BOOL                orphan_exit;

/* opzioni */
static char  *opt_server, *opt_export, *opt_volume, *opt_log;
static LONG   opt_uid = 65534, opt_gid = 65534, opt_timeout = 10, opt_rsize = 16384;
static BOOL   opt_raw_charset;
static BOOL   opt_case_sensitive;
static BOOL   opt_sync, opt_readonly;
static LONG   opt_wsize = 16384, opt_umask = 022, opt_par = 4, opt_notifypoll = 4;

/* ------------------------------------------------------------------------ */
/* Log (solo se LOG= e' impostato)                                          */
/* ------------------------------------------------------------------------ */

static void plog(const char *f, ...)
{
    char line[512];
    struct DateStamp ds;
    BPTR fh;
    va_list ap;
    int n;
    LONG secs;

    if (!opt_log) return;
    DateStamp(&ds);
    secs = ds.ds_Minute * 60 + ds.ds_Tick / TICKS_PER_SECOND;
    n = snprintf(line, sizeof(line), "%02ld:%02ld:%02ld.%02ld ",
                 (long)(secs / 3600) % 24, (long)(secs / 60) % 60, (long)secs % 60,
                 (long)(ds.ds_Tick % TICKS_PER_SECOND) * 2);
    va_start(ap, f);
    n += vsnprintf(line + n, sizeof(line) - n - 1, f, ap);
    va_end(ap);
    if (n > (int)sizeof(line) - 2) n = sizeof(line) - 2;
    line[n++] = '\n';

    fh = Open((STRPTR)opt_log, MODE_READWRITE);
    if (!fh) return;
    LOG_SEEK_END(fh);
    Write(fh, line, n);
    Close(fh);
}

static void assert_hook(const char *file, int line, const char *expr)
{
    plog("!! ASSERT FALLITA %s:%d: %s -- handler fermato", file, line, expr);
}

/* ------------------------------------------------------------------------ */
/* Utility                                                                  */
/* ------------------------------------------------------------------------ */

static void bstr2c(BSTR b, char *out, ULONG max)
{
    UBYTE *p = (UBYTE *)BADDR(b);
    ULONG n, i;
    if (!p) { out[0] = 0; return; }
    n = p[0];
    if (n > max - 1) n = max - 1;
    for (i = 0; i < n; i++) out[i] = p[i + 1];
    out[n] = 0;
}

static void reply(struct DosPacket *pkt, LONG res1, LONG res2)
{
    if (opt_log) plog("-> %4ld res1=%ld res2=%ld", (long)pkt->dp_Type, (long)res1, (long)res2);
    ReplyPkt(pkt, res1, res2);
}

/* Log del pacchetto in arrivo (solo con LOG=): tipo, argomenti, nome */
static void log_packet(struct DosPacket *pkt)
{
    char name[256];
    BSTR b = 0;
    if (!opt_log) return;
    switch (pkt->dp_Type) {
    case ACTION_LOCATE_OBJECT: case ACTION_DELETE_OBJECT: case ACTION_CREATE_DIR:
    case ACTION_RENAME_OBJECT: b = (BSTR)pkt->dp_Arg2; break;
    case ACTION_FINDINPUT: case ACTION_FINDOUTPUT: case ACTION_FINDUPDATE:
    case ACTION_SET_PROTECT: case ACTION_SET_COMMENT: case ACTION_SET_DATE:
        b = (BSTR)pkt->dp_Arg3; break;
    }
    name[0] = 0;
    if (b) bstr2c(b, name, sizeof(name));
    {
        /* chi manda il pacchetto (porta di risposta -> task) */
        const char *who = "?";
        struct MsgPort *rp = pkt->dp_Port;
        if (rp && (rp->mp_Flags & PF_ACTION) == PA_SIGNAL && rp->mp_SigTask &&
            ((struct Task *)rp->mp_SigTask)->tc_Node.ln_Name)
            who = ((struct Task *)rp->mp_SigTask)->tc_Node.ln_Name;
        plog("<- %4ld a1=%08lx(%s) a2=%08lx a3=%08lx name=\"%s\" da '%.30s'",
             (long)pkt->dp_Type, (unsigned long)pkt->dp_Arg1,
             (pkt->dp_Type != ACTION_READ && pkt->dp_Type != ACTION_SEEK &&
              pkt->dp_Type != ACTION_END && pkt->dp_Arg1 &&
              (pkt->dp_Type == ACTION_LOCATE_OBJECT || pkt->dp_Type == ACTION_PARENT ||
               pkt->dp_Type == ACTION_COPY_DIR || pkt->dp_Type == ACTION_EXAMINE_OBJECT ||
               pkt->dp_Type == ACTION_EXAMINE_NEXT || pkt->dp_Type == ACTION_FREE_LOCK))
             ? ((struct NLock *)BADDR(pkt->dp_Arg1))->path : "-",
             (unsigned long)pkt->dp_Arg2, (unsigned long)pkt->dp_Arg3, name, who);
    }
}

/* errno (negativo da libnfs) -> codice errore DOS */
static LONG map_err(int err)
{
    if (err < 0) err = -err;
    switch (err) {
    case 2:  return ERROR_OBJECT_NOT_FOUND;         /* ENOENT */
    case 1:                                         /* EPERM */
    case 13: return ERROR_READ_PROTECTED;           /* EACCES */
    case 17: return ERROR_OBJECT_EXISTS;            /* EEXIST */
    case 20: return ERROR_DIR_NOT_FOUND;            /* ENOTDIR */
    case 21: return ERROR_OBJECT_WRONG_TYPE;        /* EISDIR */
    case 28: return ERROR_DISK_FULL;                /* ENOSPC */
    case 30: return ERROR_DISK_WRITE_PROTECTED;     /* EROFS */
    case 63: return ERROR_INVALID_COMPONENT_NAME;   /* ENAMETOOLONG */
    case 66: return ERROR_DIRECTORY_NOT_EMPTY;      /* ENOTEMPTY */
    case 70: return ERROR_OBJECT_NOT_FOUND;         /* ESTALE */
    case 12: return ERROR_NO_FREE_STORE;            /* ENOMEM */
    case 32: case 54: case 57: case 60: case 61: case 65:
    case 4:  case 14:   /* EINTR/EFAULT: richiesta annullata o scaduta */
             return ERROR_NO_DISK;                  /* rete giu' */
    }
    return ERROR_NOT_A_DOS_DISK;
}

/* EACCES/EPERM in base all'operazione (come FFS) */
static LONG map_err_op(int err, LONG denied)
{
    if (err == -13 || err == -1) return denied;
    return map_err(err);
}

/* ------------------------------------------------------------------------ */
/* Nomi: Latin-1 (Amiga) <-> UTF-8 (server)                                 */
/*                                                                          */
/* Server -> Amiga: code point < 256 diventano byte Latin-1; tutto il resto */
/* (e ':' e '%') diventa %XX dei byte UTF-8 originali.                      */
/* Amiga -> server: %XX -> byte XX; byte >= 0x80 -> UTF-8.                  */
/* Cosi' ogni nome mostrato riapre esattamente lo stesso file.              */
/* ------------------------------------------------------------------------ */

static const char hexd[] = "0123456789ABCDEF";

static void put_esc(char **o, char *end, unsigned char b)
{
    if (*o + 3 < end) {
        *(*o)++ = '%'; *(*o)++ = hexd[b >> 4]; *(*o)++ = hexd[b & 15];
    }
}

static void utf8_to_amiga(const char *in, char *out, int outsz)
{
    const unsigned char *s = (const unsigned char *)in;
    char *o = out, *end = out + outsz - 1;

    if (opt_raw_charset) {
        strncpy(out, in, outsz - 1);
        out[outsz - 1] = 0;
        return;
    }
    while (*s && o < end) {
        unsigned char c = *s;
        if (c < 0x80) {
            if (c == '%' || c == ':') put_esc(&o, end, c);
            else *o++ = c;
            s++;
        } else if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
            unsigned cp = ((c & 0x1F) << 6) | (s[1] & 0x3F);
            if (cp >= 0xA0 && cp < 0x100) *o++ = (char)cp;
            else { put_esc(&o, end, s[0]); put_esc(&o, end, s[1]); }
            s += 2;
        } else {
            /* sequenze piu' lunghe o non valide: escape byte per byte */
            int n = (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1, i;
            for (i = 0; i < n && s[i]; i++) put_esc(&o, end, s[i]);
            s += i ? i : 1;
        }
    }
    *o = 0;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Converte un componente (len byte) e lo appende a out */
static int amiga_to_utf8(const char *in, int len, char *out, int outsz)
{
    int i, o = 0;
    for (i = 0; i < len; i++) {
        unsigned char c = in[i];
        if (o + 3 >= outsz) return -1;
        if (!opt_raw_charset && c == '%' && i + 2 < len &&
            hexval(in[i + 1]) >= 0 && hexval(in[i + 2]) >= 0) {
            out[o++] = (char)(hexval(in[i + 1]) * 16 + hexval(in[i + 2]));
            i += 2;
        } else if (!opt_raw_charset && c >= 0x80) {
            out[o++] = (char)(0xC0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[o++] = c;
        }
    }
    out[o] = 0;
    return o;
}

/* ------------------------------------------------------------------------ */
/* Percorsi                                                                 */
/* ------------------------------------------------------------------------ */

#define MAXPATH 1024

/* Costruisce il percorso sul server da (lock base, nome DOS).
 * Semantica AmigaDOS: "vol:x" = dalla root, "/" = genitore, "a//b" = b nel
 * genitore di a. Ritorna stringa malloc() o NULL (err in *errp). */
static char *resolve_path(struct NLock *base, const char *name, LONG *errp)
{
    char  buf[MAXPATH], comp[MAXPATH];
    const char *p;
    int   len;

    strcpy(buf, base ? base->path : "/");
    p = strchr(name, ':');
    if (p) { strcpy(buf, "/"); name = p + 1; }

    p = name;
    while (*p) {
        if (*p == '/') {                /* genitore */
            char *s;
            if (!strcmp(buf, "/")) { *errp = ERROR_OBJECT_NOT_FOUND; return NULL; }
            s = strrchr(buf, '/');
            if (s == buf) buf[1] = 0; else *s = 0;
            p++;
            continue;
        }
        {
            const char *e = strchr(p, '/');
            int n = e ? (int)(e - p) : (int)strlen(p);
            if (amiga_to_utf8(p, n, comp, sizeof(comp)) < 0) {
                *errp = ERROR_INVALID_COMPONENT_NAME; return NULL;
            }
            len = strlen(buf);
            if (len + 1 + (int)strlen(comp) >= MAXPATH) {
                *errp = ERROR_INVALID_COMPONENT_NAME; return NULL;
            }
            if (len > 1) buf[len++] = '/';
            strcpy(buf + len, comp);
            p += n;
            if (*p == '/') p++;         /* separatore */
        }
    }
    {
        char *r = strdup(buf);
        if (!r) *errp = ERROR_NO_FREE_STORE;
        return r;
    }
}

static char *parent_path(const char *path)
{
    char *r, *s;
    if (!strcmp(path, "/")) return NULL;
    r = strdup(path);
    if (!r) return NULL;
    s = strrchr(r, '/');
    if (s == r) r[1] = 0; else *s = 0;
    return r;
}

/* ------------------------------------------------------------------------ */
/* Lock e file                                                              */
/* ------------------------------------------------------------------------ */

static struct NLock *new_lock(char *path, const struct nfs_stat_64 *st, LONG access,
                              struct DosPacket *pkt)
{
    struct NLock *l = plat_lock_alloc(sizeof(*l));
    if (!l) { free(path); return NULL; }
    if (pkt && pkt->dp_Port && pkt->dp_Port->mp_SigTask &&
        ((struct Task *)pkt->dp_Port->mp_SigTask)->tc_Node.ln_Name) {
        strncpy(l->owner, ((struct Task *)pkt->dp_Port->mp_SigTask)->tc_Node.ln_Name,
                sizeof(l->owner) - 1);
    }
    l->fl.fl_Key    = (LONG)st->nfs_ino;
    l->fl.fl_Access = access;
    l->fl.fl_Task   = pktport;
    l->fl.fl_Volume = MKBADDR(volnode);
    l->path = path;
    l->st   = *st;
    l->next = locks;
    locks   = l;
    numlocks++;
    return l;
}

static void free_lock(struct NLock *l)
{
    struct NLock **pp;
    if (!l) return;
    for (pp = &locks; *pp; pp = &(*pp)->next)
        if (*pp == l) { *pp = l->next; break; }
    if (l->dir) nfs_closedir(nfs, l->dir);
    free(l->path);
    plat_lock_free(l);
    numlocks--;
}

static void cb_ignore(int err, struct nfs_context *n, void *data, void *priv)
{
    (void)err; (void)n; (void)data; (void)priv;
}

static void free_file(struct NFile *f)
{
    struct NFile **pp;
    for (pp = &files; *pp; pp = &(*pp)->next)
        if (*pp == f) { *pp = f->next; break; }
    if (f->fh && nfs) nfs_close_async(nfs, f->fh, cb_ignore, NULL);
    free(f->path);
    free(f);
    numfiles--;
}

#define LOCK(b)  ((struct NLock *)BADDR(b))

/* Oggetto aperto o con lock? (Delete e MODE_NEWFILE lo rifiutano) */
static BOOL path_in_use(const char *path)
{
    struct NLock *l;
    struct NFile *f;
    for (l = locks; l; l = l->next) if (!strcmp(l->path, path)) return TRUE;
    for (f = files; f; f = f->next) if (!strcmp(f->path, path)) return TRUE;
    return FALSE;
}

static char *join_path(const char *dir, const char *name)
{
    int dl = strlen(dir), nl = strlen(name);
    char *p = malloc(dl + nl + 2);
    if (!p) return NULL;
    memcpy(p, dir, dl);
    if (dl > 1) p[dl++] = '/';
    memcpy(p + dl, name, nl + 1);
    return p;
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* Rename: aggiorna i percorsi di lock e file aperti sotto "old" */
static void rename_paths(const char *old, const char *new_)
{
    int ol = strlen(old);
    struct NLock *l;
    struct NFile *f;
    char **pp, *np;
    for (l = locks; l; l = l->next) {
        pp = &l->path;
        if (!strncmp(*pp, old, ol) && ((*pp)[ol] == 0 || (*pp)[ol] == '/')) {
            np = malloc(strlen(new_) + strlen(*pp + ol) + 1);
            if (np) { strcpy(np, new_); strcat(np, *pp + ol); free(*pp); *pp = np; }
        }
    }
    for (f = files; f; f = f->next) {
        pp = &f->path;
        if (!strncmp(*pp, old, ol) && ((*pp)[ol] == 0 || (*pp)[ol] == '/')) {
            np = malloc(strlen(new_) + strlen(*pp + ol) + 1);
            if (np) { strcpy(np, new_); strcat(np, *pp + ol); free(*pp); *pp = np; }
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Conversione attributi -> FileInfoBlock                                   */
/* ------------------------------------------------------------------------ */

static void unix_to_datestamp(uint64_t usecs, struct DateStamp *ds)
{
    LONG s = (LONG)usecs - gmt_offset_secs - AMIGA_EPOCH_OFFSET;
    if ((int64_t)usecs - gmt_offset_secs < AMIGA_EPOCH_OFFSET) s = 0;
    ds->ds_Days   = s / 86400;
    ds->ds_Minute = (s % 86400) / 60;
    ds->ds_Tick   = (s % 60) * TICKS_PER_SECOND;
}

static LONG unix_to_prot(uint32_t mode)
{
    LONG p = 0;                         /* RWED attivi bassi */
    if (!(mode & 0400)) p |= FIBF_READ;
    if (!(mode & 0200)) p |= FIBF_WRITE | FIBF_DELETE;
    if (!(mode & 0100)) p |= FIBF_EXECUTE;
    return p;
}

static void fill_fib(struct FileInfoBlock *fib, const char *name_utf8,
                     int type, uint32_t mode, uint64_t size, uint64_t mtime,
                     uint32_t uid, uint32_t gid, uint64_t ino, BOOL root)
{
    char name[108];
    int  n;

    memset(fib, 0, sizeof(*fib));
    if (root) strncpy(name, opt_volume, sizeof(name) - 1), name[sizeof(name) - 1] = 0;
    else utf8_to_amiga(name_utf8, name, sizeof(name));
    n = strlen(name);
    if (n > 106) n = 106;
    fib->fib_FileName[0] = n;
    memcpy(fib->fib_FileName + 1, name, n);

    fib->fib_DiskKey      = (LONG)ino;
    fib->fib_DirEntryType = root ? ST_ROOT : (type == NF3DIR ? ST_USERDIR : ST_FILE);
    fib->fib_EntryType    = fib->fib_DirEntryType;
    fib->fib_Protection   = unix_to_prot(mode);
    if (type == NF3DIR) {
        fib->fib_Size = 0;
    } else {
        fib->fib_Size = size > 0x7FFFFFFFULL ? 0x7FFFFFFF : (LONG)size;
    }
    fib->fib_NumBlocks = (LONG)((size + 511) / 512);
    unix_to_datestamp(mtime, &fib->fib_Date);
    fib->fib_OwnerUID = (UWORD)uid;
    fib->fib_OwnerGID = (UWORD)gid;
}

static int stat_type(const struct nfs_stat_64 *st)
{
    switch (st->nfs_mode & 0170000) {
    case 0040000: return NF3DIR;
    case 0120000: return NF3LNK;
    default:      return NF3REG;
    }
}

static void fill_fib_stat(struct FileInfoBlock *fib, const char *path,
                          const struct nfs_stat_64 *st)
{
    const char *name = strrchr(path, '/');
    BOOL root = !strcmp(path, "/");
    fill_fib(fib, name ? name + 1 : path, stat_type(st), (uint32_t)st->nfs_mode,
             st->nfs_size, st->nfs_mtime, (uint32_t)st->nfs_uid,
             (uint32_t)st->nfs_gid, st->nfs_ino, root);
}

static void fill_info(struct InfoData *id, const struct nfs_statvfs_64 *sv)
{
    uint64_t total = 0, used = 0;
    ULONG bpb = 512;

    if (sv) {
        uint64_t fr = sv->f_frsize ? sv->f_frsize : sv->f_bsize;
        total = sv->f_blocks * fr;
        used  = (sv->f_blocks - sv->f_bfree) * fr;
        while (total / bpb > 0x7FFFFFFFULL) bpb <<= 1;
    }
    memset(id, 0, sizeof(*id));
    id->id_DiskState     = opt_readonly ? ID_WRITE_PROTECTED : ID_VALIDATED;
    id->id_NumBlocks     = (LONG)(total / bpb);
    id->id_NumBlocksUsed = (LONG)(used / bpb);
    /* Info (e altri) dividono per id_NumBlocks: mai 0 (guru 80000005) */
    if (id->id_NumBlocks <= 0) { id->id_NumBlocks = 1; id->id_NumBlocksUsed = 1; }
    id->id_BytesPerBlock = bpb;
    id->id_DiskType      = ID_DOS_DISK;
    id->id_VolumeNode    = MKBADDR(volnode);
    id->id_InUse         = (numlocks || numfiles) ? DOSTRUE : DOSFALSE;
}

#ifdef __amigaos4__
/* ------------------------------------------------------------------------ */
/* Pacchetti nativi OS4 (dos.dospackets.doc)                                */
/* ------------------------------------------------------------------------ */

/* Risposta a un DosPacket64: Res1 a 64 bit, e dp_Res0 NON va toccato (chi
 * manda lo usa per capire se l'handler supporta il pacchetto): niente
 * ReplyPkt(), che scriverebbe Res1 a 32 bit proprio li'. */
static void reply64(struct DosPacket *pkt, int64_t res1, LONG res2)
{
    struct DosPacket64 *p = (struct DosPacket64 *)pkt;
    struct MsgPort *rp = p->dp_Port;
    if (opt_log) plog("-> %4ld res1=%lld res2=%ld", (long)p->dp_Type,
                      (long long)res1, (long)res2);
    p->dp_Res1 = res1;
    p->dp_Res2 = res2;
    p->dp_Port = pktport;
    PutMsg(rp, p->dp_Link);
}

/* ExamineData (allocata con AllocDosObject: la libera chi la riceve) dagli
 * attributi NFS, con le stesse conversioni della FileInfoBlock ma con la
 * dimensione a 64 bit. ctx != NULL: nodo per EXAMINEDATA_DIR (memoria dal
 * pool del contesto). name e' gia' nel set di caratteri Amiga. */
static struct ExamineData *make_exd_raw(const char *name, BOOL dir, uint64_t size,
                                        uint64_t mtime, uint32_t mode, uint32_t uid,
                                        uint32_t gid, uint64_t ino, APTR ctx)
{
    struct ExamineData *d = ctx
        ? AllocDosObjectTags(DOS_EXAMINEDATA,
                             ADO_ExamineData_NameSize, strlen(name) + 1,
                             ADO_ExamineData_CommentSize, 1,
                             ADO_ExamineDir_Context, ctx, TAG_END)
        : AllocDosObjectTags(DOS_EXAMINEDATA,
                             ADO_ExamineData_NameSize, strlen(name) + 1,
                             ADO_ExamineData_CommentSize, 1, TAG_END);
    if (!d) return NULL;
    d->Type       = dir ? FSO_TYPE_DIRECTORY : FSO_TYPE_FILE;
    d->FileSize   = dir ? -1LL : (int64)size;
    unix_to_datestamp(mtime, &d->Date);
    d->RefCount   = 0;                  /* niente hardlink alla Amiga */
    d->ObjectID   = ino;
    strcpy(d->Name, name);
    d->Comment[0] = 0;
    d->Protection = unix_to_prot(mode);
    d->OwnerUID   = uid;
    d->OwnerGID   = gid;
    return d;
}

static struct ExamineData *make_exd(const char *path, const struct nfs_stat_64 *st)
{
    char name[256];
    const char *base = strrchr(path, '/');
    BOOL root = !strcmp(path, "/");

    if (root) { strncpy(name, opt_volume, sizeof(name) - 1); name[sizeof(name) - 1] = 0; }
    else utf8_to_amiga(base ? base + 1 : path, name, sizeof(name));
    return make_exd_raw(name, root || stat_type(st) == NF3DIR, st->nfs_size,
                        st->nfs_mtime, (uint32_t)st->nfs_mode, (uint32_t)st->nfs_uid,
                        (uint32_t)st->nfs_gid, st->nfs_ino, NULL);
}

static void end_req(struct Req *r, LONG res1, LONG res2);

static void cb_examinedata(int err, struct nfs_context *n, void *data, void *priv)
{
    struct Req *r = priv;
    struct ExamineData *d;
    (void)n;
    if (err) { end_req(r, 0, map_err(err)); return; }
    r->lock->st = *(struct nfs_stat_64 *)data;
    d = make_exd(r->lock->path, &r->lock->st);
    end_req(r, (LONG)d, d ? 0 : ERROR_NO_FREE_STORE);
}

/* --- EXAMINEDATA_DIR (ExamineDir() nativo) ---
 * Un solo giro: tutto il listing va nella FreshNodeList, poi FALSE +
 * ERROR_NO_MORE_ENTRIES (l'unico codice che dice al DOS "scansione finita",
 * dos.dospackets.doc). FSPrivate[0]: 1 = finita, 2 = errore (in [1]).
 * I symlink si mostrano col tipo/dimensione del bersaglio, come in
 * EXAMINE_NEXT (link trasparenti); bersaglio assente = file. */
#define EXD_CTX(r) ((struct PRIVATE_ExamineDirContext *)(r)->ctx)

static void exd_add(struct Req *r, const char *name_utf8, BOOL dir, uint64_t size,
                    uint64_t mtime, uint32_t mode, uint32_t uid, uint32_t gid,
                    uint64_t ino)
{
    char name[256];
    struct ExamineData *d;
    utf8_to_amiga(name_utf8, name, sizeof(name));
    d = make_exd_raw(name, dir, size, mtime, mode, uid, gid, ino, r->ctx);
    if (d) AddTail((struct List *)&EXD_CTX(r)->FreshNodeList, (struct Node *)d);
    else if (!r->io_err) r->io_err = ERROR_NO_FREE_STORE;
}

static void exd_fill(struct Req *r);

static void cb_exd_link(int err, struct nfs_context *n, void *data, void *priv)
{
    struct Req *r = priv;
    struct nfsdirent *e = r->ent;
    (void)n;
    if (err)
        exd_add(r, e->name, FALSE, e->size, e->mtime.tv_sec, e->mode, e->uid,
                e->gid, e->inode);
    else {
        struct nfs_stat_64 *st = data;
        exd_add(r, e->name, stat_type(st) == NF3DIR, st->nfs_size, st->nfs_mtime,
                (uint32_t)st->nfs_mode, (uint32_t)st->nfs_uid, (uint32_t)st->nfs_gid,
                st->nfs_ino);
    }
    free(r->path); r->path = NULL;
    exd_fill(r);
}

static void exd_fill(struct Req *r)
{
    struct NLock *l = r->lock;
    struct nfsdirent *e;

    while (!r->io_err && (e = nfs_readdir(nfs, r->xdir))) {
        if (!strcmp(e->name, ".") || !strcmp(e->name, "..")) continue;
        if (e->type == NF3LNK) {
            int len = strlen(l->path) + strlen(e->name) + 2;
            r->path = malloc(len);
            if (r->path) {
                snprintf(r->path, len, "%s/%s",
                         strcmp(l->path, "/") ? l->path : "", e->name);
                r->ent = e;
                if (!nfs_stat64_async(nfs, r->path, cb_exd_link, r)) return;
                free(r->path); r->path = NULL;
            }
            exd_add(r, e->name, FALSE, e->size, e->mtime.tv_sec, e->mode,
                    e->uid, e->gid, e->inode);
            continue;
        }
        exd_add(r, e->name, e->type == NF3DIR, e->size, e->mtime.tv_sec, e->mode,
                e->uid, e->gid, e->inode);
    }
    nfs_closedir(nfs, r->xdir);
    r->xdir = NULL;
    if (r->io_err) {
        EXD_CTX(r)->FSPrivate[0] = 2;
        EXD_CTX(r)->FSPrivate[1] = r->io_err;
        end_req(r, DOSFALSE, r->io_err);
    } else {
        EXD_CTX(r)->FSPrivate[0] = 1;
        end_req(r, DOSFALSE, ERROR_NO_MORE_ENTRIES);
    }
}

static void cb_exd_opendir(int err, struct nfs_context *n, void *data, void *priv)
{
    struct Req *r = priv;
    (void)n;
    if (err) {
        EXD_CTX(r)->FSPrivate[0] = 2;
        EXD_CTX(r)->FSPrivate[1] = map_err(err);
        end_req(r, DOSFALSE, map_err(err));
        return;
    }
    r->xdir = data;
    exd_fill(r);
}
#endif

/* ------------------------------------------------------------------------ */
/* Richieste asincrone                                                      */
/* ------------------------------------------------------------------------ */

static struct Req *new_req(struct DosPacket *pkt)
{
    struct Req *r = calloc(1, sizeof(*r));
    if (!r) { reply(pkt, DOSFALSE, ERROR_NO_FREE_STORE); return NULL; }
    r->pkt = pkt;
    active_reqs++;
    if (opt_log && eclock_freq) { r->m_rpc0 = rpc_calls; r->m_t0 = plat_eclock_read(); }
    return r;
}

static void end_req(struct Req *r, LONG res1, LONG res2)
{
    if (opt_log && eclock_freq && r->pkt) {
        uint64_t t1 = plat_eclock_read();
        plog("   %4ld: rpc=%lu t=%lums", (long)r->pkt->dp_Type,
             (unsigned long)(rpc_calls - r->m_rpc0),
             (unsigned long)((t1 - r->m_t0) * 1000 / eclock_freq));
    }
#ifdef __amigaos4__
    if (r->pkt && r->is64) reply64(r->pkt, res2 == 0 ? -1LL : 0LL, res2);
    else
#endif
    if (r->pkt) reply(r->pkt, res1, res2);
    free(r->ci_parent);
    free(r->src);
    free(r->dst);
    free(r->dstname);
    free(r->ci_prefix);
    free(r->ci_cand);
    free(r->path);
    free(r);
    active_reqs--;
}

/* ------------------------------------------------------------------------ */
/* Ricerca case-insensitive (CASE=INSENSITIVE, default)                     */
/*                                                                          */
/* ci_stat(r, cb) si comporta come nfs_stat64_async(r->path, cb, r) ma, se  */
/* il percorso esatto non esiste, lo risolve componente per componente:     */
/* stat esatta del componente, e solo se manca lettura della directory e    */
/* confronto senza maiuscole (ASCII + Latin-1 in UTF-8). Alla fine r->path  */
/* contiene i nomi REALI del server.                                        */
/* ------------------------------------------------------------------------ */

/* Percorso per CREARE r->path: se la ricerca case-insensitive ha trovato il
 * genitore reale, nome richiesto dentro quello; altrimenti r->path tale e quale */
static char *target_path(struct Req *r)
{
    if (r->ci_parent) return join_path(r->ci_parent, base_name(r->path));
    return strdup(r->path);
}

/* prossimo carattere "ripiegato" in minuscolo; avanza *pp */
static unsigned fold_next(const unsigned char **pp)
{
    const unsigned char *p = *pp;
    unsigned c = *p++;
    if (c >= 'A' && c <= 'Z') c += 32;
    else if (c == 0xC3 && *p >= 0x80 && *p <= 0x9E && *p != 0x97) {
        c = 0xC300 | (*p + 0x20); p++;          /* À..Þ -> à..þ */
    } else if (c == 0xC3 && *p) {
        c = 0xC300 | *p; p++;
    }
    *pp = p;
    return c;
}

/* confronta a (len byte) con la stringa b, ignorando le maiuscole */
static BOOL fold_eq(const char *a, int alen, const char *b)
{
    const unsigned char *p = (const unsigned char *)a, *pe = p + alen;
    const unsigned char *q = (const unsigned char *)b;
    while (p < pe && *q)
        if (fold_next(&p) != fold_next(&q)) return FALSE;
    return p >= pe && !*q;
}

/* Cache dei nomi di poche directory, convalidata con mtime (sec+nsec) della
 * directory e aggiornata dall'handler dopo le PROPRIE creazioni/cancellazioni:
 * evita di rileggere l'intera directory a ogni file creato. */
#define NC_SLOTS 4
struct NameCache {
    char     *path;
    uint64_t  mt, mtns;
    char    **names;
    int       n, cap;
    ULONG     stamp;
};
static struct NameCache ncache[NC_SLOTS];
static ULONG nc_clock;

static void nc_drop(struct NameCache *c)
{
    int i;
    for (i = 0; i < c->n; i++) free(c->names[i]);
    free(c->names);
    free(c->path);
    memset(c, 0, sizeof(*c));
}

static struct NameCache *nc_lookup(const char *path)
{
    int i;
    for (i = 0; i < NC_SLOTS; i++)
        if (ncache[i].path && !strcmp(ncache[i].path, path)) return &ncache[i];
    return NULL;
}

static struct NameCache *nc_find(const char *path, const struct nfs_stat_64 *st)
{
    struct NameCache *c = nc_lookup(path);
    if (!c) return NULL;
    if (c->mt != st->nfs_mtime || c->mtns != st->nfs_mtime_nsec) { nc_drop(c); return NULL; }
    c->stamp = ++nc_clock;
    return c;
}

static int nc_add(struct NameCache *c, const char *name)
{
    if (c->n == c->cap) {
        int nc = c->cap ? c->cap * 2 : 32;
        char **nn = realloc(c->names, nc * sizeof(char *));
        if (!nn) return -1;
        c->names = nn; c->cap = nc;
    }
    if (!(c->names[c->n] = strdup(name))) return -1;
    c->n++;
    return 0;
}

static struct NameCache *nc_new(const char *path, const struct nfs_stat_64 *st)
{
    struct NameCache *c = nc_lookup(path);
    int i;
    if (!c) {
        c = &ncache[0];
        for (i = 0; i < NC_SLOTS; i++) {
            if (!ncache[i].path) { c = &ncache[i]; break; }
            if (ncache[i].stamp < c->stamp) c = &ncache[i];
        }
    }
    nc_drop(c);
    if (!(c->path = strdup(path))) return NULL;
    c->mt = st->nfs_mtime; c->mtns = st->nfs_mtime_nsec;
    c->stamp = ++nc_clock;
    return c;
}

static void cb_nc_refresh(int err, struct nfs_context *n, void *data, void *priv)
{
    char *path = priv;
    struct NameCache *c = nc_lookup(path);
    (void)n;
    if (c) {
        if (err) nc_drop(c);
        else {
            c->mt   = ((struct nfs_stat_64 *)data)->nfs_mtime;
            c->mtns = ((struct nfs_stat_64 *)data)->nfs_mtime_nsec;
        }
    }
    free(path);
}

/* L'handler ha creato (add) o cancellato (!add) path: aggiorna la cache del
 * genitore e ne rilegge la mtime. */
static void nc_note(const char *path, BOOL add)
{
    char *par = parent_path(path);
    const char *name = base_name(path);
    struct NameCache *c;
    int i;
    if (!par) return;
    c = nc_lookup(par);
    if (c) {
        if (add) {
            if (nc_add(c, name)) nc_drop(c);
        } else {
            for (i = 0; i < c->n; i++)
                if (!strcmp(c->names[i], name)) {
                    free(c->names[i]);
                    c->names[i] = c->names[--c->n];
                    break;
                }
        }
        if (c->path && nfs_stat64_async(nfs, par, cb_nc_refresh, par) == 0) return;
        if (c->path) nc_drop(c);
    }
    free(par);
}

/* Rename/delete di una directory: le cache dei percorsi sotto diventano false */
static void nc_drop_tree(const char *path)
{
    int i, l = strlen(path);
    for (i = 0; i < NC_SLOTS; i++)
        if (ncache[i].path && !strncmp(ncache[i].path, path, l) &&
            (ncache[i].path[l] == 0 || ncache[i].path[l] == '/'))
            nc_drop(&ncache[i]);
}

static void ci_step(struct Req *r);
static void ci_scan(struct Req *r);

static void ci_done(struct Req *r, int err, struct nfs_stat_64 *st)
{
    nfs_cb cb = r->ci_cb;
    if (!err && r->ci_prefix) {
        free(r->path);
        r->path = r->ci_prefix;
        r->ci_prefix = NULL;
    }
    free(r->ci_prefix); r->ci_prefix = NULL;
    free(r->ci_cand);   r->ci_cand = NULL;
    cb(err, nfs, st, r);
}

static void cb_ci_dir(int err, struct nfs_context *n, void *data, void *priv);

static void cb_ci_comp(int err, struct nfs_context *n, void *data, void *priv)
{
    struct Req *r = priv;
    (void)n;
    if (!err) {
        free(r->ci_prefix);
        r->ci_prefix = r->ci_cand;
        r->ci_cand = NULL;
        r->st = *(struct nfs_stat_64 *)data;
        r->st_valid = TRUE;
        r->ci_rest += r->ci_complen;
        r->ci_scanned = FALSE;
        ci_step(r);
        return;
    }
    if (err == -2 && !r->ci_scanned) {          /* ENOENT: cerca nel listing */
        r->ci_scanned = TRUE;
        ci_scan(r);
        return;
    }
    ci_done(r, err, NULL);
}

static void ci_try(struct Req *r, const char *name, int len)
{
    int plen = strlen(r->ci_prefix);
    free(r->ci_cand);
    r->ci_cand = malloc(plen + len + 2);
    if (!r->ci_cand) { ci_done(r, -12, NULL); return; }
    memcpy(r->ci_cand, r->ci_prefix, plen);
    if (plen > 1) r->ci_cand[plen++] = '/';
    memcpy(r->ci_cand + plen, name, len);
    r->ci_cand[plen + len] = 0;
    if (nfs_stat64_async(nfs, r->ci_cand, cb_ci_comp, r))
        ci_done(r, -5, NULL);
}

/* esito della ricerca del componente corrente nel listing */
static void ci_found(struct Req *r, const char *found)
{
    if (!found) {
        /* manca solo l'ultimo componente? allora il genitore esiste: serve
         * per creare file/dir col nome richiesto nella directory reale */
        const char *t = r->ci_rest + r->ci_complen;
        while (*t == '/') t++;
        if (!*t) { free(r->ci_parent); r->ci_parent = strdup(r->ci_prefix); }
        ci_done(r, -2, NULL);
        return;
    }
    if ((int)strlen(found) != r->ci_complen || strncmp(found, r->ci_rest, r->ci_complen))
        plog("   case: \"%.*s\" -> \"%s\"", r->ci_complen, r->ci_rest, found);
    ci_try(r, found, strlen(found));
}

static void cb_ci_dir(int err, struct nfs_context *n, void *data, void *priv)
{
    struct Req *r = priv;
    struct nfsdir *d = data;
    struct nfsdirent *e;
    struct NameCache *c;
    char *found = NULL;
    (void)n;
    if (err) { ci_done(r, err, NULL); return; }
    c = r->st_valid ? nc_new(r->ci_prefix, &r->st) : NULL;
    while ((e = nfs_readdir(nfs, d))) {
        if (!strcmp(e->name, ".") || !strcmp(e->name, "..")) continue;
        if (c && nc_add(c, e->name)) { nc_drop(c); c = NULL; }
        if (!found && fold_eq(r->ci_rest, r->ci_complen, e->name))
            found = strdup(e->name);
        if (found && !c) break;
    }
    nfs_closedir(nfs, d);
    ci_found(r, found);
    free(found);
}

/* cerca il componente corrente in ci_prefix: dalla cache se valida */
static void ci_scan(struct Req *r)
{
    struct NameCache *c = r->st_valid ? nc_find(r->ci_prefix, &r->st) : NULL;
    if (c) {
        int i;
        for (i = 0; i < c->n; i++)
            if (fold_eq(r->ci_rest, r->ci_complen, c->names[i])) {
                char *f = strdup(c->names[i]);
                ci_found(r, f);
                free(f);
                return;
            }
        ci_found(r, NULL);
        return;
    }
    if (nfs_opendir_async(nfs, r->ci_prefix, cb_ci_dir, r))
        ci_done(r, -5, NULL);
}

static void ci_step(struct Req *r)
{
    const char *e;
    while (*r->ci_rest == '/') r->ci_rest++;
    if (!*r->ci_rest) { ci_done(r, 0, &r->st); return; }
    e = strchr(r->ci_rest, '/');
    r->ci_complen = e ? (int)(e - r->ci_rest) : (int)strlen(r->ci_rest);
    ci_try(r, r->ci_rest, r->ci_complen);
}

static void cb_ci_parent(int err, struct nfs_context *n, void *data, void *priv)
{
    struct Req *r = priv;
    (void)n;
    if (!err) {
        r->st = *(struct nfs_stat_64 *)data;
        r->st_valid = TRUE;
        r->ci_rest = base_name(r->path);
        r->ci_complen = strlen(r->ci_rest);
        r->ci_scanned = TRUE;           /* l'esatto e' gia' stato provato */
        ci_scan(r);
        return;
    }
    if (err != -2) { ci_done(r, err, NULL); return; }
    free(r->ci_prefix);
    r->ci_prefix = strdup("/");
    if (!r->ci_prefix) { ci_done(r, -12, NULL); return; }
    r->st_valid = FALSE;
    r->ci_rest = r->path;
    r->ci_scanned = FALSE;
    ci_step(r);
}

static void cb_ci_first(int err, struct nfs_context *n, void *data, void *priv)
{
    struct Req *r = priv;
    (void)n;
    if (err != -2 || opt_case_sensitive) { r->ci_cb(err, nfs, data, r); return; }
    /* il percorso esatto non esiste: prima si prova il genitore esatto (caso
     * tipico: file nuovo in una directory esistente), poi, se manca anche
     * quello, risoluzione componente per componente dalla root */
    r->ci_prefix = parent_path(r->path);
    if (!r->ci_prefix) { r->ci_cb(-2, nfs, NULL, r); return; }
    if (nfs_stat64_async(nfs, r->ci_prefix, cb_ci_parent, r))
        r->ci_cb(-5, nfs, NULL, r);
}

/* stat di r->path con fallback case-insensitive; cb riceve (err, st) */
static int ci_stat(struct Req *r, nfs_cb cb)
{
    r->ci_cb = cb;
    return nfs_stat64_async(nfs, r->path, cb_ci_first, r);
}

/* Variante per le CREAZIONI: di solito il file non esiste, quindi si salta
 * la stat esatta e si parte dal genitore + cache dei nomi (che dice anche se
 * il nome esiste gia', con qualunque combinazione di maiuscole). */
static int ci_stat_create(struct Req *r, nfs_cb cb)
{
    if (opt_case_sensitive) return nfs_stat64_async(nfs, r->path, cb, r);
    r->ci_cb = cb;
    r->ci_prefix = parent_path(r->path);
    if (!r->ci_prefix) return nfs_stat64_async(nfs, r->path, cb_ci_first, r);
    return nfs_stat64_async(nfs, r->ci_prefix, cb_ci_parent, r);
}

/* --- LOCATE_OBJECT / PARENT / COPY_DIR: stat -> nuovo lock --- */
static void cb_lock(int err, struct nfs_context *n, void *data, void *priv)
{
    struct Req *r = priv;
    struct NLock *l;
    (void)n;
    if (err) { end_req(r, DOSFALSE, map_err(err)); return; }
    l = new_lock(r->path, (struct nfs_stat_64 *)data,
                 (r->pkt->dp_Type == ACTION_LOCATE_OBJECT &&
                  r->pkt->dp_Arg3 == EXCLUSIVE_LOCK) ||
                 r->pkt->dp_Type == ACTION_CREATE_DIR ? EXCLUSIVE_LOCK : SHARED_LOCK,
                 r->pkt);
    r->path = NULL;                     /* ora appartiene al lock */
    end_req(r, l ? MKBADDR(l) : DOSFALSE, l ? 0 : ERROR_NO_FREE_STORE);
}

static void start_lock(struct DosPacket *pkt, char *path)
{
    plog("   stat %s", path);
    struct Req *r = new_req(pkt);
    if (!r) { free(path); return; }
    r->path = path;
    if (ci_stat(r, cb_lock))
        end_req(r, DOSFALSE, ERROR_NO_DISK);
}

/* --- EXAMINE_OBJECT su lock senza attributi aggiornati --- */
static void cb_examine(int err, struct nfs_context *n, void *data, void *priv)
{
    struct Req *r = priv;
    (void)n;
    if (err) { end_req(r, DOSFALSE, map_err(err)); return; }
    fill_fib_stat((struct FileInfoBlock *)BADDR(r->fib), "/",
                  (struct nfs_stat_64 *)data);
    end_req(r, DOSTRUE, 0);
}

static void cb_examine_lock(int err, struct nfs_context *n, void *data, void *priv)
{
    struct Req *r = priv;
    (void)n;
    if (err) { end_req(r, DOSFALSE, map_err(err)); return; }
    r->lock->st = *(struct nfs_stat_64 *)data;
    fill_fib_stat((struct FileInfoBlock *)BADDR(r->fib), r->lock->path, &r->lock->st);
    end_req(r, DOSTRUE, 0);
}

/* --- EXAMINE_NEXT --- */

/* Symlink nel listing: mostrato col tipo/dimensione del bersaglio (link
 * trasparenti); se il bersaglio non esiste resta un file. */
static void cb_exnext_link(int err, struct nfs_context *n, void *data, void *priv)
{
    struct Req *r = priv;
    struct FileInfoBlock *fib = (struct FileInfoBlock *)BADDR(r->fib);
    struct nfsdirent *e = r->ent;
    (void)n;
    if (err)
        fill_fib(fib, e->name, NF3REG, e->mode, e->size, e->mtime.tv_sec,
                 e->uid, e->gid, e->inode, FALSE);
    else
        fill_fib_stat(fib, r->path, (struct nfs_stat_64 *)data);
    end_req(r, DOSTRUE, 0);
}

static void exnext_from_dir(struct Req *r)
{
    struct NLock *l = r->lock;
    struct nfsdirent *e;
    struct FileInfoBlock *fib = (struct FileInfoBlock *)BADDR(r->fib);

    while ((e = nfs_readdir(nfs, l->dir))) {
        if (!strcmp(e->name, ".") || !strcmp(e->name, "..")) continue;
        if (e->type == NF3LNK) {
            int len = strlen(l->path) + strlen(e->name) + 2;
            r->path = malloc(len);
            if (r->path) {
                snprintf(r->path, len, "%s/%s",
                         strcmp(l->path, "/") ? l->path : "", e->name);
                r->ent = e;
                if (!nfs_stat64_async(nfs, r->path, cb_exnext_link, r)) return;
                free(r->path); r->path = NULL;
            }
        }
        fill_fib(fib, e->name, e->type == NF3LNK ? NF3REG : (int)e->type,
                 e->mode, e->size, e->mtime.tv_sec, e->uid, e->gid,
                 e->inode, FALSE);
        end_req(r, DOSTRUE, 0);
        return;
    }
    nfs_closedir(nfs, l->dir);
    l->dir = NULL;
    end_req(r, DOSFALSE, ERROR_NO_MORE_ENTRIES);
}

static void cb_opendir(int err, struct nfs_context *n, void *data, void *priv)
{
    struct Req *r = priv;
    (void)n;
    if (err) { end_req(r, DOSFALSE, map_err(err)); return; }
    r->lock->dir = data;
    exnext_from_dir(r);
}

/* --- INFO / DISK_INFO --- */
static void cb_statvfs(int err, struct nfs_context *n, void *data, void *priv)
{
    struct Req *r = priv;
    (void)n;
    fill_info((struct InfoData *)BADDR(r->fib),
              err ? NULL : (struct nfs_statvfs_64 *)data);
    end_req(r, DOSTRUE, 0);
}

/* --- FINDINPUT / FH_FROM_LOCK: stat -> open --- */
static void cb_open(int err, struct nfs_context *n, void *data, void *priv)
{
    struct Req *r = priv;
    struct NFile *f;
    (void)n;
    if (err == -13 && r->omode == OPEN_OLD && (r->oflags & O_RDWR)) {
        /* MODE_OLDFILE su file senza permesso di scrittura: sola lettura */
        r->oflags = O_RDONLY;
        if (!nfs_open2_async(nfs, r->path, r->oflags, 0, cb_open, r)) return;
        err = -5;
    }
    if (err) {
        end_req(r, DOSFALSE, r->omode == OPEN_OLD ? map_err(err)
                             : map_err_op(err, ERROR_WRITE_PROTECTED));
        return;
    }
    f = calloc(1, sizeof(*f));
    if (!f) {
        nfs_close_async(nfs, data, cb_ignore, NULL);
        end_req(r, DOSFALSE, ERROR_NO_FREE_STORE);
        return;
    }
    f->fh   = data;
    f->path = r->path; r->path = NULL;
    f->st   = r->st;
    if (r->oflags & O_TRUNC) f->st.nfs_size = 0;
    f->oflags = r->oflags & ~(O_CREAT | O_TRUNC | O_EXCL);
    if (r->oflags & O_CREAT) { nc_note(f->path, TRUE); notify_touch(f->path); }
    f->next = files; files = f; numfiles++;
    r->fh->fh_Arg1 = (LONG)f;
    f->pubfh = r->fh;
    if (r->freelock) free_lock(LOCK(r->freelock));
    end_req(r, DOSTRUE, 0);
}

static void cb_open_stat(int err, struct nfs_context *n, void *data, void *priv)
{
    struct Req *r = priv;
    struct nfs_stat_64 *st = data;
    (void)n;
    int mode;

    if (err == -2 && r->omode != OPEN_OLD) {
        /* non esiste: lo creiamo (MODE_NEWFILE / MODE_READWRITE) */
        char *np;
        if (opt_readonly) { end_req(r, DOSFALSE, ERROR_DISK_WRITE_PROTECTED); return; }
        np = target_path(r);
        if (!np) { end_req(r, DOSFALSE, ERROR_NO_FREE_STORE); return; }
        free(r->path); r->path = np;
        mode = 0666 & ~opt_umask;
        memset(&r->st, 0, sizeof(r->st));
        r->st.nfs_mode  = S_IFREG_ | mode;
        r->st.nfs_uid   = opt_uid;
        r->st.nfs_gid   = opt_gid;
        r->st.nfs_mtime = time(NULL) + gmt_offset_secs;
        r->oflags = O_CREAT | O_RDWR | (r->omode == OPEN_NEW ? O_TRUNC : 0) |
                    (opt_sync ? O_SYNC : 0);
        plog("   create %s", r->path);
        if (nfs_open2_async(nfs, r->path, r->oflags, mode, cb_open, r))
            end_req(r, DOSFALSE, ERROR_NO_DISK);
        return;
    }
    if (err) { end_req(r, DOSFALSE, map_err(err)); return; }
    if (stat_type(st) == NF3DIR) { end_req(r, DOSFALSE, ERROR_OBJECT_WRONG_TYPE); return; }
    if (r->omode == OPEN_NEW && path_in_use(r->path)) {
        end_req(r, DOSFALSE, ERROR_OBJECT_IN_USE);
        return;
    }
    if (opt_readonly && r->omode != OPEN_OLD) {
        end_req(r, DOSFALSE, ERROR_DISK_WRITE_PROTECTED);
        return;
    }
    r->st = *st;
    r->oflags = opt_readonly ? O_RDONLY
              : O_RDWR | (r->omode == OPEN_NEW ? O_TRUNC : 0) | (opt_sync ? O_SYNC : 0);
    if (nfs_open2_async(nfs, r->path, r->oflags, 0, cb_open, r))
        end_req(r, DOSFALSE, ERROR_NO_DISK);
}

static void start_open(struct DosPacket *pkt, struct FileHandle *fh, char *path,
                       BPTR freelock, int omode)
{
    plog("   open %s (modo %d)", path, omode);
    struct Req *r = new_req(pkt);
    if (!r) { free(path); return; }
    r->path = path;
    r->omode = omode;
    r->fh = fh;
    r->freelock = freelock;
    if ((omode == OPEN_OLD ? ci_stat(r, cb_open_stat) : ci_stat_create(r, cb_open_stat)))
        end_req(r, DOSFALSE, ERROR_NO_DISK);
}

/* --- READ: pread ripetute fino a soddisfare la richiesta --- */
/* --- READ / WRITE: fino a opt_par blocchi in volo per pacchetto ---
 * Con latenze alte (rete lenta, stack emulato) il throughput cresce quasi
 * linearmente col numero di richieste parallele. I blocchi scrivono/leggono
 * direttamente nel buffer del chiamante. */
struct Chunk { struct Req *r; LONG off, len; };

static void io_finish(struct Req *r)
{
    LONG total;
    if (r->io_err) {
        end_req(r, -1, r->is_write ? map_err_op(r->io_err, ERROR_WRITE_PROTECTED)
                                   : map_err(r->io_err));
        return;
    }
    total = r->eof_at >= 0 ? r->eof_at : r->issued;
    r->file->pos += total;
    if (r->is_write) {
        if (r->file->pos > r->file->st.nfs_size) r->file->st.nfs_size = r->file->pos;
        r->file->dirty = TRUE;
        r->file->modified = TRUE;
    }
    end_req(r, total, 0);
}

static void io_issue(struct Req *r);

static void cb_chunk(int err, struct nfs_context *n, void *data, void *priv)
{
    struct Chunk *c = priv;
    struct Req *r = c->r;
    (void)n; (void)data;
    r->inflight--;
    if (err < 0) {
        if (!r->io_err) r->io_err = err;
    } else if (err < c->len) {              /* short: EOF (lettura) */
        LONG end = c->off + err;
        if (r->eof_at < 0 || end < r->eof_at) r->eof_at = end;
    }
    free(c);
    io_issue(r);
}

static void io_issue(struct Req *r)
{
    size_t max = r->is_write ? nfs_get_writemax(nfs) : nfs_get_readmax(nfs);

    while (r->inflight < opt_par && r->issued < r->want && !r->io_err && r->eof_at < 0) {
        struct Chunk *c = malloc(sizeof(*c));
        LONG len = r->want - r->issued;
        int rc;
        if ((size_t)len > max) len = max;
        if (!c) { r->io_err = -12; break; }
        c->r = r; c->off = r->issued; c->len = len;
        rc = r->is_write
           ? nfs_pwrite_async(nfs, r->file->fh, r->buf + c->off, len,
                              r->file->pos + c->off, cb_chunk, c)
           : nfs_pread_async(nfs, r->file->fh, r->buf + c->off, len,
                             r->file->pos + c->off, cb_chunk, c);
        if (rc) { free(c); r->io_err = -57; break; }
        r->issued += len;
        r->inflight++;
    }
    if (r->inflight == 0) io_finish(r);
}

/* Dopo una riconnessione gli NFile hanno fh == NULL: si riaprono per percorso
 * (i file handle NFSv3 sopravvivono al riavvio del server) e il pacchetto
 * viene rielaborato come se nulla fosse. */
static void dispatch(struct DosPacket *pkt);

static void cb_reopen(int err, struct nfs_context *n, void *data, void *priv)
{
    struct Req *r = priv;
    struct DosPacket *pkt = r->pkt;
    (void)n;
    if (err) { end_req(r, -1, map_err(err)); return; }
    r->file->fh = data;
    plog("   riaperto %s", r->file->path);
    r->pkt = NULL;
    end_req(r, 0, 0);
    dispatch(pkt);
}

static BOOL ensure_open(struct DosPacket *pkt, struct NFile *f)
{
    struct Req *r;
    if (f->fh) return TRUE;
    r = new_req(pkt);
    if (!r) return FALSE;
    r->file = f;
    if (nfs_open2_async(nfs, f->path, f->oflags, 0, cb_reopen, r))
        end_req(r, -1, ERROR_NO_DISK);
    return FALSE;
}

static void start_io(struct DosPacket *pkt, struct NFile *f, BOOL wr)
{
    struct Req *r = new_req(pkt);
    if (!r) return;
    r->file     = f;
    r->buf      = (UBYTE *)pkt->dp_Arg2;
    r->want     = pkt->dp_Arg3;
    r->eof_at   = -1;
    r->is_write = wr;
    io_issue(r);
}

/* ------------------------------------------------------------------------ */
/* Notify (ADD_NOTIFY / REMOVE_NOTIFY) con polling                          */
/*                                                                          */
/* NFS non ha notifiche: ogni NOTIFYPOLL secondi si fa una stat dei         */
/* percorsi sorvegliati (mtime/ctime/size/esistenza). Le modifiche fatte    */
/* tramite l'handler stesso avvisano subito (notify_touch).                 */
/* ------------------------------------------------------------------------ */

struct NNotify {
    struct NNotify       *next;
    struct NotifyRequest *nr;
    char                 *path;         /* percorso sul server */
    BOOL                  known;        /* attributi di riferimento validi */
    BOOL                  exists;
    uint64_t              mt, mtns, ct, ctns, size;
    BOOL                  polling;      /* stat in volo */
    BOOL                  removed;      /* REMOVE_NOTIFY ricevuto */
    BOOL                  again;        /* WAIT_REPLY: notifica in attesa */
    LONG                  outstanding;  /* messaggi non ancora restituiti */
};

static struct NNotify *notifies;
static LONG            numnotifies;
/* notify_msgs (globale, dichiarato sopra): DIE non deve chiudere la porta
 * finche' un'applicazione ha ancora in mano un nostro messaggio */
static struct MsgPort *notify_port;     /* ritorno dei NotifyMessage */

static void notify_free_if_done(struct NNotify *n)
{
    if (n->removed && !n->polling && n->outstanding == 0) {
        free(n->path);
        free(n);
    }
}

static void notify_fire(struct NNotify *n)
{
    struct NotifyRequest *nr = n->nr;
    if (n->removed) return;
    if (nr->nr_Flags & NRF_SEND_SIGNAL) {
        Signal(nr->nr_stuff.nr_Signal.nr_Task, 1UL << nr->nr_stuff.nr_Signal.nr_SignalNum);
    } else if (nr->nr_Flags & NRF_SEND_MESSAGE) {
        struct NotifyMessage *m;
        if ((nr->nr_Flags & NRF_WAIT_REPLY) && n->outstanding) { n->again = TRUE; return; }
        m = AllocVec(sizeof(*m), MEMF_PUBLIC | MEMF_CLEAR);
        if (!m || !notify_port) { FreeVec(m); return; }
        m->nm_ExecMessage.mn_ReplyPort = notify_port;
        m->nm_ExecMessage.mn_Length    = sizeof(*m);
        m->nm_Class      = NOTIFY_CLASS;
        m->nm_Code       = NOTIFY_CODE;
        m->nm_NReq       = nr;
        m->nm_DoNotTouch = (ULONG)n;
        n->outstanding++;
        notify_msgs++;
        nr->nr_MsgCount++;
        PutMsg(nr->nr_stuff.nr_Msg.nr_Port, &m->nm_ExecMessage);
    }
    plog("   notify -> %s", n->path);
}

/* messaggi di notifica restituiti dall'applicazione */
static void notify_replies(void)
{
    struct NotifyMessage *m;
    if (!notify_port) return;
    while ((m = (struct NotifyMessage *)GetMsg(notify_port))) {
        struct NNotify *n = (struct NNotify *)m->nm_DoNotTouch;
        FreeVec(m);
        n->outstanding--;
        notify_msgs--;
        if (!n->removed) {
            if (n->nr->nr_MsgCount) n->nr->nr_MsgCount--;
            if (n->again && n->outstanding == 0) { n->again = FALSE; notify_fire(n); }
        }
        notify_free_if_done(n);
    }
}

static void notify_set(struct NNotify *n, int err, const struct nfs_stat_64 *st)
{
    n->exists = !err;
    n->mt = st ? st->nfs_mtime : 0;   n->mtns = st ? st->nfs_mtime_nsec : 0;
    n->ct = st ? st->nfs_ctime : 0;   n->ctns = st ? st->nfs_ctime_nsec : 0;
    n->size = st ? st->nfs_size : 0;
    n->known = TRUE;
}

static void cb_notify_poll(int err, struct nfs_context *nc, void *data, void *priv)
{
    struct NNotify *n = priv;
    struct nfs_stat_64 *st = err ? NULL : data;
    (void)nc;
    n->polling = FALSE;
    if (n->removed) { notify_free_if_done(n); return; }
    if (err && err != -2) return;               /* errore di rete: riprova dopo */
    if (!n->known) { notify_set(n, err, st); return; }
    if (n->exists != !err ||
        (st && (st->nfs_mtime != n->mt || st->nfs_mtime_nsec != n->mtns ||
                st->nfs_ctime != n->ct || st->nfs_ctime_nsec != n->ctns ||
                st->nfs_size != n->size))) {
        notify_set(n, err, st);
        notify_fire(n);
    }
}

static void notify_poll_all(void)
{
    struct NNotify *n;
    if (mstate != MS_READY || !nfs) return;
    for (n = notifies; n; n = n->next)
        if (!n->polling && !n->removed) {
            n->polling = TRUE;
            if (nfs_stat64_async(nfs, n->path, cb_notify_poll, n)) n->polling = FALSE;
        }
}

/* L'handler ha modificato path: avvisa subito chi sorveglia path o il suo
 * genitore (finestre Workbench/DOpus aggiornate senza aspettare il poll). */
static void notify_touch(const char *path)
{
    struct NNotify *n;
    char *par = parent_path(path);
    for (n = notifies; n; n = n->next)
        if (!n->removed && (!strcmp(n->path, path) || (par && !strcmp(n->path, par))))
            /* Il riferimento NON si azzera: se nel frattempo arriva anche una
             * modifica esterna, il prossimo poll la vede comunque (al prezzo
             * di una notifica doppia, innocua). */
            notify_fire(n);
    free(par);
}

/* --- ADD_NOTIFY: risolve il percorso (case-insensitive) e registra --- */
static void cb_notify_add(int err, struct nfs_context *nc, void *data, void *priv)
{
    struct Req *r = priv;
    struct NotifyRequest *nr = (struct NotifyRequest *)r->pkt->dp_Arg1;
    struct NNotify *n;
    (void)nc;
    if (err && err != -2) { end_req(r, DOSFALSE, map_err(err)); return; }
    n = calloc(1, sizeof(*n));
    if (!n) { end_req(r, DOSFALSE, ERROR_NO_FREE_STORE); return; }
    n->nr   = nr;
    n->path = r->path; r->path = NULL;      /* nome reale se trovato */
    notify_set(n, err, err ? NULL : (struct nfs_stat_64 *)data);
    n->next = notifies; notifies = n; numnotifies++;
    plog("   notify aggiunto: %s", n->path);
    end_req(r, DOSTRUE, 0);
    if (nr->nr_Flags & NRF_NOTIFY_INITIAL) notify_fire(n);
}

static void start_notify_add(struct DosPacket *pkt)
{
    struct NotifyRequest *nr = (struct NotifyRequest *)pkt->dp_Arg1;
    const char *full = nr ? (const char *)(nr->nr_FullName ? nr->nr_FullName : nr->nr_Name) : NULL;
    LONG err = 0;
    struct Req *r;
    char *path;

    if (!full) { reply(pkt, DOSFALSE, ERROR_REQUIRED_ARG_MISSING); return; }
    path = resolve_path(NULL, full, &err);
    if (!path) { reply(pkt, DOSFALSE, err); return; }
    r = new_req(pkt);
    if (!r) { free(path); return; }
    r->path = path;
    if (ci_stat(r, cb_notify_add)) end_req(r, DOSFALSE, ERROR_NO_DISK);
}

static void notify_remove(struct NotifyRequest *nr)
{
    struct NNotify **pp, *n;
    for (pp = &notifies; (n = *pp); pp = &n->next)
        if (n->nr == nr) {
            *pp = n->next;
            numnotifies--;
            n->removed = TRUE;
            plog("   notify rimosso: %s", n->path);
            notify_free_if_done(n);
            return;
        }
}

/* Connessione persa: si distrugge il contesto (le richieste in volo falliscono
 * con ERROR_NO_DISK -> requester Riprova/Annulla), si dimenticano i file
 * handle aperti (riaperti per percorso al prossimo uso) e si torna a MS_IDLE:
 * il prossimo pacchetto rimonta. Da chiamare SOLO dal loop principale. */
static void connection_lost(const char *why)
{
    struct NFile *f;
    struct NNotify *nn;
    plog("!! connessione persa (%s): reset, rimount al prossimo accesso", why);
    if (nfs) { nfs_destroy_context(nfs); nfs = NULL; }
    mstate = MS_IDLE;
    nfs_reset = FALSE;
    for (f = files; f; f = f->next) { f->fh = NULL; f->dirty = FALSE; }
    for (nn = notifies; nn; nn = nn->next) nn->polling = FALSE;
}

/* ------------------------------------------------------------------------ */
/* Scrittura                                                                */
/* ------------------------------------------------------------------------ */

static void cb_simple(int err, struct nfs_context *n, void *data, void *priv)
{
    struct Req *r = priv;
    (void)n; (void)data;
    if (!err && r->path) notify_touch(r->path);     /* chmod, utimes, link */
    end_req(r, err ? DOSFALSE : DOSTRUE, err ? map_err_op(err, ERROR_WRITE_PROTECTED) : 0);
}

static void cb_delete_done(int err, struct nfs_context *n, void *data, void *priv)
{
    struct Req *r = priv;
    (void)n; (void)data;
    if (!err) { nc_drop_tree(r->path); nc_note(r->path, FALSE); notify_touch(r->path); }
    end_req(r, err ? DOSFALSE : DOSTRUE, err ? map_err_op(err, ERROR_DELETE_PROTECTED) : 0);
}

/* --- SET_FILE_SIZE --- */
static void cb_truncate(int err, struct nfs_context *n, void *data, void *priv)
{
    struct Req *r = priv;
    (void)n; (void)data;
    if (err) { end_req(r, -1, map_err(err)); return; }
    r->file->st.nfs_size = (uint64_t)r->size64;
    r->file->modified = TRUE;
    if (r->file->pos > r->file->st.nfs_size) r->file->pos = r->file->st.nfs_size;
    r->file->dirty = TRUE;
    /* pacchetto a 32 bit: size64 e' gia' stato limitato a 0x7FFFFFFF;
     * DosPacket64 (is64): end_req risponde -1LL */
    end_req(r, (LONG)r->size64, 0);
}

/* --- END (Close): close di libnfs fa anche il COMMIT --- */
static void cb_end(int err, struct nfs_context *n, void *data, void *priv)
{
    struct Req *r = priv;
    (void)n; (void)data;
    if (err) plog("!! close/commit %s: %s", r->file->path, mini_errstr(-err));
    if (r->file->modified) notify_touch(r->file->path);
    free_file(r->file);
    end_req(r, err ? DOSFALSE : DOSTRUE, err ? map_err(err) : 0);
}

/* --- FLUSH: COMMIT di tutti i file modificati --- */
static void cb_flush(int err, struct nfs_context *n, void *data, void *priv)
{
    struct Req *r = priv;
    (void)n; (void)data;
    if (err && !r->done) r->done = map_err(err);
    if (--r->ival == 0) end_req(r, r->done ? DOSFALSE : DOSTRUE, r->done);
}

/* --- DELETE_OBJECT --- */
static void cb_del_stat(int err, struct nfs_context *n, void *data, void *priv)
{
    struct Req *r = priv;
    (void)n;
    if (err) { end_req(r, DOSFALSE, map_err(err)); return; }
    if (!strcmp(r->path, "/")) { end_req(r, DOSFALSE, ERROR_OBJECT_WRONG_TYPE); return; }
    if (path_in_use(r->path)) { end_req(r, DOSFALSE, ERROR_OBJECT_IN_USE); return; }
    plog("   delete %s", r->path);
    if ((stat_type(data) == NF3DIR ? nfs_rmdir_async(nfs, r->path, cb_delete_done, r)
                                   : nfs_unlink_async(nfs, r->path, cb_delete_done, r)))
        end_req(r, DOSFALSE, ERROR_NO_DISK);
}

/* --- CREATE_DIR --- */
static void cb_lock(int err, struct nfs_context *n, void *data, void *priv);

static void cb_mkdir_done(int err, struct nfs_context *n, void *data, void *priv)
{
    struct Req *r = priv;
    (void)n; (void)data;
    if (err) { end_req(r, DOSFALSE, map_err(err)); return; }
    nc_note(r->path, TRUE);
    notify_touch(r->path);
    if (nfs_stat64_async(nfs, r->path, cb_lock, r))
        end_req(r, DOSFALSE, ERROR_NO_DISK);
}

static void cb_mkdir_stat(int err, struct nfs_context *n, void *data, void *priv)
{
    struct Req *r = priv;
    char *np;
    (void)n; (void)data;
    if (!err) { end_req(r, DOSFALSE, ERROR_OBJECT_EXISTS); return; }
    if (err != -2) { end_req(r, DOSFALSE, map_err(err)); return; }
    np = target_path(r);
    if (!np) { end_req(r, DOSFALSE, ERROR_NO_FREE_STORE); return; }
    free(r->path); r->path = np;
    plog("   mkdir %s", r->path);
    if (nfs_mkdir2_async(nfs, r->path, 0777 & ~opt_umask, cb_mkdir_done, r))
        end_req(r, DOSFALSE, ERROR_NO_DISK);
}

/* --- RENAME_OBJECT: risolve sorgente, poi destinazione --- */
static void cb_rename_done(int err, struct nfs_context *n, void *data, void *priv)
{
    struct Req *r = priv;
    (void)n; (void)data;
    if (err) { end_req(r, DOSFALSE, map_err(err)); return; }
    nc_drop_tree(r->src);
    nc_note(r->src, FALSE);
    nc_note(r->path, TRUE);
    notify_touch(r->src);
    notify_touch(r->path);
    rename_paths(r->src, r->path);
    end_req(r, DOSTRUE, 0);
}

static void cb_rename_dst(int err, struct nfs_context *n, void *data, void *priv)
{
    struct Req *r = priv;
    char *dst;
    (void)n; (void)data;
    if (!err) {
        /* la destinazione esiste: ammesso solo se e' lo stesso oggetto
         * (cambio di maiuscole/minuscole): nuovo nome nella stessa dir */
        char *par;
        if (strcmp(r->path, r->src)) { end_req(r, DOSFALSE, ERROR_OBJECT_EXISTS); return; }
        par = parent_path(r->src);
        dst = par ? join_path(par, r->dstname) : NULL;
        free(par);
    } else if (err == -2) {
        dst = target_path(r);           /* nome richiesto nel genitore reale */
    } else {
        end_req(r, DOSFALSE, map_err(err));
        return;
    }
    if (!dst) { end_req(r, DOSFALSE, ERROR_NO_FREE_STORE); return; }
    free(r->path); r->path = dst;
    plog("   rename %s -> %s", r->src, r->path);
    if (nfs_rename_async(nfs, r->src, r->path, cb_rename_done, r))
        end_req(r, DOSFALSE, ERROR_NO_DISK);
}

static void cb_rename_src(int err, struct nfs_context *n, void *data, void *priv)
{
    struct Req *r = priv;
    (void)n; (void)data;
    if (err) { end_req(r, DOSFALSE, map_err(err)); return; }
    if (!strcmp(r->path, "/")) { end_req(r, DOSFALSE, ERROR_OBJECT_WRONG_TYPE); return; }
    r->src  = r->path;                  /* sorgente reale */
    r->path = r->dst;                   /* ora si risolve la destinazione */
    r->dst  = NULL;
    free(r->ci_parent); r->ci_parent = NULL;
    if (ci_stat(r, cb_rename_dst)) end_req(r, DOSFALSE, ERROR_NO_DISK);
}

/* --- SET_PROTECT: RWED (attivi bassi) -> bit dell'owner --- */
static void cb_protect_stat(int err, struct nfs_context *n, void *data, void *priv)
{
    struct Req *r = priv;
    struct nfs_stat_64 *st = data;
    int mode;
    (void)n;
    if (err) { end_req(r, DOSFALSE, map_err(err)); return; }
    mode = (int)(st->nfs_mode & 07077);
    if (!(r->ival & FIBF_READ))    mode |= 0400;
    if (!(r->ival & FIBF_WRITE))   mode |= 0200;
    if (!(r->ival & FIBF_EXECUTE)) mode |= 0100;
    if (mode == (int)(st->nfs_mode & 07777)) {      /* niente da cambiare */
        end_req(r, DOSTRUE, 0);
        return;
    }
    plog("   chmod %s %o", r->path, mode);
    if (nfs_chmod_async(nfs, r->path, mode, cb_simple, r))
        end_req(r, DOSFALSE, ERROR_NO_DISK);
}

/* --- SET_DATE --- */
static void cb_date_stat(int err, struct nfs_context *n, void *data, void *priv)
{
    struct Req *r = priv;
    struct timeval tv[2];
    (void)n; (void)data;
    if (err) { end_req(r, DOSFALSE, map_err(err)); return; }
    tv[0].tv_sec  = r->ds.ds_Days * 86400 + r->ds.ds_Minute * 60 +
                    r->ds.ds_Tick / TICKS_PER_SECOND + AMIGA_EPOCH_OFFSET +
                    gmt_offset_secs;
    tv[0].tv_usec = 0;
    tv[1] = tv[0];
    if (nfs_utimes_async(nfs, r->path, tv, cb_simple, r))
        end_req(r, DOSFALSE, ERROR_NO_DISK);
}

/* --- MAKE_LINK --- */
static void start_link(struct DosPacket *pkt, char *path)
{
    struct Req *r = new_req(pkt);
    if (!r) { free(path); return; }
    r->path = path;
    if (pkt->dp_Arg4 == LINK_HARD) {
        struct NLock *t = LOCK(pkt->dp_Arg3);
        if (!t) { end_req(r, DOSFALSE, ERROR_OBJECT_WRONG_TYPE); return; }
        plog("   link %s -> %s", path, t->path);
        if (nfs_link_async(nfs, t->path, path, cb_simple, r))
            end_req(r, DOSFALSE, ERROR_NO_DISK);
    } else {
        /* soft link: solo bersagli relativi senza ':' (gli assoluti Amiga
         * non hanno un equivalente sul server) */
        const char *tg = (const char *)pkt->dp_Arg3;
        char buf[MAXPATH], *o = buf;
        const char *p = tg;
        if (!tg || strchr(tg, ':')) { end_req(r, DOSFALSE, ERROR_ACTION_NOT_KNOWN); return; }
        while (*p == '/' && o + 4 < buf + sizeof(buf)) { strcpy(o, "../"); o += 3; p++; }
        if (amiga_to_utf8(p, strlen(p), o, buf + sizeof(buf) - o) < 0) {
            end_req(r, DOSFALSE, ERROR_INVALID_COMPONENT_NAME);
            return;
        }
        plog("   symlink %s -> %s", path, buf);
        if (nfs_symlink_async(nfs, buf, path, cb_simple, r))
            end_req(r, DOSFALSE, ERROR_NO_DISK);
    }
}

/* Avvia una ci_stat su (lock, nome BSTR) con la callback data */
static void start_named(struct DosPacket *pkt, BPTR lock, BSTR bname, nfs_cb cb,
                        LONG ival, BOOL create)
{
    char name[MAXPATH];
    LONG err = 0;
    struct Req *r;
    char *path;

    bstr2c(bname, name, sizeof(name));
    path = resolve_path(LOCK(lock), name, &err);
    if (!path) { reply(pkt, DOSFALSE, err); return; }
    r = new_req(pkt);
    if (!r) { free(path); return; }
    r->path = path;
    r->ival = ival;
    if (create ? ci_stat_create(r, cb) : ci_stat(r, cb)) end_req(r, DOSFALSE, ERROR_NO_DISK);
}

/* ------------------------------------------------------------------------ */
/* Mount                                                                    */
/* ------------------------------------------------------------------------ */

static void dispatch(struct DosPacket *pkt);

static LONG fail_pending(LONG err)
{
    struct Node *nd;
    LONG n = 0;
    while ((nd = RemHead(&pending))) {
        reply((struct DosPacket *)nd->ln_Name, DOSFALSE, err);
        n++;
    }
    return n;
}

static void cb_mount(int err, struct nfs_context *n, void *data, void *priv)
{
    struct Node *nd;
    (void)n; (void)priv;
    if (err) {
        plog("!! mount %s:%s fallito: %s", opt_server, opt_export,
             data ? (char *)data : "?");
        mstate = MS_IDLE;
        nfs_reset = TRUE;           /* contesto da ricreare al prossimo tentativo */
        fail_pending(ERROR_NO_DISK);
        return;
    }
    plog("   mount %s:%s OK (readmax=%lu)", opt_server, opt_export,
         (unsigned long)nfs_get_readmax(nfs));
#ifdef __amigaos4__
    {
        int fd = nfs_get_fd(nfs);
        int fl = fd >= 0 ? fcntl(fd, F_GETFL) : -1;
        plog("   socket fd=%ld flags=%lx (O_NONBLOCK=%lx: %s)", (long)fd, (long)fl,
             (long)O_NONBLOCK, fl >= 0 && (fl & O_NONBLOCK) ? "non bloccante" : "BLOCCANTE");
    }
#endif
    mstate = MS_READY;
    while ((nd = RemHead(&pending)))
        dispatch((struct DosPacket *)nd->ln_Name);
}

/* Ritorna TRUE se si puo' procedere subito; altrimenti il pacchetto e'
 * stato accodato (mount in corso) o gia' risposto con errore. */
static BOOL need_mount(struct DosPacket *pkt)
{
    if (mstate == MS_READY) return TRUE;

    AddTail(&pending, &pkt->dp_Link->mn_Node);
    if (mstate == MS_MOUNTING) return FALSE;

    if (amiga_socket_init()) {
        plog("!! bsdsocket.library non disponibile");
        fail_pending(ERROR_NO_DISK);
        return FALSE;
    }
    if (!nfs) {
        nfs = nfs_init_context();
        if (!nfs) { fail_pending(ERROR_NO_FREE_STORE); return FALSE; }
        nfs_set_version(nfs, 3);
        nfs_set_uid(nfs, opt_uid);
        nfs_set_gid(nfs, opt_gid);
        nfs_set_timeout(nfs, opt_timeout * 1000);
        nfs_set_readmax(nfs, opt_rsize);
        nfs_set_writemax(nfs, opt_wsize);
        if (opt_log) rpc_set_stats_cb(nfs_get_rpc_context(nfs), cb_rpc_stats, NULL);
    }
    plog("   mount %s:%s ...", opt_server, opt_export);
    if (nfs_mount_async(nfs, opt_server, opt_export, cb_mount, NULL)) {
        plog("!! nfs_mount_async: %s", nfs_get_error(nfs));
        /* il contesto puo' restare con un socket mezzo aperto ("Trying to
         * connect while already connected" ai tentativi successivi):
         * il prossimo mount riparte da zero */
        nfs_destroy_context(nfs);
        nfs = NULL;
        fail_pending(ERROR_NO_DISK);
        return FALSE;
    }
    mstate = MS_MOUNTING;
    mount_tick = ticks;
    return FALSE;
}

/* Mount senza risposta entro TIMEOUT secondi (server spento, IP sbagliato):
 * libnfs non limita la connessione TCP, che altrimenti aspetterebbe il
 * timeout dello stack (oltre un minuto). Si chiude il contesto e si
 * risponde ERROR_NO_DISK: il requester del DOS offre "Riprova", che rifa'
 * il mount da capo. Da chiamare SOLO dal loop principale. */
static void mount_timeout(void)
{
    plog("!! mount %s:%s: nessuna risposta in %ld s", opt_server, opt_export,
         (long)opt_timeout);
    mstate = MS_IDLE;
    if (nfs) { nfs_destroy_context(nfs); nfs = NULL; }
    nfs_reset = FALSE;              /* cb_mount puo' averlo rimesso */
    fail_pending(ERROR_NO_DISK);
}

/* ------------------------------------------------------------------------ */
/* Dispatch dei pacchetti                                                   */
/* ------------------------------------------------------------------------ */

static void dispatch(struct DosPacket *pkt)
{
    char name[MAXPATH];
    LONG err = 0;
    char *path;

    log_packet(pkt);

    switch (pkt->dp_Type) {

    case ACTION_IS_FILESYSTEM:
        reply(pkt, DOSTRUE, 0);
        return;

    case ACTION_FLUSH: {
        struct NFile *f;
        struct Req *r;
        LONG n = 0;
        for (f = files; f; f = f->next) if (f->dirty && f->fh) n++;
        if (!n || mstate != MS_READY) { reply(pkt, DOSTRUE, 0); return; }
        r = new_req(pkt);
        if (!r) return;
        r->ival = n;
        for (f = files; f; f = f->next)
            if (f->dirty && f->fh) {
                f->dirty = FALSE;
                if (nfs_fsync_async(nfs, f->fh, cb_flush, r)) {
                    if (!r->done) r->done = ERROR_NO_DISK;
                    if (--r->ival == 0) { end_req(r, DOSFALSE, r->done); return; }
                }
            }
        return;
    }

    case ACTION_CURRENT_VOLUME:
        reply(pkt, MKBADDR(volnode), 0);
        return;

    case ACTION_FREE_LOCK:
        free_lock(LOCK(pkt->dp_Arg1));
        reply(pkt, DOSTRUE, 0);
        return;

    case ACTION_SAME_LOCK: {
        struct NLock *a = LOCK(pkt->dp_Arg1), *b = LOCK(pkt->dp_Arg2);
        const char *pa = a ? a->path : "/", *pb = b ? b->path : "/";
        reply(pkt, strcmp(pa, pb) ? DOSFALSE : DOSTRUE, 0);
        return;
    }

    case ACTION_CHANGE_MODE:
        reply(pkt, DOSTRUE, 0);
        return;

    case ACTION_LOCATE_OBJECT:
        if (!need_mount(pkt)) return;
        bstr2c((BSTR)pkt->dp_Arg2, name, sizeof(name));
        path = resolve_path(LOCK(pkt->dp_Arg1), name, &err);
        if (!path) { reply(pkt, DOSFALSE, err); return; }
        start_lock(pkt, path);
        return;

    case ACTION_COPY_DIR: {
        struct NLock *l = LOCK(pkt->dp_Arg1);
        if (l) {
            char *p = strdup(l->path);
            struct NLock *n = p ? new_lock(p, &l->st, SHARED_LOCK, pkt) : NULL;
            reply(pkt, n ? MKBADDR(n) : DOSFALSE, n ? 0 : ERROR_NO_FREE_STORE);
            return;
        }
        if (!need_mount(pkt)) return;
        path = strdup("/");
        if (!path) { reply(pkt, DOSFALSE, ERROR_NO_FREE_STORE); return; }
        start_lock(pkt, path);
        return;
    }

    case ACTION_PARENT: {
        struct NLock *l = LOCK(pkt->dp_Arg1);
        if (!l || !strcmp(l->path, "/")) { reply(pkt, 0, 0); return; }
        if (!need_mount(pkt)) return;
        path = parent_path(l->path);
        if (!path) { reply(pkt, DOSFALSE, ERROR_NO_FREE_STORE); return; }
        start_lock(pkt, path);
        return;
    }

    case ACTION_EXAMINE_OBJECT: {
        struct NLock *l = LOCK(pkt->dp_Arg1);
        struct Req *r;
        if (l) {
            if (l->dir) { nfs_closedir(nfs, l->dir); l->dir = NULL; }
            if (!need_mount(pkt)) return;
            /* attributi sempre riletti: dimensioni/date cambiano con le scritture */
            r = new_req(pkt);
            if (!r) return;
            r->lock = l;
            r->fib  = pkt->dp_Arg2;
            if (nfs_stat64_async(nfs, l->path, cb_examine_lock, r))
                end_req(r, DOSFALSE, ERROR_NO_DISK);
            return;
        }
        /* lock nullo = root: serve uno stat */
        if (!need_mount(pkt)) return;
        r = new_req(pkt);
        if (!r) return;
        r->fib = pkt->dp_Arg2;
        if (nfs_stat64_async(nfs, "/", cb_examine, r))
            end_req(r, DOSFALSE, ERROR_NO_DISK);
        return;
    }

    case ACTION_EXAMINE_NEXT: {
        struct NLock *l = LOCK(pkt->dp_Arg1);
        struct Req *r;
        if (!l) { reply(pkt, DOSFALSE, ERROR_OBJECT_WRONG_TYPE); return; }
        if (stat_type(&l->st) != NF3DIR) {
            reply(pkt, DOSFALSE, ERROR_NO_MORE_ENTRIES);
            return;
        }
        if (!need_mount(pkt)) return;
        r = new_req(pkt);
        if (!r) return;
        r->lock = l;
        r->fib  = pkt->dp_Arg2;
        if (l->dir) { exnext_from_dir(r); return; }
        if (nfs_opendir_async(nfs, l->path, cb_opendir, r))
            end_req(r, DOSFALSE, ERROR_NO_DISK);
        return;
    }

    case ACTION_DISK_INFO:
    case ACTION_INFO: {
        BPTR id = pkt->dp_Type == ACTION_INFO ? pkt->dp_Arg2 : pkt->dp_Arg1;
        struct Req *r;
        if (mstate != MS_READY) {
            /* non montato: non forzare il mount solo per Info */
            fill_info((struct InfoData *)BADDR(id), NULL);
            reply(pkt, DOSTRUE, 0);
            return;
        }
        r = new_req(pkt);
        if (!r) return;
        r->fib = id;
        if (nfs_statvfs64_async(nfs, "/", cb_statvfs, r)) {
            fill_info((struct InfoData *)BADDR(id), NULL);
            end_req(r, DOSTRUE, 0);
        }
        return;
    }

    case ACTION_FINDINPUT: {
        struct FileHandle *fh = (struct FileHandle *)BADDR(pkt->dp_Arg1);
        if (!need_mount(pkt)) return;
        bstr2c((BSTR)pkt->dp_Arg3, name, sizeof(name));
        path = resolve_path(LOCK(pkt->dp_Arg2), name, &err);
        if (!path) { reply(pkt, DOSFALSE, err); return; }
        start_open(pkt, fh, path, 0, OPEN_OLD);
        return;
    }

    case ACTION_FH_FROM_LOCK: {
        struct FileHandle *fh = (struct FileHandle *)BADDR(pkt->dp_Arg1);
        struct NLock *l = LOCK(pkt->dp_Arg2);
        if (!l) { reply(pkt, DOSFALSE, ERROR_OBJECT_WRONG_TYPE); return; }
        if (!need_mount(pkt)) return;
        path = strdup(l->path);
        if (!path) { reply(pkt, DOSFALSE, ERROR_NO_FREE_STORE); return; }
        start_open(pkt, fh, path, pkt->dp_Arg2, OPEN_OLD);
        return;
    }

    case ACTION_READ: {
        struct NFile *f = (struct NFile *)pkt->dp_Arg1;
        if (!f || pkt->dp_Arg3 <= 0) { reply(pkt, 0, 0); return; }
        if (!need_mount(pkt)) return;
        if (!ensure_open(pkt, f)) return;
        start_io(pkt, f, FALSE);
        return;
    }

    case ACTION_SEEK: {
        struct NFile *f = (struct NFile *)pkt->dp_Arg1;
        int64_t newpos, old;
        if (!f) { reply(pkt, -1, ERROR_SEEK_ERROR); return; }
        old = f->pos;
        switch (pkt->dp_Arg3) {
        case OFFSET_BEGINNING: newpos = pkt->dp_Arg2; break;
        case OFFSET_CURRENT:   newpos = old + pkt->dp_Arg2; break;
        case OFFSET_END:       newpos = (int64_t)f->st.nfs_size + pkt->dp_Arg2; break;
        default: reply(pkt, -1, ERROR_SEEK_ERROR); return;
        }
        if (newpos < 0 || newpos > (int64_t)f->st.nfs_size) {
            reply(pkt, -1, ERROR_SEEK_ERROR);
            return;
        }
        f->pos = newpos;
        reply(pkt, old > 0x7FFFFFFF ? 0x7FFFFFFF : (LONG)old, 0);
        return;
    }

    case ACTION_END: {
        struct NFile *f = (struct NFile *)pkt->dp_Arg1;
        struct Req *r;
        if (!f) { reply(pkt, DOSTRUE, 0); return; }
        if (!f->fh || mstate != MS_READY) { free_file(f); reply(pkt, DOSTRUE, 0); return; }
        r = new_req(pkt);
        if (!r) return;
        r->file = f;
        {
            struct nfsfh *fh = f->fh;
            f->fh = NULL;               /* free_file non deve richiudere */
            if (nfs_close_async(nfs, fh, cb_end, r)) {
                free_file(f);
                end_req(r, DOSFALSE, ERROR_NO_DISK);
            }
        }
        return;
    }

    case ACTION_EXAMINE_FH: {
        struct NFile *f = (struct NFile *)pkt->dp_Arg1;
        if (!f) { reply(pkt, DOSFALSE, ERROR_OBJECT_WRONG_TYPE); return; }
        fill_fib_stat((struct FileInfoBlock *)BADDR(pkt->dp_Arg2), f->path, &f->st);
        reply(pkt, DOSTRUE, 0);
        return;
    }

    case ACTION_COPY_DIR_FH: {
        struct NFile *f = (struct NFile *)pkt->dp_Arg1;
        char *p;
        struct NLock *n;
        if (!f) { reply(pkt, DOSFALSE, ERROR_OBJECT_WRONG_TYPE); return; }
        p = strdup(f->path);
        n = p ? new_lock(p, &f->st, SHARED_LOCK, pkt) : NULL;
        reply(pkt, n ? MKBADDR(n) : DOSFALSE, n ? 0 : ERROR_NO_FREE_STORE);
        return;
    }

    case ACTION_PARENT_FH: {
        struct NFile *f = (struct NFile *)pkt->dp_Arg1;
        if (!f) { reply(pkt, DOSFALSE, ERROR_OBJECT_WRONG_TYPE); return; }
        if (!need_mount(pkt)) return;
        path = parent_path(f->path);
        if (!path) { reply(pkt, DOSFALSE, ERROR_NO_FREE_STORE); return; }
        start_lock(pkt, path);
        return;
    }

    case ACTION_FINDOUTPUT:
    case ACTION_FINDUPDATE: {
        struct FileHandle *fh = (struct FileHandle *)BADDR(pkt->dp_Arg1);
        if (opt_readonly) { reply(pkt, DOSFALSE, ERROR_DISK_WRITE_PROTECTED); return; }
        if (!need_mount(pkt)) return;
        bstr2c((BSTR)pkt->dp_Arg3, name, sizeof(name));
        path = resolve_path(LOCK(pkt->dp_Arg2), name, &err);
        if (!path) { reply(pkt, DOSFALSE, err); return; }
        start_open(pkt, fh, path, 0,
                   pkt->dp_Type == ACTION_FINDOUTPUT ? OPEN_NEW : OPEN_RW);
        return;
    }

    case ACTION_WRITE: {
        struct NFile *f = (struct NFile *)pkt->dp_Arg1;
        if (!f) { reply(pkt, -1, ERROR_OBJECT_WRONG_TYPE); return; }
        if (pkt->dp_Arg3 <= 0) { reply(pkt, 0, 0); return; }
        if (opt_readonly) { reply(pkt, -1, ERROR_DISK_WRITE_PROTECTED); return; }
        if (!need_mount(pkt)) return;
        if (!ensure_open(pkt, f)) return;
        start_io(pkt, f, TRUE);
        return;
    }

    case ACTION_SET_FILE_SIZE: {
        struct NFile *f = (struct NFile *)pkt->dp_Arg1;
        struct Req *r;
        int64_t ns;
        if (!f) { reply(pkt, -1, ERROR_OBJECT_WRONG_TYPE); return; }
        if (opt_readonly) { reply(pkt, -1, ERROR_DISK_WRITE_PROTECTED); return; }
        switch (pkt->dp_Arg3) {
        case OFFSET_BEGINNING: ns = pkt->dp_Arg2; break;
        case OFFSET_CURRENT:   ns = (int64_t)f->pos + pkt->dp_Arg2; break;
        case OFFSET_END:       ns = (int64_t)f->st.nfs_size + pkt->dp_Arg2; break;
        default: reply(pkt, -1, ERROR_SEEK_ERROR); return;
        }
        if (ns < 0 || ns > 0x7FFFFFFF) { reply(pkt, -1, ERROR_SEEK_ERROR); return; }
        if (!need_mount(pkt)) return;
        if (!ensure_open(pkt, f)) return;
        r = new_req(pkt);
        if (!r) return;
        r->file = f;
        r->size64 = ns;
        if (nfs_ftruncate_async(nfs, f->fh, (uint64_t)ns, cb_truncate, r))
            end_req(r, -1, ERROR_NO_DISK);
        return;
    }

#ifdef __amigaos4__
    /* --- OS4: Examine a 64 bit e file oltre i 2 GB --- */
    case ACTION_EXAMINEDATA: {
        /* dp_Arg1 e' un APTR alla struct Lock, non un BPTR */
        struct NLock *l = (struct NLock *)pkt->dp_Arg1;
        struct Req *r;
        if (!l) { reply(pkt, 0, ERROR_REQUIRED_ARG_MISSING); return; }
        if (l->dir) { nfs_closedir(nfs, l->dir); l->dir = NULL; }
        if (!need_mount(pkt)) return;
        r = new_req(pkt);
        if (!r) return;
        r->lock = l;
        if (nfs_stat64_async(nfs, l->path, cb_examinedata, r))
            end_req(r, 0, ERROR_NO_DISK);
        return;
    }

    case ACTION_EXAMINEDATA_DIR: {
        struct PRIVATE_ExamineDirContext *ctx =
            (struct PRIVATE_ExamineDirContext *)pkt->dp_Arg1;
        struct NLock *l = ctx ? (struct NLock *)ctx->ReferenceLock : NULL;
        struct Node *nd;
        struct Req *r;
        if (!l) { reply(pkt, DOSFALSE, ERROR_REQUIRED_ARG_MISSING); return; }
        /* scansione gia' finita o fallita: niente altri giri */
        if (ctx->FSPrivate[0] == 1) { reply(pkt, DOSFALSE, ERROR_NO_MORE_ENTRIES); return; }
        if (ctx->FSPrivate[0] == 2) { reply(pkt, DOSFALSE, (LONG)ctx->FSPrivate[1]); return; }
        if (stat_type(&l->st) != NF3DIR) {
            reply(pkt, DOSFALSE, ERROR_OBJECT_WRONG_TYPE);
            return;
        }
        /* nodi gia' consegnati: si restituiscono al pool del contesto */
        while ((nd = RemHead((struct List *)&ctx->StaleNodeList)))
            FreeDosObject(DOS_EXAMINEDATA, nd);
        if (!need_mount(pkt)) return;
        r = new_req(pkt);
        if (!r) return;
        r->lock = l;
        r->ctx  = ctx;
        if (nfs_opendir_async(nfs, l->path, cb_exd_opendir, r))
            end_req(r, DOSFALSE, ERROR_NO_DISK);
        return;
    }

    case ACTION_EXAMINEDATA_FH: {
        struct NFile *f = (struct NFile *)pkt->dp_Arg1;
        struct ExamineData *d;
        if (!f) { reply(pkt, 0, ERROR_REQUIRED_ARG_MISSING); return; }
        d = make_exd(f->path, &f->st);
        reply(pkt, (LONG)d, d ? 0 : ERROR_NO_FREE_STORE);
        return;
    }

    case ACTION_CHANGE_FILE_POSITION64: {
        struct DosPacket64 *p = (struct DosPacket64 *)pkt;
        struct NFile *f = (struct NFile *)p->dp_Arg1;
        int64_t newpos;
        if (!f) { reply64(pkt, 0, ERROR_OBJECT_WRONG_TYPE); return; }
        switch (p->dp_Arg3) {
        case OFFSET_BEGINNING: newpos = p->dp_Arg2; break;
        case OFFSET_CURRENT:   newpos = (int64_t)f->pos + p->dp_Arg2; break;
        case OFFSET_END:       newpos = (int64_t)f->st.nfs_size + p->dp_Arg2; break;
        default: reply64(pkt, 0, ERROR_SEEK_ERROR); return;
        }
        if (newpos < 0 || newpos > (int64_t)f->st.nfs_size) {
            reply64(pkt, 0, ERROR_SEEK_ERROR);
            return;
        }
        f->pos = newpos;
        reply64(pkt, -1LL, 0);
        return;
    }

    case ACTION_GET_FILE_POSITION64: {
        struct NFile *f = (struct NFile *)((struct DosPacket64 *)pkt)->dp_Arg1;
        if (!f) { reply64(pkt, -1LL, ERROR_OBJECT_WRONG_TYPE); return; }
        reply64(pkt, (int64_t)f->pos, 0);
        return;
    }

    case ACTION_GET_FILE_SIZE64: {
        struct NFile *f = (struct NFile *)((struct DosPacket64 *)pkt)->dp_Arg1;
        if (!f) { reply64(pkt, -1LL, ERROR_OBJECT_WRONG_TYPE); return; }
        reply64(pkt, (int64_t)f->st.nfs_size, 0);
        return;
    }

    case ACTION_CHANGE_FILE_SIZE64: {
        struct DosPacket64 *p = (struct DosPacket64 *)pkt;
        struct NFile *f = (struct NFile *)p->dp_Arg1;
        struct Req *r;
        int64_t ns;
        if (!f) { reply64(pkt, 0, ERROR_OBJECT_WRONG_TYPE); return; }
        if (opt_readonly) { reply64(pkt, 0, ERROR_DISK_WRITE_PROTECTED); return; }
        switch (p->dp_Arg3) {
        case OFFSET_BEGINNING: ns = p->dp_Arg2; break;
        case OFFSET_CURRENT:   ns = (int64_t)f->pos + p->dp_Arg2; break;
        case OFFSET_END:       ns = (int64_t)f->st.nfs_size + p->dp_Arg2; break;
        default: reply64(pkt, 0, ERROR_SEEK_ERROR); return;
        }
        if (ns < 0) { reply64(pkt, 0, ERROR_SEEK_ERROR); return; }
        if (!need_mount(pkt)) return;
        if (!ensure_open(pkt, f)) return;
        r = new_req(pkt);
        if (!r) return;
        r->is64   = TRUE;
        r->file   = f;
        r->size64 = ns;
        if (nfs_ftruncate_async(nfs, f->fh, (uint64_t)ns, cb_truncate, r))
            end_req(r, -1, ERROR_NO_DISK);
        return;
    }
#endif

    case ACTION_DELETE_OBJECT:
        if (opt_readonly) { reply(pkt, DOSFALSE, ERROR_DISK_WRITE_PROTECTED); return; }
        if (!need_mount(pkt)) return;
        start_named(pkt, pkt->dp_Arg1, (BSTR)pkt->dp_Arg2, cb_del_stat, 0, FALSE);
        return;

    case ACTION_CREATE_DIR:
        if (opt_readonly) { reply(pkt, DOSFALSE, ERROR_DISK_WRITE_PROTECTED); return; }
        if (!need_mount(pkt)) return;
        start_named(pkt, pkt->dp_Arg1, (BSTR)pkt->dp_Arg2, cb_mkdir_stat, 0, TRUE);
        return;

    case ACTION_SET_PROTECT:
        if (opt_readonly) { reply(pkt, DOSFALSE, ERROR_DISK_WRITE_PROTECTED); return; }
        if (!need_mount(pkt)) return;
        start_named(pkt, pkt->dp_Arg2, (BSTR)pkt->dp_Arg3, cb_protect_stat, pkt->dp_Arg4, FALSE);
        return;

    case ACTION_SET_DATE: {
        struct Req *r;
        char *p2;
        if (opt_readonly) { reply(pkt, DOSFALSE, ERROR_DISK_WRITE_PROTECTED); return; }
        if (!need_mount(pkt)) return;
        bstr2c((BSTR)pkt->dp_Arg3, name, sizeof(name));
        p2 = resolve_path(LOCK(pkt->dp_Arg2), name, &err);
        if (!p2) { reply(pkt, DOSFALSE, err); return; }
        r = new_req(pkt);
        if (!r) { free(p2); return; }
        r->path = p2;
        r->ds = *(struct DateStamp *)pkt->dp_Arg4;
        if (ci_stat(r, cb_date_stat)) end_req(r, DOSFALSE, ERROR_NO_DISK);
        return;
    }

    case ACTION_SET_COMMENT:            /* filenote scartate (decisione di progetto) */
        reply(pkt, opt_readonly ? DOSFALSE : DOSTRUE,
              opt_readonly ? ERROR_DISK_WRITE_PROTECTED : 0);
        return;

    case ACTION_RENAME_OBJECT: {
        char name2[MAXPATH], *src, *dst;
        struct Req *r;
        if (opt_readonly) { reply(pkt, DOSFALSE, ERROR_DISK_WRITE_PROTECTED); return; }
        if (!need_mount(pkt)) return;
        bstr2c((BSTR)pkt->dp_Arg2, name, sizeof(name));
        bstr2c((BSTR)pkt->dp_Arg4, name2, sizeof(name2));
        src = resolve_path(LOCK(pkt->dp_Arg1), name, &err);
        dst = src ? resolve_path(LOCK(pkt->dp_Arg3), name2, &err) : NULL;
        if (!src || !dst) { free(src); reply(pkt, DOSFALSE, err); return; }
        r = new_req(pkt);
        if (!r) { free(src); free(dst); return; }
        r->path    = src;
        r->dst     = dst;
        r->dstname = strdup(base_name(dst));
        if (!r->dstname) { end_req(r, DOSFALSE, ERROR_NO_FREE_STORE); return; }
        if (ci_stat(r, cb_rename_src)) end_req(r, DOSFALSE, ERROR_NO_DISK);
        return;
    }

    case ACTION_MAKE_LINK:
        if (opt_readonly) { reply(pkt, DOSFALSE, ERROR_DISK_WRITE_PROTECTED); return; }
        if (!need_mount(pkt)) return;
        bstr2c((BSTR)pkt->dp_Arg2, name, sizeof(name));
        path = resolve_path(LOCK(pkt->dp_Arg1), name, &err);
        if (!path) { reply(pkt, DOSFALSE, err); return; }
        start_link(pkt, path);
        return;

    case ACTION_RENAME_DISK:
        reply(pkt, DOSFALSE, ERROR_ACTION_NOT_KNOWN);
        return;

    case ACTION_ADD_NOTIFY:
        if (!opt_notifypoll) { reply(pkt, DOSFALSE, ERROR_ACTION_NOT_KNOWN); return; }
        if (!need_mount(pkt)) return;
        start_notify_add(pkt);
        return;

    case ACTION_REMOVE_NOTIFY:
        notify_remove((struct NotifyRequest *)pkt->dp_Arg1);
        reply(pkt, DOSTRUE, 0);
        return;

    default:
        reply(pkt, DOSFALSE, ERROR_ACTION_NOT_KNOWN);
        return;
    }
}

/* ------------------------------------------------------------------------ */
/* Volume, startup, orfano                                                  */
/* ------------------------------------------------------------------------ */

static BOOL add_volume(void)
{
    volnode = MakeDosEntry((STRPTR)opt_volume, DLT_VOLUME);
    if (!volnode) return FALSE;
    volnode->dol_Task = pktport;
    DateStamp(&volnode->dol_misc.dol_volume.dol_VolumeDate);
    volnode->dol_misc.dol_volume.dol_DiskType = ID_DOS_DISK;
    if (!plat_volume_add(volnode)) {
        FreeDosEntry(volnode);
        volnode = NULL;
        return FALSE;
    }
    return TRUE;
}

static void rem_volume(void)
{
    if (!volnode) return;
    plat_volume_remove(volnode);
    volnode = NULL;
    /* "disco tolto": senza l'evento il Workbench tiene l'icona di NAS e
     * continua a cercarla (requester "inserire il volume NAS") */
    plat_disk_event(FALSE);
}

static void check_orphan(void)
{
    struct DosList *dl = AttemptLockDosList(LDF_DEVICES | LDF_READ);
    BOOL found = FALSE;
    if ((ULONG)dl <= 1) return;
    while ((dl = NextDosEntry(dl, LDF_DEVICES)))
        if ((struct DeviceNode *)dl == devnode) { found = TRUE; break; }
    UnLockDosList(LDF_DEVICES | LDF_READ);

    if (!found && devnode_seen) {
        plog("!! orfano: DeviceNode %s: rimosso senza ACTION_DIE (locks=%ld files=%ld)",
             devname, numlocks, numfiles);
        devnode_seen = FALSE;
    }
    if (!found && !numlocks && !numfiles && !numnotifies && !notify_msgs) orphan_exit = TRUE;
}

static char *dupstr(const char *s) { return s ? strdup(s) : NULL; }

/* Opzioni della riga Startup. Le stesse si possono dare come tooltype
 * nell'icona della mountlist: un tooltype attivo prevale sulla Startup. */
#define OPT_TEMPLATE "SERVER/A,EXPORT/A,VOLUME/K,UID/K/N,GID/K/N," \
                     "TIMEOUT/K/N,RSIZE/K/N,CHARSET/K,LOG/K,CASE/K," \
                     "WSIZE/K/N,SYNC/K,UMASK/K,READONLY/S,PAR/K/N,NOTIFYPOLL/K/N"
#define OPT_COUNT    16

static const char *icon_path;          /* icona letta (per il log) */
static char        icon_args[512];     /* opzioni prese dall'icona */
static BOOL        icon_bad;           /* tooltype non validi, ignorati */
static BOOL        icon_format;        /* icona non classica (PNG): ignorata */

static int upc(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

static BOOL streqi(const char *a, const char *b)
{
    while (*a && upc((UBYTE)*a) == upc((UBYTE)*b)) a++, b++;
    return upc((UBYTE)*a) == upc((UBYTE)*b);     /* vero solo se finite insieme */
}

/* Cerca la chiave key[0..klen) nel template. Ritorna 0 se non e' un'opzione
 * dell'handler, 'S' per uno switch, 'K' per le altre; in *idx la posizione
 * nel template (indice in a[] di ReadArgs). */
static int opt_kind(const char *key, ULONG klen, int *idx)
{
    const char *t = OPT_TEMPLATE;
    *idx = -1;
    while (*t) {
        const char *name = t;
        ULONG n = 0, i;
        int kind = 'K';
        while (name[n] && name[n] != '/' && name[n] != ',') n++;
        for (t = name + n; *t && *t != ','; t++)
            if (t[0] == '/' && t[1] == 'S') kind = 'S';
        if (*t == ',') t++;
        ++*idx;
        if (n != klen) continue;
        for (i = 0; i < n && upc((UBYTE)key[i]) == upc((UBYTE)name[i]); i++) ;
        if (i == n) return kind;
    }
    return 0;
}

/* Tooltype (righe separate da '\n') -> riga per ReadArgs con le sole
 * opzioni dell'handler: le parole chiave di Mount (ACTIVATE, STACKSIZE...)
 * e le altre si ignorano. Virgolette solo per i valori con spazi. Un valore
 * vuoto ("LOG=") si ignora. Uno switch con "=NO" o "=0" non va nella riga:
 * off[indice] = TRUE, e parse_startup lo spegne anche se la Startup lo ha. */
static void icon_to_args(const char *tts, char *out, ULONG size, BOOL *off)
{
    ULONG pos = 0;
    out[0] = 0;
    while (*tts) {
        const char *line = tts, *eq;
        ULONG len = 0, klen;
        int kind, idx;
        while (line[len] && line[len] != '\n') len++;
        tts = line + len + (line[len] ? 1 : 0);
        for (klen = 0; klen < len && line[klen] != '=' && line[klen] != ' '; klen++) ;
        if (!(kind = opt_kind(line, klen, &idx))) continue;
        eq = (klen < len && line[klen] == '=') ? line + klen + 1 : NULL;
        if (kind == 'S') {
            /* switch: "READONLY" o "READONLY=YES" acceso; "=NO" o "=0" spento */
            if (eq && (upc((UBYTE)*eq) == 'N' || *eq == '0')) {
                if (idx >= 0 && idx < OPT_COUNT) off[idx] = TRUE;
                continue;
            }
            if (pos + klen + 2 >= size) break;
            memcpy(out + pos, line, klen);
            pos += klen;
        } else {
            ULONG i, n = 0, vlen = len - (eq ? (ULONG)(eq - line) : len);
            BOOL  quote = FALSE;
            if (!eq) continue;                  /* opzione senza valore */
            for (i = 0; i < vlen; i++) if (eq[i] != '"') n++;
            if (!n) continue;                   /* "LOG=" o 'LOG=""': vuoto */
            if (pos + klen + vlen + 5 >= size) break;
            for (i = 0; i < vlen; i++) if (eq[i] == ' ') quote = TRUE;
            memcpy(out + pos, line, klen);
            pos += klen;
            out[pos++] = '=';
            if (quote) out[pos++] = '"';
            for (i = 0; i < vlen; i++) if (eq[i] != '"') out[pos++] = eq[i];
            if (quote) out[pos++] = '"';
        }
        out[pos++] = ' ';
        out[pos] = 0;
    }
}

#ifdef __amigaos4__
/* --- Tooltype dell'icona della mountlist (solo OS4) ----------------------
 * Su OS3 l'handler non legge l'icona: un Open() dal processo dell'handler
 * all'avvio (con il nome DEVS:... o relativo alla radice di boot, prima o
 * dopo la risposta a Mount) mandava in crash il sistema (Guru 80000006 su
 * Vampire, Amiberry bloccato) mentre Mount non tornava. Su OS4 la lettura
 * avviene dopo che plat_start ha risposto e non ha dato questi problemi.
 * Il file .info si legge direttamente con il DOS, senza icon.library: su
 * OS4 GetDiskObject() chiamata dal processo dell'handler faceva andare in
 * crash il Workbench un minuto dopo. Formato: struct DiskObject (78 byte),
 * DrawerData (56), Image normale e selezionata (20 + dati), DefaultTool,
 * ToolTypes (vedi anche tools/icontool.py). Interi big-endian come la CPU. */

static ULONG be16(const UBYTE *p) { return ((ULONG)p[0] << 8) | p[1]; }
static ULONG be32(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3];
}

static BOOL fread_all(BPTR fh, void *buf, LONG n) { return Read(fh, buf, n) == n; }
static BOOL fskip(BPTR fh, LONG n) { return n <= 0 || Seek(fh, n, OFFSET_CURRENT) != -1; }

static BOOL skip_image(BPTR fh)
{
    UBYTE im[20];
    if (!fread_all(fh, im, 20)) return FALSE;
    if (!be32(im + 10)) return TRUE;                     /* ImageData == NULL */
    return fskip(fh, (LONG)(((be16(im + 4) + 15) >> 4) * 2 * be16(im + 6) * be16(im + 8)));
}

/* Tooltype attivi (non vuoti, non tra parentesi) di <path>.info in buf,
 * separati da '\n'. Ritorna ICON_NONE se il file non c'e' o non si legge,
 * ICON_OK, o ICON_FORMAT se c'e' ma non e' un'icona classica (DiskObject):
 * le icone PNG di OS4 tengono i tooltype altrove e non si leggono. */
#define ICON_NONE   0
#define ICON_OK     1
#define ICON_FORMAT 2
static int read_icon_tooltypes(const char *path, char *buf, ULONG size)
{
    char  name[140], line[256];
    UBYTE h[78], w[4];
    BPTR  fh;
    ULONG n = 0, count, i, len;
    int   ret = ICON_FORMAT;          /* da qui il file c'e' */

    buf[0] = 0;
    snprintf(name, sizeof(name), "%s.info", path);
    if (!(fh = Open((STRPTR)name, MODE_OLDFILE))) return ICON_NONE;
    if (!fread_all(fh, h, 78) || be16(h) != 0xE310 || be16(h + 2) != 1) goto out;
    if (be32(h + 66) && !fskip(fh, 56)) goto out;                     /* DrawerData */
    if (be32(h + 22) && !skip_image(fh)) goto out;                    /* GadgetRender */
    if (be32(h + 26) && (be16(h + 16) & 0x0002) && !skip_image(fh)) goto out;
    if (be32(h + 50)) {                                               /* DefaultTool */
        if (!fread_all(fh, w, 4) || !fskip(fh, (LONG)be32(w))) goto out;
    }
    ret = ICON_OK;
    if (!be32(h + 54)) goto out;                                      /* niente tooltype */
    if (!fread_all(fh, w, 4)) goto out;
    count = be32(w) / 4;
    for (i = 1; i < count && i < 256; i++) {
        if (!fread_all(fh, w, 4) || (len = be32(w)) > 4096) break;
        if (len > sizeof(line)) {                    /* troppo lunga (NewIcons) */
            if (!fskip(fh, (LONG)len)) break;
            continue;
        }
        if (!fread_all(fh, line, (LONG)len)) break;
        line[len ? len - 1 : 0] = 0;
        len = strlen(line);
        if (!len || line[0] == '(' || n + len + 2 > size) continue;
        memcpy(buf + n, line, len);
        n += len;
        buf[n++] = '\n';
        buf[n] = 0;
    }
out:
    Close(fh);
    if (ret != ICON_OK) buf[0] = 0;
    return ret;
}

/* Cerca <device>.info in DEVS:DOSDrivers/ e poi in SYS:Storage/DOSDrivers/.
 * Ritorna il percorso (senza .info) o NULL. La prima icona trovata vale:
 * se non e' un'icona classica, icon_format = TRUE e niente tooltype. */
static const char *find_icon_tooltypes(const char *dev, char *buf, ULONG size)
{
    static const char *const dirs[] = { "DEVS:DOSDrivers/", "SYS:Storage/DOSDrivers/" };
    static char path[128];
    ULONG i;
    for (i = 0; i < 2; i++) {
        int r;
        snprintf(path, sizeof(path), "%s%s", dirs[i], dev);
        r = read_icon_tooltypes(path, buf, size);
        if (r == ICON_FORMAT) icon_format = TRUE;
        if (r != ICON_NONE) return path;
    }
    buf[0] = 0;
    return NULL;
}
#endif /* __amigaos4__ */

/* ReadArgs di una riga (terminata con '\n' come vuole ReadArgs) */
static struct RDArgs *read_args(const char *tmpl, char *src, ULONG len, LONG *a)
{
    struct RDArgs *rda = AllocDosObject(DOS_RDARGS, NULL);
    if (!rda) return NULL;
    rda->RDA_Source.CS_Buffer = (UBYTE *)src;
    rda->RDA_Source.CS_Length = len;
    rda->RDA_Source.CS_CurChr = 0;
    rda->RDA_Flags |= RDAF_NOPROMPT;
    if (!ReadArgs((STRPTR)tmpl, a, rda)) {
        FreeDosObject(DOS_RDARGS, rda);
        return NULL;
    }
    return rda;
}

static void free_args(struct RDArgs *rda)
{
    if (!rda) return;
    FreeArgs(rda);
    FreeDosObject(DOS_RDARGS, rda);
}

static BOOL parse_startup(LONG st, const char *tooltypes)
{
    char src[512], isrc[520], itmpl[sizeof(OPT_TEMPLATE)];
    ULONG i, j;
    struct RDArgs *rda = NULL, *irda = NULL;
    LONG a[OPT_COUNT], b[OPT_COUNT];
    BOOL off[OPT_COUNT];
    BOOL ok = FALSE;
    const char *t;
    char *d = itmpl;

    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));
    memset(off, 0, sizeof(off));

    /* Template senza /A, per la Startup e per i tooltype: SERVER ed EXPORT
     * possono stare nell'una o negli altri, si controllano dopo l'unione */
    for (t = OPT_TEMPLATE; *t; t++) {
        if (t[0] == '/' && t[1] == 'A') { t++; continue; }
        *d++ = *t;
    }
    *d = 0;

    /* Startup: puo' mancare se tutto sta nell'icona */
    if (st > 1024 && TypeOfMem(BADDR(st))) {
        bstr2c((BSTR)st, src, sizeof(src) - 2);
        for (i = j = 0; src[i]; i++)
            if (src[i] != '"' && (UBYTE)src[i] >= ' ') src[j++] = src[i];
        src[j++] = '\n';
        src[j] = 0;
        if (!(rda = read_args(itmpl, src, j, a))) return FALSE;
    }

    /* tooltype: ogni opzione presente prevale sulla Startup */
    if (tooltypes && *tooltypes) {
        icon_to_args(tooltypes, icon_args, sizeof(icon_args) - 2, off);
        if (icon_args[0]) {
            j = strlen(icon_args);
            memcpy(isrc, icon_args, j);
            isrc[j++] = '\n';
            isrc[j] = 0;
            if ((irda = read_args(itmpl, isrc, j, b))) {
                for (i = 0; i < OPT_COUNT; i++) if (b[i]) a[i] = b[i];
            } else {
                icon_bad = TRUE;
            }
        }
        /* switch spenti nell'icona ("READONLY=NO"), se l'icona e' valida */
        if (!icon_bad)
            for (i = 0; i < OPT_COUNT; i++) if (off[i]) a[i] = 0;
    }

    opt_server = dupstr((char *)a[0]);
    opt_export = dupstr((char *)a[1]);
    opt_volume = dupstr(a[2] ? (char *)a[2] : devname);
    if (a[3]) opt_uid     = *(LONG *)a[3];
    if (a[4]) opt_gid     = *(LONG *)a[4];
    if (a[5]) opt_timeout = *(LONG *)a[5];
    if (a[6]) opt_rsize   = *(LONG *)a[6];
    if (a[7]) opt_raw_charset = streqi((char *)a[7], "RAW");
    opt_log = dupstr((char *)a[8]);
    if (a[9]) opt_case_sensitive = streqi((char *)a[9], "SENSITIVE");
    if (a[10]) opt_wsize = *(LONG *)a[10];
    if (a[11]) opt_sync = streqi((char *)a[11], "YES");
    if (a[12]) opt_umask = strtol((char *)a[12], NULL, 8) & 0777;
    if (a[13]) opt_readonly = TRUE;
    if (opt_wsize < 1024) opt_wsize = 1024;
    if (a[14]) opt_par = *(LONG *)a[14];
    if (opt_par < 1) opt_par = 1;
    if (opt_par > 16) opt_par = 16;
    if (a[15]) opt_notifypoll = *(LONG *)a[15];
    if (opt_notifypoll < 0) opt_notifypoll = 0;
    if (opt_rsize < 1024) opt_rsize = 1024;
    if (opt_timeout < 1)  opt_timeout = 1;
    ok = opt_server && opt_export && opt_volume;
    free_args(irda);
    free_args(rda);
    return ok;
}

/* ------------------------------------------------------------------------ */
/* Smontaggio con oggetti aperti (OS4: ACTION_SHUTDOWN + ACTION_COLLECT)     */
/* ------------------------------------------------------------------------ */

/* Si possono cedere gli oggetti adesso? No con richieste in corso o in
 * attesa del mount (le callback userebbero oggetti ceduti), con file
 * scritti e non ancora committati, o con notifiche non restituite. */
static const char *shutdown_blocker(void)
{
    struct NFile *f;
    if (active_reqs) return "richieste in corso";
    if (pending.lh_Head->ln_Succ) return "pacchetti in attesa del mount";
    if (notify_msgs) return "notifiche non restituite";
    for (f = files; f; f = f->next) if (f->dirty) return "file scritti e non committati";
    return NULL;
}

/* Cede notify, file handle e lock al lockhandler del DOS (dos.dospackets:
 * ACTION_SHUTDOWN). Un oggetto ceduto non e' piu' nostro: si liberano solo i
 * dati interni, MAI la struttura pubblica. Un oggetto rifiutato (es. un
 * NotifyRequest non allocato con AllocDosObject) resta all'handler.
 * Ritorna il numero di oggetti rifiutati (0 = si puo' uscire). */
static LONG shutdown_collect(void)
{
    LONG kept = 0;
    {
        struct NNotify **pp = &notifies, *n;
        while ((n = *pp)) {
            if (plat_collect(PLAT_COLLECT_NOTIFY, n->nr)) {
                *pp = n->next;
                numnotifies--;
                n->removed = TRUE;
                notify_free_if_done(n);
            } else {
                plog("!! COLLECT notify rifiutato: %s", n->path);
                kept++;
                pp = &n->next;
            }
        }
    }
    /* un rifiuto ferma la cessione: meglio restare montati con file e lock
     * ancora nostri che a meta' (es. la directory corrente di una Shell) */
    if (kept) return kept;
    {
        struct NFile **pp = &files, *f;
        while ((f = *pp)) {
            if (f->pubfh && plat_collect(PLAT_COLLECT_FILEHANDLE, f->pubfh)) {
                *pp = f->next;
                numfiles--;
                if (f->fh && nfs) nfs_close_async(nfs, f->fh, cb_ignore, NULL);
                free(f->path);
                free(f);
            } else {
                plog("!! COLLECT file rifiutato: %s", f->path);
                kept++;
                pp = &f->next;
            }
        }
    }
    if (kept) return kept;
    {
        struct NLock **pp = &locks, *l;
        while ((l = *pp)) {
            /* dopo la cessione la struttura e' del lockhandler (che la
             * libera all'UnLock): prima si salvano i dati interni */
            struct NLock  *next = l->next;
            struct nfsdir *dir  = l->dir;
            char          *path = l->path;
            if (plat_collect(PLAT_COLLECT_LOCK, &l->fl)) {
                *pp = next;
                numlocks--;
                if (dir && nfs) nfs_closedir(nfs, dir);
                free(path);
            } else {
                plog("!! COLLECT lock rifiutato: %s", path);
                kept++;
                pp = &l->next;
            }
        }
    }
    return kept;
}

/* ------------------------------------------------------------------------ */
/* Loop principale (il punto d'ingresso sta in src/platform/<os>/)           */
/* ------------------------------------------------------------------------ */

void handler_main(const struct HandlerStartup *hs)
{
    struct Message   *msg;
    struct DosPacket *pkt, *die_pkt = NULL;
    struct MsgPort   *tport = NULL;
    timereq_t *treq = NULL;
    BOOL  timer_ok = FALSE, timer_pending = FALSE, running = TRUE;
    BOOL  startup_failed = FALSE;
    ULONG spins = 0;
    char *c;

    me = (struct Process *)FindTask(NULL);
    me->pr_WindowPtr = (APTR)-1;
    /* NewList() sta in amiga.lib, che non linkiamo */
    pending.lh_Head     = (struct Node *)&pending.lh_Tail;
    pending.lh_Tail     = NULL;
    pending.lh_TailPred = (struct Node *)&pending.lh_Head;

    if (hs) {
        /* la piattaforma ha gia' risposto al pacchetto di startup */
        pkt     = NULL;
        devnode = hs->devnode;
        pktport = hs->port;
        strncpy(devname, hs->devname, sizeof(devname) - 1);
    } else {
        WaitPort(&me->pr_MsgPort);
        msg = GetMsg(&me->pr_MsgPort);
        pkt = (struct DosPacket *)msg->mn_Node.ln_Name;
        devnode = (struct DeviceNode *)BADDR(pkt->dp_Arg3);
        bstr2c((BSTR)pkt->dp_Arg1, devname, sizeof(devname));
    }
    for (c = devname; *c; c++) if (*c == ':') *c = 0;
    if (!devname[0] && devnode) bstr2c(devnode->dn_Name, devname, sizeof(devname));

    /* OS4: tooltype dell'icona prima di creare il volume (il nome puo' venire
     * da li'); la piattaforma ha gia' risposto al pacchetto di avvio. OS3:
     * niente icona (vedi find_icon_tooltypes), si risponde a Mount solo dopo
     * aver letto le opzioni, cosi' Mount segnala una Startup sbagliata. */
    {
        static char tts[512];
        if (mini_init() || !devnode || (!pktport && !(pktport = CreateMsgPort())))
            startup_failed = TRUE;
        else {
#ifdef __amigaos4__
            if (devname[0]) icon_path = find_icon_tooltypes(devname, tts, sizeof(tts));
#endif
            if (!parse_startup((LONG)devnode->dn_Startup, tts)) startup_failed = TRUE;
        }
    }
    if (startup_failed) {
        if (pkt) {
            if (pktport) DeleteMsgPort(pktport);
            ReplyPkt(pkt, DOSFALSE, ERROR_BAD_TEMPLATE);
        } else if (pktport) {
            /* gia' montato: staccarsi dal DeviceNode e rifiutare cio' che
             * e' arrivato nel frattempo */
            Forbid();
            if (devnode) devnode->dn_Task = NULL;
            while ((msg = GetMsg(pktport)))
                ReplyPkt((struct DosPacket *)msg->mn_Node.ln_Name, DOSFALSE, ERROR_BAD_TEMPLATE);
            DeleteMsgPort(pktport);
            Permit();
        }
        mini_exit();
        return;
    }
    mini_assert_hook = assert_hook;

    if (pkt) {
        devnode->dn_Task = pktport;
        ReplyPkt(pkt, DOSTRUE, 0);
    }

    /* Il volume va creato SUBITO, prima di ogni altro I/O DOS (log,
     * locale): chi ha fatto Mount puo' usare "NAS:" appena Mount torna, e
     * se il volume non c'e' ottiene il requester "inserire il volume". Su
     * OS4 prima di qui c'e' la lettura dell'icona (vedi sopra), che non si
     * puo' spostare: VOLUME= puo' venire da li'. */
    {
        BOOL vol_ok = add_volume();
        if (vol_ok) plat_disk_event(TRUE);
        gmt_offset_secs = plat_gmt_offset();
        plog("==== %s avviato ====", HANDLER_VERSION);
        plog("   device=%s volume=%s server=%s export=%s uid=%ld gid=%ld rsize=%ld",
             devname, opt_volume, opt_server, opt_export, opt_uid, opt_gid, opt_rsize);
        if (!vol_ok) plog("!! impossibile creare il volume %s", opt_volume);
        plog("   timeout=%ld case=%s charset=%s sync=%s readonly=%s notifypoll=%ld par=%ld umask=%03lo",
             opt_timeout, opt_case_sensitive ? "SENSITIVE" : "INSENSITIVE",
             opt_raw_charset ? "RAW" : "LATIN1", opt_sync ? "YES" : "NO",
             opt_readonly ? "si" : "no", opt_notifypoll, opt_par, (unsigned long)opt_umask);
        /* per confrontare con i registri di un eventuale crash di un client */
        plog("   indirizzi: processo=%08lx porta=%08lx devnode=%08lx volnode=%08lx",
             (unsigned long)me, (unsigned long)pktport, (unsigned long)devnode,
             (unsigned long)volnode);
        if (icon_path)
            plog("   icona %s.info: %s", icon_path,
                 icon_format ? "non e' un'icona classica (PNG?), tooltype non letti" :
                 icon_bad ? "tooltype non validi, ignorati" :
                 icon_args[0] ? icon_args : "nessuna opzione dell'handler");
    }

    notify_port = CreateMsgPort();

    if ((tport = CreateMsgPort()) &&
        (treq = (timereq_t *)CreateIORequest(tport, sizeof(*treq))) &&
        OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK, (struct IORequest *)treq, 0) == 0) {
        timer_ok = TRUE;
        eclock_freq = plat_eclock_open(TR_IO(treq).io_Device);
        TR_IO(treq).io_Command = TR_ADDREQUEST;
        TR_SECS(treq) = HEARTBEAT_SECS;
        TR_MICRO(treq) = 0;
        SendIO((struct IORequest *)treq);
        timer_pending = TRUE;
    }

    while (running) {
        ULONG pktsig = 1UL << pktport->mp_SigBit;
        ULONG tsig   = timer_ok ? 1UL << tport->mp_SigBit : 0;
        ULONG nsig   = notify_port ? 1UL << notify_port->mp_SigBit : 0;
        ULONG sigs   = pktsig | tsig | nsig;
        int   fd     = (nfs && amiga_socket_ready()) ? nfs_get_fd(nfs) : -1;
        BOOL  lost   = FALSE;

        /* montati ma senza socket: libnfs non riesce a riconnettersi da sola
         * su Amiga (il "parcheggio" del fd con pipe/dup2 non esiste) */
        if (mstate == MS_READY && nfs && fd < 0) {
            connection_lost("socket chiuso");
            fd = -1;
        }

        {
            int events  = fd >= 0 ? nfs_which_events(nfs) : 0;
            int revents = plat_wait(fd, events, &sigs);
            /* Attesa a vuoto (errore o nessun evento e nessun segnale): non
             * deve succedere. Con Priority=5 un giro a vuoto toglie la CPU a
             * tutto il sistema: si registra e si cede un tick. */
            if (revents < 0 || (revents == 0 && sigs == 0)) {
                if (++spins == 1 || spins % 500 == 0)
                    plog("!! attesa a vuoto x%lu: rc=%ld errno=%ld fd=%ld events=%lx",
                         (unsigned long)spins, (long)revents, (long)errno,
                         (long)fd, (unsigned long)events);
                if (spins > 20) Delay(1);
            } else {
                spins = 0;
            }
            if (revents > 0 && nfs_service(nfs, revents) < 0) {
                plog("!! nfs_service: %s", nfs_get_error(nfs));
                if (mstate == MS_READY) lost = TRUE;
            }
        }

        if (lost) connection_lost("errore sul socket");

        if (nfs_reset && nfs) {
            nfs_destroy_context(nfs);
            nfs = NULL;
            nfs_reset = FALSE;
        }

        if (tsig && GetMsg(tport)) {
            timer_pending = FALSE;
            ticks++;
            if (nfs && fd >= 0) nfs_service(nfs, 0);     /* timeout RPC */
            if (ticks % ORPHAN_CHECK_EVERY == 0) check_orphan();
            if (mstate == MS_MOUNTING &&
                ticks - mount_tick >= (ULONG)((opt_timeout + HEARTBEAT_SECS - 1) / HEARTBEAT_SECS))
                mount_timeout();
            if (opt_notifypoll && notifies &&
                ticks % ((opt_notifypoll + HEARTBEAT_SECS - 1) / HEARTBEAT_SECS) == 0)
                notify_poll_all();
            if (orphan_exit) {
                running = FALSE;
            } else {
                TR_SECS(treq) = HEARTBEAT_SECS;
                TR_MICRO(treq) = 0;
                SendIO((struct IORequest *)treq);
                timer_pending = TRUE;
            }
        }

        notify_replies();

        while (running && (msg = GetMsg(pktport))) {
            pkt = (struct DosPacket *)msg->mn_Node.ln_Name;
            if (pkt->dp_Type == ACTION_DIE || pkt->dp_Type == ACTION_SHUTDOWN) {
                BOOL busy = numlocks || numfiles || numnotifies || notify_msgs;
                plog("<- %4ld %s (locks=%ld files=%ld notify=%ld)", (long)pkt->dp_Type,
                     pkt->dp_Type == ACTION_DIE ? "ACTION_DIE" : "ACTION_SHUTDOWN",
                     numlocks, numfiles, numnotifies);
                /* OS4: con ACTION_SHUTDOWN gli oggetti aperti passano al
                 * lockhandler del DOS e lo smontaggio riesce lo stesso */
                if (busy && pkt->dp_Type == ACTION_SHUTDOWN && plat_collect_ok()) {
                    const char *why = shutdown_blocker();
                    if (why) {
                        plog("   SHUTDOWN rimandato: %s", why);
                    } else {
                        LONG kept = shutdown_collect();
                        plog("   COLLECT: oggetti ceduti, rifiutati=%ld", kept);
                        busy = kept > 0;
                    }
                }
                if (busy) {
                    struct NLock *l;
                    struct NFile *f;
                    plog("   %s rifiutato: locks=%ld files=%ld",
                         pkt->dp_Type == ACTION_DIE ? "DIE" : "SHUTDOWN", numlocks, numfiles);
                    for (l = locks; l; l = l->next)
                        plog("     lock %08lx %s  da '%s'", (unsigned long)MKBADDR(l),
                             l->path, l->owner);
                    for (f = files; f; f = f->next)
                        plog("     file %s", f->path);
                    {
                        struct NNotify *nn;
                        for (nn = notifies; nn; nn = nn->next)
                            plog("     notify %s", nn->path);
                    }
                    reply(pkt, DOSFALSE, ERROR_OBJECT_IN_USE);
                } else {
                    running = FALSE;
                    die_pkt = pkt;
                }
                continue;
            }
            dispatch(pkt);
        }
    }

    /* --- uscita: pulizia che richiede DOS/rete PRIMA di Forbid --- */
    if (timer_ok) {
        if (timer_pending) {
            AbortIO((struct IORequest *)treq);
            WaitIO((struct IORequest *)treq);
        }
        plat_eclock_close();
        eclock_freq = 0;
        CloseDevice((struct IORequest *)treq);
    }
    if (treq)  DeleteIORequest((struct IORequest *)treq);
    if (tport) DeleteMsgPort(tport);

    /* Richieste ancora in attesa del mount: il device se ne va, quindi
     * ERROR_DEVICE_NOT_MOUNTED. Poi, se ce n'erano, un secondo di pausa
     * prima di togliere il volume e rispondere allo smontaggio: su OS4 il
     * Workbench, se riceve la risposta mentre il device sparisce, si tiene
     * la porta dell'handler e la usa decine di secondi dopo l'uscita (Grim
     * Reaper con la porta di un'istanza gia' uscita, shutdown.script). */
    if (fail_pending(ERROR_DEVICE_NOT_MOUNTED) > 0) {
        int i;
        plog("   risposto alle richieste in attesa, pausa di 1 s");
        for (i = 0; i < 10; i++) {        /* e chi riprova nel frattempo */
            Delay(5);
            while ((msg = GetMsg(pktport))) {
                pkt = (struct DosPacket *)msg->mn_Node.ln_Name;
                plog("   in pausa: rifiutato %ld da '%s'", (long)pkt->dp_Type,
                     pkt->dp_Port && pkt->dp_Port->mp_SigTask
                         ? ((struct Task *)pkt->dp_Port->mp_SigTask)->tc_Node.ln_Name : "?");
                reply(pkt, DOSFALSE, ERROR_DEVICE_NOT_MOUNTED);
            }
        }
    }
    while (notifies) notify_remove(notifies->nr);
    while (locks) free_lock(locks);
    while (files) free_file(files);
    plog("   totale RPC inviate: %lu", (unsigned long)rpc_calls);
    if (nfs) { nfs_destroy_context(nfs); nfs = NULL; }
    amiga_socket_exit();
    rem_volume();
    plog("==== uscita (%s) ====", !die_pkt ? "orfano" :
         die_pkt->dp_Type == ACTION_DIE ? "ACTION_DIE" : "ACTION_SHUTDOWN");

    Forbid();
    plat_detach(devnode, pktport, die_pkt);
    /* la porta dei notify resta se ci sono messaggi non restituiti (non
     * succede: DIE viene rifiutato finche' ci sono notify attivi) */
    if (notify_port) DeleteMsgPort(notify_port);
    mini_exit();
}

/*
 * pktlog-handler -- M0 di NG-nfs-handler
 *
 * Handler DOS minimale (volume vuoto, read-only) che registra su file ogni
 * pacchetto ricevuto. Serve a verificare:
 *   - quali pacchetti inviano Workbench, Dir, Info, Assign, ecc.
 *   - se "Assign XXX: DISMOUNT" invia ACTION_DIE all'handler (OS3.1)
 *   - come Mount passa la stringa Startup (virgolette o no)
 *
 * Mountlist (DEVS:DOSDrivers o MountList):
 *   Handler = L:pktlog-handler
 *   GlobVec = -1
 *   Startup = "VOLUME=PKTLOG LOG=RAM:pktlog.log"
 *
 * Note di architettura (valide anche per il vero nfs-handler):
 *   - I pacchetti vengono ricevuti su una porta PRIVATA (dn_Task, fl_Task,
 *     dol_Task), non su pr_MsgPort: cosi' le chiamate DOS fatte dall'handler
 *     stesso (scrittura del log) non rubano i nostri pacchetti in arrivo.
 *   - pr_WindowPtr = -1: nessun requester dalle nostre chiamate DOS.
 *
 * Licenza: LGPL-2.1
 * Copyright (C) 2026 VagaPPC (Benedetto Lorello)
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <dos/notify.h>
#include <dos/rdargs.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdarg.h>

struct ExecBase   *SysBase;
struct DosLibrary *DOSBase;

#define HANDLER_VERSION "pktlog-handler 0.2 (26.11.2026)"
#define HEARTBEAT_SECS  10
#define ALIVE_EVERY     6          /* log "alive" ogni 6 heartbeat = 60 s */

int  handler_entry(void);         /* chiamata da start.s */
static void handler_main(void);

static const char verstag[] __attribute__((used)) = "$VER: " HANDLER_VERSION;

/* ------------------------------------------------------------------------ */
/* Stato globale                                                            */
/* ------------------------------------------------------------------------ */

static struct Process        *me;
static struct MsgPort        *pktport;      /* porta privata dei pacchetti */
static struct DeviceNode     *devnode;
static struct DosList        *volnode;
static char                   devname[64];
static char                   volname[32];
static char                   logname[128];
static LONG                   numlocks;
static BOOL                   devnode_seen = TRUE;
static BOOL                   devtask_ok   = TRUE;
static BOOL                   orphan_exit  = FALSE;

/* ------------------------------------------------------------------------ */
/* Utility                                                                  */
/* ------------------------------------------------------------------------ */

static ULONG slen(const char *s) { ULONG n = 0; while (s[n]) n++; return n; }

static void scopy(char *d, const char *s, ULONG max)
{
    ULONG i = 0;
    if (!max) return;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

/* BSTR -> C string */
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

/* move.b d0,(a3)+ ; rts  -- PutChProc per RawDoFmt */
static const UWORD stuffchar[] = { 0x16c0, 0x4e75 };

static void vfmt(char *buf, const char *fmt, va_list ap)
{
    RawDoFmt((CONST_STRPTR)fmt, (APTR)ap, (void (*)())stuffchar, buf);
}

static void fmt(char *buf, const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    vfmt(buf, f, ap);
    va_end(ap);
}

/* Scrive una riga nel log: apre, appende, chiude (sopravvive ai crash e
 * permette di leggere il file mentre l'handler gira). */
static void plog(const char *f, ...)
{
    char line[600], ts[32];
    struct DateStamp ds;
    BPTR fh;
    va_list ap;
    LONG secs;

    DateStamp(&ds);
    secs = ds.ds_Minute * 60 + ds.ds_Tick / TICKS_PER_SECOND;
    fmt(ts, "%02ld:%02ld:%02ld.%02ld ",
        (secs / 3600) % 24, (secs / 60) % 60, secs % 60,
        (ds.ds_Tick % TICKS_PER_SECOND) * 100 / TICKS_PER_SECOND);

    va_start(ap, f);
    vfmt(line, f, ap);
    va_end(ap);

    if (!logname[0]) return;
    fh = Open((STRPTR)logname, MODE_READWRITE);
    if (!fh) return;
    Seek(fh, 0, OFFSET_END);
    Write(fh, ts, slen(ts));
    Write(fh, line, slen(line));
    Write(fh, "\n", 1);
    Close(fh);
}

/* ------------------------------------------------------------------------ */
/* Nomi dei pacchetti                                                       */
/* ------------------------------------------------------------------------ */

struct pktname { LONG action; const char *name; };

static const struct pktname pktnames[] = {
    { ACTION_NIL,             "NIL" },
    { ACTION_DIE,             "DIE" },
    { ACTION_EVENT,           "EVENT" },
    { ACTION_CURRENT_VOLUME,  "CURRENT_VOLUME" },
    { ACTION_LOCATE_OBJECT,   "LOCATE_OBJECT" },
    { ACTION_RENAME_DISK,     "RENAME_DISK" },
    { ACTION_WRITE,           "WRITE" },
    { ACTION_READ,            "READ" },
    { ACTION_FREE_LOCK,       "FREE_LOCK" },
    { ACTION_DELETE_OBJECT,   "DELETE_OBJECT" },
    { ACTION_RENAME_OBJECT,   "RENAME_OBJECT" },
    { ACTION_MORE_CACHE,      "MORE_CACHE" },
    { ACTION_COPY_DIR,        "COPY_DIR" },
    { ACTION_WAIT_CHAR,       "WAIT_CHAR" },
    { ACTION_SET_PROTECT,     "SET_PROTECT" },
    { ACTION_CREATE_DIR,      "CREATE_DIR" },
    { ACTION_EXAMINE_OBJECT,  "EXAMINE_OBJECT" },
    { ACTION_EXAMINE_NEXT,    "EXAMINE_NEXT" },
    { ACTION_DISK_INFO,       "DISK_INFO" },
    { ACTION_INFO,            "INFO" },
    { ACTION_FLUSH,           "FLUSH" },
    { ACTION_SET_COMMENT,     "SET_COMMENT" },
    { ACTION_PARENT,          "PARENT" },
    { ACTION_TIMER,           "TIMER" },
    { ACTION_INHIBIT,         "INHIBIT" },
    { ACTION_DISK_TYPE,       "DISK_TYPE" },
    { ACTION_DISK_CHANGE,     "DISK_CHANGE" },
    { ACTION_SET_DATE,        "SET_DATE" },
    { ACTION_SAME_LOCK,       "SAME_LOCK" },
    { ACTION_SCREEN_MODE,     "SCREEN_MODE" },
    { ACTION_CHANGE_SIGNAL,   "CHANGE_SIGNAL" },
    { ACTION_READ_RETURN,     "READ_RETURN" },
    { ACTION_WRITE_RETURN,    "WRITE_RETURN" },
    { ACTION_FINDUPDATE,      "FINDUPDATE" },
    { ACTION_FINDINPUT,       "FINDINPUT" },
    { ACTION_FINDOUTPUT,      "FINDOUTPUT" },
    { ACTION_END,             "END" },
    { ACTION_SEEK,            "SEEK" },
    { ACTION_FORMAT,          "FORMAT" },
    { ACTION_MAKE_LINK,       "MAKE_LINK" },
    { ACTION_SET_FILE_SIZE,   "SET_FILE_SIZE" },
    { ACTION_WRITE_PROTECT,   "WRITE_PROTECT" },
    { ACTION_READ_LINK,       "READ_LINK" },
    { ACTION_FH_FROM_LOCK,    "FH_FROM_LOCK" },
    { ACTION_IS_FILESYSTEM,   "IS_FILESYSTEM" },
    { ACTION_CHANGE_MODE,     "CHANGE_MODE" },
    { ACTION_COPY_DIR_FH,     "COPY_DIR_FH" },
    { ACTION_PARENT_FH,       "PARENT_FH" },
    { ACTION_EXAMINE_ALL,     "EXAMINE_ALL" },
    { ACTION_EXAMINE_FH,      "EXAMINE_FH" },
#ifdef ACTION_EXAMINE_ALL_END
    { ACTION_EXAMINE_ALL_END, "EXAMINE_ALL_END" },
#endif
#ifdef ACTION_SET_OWNER
    { ACTION_SET_OWNER,       "SET_OWNER" },
#endif
    { ACTION_LOCK_RECORD,     "LOCK_RECORD" },
    { ACTION_FREE_RECORD,     "FREE_RECORD" },
    { ACTION_ADD_NOTIFY,      "ADD_NOTIFY" },
    { ACTION_REMOVE_NOTIFY,   "REMOVE_NOTIFY" },
#ifdef ACTION_SERIALIZE_DISK
    { ACTION_SERIALIZE_DISK,  "SERIALIZE_DISK" },
#endif
#ifdef ACTION_GET_DISK_FSSM
    { ACTION_GET_DISK_FSSM,   "GET_DISK_FSSM" },
    { ACTION_FREE_DISK_FSSM,  "FREE_DISK_FSSM" },
#endif
    { 0, NULL }
};

static const char *pkt_name(LONG action)
{
    const struct pktname *p;
    for (p = pktnames; p->name; p++)
        if (p->action == action) return p->name;
    return "???";
}

/* Indice (1..4) dell'argomento BSTR con un nome di file, 0 se nessuno */
static int bstr_name_arg(LONG action)
{
    switch (action) {
    case ACTION_LOCATE_OBJECT:
    case ACTION_DELETE_OBJECT:
    case ACTION_CREATE_DIR:
    case ACTION_MAKE_LINK:
    case ACTION_RENAME_OBJECT:   /* a2 = vecchio nome, a4 = nuovo */
        return 2;
    case ACTION_FINDINPUT:
    case ACTION_FINDOUTPUT:
    case ACTION_FINDUPDATE:
    case ACTION_SET_PROTECT:
    case ACTION_SET_COMMENT:
    case ACTION_SET_DATE:
        return 3;
    case ACTION_RENAME_DISK:
        return 1;
    }
    return 0;
}

static const char *sender_name(struct DosPacket *pkt)
{
    struct MsgPort *rp = pkt->dp_Port;
    struct Task *t;
    if (!rp) return "?";
    t = (struct Task *)rp->mp_SigTask;
    if (!t || !t->tc_Node.ln_Name) return "?";
    return t->tc_Node.ln_Name;
}

static void log_packet(struct DosPacket *pkt)
{
    char extra[300];
    int  ni = bstr_name_arg(pkt->dp_Type);
    LONG args[5];

    args[1] = pkt->dp_Arg1; args[2] = pkt->dp_Arg2;
    args[3] = pkt->dp_Arg3; args[4] = pkt->dp_Arg4;
    extra[0] = 0;

    if (ni) {
        char n1[256];
        bstr2c((BSTR)args[ni], n1, sizeof(n1));
        if (pkt->dp_Type == ACTION_RENAME_OBJECT) {
            char n2[256];
            bstr2c((BSTR)args[4], n2, sizeof(n2));
            fmt(extra, " name=\"%s\" new=\"%s\"", n1, n2);
        } else {
            fmt(extra, " name=\"%s\"", n1);
        }
    } else if (pkt->dp_Type == ACTION_READ_LINK) {
        /* in READ_LINK arg2 e' una C string */
        fmt(extra, " name=\"%s\"", args[2] ? (char *)args[2] : "");
    } else if (pkt->dp_Type == ACTION_ADD_NOTIFY && args[1]) {
        struct NotifyRequest *nr = (struct NotifyRequest *)args[1];
        fmt(extra, " notify=\"%s\" flags=$%lx",
            nr->nr_FullName ? (char *)nr->nr_FullName : "(null)",
            nr->nr_Flags);
    }

    plog("<- %-15s (%4ld) from '%s' a1=$%08lx a2=$%08lx a3=$%08lx a4=$%08lx%s",
         pkt_name(pkt->dp_Type), pkt->dp_Type, sender_name(pkt),
         args[1], args[2], args[3], args[4], extra);
}

static void reply(struct DosPacket *pkt, LONG res1, LONG res2)
{
    plog("-> %-15s res1=%ld res2=%ld", pkt_name(pkt->dp_Type), res1, res2);
    ReplyPkt(pkt, res1, res2);
}

/* ------------------------------------------------------------------------ */
/* Lock e volume                                                            */
/* ------------------------------------------------------------------------ */

#define ROOT_KEY 1

static BPTR make_lock(LONG access)
{
    struct FileLock *fl = AllocVec(sizeof(*fl), MEMF_PUBLIC | MEMF_CLEAR);
    if (!fl) return 0;
    fl->fl_Key    = ROOT_KEY;
    fl->fl_Access = access;
    fl->fl_Task   = pktport;
    fl->fl_Volume = MKBADDR(volnode);
    numlocks++;
    return MKBADDR(fl);
}

static void free_lock(BPTR l)
{
    if (!l) return;
    FreeVec(BADDR(l));
    numlocks--;
}

/* Il volume e' vuoto: esiste solo la root. Ritorna TRUE se il nome
 * (relativo a un lock sulla root) indica la root stessa. */
static BOOL is_root_name(BSTR bname)
{
    char name[256], *p;
    bstr2c(bname, name, sizeof(name));
    p = name;
    {   /* salta "VOLUME:" */
        char *c;
        for (c = name; *c; c++) if (*c == ':') p = c + 1;
    }
    return *p == 0;
}

static BOOL add_volume(void)
{
    struct DosList *dl;

    volnode = MakeDosEntry((STRPTR)volname, DLT_VOLUME);
    if (!volnode) return FALSE;
    volnode->dol_Task = pktport;
    DateStamp(&volnode->dol_misc.dol_volume.dol_VolumeDate);
    volnode->dol_misc.dol_volume.dol_DiskType = ID_DOS_DISK;

    dl = LockDosList(LDF_VOLUMES | LDF_WRITE);
    (void)dl;
    if (!AddDosEntry(volnode)) {
        UnLockDosList(LDF_VOLUMES | LDF_WRITE);
        FreeDosEntry(volnode);
        volnode = NULL;
        return FALSE;
    }
    UnLockDosList(LDF_VOLUMES | LDF_WRITE);
    return TRUE;
}

static void rem_volume(void)
{
    if (!volnode) return;
    LockDosList(LDF_VOLUMES | LDF_WRITE);
    RemDosEntry(volnode);
    UnLockDosList(LDF_VOLUMES | LDF_WRITE);
    FreeDosEntry(volnode);
    volnode = NULL;
}

static void fill_info(struct InfoData *id)
{
    id->id_NumSoftErrors = 0;
    id->id_UnitNumber    = 0;
    id->id_DiskState     = ID_WRITE_PROTECTED;
    id->id_NumBlocks     = 1;
    id->id_NumBlocksUsed = 1;
    id->id_BytesPerBlock = 512;
    id->id_DiskType      = ID_DOS_DISK;
    id->id_VolumeNode    = MKBADDR(volnode);
    id->id_InUse         = numlocks ? DOSTRUE : DOSFALSE;
}

static void fill_root_fib(struct FileInfoBlock *fib)
{
    ULONG n = slen(volname), i;
    if (n > 106) n = 106;
    fib->fib_DiskKey      = ROOT_KEY;
    fib->fib_DirEntryType = ST_ROOT;
    fib->fib_EntryType    = ST_ROOT;
    fib->fib_FileName[0]  = n;
    for (i = 0; i < n; i++) fib->fib_FileName[i + 1] = volname[i];
    fib->fib_FileName[n + 1] = 0;
    fib->fib_Protection   = 0;
    fib->fib_Size         = 0;
    fib->fib_NumBlocks    = 0;
    if (volnode) fib->fib_Date = volnode->dol_misc.dol_volume.dol_VolumeDate;
    else         DateStamp(&fib->fib_Date);
    fib->fib_Comment[0]   = 0;
    fib->fib_Comment[1]   = 0;
}

/* ------------------------------------------------------------------------ */
/* Heartbeat: il nostro DeviceNode e' ancora nella DosList?                 */
/* ------------------------------------------------------------------------ */

static void heartbeat(ULONG tick)
{
    struct DosList *dl;
    BOOL found = FALSE, taskok = FALSE;

    dl = AttemptLockDosList(LDF_DEVICES | LDF_READ);
    if ((ULONG)dl <= 1) return;     /* lista occupata: riprova dopo */
    while ((dl = NextDosEntry(dl, LDF_DEVICES))) {
        if ((struct DeviceNode *)dl == devnode) {
            found  = TRUE;
            taskok = (dl->dol_Task == pktport);
            break;
        }
    }
    UnLockDosList(LDF_DEVICES | LDF_READ);

    if (found != devnode_seen) {
        if (!found)
            plog("!! ORFANO: il DeviceNode %s: e' stato RIMOSSO dalla DosList "
                 "senza ACTION_DIE (handler ancora vivo, locks=%ld)",
                 devname, numlocks);
        else
            plog("!! il DeviceNode %s: e' ricomparso nella DosList", devname);
        devnode_seen = found;
    }
    if (found && taskok != devtask_ok) {
        plog("!! dn_Task di %s: %s", devname,
             taskok ? "di nuovo nostro" : "CAMBIATO (non punta piu' a noi)");
        devtask_ok = taskok;
    }
    if (tick % ALIVE_EVERY == 0)
        plog("   alive: locks=%ld devnode=%s", numlocks,
             found ? "presente" : "ASSENTE");

    /* Orfano (es. dopo "Assign DISMOUNT") e nessun lock in giro: nessuno
     * puo' piu' raggiungerci tramite il device, quindi ci chiudiamo da soli.
     * Da qui in poi devnode NON va piu' toccato (memoria forse liberata). */
    if (!found && numlocks == 0) {
        plog("   orfano senza lock: autochiusura");
        orphan_exit = TRUE;
    }
}

/* ------------------------------------------------------------------------ */
/* Startup                                                                  */
/* ------------------------------------------------------------------------ */

static void dump_startup(LONG st)
{
    char buf[300];

    plog("   dn_Startup raw = $%08lx", st);
    if (st > 1024 && TypeOfMem(BADDR(st))) {
        UBYTE *p = (UBYTE *)BADDR(st);
        char hex[3 * 32 + 1];
        ULONG i, n = p[0];
        bstr2c((BSTR)st, buf, sizeof(buf));
        plog("   dn_Startup come BSTR (len %ld): [%s]", n, buf);
        for (i = 0; i < n && i < 32; i++)
            fmt(hex + 3 * i, "%02lx ", (ULONG)p[i + 1]);
        hex[3 * i] = 0;
        plog("   primi byte: %s", hex);
    } else {
        plog("   dn_Startup non e' un BPTR valido (intero?)");
    }
}

/* Interpreta la stringa Startup con ReadArgs: VOLUME/K,LOG/K */
static void parse_startup(LONG st)
{
    char src[300];
    ULONG n, i, j;
    struct RDArgs *rda;
    LONG argv[2] = { 0, 0 };

    if (!(st > 1024 && TypeOfMem(BADDR(st)))) return;
    bstr2c((BSTR)st, src, sizeof(src) - 2);

    /* togli eventuali virgolette e caratteri di controllo */
    for (i = j = 0; src[i]; i++)
        if (src[i] != '"' && (UBYTE)src[i] >= ' ') src[j++] = src[i];
    src[j++] = '\n';
    src[j] = 0;
    n = j;

    rda = AllocDosObject(DOS_RDARGS, NULL);
    if (!rda) return;
    rda->RDA_Source.CS_Buffer = (UBYTE *)src;
    rda->RDA_Source.CS_Length = n;
    rda->RDA_Source.CS_CurChr = 0;
    rda->RDA_Flags |= RDAF_NOPROMPT;
    if (ReadArgs((STRPTR)"VOLUME/K,LOG/K", argv, rda)) {
        if (argv[0]) scopy(volname, (char *)argv[0], sizeof(volname));
        if (argv[1]) scopy(logname, (char *)argv[1], sizeof(logname));
        FreeArgs(rda);
    }
    FreeDosObject(DOS_RDARGS, rda);
}

/* ------------------------------------------------------------------------ */
/* Entry                                                                    */
/* ------------------------------------------------------------------------ */

int handler_entry(void)
{
    SysBase = *(struct ExecBase **)4;
    me = (struct Process *)FindTask(NULL);
    DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 37);
    if (!DOSBase) {
        /* impossibile rispondere in modo pulito senza dos V37 */
        struct Message *m;
        WaitPort(&me->pr_MsgPort);
        m = GetMsg(&me->pr_MsgPort);
        (void)m;
        return 0;
    }
    handler_main();
    CloseLibrary((struct Library *)DOSBase);
    return 0;
}

static void handler_main(void)
{
    struct Message   *msg;
    struct DosPacket *pkt;
    struct MsgPort   *tport = NULL;
    struct timerequest *treq = NULL;
    BOOL  timer_ok = FALSE, timer_pending = FALSE, running = TRUE;
    struct DosPacket *die_pkt = NULL;
    ULONG tick = 0;
    LONG  st, sa1, sa2, sa3;
    char  *c;

    me->pr_WindowPtr = (APTR)-1;

    /* --- pacchetto di startup su pr_MsgPort --- */
    WaitPort(&me->pr_MsgPort);
    msg = GetMsg(&me->pr_MsgPort);
    pkt = (struct DosPacket *)msg->mn_Node.ln_Name;

    devnode = (struct DeviceNode *)BADDR(pkt->dp_Arg3);
    bstr2c((BSTR)pkt->dp_Arg1, devname, sizeof(devname));
    for (c = devname; *c; c++) if (*c == ':') *c = 0;
    if (!devname[0] && devnode) bstr2c(devnode->dn_Name, devname, sizeof(devname));

    scopy(volname, devname[0] ? devname : "PKTLOG", sizeof(volname));
    scopy(logname, "RAM:pktlog.log", sizeof(logname));
    st = devnode ? (LONG)devnode->dn_Startup : pkt->dp_Arg2;
    parse_startup(st);

    pktport = CreateMsgPort();
    if (!pktport || !devnode) {
        ReplyPkt(pkt, DOSFALSE, ERROR_NO_FREE_STORE);
        if (pktport) DeleteMsgPort(pktport);
        return;
    }

    /* Copia gli argomenti: dopo ReplyPkt il pacchetto non e' piu' nostro */
    sa1 = pkt->dp_Arg1; sa2 = pkt->dp_Arg2; sa3 = pkt->dp_Arg3;

    /* Rispondi SUBITO allo startup: chi ci avvia (Mount/DOS) e' in attesa e
     * fare I/O DOS (log su RAM:) o AddDosEntry prima della risposta rischia
     * un deadlock. I pacchetti che arrivano nel frattempo restano in coda. */
    devnode->dn_Task = pktport;
    ReplyPkt(pkt, DOSTRUE, 0);

    plog("==== %s avviato ====", HANDLER_VERSION);
    plog("   exec V%ld.%ld  dos V%ld.%ld  task=$%08lx port=$%08lx",
         (LONG)SysBase->LibNode.lib_Version, (LONG)SysBase->LibNode.lib_Revision,
         (LONG)DOSBase->dl_lib.lib_Version, (LONG)DOSBase->dl_lib.lib_Revision,
         (LONG)me, (LONG)pktport);
    plog("   startup pkt: a1=$%08lx a2=$%08lx a3=$%08lx  device=\"%s\"",
         sa1, sa2, sa3, devname);
    plog("   DeviceNode: stack=%ld pri=%ld globvec=%ld seglist=$%08lx",
         devnode->dn_StackSize, devnode->dn_Priority,
         devnode->dn_GlobalVec, (LONG)devnode->dn_SegList);
    dump_startup((LONG)devnode->dn_Startup);
    if (sa2 != (LONG)devnode->dn_Startup)
        dump_startup(sa2);
    plog("   volume=\"%s\" log=\"%s\"", volname, logname);

    if (add_volume())
        plog("   startup completato, volume %s: montato", volname);
    else
        plog("!! impossibile creare il volume \"%s\" (il device %s: resta "
             "accessibile)", volname, devname);

    /* --- timer per heartbeat --- */
    if ((tport = CreateMsgPort()) &&
        (treq = (struct timerequest *)CreateIORequest(tport, sizeof(*treq))) &&
        OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK,
                   (struct IORequest *)treq, 0) == 0) {
        timer_ok = TRUE;
        treq->tr_node.io_Command = TR_ADDREQUEST;
        treq->tr_time.tv_secs    = HEARTBEAT_SECS;
        treq->tr_time.tv_micro   = 0;
        SendIO((struct IORequest *)treq);
        timer_pending = TRUE;
    } else {
        plog("!! timer.device non disponibile: niente heartbeat");
    }

    /* --- loop principale --- */
    while (running) {
        ULONG sigs = Wait((1UL << pktport->mp_SigBit) |
                          (timer_ok ? (1UL << tport->mp_SigBit) : 0));

        if (timer_ok && (sigs & (1UL << tport->mp_SigBit))) {
            if (GetMsg(tport)) {
                heartbeat(++tick);
                if (orphan_exit) {
                    running = FALSE;
                    timer_pending = FALSE;
                } else {
                    treq->tr_time.tv_secs  = HEARTBEAT_SECS;
                    treq->tr_time.tv_micro = 0;
                    SendIO((struct IORequest *)treq);
                }
            }
        }

        while (running && (msg = GetMsg(pktport))) {
            pkt = (struct DosPacket *)msg->mn_Node.ln_Name;
            log_packet(pkt);

            switch (pkt->dp_Type) {

            case ACTION_IS_FILESYSTEM:
            case ACTION_FLUSH:
                reply(pkt, DOSTRUE, 0);
                break;

            case ACTION_CURRENT_VOLUME:
                reply(pkt, MKBADDR(volnode), 0);
                break;

            case ACTION_LOCATE_OBJECT:
                if (is_root_name((BSTR)pkt->dp_Arg2)) {
                    BPTR l = make_lock(pkt->dp_Arg3 == EXCLUSIVE_LOCK
                                       ? EXCLUSIVE_LOCK : SHARED_LOCK);
                    if (l) reply(pkt, l, 0);
                    else   reply(pkt, 0, ERROR_NO_FREE_STORE);
                } else {
                    reply(pkt, 0, ERROR_OBJECT_NOT_FOUND);
                }
                break;

            case ACTION_COPY_DIR:
            case ACTION_COPY_DIR_FH: {
                BPTR l = make_lock(SHARED_LOCK);
                if (l) reply(pkt, l, 0);
                else   reply(pkt, 0, ERROR_NO_FREE_STORE);
                break;
            }

            case ACTION_FREE_LOCK:
                free_lock((BPTR)pkt->dp_Arg1);
                reply(pkt, DOSTRUE, 0);
                break;

            case ACTION_PARENT:
                reply(pkt, 0, 0);       /* la root non ha genitore */
                break;

            case ACTION_SAME_LOCK:
                reply(pkt, DOSTRUE, 0); /* esiste solo la root */
                break;

            case ACTION_EXAMINE_OBJECT:
                fill_root_fib((struct FileInfoBlock *)BADDR(pkt->dp_Arg2));
                reply(pkt, DOSTRUE, 0);
                break;

            case ACTION_EXAMINE_NEXT:
                reply(pkt, DOSFALSE, ERROR_NO_MORE_ENTRIES);
                break;

            case ACTION_DISK_INFO:
                fill_info((struct InfoData *)BADDR(pkt->dp_Arg1));
                reply(pkt, DOSTRUE, 0);
                break;

            case ACTION_INFO:
                fill_info((struct InfoData *)BADDR(pkt->dp_Arg2));
                reply(pkt, DOSTRUE, 0);
                break;

            case ACTION_FINDINPUT:
                reply(pkt, DOSFALSE, ERROR_OBJECT_NOT_FOUND);
                break;

            case ACTION_FINDOUTPUT:
            case ACTION_FINDUPDATE:
            case ACTION_DELETE_OBJECT:
            case ACTION_RENAME_OBJECT:
            case ACTION_CREATE_DIR:
            case ACTION_SET_PROTECT:
            case ACTION_SET_COMMENT:
            case ACTION_SET_DATE:
            case ACTION_RENAME_DISK:
            case ACTION_MAKE_LINK:
                reply(pkt, DOSFALSE, ERROR_DISK_WRITE_PROTECTED);
                break;

            case ACTION_DIE:
                if (numlocks > 0) {
                    plog("   DIE rifiutato: %ld lock aperti", numlocks);
                    reply(pkt, DOSFALSE, ERROR_OBJECT_IN_USE);
                } else {
                    /* La risposta parte solo alla fine, dentro Forbid():
                     * chi ha inviato il DIE riprende quando siamo gia' morti
                     * e puo' scaricare il nostro codice (UnLoadSeg). */
                    running = FALSE;
                    die_pkt = pkt;
                    plog("   DIE accettato");
                }
                break;

            default:
                reply(pkt, DOSFALSE, ERROR_ACTION_NOT_KNOWN);
                break;
            }
        }
    }

    /* --- uscita: tutta la pulizia che richiede DOS va fatta PRIMA di Forbid --- */
    if (timer_ok) {
        if (timer_pending) {
            AbortIO((struct IORequest *)treq);
            WaitIO((struct IORequest *)treq);
        }
        CloseDevice((struct IORequest *)treq);
    }
    if (treq)  DeleteIORequest((struct IORequest *)treq);
    if (tport) DeleteMsgPort(tport);

    rem_volume();
    plog("==== uscita (%s) ====", die_pkt ? "ACTION_DIE" : "orfano");

    Forbid();
    if (die_pkt) {
        devnode->dn_Task = NULL;    /* solo col DIE: il DeviceNode esiste */
        ReplyPkt(die_pkt, DOSTRUE, 0);
    }

    /* Pacchetti arrivati nel frattempo: rispondi con errore e chiudi la porta
     * senza finestre temporali (Forbid resta attivo fino alla fine del task). */
    while ((msg = GetMsg(pktport))) {
        pkt = (struct DosPacket *)msg->mn_Node.ln_Name;
        ReplyPkt(pkt, DOSFALSE, ERROR_ACTION_NOT_KNOWN);
    }
    DeleteMsgPort(pktport);
}

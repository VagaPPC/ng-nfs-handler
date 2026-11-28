/*
 * plat_os4.c -- piattaforma AmigaOS 4.1 (PPC, adtools gcc, clib4)
 *
 * Niente crt0 di clib4: il suo clib4_start, in un processo senza CLI,
 * aspetterebbe un messaggio di Workbench su pr_MsgPort e si prenderebbe il
 * pacchetto di startup del DOS. _start apre clib4.library a mano e chiama
 * library_start (vettore 76 dell'interfaccia "main", senza self).
 *
 * Ordine obbligato, verificato con tools/clib4probe su X5000 + clib4 2.5:
 *   1. rispondere al pacchetto di startup PRIMA di aprire clib4: con il
 *      pacchetto ancora in sospeso e la console di Mount ereditata,
 *      OpenLibrary("clib4.library") bloccava tutto il sistema;
 *   2. niente console: SetConsolePort(NULL), Input/Output su NIL:;
 *      niente requester (pr_WindowPtr = -1); PROGDIR: = cartella
 *      dell'handler (clib4 cerca PROGDIR:.unix);
 *   3. SetArgStr("\n"): arg_init di clib4 legge la riga di comando con
 *      GetArgStr(), NULL in un handler, e senza _main esce con 20.
 *
 * NG-nfs-handler -- LGPL-2.1
 * Copyright (C) 2026 VagaPPC (Benedetto Lorello)
 */
/* qui le interfacce si chiamano esplicitamente (IExec->...): niente
 * macro di __USE_INLINE__, che servono solo al nucleo */
#undef __USE_INLINE__

#include "config.h"
#include "aros_compat.h"

#include <exec/exec.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <dos/obsolete.h>
#include <devices/timer.h>
#include <libraries/locale.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/locale.h>
#include <proto/timer.h>

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/select.h>

#include "platform.h"
#include "minilibc.h"

/* Simboli che normalmente definisce crtbegin.o */
struct Library    *SysBase;
struct ExecIFace  *IExec;
struct Library    *DOSBase;
struct DOSIFace   *IDOS;
struct Library    *Clib4Base;
struct Interface  *IClib4;

struct TimerIFace *ITimer;

/* Liste costruttori/distruttori: -1 qui (oggetto linkato per primo tra
 * quelli con .ctors), 0 da crtend.o (linkato per ultimo) */
static void (*ctor_list[1])(void) __attribute__((used, section(".ctors"))) = { (void (*)(void))-1 };
static void (*dtor_list[1])(void) __attribute__((used, section(".dtors"))) = { (void (*)(void))-1 };

typedef int (*library_start_t)(char *argstr, int arglen,
                               int (*mainfn)(int, char **),
                               void (**ctors)(void), void (**dtors)(void),
                               APTR wbmsg);

static struct HandlerStartup hs;

/* --------------------------------------------------------------- entry -- */

int32 plat_start(STRPTR argstr, int32 arglen, struct ExecBase *sysbase);

/* r13 = _SDA_BASE_ come in clib4_start: gli stub di clib4 trovano IClib4
 * tramite small data */
__asm__(
    "	.section .text\n"
    "	.globl _start\n"
    "	.type _start,@function\n"
    "_start:\n"
    "	stwu 1,-16(1)\n"
    "	mflr 0\n"
    "	stw 0,20(1)\n"
    "	stw 13,8(1)\n"
    "	lis 13,_SDA_BASE_@ha\n"
    "	addi 13,13,_SDA_BASE_@l\n"
    "	bl plat_start\n"
    "	lwz 13,8(1)\n"
    "	lwz 0,20(1)\n"
    "	mtlr 0\n"
    "	addi 1,1,16\n"
    "	blr\n");

static void bstr_copy(BSTR b, char *out, int max)
{
    UBYTE *p = (UBYTE *)BADDR(b);
    int n, i;
    out[0] = 0;
    if (!p) return;
    n = p[0];
    if (n > max - 1) n = max - 1;
    for (i = 0; i < n; i++) out[i] = p[i + 1];
    out[n] = 0;
}

/* Uscita rimandata alla fine di plat_start (vedi plat_detach()): dopo la
 * risposta allo smontaggio Dismount puo' scaricare il segmento, quindi
 * prima deve finire tutto (uscita di clib4 compresa). Fino ad allora la
 * porta resta viva e il DeviceNode punta a lei. */
static struct DosPacket   *die_pkt;
static struct DeviceNode  *fin_dn;
static struct MsgPort     *fin_port;

static int plat_main(int argc, char **argv)
{
    handler_main(&hs);
    return 0;
}

int32 plat_start(STRPTR argstr, int32 arglen, struct ExecBase *sysbase)
{
    struct Process   *me;
    struct Message   *m;
    struct DosPacket *pkt;
    BPTR  progdir = 0, old_progdir = 0, nil_in, nil_out;
    BPTR  old_in = 0, old_out = 0;
    struct MsgPort *old_con = NULL;
    STRPTR old_args = NULL;
    int   started = 0;

    SysBase = (struct Library *)(sysbase ? sysbase : *(struct ExecBase **)4);
    IExec = (struct ExecIFace *)((struct ExecBase *)SysBase)->MainInterface;
    IExec->Obtain();
    me = (struct Process *)IExec->FindTask(NULL);

    DOSBase = IExec->OpenLibrary("dos.library", 52);
    IDOS = DOSBase ? (struct DOSIFace *)IExec->GetInterface(DOSBase, "main", 1, NULL) : NULL;

    /* 1. pacchetto di startup: copiare cio' che serve e rispondere subito */
    IExec->WaitPort(&me->pr_MsgPort);
    m = IExec->GetMsg(&me->pr_MsgPort);
    pkt = (struct DosPacket *)m->mn_Node.ln_Name;
    if (!IDOS) goto out;              /* senza dos: il pacchetto resta li' */

    hs.devnode = (struct DeviceNode *)BADDR(pkt->dp_Arg3);
    bstr_copy((BSTR)pkt->dp_Arg1, hs.devname, sizeof(hs.devname));
    /* "NFS0:" -> "NFS0"; vuoto -> dn_Name */
    {
        char *c;
        for (c = hs.devname; *c; c++) if (*c == ':') { *c = 0; break; }
        if (!hs.devname[0] && hs.devnode)
            bstr_copy(hs.devnode->dn_Name, hs.devname, sizeof(hs.devname));
    }
    hs.port = hs.devnode ? IExec->CreateMsgPort() : NULL;
    if (!hs.port) {
        IDOS->ReplyPkt(pkt, DOSFALSE, hs.devnode ? ERROR_NO_FREE_STORE : ERROR_BAD_TEMPLATE);
        goto out;
    }
    /* PROGDIR: = cartella dell'handler (da dn_Handler, finche' il
     * DeviceNode e' sicuramente valido) */
    {
        char path[256];
        bstr_copy(hs.devnode->dn_Handler, path, sizeof(path));
        if (path[0]) {
            *(char *)IDOS->PathPart(path) = 0;
            progdir = IDOS->Lock(path, SHARED_LOCK);
        }
    }
    hs.devnode->dn_Port = hs.port;
    IDOS->ReplyPkt(pkt, DOSTRUE, 0);

    /* 2. ambiente pulito per clib4 */
    me->pr_WindowPtr = (APTR)-1;
    old_con = IDOS->SetConsolePort(NULL);
    nil_in  = IDOS->Open("NIL:", MODE_OLDFILE);
    nil_out = IDOS->Open("NIL:", MODE_NEWFILE);
    if (nil_in)  old_in  = IDOS->SelectInput(nil_in);
    if (nil_out) old_out = IDOS->SelectOutput(nil_out);
    if (progdir) old_progdir = IDOS->SetProgramDir(progdir);

    /* 3. clib4 */
    Clib4Base = IExec->OpenLibrary("clib4.library", 2);
    if (Clib4Base) IClib4 = IExec->GetInterface(Clib4Base, "main", 1, NULL);
    if (IClib4) {
        static char noargs[] = "\n";
        library_start_t lstart = *(library_start_t *)((UBYTE *)IClib4 + 76);
        old_args = IDOS->SetArgStr(noargs);
        started = 1;
        lstart(noargs, 1, plat_main, ctor_list, dtor_list, NULL);
        IDOS->SetArgStr(old_args);
    }
    if (IClib4) IExec->DropInterface(IClib4);
    if (Clib4Base) IExec->CloseLibrary(Clib4Base);
    IClib4 = NULL;
    Clib4Base = NULL;

    if (!started) {
        /* clib4 assente: staccarsi dal DeviceNode */
        IExec->Forbid();
        hs.devnode->dn_Port = NULL;
        while ((m = IExec->GetMsg(hs.port)))
            IDOS->ReplyPkt((struct DosPacket *)m->mn_Node.ln_Name, DOSFALSE, ERROR_NO_DISK);
        IExec->DeleteMsgPort(hs.port);
        IExec->Permit();
    }

    if (progdir) { IDOS->SetProgramDir(old_progdir); IDOS->UnLock(progdir); }
    if (nil_in)  { IDOS->SelectInput(old_in);   IDOS->Close(nil_in); }
    if (nil_out) { IDOS->SelectOutput(old_out); IDOS->Close(nil_out); }
    IDOS->SetConsolePort(old_con);
out:
    /* Da qui in poi niente che possa fare Wait(): la risposta allo
     * smontaggio permette di scaricare il nostro codice, e il Forbid()
     * resta fino alla fine del processo. */
    IExec->Forbid();
    if (fin_port && IDOS) {
        if (die_pkt && fin_dn) fin_dn->dn_Port = NULL;
        while ((m = IExec->GetMsg(fin_port)))
            IDOS->ReplyPkt((struct DosPacket *)m->mn_Node.ln_Name, DOSFALSE,
                           ERROR_ACTION_NOT_KNOWN);
        IExec->DeleteMsgPort(fin_port);
    }
    if (die_pkt && IDOS) IDOS->ReplyPkt(die_pkt, DOSTRUE, 0);
    if (IDOS) IExec->DropInterface((struct Interface *)IDOS);
    if (DOSBase) IExec->CloseLibrary(DOSBase);
    IExec->Release();
    return 0;
}

/* ------------------------------------------------- servizi della libc -- */

/* La memoria la gestisce clib4: niente pool */
int  mini_init(void) { return 0; }
void mini_exit(void) { }
const char *mini_errstr(int e) { return strerror(e); }

void plat_detach(struct DeviceNode *dn, struct MsgPort *port, struct DosPacket *die)
{
    fin_dn   = dn;
    fin_port = port;
    die_pkt  = die;
}

void (*mini_assert_hook)(const char *file, int line, const char *expr);

/* assert() di clib4: niente abort() in un handler. Si registra e si ferma
 * il processo (Wait(0)) senza far cadere il sistema. */
void __assertion_failure(const char *file, int line, const char *expr)
{
    if (mini_assert_hook) mini_assert_hook(file, line, expr);
    IExec->Wait(0);
}

/* ------------------------------------------------------------- socket -- */

/* clib4 apre bsdsocket.library per il processo in library_start; qui si
 * verifica solo che lo stack ci sia davvero. */
static int sock_ok;

int amiga_socket_init(void)
{
    int s;
    if (sock_ok) return 0;
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    close(s);
    sock_ok = 1;
    return 0;
}

void amiga_socket_exit(void) { sock_ok = 0; }
int  amiga_socket_ready(void) { return sock_ok; }

/* readv/writev sui socket con recv/send (vedi aros_compat.h) */
#undef readv
#undef writev
ssize_t amiga_readv(int fd, const struct iovec *iov, int iovcnt)
{
    ssize_t total = 0;
    int i;

    for (i = 0; i < iovcnt; i++) {
        ssize_t n;
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

    /* pacchetti piccoli: un solo send() (niente segmenti TCP spezzati);
     * buffer statico: libnfs gira in un solo processo */
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
        ssize_t n;
        if (!iov[i].iov_len) continue;
        n = send(fd, iov[i].iov_base, iov[i].iov_len, 0);
        if (n < 0) return total ? total : -1;
        total += n;
        if ((size_t)n < iov[i].iov_len) break;
    }
    return total;
}

/* -------------------------------------------- disco inserito/tolto -- */

/* Volume nella DosList. Predefinito: LockDosList + AddDosEntry/RemDosEntry
 * (com'era prima, 20 cicli di Mount/Dismount senza crash). Con
 * -DOS4_NBM_VOLUME: NonBlockingModifyDosEntry (NBM_ADDDOSENTRY,
 * NBM_REMFREEDOSENTRY), che con shutdown.script e tooltype/os4.script ha
 * fatto cadere il Workbench dopo il Dismount (porta dell'handler cancellata). */
#ifdef OS4_NBM_VOLUME
BOOL plat_volume_add(struct DosList *vol)
{
    return IDOS->NonBlockingModifyDosEntry(vol, NBM_ADDDOSENTRY, NULL, NULL) != 0;
}

/* rimuove e libera: il nodo non va piu' toccato. Prima la porta a NULL: se il
 * DOS rimanda la rimozione, il nodo non punta a una porta cancellata. */
void plat_volume_remove(struct DosList *vol)
{
    vol->dol_Port = NULL;
    IDOS->NonBlockingModifyDosEntry(vol, NBM_REMFREEDOSENTRY, NULL, NULL);
}
#else
BOOL plat_volume_add(struct DosList *vol)
{
    BOOL ok;
    IDOS->LockDosList(LDF_VOLUMES | LDF_WRITE);
    ok = IDOS->AddDosEntry(vol) != 0;
    IDOS->UnLockDosList(LDF_VOLUMES | LDF_WRITE);
    return ok;
}

void plat_volume_remove(struct DosList *vol)
{
    IDOS->LockDosList(LDF_VOLUMES | LDF_WRITE);
    IDOS->RemDosEntry(vol);
    IDOS->UnLockDosList(LDF_VOLUMES | LDF_WRITE);
    IDOS->FreeDosEntry(vol);
}
#endif

void plat_disk_event(BOOL inserted)
{
    IDOS->NonBlockingModifyDosEntry(NULL, inserted ? NBM_DISKINSERTED : NBM_DISKREMOVED,
                                    NULL, NULL);
}

/* ------------------------------------------- lock e ACTION_COLLECT -- */

void *plat_lock_alloc(ULONG size)
{
    UBYTE *l = IDOS->AllocDosObjectTags(DOS_LOCK, ADO_Size, size, TAG_END);
    /* AllocDosObject prepara la parte pubblica (fl_DOSType,
     * fl_StructSize...): si azzera solo la parte privata che segue */
    if (l && size > sizeof(struct Lock))
        memset(l + sizeof(struct Lock), 0, size - sizeof(struct Lock));
    return l;
}

void plat_lock_free(void *lock)
{
    IDOS->FreeDosObject(DOS_LOCK, lock);
}

BOOL plat_collect_ok(void) { return TRUE; }

BOOL plat_collect(LONG type, APTR object)
{
    struct MsgPort *port;
    static const int32 ids[] = { 0, ID_COLLECT_LOCK, ID_COLLECT_FILEHANDLE,
                                 ID_COLLECT_NOTIFICATION };
    if (type < 1 || type > 3 || !object) return FALSE;
    IExec->Forbid();
    port = IExec->FindPort("dos_lock_handler_port");
    IExec->Permit();
    if (!port) return FALSE;
    return IDOS->DoPkt(port, ACTION_COLLECT, ids[type], (int32)object, 0, 0, 0) != 0;
}

/* ------------------------------------------------------- piattaforma -- */

LONG plat_gmt_offset(void)
{
    LONG off = 0;
    struct Library *LocaleBase = IExec->OpenLibrary("locale.library", 50);
    struct LocaleIFace *ILocale = LocaleBase ?
        (struct LocaleIFace *)IExec->GetInterface(LocaleBase, "main", 1, NULL) : NULL;
    if (ILocale) {
        struct Locale *loc = ILocale->OpenLocale(NULL);
        if (loc) {
            off = loc->loc_GMTOffset * 60;      /* minuti a ovest di GMT */
            ILocale->CloseLocale(loc);
        }
        IExec->DropInterface((struct Interface *)ILocale);
    }
    if (LocaleBase) IExec->CloseLibrary(LocaleBase);
    return off;
}

ULONG plat_eclock_open(struct Device *timer)
{
    struct EClockVal ev;
    ITimer = timer ? (struct TimerIFace *)IExec->GetInterface((struct Library *)timer,
                                                              "main", 1, NULL) : NULL;
    return ITimer ? ITimer->ReadEClock(&ev) : 0;
}

void plat_eclock_close(void)
{
    if (ITimer) IExec->DropInterface((struct Interface *)ITimer);
    ITimer = NULL;
}

uint64_t plat_eclock_read(void)
{
    struct EClockVal ev;
    if (!ITimer) return 0;
    ITimer->ReadEClock(&ev);
    return ((uint64_t)ev.ev_hi << 32) | ev.ev_lo;
}

int plat_wait(int fd, int events, ULONG *sigs)
{
    fd_set rfds, wfds;
    int rc, revents = 0;

    if (fd < 0) {
        *sigs = IExec->Wait(*sigs);
        return 0;
    }
    FD_ZERO(&rfds); FD_ZERO(&wfds);
    if (events & POLLIN)  FD_SET(fd, &rfds);
    if (events & POLLOUT) FD_SET(fd, &wfds);
    rc = waitselect(fd + 1, &rfds, &wfds, NULL, NULL, (unsigned long *)sigs);
    if (rc < 0) { *sigs = 0; return -1; }
    if (rc > 0) {
        if (FD_ISSET(fd, &rfds)) revents |= POLLIN;
        if (FD_ISSET(fd, &wfds)) revents |= POLLOUT;
    }
    return revents;
}

/*
 * clib4probe -- handler di prova: clib4 dentro un handler DOS su AmigaOS 4
 *
 * Il crt0 di clib4 (clib4_start in crtbegin.o), se il processo non ha CLI,
 * aspetta un messaggio di Workbench su pr_MsgPort: in un handler si
 * prenderebbe il pacchetto di startup del DOS. Qui niente crt0: _start apre
 * clib4.library e chiama direttamente library_start (vettore 76
 * dell'interfaccia, senza puntatore self) con messaggio Workbench = NULL.
 *
 * Domande a cui risponde (log in NGNFS:build/os4-probe.log):
 *   1. clib4 si inizializza senza crt0? main() viene chiamata?
 *   2. malloc/free/snprintf/strtol/time funzionano nel processo handler?
 *   3. il pacchetto di startup arriva intatto dopo l'init di clib4?
 *   4. socket di clib4 + waitselect() svegliano sia per il socket sia per
 *      il segnale della porta pacchetti? (connect non bloccante e chiamata
 *      RPC NULL al server NFS indicato da SERVER= nella Startup)
 *   5. ACTION_DIE: uscita pulita attraverso exit di clib4?
 *
 * Mountlist: test/os4/PROBE (o test/os4/local/PROBE con l'IP vero).
 *
 * NG-nfs-handler -- LGPL-2.1
 */
#include <exec/exec.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>
#include <dos/obsolete.h>     /* ACTION_DIE */
#include <sys/amigaos-va.h>

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define LOGFILE "NGNFS:build/os4-probe.log"

static const char verstag[] __attribute__((used)) =
    "$VER: clib4probe 0.1 (27.11.2026)";

/* Simboli che normalmente definisce crtbegin.o */
struct Library     *SysBase;
struct ExecIFace   *IExec;
struct Library     *DOSBase;
struct DOSIFace    *IDOS;
struct Library     *Clib4Base;
struct Interface   *IClib4;
struct Library     *UtilityBase;
struct UtilityIFace *IUtility;

/* Liste costruttori/distruttori: -1 qui (oggetto linkato per primo),
 * 0 da crtend.o (linkato per ultimo), come fa crtbegin.o */
static void (*ctor_list[1])(void) __attribute__((used, section(".ctors"))) = { (void (*)(void))-1 };
static void (*dtor_list[1])(void) __attribute__((used, section(".dtors"))) = { (void (*)(void))-1 };

typedef int (*library_start_t)(char *argstr, int arglen,
                               int (*mainfn)(int, char **),
                               void (**ctors)(void), void (**dtors)(void),
                               APTR wbmsg);

static struct Process  *me;
static struct DosPacket *startup_pkt;   /* non ancora risposto se != NULL */
static int  main_reached;
static int  startup_done;

/* --------------------------------------------------- log (solo IDOS) -- */

/* formato RawDoFmt (utility.library): %ld/%lx/%s/%p */
static void VARARGS68K plog(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    BPTR fh;
    va_startlinear(ap, fmt);
    if (IUtility) IUtility->VSNPrintf(line, sizeof(line), fmt, va_getlinearva(ap, APTR));
    else strcpy(line, fmt);
    va_end(ap);
    fh = IDOS->Open(LOGFILE, MODE_READWRITE);
    if (!fh) return;
    IDOS->ChangeFilePosition(fh, 0, OFFSET_END);
    IDOS->FPuts(fh, line);
    IDOS->FPuts(fh, "\n");
    IDOS->Close(fh);
}

static void bstr2c(BSTR b, char *out, int max)
{
    UBYTE *p = (UBYTE *)BADDR(b);
    int n, i;
    if (!p) { out[0] = 0; return; }
    n = p[0];
    if (n > max - 1) n = max - 1;
    for (i = 0; i < n; i++) out[i] = p[i + 1];
    out[n] = 0;
}

/* ------------------------------------------------------------- test -- */

static int test_libc(void)
{
    char buf[128];
    const char *expect = "abc 42 -7 1234567890123 ff 0000beef";
    int i, ok = 1;
    void *big;
    char *s;

    /* malloc/free: blocchi di varie dimensioni */
    for (i = 0; i < 200; i++) {
        size_t n = 1 + (i * 37) % 4000;
        char *p = malloc(n);
        if (!p) { plog("FAIL malloc(%lu) al giro %ld", (ULONG)n, (LONG)i); return 0; }
        memset(p, i, n);
        free(p);
    }
    big = malloc(1024 * 1024);
    plog("%s malloc(1MB) = %08lx", big ? "OK  " : "FAIL", (ULONG)big);
    if (!big) ok = 0;
    free(big);

    snprintf(buf, sizeof(buf), "%s %d %ld %lld %x %08lx", "abc", 42, -7L,
             1234567890123LL, 255, 0xbeefUL);
    if (strcmp(buf, expect)) { plog("FAIL snprintf: \"%s\"", buf); ok = 0; }
    else plog("OK   snprintf: \"%s\"", buf);

    if (strtol("0755", NULL, 8) != 493) { plog("FAIL strtol"); ok = 0; }
    else plog("OK   strtol");

    s = strdup("dup");
    plog("%s strdup", s && !strcmp(s, "dup") ? "OK  " : "FAIL");
    free(s);

    plog("OK   time(NULL) = %ld", (LONG)time(NULL));
    return ok;
}

/* Chiamata RPC NULL a NFS v3 su TCP (record marking) */
static int rpc_null_call(unsigned char *out)
{
    static const ULONG call[] = {
        0x80000028UL,           /* record mark: ultimo frammento, 40 byte */
        0x4E474E31UL,           /* xid 'NGN1' */
        0, 2,                   /* CALL, RPC v2 */
        100003, 3, 0,           /* NFS, v3, NULL */
        0, 0,                   /* cred AUTH_NONE */
        0, 0                    /* verf AUTH_NONE */
    };
    memcpy(out, call, sizeof(call));    /* PPC: gia' big-endian */
    return sizeof(call);
}

static void handle_packets(struct MsgPort *port, int *die)
{
    struct Message *m;
    while ((m = IExec->GetMsg(port))) {
        struct DosPacket *pkt = (struct DosPacket *)m->mn_Node.ln_Name;
        plog("     pacchetto %ld ricevuto", (LONG)pkt->dp_Type);
        if (pkt->dp_Type == ACTION_DIE || pkt->dp_Type == ACTION_SHUTDOWN) {
            *die = 1;
            /* la risposta a DIE la diamo dopo la pulizia */
            startup_pkt = pkt;
            return;
        }
        IDOS->ReplyPkt(pkt, DOSFALSE, ERROR_ACTION_NOT_KNOWN);
    }
}

/* Pacchetto di startup gestito PRIMA di aprire clib4 (senza libc: gli stub
 * di clib4 non funzionano ancora). Mount aspetta la risposta: se l'apertura
 * di clib4 si blocca, almeno Mount e il resto del sistema vanno avanti. */
static struct DeviceNode *startup_dn;
static struct MsgPort    *startup_port;
static char               startup_server[64];

static int do_startup(void)
{
    struct Message *m;
    struct DosPacket *sp;
    char st[256];
    int i, j;

    plog("== 3. pacchetto di startup (prima di clib4)");
    if (!(m = IExec->GetMsg(&me->pr_MsgPort))) {
        IExec->WaitPort(&me->pr_MsgPort);
        m = IExec->GetMsg(&me->pr_MsgPort);
    }
    sp = (struct DosPacket *)m->mn_Node.ln_Name;
    startup_dn = (struct DeviceNode *)BADDR(sp->dp_Arg3);
    plog("OK   startup: tipo=%ld dn=%08lx", (LONG)sp->dp_Type, (ULONG)startup_dn);
    if (!startup_dn) { IDOS->ReplyPkt(sp, DOSFALSE, ERROR_BAD_TEMPLATE); return 0; }
    bstr2c((BSTR)startup_dn->dn_Startup, st, sizeof(st));
    startup_server[0] = 0;
    for (i = 0; st[i]; i++)
        if (!IUtility->Strnicmp(st + i, "SERVER=", 7)) {
            i += 7;
            for (j = 0; st[i] && st[i] != ' ' && st[i] != '"' && j < 63; ) startup_server[j++] = st[i++];
            startup_server[j] = 0;
            break;
        }
    plog("     Startup=\"%s\" server=%s", st, startup_server);
    startup_port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_DONE);
    if (!startup_port) { IDOS->ReplyPkt(sp, DOSFALSE, ERROR_NO_FREE_STORE); return 0; }
    startup_dn->dn_Port = startup_port;
    IDOS->ReplyPkt(sp, DOSTRUE, 0);
    plog("OK   risposto al pacchetto di startup (Mount puo' tornare)");
    return 1;
}

static int probe_main(int argc, char **argv)
{
    struct DeviceNode *dn;
    struct MsgPort *port;
    char startup[256], server[64], *p;
    int s = -1, die = 0, phase = 0, rpc_ok = 0, wake_sig = 0, wake_sock = 0;
    struct sockaddr_in sa;
    unsigned char pkt_buf[64];
    time_t t0;

    main_reached = 1;
    plog("OK   main() chiamata da clib4 (argc=%ld)", (LONG)argc);
    plog("     pr_CLI=%lx pr_CIS=%lx pr_COS=%lx", (ULONG)me->pr_CLI,
         (ULONG)me->pr_CIS, (ULONG)me->pr_COS);

    plog("== 2. libc nel processo handler");
    test_libc();

    dn = startup_dn;
    port = startup_port;
    strcpy(server, startup_server);
    (void)startup; (void)p;

    plog("== 4. socket clib4 + waitselect");
    s = socket(AF_INET, SOCK_STREAM, 0);
    plog("%s socket() = %ld (errno %ld)", s >= 0 ? "OK  " : "FAIL", (LONG)s, (LONG)errno);
    if (s >= 0) {
        int on = 1, rc;
        rc = ioctl(s, FIONBIO, &on);
        plog("%s ioctl(FIONBIO) = %ld", rc == 0 ? "OK  " : "FAIL", (LONG)rc);
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(2049);
        sa.sin_addr.s_addr = inet_addr(server);
        rc = connect(s, (struct sockaddr *)&sa, sizeof(sa));
        plog("     connect(%s:2049) = %ld errno=%ld (atteso EINPROGRESS=%ld)",
             server, (LONG)rc, (LONG)errno, (LONG)EINPROGRESS);
        phase = 1;
    }

    t0 = time(NULL);
    while (!die && time(NULL) - t0 < 60) {
        fd_set rfds, wfds;
        struct timeval tv = { 1, 0 };
        ULONG sigs = (1UL << port->mp_SigBit) | SIGBREAKF_CTRL_C;
        int rc;
        FD_ZERO(&rfds); FD_ZERO(&wfds);
        if (s >= 0 && phase == 1) FD_SET(s, &wfds);
        if (s >= 0 && phase == 2) FD_SET(s, &rfds);
        if (s >= 0 && phase == 3) FD_SET(s, &rfds);     /* solo attesa */
        rc = waitselect(s >= 0 ? s + 1 : 0, &rfds, &wfds, NULL, &tv, &sigs);
        if (rc < 0) { plog("FAIL waitselect = %ld errno=%ld", (LONG)rc, (LONG)errno); break; }
        if (sigs & (1UL << port->mp_SigBit)) {
            if (!wake_sig) plog("OK   waitselect svegliata dal segnale della porta");
            wake_sig = 1;
            handle_packets(port, &die);
        }
        if (sigs & SIGBREAKF_CTRL_C) { plog("     CTRL-C"); break; }
        if (rc > 0 && phase == 1 && FD_ISSET(s, &wfds)) {
            int err = 0; socklen_t len = sizeof(err);
            getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len);
            plog("%s connessione: SO_ERROR=%ld", err ? "FAIL" : "OK  ", (LONG)err);
            if (!wake_sock) plog("OK   waitselect svegliata dal socket (scrivibile)");
            wake_sock = 1;
            if (err) { phase = 3; continue; }
            {
                int n = rpc_null_call(pkt_buf);
                rc = send(s, pkt_buf, n, 0);
                plog("%s send(RPC NULL) = %ld", rc == n ? "OK  " : "FAIL", (LONG)rc);
            }
            phase = 2;
        } else if (rc > 0 && phase == 2 && FD_ISSET(s, &rfds)) {
            ULONG rep[8];
            rc = recv(s, rep, sizeof(rep), 0);
            plog("OK   waitselect svegliata dal socket (leggibile)");
            if (rc >= 28 && rep[1] == 0x4E474E31UL && rep[2] == 1 && rep[3] == 0)
                { plog("OK   risposta RPC NULL (%ld byte, xid giusto, accettata)", (LONG)rc); rpc_ok = 1; }
            else
                plog("FAIL risposta RPC: %ld byte, xid=%lx tipo=%lu stato=%lu", (LONG)rc,
                     rc >= 8 ? rep[1] : 0, rc >= 12 ? rep[2] : 0, rc >= 16 ? rep[3] : 0);
            phase = 3;
            plog("     ora aspetto pacchetti (ACTION_DIE per uscire, max 60 s)");
        }
    }

    plog("== 5. uscita (%s)", die ? "ACTION_DIE" : "timeout/errore");
    plog("     riepilogo: segnale=%s socket=%s rpc=%s", wake_sig ? "si" : "NO",
         wake_sock ? "si" : "NO", rpc_ok ? "si" : "NO");
    if (s >= 0) close(s);

    /* il DeviceNode e' ancora nella DosList? (un dismount puo' averlo tolto) */
    {
        struct DosList *dl = IDOS->LockDosList(LDF_DEVICES | LDF_READ);
        BOOL found = FALSE;
        while ((dl = IDOS->NextDosEntry(dl, LDF_DEVICES)))
            if ((struct DeviceNode *)dl == dn) { found = TRUE; break; }
        IDOS->UnLockDosList(LDF_DEVICES | LDF_READ);
        plog("     DeviceNode ancora nella DosList: %s", found ? "si" : "no");
        if (!found) dn = NULL;
    }
    IExec->Forbid();
    if (dn) dn->dn_Port = NULL;
    if (startup_pkt) { IDOS->ReplyPkt(startup_pkt, DOSTRUE, 0); startup_pkt = NULL; }
    {
        struct Message *m;
        while ((m = IExec->GetMsg(port)))
            IDOS->ReplyPkt((struct DosPacket *)m->mn_Node.ln_Name, DOSFALSE, ERROR_ACTION_NOT_KNOWN);
    }
    IExec->FreeSysObject(ASOT_PORT, port);
    IExec->Permit();
    return 0;
}

/* -------------------------------------------------------------- entry -- */

/* _start: come clib4_start, r13 = _SDA_BASE_ (small data: anche gli stub di
 * clib4 accedono a IClib4 tramite r13), poi probe_start. */
int32 probe_start(STRPTR argstr, int32 arglen, struct ExecBase *sysbase);
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
    "	bl probe_start\n"
    "	lwz 13,8(1)\n"
    "	lwz 0,20(1)\n"
    "	mtlr 0\n"
    "	addi 1,1,16\n"
    "	blr\n");

int32 probe_start(STRPTR argstr, int32 arglen, struct ExecBase *sysbase)
{
    library_start_t lstart;
    int rc = -1;

    SysBase = (struct Library *)(sysbase ? sysbase : *(struct ExecBase **)4);
    IExec = (struct ExecIFace *)((struct ExecBase *)SysBase)->MainInterface;
    IExec->Obtain();
    me = (struct Process *)IExec->FindTask(NULL);

    DOSBase = IExec->OpenLibrary("dos.library", 52);
    IDOS = DOSBase ? (struct DOSIFace *)IExec->GetInterface(DOSBase, "main", 1, NULL) : NULL;
    if (!IDOS) {
        /* senza dos: si consuma il pacchetto per non bloccare Mount */
        IExec->WaitPort(&me->pr_MsgPort);
        IExec->GetMsg(&me->pr_MsgPort);
        goto out;
    }
    UtilityBase = IExec->OpenLibrary("utility.library", 50);
    if (UtilityBase) IUtility = (struct UtilityIFace *)IExec->GetInterface(UtilityBase, "main", 1, NULL);

    plog("==== clib4probe avviato ====");
    plog("== 1. init clib4 senza crt0");
    plog("     pr_CLI=%lx  pr_MsgPort vuota=%s", (ULONG)me->pr_CLI,
         IsMsgPortEmpty(&me->pr_MsgPort) ? "si" : "no (startup in attesa: bene)");

    plog("     pr_HomeDir=%lx pr_CurrentDir=%lx pr_ConsolePort=%08lx pr_WindowPtr=%08lx",
         (ULONG)IDOS->GetProgramDir(), (ULONG)me->pr_CurrentDir,
         (ULONG)me->pr_ConsolePort, (ULONG)me->pr_WindowPtr);

    /* Niente requester: un "inserisci volume PROGDIR:" bloccherebbe Mount.
     * clib4 cerca PROGDIR:.unix e apre CONSOLE:, che in un handler non
     * esistono: meglio un errore che un'attesa infinita. */
    me->pr_WindowPtr = (APTR)-1;

    /* PROGDIR: = cartella dell'handler (da dn_Handler del pacchetto di
     * startup, letto senza toglierlo dalla porta) */
    if (!IDOS->GetProgramDir() && !IsMsgPortEmpty(&me->pr_MsgPort)) {
        struct Message *m = (struct Message *)me->pr_MsgPort.mp_MsgList.lh_Head;
        struct DosPacket *sp = (struct DosPacket *)m->mn_Node.ln_Name;
        struct DeviceNode *sdn = (struct DeviceNode *)BADDR(sp->dp_Arg3);
        char path[256];
        BPTR l = 0;
        path[0] = 0;
        if (sdn) {
            bstr2c(sdn->dn_Handler, path, sizeof(path));
            *(char *)IDOS->PathPart(path) = 0;
            l = IDOS->Lock(path, SHARED_LOCK);
        }
        plog("     PROGDIR: <- \"%s\" %s", path, l ? "OK" : "FAIL");
        if (l) IDOS->SetProgramDir(l);   /* resta al processo fino all'uscita */
    }

    if (!IUtility) { plog("FAIL utility.library"); goto no_clib; }
    if (!do_startup()) goto no_clib;
    startup_done = 1;

    /* niente console ereditata da Mount: stdin/stdout di clib4 su NIL: */
    {
        BPTR nil_in = IDOS->Open("NIL:", MODE_OLDFILE);
        BPTR nil_out = IDOS->Open("NIL:", MODE_NEWFILE);
        IDOS->SetConsolePort(NULL);
        if (nil_in) IDOS->SelectInput(nil_in);
        if (nil_out) IDOS->SelectOutput(nil_out);
        plog("     console azzerata, Input/Output = NIL: (%s)", nil_in && nil_out ? "OK" : "FAIL");
    }

    plog("     OpenLibrary(clib4.library, 2)...");
    Clib4Base = IExec->OpenLibrary("clib4.library", 2);
    if (!Clib4Base) { plog("FAIL clib4.library v2 non trovata"); goto no_clib; }
    plog("OK   clib4.library %ld.%ld", (LONG)Clib4Base->lib_Version, (LONG)Clib4Base->lib_Revision);
    IClib4 = IExec->GetInterface(Clib4Base, "main", 1, NULL);
    if (!IClib4) { plog("FAIL GetInterface(clib4, main, 1)"); goto no_clib; }

    lstart = *(library_start_t *)((UBYTE *)IClib4 + 76);
    /* arg_init di clib4 legge la riga di comando con GetArgStr(), non dai
     * parametri: in un handler e' NULL e _main esce con 20 senza chiamare
     * main. Una riga vuota ("\n", come da Shell) basta. */
    {
        static char noargs[] = "\n";
        STRPTR old_args = IDOS->SetArgStr(noargs);
        plog("     SetArgStr(\"\\n\") (prima: %08lx)", (ULONG)old_args);
        plog("     library_start(%08lx)...", (ULONG)lstart);
        rc = lstart(noargs, 1, probe_main, ctor_list, dtor_list, NULL);
        IDOS->SetArgStr(old_args);
    }
    plog("     library_start ritornata: %ld (main %s)", (LONG)rc,
         main_reached ? "eseguita" : "MAI chiamata");

no_clib:
    if (!startup_done) {
        /* clib4 non e' partita: rispondere al pacchetto di startup */
        struct Message *m = IExec->GetMsg(&me->pr_MsgPort);
        if (m) IDOS->ReplyPkt((struct DosPacket *)m->mn_Node.ln_Name, DOSFALSE,
                              ERROR_OBJECT_NOT_FOUND);
    } else if (!main_reached && startup_port) {
        /* montato ma clib4 non partita: staccarsi dal DeviceNode */
        struct Message *m;
        IExec->Forbid();
        startup_dn->dn_Port = NULL;
        while ((m = IExec->GetMsg(startup_port)))
            IDOS->ReplyPkt((struct DosPacket *)m->mn_Node.ln_Name, DOSFALSE, ERROR_ACTION_NOT_KNOWN);
        IExec->FreeSysObject(ASOT_PORT, startup_port);
        IExec->Permit();
    }
    if (IClib4) IExec->DropInterface(IClib4);
    if (Clib4Base) {
        plog("     CloseLibrary(clib4)...");
        IExec->CloseLibrary(Clib4Base);
        plog("     CloseLibrary(clib4) fatto");
    }
    plog("==== clib4probe uscito ====");
    if (IUtility) IExec->DropInterface((struct Interface *)IUtility);
    if (UtilityBase) IExec->CloseLibrary(UtilityBase);
    IExec->DropInterface((struct Interface *)IDOS);
out:
    if (DOSBase) IExec->CloseLibrary(DOSBase);
    IExec->Release();
    return rc;
}

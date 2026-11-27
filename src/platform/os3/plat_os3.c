/*
 * plat_os3.c -- piattaforma AmigaOS 3.x (m68k, bebbo-gcc, bsdsocket.library)
 *
 * Niente startup code: start.s salta a handler_entry(), che prepara SysBase
 * e DOSBase e passa al nucleo.
 *
 * NG-nfs-handler -- LGPL-2.1
 */
#include "config.h"
#include "aros_compat.h"

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <dos/dosextens.h>
#include <devices/timer.h>
#include <devices/input.h>
#include <devices/inputevent.h>
#include <libraries/locale.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/locale.h>
#include <proto/timer.h>

#include "platform.h"

struct ExecBase   *SysBase;
struct DosLibrary *DOSBase;
struct Device     *TimerBase;

int handler_entry(void);          /* chiamata da start.s */

int handler_entry(void)
{
    struct Process *me;

    SysBase = *(struct ExecBase **)4;
    me = (struct Process *)FindTask(NULL);
    DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 39);
    if (!DOSBase) {
        /* senza dos non si puo' rispondere al pacchetto: lo si consuma */
        WaitPort(&me->pr_MsgPort);
        GetMsg(&me->pr_MsgPort);
        return 0;
    }
    handler_main(NULL);
    CloseLibrary((struct Library *)DOSBase);
    return 0;
}

LONG plat_gmt_offset(void)
{
    LONG off = 0;
    struct Library *LocaleBase = OpenLibrary((CONST_STRPTR)"locale.library", 38);
    if (LocaleBase) {
        struct Locale *loc = OpenLocale(NULL);
        if (loc) {
            off = loc->loc_GMTOffset * 60;      /* minuti a ovest di GMT */
            CloseLocale(loc);
        }
        CloseLibrary(LocaleBase);
    }
    return off;
}

ULONG plat_eclock_open(struct Device *timer)
{
    struct EClockVal ev;
    TimerBase = timer;
    return TimerBase ? ReadEClock(&ev) : 0;
}

void plat_eclock_close(void)
{
    TimerBase = NULL;
}

uint64_t plat_eclock_read(void)
{
    struct EClockVal ev;
    if (!TimerBase) return 0;
    ReadEClock(&ev);
    return ((uint64_t)ev.ev_hi << 32) | ev.ev_lo;
}

int plat_wait(int fd, int events, ULONG *sigs)
{
    fd_set rfds, wfds;
    int rc, revents = 0;

    if (fd < 0) {
        *sigs = Wait(*sigs);
        return 0;
    }
    FD_ZERO(&rfds); FD_ZERO(&wfds);
    if (events & POLLIN)  FD_SET(fd, &rfds);
    if (events & POLLOUT) FD_SET(fd, &wfds);
    rc = WaitSelect(fd + 1, &rfds, &wfds, NULL, NULL, sigs);
    if (rc < 0) { *sigs = 0; return -1; }
    if (rc > 0) {
        if (FD_ISSET(fd, &rfds)) revents |= POLLIN;
        if (FD_ISSET(fd, &wfds)) revents |= POLLOUT;
    }
    return revents;
}

/* OS3: nessun lockhandler; ACTION_DIE con oggetti aperti viene rifiutato */
void *plat_lock_alloc(ULONG size)
{
    return AllocVec(size, MEMF_PUBLIC | MEMF_CLEAR);
}

void plat_lock_free(void *lock)
{
    FreeVec(lock);
}

BOOL plat_collect_ok(void) { return FALSE; }
BOOL plat_collect(LONG type, APTR object) { return FALSE; }

/* OS3: dopo la risposta restano DeleteMsgPort e la distruzione del pool,
 * che non interrompono il Forbid(); poi il processo termina */
void plat_reply_die(struct DosPacket *pkt)
{
    ReplyPkt(pkt, DOSTRUE, 0);
}

/* OS3: l'evento si scrive su input.device (IND_WRITEEVENT) */
void plat_disk_event(BOOL inserted)
{
    struct MsgPort  *mp = CreateMsgPort();
    struct IOStdReq *io = mp ? (struct IOStdReq *)CreateIORequest(mp, sizeof(*io)) : NULL;
    struct InputEvent ie;
    UBYTE *p;
    ULONG i;

    if (io && OpenDevice((CONST_STRPTR)"input.device", 0, (struct IORequest *)io, 0) == 0) {
        for (p = (UBYTE *)&ie, i = 0; i < sizeof(ie); i++) p[i] = 0;
        ie.ie_Class   = inserted ? IECLASS_DISKINSERTED : IECLASS_DISKREMOVED;
        io->io_Command = IND_WRITEEVENT;
        io->io_Data    = &ie;
        io->io_Length  = sizeof(ie);
        DoIO((struct IORequest *)io);
        CloseDevice((struct IORequest *)io);
    }
    if (io) DeleteIORequest((struct IORequest *)io);
    if (mp) DeleteMsgPort(mp);
}

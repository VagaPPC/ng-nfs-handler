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
#include <dos/dosextens.h>
#include <devices/timer.h>
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
    handler_main();
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

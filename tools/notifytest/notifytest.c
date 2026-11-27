/*
 * NotifyTest -- sorveglia un file/directory con StartNotify() e stampa ogni
 * notifica ricevuta (per provare ADD/REMOVE_NOTIFY di nfs-handler).
 *
 *   NotifyTest PATH/A,SECS/N,INITIAL/S
 *
 * Licenza: LGPL-2.1
 * Copyright (C) 2026 VagaPPC (Benedetto Lorello)
 */
#include <exec/types.h>
#include <dos/dos.h>
#include <dos/notify.h>
#include <proto/exec.h>
#include <proto/dos.h>

static const char verstag[] __attribute__((used)) = "$VER: NotifyTest 0.9 (27.11.2026)";

int main(void)
{
    LONG args[3] = { 0, 0, 0 };
    struct RDArgs *rda;
    struct MsgPort *port;
    struct NotifyRequest nr;
    struct DateStamp ds;
    LONG secs, count = 0;
    BPTR timerless;

    rda = ReadArgs((STRPTR)"PATH/A,SECS/N,INITIAL/S", args, NULL);
    if (!rda) { PrintFault(IoErr(), (STRPTR)"NotifyTest"); return RETURN_FAIL; }
    secs = args[1] ? *(LONG *)args[1] : 60;
    (void)timerless;

    port = CreateMsgPort();
    if (!port) { FreeArgs(rda); return RETURN_FAIL; }

    {
        UBYTE *p = (UBYTE *)&nr;
        ULONG i;
        for (i = 0; i < sizeof(nr); i++) p[i] = 0;
    }
    nr.nr_Name  = (UBYTE *)args[0];
    nr.nr_Flags = NRF_SEND_MESSAGE | (args[2] ? NRF_NOTIFY_INITIAL : 0);
    nr.nr_stuff.nr_Msg.nr_Port = port;

    if (!StartNotify(&nr)) {
        PrintFault(IoErr(), (STRPTR)"NotifyTest: StartNotify fallita");
        DeleteMsgPort(port);
        FreeArgs(rda);
        return RETURN_WARN;
    }
    Printf((STRPTR)"Sorveglio %s per %ld secondi (Ctrl-C per uscire)...\n", args[0], secs);

    while (secs > 0) {
        struct NotifyMessage *m;
        ULONG sig;
        /* attesa di 1 s alla volta senza timer.device: Delay + controllo porta */
        Delay(TICKS_PER_SECOND);
        secs--;
        sig = SetSignal(0, SIGBREAKF_CTRL_C);
        if (sig & SIGBREAKF_CTRL_C) { PutStr((STRPTR)"***Break\n"); break; }
        while ((m = (struct NotifyMessage *)GetMsg(port))) {
            DateStamp(&ds);
            count++;
            Printf((STRPTR)"%02ld:%02ld:%02ld  notifica #%ld per %s\n",
                   ds.ds_Minute / 60, ds.ds_Minute % 60, ds.ds_Tick / TICKS_PER_SECOND,
                   count, args[0]);
            ReplyMsg((struct Message *)m);
        }
    }

    EndNotify(&nr);
    {
        struct Message *m;
        while ((m = GetMsg(port))) ReplyMsg(m);
    }
    Printf((STRPTR)"Fine: %ld notifiche ricevute\n", count);
    DeleteMsgPort(port);
    FreeArgs(rda);
    return RETURN_OK;
}

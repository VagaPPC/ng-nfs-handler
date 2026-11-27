/*
 * NFSDismount -- smonta un device gestito da un handler (NG-nfs-handler)
 *
 *   NFSDismount DEVICE/A,KEEPSEG/S
 *
 * Su OS3.1 "Assign X: DISMOUNT" toglie il DeviceNode senza inviare
 * ACTION_DIE (verificato in M0): l'handler resta vivo e orfano. Questo
 * comando invece:
 *   1. invia ACTION_DIE all'handler (se in esecuzione);
 *      se rifiuta (es. ERROR_OBJECT_IN_USE) si ferma e mostra l'errore;
 *   2. aspetta che il task dell'handler sia terminato;
 *   3. toglie il DeviceNode dalla DosList;
 *   4. scarica il codice dell'handler (UnLoadSeg), a meno di KEEPSEG o se
 *      il seglist e' condiviso con un altro device o il task e' ancora vivo.
 *
 * Il DeviceNode viene solo rimosso, non liberato: non sappiamo con certezza
 * come l'ha allocato Mount, e FreeDosEntry su memoria non sua corromperebbe
 * la memoria (perdita accettata: ~100 byte per dismount).
 *
 * Licenza: LGPL-2.1
 * Copyright (C) 2026 VagaPPC (Benedetto Lorello)
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

static const char verstag[] __attribute__((used)) =
    "$VER: NFSDismount 0.9 (27.11.2026)";

extern struct ExecBase *SysBase;

static BOOL task_exists(struct Task *t)
{
    struct Node *n;
    BOOL found = FALSE;

    if (!t) return FALSE;
    Forbid();
    if ((struct Task *)SysBase->ThisTask == t) found = TRUE;
    for (n = SysBase->TaskReady.lh_Head; !found && n->ln_Succ; n = n->ln_Succ)
        if ((struct Task *)n == t) found = TRUE;
    for (n = SysBase->TaskWait.lh_Head; !found && n->ln_Succ; n = n->ln_Succ)
        if ((struct Task *)n == t) found = TRUE;
    Permit();
    return found;
}

static BOOL seg_shared(BPTR seg)
{
    struct DosList *dl = LockDosList(LDF_DEVICES | LDF_READ);
    BOOL shared = FALSE;
    while ((dl = NextDosEntry(dl, LDF_DEVICES)))
        if (dl->dol_misc.dol_handler.dol_SegList == seg) { shared = TRUE; break; }
    UnLockDosList(LDF_DEVICES | LDF_READ);
    return shared;
}

int main(void)
{
    LONG   args[2] = { 0, 0 };
    struct RDArgs *rda;
    char   name[64];
    struct DosList *dl, *node;
    struct MsgPort *port;
    struct Task *htask = NULL;
    BPTR   seg;
    int    i, rc = RETURN_OK;

    rda = ReadArgs((STRPTR)"DEVICE/A,KEEPSEG/S", args, NULL);
    if (!rda) {
        PrintFault(IoErr(), (STRPTR)"NFSDismount");
        return RETURN_FAIL;
    }

    /* nome senza ':' finale */
    for (i = 0; i < (int)sizeof(name) - 1 && ((char *)args[0])[i]; i++)
        name[i] = ((char *)args[0])[i];
    name[i] = 0;
    while (i > 0 && name[i - 1] == ':') name[--i] = 0;
    if (!name[0]) {
        PutStr((STRPTR)"NFSDismount: nome device mancante\n");
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    dl = LockDosList(LDF_DEVICES | LDF_READ);
    node = FindDosEntry(dl, (STRPTR)name, LDF_DEVICES);
    port = node ? node->dol_Task : NULL;
    if (port && (port->mp_Flags & PF_ACTION) == PA_SIGNAL)
        htask = (struct Task *)port->mp_SigTask;
    UnLockDosList(LDF_DEVICES | LDF_READ);

    if (!node) {
        Printf((STRPTR)"NFSDismount: device %s: non trovato\n", (LONG)name);
        FreeArgs(rda);
        return RETURN_WARN;
    }

    /* 1. ACTION_DIE (senza tenere bloccata la DosList) */
    if (port) {
        if (!DoPkt(port, ACTION_DIE, 0, 0, 0, 0, 0)) {
            LONG err = IoErr();
            Printf((STRPTR)"NFSDismount: %s: l'handler ha rifiutato lo smontaggio\n",
                   (LONG)name);
            PrintFault(err, (STRPTR)"NFSDismount");
            FreeArgs(rda);
            return RETURN_WARN;
        }
    }

    /* 2. attesa fine del task dell'handler (max ~5 s) */
    for (i = 0; i < 250 && task_exists(htask); i++)
        Delay(1);
    if (task_exists(htask)) {
        Printf((STRPTR)"NFSDismount: %s: l'handler e' ancora in esecuzione, "
               "il codice non verra' scaricato\n", (LONG)name);
        args[1] = TRUE;
        rc = RETURN_WARN;
    }

    /* 3. rimozione del DeviceNode (ricontrollato sotto lock: nel frattempo
     *    qualcun altro potrebbe averlo tolto) */
    dl = LockDosList(LDF_DEVICES | LDF_WRITE);
    while ((dl = NextDosEntry(dl, LDF_DEVICES)) && dl != node)
        ;
    if (!dl) {
        UnLockDosList(LDF_DEVICES | LDF_WRITE);
        Printf((STRPTR)"NFSDismount: %s: gia' rimosso da altri\n", (LONG)name);
        FreeArgs(rda);
        return rc;
    }
    seg = node->dol_misc.dol_handler.dol_SegList;
    RemDosEntry(node);
    node->dol_misc.dol_handler.dol_SegList = 0;
    node->dol_Task = NULL;
    UnLockDosList(LDF_DEVICES | LDF_WRITE);

    /* 4. scarica il codice dell'handler */
    if (seg && !args[1] && !seg_shared(seg)) {
        UnLoadSeg(seg);
        Printf((STRPTR)"%s: smontato\n", (LONG)name);
    } else {
        Printf((STRPTR)"%s: smontato (codice handler mantenuto in memoria)\n",
               (LONG)name);
    }

    FreeArgs(rda);
    return rc;
}

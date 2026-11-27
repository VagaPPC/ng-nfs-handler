/*
 * platform.h -- interfaccia tra il nucleo dell'handler e la piattaforma
 *
 * Il nucleo (src/dos/nfs-handler.c) usa exec/dos con le chiamate classiche;
 * qui sta solo cio' che cambia tra AmigaOS 3.x, OS4, MorphOS e AROS:
 *   - punto d'ingresso e basi/interfacce delle librerie;
 *   - fuso orario (locale.library), orologio EClock (timer.device);
 *   - attesa contemporanea di socket NFS e segnali exec.
 *
 * Ogni piattaforma ha la sua implementazione in src/platform/<os>/.
 *
 * NG-nfs-handler -- LGPL-2.1
 */
#ifndef NG_PLATFORM_H
#define NG_PLATFORM_H

#include <exec/types.h>
#include <stdint.h>

/* Nucleo dell'handler: chiamato dal punto d'ingresso della piattaforma
 * quando exec, dos e la libc sono pronti; gira fino ad ACTION_DIE /
 * ACTION_SHUTDOWN (o handler orfano) e ritorna.
 *
 * hs == NULL: il nucleo legge il pacchetto di startup da pr_MsgPort e
 * risponde dopo aver letto le opzioni (OS3: Mount riceve un errore se la
 * Startup e' sbagliata).
 * hs != NULL: la piattaforma ha gia' preso il pacchetto, creato la porta
 * dei pacchetti, messa in dn_Port e risposto (OS4: va fatto prima di aprire
 * clib4, altrimenti Mount e l'apertura della libreria si bloccano a
 * vicenda). Il pacchetto non esiste piu': ne restano solo i dati copiati. */
struct DeviceNode;
struct MsgPort;
struct HandlerStartup {
    struct DeviceNode *devnode;
    struct MsgPort    *port;        /* creata con CreateMsgPort() */
    char               devname[64]; /* da dp_Arg1, senza ':' */
};
void handler_main(const struct HandlerStartup *hs);

/* Minuti a ovest di GMT * 60 (da locale.library); 0 se non disponibile. */
LONG plat_gmt_offset(void);

/* EClock per le misure del log. plat_eclock_open riceve il timer.device gia'
 * aperto (io_Device) e ritorna la frequenza, 0 se non disponibile. */
ULONG    plat_eclock_open(struct Device *timer);
void     plat_eclock_close(void);
uint64_t plat_eclock_read(void);

/* Aspetta eventi sul socket fd (events = POLLIN|POLLOUT) e/o i segnali in
 * *sigs. In uscita *sigs contiene i segnali arrivati. Ritorna gli eventi del
 * socket (POLLIN|POLLOUT, 0 se nessuno) o -1 se l'attesa sul socket e'
 * fallita. Con fd < 0 aspetta solo i segnali. */
int plat_wait(int fd, int events, ULONG *sigs);

/* Evento globale "disco inserito/tolto" (IECLASS_DISKINSERTED/REMOVED),
 * da mandare dopo aver aggiunto/tolto il volume: il Workbench aggiorna
 * le icone. */
void plat_disk_event(BOOL inserted);

/* Lock pubblici dell'handler (struct NLock, con la FileLock in testa).
 * OS4: allocati con AllocDosObject(DOS_LOCK), condizione perche' il
 * lockhandler del DOS li accetti con ACTION_COLLECT. Memoria azzerata. */
void *plat_lock_alloc(ULONG size);
void  plat_lock_free(void *lock);

/* Smontaggio con oggetti aperti (OS4, dos.dospackets/ACTION_SHUTDOWN):
 * plat_collect_ok() dice se la piattaforma ha il lockhandler; plat_collect()
 * gli cede un oggetto pubblico e ritorna TRUE se accettato. */
#define PLAT_COLLECT_LOCK        1
#define PLAT_COLLECT_FILEHANDLE  2
#define PLAT_COLLECT_NOTIFY      3
BOOL plat_collect_ok(void);
BOOL plat_collect(LONG type, APTR object);

/* Risposta al pacchetto di smontaggio (ACTION_DIE / ACTION_SHUTDOWN), da
 * chiamare sotto Forbid() con dn_Task gia' azzerato. Dopo la risposta chi
 * smonta puo' scaricare il segmento dell'handler: non si deve piu' eseguire
 * codice che possa interrompere il Forbid(). OS3: risponde subito (dopo
 * resta solo la distruzione del pool). OS4: la risposta viene rimandata alla
 * fine di plat_start, dopo l'uscita di clib4 (che chiude file e librerie e
 * puo' fare Wait()). */
void plat_reply_die(struct DosPacket *pkt);

#endif

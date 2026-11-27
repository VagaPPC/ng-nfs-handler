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
 * quando exec e dos sono pronti. Legge il pacchetto di startup da
 * pr_MsgPort, gira fino ad ACTION_DIE (o handler orfano) e ritorna. */
void handler_main(void);

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

#endif

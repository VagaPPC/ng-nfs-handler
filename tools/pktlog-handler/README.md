# pktlog-handler (M0)

Handler DOS di test: monta un volume vuoto in sola lettura e scrive nel log
ogni pacchetto ricevuto (chi lo invia, argomenti, risposta). Serve a chiarire
tre punti prima di scrivere il vero nfs-handler:

1. **`Assign DISMOUNT` invia `ACTION_DIE`?** (OS3.1)
2. Come `Mount` passa la stringa `Startup` (con e senza virgolette).
3. Quali pacchetti inviano Workbench, `Dir`, `List`, `Info`, `CD` e gli altri programmi.

## Build

```
make PLATFORM=os3        # -> build/os3/pktlog-handler
```

## Installazione su Amiga (OS3.1)

```
Copy pktlog-handler L:
Copy PKT0 PKT1 DEVS:DOSDrivers/   ; oppure in RAM: per non toccare il boot
```

## Procedura di test

Il log è in `RAM:pktlog.log` e si può leggere mentre l'handler gira
(`Type RAM:pktlog.log`). Ogni 10 s l'handler controlla se il suo DeviceNode è
ancora nella DosList; ogni 60 s scrive una riga `alive`.

### T1 — Mount e Startup
```
Mount DEVS:DOSDrivers/PKT0
Mount DEVS:DOSDrivers/PKT1
Type RAM:pktlog.log
```
Annotare le righe `dn_Startup come BSTR` e `primi byte`: dicono se le
virgolette arrivano all'handler (`22` in esadecimale = `"`) e se c'è un `\n`
finale (`0a`).

### T2 — Comandi base
```
Info PKTLOG:
Dir PKTLOG:
List PKTLOG:
CD PKTLOG:
CD SYS:
Type PKTLOG:nonesiste
```

### T3 — Workbench
Aprire il volume PKTLOG da Workbench, chiudere la finestra, fare
"Aggiorna" (Update). Annotare `ADD_NOTIFY` / `REMOVE_NOTIFY` / `EXAMINE_ALL`.

### T4 — Dismount (il test decisivo)
Chiudere tutte le finestre e fare `CD` fuori da PKT0:, poi:
```
Assign PKT0: DISMOUNT
Wait 15
Type RAM:pktlog.log
```
Esiti possibili nel log:
- `<- DIE` seguito da `==== ACTION_DIE accettato, uscita ====` → **i comandi
  nativi bastano**.
- `!! ORFANO: il DeviceNode PKT0: e' stato RIMOSSO dalla DosList senza
  ACTION_DIE` → **serve un tool di dismount nostro** su OS3.1.
- Nessuna delle due righe, ma `alive` continua → annotare l'output di `Info` e
  di `Assign LIST`.

Rifare il test **con un lock aperto** (es. `CD PKTLOG:` in un'altra shell): il
DIE, se arriva, deve essere rifiutato con `res2=202` (ERROR_OBJECT_IN_USE).

### T5 — Dopo il dismount
```
Dir PKT0:
```
Se il DeviceNode è ancora presente con `dn_Task = NULL`, il DOS riavvia
l'handler (compare un nuovo `==== avviato ====`).

## Da riportare

Copiare `RAM:pktlog.log` dopo ogni test (es. `Copy RAM:pktlog.log
SYS:pktlog-T4.log`) e annotare la versione di OS e Workbench usata.

Se possibile, ripetere T4 anche su OS3.9/3.2, OS4.1, MorphOS e AROS: il
binario 68k gira anche su OS4 (emulazione) e su MorphOS (Trance), ma il
risultato più affidabile si ottiene con il comando nativo di ciascun sistema.

/*
 * bigtest -- prova dei pacchetti a 64 bit di OS4 su nfs-handler
 *
 *   bigtest FILE/A,WFILE/K
 *
 * FILE: file grande (es. NAS:big/grande.bin, 5 GB sparso con marcatori
 *       "NGNFS@<offset>" a offset noti). Prova ExamineObject (EXAMINEDATA e
 *       EXAMINEDATA_FH), GetFileSize, ChangeFilePosition e letture oltre i
 *       2 e 4 GB, GetFilePosition.
 * WFILE: file da creare per provare ChangeFileSize a 64 bit (3 GB sparso),
 *        poi cancellato.
 *
 * Solo AmigaOS 4 (dos.library 53+). Stampa con printf() di newlib:
 * IDOS->Printf() con %lld sfasa gli argomenti successivi (crash).
 * NG-nfs-handler -- LGPL-2.1
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dos.h>
#include <string.h>
#include <stdio.h>

static const char verstag[] __attribute__((used)) = "$VER: bigtest 0.1 (27.11.2026)";

static int fails;

static void ok(BOOL cond, CONST_STRPTR what)
{
    printf("%s %s\n", cond ? "OK  " : "FAIL", what);
    if (!cond) fails++;
}

static void marker(BPTR fh, int64 off)
{
    char buf[48];
    int32 n;
    char expect[24];
    BOOL pos_ok = IDOS->ChangeFilePosition(fh, off, OFFSET_BEGINNING);
    printf("     ChangeFilePosition(%lld) = %ld, GetFilePosition = %lld\n",
                 (long long)off, (long)pos_ok, (long long)IDOS->GetFilePosition(fh));
    memset(buf, 0, sizeof(buf));
    n = IDOS->Read(fh, buf, 40);
    buf[n > 0 ? n : 0] = 0;
    printf("     Read = %ld: \"%s\"\n", (long)n, buf);
    snprintf(expect, sizeof(expect), "NGNFS@%010lld", (long long)off);
    ok(pos_ok && n > 0 && !strncmp(buf, expect, strlen(expect)), expect);
}

int main(void)
{
    int32 args[2] = { 0, 0 };
    struct RDArgs *rda = IDOS->ReadArgs("FILE/A,WFILE/K", args, NULL);
    CONST_STRPTR file, wfile;
    struct ExamineData *d;
    BPTR fh;
    int64 size;

    setvbuf(stdout, NULL, _IONBF, 0);   /* niente righe perse se cade */
    if (!rda) { IDOS->PrintFault(IDOS->IoErr(), "bigtest"); return RETURN_FAIL; }
    file  = (CONST_STRPTR)args[0];
    wfile = (CONST_STRPTR)args[1];

    printf("== ExamineObject per nome (ACTION_EXAMINEDATA)\n");
    d = IDOS->ExamineObjectTags(EX_StringNameInput, file, TAG_END);
    if (d) {
        printf("     Name=\"%s\" FileSize=%lld Type=%lx emulato=%s\n", d->Name,
               (long long)d->FileSize, (unsigned long)d->Type, EXD_IS_EMULATED(d) ? "si" : "no");
        ok(d->FileSize > 0x100000000LL, "dimensione oltre i 4 GB");
        ok(!EXD_IS_EMULATED(d), "risposta nativa (non emulata dal DOS)");
        IDOS->FreeDosObject(DOS_EXAMINEDATA, d);
    } else ok(FALSE, "ExamineObject");

    fh = IDOS->Open(file, MODE_OLDFILE);
    if (!fh) { IDOS->PrintFault(IDOS->IoErr(), file); IDOS->FreeArgs(rda); return RETURN_FAIL; }

    printf("== ExamineObject sul file aperto (ACTION_EXAMINEDATA_FH)\n");
    d = IDOS->ExamineObjectTags(EX_FileHandleInput, fh, TAG_END);
    if (d) {
        printf("     FileSize=%lld emulato=%s\n", (long long)d->FileSize,
                     EXD_IS_EMULATED(d) ? "si" : "no");
        ok(d->FileSize > 0x100000000LL, "dimensione oltre i 4 GB (FH)");
        IDOS->FreeDosObject(DOS_EXAMINEDATA, d);
    } else ok(FALSE, "ExamineObject FH");

    printf("== GetFileSize (ACTION_GET_FILE_SIZE64)\n");
    size = IDOS->GetFileSize(fh);
    printf("     GetFileSize = %lld\n", (long long)size);
    ok(size > 0x100000000LL, "GetFileSize oltre i 4 GB");

    printf("== Letture ai marcatori (ACTION_CHANGE_FILE_POSITION64)\n");
    marker(fh, 0);
    marker(fh, 2147483640LL);
    marker(fh, 4294967290LL);
    marker(fh, 5368709097LL);
    printf("== Oltre la fine (deve fallire)\n");
    ok(!IDOS->ChangeFilePosition(fh, size + 10, OFFSET_BEGINNING), "seek oltre EOF rifiutato");
    IDOS->Close(fh);

    if (wfile) {
        printf("== ChangeFileSize a 3 GB (ACTION_CHANGE_FILE_SIZE64)\n");
        fh = IDOS->Open(wfile, MODE_NEWFILE);
        if (fh) {
            int64 want = 3LL * 1024 * 1024 * 1024;
            BOOL r = IDOS->ChangeFileSize(fh, want, OFFSET_BEGINNING);
            int64 got = IDOS->GetFileSize(fh);
            printf("     ChangeFileSize = %ld, GetFileSize = %lld\n", (long)r, (long long)got);
            ok(r && got == want, "file portato a 3 GB");
            IDOS->Close(fh);
            ok(IDOS->Delete(wfile), "file di prova cancellato");
        } else ok(FALSE, "apertura WFILE");
    }

    printf("== %s (%d errori)\n", fails ? "FALLITO" : "TUTTO OK", fails);
    IDOS->FreeArgs(rda);
    return fails ? RETURN_WARN : RETURN_OK;
}

# NG-nfs-handler -- build incrociata Linux -> Amiga
#
#   make                      -> PLATFORM=os3 (default)
#   make PLATFORM=os3 pktlog  -> build/os3/pktlog-handler
#   make clean
#
#   make PLATFORM=os4 handler -> build/os4/nfs-handler (clib4)
#   make PLATFORM=os4 probe   -> build/os4/clib4probe
#
# Piattaforme previste: os3 | os4 | mos | aros
# Implementate: os3, os4.

PLATFORM ?= os3
BUILD    := build/$(PLATFORM)

# ---------------------------------------------------------------- os3 ----
ifeq ($(PLATFORM),os3)
# bebbo amiga-gcc: /opt/amiga e' il prefisso standard; altrimenti
# make M68K_PREFIX=/percorso/del/toolchain
M68K_GCC    := $(firstword $(wildcard /opt/amiga/bin/m68k-amigaos-gcc) \
               $(wildcard $(HOME)/toolchains/m68k-amigaos/opt/appveyor/build-agent/opt/amiga/bin/m68k-amigaos-gcc))
M68K_PREFIX ?= $(patsubst %/bin/m68k-amigaos-gcc,%,$(M68K_GCC))
CC          := $(M68K_PREFIX)/bin/m68k-amigaos-gcc
CPUFLAGS    := -m68020
# Handler: niente startup code ne' libc, entry = prima funzione del codice
HANDLER_CFLAGS  := -Os $(CPUFLAGS) -noixemul -Wall -Wextra -Wno-unused-parameter \
                   -fomit-frame-pointer -fno-toplevel-reorder -fno-reorder-functions \
                   -fno-builtin -ffreestanding
HANDLER_LDFLAGS := -noixemul -nostartfiles -nostdlib \
                   -L$(M68K_PREFIX)/m68k-amigaos/lib -L$(M68K_PREFIX)/m68k-amigaos/libnix/lib
HANDLER_LIBS    := -lgcc
# Comandi CLI: startup libnix normale
TOOL_CFLAGS     := -Os $(CPUFLAGS) -noixemul -Wall -Wextra -fomit-frame-pointer
TOOL_LDFLAGS    := -noixemul \
                   -L$(M68K_PREFIX)/m68k-amigaos/lib -L$(M68K_PREFIX)/m68k-amigaos/libnix/lib
# libnfs: compat POSIX nostro (bsdsocket + minilibc)
COMPAT_DIR  := src/net
COMPAT_SRC  := src/net/amiga_compat.c
OPTFLAGS    := -Os $(CPUFLAGS) -noixemul -fomit-frame-pointer
AR          := $(M68K_PREFIX)/bin/m68k-amigaos-ar
endif

ifeq ($(PLATFORM),os4)
# adtools ppc-amigaos-gcc con clib4 (2.x, l'SDK di clib4 in SDK/clib4)
OS4_GCC := $(firstword $(wildcard /opt/ppc-amigaos/bin/ppc-amigaos-gcc) \
           $(wildcard $(HOME)/toolchains/bin/ppc-amigaos-gcc) ppc-amigaos-gcc)
OS4_SDK ?= $(firstword $(wildcard /opt/ppc-amigaos/ppc-amigaos/SDK) \
           $(wildcard $(HOME)/toolchains/ppc-amigaos-rootfs/opt/ppc-amigaos/ppc-amigaos/SDK))
CC      := $(OS4_GCC)
AR      := $(patsubst %-gcc,%-ar,$(OS4_GCC))
STRIP   := $(patsubst %-gcc,%-strip,$(OS4_GCC))
# libnfs: clib4 e' quasi POSIX, il compat sta in src/platform/os4
COMPAT_DIR := src/platform/os4
COMPAT_SRC :=
OPTFLAGS   := -O2 -mcrt=clib4 -D__USE_INLINE__ -Wno-deprecated-declarations
endif
ifeq ($(PLATFORM),mos)
$(error PLATFORM=mos non ancora implementato)
endif
ifeq ($(PLATFORM),aros)
$(error PLATFORM=aros non ancora implementato)
endif

# ---------------------------------------------------------------- target --

# ---------------------------------------------------------------- libnfs --
# libnfs (vendorizzata, sorgenti intatti) compilata con -DAROS e il nostro
# src/net/aros_compat.h in testa al path degli include.
LIBNFS_DIRS := lib mount nfs nfs4 portmap nlm nsm rquota
LIBNFS_SRC  := $(filter-out libnfs/lib/krb5-wrapper.c, \
               $(foreach d,$(LIBNFS_DIRS),$(wildcard libnfs/$(d)/*.c))) \
               $(COMPAT_SRC)
LIBNFS_OBJ  := $(patsubst %.c,$(BUILD)/obj/%.o,$(LIBNFS_SRC))
LIBNFS_INC  := -I$(COMPAT_DIR) -Ilibnfs/include -Ilibnfs/include/nfsc \
               $(foreach d,mount nfs nfs4 portmap nlm nsm rquota,-Ilibnfs/$(d))
LIBNFS_CFLAGS := $(OPTFLAGS) -DAROS -DHAVE_CONFIG_H $(LIBNFS_INC) \
                 -include $(COMPAT_DIR)/config.h

$(BUILD)/obj/%.o: %.c $(COMPAT_DIR)/aros_compat.h $(COMPAT_DIR)/config.h
	@mkdir -p $(dir $@)
	$(CC) $(LIBNFS_CFLAGS) -c $< -o $@

$(BUILD)/libnfs.a: $(LIBNFS_OBJ)
	rm -f $@
	$(AR) rcs $@ $^

all: pktlog nfsdismount nfsls handler notifytest

notifytest: $(BUILD)/NotifyTest

$(BUILD)/NotifyTest: tools/notifytest/notifytest.c | $(BUILD)
	$(CC) $(TOOL_CFLAGS) $(TOOL_LDFLAGS) -s -o $@ $<

# ------------------------------------------------------------- nfs-handler --
# Nucleo comune (src/dos/) + strato di piattaforma (src/platform/<os>/).
# OS3: niente startup code ne' libnix: start.s (entry) + handler + libnfs +
# minilibc.
# OS4: plat_os4.o per primo (_start, liste .ctors/.dtors) + clib4 senza crt0
# + crtend.o per ultimo.
PLAT_DIR    := src/platform/$(PLATFORM)
PLAT_INC    := -Isrc/platform -Isrc/libc
ifeq ($(PLATFORM),os4)
HANDLER_OBJ := $(BUILD)/obj/$(PLAT_DIR)/plat_$(PLATFORM).o \
               $(BUILD)/obj/src/dos/nfs-handler.o
else
HANDLER_OBJ := $(BUILD)/obj/$(PLAT_DIR)/start.o \
               $(BUILD)/obj/src/dos/nfs-handler.o \
               $(BUILD)/obj/$(PLAT_DIR)/plat_$(PLATFORM).o
endif
HANDLER_DEPS := $(COMPAT_DIR)/aros_compat.h $(COMPAT_DIR)/config.h src/libc/minilibc.h \
                src/platform/platform.h

$(BUILD)/obj/$(PLAT_DIR)/start.o: $(PLAT_DIR)/start.s
	@mkdir -p $(dir $@)
	$(CC) $(CPUFLAGS) -c $< -o $@

$(BUILD)/obj/src/dos/nfs-handler.o: src/dos/nfs-handler.c $(HANDLER_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(LIBNFS_CFLAGS) $(PLAT_INC) -Wall -Wextra -Wno-unused-parameter -c $< -o $@

$(BUILD)/obj/$(PLAT_DIR)/plat_$(PLATFORM).o: $(PLAT_DIR)/plat_$(PLATFORM).c $(HANDLER_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(LIBNFS_CFLAGS) $(PLAT_INC) -Wall -Wextra -Wno-unused-parameter -c $< -o $@

# minilibc: niente builtin/trasformazioni in memset/memcpy (ricorsione!)
$(BUILD)/obj/src/libc/minilibc.o: src/libc/minilibc.c src/libc/minilibc.h
	@mkdir -p $(dir $@)
	$(CC) -Os $(CPUFLAGS) -noixemul -fomit-frame-pointer -ffreestanding -fno-builtin \
	      -fno-tree-loop-distribute-patterns -Isrc/libc -Wall -c $< -o $@

handler: $(BUILD)/nfs-handler

ifeq ($(PLATFORM),os4)
$(BUILD)/nfs-handler: $(HANDLER_OBJ) $(BUILD)/libnfs.a
	$(CC) -mcrt=clib4 -nostartfiles -o $@ \
	      $(HANDLER_OBJ) $(BUILD)/libnfs.a $(OS4_SDK)/clib4/lib/crtend.o
else
# start.o DEVE essere il primo oggetto
$(BUILD)/nfs-handler: $(HANDLER_OBJ) $(BUILD)/obj/src/libc/minilibc.o $(BUILD)/libnfs.a
	$(CC) $(CPUFLAGS) -noixemul -nostartfiles -nostdlib -o $@ \
	      $(HANDLER_OBJ) $(BUILD)/libnfs.a $(BUILD)/obj/src/libc/minilibc.o -lgcc
# (minilibc.o anche dopo libnfs.a: risolve i simboli richiesti dalla libreria)
endif

nfsls: $(BUILD)/nfsls

$(BUILD)/nfsls: tools/nfsls/nfsls.c $(BUILD)/libnfs.a
	$(CC) $(TOOL_CFLAGS) $(LIBNFS_INC) -DAROS -DHAVE_CONFIG_H $(TOOL_LDFLAGS) \
	      -o $@ $(M68K_PREFIX)/m68k-amigaos/libnix/lib/swapstack.o $< $(BUILD)/libnfs.a

# ------------------------------------------------------------ clib4probe --
# OS4: handler di prova con clib4 senza crt0 (vedi tools/clib4probe/).
# crtend.o chiude le liste .ctors/.dtors aperte in clib4probe.c.
probe: $(BUILD)/clib4probe

$(BUILD)/clib4probe: tools/clib4probe/clib4probe.c | $(BUILD)
	$(CC) -mcrt=clib4 -O2 -Wall -Wextra -Wno-unused-parameter -nostartfiles \
	      -o $@ $< $(OS4_SDK)/clib4/lib/crtend.o

# OS4: prova dei pacchetti a 64 bit (vedi tools/bigtest/)
bigtest: $(BUILD)/bigtest

$(BUILD)/bigtest: tools/bigtest/bigtest.c | $(BUILD)
	$(CC) -O2 -Wall -o $@ $<

pktlog: $(BUILD)/pktlog-handler
nfsdismount: $(BUILD)/NFSDismount

$(BUILD):
	mkdir -p $@

# start.s DEVE essere il primo oggetto: e' il punto d'ingresso dell'handler
PKTLOG_SRC := tools/pktlog-handler/start.s tools/pktlog-handler/pktlog-handler.c

$(BUILD)/pktlog-handler: $(PKTLOG_SRC) | $(BUILD)
	$(CC) $(HANDLER_CFLAGS) $(HANDLER_LDFLAGS) -o $@ $(PKTLOG_SRC) $(HANDLER_LIBS)

$(BUILD)/NFSDismount: tools/nfsdismount/nfsdismount.c | $(BUILD)
	$(CC) $(TOOL_CFLAGS) $(TOOL_LDFLAGS) -o $@ $<

# ------------------------------------------------------------------- dist --
# Archivio di rilascio unico OS3 + OS4: binari (senza simboli), mountlist
# con icona per ciascun sistema, documentazione, licenze e sorgenti completi
# (richiesti dalla LGPL). Ricompila entrambe le piattaforme con sub-make.
# In Source/ vanno SOLO i file tracciati da git: le mountlist con i dati
# della rete locale (test/*/local/, escluse da git) non devono uscire.
DIST_TOP  := build/dist
DIST_DIR  := $(DIST_TOP)/NG-nfs-handler
# Toolchain dai prefissi gia' usati per compilare (M68K_PREFIX, OS4_SDK),
# cosi' "make dist M68K_PREFIX=... OS4_SDK=..." vale anche qui.
OS3_STRIP := $(if $(M68K_PREFIX),$(M68K_PREFIX)/bin/m68k-amigaos-strip,m68k-amigaos-strip)
OS4_STRIP := $(firstword $(wildcard /opt/ppc-amigaos/bin/ppc-amigaos-strip) \
             $(wildcard $(HOME)/toolchains/bin/ppc-amigaos-strip) ppc-amigaos-strip)
DIST_OS4_SDK := $(or $(OS4_SDK),$(firstword $(wildcard /opt/ppc-amigaos/ppc-amigaos/SDK) \
                $(wildcard $(HOME)/toolchains/ppc-amigaos-rootfs/opt/ppc-amigaos/ppc-amigaos/SDK)))
CLIB4_LICENSE := $(DIST_OS4_SDK)/clib4/LICENSE

dist:
	@command -v $(OS3_STRIP) >/dev/null || { echo "m68k-amigaos-strip non trovato: make dist M68K_PREFIX=..."; exit 1; }
	@test -f $(CLIB4_LICENSE) || { echo "LICENSE di clib4 non trovata: make dist OS4_SDK=..."; exit 1; }
	$(MAKE) PLATFORM=os3 nfsdismount nfsls handler notifytest
	$(MAKE) PLATFORM=os4 handler
	rm -rf $(DIST_TOP) build/NG-nfs-handler.lha
	mkdir -p $(DIST_DIR)/OS3/L $(DIST_DIR)/OS3/C $(DIST_DIR)/OS3/Tools \
	         $(DIST_DIR)/OS4/L $(DIST_DIR)/Docs $(DIST_DIR)/Source
	cp build/os3/nfs-handler $(DIST_DIR)/OS3/L/
	cp build/os3/NFSDismount $(DIST_DIR)/OS3/C/
	cp build/os3/nfsls build/os3/NotifyTest $(DIST_DIR)/OS3/Tools/
	$(OS3_STRIP) $(DIST_DIR)/OS3/L/nfs-handler $(DIST_DIR)/OS3/C/NFSDismount \
	             $(DIST_DIR)/OS3/Tools/nfsls $(DIST_DIR)/OS3/Tools/NotifyTest
	cp build/os4/nfs-handler $(DIST_DIR)/OS4/L/
	$(OS4_STRIP) $(DIST_DIR)/OS4/L/nfs-handler
	cp -r dist/OS3/Storage $(DIST_DIR)/OS3/
	cp -r dist/OS4/Storage $(DIST_DIR)/OS4/
	cp doc/nfs-handler.txt doc/nfs-handler.it.txt COPYING $(DIST_DIR)/Docs/
	cp libnfs/COPYING $(DIST_DIR)/Docs/COPYING.libnfs
	cp $(CLIB4_LICENSE) $(DIST_DIR)/Docs/COPYING.clib4
	cp dist/NG-nfs-handler.readme $(DIST_TOP)/
	git ls-files -z -- Makefile README.md COPYING src tools libnfs test doc dist | \
	    xargs -0 cp --parents -t $(DIST_DIR)/Source/
	cd $(DIST_TOP) && lha -aq ../NG-nfs-handler.lha NG-nfs-handler NG-nfs-handler.readme
	@echo "Creato build/NG-nfs-handler.lha"

clean:
	rm -rf build

.PHONY: probe bigtest all pktlog nfsdismount nfsls handler notifytest dist clean

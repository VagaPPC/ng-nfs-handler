# NG-nfs-handler -- build incrociata Linux -> Amiga
#
#   make                      -> PLATFORM=os3 (default)
#   make PLATFORM=os3 pktlog  -> build/os3/pktlog-handler
#   make clean
#
# Piattaforme previste: os3 | os4 | mos | aros | linux (solo test VFS)
# Per ora e' implementato solo os3.

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
endif

ifeq ($(PLATFORM),os4)
$(error PLATFORM=os4 non ancora implementato)
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
               src/net/amiga_compat.c
LIBNFS_OBJ  := $(patsubst %.c,$(BUILD)/obj/%.o,$(LIBNFS_SRC))
LIBNFS_INC  := -Isrc/net -Ilibnfs/include -Ilibnfs/include/nfsc \
               $(foreach d,mount nfs nfs4 portmap nlm nsm rquota,-Ilibnfs/$(d))
LIBNFS_CFLAGS := -Os $(CPUFLAGS) -noixemul -fomit-frame-pointer \
                 -DAROS -DHAVE_CONFIG_H $(LIBNFS_INC) -include src/net/config.h

$(BUILD)/obj/%.o: %.c src/net/aros_compat.h src/net/config.h
	@mkdir -p $(dir $@)
	$(CC) $(LIBNFS_CFLAGS) -c $< -o $@

$(BUILD)/libnfs.a: $(LIBNFS_OBJ)
	rm -f $@
	$(M68K_PREFIX)/bin/m68k-amigaos-ar rcs $@ $^

all: pktlog nfsdismount nfsls handler notifytest

notifytest: $(BUILD)/NotifyTest

$(BUILD)/NotifyTest: tools/notifytest/notifytest.c | $(BUILD)
	$(CC) $(TOOL_CFLAGS) $(TOOL_LDFLAGS) -s -o $@ $<

# ------------------------------------------------------------- nfs-handler --
# Nucleo comune (src/dos/) + strato di piattaforma (src/platform/<os>/).
# OS3: niente startup code ne' libnix: start.s (entry) + handler + libnfs +
# minilibc.
PLAT_DIR    := src/platform/$(PLATFORM)
PLAT_INC    := -Isrc/platform -Isrc/libc
HANDLER_OBJ := $(BUILD)/obj/$(PLAT_DIR)/start.o \
               $(BUILD)/obj/src/dos/nfs-handler.o \
               $(BUILD)/obj/$(PLAT_DIR)/plat_$(PLATFORM).o
HANDLER_DEPS := src/net/aros_compat.h src/net/config.h src/libc/minilibc.h \
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

# start.o DEVE essere il primo oggetto
$(BUILD)/nfs-handler: $(HANDLER_OBJ) $(BUILD)/obj/src/libc/minilibc.o $(BUILD)/libnfs.a
	$(CC) $(CPUFLAGS) -noixemul -nostartfiles -nostdlib -o $@ \
	      $(HANDLER_OBJ) $(BUILD)/libnfs.a $(BUILD)/obj/src/libc/minilibc.o -lgcc
# (minilibc.o anche dopo libnfs.a: risolve i simboli richiesti dalla libreria)

nfsls: $(BUILD)/nfsls

$(BUILD)/nfsls: tools/nfsls/nfsls.c $(BUILD)/libnfs.a
	$(CC) $(TOOL_CFLAGS) $(LIBNFS_INC) -DAROS -DHAVE_CONFIG_H $(TOOL_LDFLAGS) \
	      -o $@ $(M68K_PREFIX)/m68k-amigaos/libnix/lib/swapstack.o $< $(BUILD)/libnfs.a

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
# Archivio di rilascio: binari (senza simboli), documentazione, licenze e
# sorgenti completi (richiesti dalla LGPL).
DIST_DIR := build/dist/NG-nfs-handler
STRIP    := $(M68K_PREFIX)/bin/m68k-amigaos-strip

dist: all
	rm -rf build/dist build/NG-nfs-handler.lha
	mkdir -p $(DIST_DIR)/L $(DIST_DIR)/C $(DIST_DIR)/Tools $(DIST_DIR)/Docs \
	         $(DIST_DIR)/Storage/DOSDrivers $(DIST_DIR)/Source
	cp $(BUILD)/nfs-handler $(DIST_DIR)/L/
	cp $(BUILD)/NFSDismount $(DIST_DIR)/C/
	cp $(BUILD)/nfsls $(BUILD)/NotifyTest $(DIST_DIR)/Tools/
	$(STRIP) $(DIST_DIR)/L/nfs-handler $(DIST_DIR)/C/NFSDismount \
	         $(DIST_DIR)/Tools/nfsls $(DIST_DIR)/Tools/NotifyTest
	cp dist/Storage/DOSDrivers/NFS0 $(DIST_DIR)/Storage/DOSDrivers/
	cp doc/nfs-handler.txt doc/nfs-handler.it.txt COPYING $(DIST_DIR)/Docs/
	cp libnfs/COPYING $(DIST_DIR)/Docs/COPYING.libnfs
	cp dist/NG-nfs-handler.readme build/dist/
	cp -r Makefile README.md COPYING src tools libnfs test doc dist $(DIST_DIR)/Source/
	cd build/dist && lha -aq ../NG-nfs-handler.lha NG-nfs-handler NG-nfs-handler.readme
	@echo "Creato build/NG-nfs-handler.lha"

clean:
	rm -rf build

.PHONY: all pktlog nfsdismount nfsls handler notifytest dist clean

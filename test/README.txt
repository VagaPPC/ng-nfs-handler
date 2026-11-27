NG-nfs-handler test scripts
===========================

The scripts run on the Amiga and expect an assign to this source tree:

  Assign NGNFS: <volume>:path/to/NG-nfs-handler

(the directory must be reachable from the Amiga, e.g. through a network
filesystem or a UAE/Amiberry directory mount) and the binaries in
NGNFS:build/os3/ (make).

Mountlists use SERVER=nfs-server / 192.168.1.100 and EXPORT=/export/amiga:
edit them for your network. The scripts write their results to NGNFS:build/.

  m2/   read-only tests (names, charset, symlinks, binary copy)
  m3/   write tests (create, rename, protect, delete)
  perf/ timing tests
  real/ functional + timing tests for real hardware (log in RAM:)

The read tests expect a small tree on the export: Readme.txt (mode 0444),
MAIUSCOLO.TXT, dir1/sub/file.bin (307201 bytes), a symlink link_a_dir1 ->
dir1, an empty directory Vuota and files with Latin-1 and non-Latin-1 names.

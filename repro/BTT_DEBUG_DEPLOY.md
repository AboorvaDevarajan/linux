# BTT Debug Build & Deploy Instructions

This packages the work from [minimize_btt_reproducer plan](../.cursor/plans/minimize_btt_reproducer_bfeedb5d.plan.md):

- **Reproducer**: [`repro/htx_btt_repro.c`](htx_btt_repro.c) — adapted to mimic
  the new HTX rule (phase 1 = `bwrc`, phase 3 = `RC`).
- **Kernel patch**: [`btt-trace-debug.patch`](../btt-trace-debug.patch) — adds
  three `trace_printk` instrumentation points to `drivers/nvdimm/btt.c`.
- **Build script**: [`repro/build_and_deploy.sh`](build_and_deploy.sh)
- **Run script**: [`repro/run_btt_debug.sh`](run_btt_debug.sh)

---

## On the build host (this tree, branch `debug_pmem_bug`)

The patch is already applied in this checkout. To rebuild:

```bash
cd /path/to/linux-tmp
# (verify it is applied)
git diff drivers/nvdimm/btt.c | head -40

# build + install (uses CONFIG_LOCALVERSION="-bttdebug")
sudo ./repro/build_and_deploy.sh
```

If you need to re-apply the patch onto a fresh checkout:

```bash
git apply btt-trace-debug.patch
```

The build script will:

1. Copy `config.1` to `.config` if no `.config` exists.
2. Force `CONFIG_LOCALVERSION="-bttdebug"` so the kernel installs as
   `7.1.0-rc3-bttdebug` and is distinguishable in `/boot`.
3. Run `make olddefconfig && make -j$(nproc) vmlinux modules`.
4. `make modules_install && make install`.

After install, set it default and reboot:

```bash
sudo grubby --set-default /boot/vmlinuz-7.1.0-rc3-bttdebug
sudo reboot
```

After reboot, verify:

```bash
uname -r           # must show ...-bttdebug
zgrep CONFIG_PREEMPT /proc/config.gz   # whichever you intend to test
```

---

## On the test host

1. Copy `repro/htx_btt_repro.c` and `repro/run_btt_debug.sh` over.
2. Make sure `ndctl` is installed.
3. Run, as root:

```bash
sudo ./run_btt_debug.sh /dev/pmem0.4s namespace0.4 5
```

Arguments: `<device> <namespace> [phase2_sleep_seconds]`.

The script will:

- Recreate the namespace with `ndctl create-namespace --force -m sector`.
- Bump the per-CPU ftrace ring buffer to 32 MiB.
- Enable tracing.
- Run the reproducer with `nproc` phase-3 threads, 50 000 ops/thread,
  stopping after the first 20 errors.
- Stop tracing immediately on exit and copy the trace into the output
  directory (`/tmp/btt_debug_<timestamp>/btt-trace.log`).
- Save the miscompare reports and the last 200 lines of `dmesg`.

---

## Expected outputs to share back

After a `FAIL` run:

- `/tmp/btt_debug_<timestamp>/btt-trace.log`
- `/tmp/btt_debug_<timestamp>/htxbtt_mis_*.txt` (one per miscompare)
- `/tmp/btt_debug_<timestamp>/htxbtt_mis_*.{wbuf,rbuf}` (write/read buffers)
- `/tmp/btt_debug_<timestamp>/dmesg.log`
- BTT debugfs snapshot:
  ```bash
  for f in /sys/kernel/debug/btt/btt*/arena*/*; do
      echo "=== $f ==="; cat "$f"
  done > /tmp/btt_debug_<timestamp>/debugfs.log 2>&1
  ```

These will be analyzed in the next iteration to confirm/refute the
"BTT map/freelist dual-ownership" hypothesis described in the plan.

---

## Trace event reference

Three `trace_printk` events are emitted by the patched kernel:

| Event                | Where                       | Fields                                                 |
|----------------------|-----------------------------|--------------------------------------------------------|
| `BTT_WRITE`          | end of `btt_write_pg` loop  | `cpu`, `lane`, `premap`, `old_post`, `new_post`        |
| `BTT_READ_MISMATCH`  | after `btt_data_read`       | `cpu`, `lane`, `premap`, `postmap`, `data_lba`, `data_magic` |
| `BTT_FLOG`           | end of `btt_flog_write`     | `lane`, `old_map`, `new_map`, `free_block`             |

`BTT_READ_MISMATCH` fires only when the LBA stamped inside the just-read
4 KiB block does not match the `premap` we asked for. This catches the
case where the BTT map sends us to a physical block belonging to a
different LBA — i.e. the suspected dual-ownership condition.

---

## Automated analysis

Once you have a `btt-trace.log`, run:

```bash
./repro/analyze_trace.sh /tmp/btt_debug_<ts>/btt-trace.log
```

This will produce `<log>.analysis.txt` containing:

1. Event counts (`BTT_WRITE`, `BTT_READ_MISMATCH`, `BTT_FLOG`).
2. The first 20 `BTT_READ_MISMATCH` lines.
3. A `(premap, postmap, data_lba)` frequency table.
4. For each unique mismatched `postmap`, the writes/flog events that
   touched it.
5. **Smoking-gun check**: for each mismatch, does some `BTT_WRITE` exist
   whose `(premap=data_lba, new_post=postmap)` matches? If yes, two
   different premaps are sharing the same physical block — the
   dual-ownership pattern from the previous investigation.
6. `free_block` recycling counts (looking for the same physical block
   being released to the freelist multiple times).

# Embedded Test & App Architecture — Design

**Date:** 2026-09-05
**Status:** approved, ready for implementation
**Trigger:** while completing the GitHub org restructuring
(`docs/superpowers/specs/2026-09-04-github-organization-design.md`),
`neoos-kernel`'s `make test` turned out to structurally depend on
BusyBox (a port, which the kernel repo must not contain) and on ~65
test binaries currently baked into the kernel repo's own `Makefile`
and copied onto the FAT test disk. Loading test executables from the
filesystem under test is also backwards for filesystem/VFS tests. This
spec generalizes the fix: kernel-native tests and ports both become
external, pluggable contributors of {binary, test manifest} pairs that
the kernel embeds directly into its image and serves from a new
in-memory filesystem, never touching the FAT disk at all.

## 1. Problem

1. `neoos-kernel`'s inittab and `REQUIRED_MARKERS` hardcode ~65
   kernel-native test binaries and 3 BusyBox-dependent ones. None of
   this is data-driven — adding, removing, or reordering a test means
   editing the kernel's own `Makefile`.
2. A port (BusyBox) being absent breaks `make test` outright, even
   though the kernel repo must not contain ports (org design spec §3.1).
3. Every test binary is loaded by mcopy-ing it onto the FAT disk image
   and exec'd from there. Tests that exercise the VFS/FAT layer
   (`vfstest`, `stattest`, `fatfs` selftests) then depend on the very
   layer they're testing just to start running — a bug there can look
   like the test itself failed, or silently prevent it from running at
   all (as already happened once per the Makefile's own `REQUIRED_MARKERS`
   comment: "a suite that never RUNS is not a pass").

## 2. Decisions

### 2.1 Three categories of userland binary, three different homes

| Category | Examples | Lives in | Mandatory for boot? |
|---|---|---|---|
| **Boot-critical native apps** | `init.nex`, `login.nex`, `term.nex`, `nsh.nex` | `neoos-kernel` (unchanged) | Yes — the kernel hardcodes `spawn("/sbin/init.nex")` at boot (`kernel/kernel.c:357`); without it there is nothing to boot into. |
| **Kernel regression tests** | `spin.nex`, `forkstorm.nex`, `vfstest.nex`, `bbspike.nex`, all ~65 current `userland/*.c` test programs (native-ABI ones link `libneoos`; `login`/`muslfork`/`muslhelo` link musl) | new repo `neoos-kernel-tests-common` | No — optional, embedded only when explicitly requested. |
| **Ports** | BusyBox, 3d-ascii-viewer | `neoos-busybox`, `neoos-3d-ascii-viewer` (per the org design spec, unchanged) | No — optional, same mechanism as tests. |

This resolves the earlier ambiguity cleanly: nothing about *testing*
FAT requires the *test binary itself* to be loaded from FAT — a test
exercises FAT-mounted paths as its subject regardless of where its own
executable came from. So all ~65 regression tests, including the
FAT/VFS-specific ones, move uniformly.

### 2.2 `neoos-libneoos` becomes its own repo

`lib/` (the NeoOS-native libc alternative to musl: `crt0.o`, syscall
wrappers, headers) moves out of `neoos-kernel` into `neoos-libneoos`,
mirroring `neoos-musl`'s split from the kernel. `neoos-kernel-tests-common`
and any future native (non-musl, non-port) app both build against it.

**Build contract** (mirrors `neoos-musl`'s `KERNEL_SHIM_DIR` pattern):

```sh
git clone https://github.com/NeoOSOrganization/neoos-libneoos
cd neoos-libneoos
make
# Produces: build-output/include/, build-output/lib/libneoos.a,
#           build-output/lib/crt0.o
```

`neoos-libneoos` needs nothing from the kernel repo to build (unlike
musl, it has no shim to integrate — it calls NeoOS syscalls directly by
NeoOS's own numbers, which is the whole point of it existing beside
musl). It only needs the syscall number/ABI headers, which move with
it (they were already `lib/include/`, not `kernel/`-internal headers).

### 2.3 Manifest contract (identical for tests-common and every port)

A **build output**, not a source file — produced fresh each build, one
per binary, alongside it:

```
build/<name>.nex
build/<name>.test.json
```

Schema:

```json
{
  "name": "busybox",
  "category": "bin",
  "boot_entries": [
    {"after": "thrdmany", "line": "wait /usr/tests/bbspike.nex"},
    {"after": "bbspike",  "line": "wait /usr/tests/nshtest.nex"},
    {"after": "logintest","line": "wait /usr/tests/bbsh.nex"}
  ],
  "required_markers": [
    "[bbspike] ALL PASSED",
    "[nshtest] ALL PASSED",
    "[bbsh] ALL PASSED"
  ]
}
```

- `category`: one of `bin`, `sbin`, `tests` — determines which embedfs
  mount the binary is placed under (`/bin`, `/sbin`, `/usr/tests`
  respectively; see §2.4). `neoos-kernel-tests-common` emits one
  manifest per test binary with `category: "tests"` and typically a
  single boot entry for itself (`spawn`/`wait /usr/tests/<name>.nex`).
- `boot_entries`: each has `line` (the literal inittab line to insert)
  and `after` (the name — without `.nex` — of an existing inittab
  entry to insert immediately after; entries within one manifest are
  applied in array order, so a later entry's `after` may reference an
  earlier entry in the *same* manifest). This is how BusyBox's
  historically hand-tuned interleaving (bbspike right after thrdmany;
  nshtest right after bbspike; bbsh right after logintest — preserving
  exactly what `Makefile`'s current inittab printf already encodes)
  survives the move to data-driven generation.
- `required_markers`: exact strings checked with `grep -F` against the
  serial log, same semantics as the kernel's existing `REQUIRED_MARKERS`.
  A manifest with no entry here is valid (e.g. a port with only a
  standalone `smoke-test.sh`, no kernel-boot-time assertion).
- Failure detection needs no new mechanism: `make test`'s existing
  blanket `grep -q FAILED serial.log` already catches any test's
  `[name] FAILED: ...` output regardless of where the test came from.

### 2.4 `embedfs` — a new read-only, in-kernel-image filesystem

Replaces FAT-disk delivery of every executable (not config files, not
test *data* fixtures — `/etc/passwd`, `/etc/inittab`,
`/usr/share/test/*`, `/usr/share/models/*` stay on FAT exactly as
today, since those are legitimately storage content, not code).

**Kernel-side (new, in `neoos-kernel`):**
- `kernel/fs/embedfs.h` / `embedfs.c`: a `struct vfs_ops` implementation
  mirroring `devfs.c`'s shape. Backing store is a build-generated,
  linked-in table:
  ```c
  struct embedfs_entry { const char *category; const char *name; const void *data; uint32_t size; };
  extern const struct embedfs_entry g_embedfs_table[];
  extern const int g_embedfs_table_count;
  ```
  - `mount(m, source)`: stores `source` (one of `"bin"`, `"sbin"`,
    `"tests"`) in `m->fs_private` as the category filter for this mount.
  - `lookup`: linear scan of `g_embedfs_table` for an entry whose
    `category` matches this mount's and whose `name` matches; inode_id
    = table index (globally unique, so `read_inode` needs no extra
    context beyond the index).
  - `read`: `memcpy` directly out of the entry's linked-in `data`
    pointer — no page copying, no size cap (unlike ramfs's existing
    16KiB-per-file `RAMFS_MAX_PAGES` limit, which is why this is a new
    fs rather than reusing ramfs: BusyBox alone is ~450KB).
  - `readdir`: iterate matching-category entries in table order.
  - `write`/`create`/`mkdir`/`unlink`/`truncate`: return `-EROFS`
    (never `NULL`, per the existing "no op pointer is ever NULL"
    convention in `vfs_ops`).
- `kernel/kernel.c`: replace nothing about `/`, `/dev`, `/tmp`, `/proc`,
  `/mnt` (unchanged). Add three new mounts right after the existing
  block:
  ```c
  vfs_mount_fs("bin",   "/bin",       "embedfs");
  vfs_mount_fs("sbin",  "/sbin",      "embedfs");
  vfs_mount_fs("tests", "/usr/tests", "embedfs");
  ```
  With an empty `g_embedfs_table` (the default, no `EMBED_DIR` at build
  time), these mounts are present but empty directories — `/sbin/init.nex`
  then doesn't exist and boot PANICs with the existing "did not start as
  PID 1" message. So **`neoos-kernel`'s own build always embeds its own
  boot-critical apps** (init/login/term/nsh — §2.1's first category)
  unconditionally; only the *optional* categories (tests, ports) depend
  on an external `EMBED_DIR`.

**Build-side (generic, works for boot-critical apps, tests, and ports alike):**
- `tools/gen-embedfs.py <output.c> <dir>...`: takes one or more
  directories, each containing `*.nex` + optional `*.test.json` pairs.
  For every `.nex` found:
  1. `ld -r -b binary -o build/embedfs-obj/<safe_name>.o <path>` from
     inside a staging copy so the generated symbol names
     (`_binary_<safe_name>_start/_end`) are predictable regardless of
     the source path.
  2. Emits one `g_embedfs_table[]` row using the manifest's `category`
     (default `"tests"` if no manifest is present, matching today's
     `usr/tests` default for anything with no explicit categorization).
  3. Collects every manifest's `boot_entries` and `required_markers`
     into two side files consumed by the `Makefile`:
     `build/embedfs-inittab-patch.txt` (ordered `after`/`line` pairs)
     and `build/embedfs-markers.txt` (one required marker per line).
- `Makefile` changes:
  - The existing giant inittab `printf` block loses ONLY the three
    BusyBox-dependent lines (`wait /usr/tests/bbspike.nex`,
    `wait /usr/tests/nshtest.nex`, `wait /usr/tests/bbsh.nex`) and every
    line for a test now sourced from `neoos-kernel-tests-common` (all
    ~65 `spawn`/`wait /usr/tests/*.nex` lines) — these become
    manifest-driven insertions instead of hardcoded text. Boot-critical
    app lines (`respawn /bin/term.nex /sbin/login.nex login`, etc.) and
    any port-independent kernel selftests expressed as boot markers
    that aren't separate binaries (there are none — all `REQUIRED_MARKERS`
    entries other than the userland ones correspond to in-kernel
    selftests like `[wxorx] kernel selftest passed`, which print from
    kernel code directly at boot and need no userland binary at all)
    stay exactly as they are.
  - Replace the ~65 individual
    `./tools/nexify.sh $(USERLAND_BUILD)/X.ELF $(DISK_SRC)/nex/x.nex`
    + `mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/x.nex ::usr/tests/x.nex`
    pairs: **keep every `nexify.sh` line** (still need the flat `.nex`
    files as embed input) and **delete every corresponding `mcopy`
    line** for an executable. `mcopy` calls for `/etc/*` and
    `/usr/share/*` DATA files are unaffected.
  - New variables: `EMBED_DIRS ?=` (space-separated list of external
    directories — tests-common's `build/`, each port's `build/`), used
    as: `python3 tools/gen-embedfs.py build/embedfs_table.c $(DISK_SRC)/nex $(EMBED_DIRS)`
    (kernel's own `$(DISK_SRC)/nex` — boot-critical apps only, since
    that's all that's left there once the ~65 test `.nex` files stop
    being produced by the kernel's own build — is always included,
    unconditionally, for the reason in the mount section above).
  - `REQUIRED_MARKERS` becomes `CORE_REQUIRED_MARKERS` (today's list
    minus the ~65 userland-test-binary markers and the 3 BusyBox ones)
    plus `$(shell cat build/embedfs-markers.txt 2>/dev/null)`.
  - `test:` target's recipe gains one step before writing the inittab:
    apply `build/embedfs-inittab-patch.txt`'s ordered insertions (a
    small `tools/apply-inittab-patch.py`) to the printf'd base inittab
    before `mcopy`-ing it in — same anchor-insertion algorithm as
    §2.3's `after`/`line` semantics.

**Net effect on `neoos-kernel` standalone:** `make test` with
`EMBED_DIRS` unset boots with only boot-critical apps present, zero
kernel-native-test and zero port markers required, and passes cleanly
— true standalone testing, no port or test-suite dependency. This
satisfies org design spec §8's success criterion 2 for real.

### 2.5 `neoos-os-builder`: pulling tests in is optional

Per the org design spec (§3.3), `neoos-os-builder`'s config format
(spec'd, not yet implemented — org-restructuring-completion plan Task 7)
gains one more optional section:

```yaml
kernel:
  version: "main"
tests:
  include: false      # default: false. true clones neoos-kernel-tests-common
                       # and passes its build/ dir as one of EMBED_DIRS.
ports:
  - busybox
  - 3d-ascii-viewer
iso:
  name: "neoos-custom-build"
  disk_size: 2G
```

Default is `false` — a production/minimal image never carries the
regression suite. `neoos-os-builder`'s own CI, and any developer who
wants the full gauntlet reproduced from an assembled image, sets
`tests.include: true`.

## 3. What does NOT change

- FAT (`/`, `/mnt`), devfs (`/dev`), ramfs (`/tmp`), procfs (`/proc`)
  mounts and their contents — config files, `/etc/passwd`,
  `/usr/share/test/*` fixtures, `/usr/share/models/*` all stay
  FAT-delivered exactly as today, since they're data under test, not
  code being loaded.
- `REQUIRED_MARKERS` entries that are kernel-internal selftests with no
  associated userland binary (e.g. `[wxorx] kernel selftest passed`,
  `[pci] ALL PASSED`) — those print directly from kernel boot code and
  are untouched by this spec.
- The org design spec's contracts for `neoos-musl`, `neoos-busybox`,
  `neoos-3d-ascii-viewer`, `neoos-docs` — unaffected. `neoos-os-builder`
  gains the `tests.include` flag but its kernel/musl/port contracts are
  otherwise as already specced.
- `git filter-repo`/dual-push mechanics from the org-restructuring plan
  — unaffected; `neoos-kernel-tests-common` and `neoos-libneoos` are
  brand-new repos with fresh history, not extracted-with-history from
  this monorepo (their content is moving, not their commit history —
  consistent with how the port repos are being handled).

## 4. New repo list (supersedes the org design spec's list of 6)

1. `neoos-kernel` — kernel + `embedfs` + boot-critical apps
   (init/login/term/nsh) only. No tests, no libneoos, no musl, no ports.
2. `neoos-libneoos` — native NeoOS libc alternative to musl (new).
3. `neoos-musl` — unchanged from the org design spec.
4. `neoos-kernel-tests-common` — the ~65 existing regression test
   programs, source only, built against `neoos-libneoos` and
   `neoos-musl` (new).
5. `neoos-busybox`, `neoos-3d-ascii-viewer` — ports, unchanged contract,
   now also emit a `.test.json` manifest per §2.3.
6. `neoos-os-builder` — gains the optional `tests.include` flag.
7. `neoos-docs` — unchanged, port/test-manifest format gets a page.

## 5. Migration ordering

This spec's work slots into the existing
`docs/superpowers/plans/2026-09-05-github-org-restructuring-completion.md`
as a new Task 2.5 (between the existing Task 2 "fix neoos-kernel's
Makefile" and Task 3 "regression-check the monorepo"), with its own
sub-tasks. `neoos-libneoos` extraction and `neoos-kernel-tests-common`
creation both depend on `embedfs` existing first (tests need somewhere
to be embedded into), so implementation order is: embedfs in the
monorepo → verify gauntlet green with zero behavior change → extract
`neoos-libneoos` → extract `neoos-kernel-tests-common` → wire
`neoos-kernel`'s standalone build against both → re-verify → resume the
org-restructuring plan's remaining tasks.

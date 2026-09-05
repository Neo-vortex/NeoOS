# Embedded Test & App Architecture Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace FAT-disk delivery of every NeoOS executable with a new
in-kernel-image filesystem (`embedfs`), make kernel-native regression
tests and ports both pluggable via an identical binary+manifest
contract, and split `lib/` (libneoos) and the ~65 test programs out of
`neoos-kernel` into their own repos — so `neoos-kernel`'s `make test`
finally passes standalone with zero test-suite or port dependency.

**Architecture:** See
`docs/superpowers/specs/2026-09-05-embedded-test-and-app-architecture.md`
for the full design. Summary: boot-critical apps (init/login/term/nsh)
stay in `neoos-kernel`, always embedded. Regression tests move to
`neoos-kernel-tests-common`. `lib/` moves to `neoos-libneoos`. Both, plus
ports, become "embed dirs" — directories of `<name>.nex` +
`<name>.test.json` pairs that a generic build-time script folds into
the kernel image and a generic boot-time filesystem serves.

**Tech Stack:** GNU Make, Python 3 (`tools/gen-embedfs.py` and friends,
matching the existing `tools/bdf2c.py`/`tools/lock_check.py`
convention), `ld -r -b binary`, C (kernel VFS driver).

**Spec:** `docs/superpowers/specs/2026-09-05-embedded-test-and-app-architecture.md`

## Global Constraints

- **No gauntlet regressions at any task boundary**: every task ends
  with `./tools/gauntlet.sh` (or a single `make test` while iterating,
  gauntlet before moving to the next task) reporting the SAME
  `PASSED: N/N` as the pre-task baseline, on this monorepo, before
  moving to the next task.
- **Config files and test-data fixtures stay on FAT**: `/etc/passwd`,
  `/etc/inittab`, `/etc/nshrc`, `/etc/nsh.logo`, `/usr/share/test/*`,
  `/usr/share/models/*`. Only executables move to `embedfs`.
- **`vfs_ops` convention**: no op pointer is ever `NULL` — unsupported
  operations on the new read-only fs return the correct errno
  (`-EROFS` for write/create/mkdir/unlink/truncate), never crash.
- **musl shim stays translation-only, ABI stays Linux-shaped** — this
  work touches no syscall-facing behavior at all, only how binaries are
  loaded, so this should be a non-issue, but any accidental syscall
  change is out of scope and a bug.
- **Work happens on `main`** in this monorepo; this plan's later tasks
  push filtered content to new org repos exactly like the
  org-restructuring-completion plan's Tasks 1/4/5 do (clone fresh
  history, no `git filter-repo` needed since these two repos have no
  prior history to preserve).

## Known state going in

- `tools/gauntlet.sh` baseline: capture `PASSED: N/N` before Task 1 and
  compare after every task.
- `Makefile`'s inittab-generating recipe (in the `test:`/`disk-image:`
  chain) is one large `printf '%s\n' ... > $(BUILD_DIR)/disk-src/inittab`
  call containing every `spawn`/`wait` line in hand-tuned order, with
  comments explaining several non-obvious orderings (BBSPIKE, PTYCHURN,
  POLLSTORM). This exact ordering must survive the migration — see
  Task 3's extraction script.
- `REQUIRED_MARKERS` (Makefile, ~75 lines) is the full list the `test:`
  target's `for m in $(REQUIRED_MARKERS)` loop checks for.
- `kernel/fs/ramfs.c`'s `RAMFS_MAX_PAGES` is 4 (16KiB/file) — confirmed
  too small for BusyBox (~450KB), which is why `embedfs` is a new
  filesystem rather than a ramfs extension.
- `kernel/fs/devfs.c` is the closest existing pattern for a synthetic,
  no-real-storage `vfs_ops` implementation — `embedfs.c` mirrors its
  shape (see Task 1).
- `kernel/kernel.c:220-232` is the mount sequence; `kernel/kernel.c:357`
  is the hardcoded `spawn("/sbin/init.nex")` that makes init mandatory.

---

### Task 1: `embedfs` kernel driver (dead code, not yet mounted)

**Files:**
- Create: `kernel/fs/embedfs.h`, `kernel/fs/embedfs.c`
- Modify: wherever `kernel/fs/devfs.c`/`ramfs.c` are added to the
  compile sources (check: does the Makefile glob `kernel/fs/*.c`
  automatically, or list files explicitly? If explicit, add
  `embedfs.c` next to the others.)

**Interfaces:**
- Produces: `extern const struct vfs_ops embedfs_ops;` (or whatever
  naming `devfs.c`/`ramfs.c` use for their equivalent — match it
  exactly for consistency) and the two externs `embedfs.c` expects the
  build to provide: `extern const struct embedfs_entry g_embedfs_table[];`
  and `extern const int g_embedfs_table_count;` (defined later, in
  Task 2's generated file — until then, Task 1 stub-defines an EMPTY
  table locally, e.g. in `embedfs.c` itself guarded by
  `#ifndef EMBEDFS_TABLE_PROVIDED`, so the kernel links and boots with
  zero embedfs behavior change while this task is verified in
  isolation).

- [ ] **Step 1: Write the entry struct and table externs**

```c
// kernel/fs/embedfs.h
#pragma once
#include <stdint.h>

struct embedfs_entry {
    const char *category;  // "bin", "sbin", or "tests"
    const char *name;      // e.g. "busybox.nex" -- matched exactly, no path
    const void *data;
    uint32_t    size;
};

void embedfs_init(void);  // called once at boot, before the three mounts
```

- [ ] **Step 2: Implement the `vfs_ops` table, mirroring `devfs.c`'s per-entry read pattern**

```c
// kernel/fs/embedfs.c
#include "fs/embedfs.h"
#include "fs/vfs.h"
#include "errno.h"
#include <stddef.h>

extern const struct embedfs_entry g_embedfs_table[];
extern const int g_embedfs_table_count;

struct embedfs_mount_ctx { const char *category; };
// One static context per possible mount (bin/sbin/tests) -- vfs_mount
// only gives us fs_private to stash a pointer in, and the category
// string passed as `source` outlives the mount (it's a Makefile/kernel.c
// string literal), so storing the pointer directly is safe.

static int embedfs_mount_op(struct vfs_mount *m, const char *source) {
    m->fs_private = (void *)source;  // "bin" / "sbin" / "tests"
    return 0;
}

static void embedfs_umount_op(struct vfs_mount *m) { (void)m; }

// inode_id is 1 + the table index (0 is reserved for "no such inode" by
// convention elsewhere in vfs.c -- verify this against vnode_get's
// actual convention before relying on it; adjust to match if vnode 0 is
// valid here).
static int embedfs_read_inode(struct vfs_mount *m, uint64_t inode_id, struct vnode *out) {
    if (inode_id == 0 || inode_id > (uint64_t)g_embedfs_table_count) { return -ENOENT; }
    const struct embedfs_entry *e = &g_embedfs_table[inode_id - 1];
    out->type = VNODE_FILE;
    out->size = e->size;
    out->mtime = out->atime = out->ctime = 0;
    out->fs_private = (void *)e;
    return 0;
}

static int embedfs_sync_inode(struct vnode *vn) { (void)vn; return 0; }  // nothing to flush

static int embedfs_lookup(struct vnode *dir, const char *name, uint64_t *out_inode_id) {
    const char *category = (const char *)dir->mount->fs_private;
    for (int i = 0; i < g_embedfs_table_count; i++) {
        if (name_eq_helper(g_embedfs_table[i].category, category) &&
            name_eq_helper(g_embedfs_table[i].name, name)) {
            *out_inode_id = (uint64_t)(i + 1);
            return 0;
        }
    }
    return -ENOENT;
}

static int64_t embedfs_read(struct vnode *vn, uint32_t pos, void *buf, uint32_t len) {
    const struct embedfs_entry *e = (const struct embedfs_entry *)vn->fs_private;
    if (pos >= e->size) { return 0; }
    if (pos + len > e->size) { len = e->size - pos; }
    const uint8_t *src = (const uint8_t *)e->data + pos;
    uint8_t *dst = (uint8_t *)buf;
    for (uint32_t i = 0; i < len; i++) { dst[i] = src[i]; }
    return (int64_t)len;
}

static int64_t embedfs_write(struct vnode *vn, uint32_t pos, const void *buf, uint32_t len) {
    (void)vn; (void)pos; (void)buf; (void)len; return -EROFS;
}
static int embedfs_create(struct vnode *dir, const char *name, uint64_t *out_id) {
    (void)dir; (void)name; (void)out_id; return -EROFS;
}
static int embedfs_mkdir(struct vnode *dir, const char *name) {
    (void)dir; (void)name; return -EROFS;
}
static int embedfs_unlink(struct vnode *dir, const char *name) {
    (void)dir; (void)name; return -EROFS;
}
static int embedfs_truncate(struct vnode *vn) { (void)vn; return -EROFS; }

static int embedfs_readdir(struct vnode *dir, uint32_t index, struct vfs_dirent *out) {
    const char *category = (const char *)dir->mount->fs_private;
    uint32_t seen = 0;
    for (int i = 0; i < g_embedfs_table_count; i++) {
        if (!name_eq_helper(g_embedfs_table[i].category, category)) { continue; }
        if (seen == index) {
            name_copy_helper(out->name, g_embedfs_table[i].name);
            out->type = VNODE_FILE;
            return 1;
        }
        seen++;
    }
    return 0;
}

const struct vfs_ops embedfs_ops = {
    .mount = embedfs_mount_op, .umount = embedfs_umount_op,
    .read_inode = embedfs_read_inode, .sync_inode = embedfs_sync_inode,
    .lookup = embedfs_lookup, .read = embedfs_read, .write = embedfs_write,
    .create = embedfs_create, .mkdir = embedfs_mkdir, .unlink = embedfs_unlink,
    .truncate = embedfs_truncate, .readdir = embedfs_readdir,
};
```

(`name_eq_helper`/`name_copy_helper`: check `ramfs.c`'s existing
private `name_eq`/`name_copy` helpers at the top of that file — either
reuse them if they're non-static/exposed, or copy the same
2-3-line implementation into `embedfs.c`; do not invent a different
string comparison convention.)

- [ ] **Step 3: Register `"embedfs"` as a mountable fstype**

Find where `vfs_mount_fs`'s `fstype` string dispatches to `fat_ops` /
`devfs_ops` / `ramfs_ops` / `procfs_ops` (likely a chain of `strcmp` in
`vfs.c`) and add `"embedfs"` -> `embedfs_ops` alongside them.

- [ ] **Step 4: Stub table so the kernel links with zero behavior change (removed in Task 2)**

At the bottom of `embedfs.c`, temporarily:

```c
const struct embedfs_entry g_embedfs_table[] = {{0}};
const int g_embedfs_table_count = 0;
```

- [ ] **Step 5: Build and confirm zero behavior change**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
./tools/gauntlet.sh
```

Expected: identical `PASSED: N/N` to the pre-task baseline — this task
adds dead code only, no mounts yet, no inittab/Makefile changes.

- [ ] **Step 6: Commit**

```bash
git add kernel/fs/embedfs.h kernel/fs/embedfs.c <the vfs.c dispatch site>
git commit -m "fs: add embedfs, a read-only in-image filesystem (not yet mounted)"
```

---

### Task 2: Embed the boot-critical apps, cut `/bin` and `/sbin` over

**Files:**
- Create: `tools/gen-embedfs.py`
- Modify: `Makefile` (mount calls moved to `kernel/kernel.c`, embed-table
  generation and linking added to the `iso`/`test` chain)
- Modify: `kernel/kernel.c` (add the two mounts + `embedfs_init()` call)
- Remove: the `mcopy` lines for `::bin/av.nex`, `::bin/busybox.nex`,
  `::bin/nsh.nex`, `::bin/term.nex`, `::sbin/init.nex`, `::sbin/login.nex`
  — **NOT YET** for av/busybox (those are ports, migrated later per the
  org-restructuring plan; leave their `mcopy` calls in place for now,
  conditional exactly as today) — only `nsh.nex`, `term.nex`,
  `init.nex`, `login.nex` this task.

**Interfaces:**
- Consumes: `kernel/fs/embedfs.h`'s `struct embedfs_entry` (Task 1).
- Produces: `build/embedfs_table.c` (generated, not committed — add to
  `.gitignore` if `build/` isn't already wholly ignored).

- [ ] **Step 1: Write the generator**

```python
#!/usr/bin/env python3
# tools/gen-embedfs.py <output.c> <dir> [<dir> ...]
#
# For every *.nex in each directory, embeds it via `ld -r -b binary`
# and emits one row in a generated embedfs table. A *.test.json next to
# a *.nex supplies its category/boot_entries/required_markers; absent
# a manifest, category defaults to "tests" (matching today's default
# home for anything not explicitly bin/sbin).
import json, os, subprocess, sys

def main():
    out_c, dirs = sys.argv[1], sys.argv[2:]
    entries = []
    boot_entries = []
    markers = []
    obj_dir = os.path.join(os.path.dirname(out_c), "embedfs-obj")
    os.makedirs(obj_dir, exist_ok=True)

    for d in dirs:
        if not os.path.isdir(d):
            continue
        for fname in sorted(os.listdir(d)):
            if not fname.endswith(".nex"):
                continue
            name = fname
            manifest_path = os.path.join(d, fname[:-4] + ".test.json")
            manifest = {}
            if os.path.exists(manifest_path):
                with open(manifest_path) as f:
                    manifest = json.load(f)
            category = manifest.get("category", "tests")
            safe = "".join(c if c.isalnum() else "_" for c in name)
            obj_path = os.path.join(obj_dir, safe + ".o")
            subprocess.run(
                ["ld", "-r", "-b", "binary", "-o", obj_path, os.path.abspath(os.path.join(d, fname))],
                check=True, cwd=d,
            )
            # ld -r -b binary mangles the FULL path given into the symbol
            # name. Running with cwd=d and an absolute path keeps this
            # predictable to compute here: ld replaces every non-alnum
            # byte of the path (including leading '/') with '_'.
            abs_path = os.path.abspath(os.path.join(d, fname))
            sym = "".join(c if c.isalnum() else "_" for c in abs_path)
            entries.append((category, name, sym, obj_path))
            for be in manifest.get("boot_entries", []):
                boot_entries.append(be)
            markers.extend(manifest.get("required_markers", []))

    with open(out_c, "w") as f:
        f.write("#include \"fs/embedfs.h\"\n#include <stddef.h>\n\n")
        for _, _, sym, _ in entries:
            f.write(f"extern char _binary_{sym}_start[], _binary_{sym}_end[];\n")
        f.write("\nconst struct embedfs_entry g_embedfs_table[] = {\n")
        for category, name, sym, _ in entries:
            f.write(
                f'    {{"{category}", "{name}", _binary_{sym}_start, '
                f'(unsigned int)(_binary_{sym}_end - _binary_{sym}_start)}},\n'
            )
        f.write("};\nconst int g_embedfs_table_count = "
                 f"{len(entries)};\n")

    obj_list = os.path.join(os.path.dirname(out_c), "embedfs-objs.txt")
    with open(obj_list, "w") as f:
        f.write(" ".join(p for _, _, _, p in entries))

    patch_path = os.path.join(os.path.dirname(out_c), "embedfs-inittab-patch.json")
    with open(patch_path, "w") as f:
        json.dump(boot_entries, f)

    markers_path = os.path.join(os.path.dirname(out_c), "embedfs-markers.txt")
    with open(markers_path, "w") as f:
        f.write("\n".join(markers))

if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Wire it into the Makefile, replacing the 6 boot-critical `mcopy` lines**

Find and delete these 6 lines (search for `::bin/nsh.nex`,
`::bin/term.nex`, `::sbin/init.nex`, `::sbin/login.nex` — leave the
`av`/`busybox` ones alone per this task's scope) and their paired
`nexify.sh` lines stay. Add, in the same recipe right before the
`mcopy ... ::etc/inittab` step:

```makefile
EMBED_DIRS ?=
build/embedfs_table.c: $(DISK_SRC)/nex/init.nex $(DISK_SRC)/nex/login.nex \
                        $(DISK_SRC)/nex/term.nex $(DISK_SRC)/nex/nsh.nex
	python3 tools/gen-embedfs.py $@ $(DISK_SRC)/nex-embed-boot $(EMBED_DIRS)
```

(`$(DISK_SRC)/nex-embed-boot`: a NEW small directory containing only
`init.nex`, `login.nex`, `term.nex`, `nsh.nex` — copy these four out of
the shared `$(DISK_SRC)/nex/` into it right after their `nexify.sh`
lines, e.g. `mkdir -p $(DISK_SRC)/nex-embed-boot && cp $(DISK_SRC)/nex/init.nex $(DISK_SRC)/nex-embed-boot/`.
Keeping this a SEPARATE directory from the general `$(DISK_SRC)/nex/`
matters once Task 3 starts embedding test binaries too — it keeps
"boot-critical, always embedded" cleanly separate from "everything
else," matching the spec's category table.)

Then link `build/embedfs_table.c`'s object and every object listed in
`build/embedfs-objs.txt` into `kernel.elf` — find the final link recipe
(the `$(CC) -T linker.ld -o build/kernel.elf ...` line already visible
from Task 2's earlier build output) and add
`$$(cat build/embedfs-objs.txt) build/embedfs_table.o` to its object
list, with `build/kernel.elf`'s prerequisites gaining
`build/embedfs_table.c build/embedfs_table.o`.

- [ ] **Step 3: Wire the two mounts and `embedfs_init()` into `kernel.c`**

```c
vfs_mount_fs(0,      "/proc",      "procfs");
vfs_mount_fs("hd1",  "/mnt",       "fat");
vfs_mount_fs("bin",  "/bin",       "embedfs");
vfs_mount_fs("sbin", "/sbin",      "embedfs");
```

(`/usr/tests` mount deferred to Task 3, since nothing embeds there
yet.)

- [ ] **Step 4: Build and verify boot still reaches a login prompt / gauntlet still green**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
./tools/gauntlet.sh
```

Expected: identical `PASSED: N/N`. If `init.nex` fails to be found,
check the `embedfs_lookup` category match and the generator's category
default — `init`/`login` need `category: "sbin"` and
`term`/`nsh` need `category: "bin"`, which requires manifests for
these four (Step 4a below), since the generator's default is `"tests"`.

- [ ] **Step 4a: Add the 4 boot-critical manifests**

```json
// build/../ (wherever nex-embed-boot's manifests live, e.g. userland/boot-apps.test.json per app)
```

Simplest: one manifest per app, named to match
(`init.test.json`, `login.test.json`, `term.test.json`, `nsh.test.json`),
each just `{"category": "sbin"}` or `{"category": "bin"}` with no
`boot_entries`/`required_markers` (those stay hardcoded in the
Makefile's inittab printf for these four — they're not optional, so
they don't need the dynamic insertion mechanism). Place them in a new
`userland/boot-apps/` directory alongside nothing else (just the four
`.test.json` files — the `.nex` files themselves are generated into
`$(DISK_SRC)/nex-embed-boot/`, not this directory; the generator reads
manifests by looking for `<name>.test.json` next to each `<name>.nex`
it's given, so copy these four json files into
`$(DISK_SRC)/nex-embed-boot/` alongside their `.nex` counterparts in
the same Makefile step that copies the binaries there).

- [ ] **Step 5: Re-run verification, commit**

```bash
./tools/gauntlet.sh
git add tools/gen-embedfs.py Makefile kernel/kernel.c userland/boot-apps/
git commit -m "boot: serve init/login/term/nsh from embedfs instead of the FAT disk"
```

---

### Task 3: Migrate the ~65 kernel-native regression tests to embedfs

**Files:**
- Create: `tools/extract-inittab-manifests.py` (one-time migration
  helper, not a permanent build tool)
- Modify: `Makefile` (delete the ~59 `mcopy ... ::usr/tests/*.nex`
  lines; delete the corresponding ~65 `REQUIRED_MARKERS` entries that
  have a userland-test-binary source, replacing with
  `CORE_REQUIRED_MARKERS` + dynamic; replace the static inittab
  `printf` block's `/usr/tests/*` lines with the anchor-patch mechanism)
- Create: `tools/apply-inittab-patch.py`
- Create: one manifest file per test (or one bundle file — see Step 1)

**Interfaces:**
- Consumes: Task 2's `gen-embedfs.py` (extended, not replaced) and
  `embedfs.c`/mounts (Task 1-2).
- Produces: `/usr/tests` embedfs mount fully populated; a
  `build/embedfs-inittab-patch.json` bundling every test's boot entry.

- [ ] **Step 1: Write the one-time extraction script**

This reads the Makefile's CURRENT inittab `printf` block (before Step 2
deletes anything from it) and emits one manifest bundle file preserving
exact order and spawn/wait mode:

```python
#!/usr/bin/env python3
# tools/extract-inittab-manifests.py Makefile > userland/tests.manifest.json
#
# One-time migration: reads the hand-written inittab printf block and
# turns each `spawn|wait /usr/tests/X.nex` line into a chained manifest
# entry (each anchored to the PREVIOUS test's name), so the exact
# existing order survives being made data-driven. Comments in the
# printf block (lines starting with '#') are preserved as a "note"
# field for human reference, not used by the build.
import json, re, sys

def main():
    with open(sys.argv[1]) as f:
        text = f.read()
    # Isolate the printf block: from `printf '%s\n' \` (the one in the
    # `test:` recipe, not `shell-serial`/`faultflood`'s) to the closing
    # `> $(BUILD_DIR)/disk-src/inittab` line -- confirm this matches
    # exactly ONE block before trusting the output; multiple matches
    # means the regex needs tightening against the real file.
    block_match = re.search(
        r"printf '%s\\n' \\\n(.*?)\n\s*> \$\(BUILD_DIR\)/disk-src/inittab",
        text, re.S,
    )
    assert block_match, "inittab printf block not found -- check the Makefile hasn't already changed"
    lines = re.findall(r"'([^']*)'", block_match.group(1))

    entries = []
    prev_test_name = "term"  # the line immediately before the test block starts
    for line in lines:
        m = re.match(r"(spawn|wait) /usr/tests/(\w+)\.nex$", line)
        if not m:
            continue  # comments and non-/usr/tests lines (bbspike/nshtest/bbsh handled separately -- port-dependent)
        mode, name = m.groups()
        entries.append({
            "_test": name,
            "category": "tests",
            "boot_entries": [{"after": prev_test_name, "line": f"{mode} /usr/tests/{name}.nex"}],
            "required_markers": [],  # filled in Step 2 from REQUIRED_MARKERS
        })
        prev_test_name = name

    json.dump(entries, sys.stdout, indent=2)

if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Run it, then fill in `required_markers` from the current `REQUIRED_MARKERS` list**

```bash
python3 tools/extract-inittab-manifests.py Makefile > /tmp/tests-manifest-draft.json
```

Manually cross-reference each entry's `_test` name against
`REQUIRED_MARKERS` (e.g. `spin` -> no marker in that list at all --
some tests have none; `bbspike`/`nshtest`/`bbsh` were EXCLUDED by the
regex above on purpose, since those are BusyBox-dependent and get their
own manifest per the org-restructuring plan's Task 4, not this one) and
add the matching `"[name] ... PASSED"` string(s) to each entry's
`required_markers`. Save the result as `userland/tests.manifest.json`
(drop the internal `_test` key, or keep it — it's harmless and
documents provenance). **This is a review step, not a blind script
run** — a mismatch here silently weakens the gauntlet, which is exactly
the failure mode the Makefile's own `REQUIRED_MARKERS` comment warns
about ("a suite that never runs is not a pass").

- [ ] **Step 3: Split the one bundle file the generator already supports, or extend `gen-embedfs.py` to accept a bundle**

Given `userland/tests.manifest.json` is a JSON array (not the
single-object shape `gen-embedfs.py` currently reads per `<name>.test.json`),
extend `gen-embedfs.py`'s manifest loading:

```python
def load_manifests_for(d, fname):
    """Returns a list of manifest dicts applicable to this .nex file:
    either its own <name>.test.json (single object), or any object
    inside a *.manifest.json bundle in the same dir whose "_test" or
    inferred name matches."""
    base = fname[:-4]
    single = os.path.join(d, base + ".test.json")
    if os.path.exists(single):
        with open(single) as f:
            return [json.load(f)]
    for bundle_name in os.listdir(d):
        if not bundle_name.endswith(".manifest.json"):
            continue
        with open(os.path.join(d, bundle_name)) as f:
            bundle = json.load(f)
        for entry in bundle:
            if entry.get("_test") == base:
                return [entry]
    return []
```

Replace the single-manifest lookup call in `main()` with this function,
and copy `userland/tests.manifest.json` into
`$(DISK_SRC)/nex/` (or wherever the test `.nex` files land) as part of
the Makefile recipe, so the generator finds it alongside them.

- [ ] **Step 4: Delete the ~59 `mcopy ... ::usr/tests/*.nex` lines and the 3 BusyBox-dependent inittab lines from the printf block**

Keep every `nexify.sh` line (still needed to produce the flat `.nex`
files as embed input). Delete the `mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/X.nex ::usr/tests/X.nex`
line for every test covered by Step 2's manifest. Delete the ENTIRE
`/usr/tests/*` portion of the inittab printf block's line list (all
`spawn`/`wait /usr/tests/*.nex` lines and their explanatory comments —
the comments' content should have already been preserved as a `"note"`
field in the manifest bundle if you want to keep it discoverable;
otherwise it's fine to leave the comments only in this plan/spec and
git history). Leave every non-`/usr/tests` line (boot-critical app
lines, comments about them) exactly where they are.

- [ ] **Step 5: Add the anchor-patch application step and mount `/usr/tests`**

```makefile
$(BUILD_DIR)/disk-src/inittab: build/embedfs_table.c
	printf '%s\n' \
	  ... (unchanged boot-critical lines only) ... \
	  > $(BUILD_DIR)/disk-src/inittab.base
	python3 tools/apply-inittab-patch.py $(BUILD_DIR)/disk-src/inittab.base \
	    build/embedfs-inittab-patch.json > $(BUILD_DIR)/disk-src/inittab
```

```python
#!/usr/bin/env python3
# tools/apply-inittab-patch.py <base-inittab> <patch.json>
# Applies each {"after": name, "line": text} in array order: finds the
# LAST line in the growing file containing f"/{name}.nex", inserts
# `line` immediately after it. Prints the result to stdout.
import json, sys

def main():
    with open(sys.argv[1]) as f:
        lines = [l.rstrip("\n") for l in f if l.strip() and not l.startswith("#")]
    with open(sys.argv[2]) as f:
        patch = json.load(f)
    for entry in patch:
        anchor = f"/{entry['after']}.nex"
        idx = max(i for i, l in enumerate(lines) if anchor in l)
        lines.insert(idx + 1, entry["line"])
    print("\n".join(lines))

if __name__ == "__main__":
    main()
```

(Comment lines are dropped here for simplicity — the base inittab
written by the Makefile's printf can keep its explanatory comments for
boot-critical entries since those aren't touched by the patch step; if
comment preservation through the patch matters, filter differently. Not
required for correctness of the boot sequence, since `/etc/inittab`'s
parser almost certainly already ignores `#`-prefixed lines — confirm
against `userland/init.c`'s inittab parser before assuming.)

Add the third mount:

```c
vfs_mount_fs("tests", "/usr/tests", "embedfs");
```

And update `EMBED_DIRS` usage in Task 2's generator invocation to
additionally scan wherever the ~65 tests' `.nex` files land (likely
already `$(DISK_SRC)/nex/`, the same directory `nexify.sh` always wrote
them to — no new directory needed here, unlike Task 2's boot-critical
apps which got their own `nex-embed-boot` dir).

- [ ] **Step 6: Update `REQUIRED_MARKERS`**

Split into:

```makefile
CORE_REQUIRED_MARKERS := \
	"[pci] ALL PASSED" \
	... (every entry that is NOT a userland test binary's marker --
	     i.e. keep [wxorx], [pci], [smp] local timer, [devfs], [tty],
	     [fb], [banner], [rtc], [keyboard], [input] and similar
	     in-kernel-selftest markers; remove every marker whose test now
	     has its own manifest from Step 2) \

REQUIRED_MARKERS = $(CORE_REQUIRED_MARKERS) $(shell cat build/embedfs-markers.txt 2>/dev/null)
```

(Note `=` not `:=` for `REQUIRED_MARKERS` now — it must be recursively
expanded so it re-reads `build/embedfs-markers.txt` AFTER that file is
generated by the `build/embedfs_table.c` recipe, which runs earlier in
the same `test:` prerequisite chain. Verify this ordering holds with
`make -n test` before trusting it.)

- [ ] **Step 7: Full verification**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
./tools/gauntlet.sh
```

Expected: identical `PASSED: N/N` to the ORIGINAL (Task 1) baseline —
this task is a pure mechanism change, not a coverage change. Any
missing marker means Step 2's manual cross-reference missed one; fix
the manifest, don't weaken the check.

- [ ] **Step 8: Commit**

```bash
git add tools/ Makefile kernel/kernel.c userland/tests.manifest.json
git commit -m "test: serve all kernel-native regression tests from embedfs, drive inittab/markers from manifests"
```

---

### Task 4: Wire BusyBox through the same manifest mechanism

**Files:**
- Create: `third_party/busybox-config/busybox.test.json`
- Modify: `Makefile` (busybox's binary now feeds `EMBED_DIRS` instead
  of a hardcoded `mcopy ... ::bin/busybox.nex` + hardcoded inittab
  lines)

- [ ] **Step 1: Write the manifest capturing the exact historical interleaving**

```json
{
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

(Verify these three marker strings against the CURRENT
`REQUIRED_MARKERS` list before trusting the ones written here — copy
them exactly rather than retyping.)

- [ ] **Step 2: Make `make busybox` deposit its manifest next to the binary, and feed it into `EMBED_DIRS`**

```makefile
$(BUSYBOX_BIN): $(MUSL_LIB) third_party/busybox-config/neoos.fragment \
                third_party/busybox-config/apply.sh $(USERLAND_DIR)/user.ld
	third_party/busybox-config/apply.sh
	$(MAKE) -C $(BUSYBOX_DIR) -j$(shell nproc)

# Stages busybox + its manifest into an embed-ready directory with the
# name the manifest/gen-embedfs.py expect (busybox.nex, matching
# BUSYBOX_BIN's basename convention already used by nexify.sh elsewhere).
$(BUILD_DIR)/ports-embed/busybox.nex: $(BUSYBOX_BIN)
	@mkdir -p $(BUILD_DIR)/ports-embed
	cp $(BUSYBOX_BIN) $@
	cp third_party/busybox-config/busybox.test.json $(BUILD_DIR)/ports-embed/busybox.test.json

EMBED_DIRS += $(BUILD_DIR)/ports-embed
```

(`EMBED_DIRS +=` here only takes effect if `$(BUILD_DIR)/ports-embed/busybox.nex`
was actually built first — i.e. `make busybox test` explicitly, not
plain `make test`. This preserves today's exact behavior: BusyBox is
opt-in via `make busybox`, and its tests only run/are required when it
was built. Make `build/embedfs_table.c`'s prerequisites include
`$(wildcard $(BUILD_DIR)/ports-embed/*.nex)` so it re-generates when
busybox appears or disappears from that directory.)

- [ ] **Step 3: Verify both ways**

```bash
# Without busybox: standalone-clean, no busybox markers required
make clean && ./tools/gauntlet.sh
# With busybox: full historical coverage restored
make busybox && ./tools/gauntlet.sh
```

Both must show `PASSED: N/N` — the first with a SMALLER required-marker
set (no `[bbspike]`/`[nshtest]`/`[bbsh]`) than the second, and the
second must exactly match the original (Task 1) baseline count.

- [ ] **Step 4: Commit**

```bash
git add third_party/busybox-config/busybox.test.json Makefile
git commit -m "test: BusyBox's gauntlet coverage is now manifest-driven, present only when built"
```

---

### Task 5: Extract `neoos-libneoos`

**Files (new repo, cloned/created similarly to the org-restructuring
plan's port tasks):**
- Move `lib/` (all of it: `crt0.asm`, `include/`, the syscall wrapper
  `.c` files) into a fresh `neoos-libneoos` repo.
- Add `Makefile` with the standard contract:

```makefile
.PHONY: all clean
all: build-output/lib/libneoos.a build-output/lib/crt0.o
	@mkdir -p build-output/include
	cp -r include/* build-output/include/

build-output/lib/libneoos.a: $(wildcard src/*.c)
	@mkdir -p build-output/lib build/obj
	for f in src/*.c; do \
	  $${CC:-x86_64-elf-gcc} -ffreestanding -fno-stack-protector -mno-red-zone \
	    -std=gnu11 -O2 -Iinclude -c $$f -o build/obj/$$(basename $$f .c).o; \
	done
	ar rcs $@ build/obj/*.o

build-output/lib/crt0.o: src/crt0.asm
	@mkdir -p build-output/lib
	$${AS:-x86_64-elf-as} src/crt0.asm -o $@

clean:
	rm -rf build build-output
```

(Match the EXACT flags `USER_CFLAGS` uses in this monorepo's Makefile
today — copy them verbatim rather than retyping from memory, since a
mismatch here would silently produce ABI-incompatible native binaries.)

- [ ] **Step 1: Create the repo, push source with the Makefile above**

```bash
gh repo create NeoOSOrganization/neoos-libneoos --public --description "NeoOS-native libc alternative to musl"
git clone git@github.com:NeoOSOrganization/neoos-libneoos.git /tmp/neoos-libneoos-work
cp -r /home/neo/projects/personal/NeoOS/lib/* /tmp/neoos-libneoos-work/
# (write the Makefile from above into /tmp/neoos-libneoos-work/Makefile)
cd /tmp/neoos-libneoos-work
git add -A
git commit -m "lib: initial extraction from the NeoOS monorepo, exact flags preserved"
git push origin main
```

- [ ] **Step 2: Verify standalone build**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
cd /tmp/neoos-libneoos-work && make
ls build-output/lib/libneoos.a build-output/lib/crt0.o
```

- [ ] **Step 3: Do NOT yet remove `lib/` from this monorepo**

This monorepo keeps building `lib/` in-tree exactly as today (matches
the org-restructuring plan's principle: this repo stays the primary
dev repo; `neoos-kernel` standalone consuming `neoos-libneoos`
externally is a `neoos-kernel`-repo-only concern, handled in Task 6).

---

### Task 6: Extract `neoos-kernel-tests-common`

**Files (new repo):**
- Move the ~65 test `.c` files from `userland/` (the ones covered by
  Task 3's `userland/tests.manifest.json`, i.e. everything except
  `init.c`, `login.c`, the terminal (`term/` dir), `nsh.c`/shell source
  — those stay in `neoos-kernel` per the spec's boot-critical category)
  plus `userland/tests.manifest.json` itself (split back into one
  `.test.json` per binary now that it's leaving the monorepo's single
  `Makefile` context, OR kept as one bundle file — either works per
  Task 3's Step 3 loader; keep as one bundle for less churn).
- `Makefile` building each `.c` against `LIBNEOOS_DIR` (native ABI
  ones) or `MUSL_DIR` (the 3 musl-linked ones: `login.c` stays in
  kernel per boot-critical, but `muslfork.c`/`muslhelo.c` are TESTS,
  not boot-critical, so they move here and link musl).

- [ ] **Step 1: Create the repo, copy test sources + manifest bundle + user.ld**

```bash
gh repo create NeoOSOrganization/neoos-kernel-tests-common --public \
  --description "NeoOS kernel regression test suite (embeds into neoos-kernel via embedfs)"
git clone git@github.com:NeoOSOrganization/neoos-kernel-tests-common.git /tmp/neoos-tests-work
mkdir -p /tmp/neoos-tests-work/src
cp /home/neo/projects/personal/NeoOS/userland/{spin,child,parent,looper,yielder,faulter,fileio,sse_test,fork_test,exec_target,mounttest,ttytest,tier0test,lfntest,direnttest,stattest,cwdtest,vfstest,...}.c /tmp/neoos-tests-work/src/
# (enumerate the FULL set from userland/tests.manifest.json's entries --
#  do not hand-guess the list; generate it with:
#  python3 -c "import json; print('\n'.join(e['_test'] for e in json.load(open('userland/tests.manifest.json'))))"
#  and copy each <name>.c plus muslfork.c/muslhelo.c from userland/musl/)
cp /home/neo/projects/personal/NeoOS/userland/user.ld /tmp/neoos-tests-work/
cp /home/neo/projects/personal/NeoOS/userland/tests.manifest.json /tmp/neoos-tests-work/
```

- [ ] **Step 2: Write the Makefile**

```makefile
LIBNEOOS_DIR ?= ../neoos-libneoos/build-output
MUSL_DIR     ?= ../neoos-musl/build-output
CC := x86_64-elf-gcc
NATIVE_CFLAGS := -ffreestanding -fno-stack-protector -mno-red-zone \
	-std=gnu11 -O2 -Wall -Wextra -I$(LIBNEOOS_DIR)/include
MUSL_CFLAGS := -static -nostdlib -nostdinc -ffreestanding \
	-mcmodel=large -fno-pic -mno-red-zone -fno-stack-protector -O2 \
	-isystem $(MUSL_DIR)/include

NATIVE_SRCS := $(filter-out src/muslfork.c src/muslhelo.c,$(wildcard src/*.c))
MUSL_SRCS   := src/muslfork.c src/muslhelo.c

.PHONY: all clean smoke-test
all: $(patsubst src/%.c,build/%.nex,$(NATIVE_SRCS)) $(patsubst src/%.c,build/%.nex,$(MUSL_SRCS))
	cp tests.manifest.json build/tests.manifest.json

build/%.nex: src/%.c user.ld
	@mkdir -p build
	$(CC) $(NATIVE_CFLAGS) -T user.ld -o $@ $(LIBNEOOS_DIR)/lib/crt0.o $< -L$(LIBNEOOS_DIR)/lib -lneoos

build/muslfork.nex: src/muslfork.c user.ld
	@mkdir -p build
	$(CC) $(MUSL_CFLAGS) -T user.ld -z noexecstack -o $@ $(MUSL_DIR)/lib/crt1.o $< -L$(MUSL_DIR)/lib -lc -lgcc

build/muslhelo.nex: src/muslhelo.c user.ld
	@mkdir -p build
	$(CC) $(MUSL_CFLAGS) -T user.ld -z noexecstack -o $@ $(MUSL_DIR)/lib/crt1.o $< -L$(MUSL_DIR)/lib -lc -lgcc

clean:
	rm -rf build
```

(The pattern rule `build/%.nex: src/%.c` is a simplification — a couple
of the current tests may need extra libs/flags beyond the common set
(check each `$(USERLAND_BUILD)/X.ELF:` rule in this monorepo's Makefile
before assuming uniformity; add per-file overrides here if any test's
current rule differs from the common pattern).)

- [ ] **Step 3: Verify standalone build against Task 5's libneoos and the org-restructuring plan's Task 1 musl**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
cd /tmp/neoos-tests-work
make LIBNEOOS_DIR=/tmp/neoos-libneoos-work/build-output MUSL_DIR=/tmp/neoos-musl-work/build-output
ls build/*.nex | wc -l   # expect ~65
```

- [ ] **Step 4: Commit and push**

```bash
git add -A
git commit -m "tests: initial extraction of the NeoOS kernel regression suite from the monorepo"
git push origin main
```

---

### Task 7: Wire `neoos-kernel` standalone against embedfs + external libneoos/tests

**Files (continues Task 2 of the org-restructuring-completion plan, on
the same `/tmp/neoos-kernel-work` clone):**
- Bring over `kernel/fs/embedfs.{h,c}`, `tools/gen-embedfs.py`,
  `tools/apply-inittab-patch.py`, the updated `kernel/kernel.c` mounts,
  and the trimmed `Makefile` (boot-critical-only inittab, `EMBED_DIRS`
  mechanism) from Tasks 1-4 above.
- Remove `lib/` from `neoos-kernel` (moved to `neoos-libneoos`); update
  the boot-critical apps' build rules (`init.c`, `login.c`'s musl link
  stays since login is musl-linked, `term`/`nsh` link libneoos) to take
  `LIBNEOOS_DIR ?= ../neoos-libneoos/build-output` instead of an
  in-repo `lib/`.

- [ ] **Step 1: Apply all of Tasks 1-4's monorepo changes to the `neoos-kernel` clone**

Since `neoos-kernel`'s `Makefile`/`kernel.c`/`kernel/fs/` are (after
the org-restructuring plan's Task 2) meant to track this monorepo's
kernel-relevant paths, port the same diffs here directly rather than
re-deriving them.

- [ ] **Step 2: Verify fully standalone**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
cd /tmp/neoos-kernel-work
make MUSL_DIR=/tmp/neoos-musl-work/build-output LIBNEOOS_DIR=/tmp/neoos-libneoos-work/build-output test
```

Expected: `PASSED: N/N` with ONLY `CORE_REQUIRED_MARKERS` (no test
suite, no BusyBox) — this is the actual standalone success criterion
from the org design spec, finally true.

- [ ] **Step 3: Verify WITH the full suite + BusyBox, matching original coverage**

```bash
EMBED_DIRS="/tmp/neoos-tests-work/build /tmp/neoos-busybox-work/build" \
  make MUSL_DIR=/tmp/neoos-musl-work/build-output LIBNEOOS_DIR=/tmp/neoos-libneoos-work/build-output test
```

Expected: `PASSED: N/N` matching the ORIGINAL pre-Task-1 baseline count
exactly.

- [ ] **Step 4: Push**

```bash
git add -A
git commit -m "build: embedfs-based standalone testing; libneoos and tests-common are now external"
git push origin main
```

---

### Task 8: Update the org-restructuring-completion plan and `neoos-os-builder`'s spec'd config

- [ ] **Step 1:** In
  `docs/superpowers/plans/2026-09-05-github-org-restructuring-completion.md`,
  update Task 2's "Known state going in" note and Task 7
  (os-builder)'s `scripts/build.sh` sketch to additionally clone
  `neoos-libneoos` and (only if `tests.include: true`)
  `neoos-kernel-tests-common`, passing both as `EMBED_DIRS` alongside
  ports. Update the repo count everywhere it's stated as "6" to "8".

- [ ] **Step 2:** Update `CLAUDE.md`'s restructuring section repo list
  to match §4 of this plan's spec (8 repos, not 6).

- [ ] **Step 3: Commit**

```bash
git add docs/superpowers/plans/2026-09-05-github-org-restructuring-completion.md CLAUDE.md
git commit -m "docs: fold the embedded test/app architecture into the org-restructuring tracking"
```

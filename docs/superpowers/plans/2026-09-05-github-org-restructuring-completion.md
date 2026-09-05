# GitHub Organization Restructuring — Completion Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Finish the NeoOS multi-repo migration for real — every repo in
`NeoOSOrganization` standalone-buildable exactly as its README claims,
ports genuinely split out with real upstream submodules, the OS builder
able to assemble a bootable ISO from the split repos, docs deployed, and
an honest project record (no more "complete" claims for work that
wasn't done).

**Architecture:** Six independent repos under the `NeoOSOrganization` org
(`neoos-kernel`, `neoos-musl`, `neoos-busybox`, `neoos-3d-ascii-viewer`,
`neoos-os-builder`, `neoos-docs`), each following the build contract in
the spec (`MUSL_DIR`/`KERNEL_SHIM_DIR` env vars, fixed output paths,
`make`/`make clean`/`make test` targets). This monorepo
(`Neo-vortex/NeoOS`) stays the primary dev repo; kernel work is
dual-pushed to `neoos-kernel` going forward (Task 12).

**Tech Stack:** GNU Make, bash, musl, x86_64-elf cross toolchain, GitHub
Actions, Docusaurus, `gh` CLI.

**Spec:** `docs/superpowers/specs/2026-09-04-github-organization-design.md`
(sections 3–8 define the contracts and migration phases this plan
executes).

## Global Constraints

- **musl shim is translation-only** (`CLAUDE.md`): `third_party/shim/`
  in `neoos-kernel` never emulates a primitive.
- **Struct layouts and constants crossing the syscall boundary stay
  Linux-shaped** (`CLAUDE.md`); this plan moves files, it does not
  change any ABI-facing behavior.
- **Kernel repo builds standalone**: fresh clone + `make test` works
  with only a musl artifact directory supplied externally (spec §3.1,
  §8.2).
- **musl repo output contract**: `build-output/include/` +
  `build-output/lib/libc.a` (+ `lib/crt1.o`, needed by every userland
  program) (spec §4.2).
- **Port repo output contract**: exactly one static `.nex` binary at
  `build/<portname>.nex`, built via `make MUSL_DIR=<path>` (spec §4.3).
- **Upstream submodules stay pristine** — NeoOS-specific changes live
  in sibling `config/`/`patches/` dirs, applied by an idempotent
  `apply.sh`, never edited in place (spec §3.4, matches
  `third_party/shim/apply.sh`'s existing pattern).
- **No gauntlet regressions**: every task that touches
  `third_party/shim`, the kernel `Makefile`, or musl build flags ends
  with `tools/gauntlet.sh` reporting `PGAUNTLET PASSED: N/N` on this
  monorepo before anything is pushed to `neoos-kernel`.
- **Work happens on `main`** in this monorepo (no feature branches, per
  `CLAUDE.md`); each org repo also takes pushes directly to its `main`.

## Known state going in (verified 2026-09-05)

- Org `NeoOSOrganization` exists; all 6 repos exist and are non-empty.
- `neoos-kernel` has a full file tree but its `Makefile` is an
  unmodified copy of this repo's — it still hardcodes
  `MUSL_DIR := third_party/musl` and references
  `third_party/busybox-config` and `ports/`, none of which exist in
  that repo. It does not build standalone as its README claims.
- `neoos-musl` has a real `upstream/` submodule, but `build.sh` fakes
  shim integration (`cp *.h arch/x86_64/bits/` — not what
  `third_party/shim/apply.sh` actually does) and drops the
  `-mcmodel=large -fno-pic -mno-red-zone` flags every NeoOS userland
  binary requires. It would produce a libc.a NeoOS can't link against.
- `neoos-busybox` / `neoos-3d-ascii-viewer` have only
  `Makefile`/`README.md`/`smoke-test.sh` scaffolding — no submodule, no
  source, no config/patches.
- `neoos-os-builder` has docs/README/Makefile placeholders, no
  orchestration logic.
- `neoos-docs` has Docusaurus scaffolding, unknown content/deploy
  status.
- `CLAUDE.md` (this repo) and the untracked
  `docs/superpowers/plans/2026-09-05-phase{1..5}-*.md` files assert the
  whole migration is done. It isn't, past kernel/musl scaffolding.
  Task 0 corrects this.
- `third_party/shim/apply.sh` **already** supports an external musl
  checkout via `MUSL_DIR` (commits `82f0b90`, `26e92d3`) — this was
  real, correct prep work. Task 2 builds on it rather than redoing it.

---

### Task 0: Correct the record before building on it

**Files:**
- Modify: `CLAUDE.md` (the "GitHub Organization (2026-09-05
  Restructuring)" section)
- Modify (rename): `docs/superpowers/plans/2026-09-05-phase1-completion.md`
  through `2026-09-05-phase5-docs-deployment.md`
- Create: `docs/superpowers/plans/archive/` directory for the above

**Interfaces:** None (docs-only).

- [ ] **Step 1: Move the premature completion docs out of the active plans directory**

```bash
mkdir -p docs/superpowers/plans/archive
git mv docs/superpowers/plans/2026-09-05-phase1-completion.md docs/superpowers/plans/archive/ 2>/dev/null || \
  mv docs/superpowers/plans/2026-09-05-phase1-completion.md docs/superpowers/plans/archive/
for f in 2026-09-05-phase2-completion.md 2026-09-05-phase2-musl-extraction.md \
         2026-09-05-phase2-progress.md 2026-09-05-phase3-completion.md \
         2026-09-05-phase3-port-migration.md 2026-09-05-phase4-os-builder.md \
         2026-09-05-phase5-docs-deployment.md; do
  mv "docs/superpowers/plans/$f" "docs/superpowers/plans/archive/$f"
done
```

- [ ] **Step 2: Add a one-line header to each archived file marking it aspirational, not historical**

Prepend to each file in `docs/superpowers/plans/archive/2026-09-05-phase*.md`:

```markdown
> **Status note (2026-09-05):** This document was written as a planning
> artifact but was treated as a completion report despite the described
> work not having happened (verified against the live repos — see
> `docs/superpowers/plans/2026-09-05-github-org-restructuring-completion.md`).
> Kept for the original task breakdown; do not trust its "Complete"/"✅"
> claims.
```

- [ ] **Step 3: Rewrite the CLAUDE.md restructuring section to describe reality, not the end state**

Replace the "As of 2026-09-05, NeoOS has been restructured..." opening
paragraph with:

```markdown
As of 2026-09-05, restructuring NeoOS into a distributed GitHub
organization is **in progress**, tracked in
`docs/superpowers/plans/2026-09-05-github-org-restructuring-completion.md`.
This repo remains the primary development repo until that plan's
cutover task (Task 13) lands. The section below describes the target
end state, not current fact — check the plan file for what's actually
done.
```

Leave the rest of the organization/structure/workflow description
as-is (it documents the target contract accurately); only the framing
sentence was false.

- [ ] **Step 4: Commit**

```bash
git add -A CLAUDE.md docs/superpowers/plans/
git commit -m "docs: mark the org-restructuring 'completion' reports as aspirational, not done"
```

---

### Task 1: Fix `neoos-musl`'s build script to use the real shim, not a fake one

**Files (in a local clone of `neoos-musl`, not this monorepo):**
- Modify: `build.sh`
- Test: manual build run against this monorepo's `third_party/shim/`

**Interfaces:**
- Consumes: `third_party/shim/apply.sh` from `neoos-kernel` (already
  supports being pointed at an external musl tree via the `MUSL_DIR`
  env var it reads — see apply.sh's own `musl="${MUSL_DIR:-$here/../musl}"`).
  Note the name collision: `neoos-musl`'s Makefile also has a variable
  called `MUSL_DIR` in the design spec text, but its actual Makefile
  (checked into the repo now) doesn't define one — only
  `KERNEL_SHIM_DIR`, `PREFIX`, `UPSTREAM_DIR`. Step 1 below sets the
  shim's expected `MUSL_DIR` env var explicitly to avoid ambiguity.
- Produces: `build-output/include/`, `build-output/lib/libc.a`,
  `build-output/lib/crt1.o` — consumed by Task 3 (kernel) and Tasks 5–6
  (ports).

- [ ] **Step 1: Clone neoos-musl locally for iteration**

```bash
git clone git@github.com:NeoOSOrganization/neoos-musl.git /tmp/neoos-musl-work
cd /tmp/neoos-musl-work
git submodule update --init upstream
```

- [ ] **Step 2: Replace the fake shim integration and wrong compiler flags in `build.sh`**

Replace the body of `build.sh` (from `echo "Configuring musl..."`
through the `make install` block) with:

```bash
echo "Integrating NeoOS syscall shim..."
MUSL_DIR="$(cd "$UPSTREAM_DIR" && pwd)" "$KERNEL_SHIM_DIR/apply.sh"

cd "$UPSTREAM_DIR"

echo "Configuring musl..."
./configure \
    --prefix="$(cd .. && pwd)/$PREFIX" \
    --target=x86_64 \
    --disable-shared \
    CC="${CC:-x86_64-elf-gcc}" \
    AR="${AR:-x86_64-elf-ar}" \
    RANLIB="${RANLIB:-x86_64-elf-ranlib}" \
    CFLAGS="-mcmodel=large -fno-pic -mno-red-zone -O2" 2>&1 | tail -5

echo "Building musl..."
make -j"$(nproc)"
echo "  OK Build complete"

echo "Installing musl to $PREFIX..."
make install
echo "  OK Install complete"
```

Note the three fixes versus the old script: (1) `--target=x86_64`, not
the invalid `x86_64-elf`; (2) shim applied via the real `apply.sh`
mechanism instead of a bare header copy; (3) `-mcmodel=large -fno-pic
-mno-red-zone` restored — every NeoOS userland binary is linked at
`0x200000000000` and traps without these (see this monorepo's
`Makefile` `MUSL_CFLAGS`/musl-build comment for why).

- [ ] **Step 3: Run it against this monorepo's shim and verify output**

```bash
KERNEL_SHIM_DIR=/home/neo/projects/personal/NeoOS/third_party/shim \
  UPSTREAM_DIR=upstream PREFIX=build-output ./build.sh
ls build-output/lib/libc.a build-output/lib/crt1.o build-output/include/stdio.h
```

Expected: all three paths exist, `libc.a` built without errors in the
tail-5 configure output or the `make -j` step.

- [ ] **Step 4: Diff `upstream/arch/x86_64/syscall_arch.h` against its `.orig` to confirm the shim actually applied**

```bash
diff upstream/arch/x86_64/syscall_arch.h upstream/arch/x86_64/syscall_arch.h.orig
```

Expected: a real diff (shim content vs. vanilla musl), not "files
identical." If identical, `apply.sh` silently no-op'd — stop and debug
before continuing.

- [ ] **Step 5: Push the fix**

```bash
git add build.sh
git commit -m "build: integrate the real NeoOS shim via apply.sh, restore userland ABI flags"
git push origin main
```

---

### Task 2: Make `neoos-kernel`'s Makefile actually consume an external musl

**Files (local clone of `neoos-kernel`):**
- Modify: `Makefile` (the musl section, ~lines 130–162 in this
  monorepo's copy)

**Interfaces:**
- Consumes: `build-output/{include/,lib/libc.a,lib/crt1.o}` from Task 1.
- Produces: `MUSL_LIB`, `MUSL_CFLAGS` make variables — consumed by every
  downstream userland-build rule already in the Makefile (unchanged).

- [ ] **Step 1: Clone neoos-kernel locally for iteration**

```bash
git clone git@github.com:NeoOSOrganization/neoos-kernel.git /tmp/neoos-kernel-work
cd /tmp/neoos-kernel-work
```

- [ ] **Step 2: Make `MUSL_DIR` overridable and point it at an installed-layout tree by default**

Replace:

```makefile
MUSL_DIR   := third_party/musl
MUSL_LIB   := $(MUSL_DIR)/lib/libc.a
MUSL_CFLAGS := -static -nostdlib -nostdinc -ffreestanding \
	-mcmodel=large -fno-pic -mno-red-zone -fno-stack-protector -O2 \
	-isystem $(MUSL_DIR)/include -isystem $(MUSL_DIR)/arch/x86_64 \
	-isystem $(MUSL_DIR)/arch/generic -isystem $(MUSL_DIR)/obj/include
```

with:

```makefile
# Standalone default: build musl in-repo isn't possible here (musl
# source lives in neoos-musl now) — MUSL_DIR must point at a musl
# build-output tree (neoos-musl's `make` output: include/, lib/libc.a,
# lib/crt1.o). See neoos-musl's README for how to produce one.
MUSL_DIR   ?= ../neoos-musl/build-output
MUSL_LIB   := $(MUSL_DIR)/lib/libc.a
MUSL_CFLAGS := -static -nostdlib -nostdinc -ffreestanding \
	-mcmodel=large -fno-pic -mno-red-zone -fno-stack-protector -O2 \
	-isystem $(MUSL_DIR)/include
```

- [ ] **Step 3: Delete the in-repo musl build rule (musl is no longer built here)**

Remove the `$(MUSL_LIB): $(wildcard third_party/shim/*.c ...)` rule and
the `.PHONY: musl` / `musl: $(MUSL_LIB)` target. Add instead:

```makefile
$(MUSL_LIB):
	@echo "error: $(MUSL_LIB) not found." >&2
	@echo "Build it in neoos-musl first: cd ../neoos-musl && make KERNEL_SHIM_DIR=$(CURDIR)/third_party/shim" >&2
	@exit 1
```

- [ ] **Step 4: Every rule that references `$(MUSL_DIR)/lib/crt1.o` still works unchanged**

Confirm (grep, don't edit) that `crt1.o` references
(`$(MUSL_DIR)/lib/crt1.o` in the `LOGIN.ELF`/`MUSLFORK.ELF`/etc. rules)
now resolve under the installed layout — they already point at
`$(MUSL_DIR)/lib/crt1.o`, which Task 1's `make install` produces, so no
change needed there.

- [ ] **Step 5: Strip the busybox and ports targets from this repo's Makefile**

`neoos-kernel` doesn't contain `third_party/busybox-config` or
`ports/` (spec §3.1 "Does NOT contain"). Delete the `# ---- busybox
----` section and the `# ---- ports (3d-ascii-viewer) ----` section
(including `AV_SRCS`/`AV_BIN`/`ports:` target), and the two
conditional blocks in the disk-image rule that reference `$(AV_BIN)`
and `$(BUSYBOX_BIN)` (leave the disk image buildable without them —
those blocks are already `@if [ -f ... ]` guards, so removing the
targets that would have produced those files is enough; the disk image
build just skips including them).

- [ ] **Step 6: Verify standalone build against a real external musl**

```bash
# In neoos-musl-work from Task 1, produce build-output if not already there.
cd /tmp/neoos-kernel-work
make MUSL_DIR=/tmp/neoos-musl-work/build-output test
```

Expected: `PGAUNTLET PASSED: N/N` in the serial log, same N as this
monorepo's current gauntlet baseline (check
`docs/superpowers/plans/` gauntlet baseline memory / recent `make
test` output for the current N before comparing).

- [ ] **Step 7: Push**

```bash
git add Makefile
git commit -m "build: take musl as an external artifact dir, drop in-repo busybox/ports targets"
git push origin main
```

---

### Task 3: Regression-check the monorepo's own Makefile is untouched

This plan does not change how `main` in `Neo-vortex/NeoOS` builds — the
`?=`-style default in Task 2 only changes behavior when `MUSL_DIR` is
overridden. This monorepo keeps building musl in-tree from
`third_party/musl` exactly as before.

**Files:** None modified. Verification only.

- [ ] **Step 1: Confirm this repo's Makefile was not touched by Tasks 1–2**

```bash
git status --short Makefile
```

Expected: no output (clean).

- [ ] **Step 2: Run the gauntlet once as a baseline snapshot before further changes**

```bash
./tools/gauntlet.sh
```

Record the `PASSED: N/N` count for comparison after Task 4 onward
touches shared files like `third_party/busybox-config`.

---

### Task 4: Real BusyBox port migration into `neoos-busybox`

**Files (local clone of `neoos-busybox`):**
- Create: `upstream/` (submodule), `config/neoos.fragment`,
  `config/apply.sh` (adapted from this monorepo's
  `third_party/busybox-config/apply.sh`)
- Modify: `Makefile`

**Interfaces:**
- Consumes: `build-output/` from Task 1 via `MUSL_DIR` env var (spec
  §4.3).
- Produces: `build/busybox.nex` — consumed by Task 8 (os-builder).

- [ ] **Step 1: Clone neoos-busybox and add the real upstream submodule**

```bash
git clone git@github.com:NeoOSOrganization/neoos-busybox.git /tmp/neoos-busybox-work
cd /tmp/neoos-busybox-work
git submodule add https://git.busybox.net/busybox upstream
git submodule update --init --recursive
```

Use the same commit/tag this monorepo's `third_party/busybox` submodule
currently points at, so the port doesn't silently jump versions:

```bash
cd /home/neo/projects/personal/NeoOS/third_party/busybox && git rev-parse HEAD
cd /tmp/neoos-busybox-work/upstream && git checkout <that-sha>
cd .. && git add upstream && git commit -m "pin upstream busybox to the version already validated in the monorepo"
```

- [ ] **Step 2: Copy the NeoOS-specific config into `config/`, keep the same defensive logic**

```bash
mkdir -p config
cp /home/neo/projects/personal/NeoOS/third_party/busybox-config/neoos.fragment config/
cp /home/neo/projects/personal/NeoOS/third_party/busybox-config/apply.sh config/
```

Edit `config/apply.sh`'s path defaults so it targets `upstream/`
relative to this repo instead of `../busybox` (mirror the same
`MUSL_DIR`-style override pattern Task 1 used for musl — call the
variable `BUSYBOX_DIR` here to avoid the musl-path naming collision):

```sh
here=$(cd "$(dirname "$0")" && pwd)
bb="${BUSYBOX_DIR:-$here/../upstream}"
```

(Apply this rename everywhere the script currently says `busybox=` /
references the old relative path — read the full script first, since
it's long and defensive by design per its own header comment; preserve
all three defensive behaviors described there.)

- [ ] **Step 3: Rewrite the Makefile to the port build contract**

```makefile
MUSL_DIR ?= ../neoos-musl/build-output
BUSYBOX_DIR ?= upstream

CC := x86_64-elf-gcc
MUSL_CFLAGS := -static -nostdlib -nostdinc -ffreestanding \
	-mcmodel=large -fno-pic -mno-red-zone -fno-stack-protector -O2 \
	-isystem $(MUSL_DIR)/include

build/busybox.nex: config/apply.sh config/neoos.fragment
	@[ -f $(MUSL_DIR)/lib/libc.a ] || { echo "error: musl not found at $(MUSL_DIR); build neoos-musl first" >&2; exit 1; }
	BUSYBOX_DIR=$(BUSYBOX_DIR) config/apply.sh
	$(MAKE) -C $(BUSYBOX_DIR) -j$(shell nproc) \
		CC="$(CC)" CFLAGS_EXTRA="$(MUSL_CFLAGS)" \
		LDFLAGS="-L$(MUSL_DIR)/lib -lc"
	mkdir -p build
	cp $(BUSYBOX_DIR)/busybox build/busybox.nex

.PHONY: clean smoke-test
clean:
	rm -rf build
	$(MAKE) -C $(BUSYBOX_DIR) clean 2>/dev/null || true

smoke-test: build/busybox.nex
	./smoke-test.sh
```

(This mirrors this monorepo's existing `busybox: $(BUSYBOX_BIN)` rule —
same compiler, same config-apply step — just repointed at the
standalone `MUSL_DIR` contract instead of the in-tree
`third_party/musl` path.)

- [ ] **Step 4: Verify standalone build**

```bash
make MUSL_DIR=/tmp/neoos-musl-work/build-output
ls -la build/busybox.nex
file build/busybox.nex   # expect: ELF 64-bit LSB executable, x86-64, statically linked
```

- [ ] **Step 5: Write `smoke-test.sh` per the spec's contract (§4.4)**

```bash
#!/bin/bash
set -e
[ -x build/busybox.nex ] || { echo "busybox.nex missing or not executable"; exit 1; }
file build/busybox.nex | grep -q "statically linked" || { echo "not statically linked"; exit 1; }
echo "smoke-test: OK"
```

(The spec's example runs applets *inside a NeoOS boot*, which this
standalone repo can't do without the kernel — that half moves to the
kernel's own regression harness, added in Task 7.)

- [ ] **Step 6: Commit and push**

```bash
git add -A
git commit -m "port: real busybox source, config, and standalone build against neoos-musl"
git push origin main
```

---

### Task 5: Real 3D ASCII Viewer port migration into `neoos-3d-ascii-viewer`

Mirrors Task 4. **Files (local clone):**
- Create: `upstream/` (submodule from
  `https://github.com/autopawn/3d-ascii-viewer.git`, pinned to the SHA
  this monorepo's `ports/3d-ascii-viewer/upstream` submodule currently
  points at), `ncurses-shim/` (copied verbatim from
  `ports/3d-ascii-viewer/ncurses-shim/` in this monorepo — it's NeoOS
  glue code, not upstream, so it moves as source, not a submodule)
- Modify: `Makefile`

**Interfaces:**
- Consumes: `build-output/` from Task 1 via `MUSL_DIR`.
- Produces: `build/3d-ascii-viewer.nex` — consumed by Task 8.

- [ ] **Step 1: Clone, add pinned submodule, copy the shim**

```bash
git clone git@github.com:NeoOSOrganization/neoos-3d-ascii-viewer.git /tmp/neoos-3d-viewer-work
cd /tmp/neoos-3d-viewer-work
git submodule add https://github.com/autopawn/3d-ascii-viewer.git upstream
cd /home/neo/projects/personal/NeoOS/ports/3d-ascii-viewer/upstream && git rev-parse HEAD
cd /tmp/neoos-3d-viewer-work/upstream && git checkout <that-sha>
cd ..
cp -r /home/neo/projects/personal/NeoOS/ports/3d-ascii-viewer/ncurses-shim .
git add upstream ncurses-shim
git commit -m "port: pinned upstream 3d-ascii-viewer + NeoOS ncurses shim"
```

- [ ] **Step 2: Rewrite the Makefile against the standalone build contract**

```makefile
MUSL_DIR ?= ../neoos-musl/build-output
AV_SHIM  := ncurses-shim
AV_SRCS  := $(wildcard upstream/src/*.c) $(AV_SHIM)/ncurses_shim.c

CC := x86_64-elf-gcc
MUSL_CFLAGS := -static -nostdlib -nostdinc -ffreestanding \
	-mcmodel=large -fno-pic -mno-red-zone -fno-stack-protector -O2 \
	-isystem $(MUSL_DIR)/include -I$(AV_SHIM)

build/3d-ascii-viewer.nex: $(AV_SRCS)
	@[ -f $(MUSL_DIR)/lib/libc.a ] || { echo "error: musl not found at $(MUSL_DIR); build neoos-musl first" >&2; exit 1; }
	mkdir -p build
	$(CC) $(MUSL_CFLAGS) -z noexecstack \
		-o $@ $(MUSL_DIR)/lib/crt1.o $(AV_SRCS) \
		-L$(MUSL_DIR)/lib -lc -lgcc

.PHONY: clean smoke-test
clean:
	rm -rf build

smoke-test: build/3d-ascii-viewer.nex
	./smoke-test.sh
```

(This is a direct repoint of this monorepo's existing `$(AV_BIN)` rule
— same flags, same shim, just the `MUSL_DIR` contract instead of the
in-tree path. Note this needs a `user.ld` linker script; check whether
`userland/user.ld` in the kernel repo is required here too — if the
upstream 3d-ascii-viewer build needs it, vendor a copy into this repo
under `config/user.ld` rather than depending on a kernel-repo path,
since this repo must build without `neoos-kernel` checked out per the
"Does NOT contain: Kernel source" rule and the port build contract only
requires `MUSL_DIR`.)

- [ ] **Step 3: Verify standalone build**

```bash
make MUSL_DIR=/tmp/neoos-musl-work/build-output
file build/3d-ascii-viewer.nex
```

- [ ] **Step 4: Write `smoke-test.sh`** (same shape as Task 4 Step 5, checking the `.nex` exists and is static)

- [ ] **Step 5: Decide and fix repo visibility**

This repo is currently **private** while every other port repo is
public — inconsistent with the "porting guide" / port-catalog goal in
spec §3.5, and it means `neoos-docs`' port catalog can't link a public
page to it. Ask the user whether that was deliberate before flipping it:

```bash
gh repo view NeoOSOrganization/neoos-3d-ascii-viewer --json visibility
```

If not deliberate: `gh repo edit NeoOSOrganization/neoos-3d-ascii-viewer --visibility public`

- [ ] **Step 6: Commit and push**

```bash
git add -A
git commit -m "port: standalone build for 3d-ascii-viewer against neoos-musl"
git push origin main
```

---

### Task 6: Wire port smoke tests into the kernel's own regression harness

**Files (local clone of `neoos-kernel`):**
- Modify: `userland/` or `tools/` regression harness (whichever file
  currently drives `tools/gauntlet.sh`'s subsystem list in this
  monorepo) to add an *optional*, separately-invoked "ports" pass, not
  part of the default gauntlet (mirrors this monorepo's existing
  `make ports` being "Built on request, NOT by `make test`" — same
  separation, just across repos now).

**Interfaces:**
- Consumes: `build/busybox.nex` (Task 4) and `build/3d-ascii-viewer.nex`
  (Task 5), copied in by whoever runs this — the kernel repo doesn't
  clone port repos itself (spec §3.1 "Does NOT contain: Ports"); that
  orchestration lives in `neoos-os-builder` (Task 8).

- [ ] **Step 1: Add a `make ports-smoke-test PORT_BINS=<dir>` target to `neoos-kernel`'s Makefile**

```makefile
.PHONY: ports-smoke-test
ports-smoke-test:
	@[ -n "$(PORT_BINS)" ] || { echo "usage: make ports-smoke-test PORT_BINS=<dir of .nex files>" >&2; exit 1; }
	./tools/nexify.sh $(PORT_BINS)/busybox.nex $(BUILD_DIR)/disk/nex/busybox.nex 2>/dev/null || true
	./tools/nexify.sh $(PORT_BINS)/3d-ascii-viewer.nex $(BUILD_DIR)/disk/nex/av.nex 2>/dev/null || true
	$(MAKE) test
```

- [ ] **Step 2: Verify end-to-end using the binaries built in Tasks 4–5**

```bash
mkdir -p /tmp/port-bins
cp /tmp/neoos-busybox-work/build/busybox.nex /tmp/neoos-3d-viewer-work/build/3d-ascii-viewer.nex /tmp/port-bins/
cd /tmp/neoos-kernel-work
make ports-smoke-test PORT_BINS=/tmp/port-bins MUSL_DIR=/tmp/neoos-musl-work/build-output
```

Expected: gauntlet still `PASSED: N/N`, and the serial log shows the
BusyBox/3D-viewer boot entries this monorepo's existing `make test`
already reports when those binaries are present (check current `make
test` serial log output for the exact marker strings before writing an
assertion — don't invent marker text that doesn't match what the
kernel actually prints).

- [ ] **Step 3: Commit and push**

```bash
git add Makefile
git commit -m "test: accept externally-built port binaries for the regression harness"
git push origin main
```

---

### Task 7: `neoos-os-builder` — Makefile-driven orchestration (TUI deferred)

The spec (§7 "Open Questions") explicitly leaves the TUI's implementation
language undecided. Building a TUI is a separate, later decision — this
task delivers the **config-driven** mode only (`neoos-builder build
config.yaml` from spec §3.3), which is what actually has a fully
specified contract today.

**Files (local clone of `neoos-os-builder`):**
- Create: `Makefile`, `config/example.yaml`, `scripts/build.sh`,
  `scripts/qemu-run.sh.template`

**Interfaces:**
- Consumes: build contracts from `neoos-kernel` (Task 2),
  `neoos-musl` (Task 1), `neoos-busybox` (Task 4),
  `neoos-3d-ascii-viewer` (Task 5) — clones each, runs their `make`
  with the documented env vars.
- Produces: `build/<name>.iso`, `build/disk{1,2}.img`,
  `build/metadata.json`, `build/qemu-run.sh` (spec §3.3 "Output
  directory").

- [ ] **Step 1: Write the example config**

```yaml
kernel:
  version: "main"
ports:
  - busybox
  - 3d-ascii-viewer
iso:
  name: "neoos-custom-build"
  disk_size: 2G
```

- [ ] **Step 2: Write `scripts/build.sh` implementing the build process from spec §3.3.3**

```bash
#!/bin/bash
set -euo pipefail
CONFIG="${1:?usage: build.sh <config.yaml>}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# minimal YAML read without a parser dependency: grep the two lists we need
KERNEL_VERSION=$(grep -A1 '^kernel:' "$CONFIG" | grep version | sed 's/.*: *"\(.*\)"/\1/')
PORTS=$(sed -n '/^ports:/,/^[a-z]/p' "$CONFIG" | grep '^\s*-' | sed 's/^\s*-\s*//')
ISO_NAME=$(grep -A2 '^iso:' "$CONFIG" | grep name | sed 's/.*: *"\(.*\)"/\1/')

git clone --branch "$KERNEL_VERSION" https://github.com/NeoOSOrganization/neoos-kernel "$WORK/kernel"
git clone https://github.com/NeoOSOrganization/neoos-musl "$WORK/musl"
(cd "$WORK/musl" && make KERNEL_SHIM_DIR="$WORK/kernel/third_party/shim")

mkdir -p "$WORK/port-bins"
for port in $PORTS; do
  git clone "https://github.com/NeoOSOrganization/neoos-$port" "$WORK/$port"
  (cd "$WORK/$port" && make MUSL_DIR="$WORK/musl/build-output")
  cp "$WORK/$port/build/$port.nex" "$WORK/port-bins/"
done

(cd "$WORK/kernel" && make MUSL_DIR="$WORK/musl/build-output" iso disk-image \
  ports-smoke-test PORT_BINS="$WORK/port-bins")

mkdir -p build
cp "$WORK/kernel/build/neoos.iso" "build/$ISO_NAME.iso"
cp "$WORK/kernel/build/disk"*.img build/
cat > build/metadata.json <<EOF
{"kernel_version": "$KERNEL_VERSION", "ports": "$(echo $PORTS | tr '\n' ',')", "built_at": "$(date -u +%FT%TZ)"}
EOF
sed "s/@ISO@/$ISO_NAME.iso/" scripts/qemu-run.sh.template > build/qemu-run.sh
chmod +x build/qemu-run.sh
echo "OK: build/$ISO_NAME.iso"
```

- [ ] **Step 3: Write `scripts/qemu-run.sh.template`**

```sh
#!/bin/bash
timeout 180 qemu-system-x86_64 -cpu Nehalem -boot order=d \
  -cdrom @ISO@ \
  -drive file=disk1.img,format=raw \
  -drive file=disk2.img,format=raw \
  -display none -no-reboot -serial file:qemu.log
```

- [ ] **Step 4: Write the `Makefile` front door**

```makefile
.PHONY: build clean
build: config/example.yaml
	./scripts/build.sh $(CONFIG)
clean:
	rm -rf build
```

(`CONFIG` defaults to `config/example.yaml` if unset — add
`CONFIG ?= config/example.yaml` above the target.)

- [ ] **Step 5: Verify end-to-end**

```bash
make CONFIG=config/example.yaml
ls build/neoos-custom-build.iso build/qemu-run.sh
cd build && ./qemu-run.sh && grep -q "PGAUNTLET PASSED" qemu.log
```

Expected: the assembled ISO boots and passes the gauntlet — this is
spec §8's success criterion 4–5 ("under 10 minutes" and "boots and
includes all selected ports").

- [ ] **Step 6: Commit and push**

```bash
git add -A
git commit -m "builder: config-driven ISO assembly from the split repos (TUI deferred per spec open question)"
git push origin main
```

---

### Task 8: CI workflows for all five buildable repos

**Files (one per repo, local clones):**
- Create: `.github/workflows/build.yml` in `neoos-kernel`,
  `neoos-musl`, `neoos-busybox`, `neoos-3d-ascii-viewer`,
  `neoos-os-builder`

- [ ] **Step 1: `neoos-musl/.github/workflows/build.yml`**

```yaml
name: build
on: [push, pull_request]
jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
        with: { submodules: recursive }
      - name: checkout kernel (for shim)
        run: git clone --depth 1 https://github.com/NeoOSOrganization/neoos-kernel ../neoos-kernel
      - name: install cross toolchain
        run: ../neoos-kernel/toolchain/build.sh
      - run: make KERNEL_SHIM_DIR=../neoos-kernel/third_party/shim
      - run: make verify
```

- [ ] **Step 2: `neoos-kernel/.github/workflows/build.yml`**

```yaml
name: build-and-test
on: [push, pull_request]
jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: install QEMU and cross toolchain
        run: |
          sudo apt-get update && sudo apt-get install -y qemu-system-x86 xorriso
          ./toolchain/build.sh
      - name: checkout and build musl
        run: |
          git clone --depth 1 https://github.com/NeoOSOrganization/neoos-musl ../neoos-musl
          (cd ../neoos-musl && git submodule update --init upstream && make KERNEL_SHIM_DIR=$(pwd)/../$(basename $PWD)/third_party/shim)
      - run: make MUSL_DIR=../neoos-musl/build-output test
      - name: fail on gauntlet regression
        run: grep -q "PGAUNTLET PASSED" build/qemu.log
```

(GitHub-hosted runners have no KVM — QEMU runs under TCG emulation.
Expect `make test` to take noticeably longer than local runs; if CI
times out, this is the first thing to tune, e.g. by trimming the
regression suite's timeout budget or requesting a self-hosted runner —
don't silently drop test coverage to fix a timeout.)

- [ ] **Step 3: `neoos-busybox/.github/workflows/build.yml`** and
      **`neoos-3d-ascii-viewer/.github/workflows/build.yml`** (same shape)

```yaml
name: build
on: [push, pull_request]
jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
        with: { submodules: recursive }
      - run: git clone --depth 1 https://github.com/NeoOSOrganization/neoos-kernel ../neoos-kernel
      - run: ../neoos-kernel/toolchain/build.sh
      - run: |
          git clone --depth 1 https://github.com/NeoOSOrganization/neoos-musl ../neoos-musl
          (cd ../neoos-musl && git submodule update --init upstream && make KERNEL_SHIM_DIR=$(pwd)/../neoos-kernel/third_party/shim)
      - run: make MUSL_DIR=../neoos-musl/build-output
      - run: make smoke-test
```

- [ ] **Step 4: `neoos-os-builder/.github/workflows/build.yml`**

```yaml
name: build
on: [push, pull_request]
jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - run: sudo apt-get update && sudo apt-get install -y qemu-system-x86 xorriso
      - run: make CONFIG=config/example.yaml
```

- [ ] **Step 5: Push each, then confirm all five show green in the org**

```bash
for r in neoos-kernel neoos-musl neoos-busybox neoos-3d-ascii-viewer neoos-os-builder; do
  gh run list --repo NeoOSOrganization/$r --limit 1
done
```

---

### Task 9: `neoos-docs` content

**Files (local clone of `neoos-docs`):**
- Create: `docs/getting-started.md`, `docs/kernel-development.md`,
  `docs/porting-guide.md`, `docs/api-reference.md`,
  `docs/architecture/*.md`, `docs/port-catalog.md`,
  `docs/abi-compatibility.md`

- [ ] **Step 1: Getting Started** — clone+build kernel, clone+build a
      custom OS image via `neoos-os-builder`, run it in QEMU. Base this
      directly on Task 2 Step 6 and Task 7 Step 5's verified commands —
      copy the exact commands that were proven to work, not idealized
      ones.

- [ ] **Step 2: Porting Guide** — write up Tasks 4–5 as the worked
      example: submodule pinning, `config/apply.sh` pattern, the
      `MUSL_DIR` contract, common pitfalls section listing the two real
      bugs found and fixed in this plan (missing `-mcmodel=large` etc.
      in Task 1; stale `MUSL_DIR`/`third_party/*` references in Task 2)
      so the next port author doesn't repeat them.

- [ ] **Step 3: API Reference** — pull the syscall table from this
      monorepo's `docs/stdlib.md` (copy content, add a note that
      `neoos-kernel/docs/stdlib.md` is the source of truth per spec
      §3.5 "no hard copy... avoid divergence" — link rather than
      duplicate where the content is large).

- [ ] **Step 4: Architecture Deep Dives** — one page per subsystem
      (scheduler, memory, VFS, signals), each linking to the relevant
      spec doc in `neoos-kernel/docs/superpowers/specs/` on GitHub
      rather than re-explaining it.

- [ ] **Step 5: Port Catalog** — table of `busybox` / `3d-ascii-viewer`
      with links to their repos and CI badges:
      `![build](https://github.com/NeoOSOrganization/neoos-busybox/actions/workflows/build.yml/badge.svg)`

- [ ] **Step 6: ABI Compatibility** — copy this monorepo's
      `docs/abi-compatibility.md` verbatim as the initial snapshot, with
      a note to re-sync at each kernel milestone close (per `CLAUDE.md`'s
      existing rule).

- [ ] **Step 7: Verify the site builds**

```bash
npm install && npm run build
```

- [ ] **Step 8: Commit and push**

```bash
git add -A
git commit -m "docs: getting-started, porting guide, API reference, port catalog, ABI snapshot"
git push origin main
```

---

### Task 10: Deploy `neoos-docs` — resolve the domain mismatch first

**The spec assumes `neoos.github.io`, but the actual org login is
`NeoOSOrganization`, so GitHub Pages for an org-owned repo serves at
`neoosorganization.github.io` by default — not `neoos.github.io`.**
This is a real naming conflict the spec didn't anticipate (it was
written assuming the org would be named `NeoOS`, but that login was
taken/unavailable, per the earlier discovery that `orgs/NeoOS` 404s).
Resolve with the user before deploying:

- [ ] **Step 1: Ask the user which they want:**
  - accept `neoosorganization.github.io` and update every "neoos.github.io"
    reference in `CLAUDE.md` and the spec, or
  - buy/point a custom domain (e.g. `neoos.dev`) at GitHub Pages via a
    `CNAME` file, or
  - rename the org (disruptive — every remote URL in every repo and
    this monorepo's `neoos-kernel` remote would need updating)

- [ ] **Step 2: Add the GitHub Pages deploy workflow (adjust the base URL per the decision above)**

```yaml
name: deploy-docs
on:
  push:
    branches: [main]
jobs:
  deploy:
    runs-on: ubuntu-latest
    permissions: { contents: read, pages: write, id-token: write }
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-node@v4
        with: { node-version: 20 }
      - run: npm install && npm run build
      - uses: actions/upload-pages-artifact@v3
        with: { path: build }
      - uses: actions/deploy-pages@v4
```

- [ ] **Step 3: Enable Pages on the repo**

```bash
gh api -X POST repos/NeoOSOrganization/neoos-docs/pages -f "build_type=workflow"
```

- [ ] **Step 4: Verify**

```bash
gh api repos/NeoOSOrganization/neoos-docs/pages | python3 -c "import json,sys; print(json.load(sys.stdin)['html_url'])"
curl -sI "$(gh api repos/NeoOSOrganization/neoos-docs/pages --jq .html_url)" | head -1
```

Expected: `HTTP/2 200`.

---

### Task 11: Dual-push workflow for ongoing kernel development

The user asked to start dual-pushing kernel commits to `neoos-kernel`
now, even before the rest of the migration finishes. Pushing full
monorepo history (including `ports/`, `third_party/musl`,
`docs/superpowers/plans/*`) into `neoos-kernel` would violate its "does
NOT contain ports/musl" contract, so this needs a filtered push, not a
mirror.

**Files:**
- Create: `tools/push-kernel-subtree.sh` (this monorepo)

- [ ] **Step 1: Write the filtered push script**

```bash
#!/bin/bash
# Pushes kernel-relevant paths from this monorepo's main onto
# neoos-kernel's main, as a subtree split. Run after every commit (or
# batch of commits) that touches kernel/, lib/, boot/, third_party/shim,
# userland/, tools/, shared/, docs/stdlib.md, docs/abi-compatibility.md.
set -euo pipefail
PATHS="kernel lib boot third_party/shim userland tools shared \
       docs/stdlib.md docs/abi-compatibility.md Makefile CMakeLists.txt \
       Toolchain-x86_64-elf.cmake linker.ld toolchain README.md"

SPLIT_BRANCH=$(git subtree split --prefix=. -- $PATHS 2>/dev/null || true)
# git subtree split doesn't support multiple non-contiguous prefixes
# directly; use git filter-repo instead for a multi-path filter.
git filter-repo --target /tmp/neoos-kernel-mirror --force \
  $(for p in $PATHS; do echo "--path $p"; done) \
  --refs main
cd /tmp/neoos-kernel-mirror
git remote add kernel-org git@github.com:NeoOSOrganization/neoos-kernel.git 2>/dev/null || true
git push kernel-org main:main --force-with-lease
```

Note: `git filter-repo` rewrites history on every run (it's not
incremental), so this is push-everything-again each time, using
`--force-with-lease` for safety rather than plain `--force`. For a
low-frequency push cadence (a few times a week) this is fine; if it
becomes a bottleneck, switch to `git subtree push` with a single
combined prefix directory instead (would require physically
restructuring this monorepo so kernel-relevant paths live under one
directory, a bigger and separate decision — don't do it as a side
effect of this task).

- [ ] **Step 2: Confirm `git-filter-repo` is available, install if not**

```bash
git filter-repo --version || pip install --user git-filter-repo
```

- [ ] **Step 3: Dry-run against a throwaway clone first, never this working copy**

```bash
cp -r /home/neo/projects/personal/NeoOS /tmp/neoos-dry-run
cd /tmp/neoos-dry-run
rm -rf .git && git init && git add -A && git commit -q -m snapshot
# (adjust the script's source-repo assumption for this dry run, or
#  just eyeball `git filter-repo --analyze` output first)
```

- [ ] **Step 4: Run for real, verify the pushed repo still builds**

```bash
chmod +x tools/push-kernel-subtree.sh
./tools/push-kernel-subtree.sh
git clone git@github.com:NeoOSOrganization/neoos-kernel.git /tmp/verify-push
cd /tmp/verify-push && make MUSL_DIR=/tmp/neoos-musl-work/build-output test
```

- [ ] **Step 5: Document the workflow in CLAUDE.md**

Add one paragraph under the restructuring section: kernel-relevant
commits get pushed to `neoos-kernel` via
`tools/push-kernel-subtree.sh` after landing on this repo's `main`;
this monorepo stays authoritative until Task 13's cutover.

- [ ] **Step 6: Commit**

```bash
git add tools/push-kernel-subtree.sh CLAUDE.md
git commit -m "tools: filtered push of kernel-relevant paths to neoos-kernel, for dual-push during migration"
```

---

### Task 12: Final cross-repo consistency pass

**Files:** `README.md` in each of the 6 org repos.

- [ ] **Step 1: Add a "Related repositories" section to each repo's README**, cross-linking the other five, matching the list already in this monorepo's `CLAUDE.md`.

- [ ] **Step 2: Re-run `tools/gauntlet.sh` on this monorepo one more time** to confirm nothing in Tasks 0–11 touched local build behavior.

- [ ] **Step 3: Check off spec §8's success criteria one by one, with the command that proves each:**

| Criterion | Verification command | Task |
|---|---|---|
| `neoos-musl` builds standalone | `make KERNEL_SHIM_DIR=../neoos-kernel/third_party/shim` in a fresh clone | 1 |
| `neoos-kernel` `make test` passes with musl cloned nearby | Task 2 Step 6 | 2 |
| Ports build independently against musl | Task 4 Step 4, Task 5 Step 3 | 4, 5 |
| `neoos-os-builder` builds a custom image | Task 7 Step 5 | 7 |
| Output ISO boots with selected ports | Task 7 Step 5 (`grep PGAUNTLET PASSED`) | 7 |
| Docs site live | Task 10 Step 4 | 10 |
| No disruption to kernel dev | this monorepo's `main` untouched (Task 3) | 3 |

- [ ] **Step 4: Do NOT execute spec §5 Phase 6 "Cutover" (archiving the monorepo) as part of this plan.** The user chose to keep this monorepo primary for now (dual-push only). Cutover is a separate, later decision — leave `docs/superpowers/specs/2026-09-04-github-organization-design.md` §5 Phase 6 as a future plan, not something this plan's completion implies.

---

## Notes for whoever executes this

- Every "local clone" step above uses `/tmp/neoos-*-work` paths — these
  are scratch clones, not this monorepo. Nothing in Tasks 1–2, 4–5,
  7–10 touches `/home/neo/projects/personal/NeoOS` except as a source
  to copy *from* (submodule SHAs, shim files, config files, docs
  content).
- Tasks are ordered by dependency (musl → kernel → ports → os-builder →
  docs), but Task 11 (dual-push) and Task 0 (correcting the record) can
  run any time — Task 0 should probably run first since it's cheap and
  prevents anyone else trusting the false completion docs in the
  meantime.
- Every push to a public org repo is a visible, hard-to-fully-reverse
  action. Confirm before force-pushing (Task 11) or flipping repo
  visibility (Task 5 Step 5).

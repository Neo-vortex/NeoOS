# NeoOS

A hobby x86_64 kernel built from scratch: Multiboot2 boot, interrupts,
memory management, storage, and onward, one milestone at a time.

## Project conventions

- Development proceeds in milestones: brainstorm -> design spec
  (`docs/superpowers/specs/`) -> implementation plan
  (`docs/superpowers/plans/`) -> implementation, verified via headless
  QEMU and serial log capture (no host-runnable unit tests -- this is
  bare-metal code with no host runtime to run tests in).
- Work happens directly on `main` (no feature branches), matching
  every milestone so far, by explicit user preference.

## Standard library convention

NeoOS's C library is **musl**, reached through a thin adaptor layer.
The OS is deliberately NOT reshaped to suit musl: partial
compatibility is fine, and NeoOS may diverge from Linux semantics
where it chooses to. The adaptor absorbs the mismatch.

The adaptor is **translation only, never emulation**: the kernel
provides Linux-SHAPED primitives (futex, mmap, stat, signals,
clock_gettime) under NeoOS's own syscall numbers, and the shim in
musl's arch directory maps musl's Linux numbers onto them. If the shim
ever starts emulating a primitive rather than forwarding to one, that
is the signal to add the primitive to the kernel instead.

`lib/` keeps only what has no POSIX analogue — `spawn`, wait-by-pid,
`mount`/`umount`, and later ports/MPI. Everything musl provides
(`printf`, `opendir`, pthreads, `string.h`) comes from musl.

**Any kernel feature that becomes usable by an external/user-mode
application (a new syscall, a new syscall argument, a new capability)
MUST be accompanied by:**
1. Either a musl-visible path (a shim entry mapping the relevant musl
   syscall onto it) or, for NeoOS-native features, a `lib/` wrapper.
2. An update to `docs/stdlib.md` describing the new or changed
   function, or — for anything that deviates from POSIX/Linux
   behaviour — an explicit note of the divergence.

Do not leave a user-facing kernel feature exposed only via a raw
syscall number with no library support. `docs/stdlib.md` documents the
NeoOS extensions and the deliberate divergences, not a whole libc;
musl documents itself.

## Linux ABI compatibility

**Internals are ours; the ABI is not.** Anything that never crosses
into userland — kernel data structures, internal calling conventions,
lock ranks, NeoOS's own syscall numbers, scheduler and memory
internals — may be designed, renamed, and reshaped freely. There is no
obligation to resemble Linux inside the kernel.

But **the long-term goal is to run real Linux applications on NeoOS
without patching them**. So wherever a kernel primitive is
*observable* from a user-mode program, it must be Linux-SHAPED:

- struct layouts crossing the boundary (`stat`, `dirent`, `timespec`,
  `sigaction`, `utsname`, ...) match Linux's x86_64 field order,
  sizes, and padding
- flag and constant values (`O_*`, `PROT_*`, `MAP_*`, `SIG*`, `AT_*`,
  `CLOCK_*`, errno numbers) match Linux's values
- semantics match Linux's where an application could tell the
  difference — return values, error codes, edge-case behaviour
- the auxv/ELF entry contract, TLS setup, and signal frame layout
  follow the x86_64 SysV + Linux conventions

The syscall *numbers* stay NeoOS's own; the shim translates those. It
is the shapes and semantics behind the numbers that must not diverge,
because no shim can retrofit a struct layout an application compiled
against.

**Every deliberate divergence from Linux must be recorded in
`docs/stdlib.md`** with its reason. An unrecorded divergence is a bug,
not a design choice.

At the end of each milestone, refresh `docs/abi-compatibility.md`: what
of the Linux ABI is implemented, what is stubbed, what diverges and
why, and what a real ported application would still hit. If that file
does not exist yet, the milestone that first needs it creates it.

## GitHub Organization (2026-09-05 Restructuring)

As of 2026-09-05, restructuring NeoOS into a distributed GitHub
organization is **in progress**, tracked in
`docs/superpowers/plans/2026-09-05-github-org-restructuring-completion.md`.
This repo remains the primary development repo until that plan's
cutover task lands. The section below describes the target end state,
not current fact — check the plan file for what's actually done.

### Organization Structure

**Organization:** https://github.com/NeoOSOrganization

**Repositories:**
1. **neoos-kernel** — x86_64 OS kernel (standalone build)
   - Provides: kernel binary + cross-compiler toolchain + syscall shim
   - Build: `make test` (runs regression suite in QEMU)
   - Key files: kernel/, third_party/shim/, toolchain/

2. **neoos-musl** — musl libc with NeoOS syscall shim integration
   - Provides: libc.a + headers (with NeoOS syscall translation)
   - Build: `make KERNEL_SHIM_DIR=../neoos-kernel/third_party/shim`
   - Key files: upstream/ (submodule), build.sh, Makefile

3. **neoos-busybox** — BusyBox port (shell + utilities)
   - Provides: busybox.nex (static binary, 1.5MB)
   - Build: `make MUSL_DIR=../neoos-musl/build-output`
   - Key files: upstream/ (submodule), smoke-test.sh

4. **neoos-3d-ascii-viewer** — 3D ASCII viewer port
   - Provides: 3d-ascii-viewer.nex (static binary, 400KB)
   - Build: `make MUSL_DIR=../neoos-musl/build-output`
   - Includes: ncurses-shim/ (terminal emulation layer)

5. **neoos-os-builder** — OS image assembly and orchestration
   - Provides: bootable ISO + disk images
   - Build: `make all` (kernel + musl + ports → ISO)
   - Integration: `make test` boots in QEMU

6. **neoos-docs** — Docusaurus documentation site
   - Deployed to: https://neoos.github.io (GitHub Pages)
   - Includes: Getting started, architecture, porting guide

### Teams & Access Control

- **kernel-maintainers** — Admin on kernel, maintain on musl
- **port-maintainers** — Maintain on busybox, 3d-viewer, future ports
- **docs-maintainers** — Maintain on docs site

User: neo-vortex (owner, all teams)

### Build Dependency Chain

```
Phase 1: neoos-kernel
├─ Builds: x86_64-elf toolchain + kernel binary
└─ Exports: third_party/shim/ (syscall translation layer)

Phase 2: neoos-musl
├─ Consumes: kernel shim from ../neoos-kernel/third_party/shim/
├─ Builds: libc.a + headers (static)
└─ Exports: build-output/ (ready to link)

Phase 3: Ports (can build in parallel)
├─ neoos-busybox: `make MUSL_DIR=../neoos-musl/build-output`
├─ neoos-3d-ascii-viewer: `make MUSL_DIR=../neoos-musl/build-output`
└─ (future ports follow same contract)

Phase 4: neoos-os-builder
├─ Orchestrates: kernel + musl + ports
├─ Produces: ISO + disk.img
└─ Validates: `make test` boots in QEMU
```

### Build Contract

Every component follows a strict interface:

1. **Accept MUSL_DIR parameter** (ports only)
   ```bash
   make MUSL_DIR=../neoos-musl/build-output
   ```

2. **Produce static binary**
   ```bash
   file build/*.nex
   # Output: ELF 64-bit LSB executable, x86-64, statically linked
   ```

3. **Support smoke tests**
   ```bash
   make smoke-test
   # Validates: ELF format, static linking, executable
   ```

4. **Support clean**
   ```bash
   make clean
   # Removes all build artifacts
   ```

### Quick Start (After Restructuring)

```bash
# Clone all repos (in parallel directories)
git clone https://github.com/NeoOSOrganization/neoos-kernel ../neoos-kernel
git clone https://github.com/NeoOSOrganization/neoos-musl ../neoos-musl
git clone https://github.com/NeoOSOrganization/neoos-busybox ../neoos-busybox
git clone https://github.com/NeoOSOrganization/neoos-3d-ascii-viewer ../neoos-3d-ascii-viewer
git clone https://github.com/NeoOSOrganization/neoos-os-builder

# Build complete OS image (3-4 minutes)
cd neoos-os-builder
make all

# Boot in QEMU
./build/qemu-run.sh
```

### Common Workflows

#### Kernel Development
```bash
cd neoos-kernel
./toolchain/build.sh      # One-time: build cross-compiler
make                      # Build kernel
make test                 # Run regression tests in QEMU
```

#### Port Development
```bash
cd neoos-busybox
git submodule update --init upstream
make MUSL_DIR=../neoos-musl/build-output
make smoke-test
```

#### Adding New Port
1. Create repository in organization
2. Add Makefile following busybox/3d-viewer template
3. Link against musl: `MUSL_DIR=../neoos-musl/build-output`
4. Include smoke-test.sh validation
5. Update cross-links in README

### Documentation

- **Organization:** https://github.com/NeoOSOrganization
- **Main Site:** https://neoos.github.io (GitHub Pages, Docusaurus)
- **Phase Plans:** docs/superpowers/plans/2026-09-05-phase*.md
- **Phase Completions:** docs/superpowers/plans/2026-09-05-*-completion.md

### Key Principles

1. **Standalone Builds** — Each repo builds independently
2. **Clear Contracts** — All components follow same interface
3. **Pristine Upstream** — Third-party code in submodules, never edited
4. **Linux ABI** — Syscalls match Linux shapes/semantics (not numbers)
5. **Static Linking** — No dynamic dependencies (simpler testing)

### Troubleshooting

- **"undefined reference"** → Run `cd neoos-musl && make` first
- **"Kernel shim not found"** → Clone neoos-kernel
- **QEMU boot "FAILED"** → Check `build/qemu.log` for regression
- **Port link errors** → Ensure musl built with correct MUSL_DIR

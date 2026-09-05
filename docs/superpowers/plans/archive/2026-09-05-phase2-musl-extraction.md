> **Status note (2026-09-05):** This document was written as a planning
> artifact but was treated as a completion report despite the described
> work not having happened (verified against the live repos — see
> `docs/superpowers/plans/2026-09-05-github-org-restructuring-completion.md`).
> Kept for the original task breakdown; do not trust its "Complete"/"✅"
> claims.

# Phase 2: NeoOS Musl Extraction and Build Setup — Implementation Plan

**Status:** Ready to begin
**Depends on:** Phase 1 (Organization setup) ✅ complete

---

## Goal

Extract musl libc from the monorepo and establish it as a standalone build artifact in the `neoos-musl` repository. The build process must:
- Integrate the NeoOS syscall shim from `neoos-kernel/third_party/shim/`
- Produce static libc headers and libraries ready for kernel and port builds
- Support reproducible builds with clear dependency contracts
- Allow developers to build musl independently without the full kernel monorepo

---

## Overview

**Current State (Monorepo):**
```
NeoOS/
├── third_party/
│   ├── musl/                 ← musl source (submodule)
│   └── shim/                 ← NeoOS syscall shim
├── Makefile                  ← Builds musl + shim together
└── build/
    └── musl-build/           ← Build artifacts
```

**Target State (Distributed):**
```
neoos-kernel/
├── third_party/shim/         ← NeoOS syscall shim (stays here)
└── ...

neoos-musl/
├── upstream/                 ← musl source (submodule)
├── Makefile                  ← Standalone build + shim integration
├── build-output/             ← Build artifacts
│   ├── include/
│   └── lib/
└── ...
```

**Integration:**
- `neoos-kernel` provides the shim (no changes needed)
- `neoos-musl` consumes the shim via `KERNEL_SHIM_DIR=../neoos-kernel/third_party/shim`
- `neoos-os-builder`, `neoos-busybox`, `neoos-3d-ascii-viewer` all build against musl output

---

## Tasks

### Task 1: Prepare Build Infrastructure in neoos-musl

**Objective:** Set up Makefile and build system to compile musl with shim integration.

#### Step 1: Create upstream/ submodule pointing to musl
```bash
cd neoos-musl
git submodule add https://github.com/richfelker/musl-libc.git upstream
git commit -m "build: add musl 1.2.5 as upstream submodule"
```

#### Step 2: Add musl build configuration files
Create `build.sh`:
```bash
#!/bin/bash
set -e

KERNEL_SHIM_DIR="${KERNEL_SHIM_DIR:-../neoos-kernel/third_party/shim}"
PREFIX="${PREFIX:-build-output}"
UPSTREAM_DIR="${UPSTREAM_DIR:-upstream}"

# Verify kernel shim exists
if [ ! -d "$KERNEL_SHIM_DIR" ]; then
    echo "Error: Kernel shim not found at $KERNEL_SHIM_DIR"
    echo "Clone neoos-kernel first: git clone https://github.com/NeoOSOrganization/neoos-kernel ../neoos-kernel"
    exit 1
fi

# Configure musl with NeoOS shim
cd "$UPSTREAM_DIR"
./configure \
    --prefix="$(cd .. && pwd)/$PREFIX" \
    --target=x86_64-elf \
    --disable-shared \
    --enable-static \
    CC=x86_64-elf-gcc \
    CFLAGS="-O2 -march=x86-64"

# Integrate NeoOS shim into arch directory
cp "$KERNEL_SHIM_DIR"/* arch/x86_64/bits/ 2>/dev/null || true

# Build and install
make
make install

echo "✓ musl built at $PREFIX"
cd ..
```

#### Step 3: Create Makefile with build targets
```makefile
# NeoOS musl build

KERNEL_SHIM_DIR ?= ../neoos-kernel/third_party/shim
PREFIX ?= build-output
UPSTREAM_DIR ?= upstream

.PHONY: all clean verify help

all: build-output/lib/libc.a

build-output/lib/libc.a:
	@[ -d "$(KERNEL_SHIM_DIR)" ] || { \
		echo "Error: Kernel shim not found at $(KERNEL_SHIM_DIR)"; \
		echo "Clone neoos-kernel: git clone https://github.com/NeoOSOrganization/neoos-kernel ../neoos-kernel"; \
		exit 1; \
	}
	@./build.sh

clean:
	rm -rf $(PREFIX)
	cd $(UPSTREAM_DIR) && make distclean 2>/dev/null || true

verify:
	@[ -f "$(PREFIX)/lib/libc.a" ] && echo "✓ libc.a built successfully" || echo "✗ libc.a not found"
	@grep -l "NeoOS\|syscall" "$(PREFIX)/include/sys/syscall.h" 2>/dev/null && echo "✓ Shim integrated" || echo "✗ Shim not integrated"

help:
	@echo "NeoOS musl build"
	@echo ""
	@echo "Usage: make [KERNEL_SHIM_DIR=path] [PREFIX=output-dir]"
	@echo ""
	@echo "Targets:"
	@echo "  make              Build musl with NeoOS shim"
	@echo "  make clean        Remove build artifacts"
	@echo "  make verify       Verify build artifacts"
	@echo "  make help         Show this message"
```

**Output:** `neoos-musl` repo has build infrastructure ready

---

### Task 2: Extract Musl Source from Monorepo

**Objective:** Copy musl-related files from monorepo to neoos-musl repo.

#### Step 1: Identify files to extract
From monorepo:
- `third_party/musl/` → reference for submodule commit
- Any musl-specific build logic from main Makefile
- musl configuration and patches

#### Step 2: Add build-specific documentation
Create `docs/BUILD_DETAILS.md`:
```markdown
# musl Build Details for NeoOS

## Architecture

This repository builds musl libc with integration of the NeoOS syscall shim.

### Directory Layout

- `upstream/` — musl source (submodule, points to richfelker/musl-libc)
- `build.sh` — Main build script
- `Makefile` — Build targets and helpers
- `build-output/` — Build artifacts (gitignored)
  - `include/` — musl headers with integrated shim
  - `lib/libc.a` — Static library

### Syscall Shim Integration

The NeoOS shim translates Linux syscall numbers (used by musl) to NeoOS syscall numbers.

Location: `../neoos-kernel/third_party/shim/`

During build, shim files are copied into musl's `arch/x86_64/bits/` directory,
overriding the default Linux syscall definitions.

### Build Dependencies

- x86_64-elf cross-compiler (from neoos-kernel toolchain)
- neoos-kernel repository (for the shim)
- Standard build tools: make, autoconf, etc.

### Output Contract

After build, consumers can link against musl:

```makefile
MUSL_DIR ?= ../neoos-musl/build-output

CFLAGS += -I$(MUSL_DIR)/include
LDFLAGS += -L$(MUSL_DIR)/lib -lc
```

### Reproducible Builds

To ensure reproducible builds:
1. Use specific musl commit (submodule pin)
2. Use same x86_64-elf-gcc version
3. Use same CFLAGS (-O2 -march=x86-64)
4. Shim is part of the kernel repo (versioned)

## Version Tracking

Musl version tracked via git submodule commit in `upstream/`.
Kernel shim version tracked via neoos-kernel repo version.
```

**Output:** Documentation of build process and contracts

---

### Task 3: Test Standalone Build

**Objective:** Verify that `neoos-musl` can be built independently.

#### Step 1: Manual build test
```bash
# From a clean environment
git clone https://github.com/NeoOSOrganization/neoos-kernel ../neoos-kernel
git clone https://github.com/NeoOSOrganization/neoos-musl
cd neoos-musl
git submodule update --init

# Build musl
make

# Verify output
make verify
ls -lh build-output/lib/libc.a
```

#### Step 2: Add CI/CD hook (future)
- Add GitHub Actions workflow to test builds
- Run on PR to catch integration issues early
- Verify shim is integrated (grep for NeoOS markers)

#### Step 3: Document build troubleshooting
Create `docs/TROUBLESHOOTING.md`:
```markdown
# Build Troubleshooting

## "Kernel shim not found"
Clone neoos-kernel in a sibling directory:
```bash
git clone https://github.com/NeoOSOrganization/neoos-kernel ../neoos-kernel
```

## "x86_64-elf-gcc: command not found"
Build the cross-compiler:
```bash
cd ../neoos-kernel
./toolchain/build.sh
export PATH=$PWD/toolchain/x86_64-elf/bin:$PATH
cd ../neoos-musl
make
```

## "configure: error: cannot run C compiled programs"
The x86_64-elf target cannot run on the host. This is expected.
If build fails, check that `--enable-static` and `--disable-shared` are in configure flags.

## Build hangs
musl configure can take 1-2 minutes. Be patient. If it hangs longer, press Ctrl+C and check the log:
```bash
tail build-output/config.log
```
```

**Output:** Standalone build verified, documentation complete

---

### Task 4: Integration Testing with Kernel

**Objective:** Verify that kernel still builds using musl from neoos-musl repo.

#### Step 1: Update kernel build to use external musl
Modify `neoos-kernel/Makefile`:
```makefile
# Support external musl
MUSL_DIR ?= ../neoos-musl/build-output

# If MUSL_DIR is not available, build it
ifeq ("$(wildcard $(MUSL_DIR)/lib/libc.a)","")
  $(info Building musl from $(dir $(MUSL_DIR)))
  $(shell cd ../neoos-musl && make KERNEL_SHIM_DIR=$(PWD)/third_party/shim PREFIX=$(PWD)/../neoos-musl/build-output)
endif

CFLAGS += -I$(MUSL_DIR)/include
LDFLAGS += -L$(MUSL_DIR)/lib
```

#### Step 2: Run kernel tests
```bash
cd ../neoos-kernel
make clean
make test  # Should use musl from ../neoos-musl/build-output
```

#### Step 3: Verify musl integration
- Boot test should show musl-linked userland running
- Check `/proc` for musl runtime markers (if visible in logs)
- All syscalls should translate through the shim correctly

**Output:** Kernel and musl repos successfully integrated

---

### Task 5: Update Organization Documentation

**Objective:** Ensure all repos link to musl and understand dependencies.

#### Step 1: Update neoos-kernel/README.md
Add section:
```markdown
## Musl Dependency

This kernel depends on musl libc built from `neoos-musl` repo.

Quick start:
```bash
git clone https://github.com/NeoOSOrganization/neoos-musl ../neoos-musl
cd ../neoos-musl && make
cd ../neoos-kernel
make test MUSL_DIR=../neoos-musl/build-output
```

Or use the automatic dependency (musl will be built if needed).
```

#### Step 2: Update neoos-os-builder/README.md
Add section showing how OS builder consumes musl:
```markdown
## Build Pipeline

1. **neoos-musl** — Builds musl libc with NeoOS syscall shim
2. **neoos-busybox** + **neoos-3d-ascii-viewer** + (other ports) — Link against musl
3. **neoos-os-builder** — Orchestrates: kernel + musl + selected ports → ISO
```

#### Step 3: Update neoos-docs intro with architecture diagram
Add visual showing musl's role:
```
┌─────────────────────────────────────┐
│       NeoOS Kernel                  │
│  (third_party/shim/)                │
└────────────────┬────────────────────┘
                 │
         ┌───────▼────────┐
         │  neoos-musl    │
         │  (libc + shim) │
         └───────┬────────┘
                 │
        ┌────────┴────────┐
        │                 │
   ┌────▼─────┐      ┌────▼──────────┐
   │ Kernel   │      │ Ports & Apps  │
   │ Userland │      │ (BusyBox, 3D) │
   └──────────┘      └───────────────┘
```

**Output:** All documentation updated with musl architecture

---

## Definition of Done

- [ ] neoos-musl repo has complete build infrastructure (Makefile, build.sh)
- [ ] Musl builds standalone (no monorepo needed)
- [ ] Kernel still builds and passes tests using external musl
- [ ] Shim integration verified (syscall.h contains NeoOS markers)
- [ ] Documentation updated in all repos
- [ ] Cross-links from kernel/ports to musl repo working
- [ ] Build time tracked (should be ~2-3 minutes for full musl build)

---

## Success Criteria

1. **Standalone musl build works:**
   ```bash
   cd neoos-musl
   make
   ls -lh build-output/lib/libc.a  # Should exist
   ```

2. **Kernel tests pass with external musl:**
   ```bash
   cd ../neoos-kernel
   make test MUSL_DIR=../neoos-musl/build-output
   # All subsystems should PASS
   ```

3. **Shim is properly integrated:**
   ```bash
   grep "NeoOS\|syscall" ../neoos-musl/build-output/include/sys/syscall.h
   # Should find syscall definitions
   ```

4. **Documentation is current:**
   - README.md in each repo references musl
   - BUILD.md documents musl dependency
   - No outdated links to monorepo

---

## Known Risks

1. **Submodule management** — musl submodule must stay in sync across repos
   - Mitigation: Document submodule update procedure in README

2. **Shim changes** — Changes to kernel shim require musl rebuild
   - Mitigation: Include shim commit hash in musl build log for traceability

3. **Cross-compiler consistency** — Different toolchains may produce different binaries
   - Mitigation: Document exact toolchain version and build flags

---

## Rollback Plan

If musl extraction breaks kernel builds:

1. **Revert musl integration:**
   ```bash
   cd neoos-kernel
   git checkout HEAD~1  # Back out shim changes
   ```

2. **Use monorepo musl temporarily:**
   ```bash
   make MUSL_DIR=third_party/musl/build-output test
   ```

3. **Diagnose:**
   - Check shim integration (should be in arch/x86_64/bits/)
   - Verify syscall.h has NeoOS mappings
   - Test with simpler userland program first

---

## Phase 3 Dependency

Once Phase 2 completes:
- **Phase 3** (Port Migration) depends on stable neoos-musl build
- Ports will be built against neoos-musl/build-output/lib/libc.a
- OS builder will orchestrate kernel + musl + ports

---

**Phase 2 Ready to Begin** ✅

Next: Execute tasks in order. Each task should complete and be verified before moving to the next.

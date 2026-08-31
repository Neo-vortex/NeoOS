# musl as a Git Submodule

NeoOS's C library. See
`docs/superpowers/specs/2026-08-28-musl-static-design.md` for why musl
is used and how the syscall shim works.

musl is maintained as a Git submodule to keep the kernel repository
lightweight during large-scale optimization and refactoring work. The
submodule preserves musl as an explicit external dependency without
cluttering kernel-wide source searches and edits.

## Initialization

After cloning the NeoOS repository, initialize musl with:

```sh
git submodule update --init --recursive
```

Or, when cloning initially:

```sh
git clone --recurse-submodules <neoos-repo-url>
```

## Provenance

| | |
|---|---|
| Version | 1.2.5 |
| Original source | `https://musl.libc.org/releases/musl-1.2.5.tar.gz` |
| sha256 | `a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4` |
| Committed to repository | 2026-08-28 |
| Converted to submodule | 2026-08-30 |

**Integrity of the original source was checked by two independent
downloads producing the same sha256. That is not a signature
verification** — musl's release signature was not checked, because no
trusted key was available in the vendoring environment. Anyone
re-vendoring should verify against `https://musl.libc.org/` and the
maintainer's key.

## Local modifications

**The submodule is patched at build time by `third_party/shim/apply.sh`,
which is run automatically by the `$(MUSL_LIB)` rule in the Makefile.**
The real sources live in `third_party/shim/`; musl gets copies, and the
originals are kept beside them as `*.orig` so the shim can be backed
out. `apply.sh` is idempotent.

| File in musl | Replaced by | Why |
|---|---|---|
| `arch/x86_64/syscall_arch.h` | `shim/syscall_arch.h` | funnels every C-level syscall through `__neoos_syscall` |
| `src/internal/neoos_syscall.c` | `shim/neoos_syscall.c` | **new file** — the translator itself |
| `src/thread/x86_64/syscall_cp.s` | `shim/syscall_cp.s` | cancellable calls (read/write/open/...) issue `syscall` themselves |
| `src/thread/x86_64/__set_thread_area.s` | `shim/__set_thread_area.s` | `arch_prctl`, issued directly; without this musl cannot install a thread pointer and dies before `main` |
| `src/thread/x86_64/__unmapself.s` | `shim/__unmapself.s` | `munmap`+`exit`, issued directly |
| `src/thread/x86_64/clone.s` | `shim/clone.s` | NeoOS has no `clone`; Linux's number 56 is NeoOS's `lstat` |
| `src/signal/x86_64/restore.s` | `shim/restore.s` | the signal restorer's `rt_sigreturn` |
| `src/process/x86_64/vfork.s` | `shim/vfork.s` | `fork`, issued directly |

The six assembly files matter as much as the header: each issues
`syscall` ITSELF, so none of them is covered by `syscall_arch.h`.
Leaving them alone does not produce a clean failure — Linux's number
lands on whatever NeoOS call happens to share it.

**`git diff` against upstream is therefore NOT clean once a build has
run.** To restore the pristine checkout:

```sh
cd third_party/musl && git checkout -- . && git clean -fd
```

## Build

musl is built with the NeoOS cross toolchain, in the same code model
NeoOS user programs use. `-mcmodel=large` is **not optional**: programs
link at `0x200000000000` because `userland/user.ld` avoids the low
4GiB, which is `PML4[0]` — the kernel's identity map, shared by every
process.

```sh
cd third_party/musl
./configure --target=x86_64 --disable-shared \
  CC=$HOME/opt/cross-x86_64-elf/bin/x86_64-elf-gcc \
  AR=$HOME/opt/cross-x86_64-elf/bin/x86_64-elf-ar \
  RANLIB=$HOME/opt/cross-x86_64-elf/bin/x86_64-elf-ranlib \
  CFLAGS="-mcmodel=large -fno-pic -mno-red-zone -O2"
make -j"$(nproc)"
```

`AR` and `RANLIB` must be given explicitly: configure derives them from
`--target`, yielding `x86_64-ar`, which does not exist here — the
toolchain is `x86_64-elf-*`.

Verified 2026-08-28: builds clean, `lib/libc.a` with 1644 defined text
symbols. The relocation profile confirms the code model — 8710
`R_X86_64_64` against 2 `R_X86_64_PC32` and 8 `R_X86_64_PLT32`, the
latter in hand-written assembly where they stay local.

Build outputs are ignored by musl's own `.gitignore` (`*.o`, `*.a`,
`config.mak`, `/obj/`), so only source is committed.

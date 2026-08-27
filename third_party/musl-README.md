# Vendored musl

NeoOS's C library. See
`docs/superpowers/specs/2026-08-28-musl-static-design.md` for why musl
is used and how the syscall shim works.

## Provenance

| | |
|---|---|
| Version | 1.2.5 |
| Upstream | `https://musl.libc.org/releases/musl-1.2.5.tar.gz` |
| sha256 | `a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4` |
| Vendored | 2026-08-28 |

**Integrity was checked by two independent downloads producing the same
sha256. That is not a signature verification** — musl's release
signature was not checked, because no trusted key was available in this
environment. Anyone re-vendoring should verify against
`https://musl.libc.org/` and the maintainer's key.

## Local modifications

None yet. The syscall-number remap is added in a later task and will
be listed here, kept as `third_party/neoos-syscall.patch` rather than
applied silently, so `git diff` against pristine upstream stays
meaningful.

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

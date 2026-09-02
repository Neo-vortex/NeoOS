#!/bin/bash
# Configures the BusyBox submodule for NeoOS.
#
# Mirrors third_party/shim/apply.sh: the submodule stays a pristine
# upstream checkout, and everything NeoOS-specific lives here.
#
# allnoconfig, then the fragment over the top, then `oldconfig` to
# resolve whatever the fragment made newly reachable.
#
# ---------------------------------------------------------------------
# Why this script is defensive, in three parts, each a real failure:
#
# 1. The resolve step used to be `silentoldconfig`, which REFUSES to
#    choose when input is redirected rather than taking the default:
#
#        Maximum screen width (FEATURE_VI_MAX_LEN) [4096] (NEW) aborted!
#        Console input/output is redirected. Run 'make oldconfig'
#
#    It worked until the fragment grew enough to make a numeric symbol
#    newly reachable -- adding CONFIG_VI did it. `oldconfig` is the
#    command that message names, and it answers blank input with each
#    symbol's default.
#
# 2. When that aborted, `set -e` left a HALF-WRITTEN .config behind, and
#    busybox's own make then completed it by taking defaults for
#    everything unresolved -- pulling in udhcpc, which needs
#    <linux/filter.h> and cannot build here. That is why `make shell`
#    failed intermittently and with an error nowhere near the cause. The
#    config is built on a copy now and installed only on success; any
#    failure removes it entirely, so the next run starts clean rather
#    than inheriting a broken one.
#
# 3. A fragment key that is not an upstream symbol is IGNORED by kconfig
#    with a warning lost in the build output. Renames had silently
#    stopped taking effect that way. They now fail the build.
#
# Two alternatives were tried and rejected, both worth recording:
#
#   - `yes n | make oldconfig` HANGS. `n` is not a valid answer for a
#     numeric symbol, so kconfig asks the same question forever.
#   - `make allnoconfig KCONFIG_ALLCONFIG=<file>` is exactly the right
#     mechanism on newer kconfigs, but not on this one:
#     scripts/kconfig/conf.c reads the file and then runs its
#     set-everything-to-no pass TWICE, deliberately, to defeat `select`
#     -- so the file's values are wiped and CONFIG_ASH comes out unset.
# ---------------------------------------------------------------------
set -e

here="$(cd "$(dirname "$0")" && pwd)"
bb="$here/../busybox"
musl="$here/../musl"

[ -f "$bb/Makefile" ] || { echo "apply.sh: $bb is not a busybox checkout" >&2; exit 1; }

# Any failure past this point leaves NO .config, rather than a partial
# one that the next build would silently complete with upstream
# defaults. Losing the file costs one reconfigure; keeping a broken one
# costs an afternoon.
cleanup() {
    status=$?
    if [ $status -ne 0 ]; then
        rm -f "$bb/.config"
        echo "apply.sh: failed; removed .config so the next run starts clean" >&2
    fi
    exit $status
}
trap cleanup EXIT

cflags="-nostdinc -isystem $musl/include -isystem $musl/arch/x86_64"
cflags="$cflags -isystem $musl/arch/generic -isystem $musl/obj/include"
cflags="$cflags -mcmodel=large -fno-pic -mno-red-zone -fno-stack-protector"
cflags="$cflags -ftls-model=local-exec -Wno-error"

# Everything that must reach ONLY the final link is written as a -Wl,
# option, deliberately. busybox's ld_flags is
#     $(filter-out -Wl$(comma)%, $(LDFLAGS) $(EXTRA_LDFLAGS))
# so EXTRA_LDFLAGS reaches every `ld -r` partial link too -- and a
# partial link handed the linker script and crt1.o produces a fully
# LINKED applets/built-in.o carrying its own _start, colliding with
# crt1.o's at the real link.
#
# crt1.o has to be named at all because busybox's own link never adds a
# startup file: with -nostdlib and no crt1, nothing defines _start,
# --gc-sections finds nothing reachable, and the link "succeeds" with a
# 496-byte ELF that has no program headers.
ldflags="-nostdlib -static -Wl,-z,noexecstack"
ldflags="$ldflags -Wl,-T,$here/../../userland/user.ld"
ldflags="$ldflags -Wl,$musl/lib/crt1.o -L$musl/lib"

cd "$bb"
make allnoconfig >/dev/null
python3 "$here/setconfig.py" apply .config "$here/neoos.fragment" "$cflags" "$ldflags"
yes "" | make oldconfig >/dev/null 2>&1

# The set of symbols upstream actually defines, for the check below.
# Config.in files are generated from Config.src, so this runs after
# allnoconfig rather than before it.
grep -rhoE "^config [A-Za-z0-9_]+" Config.in */Config.in */*/Config.in 2>/dev/null \
    | awk '{print $2}' | sort -u > .neoos-symbols

python3 "$here/setconfig.py" verify .config "$here/neoos.fragment" \
    .neoos-symbols "$here/neoos.unbuildable"

# The handful this port cannot do without. Checked by name so that a
# dependency change upstream fails here, loudly, rather than as a
# missing applet noticed days later.
for want in CONFIG_ASH CONFIG_SH_IS_ASH CONFIG_STATIC CONFIG_LS CONFIG_CAT; do
    grep -q "^$want=y" .config || {
        echo "apply.sh: $want did not survive configuration" >&2
        exit 1
    }
done

echo "busybox-config: .config written ($(grep -c '^CONFIG_.*=y' .config) options on)"

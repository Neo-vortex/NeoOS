#!/bin/bash
# Configures the BusyBox submodule for NeoOS.
#
# Mirrors third_party/shim/apply.sh: the submodule stays a pristine
# upstream checkout, and everything NeoOS-specific lives here.
#
# allnoconfig + fragment + olddefconfig, rather than a checked-in
# .config, so a BusyBox version bump takes upstream's default for any
# option the fragment does not name instead of silently dropping it.
set -e

here="$(cd "$(dirname "$0")" && pwd)"
bb="$here/../busybox"
musl="$here/../musl"

[ -f "$bb/Makefile" ] || { echo "apply.sh: $bb is not a busybox checkout" >&2; exit 1; }

cflags="-nostdinc -isystem $musl/include -isystem $musl/arch/x86_64"
cflags="$cflags -isystem $musl/arch/generic -isystem $musl/obj/include"
cflags="$cflags -mcmodel=large -fno-pic -mno-red-zone -fno-stack-protector"
cflags="$cflags -ftls-model=local-exec -Wno-error"

# Linked at the userland convention base by userland/user.ld, like every
# other NeoOS program, and with musl's crt1.o rather than a host one.
#
# crt1.o has to be named here because busybox's own link (scripts/trylink)
# never adds a startup file: with -nostdlib and no crt1, the only thing
# defining _start is absent, --gc-sections then finds nothing reachable
# from the entry point, and the link "succeeds" with a 496-byte ELF that
# has no program headers at all. That is what the first attempt produced.
# Everything that must reach ONLY the final link is written as a -Wl,
# option, deliberately. busybox's ld_flags is
#     $(filter-out -Wl$(comma)%, $(LDFLAGS) $(EXTRA_LDFLAGS))
# so EXTRA_LDFLAGS reaches every `ld -r` partial link too -- and a
# partial link that is handed the linker script and crt1.o produces a
# fully LINKED applets/built-in.o carrying its own _start, which then
# collides with crt1.o's at the real link. The -Wl, form is filtered out
# of the partial links and forwarded by gcc at the final one.
ldflags="-nostdlib -static -Wl,-z,noexecstack"
ldflags="$ldflags -Wl,-T,$here/../../userland/user.ld"
ldflags="$ldflags -Wl,$musl/lib/crt1.o -L$musl/lib"

cd "$bb"
make allnoconfig >/dev/null

# Apply the fragment: each "CONFIG_X=y" or "CONFIG_X=n" replaces
# whatever allnoconfig chose.
python3 - "$here/neoos.fragment" <<'PYEOF'
import re, sys
frag = sys.argv[1]
cfg = open('.config').read()
for line in open(frag):
    line = line.strip()
    if not line or line.startswith('#'):
        continue
    key, _, val = line.partition('=')
    if val == 'n':
        repl = '# %s is not set' % key
    else:
        repl = '%s=%s' % (key, val)
    if re.search(r'^%s=' % re.escape(key), cfg, re.M):
        cfg = re.sub(r'^%s=.*$' % re.escape(key), repl.replace('\\', '\\\\'), cfg, count=1, flags=re.M)
    elif re.search(r'^# %s is not set$' % re.escape(key), cfg, re.M):
        cfg = re.sub(r'^# %s is not set$' % re.escape(key), repl.replace('\\', '\\\\'), cfg, count=1, flags=re.M)
    else:
        cfg += repl + '\n'
open('.config', 'w').write(cfg)
PYEOF

# The two settings that carry paths have to be written here rather than
# in the fragment, since they depend on where the tree lives.
python3 - "$cflags" "$ldflags" <<'PYEOF'
import re, sys
cflags, ldflags = sys.argv[1], sys.argv[2]
cfg = open('.config').read()
for key, val in (('CONFIG_EXTRA_CFLAGS', cflags), ('CONFIG_EXTRA_LDFLAGS', ldflags),
                 # -nostdlib also drops the implicit -lc/-lgcc.
                 ('CONFIG_EXTRA_LDLIBS', 'c gcc')):
    line = '%s="%s"' % (key, val)
    if re.search(r'^%s=' % key, cfg, re.M):
        cfg = re.sub(r'^%s=.*$' % key, line.replace('\\', '\\\\'), cfg, count=1, flags=re.M)
    else:
        cfg += line + '\n'
open('.config', 'w').write(cfg)
PYEOF

# BusyBox 1.37 has no olddefconfig target; silentoldconfig with an
# empty stdin takes the default for every unresolved symbol, which is
# the same thing.
yes "" | make silentoldconfig >/dev/null 2>&1
echo "busybox-config: .config written ($(grep -c '^CONFIG_.*=y' .config) options on)"

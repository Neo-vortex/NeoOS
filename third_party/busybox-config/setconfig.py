#!/usr/bin/env python3
"""Apply a kconfig fragment to busybox's .config, and check it took.

    setconfig.py apply   <.config> <fragment> <extra_cflags> <extra_ldflags>
    setconfig.py verify  <.config> <fragment> <symbol-universe> <unbuildable>

Kept out of apply.sh so the quoting stays readable: this began as two
heredocs nested inside a shell script, where every edit risked mangling
the escaping rather than the logic.
"""
import re
import sys


def setopt(cfg, key, value_line):
    """Replace key's line if present in either form, else append."""
    safe = value_line.replace('\\', '\\\\')
    if re.search(r'^%s=' % re.escape(key), cfg, re.M):
        return re.sub(r'^%s=.*$' % re.escape(key), safe, cfg, count=1, flags=re.M)
    if re.search(r'^# %s is not set$' % re.escape(key), cfg, re.M):
        return re.sub(r'^# %s is not set$' % re.escape(key), safe, cfg, count=1, flags=re.M)
    return cfg + value_line + '\n'


def fragment_items(path):
    """(key, value) for every setting line, comments and blanks skipped."""
    out = []
    for line in open(path):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        key, _, val = line.partition('=')
        out.append((key, val))
    return out


def cmd_apply(config, fragment, cflags, ldflags):
    cfg = open(config).read()
    for key, val in fragment_items(fragment):
        cfg = setopt(cfg, key,
                     ('# %s is not set' % key) if val == 'n' else '%s=%s' % (key, val))
    for key, val in (('CONFIG_EXTRA_CFLAGS', cflags),
                     ('CONFIG_EXTRA_LDFLAGS', ldflags),
                     # -nostdlib also drops the implicit -lc/-lgcc.
                     ('CONFIG_EXTRA_LDLIBS', 'c gcc')):
        cfg = setopt(cfg, key, '%s="%s"' % (key, val))
    open(config, 'w').write(cfg)


def unbuildable_reasons(path):
    """{symbol: reason} for applets that cannot compile on NeoOS."""
    out = {}
    for line in open(path):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        key, _, why = line.partition(' ')
        out[key] = why.strip()
    return out


def cmd_verify(config, fragment, universe_path, unbuildable_path):
    """Report what the fragment asked for and did not get.

    Three distinct problems, which used to be one silent one:

      - a key that is not an upstream symbol at all. kconfig prints
        "trying to assign nonexistent symbol" among hundreds of lines of
        build output and carries on, so a renamed option (ASH_BUILTIN_ECHO
        became ASH_ECHO) simply stopped taking effect.
      - a key that exists but did not survive, because something it
        depends on is off.
      - symbols that ended up ON without being asked for. Not an error:
        kconfig `select`s things, and that is its business. Reported so
        the drift is visible.
    """
    universe = set()
    for line in open(universe_path):
        universe.add('CONFIG_' + line.strip())

    on = set()
    for line in open(config):
        m = re.match(r'^(CONFIG_[A-Za-z0-9_]+)=y$', line.strip())
        if m:
            on.add(m.group(1))

    unknown, missing, asked = [], [], set()
    for key, val in fragment_items(fragment):
        if key in ('CONFIG_EXTRA_CFLAGS', 'CONFIG_EXTRA_LDFLAGS', 'CONFIG_EXTRA_LDLIBS'):
            continue
        if key not in universe:
            unknown.append(key)
            continue
        if val == 'y':
            asked.add(key)
            if key not in on:
                missing.append(key)

    extra = sorted(on - asked)

    # Applets that cannot compile here. Caught now, by name, rather than
    # as a missing <linux/*.h> three minutes into the build.
    cannot = unbuildable_reasons(unbuildable_path)
    blocked = [(k, cannot[k]) for k in sorted(on) if k in cannot]

    if blocked:
        print('busybox-config: %d enabled applets cannot build on NeoOS:' % len(blocked))
        for k, why in blocked:
            print('    %-18s needs %s' % (k, why))
        print('    Set these to n in neoos.fragment. They become possible '
              'once the network milestone lands.')
    if unknown:
        print('busybox-config: %d fragment keys are NOT upstream symbols '
              '(silently ignored by kconfig):' % len(unknown))
        for k in unknown:
            print('    %s' % k)
    if missing:
        print('busybox-config: %d requested options did not survive '
              '(a dependency is off):' % len(missing))
        for k in missing:
            print('    %s' % k)
    if extra:
        print('busybox-config: %d options are on that the fragment did not ask '
              'for (kconfig selects, and defaults for newly reachable symbols)'
              % len(extra))

    # Unknown keys and unbuildable applets FAIL: the first is always a
    # mistake, the second always a build error later. Missing and extra
    # only warn -- both can be legitimate.
    return 1 if (unknown or blocked) else 0


def main():
    what = sys.argv[1]
    if what == 'apply':
        cmd_apply(*sys.argv[2:6])
        return 0
    if what == 'verify':
        return cmd_verify(*sys.argv[2:6])
    print('setconfig.py: unknown command %s' % what, file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main())

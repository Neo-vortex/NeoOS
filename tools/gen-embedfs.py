#!/usr/bin/env python3
# tools/gen-embedfs.py <output.c> <dir> [<dir> ...]
#
# For every *.nex in each directory, embeds it via `ld -r -b binary` and
# emits one row in a generated embedfs table (kernel/fs/embedfs.h). A
# *.test.json next to a *.nex supplies its "category" ("bin", "sbin", or
# "tests") plus optional "boot_entries"/"required_markers"; absent a
# manifest, category defaults to "tests". A *.manifest.json in a
# directory is a JSON array of manifest objects, each carrying its own
# "_test" name -- used for a large bundle of tests sharing one file
# instead of one <name>.test.json per binary.
#
# Outputs, alongside <output.c>:
#   embedfs-obj/*.o         one ld -r -b binary object per embedded file
#   embedfs-objs.txt        space-separated list of those object paths
#   embedfs-inittab-patch.json   collected boot_entries, in encounter order
#   embedfs-markers.txt     collected required_markers, one per line
import json
import os
import subprocess
import sys


def load_manifest_bundle(d):
    bundle_by_test = {}
    for fname in os.listdir(d):
        if not fname.endswith(".manifest.json"):
            continue
        with open(os.path.join(d, fname)) as f:
            for entry in json.load(f):
                if "_test" in entry:
                    bundle_by_test[entry["_test"]] = entry
    return bundle_by_test


def manifest_for(d, fname, bundle):
    base = fname[:-len(".nex")]
    single = os.path.join(d, base + ".test.json")
    if os.path.exists(single):
        with open(single) as f:
            return json.load(f)
    if base in bundle:
        return bundle[base]
    return {}


def main():
    out_c = sys.argv[1]
    dirs = sys.argv[2:]
    out_dir = os.path.dirname(os.path.abspath(out_c)) or "."
    obj_dir = os.path.join(out_dir, "embedfs-obj")
    os.makedirs(obj_dir, exist_ok=True)

    entries = []       # (category, name, symbol)
    boot_entries = []
    markers = []

    for d in dirs:
        if not d or not os.path.isdir(d):
            continue
        bundle = load_manifest_bundle(d)
        for fname in sorted(os.listdir(d)):
            if not fname.endswith(".nex"):
                continue
            manifest = manifest_for(d, fname, bundle)
            category = manifest.get("category", "tests")

            abs_path = os.path.abspath(os.path.join(d, fname))
            symbol = "".join(c if c.isalnum() else "_" for c in abs_path)
            safe_obj_name = "".join(c if c.isalnum() else "_" for c in fname)
            obj_path = os.path.join(obj_dir, safe_obj_name + ".o")

            ld = os.environ.get("LD", "x86_64-elf-ld")
            subprocess.run(
                [ld, "-r", "-b", "binary", "-o", obj_path, abs_path],
                check=True,
            )

            entries.append((category, fname, symbol, obj_path))
            boot_entries.extend(manifest.get("boot_entries", []))
            markers.extend(manifest.get("required_markers", []))

    with open(out_c, "w") as f:
        f.write('#include "fs/embedfs.h"\n#include <stddef.h>\n\n')
        for _, _, symbol, _ in entries:
            f.write(f"extern char _binary_{symbol}_start[], _binary_{symbol}_end[];\n")
        f.write("\nconst struct embedfs_entry g_embedfs_table[] = {\n")
        for category, name, symbol, _ in entries:
            # Both fields are plain symbol addresses (pointer-typed
            # relocations), which GCC folds fine in a static
            # initializer -- unlike `_end - _start` cast to an integer,
            # which it rejects. The byte count is computed at runtime
            # from these two (embedfs.c).
            f.write(
                f'    {{"{category}", "{name}", _binary_{symbol}_start, _binary_{symbol}_end}},\n'
            )
        if not entries:
            f.write("    {0, 0, 0, 0},\n")
        f.write("};\n")
        f.write(f"const int g_embedfs_table_count = {len(entries)};\n")

    with open(os.path.join(out_dir, "embedfs-objs.txt"), "w") as f:
        f.write(" ".join(p for _, _, _, p in entries))

    with open(os.path.join(out_dir, "embedfs-inittab-patch.json"), "w") as f:
        json.dump(boot_entries, f)

    with open(os.path.join(out_dir, "embedfs-markers.txt"), "w") as f:
        f.write("\n".join(markers))


if __name__ == "__main__":
    main()

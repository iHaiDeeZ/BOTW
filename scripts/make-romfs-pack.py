#!/usr/bin/env python3
"""The RomFS as two files for the console: romfs.pak (every file, in path order, each at a 16-byte boundary) and
romfs.idx (one "<offset>\\t<size>\\t<path>" line per file, '/' separated, relative to the RomFS root).

The runtime's rom: mount reads them when the content root has no extract/romfs folder (runtime/src/hle_fs.cpp).
Breath of the Wild has 304,207 RomFS files: over FTP that was hours of per-file overhead, two files are not.

Usage: make-romfs-pack.py <romfs folder> <out folder>
"""
import os
import sys


def main():
    src, out = sys.argv[1], sys.argv[2]
    files = []
    for d, dirs, names in os.walk(src):
        dirs.sort()
        for n in sorted(names):
            full = os.path.join(d, n)
            files.append((os.path.relpath(full, src).replace(os.sep, "/"), full))
    files.sort()
    os.makedirs(out, exist_ok=True)
    pak_tmp, idx_tmp = os.path.join(out, "romfs.pak.part"), os.path.join(out, "romfs.idx.part")
    off = 0
    with open(pak_tmp, "wb") as pak, open(idx_tmp, "w", encoding="utf-8", newline="\n") as idx:
        for k, (rel, full) in enumerate(files):
            if "\t" in rel or "\n" in rel:
                sys.exit(f"unsupported file name: {rel!r}")
            pad = (-off) % 16
            if pad:
                pak.write(b"\0" * pad)
                off += pad
            with open(full, "rb") as f:
                data = f.read()
            pak.write(data)
            idx.write(f"{off}\t{len(data)}\t{rel}\n")
            off += len(data)
            if k % 20000 == 0:
                print(f"{k}/{len(files)} files, {off / 1e9:.2f} GB", flush=True)
    os.replace(pak_tmp, os.path.join(out, "romfs.pak"))
    os.replace(idx_tmp, os.path.join(out, "romfs.idx"))
    print(f"romfs.pak: {len(files)} files, {off / 1e9:.2f} GB")


if __name__ == "__main__":
    main()

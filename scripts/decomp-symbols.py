#!/usr/bin/env python3
"""zeldaret/botw's function list (data/uking_functions.csv: Address,Quality,Size,Name for every function of main
1.5.0, addresses at the decomp's load address 0x7100000000) as the recompiler's --symbols file: one line per function,
'<hex .text offset> <size> <name or ->'. Undecompiled functions (quality U) are listed too: their starts are still
exact, and most have a name.

Usage: python3 scripts/decomp-symbols.py <uking_functions.csv> <out file>
"""
import csv
import sys

LOAD = 0x7100000000


def main():
    src, out = sys.argv[1], sys.argv[2]
    n = named = 0
    with open(src, newline="") as f, open(out, "w") as o:
        o.write("# zeldaret/botw data/uking_functions.csv (main 1.5.0): .text offset, size, name\n")
        for row in csv.DictReader(f):
            addr = int(row["Address"], 16) - LOAD
            name = row["Name"].strip() or "-"
            o.write(f"{addr:x} {int(row['Size'])} {name}\n")
            n += 1
            named += name != "-"
    print(f"{out}: {n} functions, {named} named")


if __name__ == "__main__":
    main()

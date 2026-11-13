#!/usr/bin/env python3
"""Fail if the Apple-bus call tree reaches code that executes from flash (XIP).

Why (docs/Implementation-notes-and-reasoning.md, section 1ev): core 1 serves every
$C0C4-$C0C7 cycle and has a deadline of well under a microsecond to publish the next
PIO word. Code that runs from flash goes through the XIP cache shared with core 0
(lwIP, CYW43, std::vector ...), so a miss stalls the bus core for microseconds and the
6502 reads a stale word. Every function reachable from the bus loop must therefore
live in SRAM.

Usage:
  check_bus_ram.py ELF [--objdump PATH] [--root SYM ...] [--allow SYM ...] [-v]

Walks direct calls (bl / b.w tail calls) from each root, resolving linker veneers
through their literal target address. Indirect calls (blx reg / bx reg) are reported
as warnings only. Exit status 1 when a flash callee is reachable.
"""
import argparse
import re
import subprocess
import sys

FLASH_LO, FLASH_HI = 0x10000000, 0x20000000

# BusLoop itself is not a root: its non-Uthernet branches (MegaFlash block device, FPU,
# TFTP ...) legitimately run flash code. Only the Uthernet II cycle handlers are timing-critical.
DEFAULT_ROOTS = ["U2_HandleBusAccess", "U2_PeekDataPort"]

RE_HDR = re.compile(r"^([0-9a-f]{8}) <([^>]+)>:\s*$")
RE_INS = re.compile(r"^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$")
RE_TGT = re.compile(r"\b([0-9a-f]{6,8})\s+<([^>+]+)(?:\+0x[0-9a-f]+)?>")
RE_BRANCH = re.compile(r"^(b|bl|blx)(eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)?(\.[wn])?$")
RE_WORD = re.compile(r"^\.word\s+0x([0-9a-f]+)")


def parse(objdump, elf):
    out = subprocess.run([objdump, "-d", "--no-show-raw-insn", elf],
                         check=True, capture_output=True, text=True).stdout
    funcs = {}      # name -> dict(addr, calls[list of (name, addr)], indirect, tables)
    by_addr = {}
    cur = None
    for line in out.splitlines():
        m = RE_HDR.match(line)
        if m:
            addr = int(m.group(1), 16)
            cur = {"name": m.group(2), "addr": addr, "calls": [], "indirect": 0,
                   "tables": 0, "veneer_to": None}
            funcs[cur["name"]] = cur
            by_addr[addr] = cur
            continue
        if cur is None:
            continue
        m = RE_INS.match(line)
        if not m:
            continue
        op, rest = m.group(2), m.group(3)
        if op.startswith(".word"):
            w = RE_WORD.match((op + " " + rest).strip())
            if w and cur["name"].endswith("_veneer") and cur["veneer_to"] is None:
                cur["veneer_to"] = int(w.group(1), 16) & ~1
            continue
        if RE_BRANCH.match(op):
            t = RE_TGT.search(rest)
            if t:
                tgt_addr = int(t.group(1), 16)
                tgt_name = t.group(2)
                if tgt_name != cur["name"]:
                    cur["calls"].append((tgt_name, tgt_addr))
        elif op in ("bx", "blx") and not rest.startswith("lr"):
            cur["indirect"] += 1
        elif op in ("tbb", "tbh"):
            cur["tables"] += 1
    return funcs, by_addr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--objdump", default="arm-none-eabi-objdump")
    ap.add_argument("--root", action="append")
    ap.add_argument("--allow", action="append", default=[])
    ap.add_argument("--allow-tables", action="store_true",
                    help="downgrade jump-table branches (table in flash .rodata) to warnings")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    roots = a.root or DEFAULT_ROOTS
    funcs, by_addr = parse(a.objdump, a.elf)

    bad = {}      # flash function -> first path that reaches it
    warn = []
    tabled = []
    seen = set()

    def resolve(name, addr):
        f = funcs.get(name)
        if f and f["veneer_to"] is not None:
            tgt = by_addr.get(f["veneer_to"])
            if tgt:
                return tgt["name"], tgt["addr"]
            return name, f["veneer_to"]
        return name, addr

    def walk(name, addr, path):
        name, addr = resolve(name, addr)
        if name in seen:
            return
        seen.add(name)
        in_flash = FLASH_LO <= addr < FLASH_HI
        if in_flash and name not in a.allow:
            bad[name] = path + [name]
            return  # do not descend: it is already a violation
        f = funcs.get(name)
        if not f:
            return
        if f["indirect"] and not name.startswith("__"):
            warn.append("%s: %d indirect branch(es)" % (name, f["indirect"]))
        if f["tables"]:
            msg = "%s: %d jump-table branch(es) (table is in flash .rodata; build with -fno-jump-tables)" % (name, f["tables"])
            if a.allow_tables:
                warn.append(msg)
            else:
                tabled.append(msg)
        for cname, caddr in f["calls"]:
            walk(cname, caddr, path + [name])

    for r in roots:
        f = funcs.get(r)
        if not f:
            print("check_bus_ram: root %s not found in ELF" % r, file=sys.stderr)
            return 2
        walk(r, f["addr"], [])

    if a.verbose:
        print("check_bus_ram: %d RAM functions reachable from %s" % (len(seen) - len(bad), ", ".join(roots)))
    for w in warn:
        print("  warn: " + w)
    if tabled:
        print("check_bus_ram: FAIL - flash-resident switch tables on the bus path:")
        for t in tabled:
            print("  " + t)
        return 1
    if bad:
        print("check_bus_ram: FAIL - %d flash function(s) reachable from the bus loop:" % len(bad))
        for n, p in sorted(bad.items()):
            print("  %-40s via %s" % (n, " -> ".join(p)))
        return 1
    print("check_bus_ram: OK - bus call tree is SRAM-only (%d functions)" % len(seen))
    return 0


if __name__ == "__main__":
    sys.exit(main())

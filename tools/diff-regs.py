#!/usr/bin/env python3
"""Compare TGLDmic "PreBootRegs" from two machines/OSes.
   On each OS:  ioreg -a -r -c TGLDmic -d 1 > regs-<os>.plist
   Then:        python3 diff-regs.py regs-ventura.plist regs-tahoe.plist"""
import plistlib, struct, sys

LAYOUT = [("PCI", 0x000, 0x100), ("HDA", 0x000, 0x100), ("SPB", 0x700, 0x40), ("PP", 0x800, 0x40),
          ("ML", 0xC00, 0x40), ("VS", 0x1000, 0x100)]
NAMES = {("PCI", 0x04): "CMD/STS", ("PCI", 0x10): "BAR0", ("PCI", 0x20): "BAR4", ("PCI", 0x40): "AZCTL?",
         ("PCI", 0x44): "PGCTL/TCSEL", ("PCI", 0x48): "CGCTL", ("PCI", 0x50): "PMCAP", ("PCI", 0x54): "PMCSR",
         ("PCI", 0x78): "DEVC", ("HDA", 0x08): "GCTL", ("HDA", 0x0C): "WAKEEN/STATESTS", ("HDA", 0x14): "LLCH",
         ("HDA", 0x20): "INTCTL", ("HDA", 0x24): "INTSTS", ("HDA", 0x70): "DPLBASE", ("HDA", 0x74): "DPUBASE",
         ("PP", 0x804): "PPCTL", ("PP", 0x808): "PPSTS", ("SPB", 0x704): "SPBFCCTL",
         ("VS", 0x1000): "EM1", ("VS", 0x1004): "INRC", ("VS", 0x1008): "OUTRC", ("VS", 0x1030): "EM2",
         ("VS", 0x1038): "EM3L", ("VS", 0x103C): "EM3U", ("VS", 0x1040): "EM4L", ("VS", 0x1044): "EM4U",
         ("VS", 0x1048): "LTRC/D0I3C"}

def load(p):
    d = plistlib.load(open(p, "rb"))
    d = d[0] if isinstance(d, list) else d
    raw = d.get("PreBootRegs")
    if raw is None: sys.exit(f"{p}: no PreBootRegs (needs TGLDmic >= 0.4.10 and one boot attempt)")
    v = struct.unpack("<%dI" % (len(raw) // 4), raw)
    out, i = {}, 0
    for blk, base, ln in LAYOUT:
        for o in range(base, base + ln, 4): out[(blk, o)] = v[i]; i += 1
    return out, d.get("TGLDmicVersion", "?")

a, va = load(sys.argv[1]); b, vb = load(sys.argv[2])
print(f"{'reg':24s} {sys.argv[1][:22]:>22s} {sys.argv[2][:22]:>22s}   diff bits")
for k in a:
    if a[k] != b[k]:
        nm = NAMES.get(k, "")
        print(f"{k[0]:3s} 0x{k[1]:04x} {nm:14s} 0x{a[k]:08x}{'':12s} 0x{b[k]:08x}{'':12s}   0x{a[k] ^ b[k]:08x}")

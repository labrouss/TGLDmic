#!/usr/bin/env python3
"""Embed the SOF firmware image in the kext: gen-fw.py sof-tgl.ri out.c"""
import sys
data = open(sys.argv[1], "rb").read()
with open(sys.argv[2], "w") as f:
    f.write("// Generated from %s (%d bytes) - SOF firmware, BSD-3-Clause (thesofproject/sof-bin)\n"
            % (sys.argv[1].split('/')[-1], len(data)))
    f.write("__attribute__((aligned(16))) const unsigned char kSofTglRi[] = {\n")
    for i in range(0, len(data), 24):
        f.write(",".join("0x%02x" % b for b in data[i:i + 24]) + ",\n")
    f.write("};\nconst unsigned int kSofTglRiSize = %d;\n" % len(data))

#!/usr/bin/env python3
"""Embed SOF firmware images in the kext:
   gen-fw.py --tgl <sof-tgl.ri> --cml <sof-cml.ri> -o <out.c>
   or legacy: gen-fw.py <input.ri> <out.c>
"""
import sys, argparse

def write_array(f, var_name, data, filename):
    f.write("// Generated from %s (%d bytes) - SOF firmware, BSD-3-Clause (thesofproject/sof-bin)\n"
            % (filename.split('/')[-1], len(data)))
    f.write("__attribute__((aligned(16))) const unsigned char %s[] = {\n" % var_name)
    for i in range(0, len(data), 24):
        f.write(",".join("0x%02x" % b for b in data[i:i + 24]) + ",\n")
    f.write("};\nconst unsigned int %sSize = %d;\n\n" % (var_name, len(data)))

def main():
    if len(sys.argv) == 3 and not sys.argv[1].startswith("-"):
        # Legacy positional usage
        data = open(sys.argv[1], "rb").read()
        with open(sys.argv[2], "w") as f:
            write_array(f, "kSofTglRi", data, sys.argv[1])
            # If only 1 file provided in legacy mode, also provide kSofCmlRi as alias
            write_array(f, "kSofCmlRi", data, sys.argv[1])
        return

    parser = argparse.ArgumentParser()
    parser.add_argument("--tgl", required=False, help="Tiger Lake firmware image")
    parser.add_argument("--cml", required=False, help="Comet Lake / Cannon Lake firmware image")
    parser.add_argument("-o", "--output", required=True, help="Output C source file")
    args = parser.parse_args()

    with open(args.output, "w") as f:
        f.write("// Auto-generated SOF firmware tables for TGLDmic\n\n")
        if args.tgl:
            data = open(args.tgl, "rb").read()
            write_array(f, "kSofTglRi", data, args.tgl)
        if args.cml:
            data = open(args.cml, "rb").read()
            write_array(f, "kSofCmlRi", data, args.cml)

if __name__ == "__main__":
    main()

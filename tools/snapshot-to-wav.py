#!/usr/bin/env python3
"""Extract TGLDmic's CaptureSnapshot (1 s, 4 ch, 48 kHz, S32_LE) from ioreg into a WAV.

    ioreg -a -l -w0 -c TGLDmic > tgldmic.plist      # plist form (recommended)
    python3 snapshot-to-wav.py tgldmic.plist dmic.wav
Also accepts plain `ioreg -l -w0` text output.
"""
import sys, re, struct, wave, math, plistlib

def find(node):
    if isinstance(node, dict):
        if "CaptureSnapshot" in node:
            return node["CaptureSnapshot"]
        for v in node.values():
            r = find(v)
            if r is not None: return r
    elif isinstance(node, list):
        for v in node:
            r = find(v)
            if r is not None: return r
    return None

def find_key(node, key):
    if isinstance(node, dict):
        if key in node: return node[key]
        for v in node.values():
            r = find_key(v, key)
            if r is not None: return r
    elif isinstance(node, list):
        for v in node:
            r = find_key(v, key)
            if r is not None: return r
    return None

raw = open(sys.argv[1], "rb").read()
data = None
try:
    data = find(plistlib.loads(raw))
except Exception:
    m = re.search(rb'"CaptureSnapshot" = <([0-9a-fA-F]+)>', raw)
    data = bytes.fromhex(m.group(1).decode()) if m else None
if not data:
    sys.exit("CaptureSnapshot not found (boot with -tgldmiccap)")
out = sys.argv[2] if len(sys.argv) > 2 else "tgldmic-dmic.wav"
w = wave.open(out, "wb"); w.setnchannels(4); w.setsampwidth(4); w.setframerate(48000); w.writeframes(data); w.close()
s = struct.unpack("<%di" % (len(data) // 4), data)
print(f"{out}: {len(data) // 16} frames ({len(data) / 768000:.2f} s)")
for c in range(4):
    v = s[c::4]
    rms = math.sqrt(sum(x * x for x in v) / len(v)) / 2**31
    pk = max(abs(x) for x in v) / 2**31
    db = lambda x: 20 * math.log10(x) if x > 0 else float("-inf")
    print(f"  ch{c}: rms {db(rms):6.1f} dBFS  peak {db(pk):6.1f} dBFS")

try:
    env = find_key(plistlib.loads(raw), "CaptureEnvelope")
except Exception:
    env = None
if env:
    vals = struct.unpack("<%dI" % (len(env) // 4), env)
    print("loudness envelope ch0 (mean |x| per 100 ms, dBFS); '#' = 1 dB above -70:")
    for i, v in enumerate(vals):
        db = 20 * math.log10(v / 2**31) if v else -120
        print(f"  {i / 10:4.1f}s {db:6.1f} " + "#" * max(0, int(db + 70)))

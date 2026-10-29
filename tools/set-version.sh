#!/bin/bash
# Set the TGLDmic version everywhere: tools/set-version.sh 0.4.6
set -euo pipefail
V="${1:?usage: set-version.sh X.Y.Z}"
cd "$(dirname "$0")/.."
for p in kext/TGLDmic/Info.plist hal/Info.plist; do
  python3 - "$p" "$V" <<'PY'
import plistlib, sys
p, v = sys.argv[1], sys.argv[2]
d = plistlib.load(open(p, 'rb')); d['CFBundleShortVersionString'] = d['CFBundleVersion'] = v
plistlib.dump(d, open(p, 'wb'), sort_keys=False)
PY
done
sed -i.bak -E "s/(KMOD_EXPLICIT_DECL\(com\.labrouss\.TGLDmic, \")[^\"]+/\1$V/" kext/TGLDmic/kmod_info.c
sed -i.bak -E "s/(#define kTGLDmicVersionString +\")[^\"]+/\1$V/" kext/TGLDmic/TGLDmicShared.h
sed -i.bak -E "s/^# TGLDmic [0-9.]+/# TGLDmic $V/" docs/INSTALL.md
rm -f kext/TGLDmic/*.bak docs/*.bak
grep -h -E "CFBundleShortVersionString|KMOD_EXPLICIT|kTGLDmicVersionString" -A1 kext/TGLDmic/Info.plist kext/TGLDmic/kmod_info.c kext/TGLDmic/TGLDmicShared.h hal/Info.plist | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | sort | uniq -c

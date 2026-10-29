#!/bin/bash
# Install the TGLDmic Core Audio plug-in. The kext itself goes into OpenCore (Kernel > Add) with boot-arg -tgldmic.
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
[ -d "$DIR/TGLDmic.driver" ] || { echo "TGLDmic.driver not found next to this script"; exit 1; }

# Preflight: is the matching kext loaded? (same rules as the .pkg installer)
WANT_VER="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$DIR/TGLDmic.driver/Contents/Info.plist" 2>/dev/null || echo '?')"
WANT_IF=1
IOR="$(ioreg -r -c TGLDmic -d 1 -w0 2>/dev/null)"
prop() { printf '%s\n' "$IOR" | sed -n "s/.*\"$1\" = \"\{0,1\}\([^\"]*\)\"\{0,1\}$/\1/p" | head -1; }
if [ -z "$IOR" ]; then
  echo "WARNING: TGLDmic kext is not loaded. Add TGLDmic.kext $WANT_VER to OpenCore + boot-arg -tgldmic, then reboot."
else
  KVER="$(prop TGLDmicVersion)"; KIF="$(prop TGLDmicInterface)"; KPH="$(prop TGLDmicPhase)"
  if [ -z "$KVER" ]; then
    echo "WARNING: loaded kext is older than 0.4.6 (no version reported). Update it to $WANT_VER."
  elif [ "$KIF" != "$WANT_IF" ]; then
    echo "ERROR: loaded kext $KVER uses interface $KIF, plug-in $WANT_VER needs $WANT_IF. Update the kext first."; exit 1
  elif [ "$KVER" != "$WANT_VER" ]; then
    echo "WARNING: kext $KVER, plug-in $WANT_VER (compatible; updating the kext is recommended)."
  else
    echo "OK: TGLDmic kext $KVER loaded, interface $KIF."
  fi
  [ -n "$KPH" ] && [ "$KPH" != 4 ] && echo "WARNING: boot-arg -tgldmic not set (phase $KPH); no microphone until you add it."
fi
sudo rm -rf /Library/Audio/Plug-Ins/HAL/TGLDmic.driver
sudo cp -R "$DIR/TGLDmic.driver" /Library/Audio/Plug-Ins/HAL/
sudo chown -R root:wheel /Library/Audio/Plug-Ins/HAL/TGLDmic.driver
sudo xattr -dr com.apple.quarantine /Library/Audio/Plug-Ins/HAL/TGLDmic.driver 2>/dev/null || true
sudo killall coreaudiod 2>/dev/null || sudo launchctl kickstart -k system/com.apple.audio.coreaudiod
echo "Installed. Look for 'Internal Microphone (TGLDmic)' in System Settings > Sound > Input."

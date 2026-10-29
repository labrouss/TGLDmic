#!/bin/bash
# Install the TGLDmic Core Audio plug-in. The kext itself goes into OpenCore (Kernel > Add) with boot-arg -tgldmic.
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
[ -d "$DIR/TGLDmic.driver" ] || { echo "TGLDmic.driver not found next to this script"; exit 1; }
sudo rm -rf /Library/Audio/Plug-Ins/HAL/TGLDmic.driver
sudo cp -R "$DIR/TGLDmic.driver" /Library/Audio/Plug-Ins/HAL/
sudo chown -R root:wheel /Library/Audio/Plug-Ins/HAL/TGLDmic.driver
sudo xattr -dr com.apple.quarantine /Library/Audio/Plug-Ins/HAL/TGLDmic.driver 2>/dev/null || true
sudo killall coreaudiod 2>/dev/null || sudo launchctl kickstart -k system/com.apple.audio.coreaudiod
echo "Installed. Look for 'Internal Microphone (TGLDmic)' in System Settings > Sound > Input."

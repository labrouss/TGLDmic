#!/bin/bash
sudo rm -rf /Library/Audio/Plug-Ins/HAL/TGLDmic.driver
sudo killall coreaudiod 2>/dev/null || sudo launchctl kickstart -k system/com.apple.audio.coreaudiod
echo "TGLDmic.driver removed."

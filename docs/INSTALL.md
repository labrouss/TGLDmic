# TGLDmic 0.4.4: internal microphone for the HP EliteBook 840 G8 (macOS)

Each build produces two files:

| File | What |
|---|---|
| `TGLDmic-<version>-kext.zip` | `TGLDmic.kext` (kernel driver, goes into OpenCore) + `tgldmic-probe`, `list-audio-devices.py`, `uninstall-hal.sh` |
| `TGLDmic-<version>.pkg` | Installer for the Core Audio plug-in `TGLDmic.driver` ("Internal Microphone (TGLDmic)") |

## 1. Kext (OpenCore)
1. Replace `EFI/OC/Kexts/TGLDmic.kext` with this version (keep it after AppleALC in `Kernel > Add`).
2. In `boot-args`, **remove** `-tgldmiccap` and `tgldmicdelay=…` and **add** `-tgldmic`.
3. Reboot. About 10 s after the controller comes up the DSP boots and stays running:

       ioreg -l -w0 -c TGLDmic | grep -E '"BootResult"|"TGLDmicPhase"|"Streaming"'
       # expect: BootResult = "OK FW_READY", TGLDmicPhase = 4, Streaming = No, PowerHold = No

## 2. Test the kernel path first
    ./tgldmic-probe 3 probe.wav

Expect `start: 0x0`, about 48000 frames/s and levels around -60 dBFS when quiet, -50 to -30 dBFS
when you talk. `probe.wav` is the raw 4-channel recording.

## 3. Install the Core Audio plug-in
Double-click `TGLDmic-<version>.pkg`. It is not notarized: if macOS blocks it, right-click → Open,
or allow it under System Settings → Privacy & Security. Command line alternative:

    sudo installer -pkg TGLDmic-<version>.pkg -target /

The installer replaces any previous `TGLDmic.driver` and restarts Core Audio.

Then go to **System Settings → Sound → Input**, pick **Internal Microphone (TGLDmic)** and talk:
the input level meter should move. QuickTime → File → New Audio Recording can record it.

* The **input volume slider** is a software gain from 0 to 36 dB (default 24 dB).
* Left = mic 0, right = mic 1 (the closely spaced pair).

## Logs if something does not work
    python3 list-audio-devices.py      # is "Internal Microphone (TGLDmic)" published by Core Audio?
    log show --last boot --predicate 'eventMessage CONTAINS "TGLDmic: "' --info   # plug-in property trace + IO
    log show --last 10m --predicate 'eventMessage CONTAINS "TGLDmic"' --info
    ioreg -l -w0 -c TGLDmic          # "Log" property: the kext's last 128 lines

## Known limits
* The DSP is powered only while something records: the first ~120 ms of each new recording session
  boot the DSP. While recording, TGLDmic holds the HDA PCI device in D0 (IOKit power child), so the
  HDA driver's idle power-down cannot reset the DSP mid-stream.
* System sleep while recording stops the stream cleanly; recording again after wake re-boots the DSP.
* Tested on Ventura only. Sequoia and Tahoe (HDA-Universal) still need the phase 1 stream check.

## Remove
    ./uninstall-hal.sh                          # from the kext zip; removes the plug-in
    sudo pkgutil --forget com.labrouss.TGLDmic.driver.pkg
    # and drop -tgldmic from boot-args / TGLDmic.kext from OpenCore
* Core Audio IO buffers above ~2048 frames could read outside the 4096-frame ring; normal apps use 512–1024.

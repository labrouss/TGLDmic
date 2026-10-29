# TGLDmic 0.4.0: internal microphone for the HP EliteBook 840 G8 (macOS)

Contents of this package:

| File | What |
|---|---|
| `TGLDmic.kext` | Kernel driver: boots the SOF firmware on the audio DSP and runs the DMIC pipeline |
| `TGLDmic.driver` | Core Audio plug-in: shows "Internal Microphone (TGLDmic)" as an input device |
| `tgldmic-probe` | Command-line test: talks to the kext directly, no Core Audio involved |
| `install-hal.sh` / `uninstall-hal.sh` | Install/remove the Core Audio plug-in |
| `snapshot-to-wav.py` | For the older `-tgldmiccap` boot test |

## 1. Kext (OpenCore)
1. Replace `EFI/OC/Kexts/TGLDmic.kext` with this version (keep it after AppleALC in `Kernel > Add`).
2. In `boot-args`, **remove** `-tgldmiccap` and `tgldmicdelay=…` and **add** `-tgldmic`.
3. Reboot. About 10 s after the controller comes up the DSP boots and stays running:

       ioreg -l -w0 -c TGLDmic | grep -E '"BootResult"|"TGLDmicPhase"|"Streaming"'
       # expect: BootResult = "OK FW_READY", TGLDmicPhase = 4, Streaming = No

## 2. Test the kernel path first
    ./tgldmic-probe 3 probe.wav

Expect `start: 0x0`, about 48000 frames/s and levels around -60 dBFS when quiet, -50 to -30 dBFS
when you talk. `probe.wav` is the raw 4-channel recording.

## 3. Install the Core Audio plug-in
    ./install-hal.sh

Then go to **System Settings → Sound → Input**, pick **Internal Microphone (TGLDmic)** and talk:
the input level meter should move. QuickTime → File → New Audio Recording can record it.

* The **input volume slider** is a software gain from 0 to 36 dB (default 24 dB).
* Left = mic 0, right = mic 1 (the closely spaced pair).

## Logs if something does not work
    log show --last 10m --predicate 'eventMessage CONTAINS "TGLDmic"' --info
    ioreg -l -w0 -c TGLDmic          # "Log" property: the kext's last 128 lines

## Known limits
* System sleep isn't handled yet (this machine's S3 resume doesn't work anyway).
* If the HDA driver resets the controller, the next recording reboots the DSP automatically.
* Tested on Ventura only. Sequoia and Tahoe (HDA-Universal) still need the phase 1 stream check.

## Remove
    ./uninstall-hal.sh   # and drop -tgldmic from boot-args
* Core Audio IO buffers above ~2048 frames could read outside the 4096-frame ring; normal apps use 512–1024.

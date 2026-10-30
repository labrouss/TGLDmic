# TGLDmic: internal microphone for Intel Tiger Lake & Comet Lake laptops (macOS)

Supported laptops:
- **Lenovo ThinkPad X1 Carbon Gen 7** (Comet Lake i7-10710U / i7-10510U, Whiskey Lake i7-8565U / i5-8265U) with ALC285 & 4-mic array
- **HP EliteBook 840 G8** (Tiger Lake i5-1135G7 / i7-1165G7) with ALC285 & 4-mic array
- Other Intel Tiger Lake (`8086:A0C8`, `8086:43C8`) and Comet Lake / Whiskey Lake / Coffee Lake (`8086:02C8`, `8086:9DC8`, `8086:A348`) laptops with PDM microphone arrays on the Intel cAVS audio DSP.

Each build produces two files:

| File | What |
|---|---|
| `TGLDmic-<version>-kext.zip` | `TGLDmic.kext` (kernel driver, goes into OpenCore) + `tgldmic-probe`, `list-audio-devices.py`, `uninstall-hal.sh` |
| `TGLDmic-<version>.pkg` | Installer for the Core Audio plug-in `TGLDmic.driver` ("Internal Microphone (TGLDmic)") |

## 1. Kext (OpenCore)
1. Copy `TGLDmic.kext` to your EFI: `EFI/OC/Kexts/TGLDmic.kext`.
2. In `config.plist` -> `Kernel` -> `Add`:
   - Add `TGLDmic.kext` **after** `AppleALC.kext` (or `HDA-Universal.kext`).
   - `BundlePath`: `TGLDmic.kext`
   - `ExecutablePath`: `Contents/MacOS/TGLDmic`
   - `PlistPath`: `Contents/Info.plist`
   - `Enabled`: `true`
3. In `NVRAM` -> `Add` -> `7C436110-AB2A-4BBB-A880-FE41995C9F82` -> `boot-args`:
   - Add `-tgldmic`.
   - *Note for X1 Carbon Gen 7*: Platform detection is fully automatic (`0x02C8` Comet Lake LP). If using a custom spoof ID or troubleshooting, you can explicitly add `-tgldmiccml`.
4. Reboot. About 10 s after the controller comes up the kext runs a one-time DSP boot test, then powers the DSP down until something records:

       ioreg -l -w0 -c TGLDmic | grep -E '"BootResult"|"TGLDmicPhase"|"Streaming"|"Platform"'
       # expect: BootResult = "OK FW_READY", TGLDmicPhase = 4, Streaming = No

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
* Default routing: Left = mic 0, right = mic 1 (front-facing pair).

### ThinkPad X1 Carbon Gen 7: 4-Channel Routing Modes
The X1 Carbon Gen 7 has 4 far-field 360-degree microphones. You can customize channel mapping:
```bash
# Mode 0 (default): Front mic pair (mic 0 + mic 1)
defaults write com.labrouss.TGLDmic ChannelMode -int 0

# Mode 1: Ambient / room mic pair (mic 2 + mic 3)
defaults write com.labrouss.TGLDmic ChannelMode -int 1

# Mode 2: 4-mic 360-degree blend (stereo mix of all 4 microphones)
defaults write com.labrouss.TGLDmic ChannelMode -int 2
```
Restart `coreaudiod` after changing the mode:
```bash
sudo killall coreaudiod 2>/dev/null || sudo launchctl kickstart -k system/com.apple.audio.coreaudiod
```

## Logs if something does not work
More in [TESTING.md](TESTING.md).

    python3 list-audio-devices.py      # is "Internal Microphone (TGLDmic)" published by Core Audio?
    log show --last boot --predicate 'eventMessage CONTAINS "TGLDmic: "' --info   # plug-in property trace + IO
    log show --last 10m --predicate 'eventMessage CONTAINS "TGLDmic"' --info
    ioreg -l -w0 -c TGLDmic          # "Log" property: the kext's last 128 lines

## Known limits
* The DSP is powered only while something records: the first ~120 ms of each new recording session
  boot the DSP. While recording, TGLDmic holds the HDA PCI device in D0 (IOKit power child), so the
  HDA driver's idle power-down cannot reset the DSP mid-stream.
* System sleep while recording stops the stream cleanly; recording again after wake re-boots the DSP.
* Tested on Ventura (AppleALC) and Tahoe (HDAUniversal). Sonoma and Sequoia not yet verified.
* Core Audio IO buffers above ~2048 frames could read outside the 4096-frame ring; normal apps use 512–1024.

## Remove
    ./uninstall-hal.sh                          # from the kext zip; removes the plug-in
    sudo pkgutil --forget com.labrouss.TGLDmic.driver.pkg
    # and drop -tgldmic from boot-args / TGLDmic.kext from OpenCore

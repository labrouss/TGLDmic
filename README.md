<p align="center">
  <img src="assets/tgldmic-icon-256.png" width="200" alt="TGLDmic logo">
</p>

<h1 align="center">TGLDmic</h1>
<p align="center"><b>Tiger Lake & Comet Lake microphone driver for macOS</b><br>
Internal digital microphones (DMIC) of Intel Tiger Lake and Comet Lake / Cannon Lake laptops (including ThinkPad X1 Carbon Gen 7) via the Intel Smart Sound DSP (SOF)</p>

> [!NOTE]
> **This fork is adapted for Comet Lake (CML) ThinkPad X1 Carbon Gen 7 (X1C7) from [labrouss/TGLDmic](https://github.com/labrouss/TGLDmic).** It introduces cAVS 1.8 platform support, dynamic core power gating, dual embedded Intel production SOF v2.2 firmware (`sof-cml.ri` & `sof-tgl.ri`), dynamic input stream allocation, and 4-channel microphone routing modes.

> Unofficial community project. Not affiliated with or endorsed by Intel, Apple or any laptop vendor.

<p align="center"><a href="https://hits.sh/github.com/labrouss/TGLDmic/"><img src="https://hits.sh/github.com/labrouss/TGLDmic.svg?view=today-total&style=flat-square&label=visits" alt="visits"></a></p>

On modern Intel laptops (Tiger Lake, Comet Lake, Whiskey Lake, etc.) the built-in digital microphones are not wired to the HDA codec but to the
Smart Sound DSP (cAVS), so AppleALC cannot make them work and hackintoshes end up without an internal mic.
TGLDmic boots Intel's signed SOF firmware on that DSP and exposes the mics to macOS as a normal input device,
**Internal Microphone (TGLDmic)**. It runs next to AppleHDA/AppleALC, which keep speakers and headset.

## Features
- Standard Core Audio input device: shows up in System Settings and any recording app (tested: Voice Memos, QuickTime)
- Stereo 48 kHz, input gain 0–36 dB via the macOS input volume slider
- DSP is powered only while something records (~120 ms start-up)
- Repeated recording sessions; coexists with AppleHDA/AppleALC
- Installer package for the Core Audio plug-in that checks the kext version first
- Dual architecture support: Intel cAVS 2.5 (Tiger Lake) & cAVS 1.8 (Comet Lake / Cannon Lake / Coffee Lake)

## Compatibility
| | |
|---|---|
| Supported (Comet Lake / CFL) | Intel Smart Sound DSP `8086:02C8`: Comet Lake **LP** (10th-gen Core, e.g. **Lenovo ThinkPad X1 Carbon Gen 7** i7-10710U / i5-10210U)<br>Intel Smart Sound DSP `8086:9DC8`: Cannon Lake / Whiskey Lake **LP** (e.g. ThinkPad X1 Carbon Gen 7 i7-8565U)<br>Intel Smart Sound DSP `8086:06C8`, `8086:A348`: Comet Lake-H, Coffee Lake-H |
| Supported (Tiger Lake) | Intel Smart Sound DSP `8086:A0C8`: Tiger Lake **LP** (11th-gen Core, UP3/UP4)<br>Intel Smart Sound DSP `8086:43C8`: Tiger Lake **H** |
| Tested hardware | **Lenovo ThinkPad X1 Carbon Gen 7** (Comet Lake i7-10710U, 4 PDM mics, ALC285)<br>**HP EliteBook 840 G8** (Tiger Lake i5-1135G7, 4 PDM mics, ALC285) |
| macOS | Ventura, Sonoma, Sequoia, Tahoe tested / supported |
| Audio driver | Works next to **AppleALC/AppleHDA** or **HDAUniversal** (speakers and headset stay with them) |

> **Device-id spoof:** OpenCore configs usually spoof the audio controller as `34C8` or `9DC8` for AppleALC.
> TGLDmic matches those spoof IDs too, detects the underlying hardware platform family, and boots the matching Intel-signed SOF firmware (`sof-cml.ri` for Comet Lake or `sof-tgl.ri` for Tiger Lake).

## Install
Download both files from [Releases](../../releases):

| File | What |
|---|---|
| `TGLDmic-<version>-kext.zip` | `TGLDmic.kext` for OpenCore + test tools |
| `TGLDmic-<version>.pkg` | Installer for the Core Audio plug-in |

1. Add `TGLDmic.kext` to `EFI/OC/Kexts` and to `Kernel > Add` in `config.plist`, **after AppleALC**.
2. Add `-tgldmic` to `boot-args` and reboot.
3. Run `TGLDmic-<version>.pkg`. It checks that the matching kext is loaded before installing.
4. Pick **Internal Microphone (TGLDmic)** under **System Settings → Sound → Input**.

Full steps, troubleshooting and removal: [docs/INSTALL.md](docs/INSTALL.md).

## How it works
```
 Apps ─▶ Core Audio ─▶ TGLDmic.driver (HAL plug-in) ─▶ TGLDmic.kext ─▶ Smart Sound DSP (SOF) ─▶ PDM mics
                         ring buffer + timestamps      boots firmware,      capture pipeline
                                                       sets up DMA          DMIC0 → PCM
```
The kext loads the Intel-signed SOF firmware into the DSP, replays the IPC messages Linux uses to
build the DMIC capture pipeline, and streams the audio into a ring buffer by DMA. The HAL plug-in
maps that ring and presents it to Core Audio. Details: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Your laptop is not working?
The pipeline setup is recorded from the reference machine. If the mic stays silent on your laptop:
1. Boot an Ubuntu live USB and run `sudo bash linux/capture-sof-dmic.sh`. It records no audio into the tarball.
2. Open an issue with the tarball and `ioreg -l -w0 -c TGLDmic` from macOS.

Debug boot args and test procedures: [docs/TESTING.md](docs/TESTING.md).

## Building
GitHub Actions builds both files on every push, and attaches them to a Release for `v*` tags.
Locally on macOS with Xcode command-line tools and [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK):

    make -C kext SDK=/path/to/MacKernelSDK
    make -C hal

`tools/set-version.sh X.Y.Z` sets the version in every place.

| Path | What |
|---|---|
| `kext/` | Kernel driver `TGLDmic.kext` |
| `hal/` | Core Audio plug-in `TGLDmic.driver`, `tgldmic-probe` |
| `pkg/` | Installer package sources |
| `firmware/` | Intel-signed SOF firmware embedded in the kext |
| `linux/` | Capture tool and IPC extractor |
| `captures/` | Reference captures (logs, NHLT, topology; no audio) |
| `tools/` | Build and diagnostic helpers |
| `docs/` | Install, testing, architecture |

## Credits
- [Sound Open Firmware](https://github.com/thesofproject) — the DSP firmware and the Linux driver this work is based on
- [acidanthera](https://github.com/acidanthera) — MacKernelSDK, OpenCore, AppleALC

## Licence
TGLDmic is BSD-3-Clause ([LICENSE](LICENSE)). The embedded SOF firmware is Intel's, redistributed
unmodified under [firmware/LICENCE.Intel](firmware/LICENCE.Intel).

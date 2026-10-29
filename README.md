# TGLDmic

Internal microphone driver for the HP EliteBook 840 G8 hackintosh (Tiger Lake, 4-mic PDM array
behind the Smart Sound DSP). Runs next to AppleHDA/AppleALC, which keep speakers and headset.

| Path | What |
|---|---|
| `docs/ARCHITECTURE.md` | Design, phases, risks |
| `linux/capture-sof-dmic.sh` | Phase 0 capture on a Linux live USB |
| `linux/extract-ipc.py` | Turns the capture into `kext/TGLDmic/IpcReplay.hpp` |
| `captures/` | Reference captures (firmware boot, pipeline setup, test WAV) |
| `kext/` | The kext (`make SDK=...`, or GitHub Actions) |
| `hal/` | Core Audio HAL plug-in `TGLDmic.driver`, `tgldmic-probe`, install scripts |
| `pkg/` | Installer scripts for `TGLDmic-<version>.pkg` |
| `docs/INSTALL.md` | End-user install and test steps (shipped in the build artifact) |

## Phase 1: read-only observer (default)
Add `TGLDmic.kext` to OpenCore `Kernel > Add` **after AppleALC**. It writes nothing to hardware.
Boot args: `-tgldmicoff` disables it, `-tgldmicdsp` also reads DSP status registers (BAR4).

    ioreg -l -w0 -c TGLDmic          # "Log" property holds the last 64 log lines
    sudo dmesg | grep TGLDmic
    ioreg -l -w0 -c TGLDmic

## Phase 2: DSP boot (opt-in)
Boot arg `-tgldmicboot`: ~20 s after the controller is up, TGLDmic boots the SOF firmware on the
DSP using the highest output stream as code loader (no interrupts, all polled), waits for
`FW_READY`, then powers the DSP core down again. `-tgldmickeep` leaves it running.
Result: `BootState` (1 = OK, 2 = failed) and `BootResult` in `ioreg -l -w0 -c TGLDmic`.
Test speakers/headset before and after the attempt.

## Phase 3: DMIC capture test (opt-in)
Boot arg `-tgldmiccap` (implies `-tgldmicboot`): after FW_READY, TGLDmic replays the 20 Linux IPC3
setup messages for pipeline 10, runs input stream 6 (tag 7) decoupled into the DSP, records 8 s of 4-ch 48 kHz S32_LE audio
(published: the loudest 1 s as `CaptureSnapshot`, and a 100 ms loudness envelope as `CaptureEnvelope`),
then tears everything down and powers the DSP off.

    ioreg -a -l -w0 -c TGLDmic > tgldmic.plist
    python3 tools/snapshot-to-wav.py tgldmic.plist dmic.wav     # WAV + per-channel levels

Talk near the laptop when it runs: by default 20 s after the controller is ready, or set
`tgldmicdelay=<seconds>` (10-170), e.g. `tgldmicdelay=90` to record after you have logged in.

## Phase 4: real input device (boot arg `-tgldmic`)
The kext boots the DSP 10 s after the controller is up and keeps it running. `TGLDmicUserClient`
exposes start/stop, the DMA position and the 64 KB ring (mapped read-only). The HAL plug-in
`TGLDmic.driver` turns that into "Internal Microphone (TGLDmic)": stereo Float32 48 kHz, with zero
time stamps from the DMA position and a 0–36 dB input gain. See `docs/INSTALL.md`.

## CI artifacts
Every push builds `TGLDmic-<version>-kext.zip` (kext + tools) and `TGLDmic-<version>.pkg`
(Core Audio plug-in installer) as separate artifacts. Pushing a tag `v*` (e.g. `git tag v0.4.3 && git push --tags`)
also attaches both files to a GitHub Release.

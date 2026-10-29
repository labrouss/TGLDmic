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
setup messages for pipeline 10, runs input stream 6 (tag 7) decoupled into the DSP, captures 1 s of
4-ch 48 kHz S32_LE audio, then tears everything down and powers the DSP off.

    ioreg -a -l -w0 -c TGLDmic > tgldmic.plist
    python3 tools/snapshot-to-wav.py tgldmic.plist dmic.wav     # WAV + per-channel levels

Talk near the laptop during the first ~25 s after login.

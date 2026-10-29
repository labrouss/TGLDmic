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

## Phase 1 (current): read-only observer
Add `TGLDmic.kext` to OpenCore `Kernel > Add` **after AppleALC**. It writes nothing to hardware.
Boot args: `-tgldmicoff` disables it, `-tgldmicdsp` also reads DSP status registers (BAR4).

    ioreg -l -w0 -c TGLDmic          # "Log" property holds the last 64 log lines
    sudo dmesg | grep TGLDmic
    ioreg -l -w0 -c TGLDmic

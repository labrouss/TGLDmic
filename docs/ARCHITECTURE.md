# TGLDmic — Tiger Lake DMIC driver for macOS

Target: HP EliteBook 840 G8 (cAVS 2.5 / TGL-LP, PCI 8086:A0C8, SSID 103c:8ab9),
4-element linear PDM mic array on the DSP DMIC0/1 lines (NHLT link type PDM).

## Constraint
AppleHDA (+AppleALC, layout 11) already drives the ALC285 codec for speakers
and the headset jack. The DMIC is *not* reachable over the HDA link; it sits
behind the audio DSP, which only runs with signed firmware (SOF `sof-tgl.ri`,
Intel production key, IPC3).

## Approach: hybrid, AppleHDA stays
```
             ┌──────────── HDA controller 00:1f.3 ────────────┐
 AppleHDA ──►│ BAR0: CORB/RIRB, coupled streams (playback,     │──► ALC285
             │       headset mic)                             │
 TGLDmic  ──►│ BAR0: PPCTL/PROCEN on 1 spare ISD + 1 OSD      │
             │ BAR4: DSP shim, IPC, SRAM mailbox              │──► DSP ──► DMIC 4ch
             └────────────────────────────────────────────────┘
```
* TGLDmic matches the same IOPCIDevice with its own `IOMatchCategory`
  (no exclusive open), so it loads next to AppleHDA.
* It uses only streams AppleHDA does not touch: the highest output stream
  (to DMA the firmware image to the ROM loader) and the highest input stream
  (decoupled via PPCTL.PROCEN[n]) for DMIC capture.
* DSP IPC and stream position are polled, not interrupt-driven: the MSI
  belongs to AppleHDA.
* Pipeline setup is not built from the topology at runtime: the IPC3
  messages Linux sends for the DMIC PCM are captured once (phase 0) and
  replayed, parameterised only by stream tag / buffer address.

## Phases
| # | Goal | Exit criterion |
|---|------|----------------|
| 0 | Linux capture: fw, tplg, NHLT, IPC3 payloads, mmiotrace, register dumps, test WAV | WAV has signal on 4 channels; IPC sequence decoded |
| 1 | Read-only probe kext next to AppleHDA | Logs GCAP/PPCAP/ADSPCS, no audio regression |
| 2 | DSP boot on macOS | FW_READY received; speakers still work |
| 3 | IPC replay + decoupled input DMA | PCM samples with non-zero RMS in a debug buffer |
| 4 | IOAudioEngine input device | Mic visible in Sound settings, records in QuickTime |
| 5 | Robustness | Survives AppleHDA reset, sleep/wake, gain control |

## Main risks
1. AppleHDA controller reset (CRST) on init/sleep wipes PP state → TGLDmic
   must re-arm after AppleHDA and on every power-state change.
2. Whether a coupled AppleHDA stream and a decoupled DSP stream can run at
   the same time with GPROCEN set — tested in phase 3.
3. Firmware redistribution: SOF binaries are BSD-licensed (sof-bin); embed
   like IntelBluetoothFirmware does.

## Build
MacKernelSDK + Xcode on a GitHub Actions `macos` runner (no local Mac build
needed). No Lilu dependency planned.

## Target pipeline (from sof-hda-generic-4ch.tplg, SOF v2.2, IPC3)
PCM 6 "DMIC" (48 kHz), pipeline 10 — the only one TGLDmic needs:
```
DMIC0.IN (dai, DMIC0) → BUF10.2 → EQIIR10.0 (highpass) → BUF10.1 → Dmic0 (PGA/volume) → BUF10.0 → PCM6C (host DMA → macOS)
```
PCM 7 "DMIC16kHz" (pipeline 11, DMIC1) is the same chain at 16 kHz; skipped.
Expected IPC3 replay: TPLG_PIPE_NEW, 3× TPLG_BUFFER_NEW, 3× TPLG_COMP_NEW (dai, eq_iir, volume)
+ host comp, TPLG_COMP_CONNECT×6, DAI_CONFIG (DMIC, NHLT-derived PDM params),
TPLG_PIPE_COMPLETE, STREAM_PCM_PARAMS (stream tag, buffer pages), STREAM_TRIG_START.

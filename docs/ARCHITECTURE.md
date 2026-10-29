# TGLDmic — Tiger Lake DMIC driver for macOS

Target: Intel Tiger Lake laptops (cAVS 2.5 / TGL-LP, PCI 8086:A0C8) with a PDM mic array on the DSP.
Reference machine: HP EliteBook 840 G8 (SSID 103c:8ab9), 4-element linear PDM mic array on the DSP DMIC0/1 lines (NHLT link type PDM).

## Constraint
AppleHDA (+AppleALC, layout 11) already drives the ALC285 codec for speakers
and the headset jack. The DMIC is *not* reachable over the HDA link; it sits
behind the audio DSP, which only runs with signed firmware (SOF `sof-tgl.ri`,
Intel production key, IPC3).

## Who owns the HDA controller (varies per macOS version)
| macOS | Speakers / headset | Notes |
|---|---|---|
| Ventura 13, Sonoma 14 | AppleHDA + AppleALC | verified: AppleHDA uses in0/in1 (tags 1/2), out0 (tag 1) |
| Sequoia 15 | HDA-Universal (AppleALC capped at MaxKernel 23.99.99 in this EFI) | AppleHDA still ships in 15 |
| Tahoe 26 | HDA-Universal / VoodooHDA / reinstalled AppleHDA | AppleHDA.kext removed from the OS since 26 beta 2 |

So TGLDmic must **not assume AppleHDA**: it observes which stream descriptors the current owner
uses and picks free ones, and it never relies on the owner's power management. If *no* driver
owns the controller, TGLDmic has to bring it up itself (memory/bus-master enable, GCTL.CRST),
which is a separate, later code path.

## Approach: hybrid, the existing HDA driver stays
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
| 0 ✅ | Linux capture: fw, tplg, NHLT, IPC3 payloads, mmiotrace, register dumps, test WAV | WAV has signal on 4 channels; IPC sequence decoded |
| 1 ✅ | Read-only probe kext next to AppleHDA | Logs GCAP/PPCAP/ADSPCS, no audio regression |
| 2 ✅ | DSP boot on macOS | FW_READY received; speakers still work |
| 3 ✅ | IPC replay + decoupled input DMA | PCM samples with non-zero RMS in a debug buffer |
| 4 ✅ | AudioServerPlugIn (user-space Core Audio driver) fed by the kext | Mic visible in Sound settings, records in QuickTime |
| 5 | Robustness | Survives AppleHDA reset, sleep/wake, gain control |

## Exposing the mic to macOS: AudioServerPlugIn, not IOAudioFamily
IOAudioEngine kexts depend on IOAudioFamily, whose future on Tahoe is uncertain (it ships with the
AppleHDA stack that Apple is dropping). The Core Audio-supported way is a user-space
**AudioServerPlugIn** (HAL driver, like BlackHole) in `/Library/Audio/Plug-Ins/HAL`:
```
TGLDmic.kext  ──(IOUserClient: map 64 KB DMA ring + read DPIB position)──►  TGLDmic.driver (HAL plug-in)  ──►  coreaudiod
```
* The ring buffer the DSP writes into is mapped read-only into the plug-in, so there's no copy in the kernel.
* The plug-in timestamps from the DMA position and does the 4ch→2ch (or 4ch exposed) mapping and gain.
* The same plug-in works on Ventura through Tahoe, whichever driver owns the speakers.

## Main risks
0. The controller owner differs per macOS version (see above): stream usage and power
   management must be re-checked on Sequoia/Tahoe with HDA-Universal.
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

## Status (2026-10-29, Ventura 13.6, v0.3.0)
* Phase 2: SOF v2.2 (Intel-signed) boots next to AppleHDA in ~17 ms. Needed vs. a naive port:
  SPIB enabled on the loader stream, and the production-signed image (community build -> ROM error 44).
* Phase 3: all 20 replayed IPC3 messages accepted; in6/tag 7 decoupled capture runs at the expected
  768 B/ms; samples match the Linux noise floor (-60 dBFS, ch2 == ch3 as on Linux).
  Reference: `captures/macos-v0.3.0/` (log; noise floor -60 dBFS, recording not kept).
* v0.3.2 (music next to the laptop): 8 s sustained at 806 B/ms, envelope swings -63..-49 dBFS,
  loudest second -49 dBFS RMS / -30 dBFS peak, speech band -50 dBFS, mic-to-mic correlation 0.82.
  **Internal DMIC confirmed working under macOS.** Reference: `captures/macos-v0.3.2/` (loudest second -49 dBFS, recording not kept).

## Status (2026-10-29, v0.4.2)
* **Phase 4 confirmed on Ventura:** "Internal Microphone (TGLDmic)" is published by Core Audio, the
  Sound settings level meter follows speech, Voice Memos records cleanly (no clicks/drop-outs).
* Bug found via HAL property trace: the HAL queries output-scope `saft` on input-only devices and
  drops the device if it fails -> answer scoped device properties in every scope.
* Open: the DSP was not running when the first client connected (state 0x4); the user client re-booted
  it on demand (~120 ms). To check in phase 5: is the boot-time boot skipped, or does the HDA driver's
  idle power management (D3) power the DSP down?
* v0.4.3 findings: DSP found at ADSPCS reset default 0x0f0f with GCTL 0x101 = D3->D0 cycle by the HDA
  driver's idle PM (AppleHDA doesn't know about the DSP stream). v0.4.4: TGLDmic is an IOKit PM child
  of the PCI device and holds "on" while streaming; DSP boots per recording session and is powered down
  after the last client. Also fixed: observer marked TGLDmic's own in6 as "used by HDA driver",
  blocking every recording after the first.
* v0.4.4 regression: joining the PCI device's PM tree at load time (state 0, before AppleHDA attached)
  let the function idle before AppleHDA initialized it -> no AppleHDA devices, GCTL 0, TGLDmic never
  ready. Also learned: AppleHDA's idle state keeps the PCI function in D0 but holds the controller in
  reset (CRST=0). v0.4.5: join PM only after the HDA driver attached, as *its* child (holding "on"
  keeps it out of its idle/reset state while recording); reset-aware init; no PCI-level PM join.

## Findings: Tahoe / HDAUniversal (v0.4.6 - v0.4.11)
- **DEVC no-snoop enable (PCI 0x78 bit 11) must be SET for the ROM to load firmware.** With it clear the
  ROM accepts both manifests (FSR states 7, 2, 3) and then halts (0x80000003, error 0) before fetching
  the rest. AppleHDA leaves it set (0x2800); HDAUniversal clears it (0x2000). TGLDmic sets it while the
  DSP is powered and restores the HDA driver's value when core0 powers down.
- Tahoe gives the controller no per-device IOMMU mapper; the system mapper's IOVAs work (physical
  addresses do not: DMA fetches nothing).
- HDAUniversal keeps the controller powered and must not get a power child: a child asking for "off"
  made it drop the function to D3 (audio out lost). The power hold is used only under AppleHDA.


# Testing and debugging TGLDmic

For development and bug reports. Normal users only need `-tgldmic` (see [INSTALL.md](INSTALL.md)).

## Boot arguments
| Boot arg | Mode | What it does |
|---|---|---|
| *(none)* | 1 – observer | Read-only: logs the HDA controller state, writes nothing to hardware |
| `-tgldmicdsp` | 1 + | Also reads DSP status registers (BAR4) |
| `-tgldmicboot` | 2 – DSP boot | Boots the SOF firmware once, then powers the DSP down |
| `-tgldmickeep` | 2 + | Leaves the DSP running after the boot test |
| `-tgldmiccap` | 3 – capture test | Boots, records 8 s from the mics, tears down |
| `tgldmicdelay=<s>` | 2/3 | Delay before the test, 10–170 s (default 20) |
| `-tgldmic` | 4 – service | Normal use: microphone exposed to Core Audio |
| `-tgldmicoff` | – | Disables the kext completely |
| `tgldmicdma=2` | – | Force physical DMA addresses (testing only; default uses the device or system IOMMU mapper) |
| `tgldmicsnoop=0` | – | Leave the controller's no-snoop DMA bit as found (default: clear it so DMA sees CPU-written data) |

`TGLDmicPhase` in ioreg shows the active mode. The `Log` property holds the kext's last 128 log lines.

    ioreg -l -w0 -c TGLDmic
    ioreg -r -c TGLDmic -d 1 | grep -E 'TGLDmicVersion|TGLDmicInterface|TGLDmicPhase'

## Mode 1: observer
Add the kext without boot args. Check that speakers and headset still work, and that `Log`
shows the controller found and GCAP read correctly.

## Mode 2: DSP boot
`-tgldmicboot`: about 20 s after the controller is up, TGLDmic loads the firmware through the highest
output stream (polled, no interrupts) and waits for `FW_READY`.

| ioreg | Expected |
|---|---|
| `BootState` | 1 = OK, 2 = failed |
| `BootResult` | `OK FW_READY` |

Test speakers and headset before and after the attempt.

## Mode 3: capture test
`-tgldmiccap` (implies `-tgldmicboot`) replays the Linux IPC setup for pipeline 10, records 8 s of
4-channel 48 kHz S32_LE audio from input stream 6, then tears everything down. Talk near the laptop
while it runs; `tgldmicdelay=90` gives you time to log in first.

It publishes the loudest second as `CaptureSnapshot` and a 100 ms loudness envelope as `CaptureEnvelope`:

    ioreg -a -l -w0 -c TGLDmic > tgldmic.plist
    python3 tools/snapshot-to-wav.py tgldmic.plist dmic.wav     # WAV + per-channel levels

Reference levels on the EliteBook 840 G8: about -60 dBFS quiet, -50 to -30 dBFS when talking.

## Mode 4: kernel path without Core Audio
With `-tgldmic`, test the kext directly before installing the plug-in:

    ./tgldmic-probe 3 probe.wav

Expect `start: 0x0`, about 48000 frames/s and the levels above.

## Core Audio plug-in
    python3 tools/list-audio-devices.py      # is "Internal Microphone (TGLDmic)" published?
    log show --last boot --predicate 'eventMessage CONTAINS "TGLDmic: "' --info
    log show --last 10m --predicate 'eventMessage CONTAINS "TGLDmic"' --info

While recording, ioreg should show `Streaming = Yes` and `PowerHold = Yes`.

## Checklist for a release
- [ ] Speakers and headset still work with the kext loaded
- [ ] Mic works in System Settings and one recording app
- [ ] Several recordings in a row
- [ ] Idle 5 minutes, then record again
- [ ] Sleep/wake, then record again
- [ ] Installer: no warning with matching kext; warning without kext

## Linux capture (new laptops)
`linux/capture-sof-dmic.sh` records how Linux SOF sets up the DMIC. Run it from an Ubuntu live USB:

    sudo bash capture-sof-dmic.sh

| Variable | Default | Effect |
|---|---|---|
| `KEEPWAV=1` | off | Include the test recording in the tarball |
| `ALLDMIC=1` | off | Also capture PCM 7 |
| `DEBUGFS=1` | off | Read SOF debugfs (froze the reference machine) |
| `BARDUMP=1` / `MMIOTRACE=1` | off | Register dumps / MMIO trace (can freeze) |

`linux/extract-ipc.py` turns a capture into `kext/TGLDmic/IpcReplay.hpp`.

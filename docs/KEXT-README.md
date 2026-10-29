# TGLDmic kext package

| File | What |
|---|---|
| `TGLDmic.kext` | Put in `EFI/OC/Kexts`, add to `Kernel > Add` after AppleALC, boot-arg `-tgldmic` |
| `tgldmic-probe` | Test the kernel path without Core Audio: `./tgldmic-probe 3 probe.wav` |
| `list-audio-devices.py` | List Core Audio devices: `python3 list-audio-devices.py` |
| `snapshot-to-wav.py` | For the `-tgldmiccap` boot-time capture test |
| `uninstall-hal.sh` | Remove the Core Audio plug-in installed by `TGLDmic-<version>.pkg` |

The microphone device itself comes from **`TGLDmic-<version>.pkg`** (installs
`/Library/Audio/Plug-Ins/HAL/TGLDmic.driver` and restarts Core Audio). See `INSTALL.md`.

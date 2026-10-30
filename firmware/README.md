# Firmware

| File | What |
|---|---|
| `intel/sof-tgl.ri` | Sound Open Firmware v2.2 for Tiger Lake, **Intel production-signed**, unmodified, from [thesofproject/sof-bin](https://github.com/thesofproject/sof-bin) (`intel-signed/`). sha256 `97c8d8dea6048329bfbb1dbbd56609934d388b7e3a9acffe20df0b7daf14d7d3` |
| `intel/sof-cml.ri` | Sound Open Firmware v2.2 for Comet Lake / Cannon Lake / Coffee Lake (including ThinkPad X1 Carbon Gen 7), **Intel production-signed**, unmodified, from [thesofproject/sof-bin](https://github.com/thesofproject/sof-bin) (`intel-signed/sof-cnl.ri`). sha256 `c5119cfca9c4bdf5ab87a87af08fb8fb2bf92767e0f070ea4eb52a877f10359e` |
| `LICENCE.Intel` | Its licence (BSD-3-Clause + Intel limited patent licence), copied verbatim from sof-bin |

The firmware images are embedded into `TGLDmic.kext` at build time (`tools/gen-fw.py`). The community-signed build
does not work on retail machines: the Intel Converged Security and Management Engine (CSE/ME) rejects it (ROM error 44).
Only the Intel production-signed binaries are accepted by retail hardware.

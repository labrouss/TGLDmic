#!/bin/bash
# TGLDmic phase 0 - capture how Linux SOF brings up the DMIC on a Tiger Lake laptop
# Run from an Ubuntu live USB:   sudo bash capture-sof-dmic.sh
# Safe by default: no DSP register access, no mmiotrace. Output is written next to the script (run it from a USB stick).
# Default: no register reads (BARDUMP=1 enables, can freeze), no mmiotrace (MMIOTRACE=1, can freeze), PCM 6 only (ALLDMIC=1 adds PCM 7).
# Output: ./sof-dmic-capture-<date>.tar.gz (no audio unless KEEPWAV=1; safe to attach to an issue)
set -u
[ "$(id -u)" = 0 ] || { echo "Run with sudo"; exit 1; }
PCI=0000:00:1f.3
OUT="${OUTDIR:-$(cd "$(dirname "$0")" && pwd)}/sof-dmic-capture-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"/{fw,regs,debugfs,pcm,trace}
LOG="$OUT/run.log"
log(){ echo "[$(date +%T)] $*" | tee -a "$LOG"; sync; }
TR=/sys/kernel/tracing
[ -d $TR ] || TR=/sys/kernel/debug/tracing
mount | grep -q debugfs || mount -t debugfs none /sys/kernel/debug 2>/dev/null
mount | grep -q tracefs || mount -t tracefs none /sys/kernel/tracing 2>/dev/null

cat /sys/kernel/security/lockdown > "$OUT/lockdown.txt" 2>/dev/null
grep -q '\[none\]' "$OUT/lockdown.txt" 2>/dev/null || log "WARNING: kernel lockdown active -> mmiotrace/BAR dumps will fail (disable Secure Boot)"
uname -a > "$OUT/uname.txt"; lspci -nnvvv -s $PCI > "$OUT/lspci.txt" 2>&1

# ---------- register dump helper (read-only, small known windows) ----------
cat > /tmp/bardump.py <<'PY'
import mmap, os, sys, struct
pci, tag, outdir = sys.argv[1:4]
# (bar, offset, length): BAR0 = HDA ctrl regs; BAR4 = DSP shim/IPC + SRAM mailbox windows
wins = [(0, 0x0, 0x1000)]  # HDA global/stream regs only. DSP BAR4 deliberately NOT read (can hard-hang the PCH)
for bar, off, ln in wins:
    p = f"/sys/bus/pci/devices/{pci}/resource{bar}"
    try:
        fd = os.open(p, os.O_RDONLY | os.O_SYNC)
        size = os.fstat(fd).st_size
        if off + ln > size:
            os.close(fd); continue
        m = mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ)
        data = b''.join(struct.pack('<I', struct.unpack_from('<I', m, off + i)[0]) for i in range(0, ln, 4))
        open(f"{outdir}/{tag}-bar{bar}-{off:06x}.bin", "wb").write(data)
        m.close(); os.close(fd)
        print(f"  dumped bar{bar}+0x{off:x} ({ln} bytes)")
    except Exception as e:
        print(f"  bar{bar}+0x{off:x}: {e}")
PY
bardump(){ [ "${BARDUMP:-0}" = 1 ] || return 0; [ "$(cat /sys/bus/pci/devices/$PCI/power/runtime_status 2>/dev/null)" = active ] || { log "skip BAR dump $1 (device not active)"; return 0; }; log "BAR dump: $1"; python3 /tmp/bardump.py $PCI "$1" "$OUT/regs" >> "$LOG" 2>&1; }

# ---------- 1. static material ----------
log "Copying firmware, topology, NHLT"
# keep signed and community builds apart (same file name!) - only the Intel-signed one passes the CSE on production laptops
for f in /lib/firmware/intel/sof/sof-*.ri*; do [ -f "$f" ] && cp -v "$f" "$OUT/fw/intel-signed-$(basename "$f")" >> "$LOG"; done
for f in /lib/firmware/intel/sof/community/sof-*.ri*; do [ -f "$f" ] && cp -v "$f" "$OUT/fw/community-$(basename "$f")" >> "$LOG"; done
for f in /lib/firmware/intel/sof-tplg/sof-hda-generic*.tplg* /lib/firmware/intel/sof-tplg/sof-cml*.tplg* /lib/firmware/intel/sof-tplg/sof-cnl*.tplg*; do
  [ -f "$f" ] && cp -v "$f" "$OUT/fw/" >> "$LOG"
done
for f in "$OUT"/fw/*.zst; do [ -f "$f" ] && zstd -dq "$f"; done
for f in "$OUT"/fw/*.xz;  do [ -f "$f" ] && xz -dk "$f"; done
cp /sys/firmware/acpi/tables/NHLT "$OUT/fw/NHLT.bin" 2>/dev/null
command -v alsatplg >/dev/null && for t in "$OUT"/fw/*.tplg; do
  [ -f "$t" ] && alsatplg -d "$t" -o "$t.conf" >> "$LOG" 2>&1; done
grep -iE 'sof|snd|dmic|nhlt' <(dmesg) > "$OUT/dmesg-before.txt"
echo on > /sys/bus/pci/devices/$PCI/power/control 2>/dev/null
bardump "0-running"

# ---------- 2. stop audio users & unload SOF ----------
log "Stopping PipeWire/PulseAudio"
for d in /run/user/*; do
  u=$(stat -c %U "$d"); id=$(basename "$d")
  sudo -u "$u" XDG_RUNTIME_DIR="$d" systemctl --user stop \
    pipewire.socket pipewire-pulse.socket pipewire pipewire-pulse wireplumber 2>/dev/null
done
sleep 1; fuser -k /dev/snd/* 2>/dev/null; sleep 1

MODS="snd_soc_skl_hda_dsp snd_sof_probes snd_sof_pci_intel_tgl snd_sof_pci_intel_cnl snd_sof_intel_hda_generic \
snd_sof_intel_hda_common snd_sof_intel_hda_sdw_bpt snd_sof_intel_hda snd_sof_intel_hda_mlink snd_sof_pci snd_sof_xtensa_dsp snd_sof"
log "Unloading SOF stack"
for m in $MODS; do modprobe -r "$m" >> "$LOG" 2>&1; done
if lsmod | grep -q '^snd_sof '; then
  log "ERROR: could not unload snd_sof (still in use)."
  log "Fallback: reboot the live USB with kernel parameters:"
  log "  snd_sof.sof_debug=0xa00 snd_sof.dyndbg=+p snd_sof_intel_hda_common.dyndbg=+p snd_sof_pci.dyndbg=+p"
  log "and re-run this script with NORELOAD=1"
  [ "${NORELOAD:-0}" = 1 ] || exit 2
fi
sleep 1
# no dump while unloaded: controller may be in D3

# ---------- 3. reload with full IPC payload logging + mmiotrace ----------
# sof_debug: BIT(9) print IPC success logs, BIT(11) dump IPC message payload
cat > /etc/modprobe.d/zz-tgldmic-trace.conf <<'EOF'
options snd_sof sof_debug=0xa00 dyndbg=+p
options snd_sof_intel_hda_common dyndbg=+p
options snd_sof_intel_hda_generic dyndbg=+p
options snd_sof_intel_hda dyndbg=+p
options snd_sof_pci dyndbg=+p
options snd_sof_pci_intel_tgl dyndbg=+p
options snd_sof_pci_intel_cnl dyndbg=+p
options snd_soc_skl_hda_dsp dyndbg=+p
EOF
dmesg -C
MMIO=0
if [ "${MMIOTRACE:-0}" = 1 ] && grep -qw mmiotrace $TR/available_tracers 2>/dev/null; then
  log "Enabling mmiotrace (CPUs except one go offline temporarily)"
  echo 131072 > $TR/buffer_size_kb
  echo mmiotrace > $TR/current_tracer && MMIO=1
  cat $TR/trace_pipe > "$OUT/trace/mmiotrace.txt" &
  TRPID=$!
else
  log "mmiotrace off (enable with MMIOTRACE=1 as a separate run; it can freeze the system)"
fi
log "Loading SOF stack"
echo on > /sys/bus/pci/devices/$PCI/power/control 2>/dev/null
modprobe snd_sof_pci_intel_cnl >> "$LOG" 2>&1 || modprobe snd_sof_pci_intel_tgl >> "$LOG" 2>&1
modprobe snd_soc_skl_hda_dsp >> "$LOG" 2>&1
for i in $(seq 1 30); do grep -q sof /proc/asound/cards 2>/dev/null && break; sleep 1; done
sleep 3
dmesg > "$OUT/dmesg-boot-dsp.txt"; sync
bardump "2-dsp-idle"

# ---------- 4. capture from DMIC ----------
cat /proc/asound/cards > "$OUT/pcm/cards.txt"
arecord -l > "$OUT/pcm/arecord-l.txt" 2>&1
amixer -c0 contents > "$OUT/pcm/amixer-contents.txt" 2>&1
alsactl -f "$OUT/pcm/asound.state" store 0 2>/dev/null
# Only PCM 6 "DMIC" (48 kHz, pipeline 10) is needed. ALLDMIC=1 also tries PCM 7 (16 kHz).
PAT='DMIC [(]'; [ "${ALLDMIC:-0}" = 1 ] && PAT='DMIC'
DEVS=$(arecord -l | awk -F'[ :,]+' -v p="$PAT" '$0 ~ p {print $2","$6}')
log "DMIC PCMs: $(echo $DEVS)"
for dev in $DEVS; do
  card=${dev%,*}; pcm=${dev#*,}
  P="/proc/asound/card$card/pcm${pcm}c/sub0"
  arecord -D hw:$card,$pcm --dump-hw-params -d 1 /dev/null > "$OUT/pcm/hwparams-$pcm.txt" 2>&1
  RATE=$(awk '/^RATE/ {gsub(/[\[\]]/,""); print $2; exit}' "$OUT/pcm/hwparams-$pcm.txt"); RATE=${RATE:-48000}
  CH=$(awk '/^CHANNELS/ {gsub(/[\[\]]/,""); print $NF; exit}' "$OUT/pcm/hwparams-$pcm.txt"); CH=${CH:-4}
  log "pcm$pcm supports rate=$RATE channels<=$CH"
  for fmt in S32_LE S16_LE; do
    dmesg -C
    log "Recording hw:$card,$pcm $fmt ch=$CH rate=$RATE for 6 s (speak / tap the mics!)"
    arecord -D hw:$card,$pcm -f $fmt -c $CH -r $RATE -d 6 "$OUT/pcm/dmic-$pcm-$fmt-${CH}ch-$RATE.wav" >> "$LOG" 2>&1 &
    APID=$!; sleep 3
    for x in hw_params sw_params status; do cat "$P/$x" > "$OUT/pcm/$pcm-$fmt-$x.txt" 2>/dev/null; done
    wait $APID; RC=$?
    dmesg > "$OUT/dmesg-capture-pcm$pcm-$fmt.txt"; sync
    log "  arecord rc=$RC, kernel log saved"
    [ $RC = 0 ] && break
  done
done

# ---------- 5. stop tracing, collect debugfs ----------
if [ $MMIO = 1 ]; then
  echo nop > $TR/current_tracer; sleep 1; kill $TRPID 2>/dev/null
fi
if [ "${DEBUGFS:-0}" = 1 ] && [ -d /sys/kernel/debug/sof ]; then  # off: reading DSP windows after capture froze the reference machine
  for f in /sys/kernel/debug/sof/*; do
    n=$(basename "$f")
    case "$n" in trace|*flood*|*inject*|probe*) continue;; esac
    [ -f "$f" ] && timeout 3 cat "$f" > "$OUT/debugfs/$n" 2>/dev/null
  done
fi
rm -f /etc/modprobe.d/zz-tgldmic-trace.conf

# ---------- 6. quick signal check ----------
python3 - "$OUT/pcm" >> "$OUT/pcm/levels.txt" 2>&1 <<'PY'
import wave, struct, glob, sys, math
for f in sorted(glob.glob(sys.argv[1] + "/*.wav")):
    try:
        w = wave.open(f); ch = w.getnchannels(); sw = w.getsampwidth(); n = w.getnframes()
        raw = w.readframes(n); fmt = {2: 'h', 4: 'i'}[sw]
        s = struct.unpack('<%d%s' % (len(raw)//sw, fmt), raw)
        full = float(1 << (8*sw - 1))
        out = []
        for c in range(ch):
            v = s[c::ch]
            rms = math.sqrt(sum(x*x for x in v) / max(len(v), 1)) / full
            out.append("ch%d %.1f dBFS" % (c, 20*math.log10(rms) if rms > 0 else -999))
        print(f.split('/')[-1], ch, "ch", w.getframerate(), "Hz :", ", ".join(out))
    except Exception as e:
        print(f, "ERR", e)
PY
cat "$OUT/pcm/levels.txt" | tee -a "$LOG"
log "Restarting PipeWire"
for d in /run/user/*; do
  u=$(stat -c %U "$d")
  sudo -u "$u" XDG_RUNTIME_DIR="$d" systemctl --user start pipewire.socket pipewire-pulse.socket wireplumber 2>/dev/null
done
# Privacy: the recording may contain voices from the room. Levels are kept in levels.txt;
# the WAV only goes into the tarball with KEEPWAV=1.
if [ "${KEEPWAV:-0}" != 1 ]; then
  mkdir -p "$OUT.private" && mv "$OUT"/pcm/*.wav "$OUT.private/" 2>/dev/null && \
    log "Recording kept out of the tarball: $OUT.private/ (KEEPWAV=1 to include)"
fi
cd "$(dirname "$OUT")" && tar czf "$OUT.tar.gz" "$(basename "$OUT")" && log "Done: $OUT.tar.gz"

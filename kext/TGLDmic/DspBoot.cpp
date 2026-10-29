// Phase 2: boot the cAVS 2.5 audio DSP with SOF firmware next to AppleHDA.
// Sequence mirrors Linux sound/soc/sof/intel/hda-loader.c (cl_dsp_init + hda_cl_copy_fw),
// with two deliberate differences:
//   * no stream / IPC interrupts are enabled (the MSI belongs to AppleHDA) - everything is polled;
//   * a spare output stream (highest index) is used for the code loader instead of output 0.
#include "TGLDmic.hpp"
#include "Regs.hpp"
#include <IOKit/IOLib.h>

extern "C" const unsigned char kSofTglRi[];
extern "C" const unsigned int kSofTglRiSize;

#define LOG(fmt, ...) logLine(fmt, ##__VA_ARGS__)

namespace {
constexpr uint32_t PCI_PGCTL = 0x44, PCI_PGCTL_ADSPPGD = 1u << 2;
constexpr uint32_t PCI_CGCTL = 0x48, PCI_CGCTL_ADSPDCGE = 1u << 1;
constexpr uint32_t HDA_EM2 = 0x1030, EM2_L1SEN = 1u << 13;
constexpr uint32_t SD_STS = 0x03, SD_STS_MASK = 0x1C;                // BCIS | FIFOE | DESE
constexpr uint32_t SD_CTL_INT_MASK = 0x1C;                           // IOCE | FEIE | DEIE
constexpr uint32_t SD_TAG_MASK = 0xFu << 20;
constexpr uint16_t CL_FORMAT = 0x40;
constexpr uint32_t ROM_IPC_CONTROL = 0x01000000, ROM_IPC_PURGE_FW = 0x00004000;
constexpr uint32_t FSR_MASK = 0x00FFFFFF, FSR_INIT_DONE = 0x1, FSR_FW_ENTERED = 0x5;
constexpr uint32_t CORE0 = 1;                                         // init_core_mask
inline uint32_t SPA(uint32_t m) { return m << 16; }
inline uint32_t CPA(uint32_t m) { return m << 24; }
inline uint32_t CRST(uint32_t m) { return m; }
inline uint32_t CSTALL(uint32_t m) { return m << 8; }
constexpr uint32_t XMAN_MAGIC = 0x6e614d58;                           // "XMan"
constexpr uint32_t VS_D0I3C = 0x104A, D0I3C_I3 = 1u << 2;            // 8-bit
constexpr uint32_t SPB_SPBFCCTL = 0x04, SPB_SPIB_BASE = 0x08, SPB_SPIB_STRIDE = 0x08;  // in SPB capability
constexpr uint32_t VS_DPIB_BASE = 0x1084, VS_DPIB_STRIDE = 0x20;     // DMA position in decoupled mode
}

void TGLDmic::fail(const char *what, uint32_t value) {
    snprintf(bootResult, sizeof(bootResult), "FAIL %s (0x%08x)", what, value);
    LOG("boot: %s", bootResult);
    bootState = 2;
}

bool TGLDmic::pollHda(uint32_t off, uint32_t mask, uint32_t want, uint32_t timeoutUs, uint32_t *last) {
    uint32_t v = 0;
    for (uint32_t t = 0; t <= timeoutUs; t += 10) {
        v = r32(off);
        if (v == 0xFFFFFFFF) break;                     // controller gone (D3/reset)
        if ((v & mask) == want) { if (last) *last = v; return true; }
        IODelay(10);
    }
    if (last) *last = v;
    return false;
}

bool TGLDmic::pollDsp(uint32_t off, uint32_t mask, uint32_t want, uint32_t timeoutUs, uint32_t *last) {
    uint32_t v = 0;
    for (uint32_t t = 0; t <= timeoutUs; t += 100) {
        v = d32(off);
        if (v == 0xFFFFFFFF) break;
        if ((v & mask) == want) { if (last) *last = v; return true; }
        IODelay(100);
    }
    if (last) *last = v;
    return false;
}

void TGLDmic::gating(bool enable) {
    if (!enable) {
        savedCgctl = pci->configRead32(PCI_CGCTL);
        savedPgctl = pci->configRead32(PCI_PGCTL);
        savedEm2 = r32(HDA_EM2);
        pci->configWrite32(PCI_CGCTL, savedCgctl & ~PCI_CGCTL_ADSPDCGE);
        w32(HDA_EM2, savedEm2 & ~EM2_L1SEN);
        pci->configWrite32(PCI_PGCTL, savedPgctl | PCI_PGCTL_ADSPPGD);
        LOG("boot: clock/power gating off (CGCTL 0x%08x PGCTL 0x%08x EM2 0x%08x)", savedCgctl, savedPgctl, savedEm2);
    } else {
        pci->configWrite32(PCI_PGCTL, savedPgctl);
        w32(HDA_EM2, savedEm2);
        pci->configWrite32(PCI_CGCTL, savedCgctl);
        LOG("boot: clock/power gating restored");
    }
}

bool TGLDmic::streamStopReset(uint32_t sd) {
    w32(sd, r32(sd) & ~(HDA::SD_CTL_RUN | SD_CTL_INT_MASK));
    if (!pollHda(sd, HDA::SD_CTL_RUN, 0, 300)) return false;
    w8(sd + SD_STS, SD_STS_MASK);                                      // W1C
    w32(sd, r32(sd) | HDA::SD_CTL_SRST);
    if (!pollHda(sd, HDA::SD_CTL_SRST, HDA::SD_CTL_SRST, 300)) return false;
    w32(sd, r32(sd) & ~HDA::SD_CTL_SRST);
    return pollHda(sd, HDA::SD_CTL_SRST, 0, 300);
}

bool TGLDmic::prepareLoaderStream(uint32_t idx, uint32_t tag, size_t fwBytes) {
    uint32_t sd = sdOffset(idx);
    // decouple this stream so its DMA goes to the DSP, not the HDA link
    w32(ppOffset + HDA::PP_PPCTL, r32(ppOffset + HDA::PP_PPCTL) | (1u << idx));
    if (!streamStopReset(sd)) { fail("loader stream reset", r32(sd)); return false; }
    w32(sd + HDA::SD_BDPL, 0); w32(sd + HDA::SD_BDPU, 0);

    // Two BDL entries (HDA requires LVI >= 1): fw split in halves, no IOC
    uint32_t half = (uint32_t)(fwBytes / 2) & ~0x7Fu;
    struct Bdle { uint32_t lo, hi, len, ioc; };
    auto bdl = reinterpret_cast<Bdle *>(bdlBuf.virt);
    bdl[0] = { (uint32_t)clBuf.iova, (uint32_t)(clBuf.iova >> 32), half, 0 };
    bdl[1] = { (uint32_t)(clBuf.iova + half), (uint32_t)((clBuf.iova + half) >> 32), (uint32_t)fwBytes - half, 0 };
    bdlBuf.sync(); clBuf.sync();

    w32(sd, (r32(sd) & ~SD_TAG_MASK) | (tag << HDA::SD_CTL_TAG_SHIFT));
    w32(sd + HDA::SD_CBL, (uint32_t)fwBytes);
    w16(sd + HDA::SD_FMT, CL_FORMAT);
    w16(sd + HDA::SD_LVI, 1);
    w32(sd + HDA::SD_BDPL, (uint32_t)bdlBuf.iova);
    w32(sd + HDA::SD_BDPU, (uint32_t)(bdlBuf.iova >> 32));
    // Linux hda_data_stream_prepare(): enable SPIB for the loader stream with the full size.
    // With SPIB enabled and SPIB = 0 the DMA engine fetches nothing.
    if (spbOffset) {
        uint32_t spib = spbOffset + SPB_SPIB_BASE + SPB_SPIB_STRIDE * idx;
        uint32_t ctl0 = r32(spbOffset + SPB_SPBFCCTL), val0 = r32(spib);
        w32(spbOffset + SPB_SPBFCCTL, ctl0 | (1u << idx));
        w32(spib, (uint32_t)fwBytes);
        LOG("boot: SPIB: SPBFCCTL 0x%08x -> 0x%08x, SPIB[%u] %u -> %u, MAXFIFO %u", ctl0,
            r32(spbOffset + SPB_SPBFCCTL), idx, val0, r32(spib), r32(spib + 4));
    } else {
        LOG("boot: no SPB capability");
    }
    LOG("boot: PPCTL 0x%08x (PROCEN bit %u %s)", r32(ppOffset + HDA::PP_PPCTL), idx,
        (r32(ppOffset + HDA::PP_PPCTL) & (1u << idx)) ? "set" : "NOT set");
    LOG("boot: loader stream %u (sd 0x%x) tag %u, fw %zu bytes @iova 0x%llx, bdl @0x%llx",
        idx, sd, tag, fwBytes, (unsigned long long)clBuf.iova, (unsigned long long)bdlBuf.iova);
    return true;
}

void TGLDmic::releaseLoaderStream(uint32_t idx) {
    uint32_t sd = sdOffset(idx);
    if (spbOffset) {
        w32(spbOffset + SPB_SPIB_BASE + SPB_SPIB_STRIDE * idx, 0);
        w32(spbOffset + SPB_SPBFCCTL, r32(spbOffset + SPB_SPBFCCTL) & ~(1u << idx));
    }
    streamStopReset(sd);
    w32(sd + HDA::SD_BDPL, 0); w32(sd + HDA::SD_BDPU, 0);
    w32(sd, r32(sd) & ~SD_TAG_MASK);
    w32(ppOffset + HDA::PP_PPCTL, r32(ppOffset + HDA::PP_PPCTL) & ~(1u << idx));
}

bool TGLDmic::corePowerUp() {
    dw32(DSP::ADSPCS, d32(DSP::ADSPCS) | SPA(CORE0));
    uint32_t v;
    if (!pollDsp(DSP::ADSPCS, CPA(CORE0), CPA(CORE0), 50000, &v)) { fail("core0 power up (CPA)", v); return false; }
    LOG("boot: core0 powered, ADSPCS 0x%08x", v);
    return true;
}

bool TGLDmic::coreRun() {
    dw32(DSP::ADSPCS, d32(DSP::ADSPCS) & ~CRST(CORE0));
    uint32_t v;
    if (!pollDsp(DSP::ADSPCS, CRST(CORE0), 0, 50000, &v)) { fail("core0 leave reset", v); return false; }
    dw32(DSP::ADSPCS, d32(DSP::ADSPCS) & ~CSTALL(CORE0));
    v = d32(DSP::ADSPCS);
    if ((v & (CPA(CORE0) | SPA(CORE0) | CRST(CORE0) | CSTALL(CORE0))) != (CPA(CORE0) | SPA(CORE0))) {
        fail("core0 not running", v); return false;
    }
    LOG("boot: core0 running, ADSPCS 0x%08x", v);
    return true;
}

void TGLDmic::corePowerDown() {
    if (!dsp) return;
    dw32(DSP::ADSPCS, d32(DSP::ADSPCS) | CSTALL(CORE0));
    dw32(DSP::ADSPCS, d32(DSP::ADSPCS) | CRST(CORE0));
    pollDsp(DSP::ADSPCS, CRST(CORE0), CRST(CORE0), 50000);
    dw32(DSP::ADSPCS, d32(DSP::ADSPCS) & ~SPA(CORE0));
    uint32_t v = 0;
    pollDsp(DSP::ADSPCS, CPA(CORE0), 0, 50000, &v);
    LOG("boot: core0 powered down, ADSPCS 0x%08x", v);
}

bool TGLDmic::bootDsp() {
    bootState = 0;
    LOG("boot: === phase 2 DSP boot attempt ===");
    if (!ready || !memoryDecoded() || !inD0()) { fail("controller not ready", 0); return false; }
    if (!ppOffset) { fail("no PP capability", 0); return false; }
    if (!dsp) {
        bar4 = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress4);
        if (!bar4) { fail("map BAR4", 0); return false; }
        dsp = reinterpret_cast<volatile uint8_t *>(bar4->getVirtualAddress());
    }
    uint32_t adspcs = d32(DSP::ADSPCS);
    LOG("boot: ADSPCS 0x%08x PPCTL 0x%08x before boot", adspcs, r32(ppOffset + HDA::PP_PPCTL));
    if (adspcs == 0xFFFFFFFF) { fail("ADSPCS unreadable", adspcs); return false; }
    if (adspcs & CPA(CORE0)) { LOG("boot: core0 already powered - resetting first"); corePowerDown(); }

    // Firmware: skip the SOF extended manifest ("XMan" header, full_size at +4)
    const uint32_t *w = reinterpret_cast<const uint32_t *>(kSofTglRi);
    size_t skip = (w[0] == XMAN_MAGIC) ? w[1] : 0;
    if (skip >= kSofTglRiSize) { fail("bad firmware manifest", skip); return false; }
    size_t fwBytes = kSofTglRiSize - skip;
    LOG("boot: firmware %u bytes, payload %zu bytes after %zu-byte ext manifest", kSofTglRiSize, fwBytes, skip);

    // Spare output stream: highest output descriptor (AppleHDA starts from output 0)
    uint32_t outIdx = numOut - 1, idx = numIn + outIdx, tag = outIdx + 1;
    if (everRunning & (1u << idx)) { fail("spare output stream in use by AppleHDA", idx); return false; }

    if (!clBuf.alloc(pci, fwBytes) || !bdlBuf.alloc(pci, 4096)) { fail("DMA alloc (<4GB contiguous)", 0); return false; }
    memcpy(clBuf.virt, kSofTglRi + skip, fwBytes);
    {
        IOMapper *m = IOMapper::copyMapperForDevice(pci);
        LOG("boot: DMA mapper %s, fw phys 0x%llx -> iova 0x%llx", m ? m->getMetaClass()->getClassName() : "none",
            (unsigned long long)clBuf.mem->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone),
            (unsigned long long)clBuf.iova);
        OSSafeReleaseNULL(m);
    }

    // DMA needs PCI bus mastering; AppleHDA may only enable it while it plays.
    uint16_t cmd = pci->configRead16(kIOPCIConfigCommand);
    bool forcedBme = false;
    if (!(cmd & kIOPCICommandBusMaster)) {
        pci->setBusMasterEnable(true); forcedBme = true;
        LOG("boot: bus master was OFF (CMD 0x%04x) - enabled for the boot", cmd);
    }
    uint8_t d0i3 = r8(VS_D0I3C);
    if (d0i3 & D0I3C_I3) {
        w8(VS_D0I3C, d0i3 & ~D0I3C_I3);
        LOG("boot: controller was in D0i3 (D0I3C 0x%02x) - cleared", d0i3);
    }
    gating(false);
    bool ok = false;
    uint32_t v = 0;
    do {
        if (!prepareLoaderStream(idx, tag, fwBytes)) break;
        // cl_dsp_init step 1..4
        if (!corePowerUp()) break;
        dw32(DSP::HIPCIDR, DSP::IPC_BUSY | ROM_IPC_CONTROL | ROM_IPC_PURGE_FW | ((tag - 1) << 9));
        if (!coreRun()) break;
        if (!pollDsp(DSP::HIPCIDA, DSP::IPC_DONE, DSP::IPC_DONE, 500000, &v)) { fail("ROM IPC ack (HIPCIDA)", v); break; }
        dw32(DSP::HIPCIDA, d32(DSP::HIPCIDA) | DSP::IPC_DONE);               // W1C
        if (!pollDsp(DSP::ROM_STATUS, FSR_MASK, FSR_INIT_DONE, 300000, &v)) { fail("ROM init done", v); break; }
        LOG("boot: ROM init done, FSR 0x%08x", v);
        // hda_cl_copy_fw: run the stream, wait for FW_ENTERED, stop
        uint32_t sd = sdOffset(idx), dpib = VS_DPIB_BASE + VS_DPIB_STRIDE * idx;
        LOG("boot: pre-run CMD 0x%04x INTCTL 0x%08x SD CTL 0x%08x CBL %u LVI %u FMT 0x%04x BDP 0x%08x%08x",
            pci->configRead16(kIOPCIConfigCommand), r32(HDA::INTCTL), r32(sd), r32(sd + HDA::SD_CBL),
            r16(sd + HDA::SD_LVI), r16(sd + HDA::SD_FMT), r32(sd + HDA::SD_BDPU), r32(sd + HDA::SD_BDPL));
        // Same bits Linux sets (RUN + IOCE/FEIE/DEIE); INTCTL stays untouched so no MSI reaches AppleHDA.
        w32(sd, r32(sd) | HDA::SD_CTL_RUN | SD_CTL_INT_MASK);
        // Trace FSR state/module/halted changes until FW_ENTERED, HALTED or 3 s. The wait field toggles
        // (wait 5 = DMA buffer full) every ~32 KB during the load: count those, don't log them.
        uint32_t lastFsr = d32(DSP::ROM_STATUS), t = 0, waitToggles = 0;
        LOG("boot: t=0us FSR 0x%08x", lastFsr);
        while (t < 3000000) {
            IODelay(20); t += 20;
            uint32_t f = d32(DSP::ROM_STATUS);
            if (f == lastFsr) continue;
            bool onlyWait = ((f ^ lastFsr) & ~0x0F000000u) == 0;
            lastFsr = f;
            if (onlyWait) { waitToggles++; continue; }
            if (logRing && logRing->getCount() < 120)
                LOG("boot: t=%uus FSR 0x%08x (mod %u wait %u state 0x%x%s) DPIB %u, %u wait toggles", t, f, (f >> 28) & 7,
                    (f >> 24) & 0xF, f & FSR_MASK, (f & 0x80000000u) ? " HALTED" : "", r32(dpib), waitToggles);
            if ((f & FSR_MASK) == FSR_FW_ENTERED || (f & 0x80000000u)) break;
        }
        LOG("boot: trace end t=%uus FSR 0x%08x DPIB %u, %u wait toggles", t, lastFsr, r32(dpib), waitToggles);
        bool entered = pollDsp(DSP::ROM_STATUS, FSR_MASK, FSR_FW_ENTERED, 1000, &v);
        uint32_t lpib = r32(sd + HDA::SD_LPIB), dp = r32(dpib), ctl = r32(sd);
        w32(sd, r32(sd) & ~(HDA::SD_CTL_RUN | SD_CTL_INT_MASK));
        if (!entered) {
            fail("FW_ENTERED", v);
            // ROM error code (HDA_DSP_SRAM_REG_ROM_ERROR): 40 CSE error, 41 CSE wrong response,
            // 42 IMR too small, 43 base FW not found, 44 CSE validation failed, 45 IPC fatal ...
            LOG("boot: ROM error code %u (0x%08x)", d32(DSP::ROM_STATUS + 4), d32(DSP::ROM_STATUS + 4));
            LOG("boot: SRAM 0x80000: %08x %08x %08x %08x %08x %08x %08x %08x",
                d32(0x80000), d32(0x80004), d32(0x80008), d32(0x8000C), d32(0x80010), d32(0x80014), d32(0x80018), d32(0x8001C));
            LOG("boot: HIPCTDR 0x%08x HIPCTDD 0x%08x HIPCIDR 0x%08x HIPCIDA 0x%08x ADSPCS 0x%08x",
                d32(DSP::HIPCTDR), d32(DSP::HIPCTDD), d32(DSP::HIPCIDR), d32(DSP::HIPCIDA), d32(DSP::ADSPCS));
            LOG("boot: loader CTL 0x%08x LPIB %u DPIB %u of %zu, INTSTS 0x%08x, GCTL 0x%08x", ctl, lpib, dp, fwBytes,
                r32(HDA::INTSTS), r32(HDA::GCTL));
            break;
        }
        LOG("boot: code loader FW_ENTERED, FSR 0x%08x, LPIB %u DPIB %u", v, lpib, dp);
        // FW_READY arrives as a DSP->host IPC in HIPCTDR
        if (!pollDsp(DSP::HIPCTDR, DSP::IPC_BUSY, DSP::IPC_BUSY, 1000000, &v)) { fail("FW_READY (HIPCTDR)", v); break; }
        uint32_t msg = v & 0x7FFFFFFF;
        uint32_t hdr0 = d32(DSP::MBOX_UP), hdr1 = d32(DSP::MBOX_UP + 4);
        LOG("boot: DSP message 0x%08x, mailbox hdr size %u cmd 0x%08x", msg, hdr0, hdr1);
        dw32(DSP::HIPCTDR, d32(DSP::HIPCTDR) | DSP::IPC_BUSY);              // W1C: accept
        dw32(DSP::HIPCTDA, d32(DSP::HIPCTDA) | DSP::IPC_DONE);              // done
        if (msg != DSP::FW_READY) { fail("unexpected first DSP message", msg); break; }
        ok = true;
    } while (false);

    releaseLoaderStream(idx);
    clBuf.release(); bdlBuf.release();
    if (ok) {
        snprintf(bootResult, sizeof(bootResult), "OK FW_READY");
        bootState = 1;
        LOG("boot: SUCCESS - SOF firmware running (FW_READY)");
        if (!keepRunning) corePowerDown();
    } else {
        corePowerDown();
    }
    gating(true);
    if (forcedBme && !keepRunning) pci->setBusMasterEnable(false);
    if (d0i3 & D0I3C_I3) w8(VS_D0I3C, d0i3);
    LOG("boot: PPCTL 0x%08x after boot, AppleHDA streams untouched", r32(ppOffset + HDA::PP_PPCTL));
    return ok;
}

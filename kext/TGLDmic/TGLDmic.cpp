#include "TGLDmic.hpp"
#include "Regs.hpp"
#include <IOKit/IOLib.h>
#include <libkern/version.h>

#define super IOService
OSDefineMetaClassAndStructors(TGLDmic, IOService)

#define LOG(fmt, ...) IOLog("TGLDmic: " fmt "\n", ##__VA_ARGS__)

static constexpr uint32_t kPollMs = 2000;      // stream observation interval
static constexpr uint32_t kPollCount = 90;     // 3 minutes, then stop polling

static bool bootArg(const char *name) {
    char buf[16];
    return PE_parse_boot_argn(name, buf, sizeof(buf));
}

IOService *TGLDmic::probe(IOService *provider, SInt32 *score) {
    if (bootArg("-tgldmicoff")) return nullptr;
    auto dev = OSDynamicCast(IOPCIDevice, provider);
    if (!dev) return nullptr;
    // Physical ID check (OpenCore may spoof device-id for AppleHDA).
    uint32_t id = dev->configRead32(kIOPCIConfigVendorID);
    if (id != 0xA0C88086) {
        LOG("probe: unexpected PCI id %08x, not attaching", id);
        return nullptr;
    }
    return super::probe(provider, score);
}

bool TGLDmic::inD0() {
    uint8_t pm = 0;
    if (!pci->findPCICapability(kIOPCIPowerManagementCapability, &pm)) return true;
    return (pci->configRead16(pm + 4) & 0x3) == 0;   // PMCSR.PowerState
}

bool TGLDmic::start(IOService *provider) {
    if (!super::start(provider)) return false;
    pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) return false;
    pci->retain();
    readDsp = bootArg("-tgldmicdsp");

    LOG("phase 1 observer v0.1 on %s (Darwin %d.%d), dsp-reads=%s",
        provider->getName(), version_major, version_minor, readDsp ? "on" : "off");

    bar0 = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!bar0) { LOG("cannot map BAR0"); return false; }
    hda = reinterpret_cast<volatile uint8_t *>(bar0->getVirtualAddress());
    LOG("BAR0 phys 0x%llx len 0x%llx", (unsigned long long)bar0->getPhysicalAddress(),
        (unsigned long long)bar0->getLength());

    if (readDsp) {
        bar4 = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress4);
        if (bar4) {
            dsp = reinterpret_cast<volatile uint8_t *>(bar4->getVirtualAddress());
            LOG("BAR4 phys 0x%llx len 0x%llx", (unsigned long long)bar4->getPhysicalAddress(),
                (unsigned long long)bar4->getLength());
        } else LOG("cannot map BAR4");
    }

    if (!inD0()) {
        LOG("controller not in D0 at start; deferring reads to timer");
    } else {
        uint16_t gcap = r16(HDA::GCAP);
        numOut = (gcap >> 12) & 0xF; numIn = (gcap >> 8) & 0xF; numBidi = (gcap >> 3) & 0x1F;
        LOG("GCAP 0x%04x: %u in, %u out, %u bidi; HDA %u.%u; GCTL 0x%08x STATESTS 0x%04x",
            gcap, numIn, numOut, numBidi, r8(HDA::VMAJ), r8(HDA::VMIN), r32(HDA::GCTL), r16(HDA::STATESTS));
        walkCapabilities();
        snapshot(true);
    }

    workLoop = getWorkLoop();
    if (workLoop) {
        workLoop->retain();
        timer = IOTimerEventSource::timerEventSource(this, timerFired);
        if (timer && workLoop->addEventSource(timer) == kIOReturnSuccess)
            timer->setTimeoutMS(kPollMs);
    }
    publish();
    registerService();
    return true;
}

void TGLDmic::walkCapabilities() {
    uint32_t off = r32(HDA::LLCH) & HDA::CAP_NEXT_MASK;
    for (int n = 0; off && off < bar0->getLength() && n < 10; n++) {
        uint32_t hdr = r32(off);
        uint32_t id = (hdr & HDA::CAP_ID_MASK) >> HDA::CAP_ID_SHIFT;
        switch (id) {
            case HDA::CAP_PP:   ppOffset = off; break;
            case HDA::CAP_ML:   mlOffset = off; break;
            case HDA::CAP_SPB:  spbOffset = off; break;
            case HDA::CAP_DRSM: drsmOffset = off; break;
        }
        LOG("ext cap @0x%04x id %u hdr 0x%08x", off, id, hdr);
        off = hdr & HDA::CAP_NEXT_MASK;
    }
    if (ppOffset)
        LOG("PP: PPCH 0x%08x PPCTL 0x%08x PPSTS 0x%08x", r32(ppOffset + HDA::PP_PPCH),
            r32(ppOffset + HDA::PP_PPCTL), r32(ppOffset + HDA::PP_PPSTS));
}

void TGLDmic::snapshot(bool verbose) {
    uint32_t total = numIn + numOut + numBidi;
    if (total > 30) total = 30;
    for (uint32_t i = 0; i < total; i++) {
        uint32_t base = HDA::SD_BASE + i * HDA::SD_STRIDE;
        uint32_t ctl = r32(base + HDA::SD_CTL);
        uint32_t tag = (ctl >> HDA::SD_CTL_TAG_SHIFT) & 0xF;
        bool run = ctl & HDA::SD_CTL_RUN;
        if (tag) tagsSeen[i] = tag;
        if (run && !(everRunning & (1u << i))) {
            everRunning |= 1u << i;
            LOG("stream %u (%s%u) started: CTL 0x%08x tag %u FMT 0x%04x CBL %u LVI %u",
                i, i < numIn ? "in" : "out", i < numIn ? i : i - numIn, ctl, tag,
                r16(base + HDA::SD_FMT), r32(base + HDA::SD_CBL), r16(base + HDA::SD_LVI));
        } else if (verbose) {
            LOG("stream %u (%s%u): CTL 0x%08x tag %u", i, i < numIn ? "in" : "out",
                i < numIn ? i : i - numIn, ctl, tag);
        }
    }
    if (dsp && verbose)
        LOG("DSP: ADSPCS 0x%08x ADSPIS 0x%08x HIPCIDR 0x%08x HIPCIDA 0x%08x",
            d32(DSP::ADSPCS), d32(DSP::ADSPIS), d32(DSP::HIPCIDR), d32(DSP::HIPCIDA));
}

void TGLDmic::publish() {
    setProperty("TGLDmicPhase", 1ULL, 32);
    setProperty("StreamsIn", numIn, 32);
    setProperty("StreamsOut", numOut, 32);
    setProperty("UsedStreamMask", everRunning, 32);
    setProperty("PPOffset", ppOffset, 32);
    setProperty("MLOffset", mlOffset, 32);
    setProperty("Samples", samples, 32);
    if (ppOffset && hda) setProperty("PPCTL", r32(ppOffset + HDA::PP_PPCTL), 32);
    if (dsp) setProperty("ADSPCS", d32(DSP::ADSPCS), 32);
    // Suggested spare descriptors for the DSP path: highest input and highest output not used by AppleHDA.
    int spareIn = -1, spareOut = -1;
    for (int i = (int)numIn - 1; i >= 0; i--) if (!(everRunning & (1u << i))) { spareIn = i; break; }
    for (int i = (int)numOut - 1; i >= 0; i--) if (!(everRunning & (1u << (numIn + i)))) { spareOut = i; break; }
    setProperty("SpareInputStream", (unsigned long long)(spareIn < 0 ? 0xFF : spareIn), 32);
    setProperty("SpareOutputStream", (unsigned long long)(spareOut < 0 ? 0xFF : spareOut), 32);
}

void TGLDmic::timerFired(OSObject *owner, IOTimerEventSource *sender) {
    auto self = OSDynamicCast(TGLDmic, owner);
    if (!self) return;
    if (self->inD0()) {
        if (!self->numIn && !self->numOut) {      // deferred initial read
            uint16_t gcap = self->r16(HDA::GCAP);
            self->numOut = (gcap >> 12) & 0xF; self->numIn = (gcap >> 8) & 0xF; self->numBidi = (gcap >> 3) & 0x1F;
            self->walkCapabilities();
        }
        self->snapshot(false);
    }
    self->samples++;
    self->publish();
    if (self->samples < kPollCount) sender->setTimeoutMS(kPollMs);
    else LOG("observation finished: used stream mask 0x%08x", self->everRunning);
}

void TGLDmic::stop(IOService *provider) {
    if (timer) {
        timer->cancelTimeout();
        if (workLoop) workLoop->removeEventSource(timer);
        OSSafeReleaseNULL(timer);
    }
    OSSafeReleaseNULL(workLoop);
    OSSafeReleaseNULL(bar4);
    OSSafeReleaseNULL(bar0);
    hda = dsp = nullptr;
    super::stop(provider);
}

void TGLDmic::free() {
    OSSafeReleaseNULL(pci);
    super::free();
}

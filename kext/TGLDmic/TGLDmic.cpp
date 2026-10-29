#include "TGLDmic.hpp"
#include "Regs.hpp"
#include <IOKit/IOLib.h>
#include <libkern/version.h>

#define super IOService
OSDefineMetaClassAndStructors(TGLDmic, IOService)

#include <stdarg.h>
#define LOG(fmt, ...) logLine(fmt, ##__VA_ARGS__)
static constexpr unsigned kLogLines = 128;

static constexpr uint32_t kPollMs = 2000;      // stream observation interval
static constexpr uint32_t kPollCount = 90;     // 3 minutes, then stop polling

static bool bootArg(const char *name) {
    char buf[16];
    return PE_parse_boot_argn(name, buf, sizeof(buf));
}

static IOPMPowerState gPowerStates[2] = {
    { kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { kIOPMPowerStateVersion1, kIOPMPowerOn | kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
};

IOReturn TGLDmic::setPowerState(unsigned long state, IOService *) {
    unsigned long old = pmState;
    pmState = state;
    if (state == 0 && old == 1 && gate && (streaming || setupDone)) gate->runAction(gatedPowerLost);
    return kIOPMAckImplied;
}

IOService *TGLDmic::probe(IOService *provider, SInt32 *score) {
    if (bootArg("-tgldmicoff")) return nullptr;
    auto dev = OSDynamicCast(IOPCIDevice, provider);
    if (!dev) return nullptr;
    // Physical ID check (OpenCore may spoof device-id for AppleHDA).
    uint32_t id = dev->configRead32(kIOPCIConfigVendorID);
    if (id != 0xA0C88086) {
        IOLog("TGLDmic: probe: unexpected PCI id %08x, not attaching\n", id);
        return nullptr;
    }
    return super::probe(provider, score);
}

void TGLDmic::logLine(const char *fmt, ...) {
    char buf[200];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    IOLog("TGLDmic: %s\n", buf);
    if (!logRing) logRing = OSArray::withCapacity(kLogLines);
    if (!logRing) return;
    if (logRing->getCount() >= kLogLines) logRing->removeObject(0);
    if (auto s = OSString::withCString(buf)) { logRing->setObject(s); s->release(); }
}

bool TGLDmic::memoryDecoded() {
    return pci->configRead16(kIOPCIConfigCommand) & kIOPCICommandMemorySpace;
}

// Only trust BAR0 once memory decoding is on, the device is in D0 and GCAP looks sane.
bool TGLDmic::tryInit() {
    if (ready) return true;
    if (!memoryDecoded() || !inD0()) return false;
    uint16_t gcap = r16(HDA::GCAP);
    if (gcap == 0xFFFF || gcap == 0) { badReads++; return false; }
    numOut = (gcap >> 12) & 0xF; numIn = (gcap >> 8) & 0xF; numBidi = (gcap >> 3) & 0x1F;
    LOG("GCAP 0x%04x: %u in, %u out, %u bidi; HDA %u.%u; GCTL 0x%08x STATESTS 0x%04x (after %u bad reads)",
        gcap, numIn, numOut, numBidi, r8(HDA::VMAJ), r8(HDA::VMIN), r32(HDA::GCTL), r16(HDA::STATESTS), badReads);
    walkCapabilities();
    ready = true;
    snapshot(true);
    return true;
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
    captureRequested = bootArg("-tgldmiccap");
    serviceMode = bootArg("-tgldmic") && !captureRequested;
    bootRequested = bootArg("-tgldmicboot") || captureRequested || serviceMode;
    if (serviceMode) { keepRunning = false; bootDelaySec = 10; }   // boot-time boot = self-test, then power down
    uint32_t delay = 0;
    if (PE_parse_boot_argn("tgldmicdelay", &delay, sizeof(delay)) && delay >= 10 && delay <= 170) bootDelaySec = delay;
    keepRunning = bootArg("-tgldmickeep");

    LOG("TGLDmic v0.4.4 on %s (Darwin %d.%d), dsp-reads=%s, dsp-boot=%s%s",
        provider->getName(), version_major, version_minor, readDsp ? "on" : "off",
        bootRequested ? "ON" : "off", keepRunning ? " (keep running)" : "");
    if (bootRequested) LOG("DSP boot%s scheduled %u s after the controller is ready%s", captureRequested ? " + capture" : "",
                           bootDelaySec, serviceMode ? " (service mode: DSP stays up, mic exposed to Core Audio)" : "");

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

    LOG("at start: PCI CMD 0x%04x, D0=%d", pci->configRead16(kIOPCIConfigCommand), inD0());
    if (!tryInit()) LOG("controller not decoded yet (AppleHDA not started?) - retrying from timer");

    if (serviceMode) {
        PMinit();
        provider->joinPMtree(this);
        if (registerPowerDriver(this, gPowerStates, 2) == kIOReturnSuccess) {
            pmJoined = true;
            changePowerStateTo(0);          // idle: don't keep the controller awake
        } else {
            PMstop();
            LOG("PM: registerPowerDriver failed - running without power holds");
        }
    }
    workLoop = IOWorkLoop::workLoop();      // own loop: boot polling must not block the provider's
    if (workLoop) {
        gate = IOCommandGate::commandGate(this);
        if (gate && workLoop->addEventSource(gate) != kIOReturnSuccess) OSSafeReleaseNULL(gate);
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
    // Decoupled descriptors (PPCTL PROCEN) feed the DSP: those are TGLDmic's own streams, never the
    // HDA driver's link streams. Also skip our capture descriptor while we own it.
    uint32_t ours = ppOffset ? (r32(ppOffset + HDA::PP_PPCTL) & 0x3FFFFFFF) : 0;
    if (ours == 0x3FFFFFFF) ours = 0;                    // all-ones read: ignore
    if (streaming || setupDone) ours |= 1u << 6;
    for (uint32_t i = 0; i < total; i++) {
        if (ours & (1u << i)) continue;
        uint32_t base = HDA::SD_BASE + i * HDA::SD_STRIDE;
        uint32_t ctl = r32(base + HDA::SD_CTL);
        if (ctl == 0xFFFFFFFF) { badReads++; return; }   // controller went away (D3/reset): skip this sample
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
    setProperty("TGLDmicPhase", serviceMode ? 4ULL : captureRequested ? 3ULL : bootRequested ? 2ULL : 1ULL, 32);
    if (serviceMode) {
        setProperty("ServiceClients", startCount, 32);
        setProperty("Streaming", streaming);
        setProperty("StreamGeneration", generation, 64);
        setProperty("PowerHold", pmState == 1);
    }
    setProperty("StreamsIn", numIn, 32);
    setProperty("StreamsOut", numOut, 32);
    setProperty("UsedStreamMask", everRunning, 32);
    setProperty("PPOffset", ppOffset, 32);
    setProperty("MLOffset", mlOffset, 32);
    setProperty("Samples", samples, 32);
    setProperty("Ready", ready);
    setProperty("BadReads", badReads, 32);
    setProperty("BootState", bootState, 32);
    if (bootResult[0]) setProperty("BootResult", bootResult);
    if (captureRequested) {
        setProperty("CaptureState", captureState, 32);
        if (captureResult[0]) setProperty("CaptureResult", captureResult);
        setProperty("CaptureBytes", capBytes, 64);
        if (captureState) {
            if (auto d = OSData::withBytes(chPeak, sizeof(chPeak))) { setProperty("CapturePeak", d); d->release(); }
            if (auto d = OSData::withBytes(chMeanAbs, sizeof(chMeanAbs))) { setProperty("CaptureMeanAbs", d); d->release(); }
        }
        if (capSnap && capSnap->getLength()) setProperty("CaptureSnapshot", capSnap);
        if (envLen) if (auto d = OSData::withBytes(env, envLen * 4)) { setProperty("CaptureEnvelope", d); d->release(); }
    }
    if (logRing) { if (auto copy = OSArray::withArray(logRing)) { setProperty("Log", copy); copy->release(); } }
    if (ready && ppOffset && hda) setProperty("PPCTL", r32(ppOffset + HDA::PP_PPCTL), 32);
    if (ready && dsp) setProperty("ADSPCS", d32(DSP::ADSPCS), 32);
    if (!ready) return;
    // Suggested spare descriptors for the DSP path: highest input and highest output not used by AppleHDA.
    int spareIn = -1, spareOut = -1;
    for (int i = (int)numIn - 1; i >= 0; i--) if (!(everRunning & (1u << i))) { spareIn = i; break; }
    for (int i = (int)numOut - 1; i >= 0; i--) if (!(everRunning & (1u << (numIn + i)))) { spareOut = i; break; }
    setProperty("SpareInputStream", (unsigned long long)(spareIn < 0 ? 0xFF : spareIn), 32);
    setProperty("SpareOutputStream", (unsigned long long)(spareOut < 0 ? 0xFF : spareOut), 32);
}

// After the observation window: every 10 s, log changes of PCI power state, GCTL and ADSPCS,
// so we can see when (and how) the DSP gets powered down while idle. Never touches BARs in D3.
void TGLDmic::watchdog() {
    wdTicks++;
    uint8_t pmCap = 0;
    uint16_t pm = pci->findPCICapability(kIOPCIPowerManagementCapability, &pmCap) ? (pci->configRead16(pmCap + 4) & 3) : 0;
    uint32_t gctl = 0, cs = 0;
    if (pm == 0 && memoryDecoded()) { gctl = r32(HDA::GCTL); if (dsp) cs = d32(DSP::ADSPCS); }
    if (pm != wdPm || gctl != wdGctl || cs != wdAdspcs)
        LOG("wd: t=%us D%u GCTL 0x%08x ADSPCS 0x%08x streaming %d clients %u", 180 + wdTicks * 10, pm, gctl, cs,
            streaming, startCount);
    wdPm = pm; wdGctl = gctl; wdAdspcs = cs;
}

void TGLDmic::timerFired(OSObject *owner, IOTimerEventSource *sender) {
    auto self = OSDynamicCast(TGLDmic, owner);
    if (!self) return;
    if (self->tryInit() && self->memoryDecoded() && self->inD0())
        self->snapshot(false);
    // Phase 2: one boot attempt ~20 s after the controller became usable (AppleHDA settled)
    if (self->bootRequested && self->ready && self->bootState == 0 && self->samples * (kPollMs / 1000) >= self->bootDelaySec) {
        self->bootDsp();
        self->publish();
    }
    self->samples++;
    self->publish();
    if (self->samples < kPollCount) sender->setTimeoutMS(kPollMs);
    else {
        if (self->samples == kPollCount) self->logLine("observation finished: used stream mask 0x%08x", self->everRunning);
        if (self->serviceMode) { self->watchdog(); self->publish(); sender->setTimeoutMS(10000); }
        else self->publish();
    }
}

IOReturn TGLDmic::gatedShutdown(OSObject *o, void *, void *, void *, void *) {
    auto self = OSDynamicCast(TGLDmic, o);
    if (!self) return kIOReturnBadArgument;
    if (self->streaming || self->setupDone) self->pipelineStop();
    self->startCount = 0;
    if (self->bootState == 1 && self->keepRunning) self->corePowerDown();
    self->gating(true);
    self->releaseCaptureBuffers();
    return kIOReturnSuccess;
}

void TGLDmic::stop(IOService *provider) {
    if (gate) gate->runAction(gatedShutdown); else gatedShutdown(this, nullptr, nullptr, nullptr, nullptr);
    if (pmJoined) { PMstop(); pmJoined = false; }
    if (gate) { if (workLoop) workLoop->removeEventSource(gate); OSSafeReleaseNULL(gate); }
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
    OSSafeReleaseNULL(capSnap);
    OSSafeReleaseNULL(logRing);
    OSSafeReleaseNULL(pci);
    super::free();
}

#pragma once
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOMemoryDescriptor.h>
#include "DmaBuffer.hpp"

// Phase 1: read-only observer. Matches the HDA controller next to AppleHDA (own
// IOMatchCategory, provider is never opened), maps BAR0 and records which stream
// descriptors AppleHDA uses. Nothing is written to the hardware.
class TGLDmic : public IOService {
    OSDeclareDefaultStructors(TGLDmic)
public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;

private:
    IOPCIDevice *pci {nullptr};
    IOMemoryMap *bar0 {nullptr};
    IOMemoryMap *bar4 {nullptr};
    volatile uint8_t *hda {nullptr};
    volatile uint8_t *dsp {nullptr};
    IOWorkLoop *workLoop {nullptr};
    IOTimerEventSource *timer {nullptr};

    uint32_t numIn {0}, numOut {0}, numBidi {0};
    uint32_t ppOffset {0}, mlOffset {0}, spbOffset {0}, drsmOffset {0};
    uint32_t everRunning {0};        // bitmask: stream descriptors seen with RUN set
    uint32_t tagsSeen[32] {};        // last non-zero stream tag per descriptor
    uint32_t samples {0};
    bool readDsp {false};
    bool ready {false};              // controller decoded + GCAP sane
    uint32_t badReads {0};
    OSArray *logRing {nullptr};      // last log lines, published as "Log" (IOLog may not reach `log show`)

    bool memoryDecoded();
    bool tryInit();
    void logLine(const char *fmt, ...) __attribute__((format(printf, 2, 3)));

    bool inD0();
    uint32_t r32(uint32_t off) { return *reinterpret_cast<volatile uint32_t *>(hda + off); }
    uint16_t r16(uint32_t off) { return *reinterpret_cast<volatile uint16_t *>(hda + off); }
    uint8_t  r8(uint32_t off)  { return hda[off]; }
    uint32_t d32(uint32_t off) { return *reinterpret_cast<volatile uint32_t *>(dsp + off); }

    void walkCapabilities();
    void snapshot(bool verbose);
    void publish();
    static void timerFired(OSObject *owner, IOTimerEventSource *sender);

    // ---- Phase 2: DSP boot (only with -tgldmicboot) ----
    bool bootRequested {false};
    bool keepRunning {false};
    uint32_t bootState {0};           // 0 idle, 1 done ok, 2 failed
    char bootResult[96] {};
    uint32_t savedCgctl {0}, savedPgctl {0}, savedEm2 {0};
    DmaBuffer clBuf, bdlBuf;

    void w32(uint32_t off, uint32_t v) { *reinterpret_cast<volatile uint32_t *>(hda + off) = v; }
    void w16(uint32_t off, uint16_t v) { *reinterpret_cast<volatile uint16_t *>(hda + off) = v; }
    void w8(uint32_t off, uint8_t v)   { hda[off] = v; }
    void dw32(uint32_t off, uint32_t v) { *reinterpret_cast<volatile uint32_t *>(dsp + off) = v; }
    bool pollHda(uint32_t off, uint32_t mask, uint32_t want, uint32_t timeoutUs, uint32_t *last = nullptr);
    bool pollDsp(uint32_t off, uint32_t mask, uint32_t want, uint32_t timeoutUs, uint32_t *last = nullptr);

    uint32_t sdOffset(uint32_t globalIndex) { return 0x80 + 0x20 * globalIndex; }
    bool streamStopReset(uint32_t sd);
    bool prepareLoaderStream(uint32_t globalIndex, uint32_t tag, size_t fwBytes);
    void releaseLoaderStream(uint32_t globalIndex);
    void gating(bool enable);
    bool corePowerUp();
    bool coreRun();
    void corePowerDown();
    bool bootDsp();
    void fail(const char *what, uint32_t value);
};

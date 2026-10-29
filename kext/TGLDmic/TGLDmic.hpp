#pragma once
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOCommandGate.h>
#include "DmaBuffer.hpp"

struct IpcMsg;   // IpcReplay.hpp

// Phase 1: read-only observer. Matches the HDA controller next to AppleHDA (own
// IOMatchCategory, provider is never opened), maps BAR0 and records which stream
// descriptors AppleHDA uses. Nothing is written to the hardware.
class TGLDmic : public IOService {
    OSDeclareDefaultStructors(TGLDmic)
    friend class TGLDmicUserClient;
public:
    // ---- Phase 4 service API (called by TGLDmicUserClient; all serialized on the work loop) ----
    static constexpr uint32_t kRingBytes = 0x10000, kFrameBytes = 16, kRate = 48000, kChannels = 4;
    IOReturn clientStart();                       // refcounted: first start boots DSP if needed + starts pipeline
    IOReturn clientStop();
    IOReturn readPosition(uint64_t *dpibBytes, uint64_t *hostTime, uint64_t *generation);
    IOMemoryDescriptor *ringMemory();             // retained; nullptr if not allocated
    uint32_t stateBits();                         // bit0 dsp up, bit1 streaming, bit2 service mode
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

    // ---- Phase 3: DMIC capture test (only with -tgldmiccap, implies boot) ----
    bool captureRequested {false};
    uint32_t bootDelaySec {20};      // boot arg tgldmicdelay=<s>, 10..170
    uint32_t captureState {0};        // 0 idle, 1 ok, 2 failed
    char captureResult[96] {};
    DmaBuffer ringBuf, ptBuf, capBdl;
    OSData *capSnap {nullptr};         // 1 s of raw S32_LE 4ch 48 kHz audio, published as "CaptureSnapshot"
    uint32_t chPeak[4] {}, chMeanAbs[4] {};
    uint64_t capBytes {0};
    static constexpr uint32_t kEnvMax = 100;
    uint32_t env[kEnvMax] {};        // per-100 ms mean |x| of ch0
    uint32_t envLen {0};

    bool ipcSend(const uint32_t *msg, uint32_t words, uint32_t *reply = nullptr, uint32_t replyWords = 0);
    void ackDspNotification();
    bool replay(const IpcMsg *list, uint32_t count, const char *phase);
    bool prepareCaptureStream(uint32_t idx, uint32_t tag);
    bool ensureCaptureBuffers();
    void releaseCaptureBuffers();
    bool pipelineStart();
    void pipelineStop();
    bool dspAlive();
    bool captureTest();

    // ---- Phase 4 service state ----
    bool serviceMode {false};
    bool streaming {false};
    bool setupDone {false}, triggered {false};
    bool gatingOff {false};
    uint32_t startCount {0};
    uint64_t generation {0};
    IOCommandGate *gate {nullptr};
    static IOReturn gatedStart(OSObject *o, void *, void *, void *, void *);
    static IOReturn gatedStop(OSObject *o, void *, void *, void *, void *);
    static IOReturn gatedEnsureBuffers(OSObject *o, void *, void *, void *, void *);
    static IOReturn gatedShutdown(OSObject *o, void *, void *, void *, void *);
    void capFail(const char *what, uint32_t value);
    void fail(const char *what, uint32_t value);
};

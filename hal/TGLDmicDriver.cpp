// TGLDmic.driver - Core Audio AudioServerPlugIn exposing the Tiger Lake DSP internal DMIC.
//
// Data path: TGLDmic.kext runs the SOF DSP pipeline; the DSP DMAs S32_LE 4ch 48 kHz frames into a
// 64 KB ring (4096 frames). This plug-in maps that ring read-only through TGLDmicUserClient, polls
// the DMA write position, derives zero time stamps from it (so Core Audio follows the DSP clock) and
// converts frames to stereo Float32 (ch0 = left, ch1 = right) with a software input gain.
//
// Structure follows Apple's NullAudio AudioServerPlugIn sample: one plug-in object, one device,
// one input stream, one input volume control.

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <atomic>
#include <math.h>
#include <string.h>
#include <os/log.h>
#include "../kext/TGLDmic/TGLDmicShared.h"

#define kPlugIn_BundleID   "com.labrouss.TGLDmic.driver"
#define kDevice_Name       "Internal Microphone (TGLDmic)"
#define kDevice_Manufacturer "TGLDmic"
#define kDevice_UID        "TGLDmic_InternalMic_UID"
#define kDevice_ModelUID   "TGLDmic_InternalMic_ModelUID"

enum {
    kObjectID_PlugIn = kAudioObjectPlugInObject,
    kObjectID_Device = 2,
    kObjectID_Stream_Input = 3,
    kObjectID_Volume_Input_Master = 4,
};

static const AudioObjectPropertyElement kElementMain = 0;   // kAudioObjectPropertyElementMain (macOS 12 SDK name)
static const Float64 kSampleRate = 48000.0;
static const UInt32 kChannels = 2;                       // exposed to Core Audio
static const UInt32 kRingFrames = 0x10000 / 16;          // 4096 frames in the kernel ring
static const UInt32 kZeroTimeStampPeriod = kRingFrames;
static const UInt32 kSafetyOffset = 256;
static const UInt32 kLatency = 48;                       // ~1 ms DSP period
static const Float32 kVolumeMinDB = 0.0f, kVolumeMaxDB = 36.0f, kVolumeDefaultDB = 24.0f;

static os_log_t gLog;
#define LOG(...) os_log(gLog, __VA_ARGS__)

// ------------------------------------------------------------------------------------------------
// State
// ------------------------------------------------------------------------------------------------
static pthread_mutex_t gStateMutex = PTHREAD_MUTEX_INITIALIZER;
static AudioServerPlugInHostRef gHost = nullptr;
static UInt32 gRefCount = 0;
static UInt64 gIORunning = 0;
static std::atomic<Float32> gVolumeDB { kVolumeDefaultDB };
static std::atomic<Float32> gGain { 15.85f };            // 10^(24/20)
static int gChannelMode = 0;                             // 0: ch0/1, 1: ch2/3, 2: 4ch blend

// kernel connection
static io_connect_t gConn = IO_OBJECT_NULL;
static const int32_t *gRing = nullptr;                    // S32_LE x 4ch interleaved
static mach_vm_size_t gRingSize = 0;

// time base (IO thread only)
static Float64 gTicksPerFrame = 0;
static UInt64 gTotalFrames = 0, gLastHost = 0, gGeneration = 0;
static UInt32 gLastPosFrames = 0;
static UInt64 gTimeStampSeed = 1;

// Zero time stamps: least-squares fit of DMA frame count vs host time over the last ~1.3 s.
// DPIB advances in ~1 ms chunks; fitting removes that quantization (simulated: 230 Hz -> ~16 Hz
// period-to-period rate jitter, <1 ppm mean error). Each period's stamp is cached so repeated calls
// for the same zero sample return the same host time.
static const int kFitN = 128;
static UInt64 gFitHost[kFitN];
static Float64 gFitFrames[kFitN];
static int gFitCount = 0, gFitHead = 0;
static UInt64 gZeroSample = UINT64_MAX, gZeroHost = 0;

static void fitReset() { gFitCount = 0; gFitHead = 0; gZeroSample = UINT64_MAX; gZeroHost = 0; }

static void fitAdd(UInt64 host, Float64 frames) {
    gFitHost[gFitHead] = host; gFitFrames[gFitHead] = frames;
    gFitHead = (gFitHead + 1) % kFitN;
    if (gFitCount < kFitN) gFitCount++;
}

static bool fitHostForSample(Float64 sample, UInt64 *outHost) {
    if (gFitCount < 16) return false;
    int first = (gFitHead - gFitCount + kFitN) % kFitN;
    const UInt64 h0 = gFitHost[first];
    Float64 mx = 0, my = 0;
    for (int i = 0; i < gFitCount; i++) { int k = (first + i) % kFitN; mx += (Float64)(gFitHost[k] - h0); my += gFitFrames[k]; }
    mx /= gFitCount; my /= gFitCount;
    Float64 sxx = 0, sxy = 0;
    for (int i = 0; i < gFitCount; i++) {
        int k = (first + i) % kFitN;
        Float64 dx = (Float64)(gFitHost[k] - h0) - mx;
        sxx += dx * dx; sxy += dx * (gFitFrames[k] - my);
    }
    if (sxx <= 0) return false;
    Float64 b = sxy / sxx;                                  // frames per tick
    if (b <= 0.5 / gTicksPerFrame || b >= 2.0 / gTicksPerFrame) return false;   // implausible
    Float64 x = (sample - (my - b * mx)) / b;               // ticks after h0 (may be negative)
    Float64 h = (Float64)h0 + x;
    if (h <= 0) return false;
    *outHost = (UInt64)h;
    return true;
}

// ---- property-call tracing (first kTraceMax calls) ----
static std::atomic<int> gTraceCount { 0 };
static const int kTraceMax = 0;    // set to e.g. 400 to log the HAL's first property calls (v0.4.1 debugging)
static void fourcc(UInt32 v, char out[5]) {
    for (int i = 0; i < 4; i++) { char ch = (char)((v >> (24 - 8 * i)) & 0xFF); out[i] = (ch >= 32 && ch < 127) ? ch : '?'; }
    out[4] = 0;
}
static void trace(const char *fn, AudioObjectID obj, const AudioObjectPropertyAddress *a, UInt32 size, long result) {
    if (gTraceCount.fetch_add(1) >= kTraceMax || !a) return;
    char sel[5], scope[5]; fourcc(a->mSelector, sel); fourcc(a->mScope, scope);
    LOG("TGLDmic: %{public}s obj %u '%{public}s' scope '%{public}s' el %u -> size %u result %ld", fn, obj, sel, scope,
        a->mElement, size, result);
}

static Float32 dbToGain(Float32 db) { return powf(10.0f, db / 20.0f); }
static Float32 dbToScalar(Float32 db) { return (db - kVolumeMinDB) / (kVolumeMaxDB - kVolumeMinDB); }
static Float32 scalarToDB(Float32 s) {
    if (s < 0) s = 0;
    if (s > 1) s = 1;
    return kVolumeMinDB + s * (kVolumeMaxDB - kVolumeMinDB);
}

static bool openKernel() {
    if (gConn != IO_OBJECT_NULL && gRing) return true;
    io_service_t svc = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching(kTGLDmicServiceClass));
    if (!svc) { LOG("TGLDmic: kext service not found"); return false; }
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), kTGLDmicUserClientType, &gConn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) { LOG("TGLDmic: IOServiceOpen failed 0x%x", kr); gConn = IO_OBJECT_NULL; return false; }
    uint64_t info[6] = {}; uint32_t n = 6;
    kr = IOConnectCallScalarMethod(gConn, kTGLDmicMethodGetInfo, nullptr, 0, info, &n);
    if (kr != KERN_SUCCESS || info[0] != kTGLDmicInterfaceVersion || info[4] != kRingFrames * 16) {
        LOG("TGLDmic: interface mismatch kr=0x%x ver=%llu ring=%llu", kr, info[0], info[4]);
        IOServiceClose(gConn); gConn = IO_OBJECT_NULL; return false;
    }
    mach_vm_address_t addr = 0; mach_vm_size_t size = 0;
    kr = IOConnectMapMemory64(gConn, kTGLDmicRingMemoryType, mach_task_self(), &addr, &size, kIOMapAnywhere);
    if (kr != KERN_SUCCESS || size < kRingFrames * 16) {
        LOG("TGLDmic: map ring failed 0x%x size %llu", kr, size);
        IOServiceClose(gConn); gConn = IO_OBJECT_NULL; return false;
    }
    gRing = reinterpret_cast<const int32_t *>(addr); gRingSize = size;
    LOG("TGLDmic: connected, state 0x%llx, ring %llu bytes", info[1], size);
    return true;
}

__attribute__((unused)) static void closeKernel() {
    if (gConn != IO_OBJECT_NULL) {
        if (gRing) IOConnectUnmapMemory64(gConn, kTGLDmicRingMemoryType, mach_task_self(), (mach_vm_address_t)gRing);
        IOServiceClose(gConn);
    }
    gConn = IO_OBJECT_NULL; gRing = nullptr; gRingSize = 0;
}

// Advance gTotalFrames from the DMA write position. Robust against missed ring wraps by checking the
// elapsed host time (the ring is only 85 ms long).
static bool updatePosition() {
    uint64_t out[3] = {}; uint32_t n = 3;
    if (IOConnectCallScalarMethod(gConn, kTGLDmicMethodGetPosition, nullptr, 0, out, &n) != KERN_SUCCESS) return false;
    UInt32 pos = (UInt32)(out[0] / 16) % kRingFrames;
    UInt64 host = out[1];
    if (out[2] != gGeneration || gLastHost == 0) {
        gGeneration = out[2]; gTotalFrames = pos; gLastPosFrames = pos; gLastHost = host;
        gTimeStampSeed++;
        fitReset();
        fitAdd(host, (Float64)gTotalFrames + 24.0);
        return true;
    }
    UInt64 delta = (pos + kRingFrames - gLastPosFrames) % kRingFrames;
    Float64 expected = (Float64)(host - gLastHost) / gTicksPerFrame;
    if (expected > kRingFrames * 0.75) {
        Float64 wraps = floor((expected - (Float64)delta) / kRingFrames + 0.5);
        if (wraps > 0) delta += (UInt64)wraps * kRingFrames;
    }
    gTotalFrames += delta; gLastPosFrames = pos; gLastHost = host;
    fitAdd(host, (Float64)gTotalFrames + 24.0);       // DPIB is a lower bound: true position ~half a chunk ahead
    return true;
}

// ------------------------------------------------------------------------------------------------
// Forward declarations of the interface
// ------------------------------------------------------------------------------------------------
static HRESULT QueryInterface(void *, REFIID, LPVOID *);
static ULONG AddRef(void *);
static ULONG Release(void *);
static OSStatus Initialize(AudioServerPlugInDriverRef, AudioServerPlugInHostRef);
static OSStatus CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo *, AudioObjectID *);
static OSStatus DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID);
static OSStatus AddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
static OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
static OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
static Boolean HasProperty(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *);
static OSStatus IsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, Boolean *);
static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32 *);
static OSStatus GetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32, UInt32 *, void *);
static OSStatus SetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32, const void *);
static OSStatus StartIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus StopIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64 *, UInt64 *, UInt64 *);
static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, Boolean *, Boolean *);
static OSStatus BeginIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *);
static OSStatus DoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *, void *, void *);
static OSStatus EndIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *);

static AudioServerPlugInDriverInterface gInterface = {
    nullptr, QueryInterface, AddRef, Release, Initialize, CreateDevice, DestroyDevice, AddDeviceClient,
    RemoveDeviceClient, PerformDeviceConfigurationChange, AbortDeviceConfigurationChange, HasProperty,
    IsPropertySettable, GetPropertyDataSize, GetPropertyData, SetPropertyData, StartIO, StopIO, GetZeroTimeStamp,
    WillDoIOOperation, BeginIOOperation, DoIOOperation, EndIOOperation
};
static AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef gDriverRef = &gInterfacePtr;

// ------------------------------------------------------------------------------------------------
// Factory and IUnknown
// ------------------------------------------------------------------------------------------------
extern "C" void *TGLDmic_Create(CFAllocatorRef, CFUUIDRef requestedTypeUUID) {
    if (CFEqual(requestedTypeUUID, kAudioServerPlugInTypeUUID)) return gDriverRef;
    return nullptr;
}

static HRESULT QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface) {
    if (inDriver != gDriverRef || !outInterface) return kAudioHardwareBadObjectError;
    CFUUIDRef req = CFUUIDCreateFromUUIDBytes(nullptr, inUUID);
    HRESULT r = E_NOINTERFACE;
    if (CFEqual(req, IUnknownUUID) || CFEqual(req, kAudioServerPlugInDriverInterfaceUUID)) {
        pthread_mutex_lock(&gStateMutex); ++gRefCount; pthread_mutex_unlock(&gStateMutex);
        *outInterface = gDriverRef; r = S_OK;
    }
    CFRelease(req);
    return r;
}

static ULONG AddRef(void *inDriver) {
    if (inDriver != gDriverRef) return 0;
    pthread_mutex_lock(&gStateMutex); ULONG r = ++gRefCount; pthread_mutex_unlock(&gStateMutex);
    return r;
}

static ULONG Release(void *inDriver) {
    if (inDriver != gDriverRef) return 0;
    pthread_mutex_lock(&gStateMutex); ULONG r = gRefCount ? --gRefCount : 0; pthread_mutex_unlock(&gStateMutex);
    return r;
}

static OSStatus Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost) {
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    gHost = inHost;
    gLog = os_log_create(kPlugIn_BundleID, "driver");
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    gTicksPerFrame = (1e9 * (Float64)tb.denom / (Float64)tb.numer) / kSampleRate;
    LOG("TGLDmic: plug-in initialized, ticks/frame %f", gTicksPerFrame);
    return kAudioHardwareNoError;
}

static OSStatus CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo *, AudioObjectID *) { return kAudioHardwareUnsupportedOperationError; }
static OSStatus DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID) { return kAudioHardwareUnsupportedOperationError; }
static OSStatus AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, const AudioServerPlugInClientInfo *) {
    return (inDriver == gDriverRef && inDevice == kObjectID_Device) ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}
static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, const AudioServerPlugInClientInfo *) {
    return (inDriver == gDriverRef && inDevice == kObjectID_Device) ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}
static OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *) { return kAudioHardwareNoError; }
static OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *) { return kAudioHardwareNoError; }

// ------------------------------------------------------------------------------------------------
// Properties
// ------------------------------------------------------------------------------------------------
static AudioStreamBasicDescription streamFormat() {
    AudioStreamBasicDescription f = {};
    f.mSampleRate = kSampleRate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = 4 * kChannels;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = 4 * kChannels;
    f.mChannelsPerFrame = kChannels;
    f.mBitsPerChannel = 32;
    return f;
}

static Boolean HasPropertyImpl(AudioServerPlugInDriverRef inDriver, AudioObjectID obj, pid_t, const AudioObjectPropertyAddress *a) {
    if (inDriver != gDriverRef || !a) return false;
    switch (obj) {
    case kObjectID_PlugIn:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyManufacturer: case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList: case kAudioPlugInPropertyTranslateUIDToDevice:
        case kAudioPlugInPropertyResourceBundle:
            return true;
        }
        return false;
    case kObjectID_Device:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer: case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID: case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyRelatedDevices: case kAudioDevicePropertyClockDomain: case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning: case kAudioObjectPropertyControlList:
        case kAudioDevicePropertyNominalSampleRate: case kAudioDevicePropertyAvailableNominalSampleRates:
        case kAudioDevicePropertyIsHidden: case kAudioDevicePropertyZeroTimeStampPeriod: case kAudioDevicePropertyStreams:
        // Answered in every scope, as in Apple's NullAudio: the HAL queries e.g. the output-scope
        // safety offset even on an input-only device, and drops the device if that call fails.
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency: case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyPreferredChannelsForStereo: case kAudioDevicePropertyPreferredChannelLayout:
            return true;
        }
        return false;
    case kObjectID_Stream_Input:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyOwnedObjects: case kAudioStreamPropertyIsActive: case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType: case kAudioStreamPropertyStartingChannel: case kAudioStreamPropertyLatency:
        case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat:
        case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats:
            return true;
        }
        return false;
    case kObjectID_Volume_Input_Master:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyOwnedObjects: case kAudioControlPropertyScope: case kAudioControlPropertyElement:
        case kAudioLevelControlPropertyScalarValue: case kAudioLevelControlPropertyDecibelValue:
        case kAudioLevelControlPropertyDecibelRange: case kAudioLevelControlPropertyConvertScalarToDecibels:
        case kAudioLevelControlPropertyConvertDecibelsToScalar:
            return true;
        }
        return false;
    }
    return false;
}

static OSStatus IsPropertySettableImpl(AudioServerPlugInDriverRef inDriver, AudioObjectID obj, pid_t pid,
                                   const AudioObjectPropertyAddress *a, Boolean *out) {
    if (inDriver != gDriverRef || !a || !out) return kAudioHardwareIllegalOperationError;
    if (!HasPropertyImpl(inDriver, obj, pid, a)) return kAudioHardwareUnknownPropertyError;
    *out = false;
    if (obj == kObjectID_Volume_Input_Master &&
        (a->mSelector == kAudioLevelControlPropertyScalarValue || a->mSelector == kAudioLevelControlPropertyDecibelValue))
        *out = true;
    if (obj == kObjectID_Stream_Input &&
        (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat))
        *out = true;   // accepted only if it equals the single supported format
    if (obj == kObjectID_Device && a->mSelector == kAudioDevicePropertyNominalSampleRate) *out = true;
    return kAudioHardwareNoError;
}

static OSStatus GetPropertyDataSizeImpl(AudioServerPlugInDriverRef inDriver, AudioObjectID obj, pid_t pid,
                                    const AudioObjectPropertyAddress *a, UInt32, const void *, UInt32 *out) {
    if (inDriver != gDriverRef || !a || !out) return kAudioHardwareIllegalOperationError;
    if (!HasPropertyImpl(inDriver, obj, pid, a)) return kAudioHardwareUnknownPropertyError;
    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioControlPropertyScope:
        *out = sizeof(AudioClassID); return 0;
    case kAudioObjectPropertyOwner: case kAudioPlugInPropertyTranslateUIDToDevice:
        *out = sizeof(AudioObjectID); return 0;
    case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer: case kAudioDevicePropertyDeviceUID:
    case kAudioDevicePropertyModelUID: case kAudioPlugInPropertyResourceBundle:
        *out = sizeof(CFStringRef); return 0;
    case kAudioObjectPropertyOwnedObjects:
        *out = (obj == kObjectID_PlugIn ? 1 : (obj == kObjectID_Device && a->mScope != kAudioObjectPropertyScopeOutput) ? 2 : 0)
               * sizeof(AudioObjectID); return 0;
    case kAudioPlugInPropertyDeviceList: case kAudioDevicePropertyRelatedDevices:
        *out = sizeof(AudioObjectID); return 0;
    case kAudioDevicePropertyStreams:
        *out = (a->mScope == kAudioObjectPropertyScopeOutput ? 0 : 1) * sizeof(AudioObjectID); return 0;
    case kAudioObjectPropertyControlList:
        *out = sizeof(AudioObjectID); return 0;
    case kAudioDevicePropertyNominalSampleRate:
        *out = sizeof(Float64); return 0;
    case kAudioDevicePropertyAvailableNominalSampleRates:
        *out = sizeof(AudioValueRange); return 0;
    case kAudioDevicePropertyPreferredChannelsForStereo:
        *out = 2 * sizeof(UInt32); return 0;
    case kAudioDevicePropertyPreferredChannelLayout:
        *out = offsetof(AudioChannelLayout, mChannelDescriptions) + kChannels * sizeof(AudioChannelDescription); return 0;
    case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat:
        *out = sizeof(AudioStreamBasicDescription); return 0;
    case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats:
        *out = sizeof(AudioStreamRangedDescription); return 0;
    case kAudioLevelControlPropertyScalarValue: case kAudioLevelControlPropertyDecibelValue:
    case kAudioLevelControlPropertyConvertScalarToDecibels: case kAudioLevelControlPropertyConvertDecibelsToScalar:
        *out = sizeof(Float32); return 0;
    case kAudioLevelControlPropertyDecibelRange:
        *out = sizeof(AudioValueRange); return 0;
    default:
        *out = sizeof(UInt32); return 0;    // all remaining properties are UInt32
    }
}

#define NEED(sz) do { if (inDataSize < (sz)) return kAudioHardwareBadPropertySizeError; *outDataSize = (sz); } while (0)

static OSStatus GetPropertyDataImpl(AudioServerPlugInDriverRef inDriver, AudioObjectID obj, pid_t pid,
                                const AudioObjectPropertyAddress *a, UInt32 qSize, const void *qData,
                                UInt32 inDataSize, UInt32 *outDataSize, void *outData) {
    if (inDriver != gDriverRef || !a || !outDataSize || !outData) return kAudioHardwareIllegalOperationError;
    if (!HasPropertyImpl(inDriver, obj, pid, a)) return kAudioHardwareUnknownPropertyError;
    auto u32 = static_cast<UInt32 *>(outData);
    auto ids = static_cast<AudioObjectID *>(outData);
    auto str = static_cast<CFStringRef *>(outData);

    if (obj == kObjectID_PlugIn) {
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: NEED(sizeof(AudioClassID)); *u32 = kAudioObjectClassID; return 0;
        case kAudioObjectPropertyClass: NEED(sizeof(AudioClassID)); *u32 = kAudioPlugInClassID; return 0;
        case kAudioObjectPropertyOwner: NEED(sizeof(AudioObjectID)); *ids = kAudioObjectUnknown; return 0;
        case kAudioObjectPropertyManufacturer: NEED(sizeof(CFStringRef)); *str = CFSTR(kDevice_Manufacturer); return 0;
        case kAudioObjectPropertyOwnedObjects: case kAudioPlugInPropertyDeviceList:
            *outDataSize = inDataSize >= sizeof(AudioObjectID) ? sizeof(AudioObjectID) : 0;
            if (*outDataSize) ids[0] = kObjectID_Device;
            return 0;
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            NEED(sizeof(AudioObjectID));
            CFStringRef uid = (qSize == sizeof(CFStringRef) && qData) ? *static_cast<const CFStringRef *>(qData) : nullptr;
            *ids = (uid && CFStringCompare(uid, CFSTR(kDevice_UID), 0) == kCFCompareEqualTo) ? kObjectID_Device : kAudioObjectUnknown;
            return 0;
        }
        case kAudioPlugInPropertyResourceBundle: NEED(sizeof(CFStringRef)); *str = CFSTR(""); return 0;
        }
    } else if (obj == kObjectID_Device) {
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: NEED(sizeof(AudioClassID)); *u32 = kAudioObjectClassID; return 0;
        case kAudioObjectPropertyClass: NEED(sizeof(AudioClassID)); *u32 = kAudioDeviceClassID; return 0;
        case kAudioObjectPropertyOwner: NEED(sizeof(AudioObjectID)); *ids = kObjectID_PlugIn; return 0;
        case kAudioObjectPropertyName: NEED(sizeof(CFStringRef)); *str = CFSTR(kDevice_Name); return 0;
        case kAudioObjectPropertyManufacturer: NEED(sizeof(CFStringRef)); *str = CFSTR(kDevice_Manufacturer); return 0;
        case kAudioDevicePropertyDeviceUID: NEED(sizeof(CFStringRef)); *str = CFSTR(kDevice_UID); return 0;
        case kAudioDevicePropertyModelUID: NEED(sizeof(CFStringRef)); *str = CFSTR(kDevice_ModelUID); return 0;
        case kAudioObjectPropertyOwnedObjects: {
            UInt32 n = 0;
            if (a->mScope != kAudioObjectPropertyScopeOutput) {
                if (inDataSize >= (n + 1) * sizeof(AudioObjectID)) ids[n++] = kObjectID_Stream_Input;
                if (inDataSize >= (n + 1) * sizeof(AudioObjectID)) ids[n++] = kObjectID_Volume_Input_Master;
            }
            *outDataSize = n * sizeof(AudioObjectID); return 0;
        }
        case kAudioDevicePropertyStreams:
            *outDataSize = (a->mScope != kAudioObjectPropertyScopeOutput && inDataSize >= sizeof(AudioObjectID)) ? sizeof(AudioObjectID) : 0;
            if (*outDataSize) ids[0] = kObjectID_Stream_Input;
            return 0;
        case kAudioObjectPropertyControlList:
            *outDataSize = inDataSize >= sizeof(AudioObjectID) ? sizeof(AudioObjectID) : 0;
            if (*outDataSize) ids[0] = kObjectID_Volume_Input_Master;
            return 0;
        case kAudioDevicePropertyTransportType: NEED(sizeof(UInt32)); *u32 = kAudioDeviceTransportTypeBuiltIn; return 0;
        case kAudioDevicePropertyRelatedDevices:
            *outDataSize = inDataSize >= sizeof(AudioObjectID) ? sizeof(AudioObjectID) : 0;
            if (*outDataSize) ids[0] = kObjectID_Device;
            return 0;
        case kAudioDevicePropertyClockDomain: NEED(sizeof(UInt32)); *u32 = 0; return 0;
        case kAudioDevicePropertyDeviceIsAlive: NEED(sizeof(UInt32)); *u32 = 1; return 0;
        case kAudioDevicePropertyDeviceIsRunning:
            NEED(sizeof(UInt32)); pthread_mutex_lock(&gStateMutex); *u32 = gIORunning > 0; pthread_mutex_unlock(&gStateMutex); return 0;
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: NEED(sizeof(UInt32)); *u32 = a->mScope != kAudioObjectPropertyScopeOutput; return 0;
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: NEED(sizeof(UInt32)); *u32 = 0; return 0;
        case kAudioDevicePropertyLatency: NEED(sizeof(UInt32)); *u32 = a->mScope == kAudioObjectPropertyScopeOutput ? 0 : kLatency; return 0;
        case kAudioDevicePropertySafetyOffset: NEED(sizeof(UInt32)); *u32 = a->mScope == kAudioObjectPropertyScopeOutput ? 0 : kSafetyOffset; return 0;
        case kAudioDevicePropertyNominalSampleRate: NEED(sizeof(Float64)); *static_cast<Float64 *>(outData) = kSampleRate; return 0;
        case kAudioDevicePropertyAvailableNominalSampleRates: {
            *outDataSize = inDataSize >= sizeof(AudioValueRange) ? sizeof(AudioValueRange) : 0;
            if (*outDataSize) { auto r = static_cast<AudioValueRange *>(outData); r->mMinimum = r->mMaximum = kSampleRate; }
            return 0;
        }
        case kAudioDevicePropertyIsHidden: NEED(sizeof(UInt32)); *u32 = 0; return 0;
        case kAudioDevicePropertyZeroTimeStampPeriod: NEED(sizeof(UInt32)); *u32 = kZeroTimeStampPeriod; return 0;
        case kAudioDevicePropertyPreferredChannelsForStereo: NEED(2 * sizeof(UInt32)); u32[0] = 1; u32[1] = 2; return 0;
        case kAudioDevicePropertyPreferredChannelLayout: {
            UInt32 sz = offsetof(AudioChannelLayout, mChannelDescriptions) + kChannels * sizeof(AudioChannelDescription);
            NEED(sz);
            auto l = static_cast<AudioChannelLayout *>(outData);
            l->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
            l->mChannelBitmap = 0;
            l->mNumberChannelDescriptions = kChannels;
            for (UInt32 i = 0; i < kChannels; i++) {
                l->mChannelDescriptions[i].mChannelLabel = kAudioChannelLabel_Left + i;
                l->mChannelDescriptions[i].mChannelFlags = 0;
                l->mChannelDescriptions[i].mCoordinates[0] = l->mChannelDescriptions[i].mCoordinates[1] =
                    l->mChannelDescriptions[i].mCoordinates[2] = 0;
            }
            return 0;
        }
        }
    } else if (obj == kObjectID_Stream_Input) {
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: NEED(sizeof(AudioClassID)); *u32 = kAudioObjectClassID; return 0;
        case kAudioObjectPropertyClass: NEED(sizeof(AudioClassID)); *u32 = kAudioStreamClassID; return 0;
        case kAudioObjectPropertyOwner: NEED(sizeof(AudioObjectID)); *ids = kObjectID_Device; return 0;
        case kAudioObjectPropertyOwnedObjects: *outDataSize = 0; return 0;
        case kAudioStreamPropertyIsActive: NEED(sizeof(UInt32)); *u32 = 1; return 0;
        case kAudioStreamPropertyDirection: NEED(sizeof(UInt32)); *u32 = 1; return 0;           // 1 = input
        case kAudioStreamPropertyTerminalType: NEED(sizeof(UInt32)); *u32 = kAudioStreamTerminalTypeMicrophone; return 0;
        case kAudioStreamPropertyStartingChannel: NEED(sizeof(UInt32)); *u32 = 1; return 0;
        case kAudioStreamPropertyLatency: NEED(sizeof(UInt32)); *u32 = 0; return 0;
        case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat:
            NEED(sizeof(AudioStreamBasicDescription)); *static_cast<AudioStreamBasicDescription *>(outData) = streamFormat(); return 0;
        case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats: {
            *outDataSize = inDataSize >= sizeof(AudioStreamRangedDescription) ? sizeof(AudioStreamRangedDescription) : 0;
            if (*outDataSize) {
                auto r = static_cast<AudioStreamRangedDescription *>(outData);
                r->mFormat = streamFormat();
                r->mSampleRateRange.mMinimum = r->mSampleRateRange.mMaximum = kSampleRate;
            }
            return 0;
        }
        }
    } else if (obj == kObjectID_Volume_Input_Master) {
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: NEED(sizeof(AudioClassID)); *u32 = kAudioLevelControlClassID; return 0;
        case kAudioObjectPropertyClass: NEED(sizeof(AudioClassID)); *u32 = kAudioVolumeControlClassID; return 0;
        case kAudioObjectPropertyOwner: NEED(sizeof(AudioObjectID)); *ids = kObjectID_Device; return 0;
        case kAudioObjectPropertyOwnedObjects: *outDataSize = 0; return 0;
        case kAudioControlPropertyScope: NEED(sizeof(AudioObjectPropertyScope)); *u32 = kAudioObjectPropertyScopeInput; return 0;
        case kAudioControlPropertyElement: NEED(sizeof(AudioObjectPropertyElement)); *u32 = kElementMain; return 0;
        case kAudioLevelControlPropertyScalarValue: NEED(sizeof(Float32)); *static_cast<Float32 *>(outData) = dbToScalar(gVolumeDB.load()); return 0;
        case kAudioLevelControlPropertyDecibelValue: NEED(sizeof(Float32)); *static_cast<Float32 *>(outData) = gVolumeDB.load(); return 0;
        case kAudioLevelControlPropertyDecibelRange: {
            NEED(sizeof(AudioValueRange));
            auto r = static_cast<AudioValueRange *>(outData); r->mMinimum = kVolumeMinDB; r->mMaximum = kVolumeMaxDB; return 0;
        }
        case kAudioLevelControlPropertyConvertScalarToDecibels: {
            NEED(sizeof(Float32)); auto v = static_cast<Float32 *>(outData); *v = scalarToDB(*v); return 0;
        }
        case kAudioLevelControlPropertyConvertDecibelsToScalar: {
            NEED(sizeof(Float32)); auto v = static_cast<Float32 *>(outData);
            Float32 db = *v < kVolumeMinDB ? kVolumeMinDB : *v > kVolumeMaxDB ? kVolumeMaxDB : *v;
            *v = dbToScalar(db); return 0;
        }
        }
    }
    return kAudioHardwareUnknownPropertyError;
}


// ---- traced entry points ----
static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a) {
    Boolean r = HasPropertyImpl(d, obj, pid, a);
    trace("Has", obj, a, 0, r);
    return r;
}
static OSStatus IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a, Boolean *out) {
    OSStatus r = IsPropertySettableImpl(d, obj, pid, a, out);
    trace("Settable", obj, a, 0, r);
    return r;
}
static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a,
                                    UInt32 qs, const void *qd, UInt32 *out) {
    OSStatus r = GetPropertyDataSizeImpl(d, obj, pid, a, qs, qd, out);
    trace("Size", obj, a, out ? *out : 0, r);
    return r;
}
static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a,
                                UInt32 qs, const void *qd, UInt32 inSize, UInt32 *outSize, void *out) {
    OSStatus r = GetPropertyDataImpl(d, obj, pid, a, qs, qd, inSize, outSize, out);
    trace("Get", obj, a, outSize ? *outSize : 0, r);
    return r;
}

static OSStatus SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID obj, pid_t pid,
                                const AudioObjectPropertyAddress *a, UInt32, const void *, UInt32 inDataSize, const void *inData) {
    if (inDriver != gDriverRef || !a || !inData) return kAudioHardwareIllegalOperationError;
    if (!HasPropertyImpl(inDriver, obj, pid, a)) return kAudioHardwareUnknownPropertyError;
    if (obj == kObjectID_Volume_Input_Master &&
        (a->mSelector == kAudioLevelControlPropertyScalarValue || a->mSelector == kAudioLevelControlPropertyDecibelValue)) {
        if (inDataSize != sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
        Float32 v = *static_cast<const Float32 *>(inData);
        Float32 db = a->mSelector == kAudioLevelControlPropertyScalarValue ? scalarToDB(v) : v;
        if (db < kVolumeMinDB) db = kVolumeMinDB;
        if (db > kVolumeMaxDB) db = kVolumeMaxDB;
        if (db != gVolumeDB.load()) {
            gVolumeDB.store(db); gGain.store(dbToGain(db));
            AudioObjectPropertyAddress changed[2] = {
                { kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, kElementMain },
                { kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal, kElementMain } };
            if (gHost) gHost->PropertiesChanged(gHost, kObjectID_Volume_Input_Master, 2, changed);
        }
        return kAudioHardwareNoError;
    }
    if (obj == kObjectID_Stream_Input &&
        (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat)) {
        if (inDataSize != sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
        auto f = static_cast<const AudioStreamBasicDescription *>(inData);
        AudioStreamBasicDescription ours = streamFormat();
        if (f->mSampleRate != ours.mSampleRate || f->mChannelsPerFrame != ours.mChannelsPerFrame ||
            f->mFormatID != ours.mFormatID || f->mBitsPerChannel != ours.mBitsPerChannel)
            return kAudioDeviceUnsupportedFormatError;
        return kAudioHardwareNoError;
    }
    if (obj == kObjectID_Device && a->mSelector == kAudioDevicePropertyNominalSampleRate) {
        if (inDataSize != sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        return *static_cast<const Float64 *>(inData) == kSampleRate ? kAudioHardwareNoError : kAudioHardwareIllegalOperationError;
    }
    return kAudioHardwareUnsupportedOperationError;
}

// ------------------------------------------------------------------------------------------------
// IO
// ------------------------------------------------------------------------------------------------
static OSStatus StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, UInt32) {
    if (inDriver != gDriverRef || inDevice != kObjectID_Device) return kAudioHardwareBadObjectError;
    OSStatus r = kAudioHardwareNoError;
    pthread_mutex_lock(&gStateMutex);
    if (gIORunning == 0) {
        CFPropertyListRef pref = CFPreferencesCopyAppValue(CFSTR("ChannelMode"), CFSTR("com.labrouss.TGLDmic"));
        if (pref) {
            if (CFGetTypeID(pref) == CFNumberGetTypeID()) {
                int val = 0;
                if (CFNumberGetValue((CFNumberRef)pref, kCFNumberIntType, &val)) gChannelMode = val;
            }
            CFRelease(pref);
        }
        if (!openKernel()) r = kAudioHardwareNotRunningError;
        else {
            kern_return_t kr = IOConnectCallScalarMethod(gConn, kTGLDmicMethodStart, nullptr, 0, nullptr, nullptr);
            if (kr != KERN_SUCCESS) { LOG("TGLDmic: kernel start failed 0x%x", kr); r = kAudioHardwareNotRunningError; }
            else {
                gLastHost = 0; gGeneration = 0;
                if (!updatePosition()) { LOG("TGLDmic: no DMA position"); r = kAudioHardwareNotRunningError; }
            }
        }
    }
    if (r == kAudioHardwareNoError) gIORunning++;
    pthread_mutex_unlock(&gStateMutex);
    if (r == kAudioHardwareNoError) LOG("TGLDmic: IO started (%llu clients, channelMode %d)", gIORunning, gChannelMode);
    return r;
}

static OSStatus StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, UInt32) {
    if (inDriver != gDriverRef || inDevice != kObjectID_Device) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gStateMutex);
    if (gIORunning > 0 && --gIORunning == 0 && gConn != IO_OBJECT_NULL) {
        IOConnectCallScalarMethod(gConn, kTGLDmicMethodStop, nullptr, 0, nullptr, nullptr);
        LOG("TGLDmic: IO stopped");
    }
    pthread_mutex_unlock(&gStateMutex);
    return kAudioHardwareNoError;
}

static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, UInt32,
                                 Float64 *outSampleTime, UInt64 *outHostTime, UInt64 *outSeed) {
    if (inDriver != gDriverRef || inDevice != kObjectID_Device) return kAudioHardwareBadObjectError;
    if (gConn == IO_OBJECT_NULL || !updatePosition()) return kAudioHardwareNotRunningError;
    UInt64 period = kZeroTimeStampPeriod;
    UInt64 zeroSample = (gTotalFrames / period) * period;
    if (zeroSample != gZeroSample) {
        UInt64 h = 0;
        if (!fitHostForSample((Float64)zeroSample, &h)) {
            UInt64 back = (UInt64)((Float64)(gTotalFrames - zeroSample) * gTicksPerFrame);
            h = gLastHost > back ? gLastHost - back : 0;
        }
        if (gZeroSample != UINT64_MAX && zeroSample > gZeroSample && h <= gZeroHost)   // keep host time monotonic
            h = gZeroHost + (UInt64)((Float64)(zeroSample - gZeroSample) * gTicksPerFrame);
        gZeroSample = zeroSample; gZeroHost = h;
    }
    *outSampleTime = (Float64)gZeroSample;
    *outHostTime = gZeroHost;
    *outSeed = gTimeStampSeed;
    return kAudioHardwareNoError;
}

static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, UInt32, UInt32 op,
                                  Boolean *outWillDo, Boolean *outInPlace) {
    if (inDriver != gDriverRef || inDevice != kObjectID_Device) return kAudioHardwareBadObjectError;
    if (outWillDo) *outWillDo = (op == kAudioServerPlugInIOOperationReadInput);
    if (outInPlace) *outInPlace = true;
    return kAudioHardwareNoError;
}

static OSStatus BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, UInt32, UInt32, UInt32,
                                 const AudioServerPlugInIOCycleInfo *) {
    return (inDriver == gDriverRef && inDevice == kObjectID_Device) ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}

static OSStatus DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, AudioObjectID inStream,
                              UInt32, UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle,
                              void *mainBuffer, void *) {
    if (inDriver != gDriverRef || inDevice != kObjectID_Device) return kAudioHardwareBadObjectError;
    if (op != kAudioServerPlugInIOOperationReadInput || inStream != kObjectID_Stream_Input || !mainBuffer) return kAudioHardwareNoError;
    auto out = static_cast<Float32 *>(mainBuffer);
    const int32_t *ring = gRing;
    if (!ring || !cycle) { memset(out, 0, frames * kChannels * sizeof(Float32)); return kAudioHardwareNoError; }
    Float64 st = cycle->mInputTime.mSampleTime;
    UInt64 t = st > 0 ? (UInt64)st : 0;
    const Float32 g = gGain.load() / 2147483648.0f;
    for (UInt32 i = 0; i < frames; i++) {
        const int32_t *f = ring + ((t + i) % kRingFrames) * 4;
        Float32 l = 0.f, r = 0.f;
        if (gChannelMode == 1) {
            l = (Float32)f[2] * g;
            r = (Float32)f[3] * g;
        } else if (gChannelMode == 2) {
            l = ((Float32)f[0] + (Float32)f[2]) * 0.5f * g;
            r = ((Float32)f[1] + (Float32)f[3]) * 0.5f * g;
        } else {
            l = (Float32)f[0] * g;
            r = (Float32)f[1] * g;
        }
        out[2 * i]     = l > 1.f ? 1.f : l < -1.f ? -1.f : l;
        out[2 * i + 1] = r > 1.f ? 1.f : r < -1.f ? -1.f : r;
    }
    return kAudioHardwareNoError;
}

static OSStatus EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, UInt32, UInt32, UInt32,
                               const AudioServerPlugInIOCycleInfo *) {
    return (inDriver == gDriverRef && inDevice == kObjectID_Device) ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}

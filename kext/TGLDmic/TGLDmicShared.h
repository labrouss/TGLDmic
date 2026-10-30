// Interface shared by TGLDmic.kext (TGLDmicUserClient) and TGLDmic.driver (HAL plug-in).
#pragma once

#define kTGLDmicServiceClass   "TGLDmic"
#define kTGLDmicUserClientType 0          // IOServiceOpen type
#define kTGLDmicRingMemoryType 0          // IOConnectMapMemory64 type: the 64 KB capture ring (read-only)

enum {
    kTGLDmicMethodGetInfo     = 0,   // out: version, state bits, rate, channels, ring bytes, frame bytes
    kTGLDmicMethodStart       = 1,   // out: none. Boots the DSP if needed and starts the DMIC pipeline
    kTGLDmicMethodStop        = 2,   // out: none
    kTGLDmicMethodGetPosition = 3,   // out: DMA write position in ring (bytes), mach_absolute_time, generation
    kTGLDmicMethodCount
};

enum {
    kTGLDmicStateDspUp     = 1u << 0,
    kTGLDmicStateStreaming = 1u << 1,
    kTGLDmicStateService   = 1u << 2,
};

#define kTGLDmicInterfaceVersion 1
#define kTGLDmicVersionString    "0.5.0"   // kept in sync with Info.plist by tools/set-version.sh
// Ring format: S32_LE, 4 interleaved channels, 48 kHz. ch0/ch1 = mic pair A, ch2 == ch3 = mic B.

// tgldmic-probe: talk to TGLDmic.kext directly (no Core Audio) - start the DMIC pipeline,
// read N seconds from the shared ring, print per-channel levels and optionally write a WAV.
//   tgldmic-probe [seconds] [out.wav]
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../kext/TGLDmic/TGLDmicShared.h"

#define RING_FRAMES 4096u

static void wav_header(FILE *f, uint32_t frames) {
    uint32_t data = frames * 16, riff = 36 + data, fmtlen = 16, rate = 48000, brate = rate * 16;
    uint16_t pcm = 1, ch = 4, align = 16, bits = 32;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmtlen, 4, 1, f);
    fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&brate, 4, 1, f);
    fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
}

int main(int argc, char **argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 3.0;
    const char *wav = argc > 2 ? argv[2] : NULL;
    io_service_t svc = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching(kTGLDmicServiceClass));
    if (!svc) { fprintf(stderr, "TGLDmic service not found (kext loaded? boot-arg -tgldmic?)\n"); return 1; }
    io_connect_t c;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), kTGLDmicUserClientType, &c);
    IOObjectRelease(svc);
    if (kr) { fprintf(stderr, "IOServiceOpen: 0x%x\n", kr); return 1; }

    uint64_t info[6] = {0}; uint32_t n = 6;
    kr = IOConnectCallScalarMethod(c, kTGLDmicMethodGetInfo, NULL, 0, info, &n);
    printf("info: kr=0x%x version %llu state 0x%llx rate %llu ch %llu ring %llu frame %llu\n",
           kr, info[0], info[1], info[2], info[3], info[4], info[5]);

    mach_vm_address_t addr = 0; mach_vm_size_t size = 0;
    kr = IOConnectMapMemory64(c, kTGLDmicRingMemoryType, mach_task_self(), &addr, &size, kIOMapAnywhere);
    if (kr) { fprintf(stderr, "map ring: 0x%x\n", kr); return 1; }
    const int32_t *ring = (const int32_t *)addr;
    printf("ring mapped at 0x%llx, %llu bytes\n", addr, size);

    kr = IOConnectCallScalarMethod(c, kTGLDmicMethodStart, NULL, 0, NULL, NULL);
    printf("start: 0x%x\n", kr);
    if (kr) return 1;

    uint32_t total = (uint32_t)(seconds * 48000), got = 0, last = UINT32_MAX;
    int32_t *buf = calloc(total, 16);
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    uint64_t t0 = mach_absolute_time(), polls = 0;
    while (got < total) {
        usleep(10000);
        uint64_t out[3] = {0}; uint32_t m = 3;
        if (IOConnectCallScalarMethod(c, kTGLDmicMethodGetPosition, NULL, 0, out, &m)) { fprintf(stderr, "position failed\n"); break; }
        uint32_t pos = (uint32_t)(out[0] / 16) % RING_FRAMES;
        if (last == UINT32_MAX) { last = pos; continue; }
        uint32_t avail = (pos + RING_FRAMES - last) % RING_FRAMES;
        for (uint32_t i = 0; i < avail && got < total; i++, got++)
            memcpy(buf + got * 4, ring + ((last + i) % RING_FRAMES) * 4, 16);
        last = pos; polls++;
    }
    double el = (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e9;
    IOConnectCallScalarMethod(c, kTGLDmicMethodStop, NULL, 0, NULL, NULL);
    printf("captured %u frames in %.2f s (%.0f frames/s, %llu polls)\n", got, el, got / el, polls);

    for (int ch = 0; ch < 4; ch++) {
        double ss = 0, pk = 0;
        for (uint32_t i = 0; i < got; i++) { double x = buf[i * 4 + ch] / 2147483648.0; ss += x * x; if (fabs(x) > pk) pk = fabs(x); }
        double rms = got ? sqrt(ss / got) : 0;
        printf("  ch%d: rms %6.1f dBFS  peak %6.1f dBFS\n", ch, rms > 0 ? 20 * log10(rms) : -999, pk > 0 ? 20 * log10(pk) : -999);
    }
    if (wav) {
        FILE *f = fopen(wav, "wb");
        if (f) { wav_header(f, got); fwrite(buf, 16, got, f); fclose(f); printf("wrote %s\n", wav); }
    }
    IOServiceClose(c);
    return 0;
}

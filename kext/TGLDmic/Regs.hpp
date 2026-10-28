// Register map for Intel cAVS 2.5 (Tiger Lake-LP) HDA controller + audio DSP.
// Values cross-checked against Linux include/sound/hda_register.h and sound/soc/sof/intel/hda.h.
#pragma once
#include <stdint.h>

namespace HDA {                       // BAR0
constexpr uint32_t GCAP      = 0x00;  // [15:12] OSS, [11:8] ISS, [7:3] BSS
constexpr uint32_t VMIN      = 0x02;
constexpr uint32_t VMAJ      = 0x03;
constexpr uint32_t GCTL      = 0x08;
constexpr uint32_t STATESTS  = 0x0E;
constexpr uint32_t LLCH      = 0x14;  // linked list of extended capabilities
constexpr uint32_t INTCTL    = 0x20;
constexpr uint32_t INTSTS    = 0x24;
constexpr uint32_t SD_BASE   = 0x80;  // stream descriptors: inputs first, then outputs
constexpr uint32_t SD_STRIDE = 0x20;
constexpr uint32_t SD_CTL    = 0x00;  // 24-bit CTL + 8-bit STS
constexpr uint32_t SD_LPIB   = 0x04;
constexpr uint32_t SD_CBL    = 0x08;
constexpr uint32_t SD_LVI    = 0x0C;
constexpr uint32_t SD_FMT    = 0x12;
constexpr uint32_t SD_BDPL   = 0x18;
constexpr uint32_t SD_BDPU   = 0x1C;
constexpr uint32_t SD_CTL_SRST = 1u << 0;
constexpr uint32_t SD_CTL_RUN  = 1u << 1;
constexpr uint32_t SD_CTL_TAG_SHIFT = 20;

constexpr uint32_t CAP_ID_SHIFT = 16, CAP_ID_MASK = 0xFFFu << 16, CAP_NEXT_MASK = 0xFFFF;
enum : uint32_t { CAP_GTS = 1, CAP_ML = 2, CAP_PP = 3, CAP_SPB = 4, CAP_DRSM = 5 };
constexpr uint32_t PP_PPCH = 0x00, PP_PPCTL = 0x04, PP_PPSTS = 0x08;
constexpr uint32_t PPCTL_GPROCEN = 1u << 30, PPCTL_PIE = 1u << 31;  // [29:0] PROCEN per stream
}

namespace DSP {                       // BAR4
constexpr uint32_t ADSPCS  = 0x04;    // [7:0] CRST, [15:8] CSTALL, [23:16] SPA, [31:24] CPA
constexpr uint32_t ADSPIC  = 0x08;
constexpr uint32_t ADSPIS  = 0x0C;
constexpr uint32_t HIPCTDR = 0xC0;    // DSP -> host request (BUSY bit 31)
constexpr uint32_t HIPCTDA = 0xC4;
constexpr uint32_t HIPCTDD = 0xC8;
constexpr uint32_t HIPCIDR = 0xD0;    // host -> DSP request (BUSY bit 31)
constexpr uint32_t HIPCIDA = 0xD4;    // DONE bit 31
constexpr uint32_t HIPCIDD = 0xD8;
constexpr uint32_t HIPCCTL = 0xE8;    // CNL_DSP_IPC_BASE + 0x28
constexpr uint32_t IPC_BUSY = 1u << 31, IPC_DONE = 1u << 31;
constexpr uint32_t ROM_STATUS   = 0x80000;   // SRAM window: FSR
constexpr uint32_t MBOX_UP      = 0x81000;   // DSP -> host replies (0x1000)
constexpr uint32_t MBOX_DOWN    = 0xA0000;   // host -> DSP messages (0x2000)
constexpr uint32_t STREAM_BOX   = 0xC1000;   // stream position region
constexpr uint32_t FW_READY     = 0x70000000;
}

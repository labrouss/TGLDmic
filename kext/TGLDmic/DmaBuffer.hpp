#pragma once
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOMapper.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <pexpert/pexpert.h>

// One DMA-able buffer as the HDA controller sees it: contiguous, below 4 GB (the SOF page
// table packs 20-bit page numbers), and addressed by IOVA so it works with VT-d on or off.
struct DmaBuffer {
    IOBufferMemoryDescriptor *mem {nullptr};
    IODMACommand *cmd {nullptr};
    uint8_t *virt {nullptr};
    uint64_t iova {0};
    size_t size {0};

    // How the controller addresses memory: IOVA from the device's mapper, or from the system
    // mapper when the device has none (Tahoe + HDAUniversal; a v0.4.7 test proved physical
    // addresses do not work there: DMA fetched nothing). tgldmicdma=2 forces physical for testing.
    enum Mode { kAuto = 0, kForceMapped = 1, kForcePhysical = 2 };
    bool physical {false};

    static int modeArg() {
        int v = 0;
        return PE_parse_boot_argn("tgldmicdma", &v, sizeof(v)) ? v : kAuto;
    }

    bool alloc(IOPCIDevice *pci, size_t bytes) {
        size = round_page(bytes);
        IOMapper *mapper = IOMapper::copyMapperForDevice(pci);
        int mode = modeArg();
        physical = (mode == kForcePhysical);
        IOOptionBits opts = kIODirectionInOut | kIOMemoryPhysicallyContiguous | (physical ? kIOMemoryMapperNone : 0);
        mem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, opts, size, 0x00000000FFFFF000ULL);
        if (!mem || mem->prepare() != kIOReturnSuccess) { OSSafeReleaseNULL(mapper); release(); return false; }
        virt = static_cast<uint8_t *>(mem->getBytesNoCopy());
        bzero(virt, size);
        if (physical) {
            IOByteCount len = 0;
            iova = mem->getPhysicalSegment(0, &len, kIOMemoryMapperNone);
            OSSafeReleaseNULL(mapper);
            if (!iova || len < size || iova + size > 0x100000000ULL) { release(); return false; }
            return true;
        }
        cmd = IODMACommand::withSpecification(kIODMACommandOutputHost64, 32, 0,
                  IODMACommand::kMapped, 0, 1, mapper, nullptr);
        OSSafeReleaseNULL(mapper);
        if (!cmd || cmd->setMemoryDescriptor(mem, true) != kIOReturnSuccess) { release(); return false; }
        IODMACommand::Segment64 seg {};
        UInt64 off = 0; UInt32 n = 1;
        if (cmd->gen64IOVMSegments(&off, &seg, &n) != kIOReturnSuccess || n != 1 || seg.fLength < size ||
            seg.fIOVMAddr + size > 0x100000000ULL) { release(); return false; }
        iova = seg.fIOVMAddr;
        return true;
    }

    // Push CPU-written data to RAM: the controller may DMA without snooping the CPU caches.
    void sync() {
        if (mem) mem->performOperation(kIOMemoryIncoherentIOFlush, 0, size);
        if (cmd) cmd->synchronize(kIODirectionOut);
    }
    // Drop stale cache lines before the CPU reads data the controller wrote.
    void invalidate() { if (mem) mem->performOperation(kIOMemoryIncoherentIOFlush, 0, size); }

    void release() {
        if (cmd) { cmd->clearMemoryDescriptor(true); OSSafeReleaseNULL(cmd); }
        if (mem) { mem->complete(); OSSafeReleaseNULL(mem); }
        virt = nullptr; iova = 0; size = 0; physical = false;
    }
};

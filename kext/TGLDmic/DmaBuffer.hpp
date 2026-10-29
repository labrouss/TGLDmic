#pragma once
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOMapper.h>
#include <IOKit/pci/IOPCIDevice.h>

// One DMA-able buffer as the HDA controller sees it: contiguous, below 4 GB (the SOF page
// table packs 20-bit page numbers), and addressed by IOVA so it works with VT-d on or off.
struct DmaBuffer {
    IOBufferMemoryDescriptor *mem {nullptr};
    IODMACommand *cmd {nullptr};
    uint8_t *virt {nullptr};
    uint64_t iova {0};
    size_t size {0};

    bool alloc(IOPCIDevice *pci, size_t bytes) {
        size = round_page(bytes);
        mem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
                  kIODirectionInOut | kIOMemoryPhysicallyContiguous, size, 0x00000000FFFFF000ULL);
        if (!mem || mem->prepare() != kIOReturnSuccess) { release(); return false; }
        virt = static_cast<uint8_t *>(mem->getBytesNoCopy());
        bzero(virt, size);
        IOMapper *mapper = IOMapper::copyMapperForDevice(pci);
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

    void sync() { if (cmd) cmd->synchronize(kIODirectionOut); }

    void release() {
        if (cmd) { cmd->clearMemoryDescriptor(true); OSSafeReleaseNULL(cmd); }
        if (mem) { mem->complete(); OSSafeReleaseNULL(mem); }
        virt = nullptr; iova = 0; size = 0;
    }
};

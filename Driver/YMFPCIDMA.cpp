/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "YMFPCIAudioEngine.h"
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>

bool YMFPCIAudioEngine::allocateDMA(YMFDMABuffer &buffer, unsigned size)
{
    /* Below 4 GiB, physically contiguous and page aligned. IODMACommand also
     * obtains the actual bus/IOMMU address; a CPU pointer is never a DMA base. */
    buffer.memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous, size, 0x00000000fffff000ULL);
    if (!buffer.memory)
        return false;
    buffer.bytes = buffer.memory->getBytesNoCopy();
    if (!buffer.bytes || buffer.memory->prepare() != kIOReturnSuccess)
        return false;
    buffer.prepared = true;
    buffer.command = IODMACommand::withSpecification(kIODMACommandOutputHost64,
        32, size, IODMACommand::kMapped, size, 256);
    if (!buffer.command ||
        buffer.command->setMemoryDescriptor(buffer.memory) != kIOReturnSuccess)
        return false;
    IODMACommand::Segment64 segment;
    UInt32 count = 1;
    UInt64 offset = 0;
    if (buffer.command->gen64IOVMSegments(&offset, &segment, &count) != kIOReturnSuccess ||
        count != 1 || segment.fLength < size || offset != size ||
        (segment.fIOVMAddr & 255U) ||
        segment.fIOVMAddr > 0xffffffffULL - (size - 1)) {
        IOLog("YMFPCI: rejected non-contiguous/out-of-range DMA mapping\n");
        return false;
    }
    buffer.address = static_cast<UInt32>(segment.fIOVMAddr);
    bzero(buffer.bytes, size);
    return true;
}

void YMFPCIAudioEngine::releaseDMA(YMFDMABuffer &buffer)
{
    /* The hardware must already be stopped and PCI bus mastering disabled. */
    if (buffer.command) {
        if (buffer.command->getMemoryDescriptor())
            buffer.command->clearMemoryDescriptor();
        buffer.command->release();
    }
    if (buffer.memory) {
        if (buffer.prepared)
            buffer.memory->complete();
        buffer.memory->release();
    }
    bzero(&buffer, sizeof(buffer));
}

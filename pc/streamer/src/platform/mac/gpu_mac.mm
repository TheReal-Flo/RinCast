// The GPU on macOS, from Metal: the one the system prefers, which is what VideoToolbox uses.
#include "gpu.h"

#import <Metal/Metal.h>

namespace rcs {

GpuInfo DescribeGpu() {
    GpuInfo info;
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        info.name = device ? device.name.UTF8String : "GPU";
        if (device) info.dedicatedVram = device.hasUnifiedMemory ? 0 : device.recommendedMaxWorkingSetSize;
    }
    return info;
}

}  // namespace rcs

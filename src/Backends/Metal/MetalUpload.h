#pragma once
#include "../../RHI/UploadPool.h"
#include <limits>
#import <Metal/Metal.h>

namespace dyf::Backends
{
    // Shared storage is persistently CPU-addressable, including on discrete Macs.
    struct MetalUploadPage final
    {
        uint64_t capacity = 0;
        uint8_t* mapped = nullptr;
        id<MTLBuffer> buffer = nil;
        MetalUploadPage() = default;
        MetalUploadPage(const MetalUploadPage&) = delete;
        MetalUploadPage& operator=(const MetalUploadPage&) = delete;
        ~MetalUploadPage()
        {
#if !__has_feature(objc_arc)
            [buffer release];
#endif
            buffer = nil;
        }
    };
    using MetalUploadPool = RHI::Detail::UploadPagePool<MetalUploadPage>;
    using MetalUploadArena = RHI::Detail::UploadArena<MetalUploadPage>;

    // The factory may outlive MetalDevice through a command-owned page lease.
    // Explicit MRC ownership mirrors ARC's strong Objective-C data member.
    struct MetalUploadDevice final
    {
        id<MTLDevice> device = nil;
        explicit MetalUploadDevice(id<MTLDevice> value) : device(value)
        {
#if !__has_feature(objc_arc)
            [device retain];
#endif
        }
        MetalUploadDevice(const MetalUploadDevice&) = delete;
        MetalUploadDevice& operator=(const MetalUploadDevice&) = delete;
        ~MetalUploadDevice()
        {
#if !__has_feature(objc_arc)
            [device release];
#endif
            device = nil;
        }
    };

    inline std::shared_ptr<MetalUploadPool> MakeMetalUploadPool(id<MTLDevice> device)
    {
        auto owner = std::make_shared<MetalUploadDevice>(device);
        return std::make_shared<MetalUploadPool>(
            [owner](uint64_t capacity) -> std::unique_ptr<MetalUploadPage>
            {
                if(owner->device == nil || capacity > std::numeric_limits<NSUInteger>::max() ||
                    capacity > owner->device.maxBufferLength) return {};
                auto page = std::make_unique<MetalUploadPage>();
                page->buffer = [owner->device newBufferWithLength:static_cast<NSUInteger>(capacity)
                    options:MTLResourceStorageModeShared];
                if(page->buffer == nil || page->buffer.contents == nullptr) return {};
                page->capacity = capacity;
                page->mapped = static_cast<uint8_t*>(page->buffer.contents);
                return page;
            });
    }
}

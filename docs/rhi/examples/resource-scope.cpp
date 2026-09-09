#include <RHI/Buffer.h>
#include <RHI/ResourceScope.h>

// device must outlive the scope. This example only creates a buffer.
void CreateOwnedBuffer(dy::RHI::IDevice& device)
{
    using namespace dy::RHI;
    ResourceScope resources(device);
    BufferDesc desc;
    desc.size = 3 * 3 * sizeof(float);
    desc.stride = 3 * sizeof(float);
    desc.usage = BufferUsage::Vertex;
    desc.initialState = ResourceState::CopyDestination;

    const BufferHandle vertices = resources.Keep(device.CreateBuffer(desc));
    (void)vertices;
    // The buffer is destroyed at scope exit. Upload/submission are separate.
}

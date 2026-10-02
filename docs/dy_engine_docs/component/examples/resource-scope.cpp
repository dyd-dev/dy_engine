#include <dyf/RHI.h>
#include <cstdio>
#include <exception>

// 생성 여부만 확인하는 예제다. 업로드와 제출은 별도 단계다.
bool CreateOwnedBuffer(dyf::RHI::IDevice& device)
{
    try
    {
        // device는 이 scope보다 오래 살아 있어야 한다.
        dyf::RHI::ResourceScope resources(device);
        dyf::RHI::BufferDesc desc;
        desc.size = 3 * 3 * sizeof(float);
        desc.stride = 3 * sizeof(float);
        desc.usage = dyf::RHI::BufferUsage::Vertex;
        desc.initialState = dyf::RHI::ResourceState::CopyDestination;

        const auto vertices = resources.Keep(device.CreateBuffer(desc));
        (void)vertices;
        // 동일 핸들에 DestroyBuffer를 직접 호출하지 않는다.
        // resources가 소멸할 때 이 버퍼의 소유권을 반환한다.
        return true;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return false;
    }
}
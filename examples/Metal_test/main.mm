#import <MetalKit/MetalKit.h>
#include "Platform/Window.h"
#include "RHI/IDevice.h"
#include "RHI/ICommandList.h"
#include "RHI/IBuffer.h"
#include "Backends/Metal/MetalCommandList.h"

#import <Metal/Metal.h>
#import <simd/simd.h>

struct Vertex {
    float position[2];
    float texCoord[2];
    float color[4];
};

int main()
{
    // 1. 윈도우 생성
    dy::Platform::Window window(800, 600, "Metal Test");

    // 2. RHI Device 생성
    dy::RHI::IDevice* device = dy::RHI::IDevice::Create(window.GetHandle());
    if(!device) return -1;

    // 3. 셰이더 로드
    id<MTLDevice> mtlDevice = MTLCreateSystemDefaultDevice();
    NSString* libPath = @"/Users/wnsgur7318/Documents/dy_engine/build/examples/Metal_test/default.metallib";
    NSError* libError = nil;
    id<MTLLibrary> library = [mtlDevice newLibraryWithURL:[NSURL fileURLWithPath:libPath]
                                                    error:&libError];
    if(!library) { NSLog(@"라이브러리 로드 실패: %@", libError); return -1; }

    id<MTLFunction> vertFunc = [library newFunctionWithName:@"vertexShader"];
    id<MTLFunction> fragFunc = [library newFunctionWithName:@"fragmentShader"];

    // 4. 파이프라인 생성
    NSError* error = nil;
    MTLRenderPipelineDescriptor* pipeDesc = [MTLRenderPipelineDescriptor new];
    pipeDesc.vertexFunction              = vertFunc;
    pipeDesc.fragmentFunction            = fragFunc;
    pipeDesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    id<MTLRenderPipelineState> pipelineState =
        [mtlDevice newRenderPipelineStateWithDescriptor:pipeDesc error:&error];

    // 5. 버텍스 데이터 (사각형 = 꼭짓점 4개)
    Vertex vertices[] = {
        // position        texCoord    color
        { {-0.5f,  0.5f}, {0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f} }, // 왼쪽 위  (빨강)
        { { 0.5f,  0.5f}, {1.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f} }, // 오른쪽 위 (초록)
        { {-0.5f, -0.5f}, {0.0f, 1.0f}, {0.0f, 0.0f, 1.0f, 1.0f} }, // 왼쪽 아래 (파랑)
        { { 0.5f, -0.5f}, {1.0f, 1.0f}, {1.0f, 1.0f, 0.0f, 1.0f} }, // 오른쪽 아래 (노랑)
    };

    // 6. 인덱스 데이터 (삼각형 2개)
    uint16_t indices[] = {
        0, 1, 2, // 첫번째 삼각형
        1, 3, 2  // 두번째 삼각형
    };

    // 7. 버텍스 버퍼 생성
    dy::RHI::BufferDesc vbDesc{};
    vbDesc.size   = sizeof(vertices);
    vbDesc.stride = sizeof(Vertex);
    vbDesc.usage  = dy::RHI::BufferUsage::Vertex;
    dy::RHI::IBuffer* vertexBuffer = device->CreateBuffer(vbDesc);

    void* vbData = vertexBuffer->Map(0, sizeof(vertices));
    memcpy(vbData, vertices, sizeof(vertices));
    vertexBuffer->Unmap();

    // 8. 인덱스 버퍼 생성
    dy::RHI::BufferDesc ibDesc{};
    ibDesc.size   = sizeof(indices);
    ibDesc.stride = sizeof(uint16_t);
    ibDesc.usage  = dy::RHI::BufferUsage::Index;
    dy::RHI::IBuffer* indexBuffer = device->CreateBuffer(ibDesc);

    void* ibData = indexBuffer->Map(0, sizeof(indices));
    memcpy(ibData, indices, sizeof(indices));
    indexBuffer->Unmap();
    
    // 9. 텍스처 로드
    MTKTextureLoader* loader = [[MTKTextureLoader alloc] initWithDevice:mtlDevice];
    NSString* imgPath = @"/Users/wnsgur7318/Documents/dy_engine/examples/Metal_test/맹구.jpeg";
    NSURL* imgURL = [NSURL fileURLWithPath:imgPath];
    NSError* texError = nil;
    id<MTLTexture> texture = [loader newTextureWithContentsOfURL:imgURL
                                        options:nil
                                        error:&texError];
    if(!texture) {
        NSLog(@"텍스처 로드 실패: %@", texError);
    } else {
        NSLog(@"텍스처 로드 성공! 크기: %lu x %lu", texture.width, texture.height);
    }
    

    // 10. 메인 루프
    while(window.IsRunning())
    {
        window.PollEvents();

        device->BeginFrame();
        dy::RHI::ICommandList* cmd = device->AcquireCommandList();
        cmd->SetRenderTargets(0, nullptr, nullptr);

        auto* metalCmd = static_cast<dy::Backends::MetalCommandList*>(cmd);
        metalCmd->SetNativePipelineState((__bridge void*)pipelineState);
        // 텍스처 바인딩
        metalCmd->SetNativeTexture((__bridge void*)texture, 0);

        // 버텍스 버퍼 바인딩
        metalCmd->SetNativeVertexBuffer(vertexBuffer, 0);

        // 인덱스 버퍼로 사각형 그리기
        metalCmd->DrawIndexed(indexBuffer, 6);

        cmd->Close();

        dy::RHI::ICommandList* cmdLists[] = { cmd };
        device->Submit(cmdLists, 1);
        device->Present();
    }

    device->DestroyBuffer(vertexBuffer);
    device->DestroyBuffer(indexBuffer);
    delete device;
    return 0;
}

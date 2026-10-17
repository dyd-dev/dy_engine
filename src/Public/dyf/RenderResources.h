#pragma once

#include "dyf/Material.h"
#include "dyf/RHI/RenderGraph.h"
#include <array>

namespace dyf
{
// 자원 소유권은 제공한 렌더러나 모듈에 있다. 그래프 핸들은 현재 프레임에서만 유효하다.
struct RenderTexture
{
    RHI::TextureHandle texture = nullptr;
    RHI::RGResourceHandle resource;
};

struct RenderBuffer
{
    RHI::BufferHandle buffer = nullptr;
    RHI::RGResourceHandle resource;
};

// 환경광 셰이더와 투명 모듈이 공유하는 GPU 상수 형식이다.
struct RenderEnvironmentConstants
{
    std::array<Math::float4,9> sh;
    Math::float4 settings, screen, probePosition, probeBoxMin, probeBoxMax;
};
static_assert(sizeof(RenderEnvironmentConstants)==224);

struct RenderEnvironmentResources
{
    RenderEnvironmentConstants constants = {};
    RHI::TextureHandle prefilter = nullptr, probePrefilter = nullptr;
    RHI::BufferHandle probeData = nullptr, probeParameters = nullptr;
    RHI::BufferHandle constantBuffer = nullptr;
    RenderTexture diffuse = {}, specular = {};
};

// 모듈은 조명을 관리하는 구현 클래스 대신 준비된 입력만 빌린다.
struct RenderLightingResources
{
    RenderBuffer lights = {}, shadowConstants = {};
    RenderBuffer clusterLights = {}, clusterList = {}, clusterConstants = {};
    RenderTexture areaMatrix = {}, areaAmplitude = {};
    RenderEnvironmentResources environment;
    bool clustered = false;
};

struct RenderHistory
{
    Math::float4x4 previousCameraView = Math::float4x4::Identity();
    Math::float4x4 previousCameraProjection = Math::float4x4::Identity();
    uint32_t frameIndex = 0;
    bool valid = false;
};

// 기본 경로에서 선택한 지오메트리·LOD·재질을 모든 추가 패스가 함께 사용한다.
struct RenderGeometryDraw
{
    RHI::BufferHandle vertexBuffer = nullptr, indexBuffer = nullptr;
    RHI::BufferHandle previousVertexBuffer = nullptr, instanceBuffer = nullptr, indirectArguments = nullptr;
    uint32_t instanceOffset = 0, indirectOffset = 0;
    bool deformationHistoryValid = true;
    uint32_t indexCount = 0;
    Math::Bounds3 localBounds;
    Math::float4x4 model = Math::float4x4::Identity();
    Math::float4x4 previousMVP = Math::float4x4::Identity();
    // Scene 재질은 패스 기록이 끝날 때까지 유효하다. 모듈은 프레임 종료 때 참조를 비운다.
    const MaterialDesc* material = nullptr;
    Math::float4 baseColor = {1,1,1,1};
    std::array<RHI::TextureHandle,ToIndex(MaterialTextureKind::Count)> textures = {};
    uint32_t textureFlags = 0;
};
}

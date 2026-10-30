#include "dyf/Camera.h"
#include "dyf/Mesh.h"
#include "dyf/Renderer.h"
#include "dyf/RenderExtension.h"
#include "dyf/Scene.h"
#include "dyf/Extends/AdvancedRendering.h"
#include "dyf/Extends/Model/Model.h"
#include "dyf/Extends/Model/ModelRenderer.h"
#include "dyf/Extends/Model/ModelScene.h"
#include "dyf/Platform/Window.h"
#include "dyf/RHI/Readback.h"
#include "dyf/RHI/Texture.h"
#include "../../../dyf/SceneAlgorithms.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include "RhiSurfaceConformance.h"

namespace
{
using namespace dyf;
constexpr uint32_t Size=256;
using RGB=std::array<double,3>;
using Pixel=std::array<uint8_t,4>;

void Require(bool value,const std::string& message)
{
    if(!value)throw std::runtime_error(message);
}

// Independent output reference: unlit linear radiance, Reinhard exposure=1,
// then the exact sRGB transfer function of the selected output attachment.
double Srgb(double linear)
{
    linear=std::clamp(linear,0.0,1.0);
    return linear<=0.0031308?12.92*linear:1.055*std::pow(linear,1.0/2.4)-0.055;
}
Pixel Display(RGB radiance)
{
    Pixel result{0,0,0,255};
    for(size_t i=0;i<3;++i)
        result[i]=uint8_t(std::lround(255*Srgb(radiance[i]/(1+radiance[i]))));
    return result;
}
RGB Multiply(RGB a,RGB b)
{
    for(size_t i=0;i<3;++i)a[i]*=b[i];
    return a;
}
Pixel BlendDisplay(RGB foreground,RGB background,double coverage,bool hdr)
{
    RGB linear{};
    for(size_t i=0;i<3;++i)
    {
        if(hdr)linear[i]=coverage*foreground[i]+(1-coverage)*background[i];
        else linear[i]=coverage*foreground[i]/(1+foreground[i])+
            (1-coverage)*background[i]/(1+background[i]);
    }
    if(hdr)return Display(linear);
    Pixel result{0,0,0,255};
    for(size_t i=0;i<3;++i)result[i]=uint8_t(std::lround(255*Srgb(linear[i])));
    return result;
}

MeshData Quad(float left=-.8f,float right=.8f,float bottom=-.8f,float top=.8f,
    float z=0,Math::float4 color={1,1,1,1},Math::float3 normal={0,0,1})
{
    MeshData mesh;
    for(const auto& position:std::array<Math::float3,4>{{{left,bottom,z},{right,bottom,z},
        {right,top,z},{left,top,z}}})
    {
        Vertex vertex;
        vertex.position=position;vertex.normal=normal;vertex.uv={0,0};vertex.color=color;
        mesh.vertices.push_back(vertex);
    }
    mesh.indices={0,1,2,0,2,3};
    return mesh;
}

MaterialDesc Unlit(RGB color={1,1,1})
{
    MaterialDesc material;
    material.unlit=true;
    material.baseColor={float(color[0]),float(color[1]),float(color[2]),1};
    return material;
}

RendererConfig Config(bool advanced=false)
{
    RendererConfig config;
    config.allowReadback=true;config.enableProfilerHud=false;config.vsync=true;
    config.enableLod=false;config.enableBindlessMaterials=false;
    config.clearColor={.07f,.11f,.19f,1};
    config.lighting.enabled=true;config.lighting.ambientIntensity=0;
    config.lighting.environment.diffuse=false;config.lighting.environment.specular=false;
    config.lighting.environment.background=false;
    config.enableHdrRendering=advanced;config.advanced.enabled=advanced;
    config.advanced.ambientOcclusion=false;config.advanced.diffuseGI=false;
    config.advanced.reflections=false;config.advanced.temporalAA=false;config.advanced.bloom=false;
    return config;
}

struct Options
{
    bool validation=false;
    bool cpuOnly=false;
    std::filesystem::path captures;
    std::filesystem::path rhiShaders;
    std::string model;
    std::string skinnedModel;
};

void CpuVisibilityRegression()
{
    auto affine=Math::Translation({2,-3,4})*Math::Scaling({-2,3,.5f});
    affine.m[4]=.75f;
    Require(IsAffineTransform(affine),"Finite mirrored/sheared affine bounds must remain supported.");
    auto projective=Math::float4x4::Identity();projective.m[3]=1;
    const Math::float4 point{2,0,.5f,1};
    const auto row=[&](uint32_t r) {
        return projective.m[r]*point.x+projective.m[4+r]*point.y+
            projective.m[8+r]*point.z+projective.m[12+r]*point.w;
    };
    const Math::float4 clip{row(0),row(1),row(2),row(3)};
    Require(clip.x<clip.w && clip.x>-clip.w && clip.z>0 && clip.z<clip.w,
        "Projective counterexample must actually lie inside homogeneous clip space.");
    // The previous sphere used xyz=(2,0,.5), radius=0 and incorrectly failed
    // the right plane -x+1>=0; the vertex shader instead sees w=3.
    Require(-clip.x+1<0 && !IsAffineTransform(projective),
        "Projective models must select unknown GPU bounds (visible, LOD zero).");
    Camera camera;camera.view=Math::float4x4::Identity();camera.projection=Math::float4x4::Identity();
    Math::Bounds3 bounds;bounds.Include({2,0,.5f});
    Require(!CameraFrustum(camera).Outside(bounds,projective),"CPU frustum must also fail open for projective models.");
    affine.m[15]=2;
    Require(!IsAffineTransform(affine),"Non-unit homogeneous w cannot use the affine sphere.");
    affine=Math::float4x4::Identity();affine.m[0]=std::numeric_limits<float>::infinity();
    Require(!IsAffineTransform(affine),"Non-finite transform cannot use bounded visibility.");
    std::puts("PASS CPU visibility: affine mirrors/shears, projective visible counterexample and non-finite fail-open.");
}

class TransparencyObserver final:public RenderExtension
{
public:
    struct DeformedDraw
    {
        uint32_t entity;
        uint64_t geometryRevision;
        Math::Bounds3 bounds;
        bool historyValid;
    };
    RHI::Format reactiveFormat=RHI::Format::Unknown;
    bool nearestDepth=false;
    std::vector<DeformedDraw> deformedDraws;
    bool Prepare(RenderFrame& frame,RHI::ICommandList&) override
    {
        reactiveFormat=RHI::Format::Unknown;nearestDepth=false;
        deformedDraws.clear();
        if(frame.draws)for(uint32_t entity=0;entity<frame.draws->size();++entity)
        {
            const auto& draw=(*frame.draws)[entity];
            if(draw.stockGeometry && draw.geometryRevision && draw.hasLocalBounds)
                deformedDraws.push_back({entity,draw.geometryRevision,draw.localBounds,draw.deformationHistoryValid});
        }
        return true;
    }
    bool AddPasses(RenderStage stage,RenderFrame& frame,RHI::RenderGraph& graph) override
    {
        if(stage!=RenderStage::PostProcess)return true;
        if(frame.reactive.texture)reactiveFormat=frame.reactive.texture->GetDesc().format;
        for(uint32_t index=0;const auto* pass=graph.GetPass(index);++index)
            nearestDepth|=pass->GetName()=="Transparency nearest depth";
        return true;
    }
};

class RenderingTests
{
    Platform::Window m_window{Size,Size,"dyf Rendering Conformance"};
    std::unique_ptr<RHI::IDevice> m_device;
    std::unique_ptr<Renderer> m_renderer;
    RHI::TextureHandle m_target=nullptr;
    RHI::ResourceState m_targetState=RHI::ResourceState::Undefined;
    Camera m_camera;
    const Options& m_options;
    uint32_t m_frames=0;
    std::array<uint32_t,2> m_transparencyFrames={};
    TransparencyObserver* m_transparencyObserver=nullptr;
    RHI::Format m_reactiveFormat=RHI::Format::Unknown;

public:
    explicit RenderingTests(const Options& options):m_options(options)
    {
        Require(m_window.GetHandle()!=nullptr,"Window creation failed (run inside the selected headless compositor).");
        RHI::DeviceDesc desc;desc.enableValidation=options.validation;
        m_device.reset(RHI::IDevice::Create(desc));
        Require(bool(m_device),"Device creation failed.");
        Require(m_device->Supports(RHI::Feature::Rasterization),"A native raster backend is required; Null is not a pixel reference.");
        m_renderer=Renderer::Create(*m_device,m_window.GetHandle(),Config());
        Require(bool(m_renderer),"Renderer creation failed.");
        Require(m_renderer->AddExtension(CreateAdvancedRenderingModule(AdvancedRenderModule::SurfaceLighting)),"Surface module registration failed.");
        Require(m_renderer->AddExtension(CreateAdvancedRenderingModule(AdvancedRenderModule::Transparency)),"Transparency module registration failed.");
        auto observer=std::make_unique<TransparencyObserver>();m_transparencyObserver=observer.get();
        Require(m_renderer->AddExtension(std::move(observer)),"Transparency observer registration failed.");
        RHI::TextureDesc target;
        target.width=target.height=Size;target.format=RHI::Format::R8G8B8A8_UNORM_SRGB;
        target.usage=RHI::TextureUsage::RenderTarget|RHI::TextureUsage::ShaderResource;
        m_target=m_device->CreateTexture(target);
        Require(m_target!=nullptr,"sRGB render/readback target creation failed.");
        Require(m_camera.LookAt({0,0,2},{0,0,0},{0,1,0}) && m_camera.SetOrthographic(2,2,.1f,10),"Camera creation failed.");
        if(!options.captures.empty())std::filesystem::create_directories(options.captures);
    }

    ~RenderingTests()
    {
        if(m_device && m_target)m_device->DestroyTexture(m_target);
    }

    RHI::TextureReadback Render(const Scene& scene,const char* name,const RendererConfig& config)
    {
        Require(m_renderer->SetConfig(config),std::string(name)+": configuration rejected.");
        Platform::Window::PollEvents();
        Require(m_renderer->RenderToTexture(scene,m_camera,m_target,m_targetState),std::string(name)+": render failed.");
        m_targetState=RHI::ResourceState::ShaderResource;
        RHI::TextureReadback image;
        Require(m_device->ReadTexture(m_target,image),std::string(name)+": readback failed.");
        Require(image.width==Size && image.height==Size && image.rowPitch>=Size*4 &&
            image.format==RHI::Format::R8G8B8A8_UNORM_SRGB,std::string(name)+": unexpected readback layout.");
        if(m_transparencyObserver->reactiveFormat!=RHI::Format::Unknown)
        {
            const auto format=m_transparencyObserver->reactiveFormat;
            Require(format==RHI::Format::R16_FLOAT || format==RHI::Format::R16G16B16A16_FLOAT,
                std::string(name)+": unexpected reactive storage format.");
            Require(m_transparencyObserver->nearestDepth==(config.advanced.transparencyLayers==2),
                std::string(name)+": configured transparent layers did not select the expected nearest-depth path.");
            ++m_transparencyFrames[config.advanced.transparencyLayers-1];m_reactiveFormat=format;
        }
        ++m_frames;
        if(!m_options.captures.empty())
        {
            const auto path=m_options.captures/(std::string(name)+".png");
            Require(stbi_write_png(path.string().c_str(),Size,Size,4,image.pixels.data(),int(image.rowPitch))!=0,
                "Cannot write "+path.string());
        }
        return image;
    }

    void Expect(const RHI::TextureReadback& image,uint32_t x,uint32_t y,Pixel expected,
        const std::string& label,int tolerance=2) const
    {
        const auto* actual=image.pixels.data()+size_t(y)*image.rowPitch+x*4;
        for(size_t channel=0;channel<4;++channel)
            if(std::abs(int(actual[channel])-int(expected[channel]))>tolerance)
            {
                std::fprintf(stderr,"%s pixel=(%u,%u) actual=(%u,%u,%u,%u) expected=(%u,%u,%u,%u) tolerance=%d\n",
                    label.c_str(),x,y,actual[0],actual[1],actual[2],actual[3],
                    expected[0],expected[1],expected[2],expected[3],tolerance);
                throw std::runtime_error(label+": analytic pixel mismatch.");
            }
    }

    void VertexColors()
    {
        auto mesh=Quad();
        const std::array<RGB,4> colors={RGB{1,0,0},RGB{0,1,0},RGB{0,0,1},RGB{1,1,1}};
        for(size_t i=0;i<4;++i)mesh.vertices[i].color={float(colors[i][0]),float(colors[i][1]),float(colors[i][2]),1};
        const RGB factor{.8,.6,.4};Scene scene;
        Require(bool(scene.Add(mesh,Unlit(factor))),"Cannot create vertex-color scene.");
        const auto image=Render(scene,"vertex-rgba",Config());
        for(const auto& sample:std::array<std::array<uint32_t,2>,3>{{{80,96},{164,166},{104,72}}})
        {
            const double x=2*(sample[0]+.5)/Size-1,y=1-2*(sample[1]+.5)/Size;
            const double s=(x+.8)/1.6,t=(y+.8)/1.6;
            const std::array<double,4> weights=t<=s?std::array<double,4>{1-s,s-t,t,0}:
                std::array<double,4>{1-t,0,s,t-s};
            RGB interpolated{};
            for(size_t vertex=0;vertex<4;++vertex)
                for(size_t c=0;c<3;++c)interpolated[c]+=weights[vertex]*colors[vertex][c];
            Expect(image,sample[0],sample[1],Display(Multiply(interpolated,factor)),"vertex RGBA interpolation");
        }
        Expect(image,8,8,{18,28,48,255},"clear display color",1);
        std::puts("PASS vertex RGB interpolation, material multiplication and untouched clear pixels.");
    }

    void Coverage(bool advanced,uint32_t layers=1)
    {
        auto config=Config(advanced);config.advanced.transparencyLayers=layers;
        const RGB foreground{.5,.08,.015},background{.02,.6,.08};
        const std::array<MaterialAlphaMode,3> modes={MaterialAlphaMode::Opaque,MaterialAlphaMode::Mask,MaterialAlphaMode::Blend};
        const char* names[]={"opaque","mask","blend"};
        for(size_t mode=0;mode<modes.size();++mode)
        {
            Scene scene;auto material=Unlit(foreground);material.alphaMode=modes[mode];
            Require(bool(scene.Add(Quad(-.85f,-.1f,-.75f,.75f,0,{1,1,1,0}),material)),"Cannot add uncovered foreground.");
            Require(bool(scene.Add(Quad(.1f,.85f,-.75f,.75f,0),material)),"Cannot add covered foreground.");
            // Register the farther geometry last: MASK discard must leave its depth visible.
            Require(bool(scene.Add(Quad(-.95f,.95f,-.9f,.9f,-.2f),Unlit(background))),"Cannot add background.");
            const std::string name=std::string(advanced?"advanced-":"basic-")+names[mode]+
                (advanced?"-layers"+std::to_string(layers):"");
            const auto image=Render(scene,name.c_str(),config);
            Expect(image,64,128,Display(mode==0?foreground:background),name+" zero alpha/depth");
            Expect(image,192,128,Display(foreground),name+" full alpha/depth");
        }
        Scene scene;auto material=Unlit(foreground);material.alphaMode=MaterialAlphaMode::Blend;
        material.baseColor.w=.5f;
        material.baseColorTexture=Image(1,1,{255,255,255,128});
        Require(bool(scene.Add(Quad(-.8f,.8f,-.8f,.8f,0,{1,1,1,.5f}),material)),"Cannot add fractional foreground.");
        Require(bool(scene.Add(Quad(-.95f,.95f,-.9f,.9f,-.2f),Unlit(background))),"Cannot add fractional background.");
        const auto name=advanced?"advanced-blend-product-layers"+std::to_string(layers):std::string("basic-blend-product");
        const auto image=Render(scene,name.c_str(),config);
        Expect(image,128,128,BlendDisplay(foreground,background,.5*.5*(128.0/255),advanced),"factor * vertex * texture alpha");
        std::printf("PASS %s OPAQUE/MASK/BLEND coverage, alpha product and foreground/background depth partition; transparency_layers=%u.\n",
            advanced?"advanced":"basic",layers);
    }

    void Transmission(uint32_t layers)
    {
        auto config=Config(true);config.advanced.transparencyLayers=layers;
        const RGB background{.02,.6,.08},tint{.5,.25,.75};
        for(const auto mode:{MaterialAlphaMode::Opaque,MaterialAlphaMode::Mask,MaterialAlphaMode::Blend})
        {
            Scene scene;auto material=Unlit(tint);material.unlit=false;
            material.alphaMode=mode;material.transmissionFactor=1;material.indexOfRefraction=1;
            Require(bool(scene.Add(Quad(-.85f,-.1f,-.75f,.75f,0,{1,1,1,0}),material)),"Cannot add transmission alpha-zero foreground.");
            Require(bool(scene.Add(Quad(.1f,.85f,-.75f,.75f,0),material)),"Cannot add transmission foreground.");
            Require(bool(scene.Add(Quad(-.95f,.95f,-.9f,.9f,-.2f),Unlit(background))),"Cannot add transmission background.");
            const std::string name=std::string(mode==MaterialAlphaMode::Opaque?"transmission-opaque":
                mode==MaterialAlphaMode::Mask?"transmission-mask":"transmission-blend")+"-layers"+std::to_string(layers);
            const auto image=Render(scene,name.c_str(),config);
            const auto through=Display(Multiply(background,tint));
            Expect(image,64,128,mode==MaterialAlphaMode::Opaque?through:Display(background),std::string(name)+" zero coverage");
            Expect(image,192,128,through,std::string(name)+" tinted unit-IOR transmission");
        }
        std::printf("PASS optical transmission is independent of OPAQUE/MASK/BLEND alpha coverage; transparency_layers=%u.\n",layers);
    }

    void NearestDepthAlphaHoles()
    {
        const RGB background{.02,.6,.08},rear{.45,.08,.6},factor{.8,.6,.9};
        const RGB textureTint{128.0/255,192.0/255,1};
        const auto expected=Display(Multiply(rear,Multiply(factor,textureTint)));
        auto config=Config(true);config.advanced.transparencyLayers=2;
        for(const auto mode:{MaterialAlphaMode::Blend,MaterialAlphaMode::Mask})
        {
            Scene scene;auto ghost=Unlit({1,0,0});ghost.alphaMode=mode;
            // Route MASK through transparency as well, retaining its own cutoff.
            ghost.transmissionFactor=1;ghost.indexOfRefraction=1;
            ghost.baseColorTexture=Image(2,1,{255,255,255,0,255,255,255,255});
            auto vertexHole=Quad(-.85f,-.1f,-.75f,.75f,0,{1,1,1,0});
            for(auto& vertex:vertexHole.vertices)vertex.uv={.75f,.5f};
            auto textureHole=Quad(.1f,.85f,-.75f,.75f,0);
            for(auto& vertex:textureHole.vertices)vertex.uv={.25f,.5f};
            Require(bool(scene.Add(vertexHole,ghost)) && bool(scene.Add(textureHole,ghost)),"Cannot add nearest-depth alpha holes.");
            auto refractor=Unlit(factor);refractor.unlit=false;
            refractor.alphaMode=MaterialAlphaMode::Blend;refractor.transmissionFactor=1;refractor.indexOfRefraction=1;
            refractor.baseColorTexture=Image(1,1,{128,192,255,255},ColorSpace::Linear);
            Require(bool(scene.Add(Quad(-.9f,.9f,-.8f,.8f,-.1f),refractor)),"Cannot add the textured unit-IOR refractor.");
            auto rearMaterial=Unlit(rear);rearMaterial.alphaMode=MaterialAlphaMode::Blend;
            Require(bool(scene.Add(Quad(-.95f,.95f,-.9f,.9f,-.25f),rearMaterial)),"Cannot add the second real transparent layer.");
            Require(bool(scene.Add(Quad(-.98f,.98f,-.95f,.95f,-.4f),Unlit(background))),"Cannot add nearest-depth opaque background.");
            // Only the refractor and rear layer exist at each sampled pixel. A
            // ghost depth incorrectly puts both in the rear pass: the refractor
            // then samples the opaque background instead of the colored layer.
            const std::string name=mode==MaterialAlphaMode::Mask?"nearest-mask-alpha-holes":"nearest-blend-alpha-holes";
            const auto image=Render(scene,name.c_str(),config);
            Require(m_transparencyObserver->nearestDepth,name+": nearest-depth pass was not recorded.");
            Expect(image,64,128,expected,name+" vertex-alpha hole preserves refracted rear layer");
            Expect(image,192,128,expected,name+" texture-alpha hole preserves refracted rear layer");
            Expect(image,128,128,expected,name+" no-ghost control");
        }
        std::puts("PASS nearest-depth BLEND/MASK vertex/texture alpha holes preserve two real layers and textured unit-IOR refraction.");
    }

    void ProjectiveVisibility()
    {
        if(!m_device->Supports(RHI::Feature::Compute) || !m_device->Supports(RHI::Feature::IndirectDraw))
        {
            std::puts("SKIP GPU projective visibility comparison: buffer Compute/IndirectDraw unavailable.");
            return;
        }
        Scene scene;auto projective=Math::float4x4::Identity();projective.m[3]=1;
        const RGB radiance{.8,.2,.05};
        Require(bool(scene.Add(Quad(1.8f,2.2f,-.4f,.4f,.5f),Unlit(radiance),projective)),
            "Cannot add projective visibility counterexample.");
        auto config=Config();
        const auto direct=Render(scene,"projective-direct",config);
        Expect(direct,211,128,Display(radiance),"projective direct clip-space visibility");
        config.enableGpuCulling=true;
        const auto culled=Render(scene,"projective-gpu-fail-open",config);
        Expect(culled,211,128,Display(radiance),"projective GPU culling must fail open");
        std::puts("PASS visible projective geometry survives GPU visibility, matching the direct analytic pixel.");
    }

    void OrthographicSpecular()
    {
        // A plane under a directional light has the same BRDF at every pixel for parallel view rays.
        for(uint32_t path=0;path<3;++path)
        {
            MaterialDesc material;material.baseColor={.5f,.5f,.5f,path==2?.75f:1.f};
            material.metallicFactor=1;material.roughnessFactor=.25f;
            if(path==2)material.alphaMode=MaterialAlphaMode::Blend;
            Scene scene;Require(bool(scene.Add(Quad(),material)),"Orthographic specular plane failed.");
            DirectionalLight light;light.direction={0,0,1};light.color={1,1,1};light.intensity=.05f;light.castShadow=false;
            Require(bool(scene.Add(light)),"Orthographic specular light failed.");
            auto config=Config(path!=0);config.lighting.shadows=false;
            const auto image=Render(scene,("orthographic-specular-"+std::to_string(path)).c_str(),config);
            const auto* center=image.pixels.data()+size_t(128)*image.rowPitch+128*4;
            Require(center[0]>30 && center[0]<245,"Orthographic specular control must be visible and unsaturated.");
            for(uint32_t x:{64u,96u,160u,192u})
                Expect(image,x,128,{center[0],center[1],center[2],center[3]},"Orthographic view direction is constant across the plane",2);
        }
        std::puts("PASS orthographic specular: basic, advanced opaque and transparent plane pixels are spatially constant.");
    }

    void NormalsAndInstancing()
    {
        Scene scene;const Math::float3 normal{.6f,0,.8f};
        auto mesh=Quad(-.18f,.18f,-.6f,.6f,0,{.3f,.6f,.9f,1},normal);
        auto material=Unlit();material.unlit=false;
        const auto source=scene.Add(mesh,material,Math::Translation({-.65f,0,0}));
        Require(bool(source),"Cannot add normal reference.");
        Require(bool(scene.AddInstance(source,Math::Translation({-.22f,0,0}))),"Cannot add regular instance.");
        const auto mirrored=Math::Scaling({-1,1,1});
        Require(bool(scene.AddInstance(source,Math::Translation({.22f,0,0})*mirrored)),"Cannot add mirrored instance.");
        Require(bool(scene.AddInstance(source,Math::Translation({.65f,0,0})*mirrored)),"Cannot add second mirrored instance.");
        auto config=Config(true);config.advanced.surfaceDebugView=SurfaceDebugView::Normal;
        const auto instanced=Render(scene,"surface-normal-instances",config);
        Expect(instanced,45,128,{204,128,230,255},"regular inverse-transpose normal",2);
        Expect(instanced,211,128,{51,128,230,255},"mirrored inverse-transpose normal",2);
        config.enableInstancing=false;
        const auto individual=Render(scene,"surface-normal-individual",config);
        for(uint32_t x:{45u,100u,156u,211u})
        {
            const auto* p=individual.pixels.data()+size_t(128)*individual.rowPitch+x*4;
            Expect(instanced,x,128,{p[0],p[1],p[2],p[3]},"instance stream versus individual draw",1);
        }
        // A tiny but nonsingular model is still well conditioned. Its large
        // immutable geometry compensates the scale so the normal remains visible.
        Scene tinyScene;auto large=Quad(-1.8e11f,1.8e11f,-6e11f,6e11f,0,{1,1,1,1},normal);
        const auto tiny=tinyScene.Add(large,material,Math::Translation({-.3f,0,0})*Math::Scaling(1e-12f));
        Require(bool(tiny) && bool(tinyScene.AddInstance(tiny,Math::Translation({.3f,0,0})*Math::Scaling(1e-12f))),"Cannot add tiny nonsingular instances.");
        config.enableInstancing=true;
        const auto image=Render(tinyScene,"surface-normal-tiny-nonsingular",config);
        Expect(image,90,128,{204,128,230,255},"tiny nonsingular instance normal",2);
        Expect(image,166,128,{204,128,230,255},"second tiny nonsingular instance normal",2);
        std::puts("PASS mirrored culling/normals, regular versus instanced normals and tiny nonsingular transforms.");
    }

    void Bc5Normals()
    {
        if(!m_device->Supports(RHI::Format::BC5_UNORM,RHI::FormatFeature::CompressedSampling))
        {std::puts("SKIP native BC5 normals: compressed sampling unsupported.");return;}
        std::vector<uint8_t> encoded(128+16*3,0);
        const auto put=[&](size_t offset,uint32_t value){std::memcpy(encoded.data()+offset,&value,4);};
        put(0,0x20534444);put(4,124);put(8,0x21007);put(12,4);put(16,4);put(28,3);
        put(76,32);put(80,0x80000004u);put(84,0x32495441);put(88,0x59583241);put(108,0x401008);
        for(size_t offset=128;offset<encoded.size();offset+=16)
        {encoded[offset]=180;encoded[offset+1]=140;encoded[offset+8]=100;encoded[offset+9]=80;}
        Image normal;Require(LoadImage(encoded.data(),encoded.size(),normal),"BC5 fixture decode failed.");
        normal.SetColorSpace(ColorSpace::Linear);
        const Image decoded(4,4,normal.GetPixels(),ColorSpace::Linear);
        MaterialDesc material;material.normalTexture=decoded;
        Scene reference;Require(bool(reference.Add(Quad(),material)),"BC5 reference scene failed.");
        material.normalTexture=normal;
        Scene compressed;Require(bool(compressed.Add(Quad(),material)),"BC5 compressed scene failed.");
        auto config=Config(true);config.advanced.surfaceDebugView=SurfaceDebugView::Normal;
        const auto expected=Render(reference,"normal-rgba-reference",config);
        const auto actual=Render(compressed,"normal-bc5-native",config);
        const auto compare=[&](const RHI::TextureReadback& a,const RHI::TextureReadback& b,const char* label){
            for(uint32_t x:{64u,128u,192u}) {
                const auto* pixel=a.pixels.data()+size_t(128)*a.rowPitch+x*4;
                Expect(b,x,128,{pixel[0],pixel[1],pixel[2],pixel[3]},label,2);
            }
        };
        compare(expected,actual,"BC5 reconstructed normal");
        Require(normal.ReleaseDecodedPixels()>0,"BC5 decoded cache was not retained.");
        compare(expected,Render(compressed,"normal-bc5-cache-released",config),"BC5 released-cache normal");
        Require(normal.ReleaseDecodedPixels()==0,"Stable GPU BC5 binding redecoded the CPU cache.");
        // The same encoded image used as a generic color retains its public RGBA semantics.
        material.normalTexture={};material.baseColorTexture=normal;material.unlit=true;
        Scene color;Require(bool(color.Add(Quad(),material)),"BC5 generic-color scene failed.");
        material.baseColorTexture=decoded;
        Scene colorReference;Require(bool(colorReference.Add(Quad(),material)),"BC5 color reference failed.");
        config=Config();config.lighting.enabled=false;
        compare(Render(colorReference,"bc5-color-rgba-reference",config),Render(color,"bc5-color-rgba-fallback",config),"BC5 non-normal RGBA fallback");
        Image srgbNormal=normal;srgbNormal.SetColorSpace(ColorSpace::Srgb);
        Image srgbDecoded=decoded;srgbDecoded.SetColorSpace(ColorSpace::Srgb);
        material.baseColorTexture=srgbNormal;Scene srgb;Require(bool(srgb.Add(Quad(),material)),"BC5 sRGB color scene failed.");
        material.baseColorTexture=srgbDecoded;Scene srgbReference;Require(bool(srgbReference.Add(Quad(),material)),"BC5 sRGB reference failed.");
        compare(Render(srgbReference,"bc5-srgb-rgba-reference",config),Render(srgb,"bc5-srgb-rgba-fallback",config),"BC5 sRGB generic fallback");
        material.baseColorTexture={};material.baseColor={.7f,.5f,.3f,.5f};material.unlit=false;
        material.alphaMode=MaterialAlphaMode::Blend;material.roughnessFactor=1;material.normalTexture=normal;
        Scene transparent;Require(bool(transparent.Add(Quad(),material)),"BC5 transparent scene failed.");
        material.normalTexture=decoded;Scene transparentReference;
        Require(bool(transparentReference.Add(Quad(),material)),"BC5 transparent reference failed.");
        DirectionalLight light;light.direction={.3f,.2f,1};light.intensity=1;
        Require(bool(transparent.Add(light)) && bool(transparentReference.Add(light)),"BC5 transparent light failed.");
        config=Config(true);
        compare(Render(transparentReference,"bc5-transparent-rgba-reference",config),Render(transparent,"bc5-transparent-native",config),"BC5 transparent normal");
        std::puts("PASS BC5 native normal reconstruction, explicit CPU cache release and generic-color RGBA fallback.");
    }

    void ImportedModel()
    {
        if(m_options.model.empty())return;
        ModelScene scene;ModelSceneDesc desc;
        desc.path=m_options.model;desc.normalize=true;desc.normalizedSize=1.4f;
        Require(AddModelToScene(scene,desc),"Imported fixture load/instantiate failed.");
        Require(scene.GetEntityCount()!=0,"Imported fixture contains no renderable entities.");
        auto config=Config();config.lighting.enabled=false;
        Render(scene,"imported-model",config);
        std::printf("CAPTURE imported fixture entities=%u source=%s (external image/reference comparison remains separate).\n",
            scene.GetEntityCount(),m_options.model.c_str());
    }

    Image ModelFrame(ModelRenderer& model,const ModelScene& scene,const char* name)
    {
        Image image;
        // Swapchain acquisition may skip while a previous image is released.
        // Bound both retries and event waits; this is not a presentation loop.
        for(uint32_t attempt=0;attempt<16 && !image.IsValid();++attempt)
        {
            Platform::Window::PollEvents();
            Require(model.Render(scene,m_camera,&image),std::string(name)+": ModelRenderer failed.");
            if(!image.IsValid())Platform::Window::WaitEvents(.01);
        }
        Require(image.IsValid(),std::string(name)+": no readback within sixteen acquisition attempts.");
        Require(!m_renderer->GetShaders().stockShadowCoverage,
            "ModelRenderer did not restore the caller's shadow coverage setting.");
        ++m_frames;
        if(!m_options.captures.empty())
        {
            const auto path=m_options.captures/(std::string(name)+".png");
            Require(stbi_write_png(path.string().c_str(),int(image.GetWidth()),int(image.GetHeight()),4,
                image.GetPixels().data(),int(image.GetWidth()*4))!=0,"Cannot write "+path.string());
        }
        return image;
    }

    void CompareModelInteriors(const Image& vertex,const Image& compute,const char* label,
        const char* comparison="vertex/compute")
    {
        Require(vertex.GetWidth()==compute.GetWidth() && vertex.GetHeight()==compute.GetHeight(),
            "Model deformation captures have different dimensions.");
        const auto width=vertex.GetWidth(),height=vertex.GetHeight();
        const auto pixel=[width](const Image& image,uint32_t x,uint32_t y) {
            return image.GetPixels().data()+(size_t(y)*width+x)*4;
        };
        uint64_t compared=0;
        for(uint32_t y=2;y+2<height;++y)for(uint32_t x=2;x+2<width;++x)
        {
            const auto* a=pixel(vertex,x,y);
            const int chroma=std::max({int(a[0]),int(a[1]),int(a[2])})-
                std::min({int(a[0]),int(a[1]),int(a[2])});
            if(chroma<24)continue;
            if(std::max({std::abs(int(a[0])-18),std::abs(int(a[1])-28),std::abs(int(a[2])-48)})<=3)continue;
            // Compare smooth colored interiors, excluding triangle silhouettes
            // and alpha-cutoff edges affected by subpixel arithmetic ordering.
            bool interior=true;
            for(int dy=-2;dy<=2 && interior;++dy)for(int dx=-2;dx<=2;++dx)
            {
                const auto* neighbor=pixel(vertex,uint32_t(int(x)+dx),uint32_t(int(y)+dy));
                for(size_t channel=0;channel<4;++channel)
                    if(std::abs(int(a[channel])-int(neighbor[channel]))>16)interior=false;
            }
            if(!interior)continue;
            const auto* b=pixel(compute,x,y);
            for(size_t channel=0;channel<4;++channel)
                Require(std::abs(int(a[channel])-int(b[channel]))<=2,
                    std::string(label)+": reference/candidate RGBA differs in a smooth interior.");
            ++compared;
        }
        Require(compared>=64,std::string(label)+": fewer than 64 colored interior pixels; fixture comparison is inconclusive.");
        std::printf("PASS %s %s RGBA colored_interiors=%llu tolerance=2 bytes.\n",
            label,comparison,static_cast<unsigned long long>(compared));
    }

    void CompareRepeatedDeformation(const Image& reference,const Image& cached,
        const std::vector<TransparencyObserver::DeformedDraw>& referenceDraws,const char* label)
    {
        CompareModelInteriors(reference,cached,label,"repeated frames");
        Require(reference.GetPixels().size()==cached.GetPixels().size(),std::string(label)+": output byte counts differ.");
        int maximumDifference=0;
        for(size_t byte=0;byte<reference.GetPixels().size();++byte)
            maximumDifference=std::max(maximumDifference,std::abs(int(reference.GetPixels()[byte])-int(cached.GetPixels()[byte])));
        Require(maximumDifference<=2,std::string(label)+": repeated deformation differs by more than 2 bytes over the full RGBA image.");
        const auto& cachedDraws=m_transparencyObserver->deformedDraws;
        Require(!referenceDraws.empty() && referenceDraws.size()==cachedDraws.size(),
            std::string(label)+": no matching advanced stock deformation draw observations.");
        for(size_t index=0;index<referenceDraws.size();++index)
        {
            const auto& a=referenceDraws[index];const auto& b=cachedDraws[index];
            Require(a.entity==b.entity && a.geometryRevision==b.geometryRevision,
                std::string(label)+": repeated deformation changed the geometry revision.");
            Require(a.bounds.valid && b.bounds.valid &&
                a.bounds.min.x==b.bounds.min.x && a.bounds.min.y==b.bounds.min.y && a.bounds.min.z==b.bounds.min.z &&
                a.bounds.max.x==b.bounds.max.x && a.bounds.max.y==b.bounds.max.y && a.bounds.max.z==b.bounds.max.z,
                std::string(label)+": repeated deformation changed exact deformation bounds.");
            Require(b.historyValid,std::string(label)+": repeated deformation discarded previous deformation history.");
        }
        std::printf("PASS %s full_RGBA_bytes=%zu max_difference=%d exact_bounds_and_revision_draws=%zu history_preserved=1; shadow-fit/sort bound inputs unchanged for this fixture.\n",
            label,reference.GetPixels().size(),maximumDifference,referenceDraws.size());
    }

    void SkinnedModel()
    {
        if(m_options.skinnedModel.empty())return;
        if(!m_device->Supports(RHI::Feature::Compute))
        {
            std::puts("SKIP skinned RGBA comparison: Compute unavailable.");
            return;
        }
        ModelScene scene;ModelSceneDesc desc;
        desc.path=m_options.skinnedModel;desc.normalize=true;desc.normalizedSize=1.4f;
        Require(AddModelToScene(scene,desc) && scene.UpdateAnimations(0),"Skinned fixture load/pose failed.");
        bool hasSkin=false,hasColor=false,hasFractionalAlpha=false;
        for(uint32_t i=0;i<scene.GetEntityCount();++i)
            hasSkin|=scene.GetEntitySkinPaletteOffset(static_cast<EntityID>(i))!=UINT32_MAX;
        for(const auto& mesh:scene.Meshes())for(const auto& vertex:mesh->vertices)
        {
            hasColor|=vertex.color.x!=vertex.color.y || vertex.color.y!=vertex.color.z;
            hasFractionalAlpha|=vertex.color.w>0 && vertex.color.w<1;
        }
        Require(hasSkin && hasColor && hasFractionalAlpha,
            "--skinned-model requires actual skin influences, vertex RGB and fractional vertex alpha.");
        DirectionalLight light;light.direction={0,0,1};light.intensity=1;
        Require(bool(scene.Add(light)),"Cannot add the MASK shadow coverage light.");
        ModelRenderer model(*m_renderer);
        for(const auto mode:{MaterialAlphaMode::Blend,MaterialAlphaMode::Mask})
        {
            for(uint32_t i=0;i<scene.GetEntityCount();++i)
            {
                auto entity=scene.GetEntity(static_cast<EntityID>(i));auto material=entity.GetMaterial();
                material.unlit=true;material.baseColor={1,1,1,.8f};material.alphaMode=mode;
                Require(entity.SetMaterial(material),"Cannot set the skinned RGBA coverage material.");
            }
            auto config=Config();config.lighting.shadows=mode==MaterialAlphaMode::Mask;
            config.lighting.shadowQuality=ShadowQuality::Low;
            Require(m_renderer->SetConfig(config),"Skinned test configuration rejected.");
            Require(model.SetSkinningExecutionMode(SkinningExecutionMode::VertexShader),"Cannot select vertex skinning.");
            const auto vertex=ModelFrame(model,scene,mode==MaterialAlphaMode::Blend?"skin-blend-vertex":"skin-mask-shadow-vertex");
            Require(model.SetSkinningExecutionMode(SkinningExecutionMode::ComputePreSkin),"Cannot select compute skinning.");
            const auto compute=ModelFrame(model,scene,mode==MaterialAlphaMode::Blend?"skin-blend-compute":"skin-mask-shadow-compute");
            CompareModelInteriors(vertex,compute,mode==MaterialAlphaMode::Blend?"skinned BLEND":"skinned MASK + shadow pipeline");
        }
        // The basic cases above select the custom model vertex path. Advanced
        // HDR selects stock geometry and Compute deformation, including its
        // positions-only CPU bounds evaluation; repeat it with unchanged inputs.
        const uint32_t modelEntityCount=scene.GetEntityCount();
        auto receiver=Unlit({.3,.3,.3});receiver.unlit=false;
        Require(bool(scene.Add(Quad(-.95f,.95f,-.95f,.95f,-.9f),receiver)),"Cannot add the cache comparison shadow receiver.");
        for(const auto mode:{MaterialAlphaMode::Blend,MaterialAlphaMode::Mask})
        {
            for(uint32_t entityIndex=0;entityIndex<modelEntityCount;++entityIndex)
            {
                auto entity=scene.GetEntity(static_cast<EntityID>(entityIndex));auto material=entity.GetMaterial();
                material.unlit=true;material.baseColor={1,1,1,.8f};material.alphaMode=mode;
                Require(entity.SetMaterial(material),"Cannot set the advanced cache comparison material.");
            }
            auto config=Config(true);config.lighting.shadows=mode==MaterialAlphaMode::Mask;
            config.lighting.shadowQuality=ShadowQuality::Low;
            Require(m_renderer->SetConfig(config),"Advanced cache test configuration rejected.");
            const auto reference=ModelFrame(model,scene,mode==MaterialAlphaMode::Blend?
                "skin-advanced-blend-first":"skin-advanced-mask-shadow-first");
            const auto referenceDraws=m_transparencyObserver->deformedDraws;
            const auto referenceStatistics=m_renderer->GetLastRenderStatistics();
            const auto cached=ModelFrame(model,scene,mode==MaterialAlphaMode::Blend?
                "skin-advanced-blend-repeated":"skin-advanced-mask-shadow-repeated");
            if(mode==MaterialAlphaMode::Mask)
            {
                const auto& statistics=m_renderer->GetLastRenderStatistics();
                Require(referenceStatistics.shadowViewCount>0 &&
                    (referenceStatistics.shadowDrawCount>0 || referenceStatistics.reusedShadowAtlas),
                    "Advanced MASK reference did not render or reuse an actual shadow atlas.");
                Require(statistics.shadowViewCount==referenceStatistics.shadowViewCount &&
                    statistics.shadowViewResolution==referenceStatistics.shadowViewResolution &&
                    statistics.directionalShadowDistance==referenceStatistics.directionalShadowDistance,
                    "Repeated deformation changed shadow view allocation or distance.");
            }
            CompareRepeatedDeformation(reference,cached,referenceDraws,mode==MaterialAlphaMode::Blend?
                "advanced skinned BLEND":"advanced skinned MASK + lit shadow receiver");
        }
    }

    void AdditionalContracts()
    {
        if(m_options.rhiShaders.empty())
        {std::puts("SKIP added RHI/Surface contracts: pass --rhi-shader-dir with generated Canvas binaries.");return;}
        auto observer=std::make_unique<RenderingProbe::SurfaceObserver>(*m_device,m_options.rhiShaders);
        auto* probe=observer.get();probe->rhi.Run();
        Require(m_renderer->AddExtension(std::move(observer)),"Surface demand observer registration failed.");
        Scene scene;auto mesh=Quad();
        for(size_t v=0;v<mesh.vertices.size();++v)mesh.vertices[v].uv={(v==1 || v==2)?1.f:0.f,.5f};
        auto mask=Unlit({.25,.5,.75});mask.alphaMode=MaterialAlphaMode::Mask;mask.metallicFactor=1;
        mask.baseColorTexture=Image(2,1,{255,255,255,0,255,255,255,255});
        Require(bool(scene.Add(mesh,mask)),"Surface MASK fixture creation failed.");
        probe->active=true;
        for(uint32_t demand:{7u,0u,4u,1u,5u,3u})
        {
            probe->mask=demand;probe->observed=false;
            const auto image=Render(scene,("surface-demand-"+std::to_string(demand)).c_str(),Config(true));
            Expect(image,64,128,{18,28,48,255},"Surface MASK hole preserves Main clear",2);
            Expect(image,192,128,Display({.25,.5,.75}),"Surface MASK covered Main pixel",2);
            probe->Pixels();
            if(!demand)
            {
                Scene layered;Require(bool(layered.Add(mesh,mask)) &&
                    bool(layered.Add(Quad(-.95f,.95f,-.9f,.9f,-.2f),Unlit({.05,.2,.8}))),"Depth-only MASK receiver fixture failed.");
                const auto depth=Render(layered,"surface-demand-0-mask-depth",Config(true));
                Expect(depth,64,128,Display({.05,.2,.8}),"Zero-MRT MASK hole preserves rear geometry depth",2);
                Expect(depth,192,128,Display({.25,.5,.75}),"Zero-MRT MASK foreground keeps nearest depth",2);
            }
            std::printf("PASS Surface demand mask=%u actual outputs/MRT and stock MASK pixels.\n",demand);
        }
        {
            Scene temporal;Require(bool(temporal.Add(Quad(-.8f,.8f,-.8f,.8f,1.5f),Unlit())),"Orthographic motion fixture failed.");
            probe->mask=4;probe->observed=false;
            Render(temporal,"orthographic-motion-first",Config(true));
            probe->MotionDepth(.5f,0);
            // The Render helper calls SetConfig, which intentionally invalidates frame history.
            Require(m_renderer->RenderToTexture(temporal,m_camera,m_target,m_targetState),"Orthographic motion history render failed.");
            ++m_frames;
            probe->MotionDepth(.5f,.5f);
            std::puts("PASS orthographic Surface motion: current and previous linear depth are 0.5, independent of clip.w=1.");
        }
        probe->active=false;
        {
            Platform::Window window{Size,Size,"dyf depth-only conformance"};
            Require(window.GetHandle()!=nullptr,"Depth-only window creation failed.");
            RHI::DeviceDesc desc;desc.enableValidation=m_options.validation;
            std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create(desc));
            Require(bool(device),"Depth-only device creation failed.");
            auto renderer=Renderer::Create(*device,window.GetHandle(),Config(true));
            Require(bool(renderer),"Depth-only renderer creation failed.");
            Require(renderer->AddExtension(std::make_unique<RenderingProbe::DepthOnlyProvider>()),"Depth-only provider registration failed.");
            RHI::ResourceScope scope(*device);RHI::TextureDesc target;
            target.width=target.height=Size;target.format=RHI::Format::R8G8B8A8_UNORM_SRGB;
            target.usage=RHI::TextureUsage::RenderTarget|RHI::TextureUsage::ShaderResource;
            auto* output=scope.Keep(device->CreateTexture(target));
            RHI::ResourceState before=RHI::ResourceState::Undefined;
            for(uint32_t repeat=0;repeat<2;++repeat)
            {
                Require(renderer->RenderToTexture(scene,m_camera,output,before),"Depth-only/no-AO render failed.");
                before=RHI::ResourceState::ShaderResource;RHI::TextureReadback image;
                Require(device->ReadTexture(output,image),"Depth-only readback failed.");
                Expect(image,64,128,{18,28,48,255},"Depth-only MASK hole",2);
                Expect(image,192,128,Display({.25,.5,.75}),"Unproven prepass cannot preserve stale zero depth",2);
            }
            Require(device->WaitIdle(),"Depth-only final GPU wait failed.");
            std::puts("PASS depth-only custom provider: empty AO, unproven zero-depth prepass and repeated MASK pixels.");
        }
        Scene present;
        auto colored=present.Add(Quad(),Unlit());
        Require(bool(colored),"Present regression fixture failed.");
        for(bool vsync:{true,false})
        {
            auto config=Config();config.vsync=vsync;
            Require(m_renderer->SetConfig(config),"Present regression configuration failed.");
            m_window.Resize(Size,Size);
            uint32_t frames=0;bool resized=false;
            // Acquisition retries need elapsed time, independent of CPU loop speed.
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
            while(frames<40 && std::chrono::steady_clock::now()<deadline)
            {
                Require(m_window.IsRunning(),"Present regression window closed.");Platform::Window::PollEvents();
                const RGB color{.05+.01*frames,.45-.005*frames,.025+.002*frames};
                Require(colored.SetMaterial(Unlit(color)),"Presented frame material update failed.");
                Image capture;const bool sample=frames==0 || frames==13 || frames==21 || frames==39;
                Require(m_renderer->Render(present,m_camera,sample?&capture:nullptr),"On-window rendering/present failed.");
                if(!m_renderer->GetLastRenderStatistics().renderWidth){Platform::Window::WaitEvents(.001);continue;}
                if(sample)
                {
                    const auto extent=m_window.GetFramebufferSize();
                    Require(capture.IsValid() && capture.GetWidth()==uint32_t(extent.width) &&
                        capture.GetHeight()==uint32_t(extent.height),"Presented frame readback has a stale extent.");
                    // The default window target is UNORM; the basic output uses gamma 2.2.
                    Pixel presented{0,0,0,255};
                    for(size_t channel=0;channel<3;++channel)
                        presented[channel]=uint8_t(std::lround(255*std::pow(color[channel]/(1+color[channel]),1/2.2)));
                    RHI::TextureReadback image;image.width=capture.GetWidth();image.height=capture.GetHeight();image.rowPitch=image.width*4;
                    image.pixels=capture.GetPixels();Expect(image,image.width/2,image.height/2,presented,"Presented changing frame pixel",2);
                    if(frames>20)resized|=image.width==Size+32 && image.height==Size+16;
                }
                // Keep the next acquired image across an idle drain, and retire it on resize.
                if(frames==12)Require(m_device->WaitIdle(),"Mid-stream Present/acquire drain failed.");
                if(frames==20)m_window.Resize(Size+32,Size+16);
                ++frames;
            }
            Require(frames==40 && resized && m_device->WaitIdle(),
                "FIFO/Immediate Present lifecycle failed; completed="+std::to_string(frames));
            std::printf("PASS on-window %s: 40 changing frames, sampled pixels, idle/prefetch reuse, resize and final drain.\n",vsync?"FIFO":"Immediate");
        }
        {
            Platform::Window window{Size,Size,"dyf pending Present shutdown"};
            RHI::DeviceDesc desc;desc.enableValidation=m_options.validation;desc.maxFramesInFlight=2;
            std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create(desc));
            Require(bool(device),"Pending Present shutdown device creation failed.");
            RHI::SwapchainDesc output;output.window=window.GetHandle();output.format=RHI::DefaultSwapchainFormat;
            Require(device->CreateSwapchain(output),"Pending Present shutdown swapchain creation failed.");
            uint32_t frames=0;
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
            while(frames<3 && std::chrono::steady_clock::now()<deadline)
            {
                Platform::Window::PollEvents();
                if(!device->BeginFrame()){Require(!device->IsLost(),"Pending Present shutdown device lost.");Platform::Window::WaitEvents(.001);continue;}
                Require(device->Present(),"Pending Present shutdown submission failed.");++frames;
            }
            Require(frames==3,"Pending Present shutdown could not acquire three frames.");
            device.reset(); // Exercise the device's own worker/acquire drain without an explicit WaitIdle.
            std::puts("PASS pending Present shutdown: device destroyed before window, without explicit idle.");
        }
    }

    void Run()
    {
        VertexColors();Coverage(false);
        for(uint32_t layers:{1u,2u}){Coverage(true,layers);Transmission(layers);}
        NearestDepthAlphaHoles();ProjectiveVisibility();OrthographicSpecular();NormalsAndInstancing();Bc5Normals();ImportedModel();SkinnedModel();
        Require(m_device->WaitIdle(),"Final GPU wait failed.");
        std::printf("INFO reactive storage=%s; direct half-value precision readback unavailable (ReadTexture supports RGBA8/BGRA8 only).\n",
            m_reactiveFormat==RHI::Format::R16_FLOAT?"R16_FLOAT":"RGBA16_FLOAT");
        std::printf("PASS RenderingConformance frames=%u size=%ux%u transparency_layer1_frames=%u transparency_layer2_nearest_frames=%u; analytic tolerances are 1-2 output bytes.\n",
            m_frames,Size,Size,m_transparencyFrames[0],m_transparencyFrames[1]);
        AdditionalContracts();
    }
};
}

int main(int argc,char** argv) try
{
    Options options;
    for(int i=1;i<argc;++i)
    {
        const std::string argument=argv[i];
        if(argument=="--help")
        {
            std::puts("RenderingConformanceTests [--cpu-only] [--validation] [--capture-dir DIR] [--model PATH] [--skinned-model PATH] [--rhi-shader-dir DIR]\n"
                "Finite analytic GPU regressions; run the client inside a headless compositor.\n"
                "PNG files are written only with --capture-dir. --model adds a public Model import/capture, not an external-renderer reference assertion.\n"
                "--rhi-shader-dir adds RHI/Surface contracts and FIFO/Immediate Present lifecycle checks with generated CanvasVertex.bin/CanvasFragment.bin.\n"
                "--skinned-model compares public vertex/compute RGBA and advanced/HDR skin-transform cache OFF/ON for an actually skinned, colored fixture with fractional alpha.");
            return 0;
        }
        if(argument=="--validation")options.validation=true;
        else if(argument=="--cpu-only")options.cpuOnly=true;
        else if((argument=="--capture-dir" || argument=="--model" || argument=="--skinned-model" || argument=="--rhi-shader-dir") && i+1<argc)
        {
            if(argument=="--rhi-shader-dir")options.rhiShaders=argv[++i];
            else if(argument=="--capture-dir")options.captures=argv[++i];
            else if(argument=="--model")options.model=argv[++i];
            else options.skinnedModel=argv[++i];
        }
        else throw std::runtime_error("Unknown or incomplete argument: "+argument);
    }
    CpuVisibilityRegression();
    if(!options.cpuOnly)RenderingTests(options).Run();
    return 0;
}
catch(const std::exception& error)
{
    std::fprintf(stderr,"FAIL RenderingConformance: %s\n",error.what());
    return 1;
}

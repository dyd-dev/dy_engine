#include "EnvironmentLighting.h"
#include "SceneAlgorithms.h"
#include "dyf/RHI/ResourceSet.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <future>
#include <limits>
#include <vector>
#if defined(ENABLE_METAL)
#include "EnvironmentLibrary.h"
#elif defined(ENABLE_VULKAN) || defined(ENABLE_D3D12)
#include "EnvironmentVertex.h"
#include "EnvironmentSpecularFragment.h"
#include "EnvironmentBackgroundFragment.h"
#include "EnvironmentDiffuseFragment.h"
#endif

namespace dyf
{
namespace
{
constexpr double Pi=3.14159265358979323846;

std::array<double,9> SHBasis(Math::float3 n)
{
    return {.28209479177387814,.4886025119029199*n.y,.4886025119029199*n.z,.4886025119029199*n.x,
        1.0925484305920792*n.x*n.y,1.0925484305920792*n.y*n.z,.31539156525252005*(3*n.z*n.z-1),
        1.0925484305920792*n.x*n.z,.5462742152960396*(n.x*n.x-n.y*n.y)};
}

Math::float3 SampleRadiance(const HdrImage& image,Math::float3 direction)
{
    const double u=std::atan2(direction.y,direction.x)/(2*Pi)+.5;
    const double v=std::acos(std::clamp(double(direction.z),-1.0,1.0))/Pi;
    const double x=u*image.GetWidth()-.5,y=v*image.GetHeight()-.5;
    const int x0=static_cast<int>(std::floor(x)),y0=static_cast<int>(std::floor(y));
    const float fx=static_cast<float>(x-x0),fy=static_cast<float>(y-y0);
    const int width=static_cast<int>(image.GetWidth()),height=static_cast<int>(image.GetHeight());
    const auto pixel=[&](int px,int py) {
        px=(px%width+width)%width;py=std::clamp(py,0,height-1);
        const size_t i=(size_t(py)*width+px)*4;
        const auto& data=image.GetPixels();return Math::float3{data[i],data[i+1],data[i+2]};
    };
    return (pixel(x0,y0)*(1-fx)+pixel(x0+1,y0)*fx)*(1-fy)+
        (pixel(x0,y0+1)*(1-fx)+pixel(x0+1,y0+1)*fx)*fy;
}

float RadicalInverse(uint32_t bits)
{
    bits=(bits<<16)|(bits>>16);
    bits=((bits&0x55555555u)<<1)|((bits&0xaaaaaaaau)>>1);
    bits=((bits&0x33333333u)<<2)|((bits&0xccccccccu)>>2);
    bits=((bits&0x0f0f0f0fu)<<4)|((bits&0xf0f0f0f0u)>>4);
    bits=((bits&0x00ff00ffu)<<8)|((bits&0xff00ff00u)>>8);
    return bits*2.3283064365386963e-10f;
}

Math::float3 GGXHalf(uint32_t sample,uint32_t count,float roughness)
{
    const double phi=2*Pi*double(sample)/count;
    const double a=double(roughness)*roughness,a2=a*a;
    const double y=RadicalInverse(sample);
    const double z=std::sqrt((1-y)/(1+(a2-1)*y));
    const double r=std::sqrt(std::max(0.0,1-z*z));
    return {float(std::cos(phi)*r),float(std::sin(phi)*r),float(z)};
}

std::array<Math::float4,9> IntegrateDiffuse(const HdrImage& source)
{
    std::array<Math::float4,9> result={};
    if(!source.IsValid()) return result;
    const uint32_t width=source.GetWidth(),height=source.GetHeight();
    double coefficients[9][3]={};
    for(uint32_t y=0;y<height;++y)
    {
        const double theta=Pi*(y+.5)/height;
        const double solidAngle=2*Pi/width*(std::cos(Pi*y/height)-std::cos(Pi*(y+1)/height));
        for(uint32_t x=0;x<width;++x)
        {
            const double phi=2*Pi*((x+.5)/width-.5);
            const auto basis=SHBasis({float(std::sin(theta)*std::cos(phi)),float(std::sin(theta)*std::sin(phi)),float(std::cos(theta))});
            const size_t pixel=(size_t(y)*width+x)*4;
            for(uint32_t i=0;i<9;++i) for(uint32_t c=0;c<3;++c)
                coefficients[i][c]+=source.GetPixels()[pixel+c]*basis[i]*solidAngle;
        }
    }
    for(uint32_t i=0;i<9;++i)
    {
        const double cosine=i==0?Pi:i<4?2*Pi/3:Pi/4;
        result[i]={float(coefficients[i][0]*cosine),float(coefficients[i][1]*cosine),float(coefficients[i][2]*cosine),0};
    }
    return result;
}

uint32_t MipCount(uint32_t width,uint32_t height)
{
    uint32_t count=1;
    while(width>1 || height>1) {width=std::max(1u,width/2);height=std::max(1u,height/2);++count;}
    return count;
}

HdrImage ResamplePanorama(const HdrImage& source,uint32_t width,uint32_t height)
{
    if(source.GetWidth()==width && source.GetHeight()==height) return source;
    std::vector<float> pixels(size_t(width)*height*4);
    const uint32_t sw=source.GetWidth(),sh=source.GetHeight();
    for(uint32_t y=0;y<height;++y) for(uint32_t x=0;x<width;++x)
    {
        Math::float3 color={};
        if(width>sw || height>sh)
        {
            const double theta=Pi*(y+.5)/height,phi=2*Pi*((x+.5)/width-.5);
            color=SampleRadiance(source,{float(std::sin(theta)*std::cos(phi)),float(std::sin(theta)*std::sin(phi)),float(std::cos(theta))});
        }
        else
        {
            const double left=double(x)*sw/width,right=double(x+1)*sw/width;
            const double top=double(y)*sh/height,bottom=double(y+1)*sh/height;
            double sum[3]={},weight=0;
            for(uint32_t sy=uint32_t(top);sy<std::min(sh,uint32_t(std::ceil(bottom)));++sy)
            {
                const double row=std::cos(Pi*std::max(top,double(sy))/sh)-
                    std::cos(Pi*std::min(bottom,double(sy+1))/sh);
                for(uint32_t sx=uint32_t(left);sx<std::min(sw,uint32_t(std::ceil(right)));++sx)
                {
                    const double w=row*(std::min(right,double(sx+1))-std::max(left,double(sx)));
                    const size_t index=(size_t(sy)*sw+sx)*4;
                    for(uint32_t c=0;c<3;++c) sum[c]+=source.GetPixels()[index+c]*w;
                    weight+=w;
                }
            }
            if(weight>0) color={float(sum[0]/weight),float(sum[1]/weight),float(sum[2]/weight)};
        }
        const size_t index=(size_t(y)*width+x)*4;
        pixels[index]=color.x;pixels[index+1]=color.y;pixels[index+2]=color.z;pixels[index+3]=1;
    }
    return HdrImage(width,height,std::move(pixels));
}

std::vector<HdrImage> PanoramaMips(const HdrImage& source)
{
    std::vector<HdrImage> levels{source};
    uint32_t width=source.GetWidth(),height=source.GetHeight();
    while(width>1 || height>1)
    {
        width=std::max(1u,width/2);height=std::max(1u,height/2);
        levels.push_back(ResamplePanorama(levels.back(),width,height));
    }
    return levels;
}

Math::float3 SampleRadiance(const std::vector<HdrImage>& levels,Math::float3 direction,float lod)
{
    lod=std::clamp(lod,0.f,float(levels.size()-1));
    const auto low=static_cast<uint32_t>(lod),high=std::min(low+1,static_cast<uint32_t>(levels.size()-1));
    const float fraction=lod-low;
    return SampleRadiance(levels[low],direction)*(1-fraction)+SampleRadiance(levels[high],direction)*fraction;
}

std::vector<std::vector<float>> PrefilterRadiance(const HdrImage& source,uint32_t width,uint32_t height,
    uint32_t maxResolution,uint32_t preparationThreads)
{
    constexpr uint32_t samples=64;
    const uint32_t mipCount=MipCount(width,height);
    const auto sourceMips=PanoramaMips(source);
    const float outputFootprint=std::max(float(source.GetWidth())/width,float(source.GetHeight())/height);
    const float sourceFootprint=maxResolution ? float(std::max(source.GetWidth(),source.GetHeight()))/maxResolution : 1.f;
    const float minimumLod=std::max(0.f,std::log2(std::max(outputFootprint,sourceFootprint)));
    std::vector<std::vector<float>> result(mipCount);
    for(uint32_t mip=0;mip<mipCount;++mip)
    {
        if(!mip && minimumLod==0 && width==source.GetWidth() && height==source.GetHeight())
        {
            result[mip]=source.GetPixels();
            width=std::max(1u,width/2);height=std::max(1u,height/2);
            continue;
        }
        const float roughness=mipCount>1 ? float(mip)/float(mipCount-1) : 0;
        auto& pixels=result[mip];pixels.resize(size_t(width)*height*4);
        std::array<Math::float3,samples> halves={};
        if(mip) for(uint32_t i=0;i<samples;++i) halves[i]=GGXHalf(i,samples,roughness);
        const auto rows=[&](uint32_t begin,uint32_t end) {
          for(uint32_t y=begin;y<end;++y) for(uint32_t x=0;x<width;++x)
          {
            const double theta=Pi*(y+.5)/height,phi=2*Pi*((x+.5)/width-.5);
            const Math::float3 n{float(std::sin(theta)*std::cos(phi)),float(std::sin(theta)*std::sin(phi)),float(std::cos(theta))};
            Math::float3 color={};float weight=0;
            if(!mip) color=SampleRadiance(sourceMips,n,minimumLod);
            else
            {
                const auto tangent=Math::Normalize(Math::Cross(std::abs(n.z)<.999f?Math::float3{0,0,1}:Math::float3{1,0,0},n));
                const auto bitangent=Math::Cross(n,tangent);
                for(const auto& h:halves)
                {
                    const auto half=tangent*h.x+bitangent*h.y+n*h.z;
                    const auto direction=half*(2*Math::Dot(n,half))-n;
                    const float nDotL=std::max(Math::Dot(n,direction),0.f);
                    if(nDotL>0)
                    {
                        // GGX 표본의 입체각에 맞는 원본 밉을 읽어 작은 고휘도 광원의 분산을 줄인다.
                        const double a=double(roughness)*roughness,a2=a*a;
                        const double denominator=h.z*h.z*(a2-1)+1;
                        const double pdf=a2/(4*Pi*denominator*denominator);
                        const double theta=std::acos(std::clamp(double(direction.z),-1.0,1.0));
                        const uint32_t row=std::min(uint32_t(theta/Pi*source.GetHeight()),source.GetHeight()-1);
                        const double texelAngle=2*Pi/source.GetWidth()*(std::cos(Pi*row/source.GetHeight())-
                            std::cos(Pi*(row+1)/source.GetHeight()));
                        const float lod=float(.5*std::log2(1/(samples*pdf*texelAngle)));
                        color=color+SampleRadiance(sourceMips,direction,std::max(minimumLod,lod))*nDotL;weight+=nDotL;
                    }
                }
                if(weight>0) color=color*(1/weight);
            }
            const size_t pixel=(size_t(y)*width+x)*4;
            pixels[pixel]=color.x;pixels[pixel+1]=color.y;pixels[pixel+2]=color.z;pixels[pixel+3]=1;
          }
        };
        const uint32_t workers=std::min(std::max(1u,preparationThreads),height);
        // 비동기 작업의 future를 입력보다 먼저 폐기해 예외가 나도 모든 쓰기가 끝난 뒤 해제한다.
        std::vector<std::future<void>> tasks;tasks.reserve(workers-1);
        for(uint32_t worker=1;worker<workers;++worker)
            tasks.push_back(std::async(std::launch::async,rows,uint32_t(uint64_t(height)*worker/workers),
                uint32_t(uint64_t(height)*(worker+1)/workers)));
        rows(0,height/workers);
        for(auto& task:tasks) task.get();
        width=std::max(1u,width/2);height=std::max(1u,height/2);
    }
    return result;
}

std::vector<std::vector<float>> VisibilityMoments(const HdrImage& source,uint32_t width,uint32_t height,float& scale)
{
    scale=0;
    if(source.IsValid())
        for(size_t i=0;i<source.GetPixels().size();i+=4)
        {
            const float distance=source.GetPixels()[i],second=source.GetPixels()[i+1];
            if(!std::isfinite(distance) || distance<0 || !std::isfinite(second) || second<0) return {};
            scale=std::max(scale,distance);
        }
    HdrImage normalized;
    if(scale>0)
    {
        auto pixels=source.GetPixels();
        for(size_t i=0;i<pixels.size();i+=4)
        {
            pixels[i]=float(double(pixels[i])/scale);
            pixels[i+1]=float(double(pixels[i+1])/double(scale)/scale);pixels[i+2]=0;pixels[i+3]=1;
        }
        normalized=HdrImage(source.GetWidth(),source.GetHeight(),std::move(pixels));
    }
    else normalized=HdrImage(1,1,{0,0,0,1});
    const auto levels=PanoramaMips(ResamplePanorama(normalized,width,height));
    std::vector<std::vector<float>> result;result.reserve(levels.size());
    for(const auto& level:levels) result.push_back(level.GetPixels());
    return result;
}

bool UploadTexture(RHI::IDevice& device,RHI::ICommandList& commands,uint32_t width,uint32_t height,
    const std::vector<std::vector<float>>& levels,RHI::TextureHandle& result)
{
    using namespace RHI;
    TextureDesc desc;desc.width=width;desc.height=height;desc.mipLevels=static_cast<uint32_t>(levels.size());
    desc.format=Format::R32G32B32A32_FLOAT;desc.usage=TextureUsage::ShaderResource;
    auto* texture=device.CreateTexture(desc);
    if(!texture) return false;
    ResourceBarrierDesc barrier{nullptr,texture,ResourceState::Undefined,ResourceState::CopyDestination,{}};
    commands.ResourceBarrier(&barrier,1);
    for(uint32_t level=0;level<levels.size();++level)
    {
        const uint64_t pitch=uint64_t(width)*4*sizeof(float),size=pitch*height;
        if(size>UINT32_MAX || levels[level].size()!=size/sizeof(float) ||
            !device.UpdateTexture(commands,texture,level,0,levels[level].data(),static_cast<uint32_t>(size),
                static_cast<uint32_t>(pitch),static_cast<uint32_t>(size)))
        {device.DestroyTexture(texture);return false;}
        width=std::max(1u,width/2);height=std::max(1u,height/2);
    }
    barrier.before=ResourceState::CopyDestination;barrier.after=ResourceState::ShaderResource;
    commands.ResourceBarrier(&barrier,1);
    result=texture;return true;
}

bool UpdateSet(RHI::IDevice& device,RHI::PipelineHandle pipeline,RHI::ResourceSetHandle& set,
    const std::vector<RHI::ResourceBinding>& bindings)
{
    bool same=set && set->GetPipeline()==pipeline && set->GetBindingCount()==bindings.size();
    for(uint32_t i=0;same && i<bindings.size();++i)
    {
        const auto& old=set->GetBindings()[i];const auto& next=bindings[i];
        same=old.binding==next.binding && old.texture==next.texture && old.buffer==next.buffer && old.size==next.size;
    }
    if(same) return true;
    device.DestroyResourceSet(set);set=device.CreateResourceSet({pipeline,bindings.data(),static_cast<uint32_t>(bindings.size())});
    return set!=nullptr;
}
}

EnvironmentLighting::~EnvironmentLighting() {Reset();}

void EnvironmentLighting::Reset()
{
    if(m_device)
    {
        m_device->DestroyResourceSet(m_specularSet);m_device->DestroyResourceSet(m_backgroundSet);
        m_device->DestroyResourceSet(m_diffuseSet);
        m_device->DestroyPipeline(m_specularPipeline);m_device->DestroyPipeline(m_backgroundPipeline);
        m_device->DestroyPipeline(m_diffusePipeline);
        for(auto* shader:m_shaders) m_device->DestroyShader(shader);
        for(auto* texture:{m_radiance,m_prefilter,m_brdf,m_white,m_specular,m_blackSpecular,m_diffuse,m_probePrefilter}) m_device->DestroyTexture(texture);
        m_device->DestroyBuffer(m_constants);m_device->DestroyBuffer(m_frameBuffer);
        m_device->DestroyBuffer(m_probeData);m_device->DestroyBuffer(m_probeParameters);
    }
    m_device=nullptr;m_source={};m_diffuseSource={};m_description={};m_data={};m_irradiance={};m_frame={};
    m_prefilterMaxMagnitude=0;m_prefilterFinite=false;m_linearFiltering=false;
    m_radiance=m_prefilter=m_brdf=m_white=m_specular=nullptr;m_constants=m_frameBuffer=nullptr;
    m_blackSpecular=nullptr;
    m_constantBytes.clear();m_frameBytes.clear();
    m_diffuse=nullptr;m_diffuseSet=nullptr;m_diffusePipeline=nullptr;
    m_shaders={};m_specularPipeline=m_backgroundPipeline=nullptr;m_specularSet=m_backgroundSet=nullptr;
    m_constantsReady=m_frameReady=m_prepared=m_active=m_diffuseEnabled=m_specularEnabled=m_specularRecorded=m_diffuseRecorded=false;
    m_specularState=RHI::ResourceState::Undefined;m_specularHandle={};m_width=m_height=0;
    m_diffuseState=RHI::ResourceState::Undefined;m_diffuseHandle={};
    m_probePrefilter=nullptr;m_probeData=m_probeParameters=nullptr;
    m_probeCache.clear();m_probeIndices.clear();m_probeBytes.clear();m_probeParameterBytes.clear();m_probeReady=m_probeParametersReady=false;
}

bool EnvironmentLighting::BuildEnvironment(RHI::ICommandList& commands,const HdrImage& source,const HdrImage& diffuseSource,
    uint32_t preparationThreads)
{
    std::array<RHI::TextureHandle,4> pending={};
    const bool valid=source.IsValid();
    std::array<Math::float4,9> irradiance={};
    std::vector<std::vector<float>> radiance(1),prefilter,brdf(1);
    uint32_t width=1,height=1;
    if(valid)
    {
        if(source.GetWidth()>m_device->GetLimit(RHI::Limit::Texture2DDimension) ||
            source.GetHeight()>m_device->GetLimit(RHI::Limit::Texture2DDimension) ||
            uint64_t(source.GetWidth())*source.GetHeight()>UINT32_MAX/(4*sizeof(float))) return false;
        std::fprintf(stderr,"dyf: Preparing HDR environment: %u x %u, SH/GGX/BRDF...\n",source.GetWidth(),source.GetHeight());
        width=source.GetWidth();height=source.GetHeight();radiance[0]=source.GetPixels();
        // 직접 발광체를 제외한 파노라마로 이미 직접광에 포함된 발광을 중복 적분하지 않는다.
        const auto& diffuse=diffuseSource.IsValid() ? diffuseSource : source;
        irradiance=IntegrateDiffuse(diffuse);
        prefilter=PrefilterRadiance(source,width,height,0,preparationThreads);
    }
    else {radiance[0]={0,0,0,1};prefilter={{0,0,0,1}};}
    double prefilterMaxMagnitude=0;
    bool prefilterFinite=true;
    // Inspect the uploaded mip data once, including programmatically supplied HDR.
    for(const auto& level:prefilter) for(size_t pixel=0;pixel<level.size();pixel+=4)
        for(size_t channel=0;channel<3;++channel)
        {
            const double value=level[pixel+channel];
            if(!std::isfinite(value)) prefilterFinite=false;
            else prefilterMaxMagnitude=std::max(prefilterMaxMagnitude,std::abs(value));
        }
    {
        constexpr uint32_t lutSize=128,lutSamples=128;
        brdf[0].resize(lutSize*lutSize*4);
        for(uint32_t y=0;y<lutSize;++y) for(uint32_t x=0;x<lutSize;++x)
        {
            const float roughness=(y+.5f)/lutSize,nDotV=(x+.5f)/lutSize;
            const Math::float3 view{std::sqrt(1-nDotV*nDotV),0,nDotV};
            float a=0,b=0;const float k=roughness*roughness*.5f;
            for(uint32_t sample=0;sample<lutSamples;++sample)
            {
                const auto h=GGXHalf(sample,lutSamples,roughness);
                const float vDotH=std::max(Math::Dot(view,h),0.0f);
                const auto l=h*(2*vDotH)-view;
                const float nDotL=std::max(l.z,0.0f);
                if(nDotL<=0) continue;
                const float gV=nDotV/(nDotV*(1-k)+k),gL=nDotL/(nDotL*(1-k)+k);
                const float visibility=gV*gL*vDotH/std::max(h.z*nDotV,1e-5f);
                const float fresnel=std::pow(1-vDotH,5.0f);
                a+=(1-fresnel)*visibility;b+=fresnel*visibility;
            }
            const uint32_t index=(y*lutSize+x)*4;
            brdf[0][index]=a/lutSamples;brdf[0][index+1]=b/lutSamples;brdf[0][index+2]=0;brdf[0][index+3]=1;
        }
        if(valid) std::fprintf(stderr,"dyf: HDR environment precomputation complete; recording uploads.\n");
    }
    // The AO fallback is sampled with a native linear sampler, so keep it in
    // the same filterable half-float family as the screen-space AO target.
    RHI::TextureDesc whiteDesc;whiteDesc.width=whiteDesc.height=1;
    whiteDesc.format=RHI::Format::R16G16B16A16_FLOAT;whiteDesc.usage=RHI::TextureUsage::ShaderResource;
    pending[3]=m_device->CreateTexture(whiteDesc);
    bool whiteReady=false;
    if(pending[3])
    {
        const uint16_t white[4]={0x3c00,0x3c00,0x3c00,0x3c00};
        RHI::ResourceBarrierDesc barrier{nullptr,pending[3],RHI::ResourceState::Undefined,RHI::ResourceState::CopyDestination,{}};
        commands.ResourceBarrier(&barrier,1);
        whiteReady=m_device->UpdateTexture(commands,pending[3],0,0,white,sizeof(white),sizeof(white),sizeof(white));
        if(whiteReady)
        {
            barrier.before=RHI::ResourceState::CopyDestination;barrier.after=RHI::ResourceState::ShaderResource;
            commands.ResourceBarrier(&barrier,1);
        }
    }
    if(!whiteReady || !UploadTexture(*m_device,commands,width,height,radiance,pending[0]) ||
        !UploadTexture(*m_device,commands,width,height,prefilter,pending[1]) ||
        !UploadTexture(*m_device,commands,128u,128u,brdf,pending[2]))
    {
        for(auto* texture:pending) m_device->DestroyTexture(texture);
        return false;
    }
    m_device->DestroyResourceSet(m_specularSet);m_device->DestroyResourceSet(m_backgroundSet);
    m_device->DestroyResourceSet(m_diffuseSet);m_diffuseSet=nullptr;
    m_specularSet=m_backgroundSet=nullptr;
    for(auto* texture:{m_radiance,m_prefilter,m_brdf,m_white}) m_device->DestroyTexture(texture);
    m_radiance=pending[0];m_prefilter=pending[1];m_brdf=pending[2];m_white=pending[3];
    m_source=source;m_diffuseSource=diffuseSource.IsValid() ? diffuseSource : source;m_irradiance=irradiance;
    m_prefilterMaxMagnitude=prefilterMaxMagnitude;m_prefilterFinite=prefilterFinite;
    return true;
}

bool EnvironmentLighting::BuildProbes(RHI::ICommandList& commands,const std::vector<EnvironmentProbeDesc>& probes,
    uint32_t preparationThreads)
{
    using namespace RHI;
    const uint64_t count=probes.size();
    if(count>UINT32_MAX/sizeof(ProbeGPU)) return false;
    const uint64_t layers=std::max<uint64_t>(2,count*2);
    uint32_t width=1,height=1;
    for(const auto& probe:probes)
    {
        if(!probe.radiance.IsValid()) return false;
        const uint32_t longest=std::max(probe.radiance.GetWidth(),probe.radiance.GetHeight());
        const double ratio=probe.maxReflectionResolution && probe.maxReflectionResolution<longest ?
            double(probe.maxReflectionResolution)/longest : 1;
        width=std::max(width,std::max(1u,uint32_t(probe.radiance.GetWidth()*ratio)));
        height=std::max(height,std::max(1u,uint32_t(probe.radiance.GetHeight()*ratio)));
    }
    if(count>UINT32_MAX/sizeof(ProbeGPU) || layers>m_device->GetLimit(Limit::TextureArrayLayers) ||
        std::max<uint64_t>(1,count)*sizeof(ProbeGPU)>m_device->GetLimit(Limit::StorageBufferBytes) ||
        width>m_device->GetLimit(Limit::Texture2DDimension) || height>m_device->GetLimit(Limit::Texture2DDimension) ||
        uint64_t(width)*height>UINT32_MAX/(4*sizeof(float))) return false;
    bool changed=!m_probePrefilter || probes.size()!=m_probeIndices.size() ||
        m_probePrefilter->GetDesc().width!=width || m_probePrefilter->GetDesc().height!=height;
    for(size_t i=0;i<probes.size() && !changed;++i)
    {
        const auto& diffuse=probes[i].diffuseRadiance.IsValid()?probes[i].diffuseRadiance:probes[i].radiance;
        const auto& cached=m_probeCache[m_probeIndices[i]];
        changed=cached.source.GetPixels().data()!=probes[i].radiance.GetPixels().data() ||
            cached.diffuseSource.GetPixels().data()!=diffuse.GetPixels().data() ||
            cached.visibility.GetPixels().data()!=probes[i].visibility.GetPixels().data() ||
            cached.maxReflectionResolution!=probes[i].maxReflectionResolution;
    }
    if(!changed) return true;
    std::vector<size_t> pending;pending.reserve(probes.size());
    for(const auto& probe:probes)
    {
        if(!probe.radiance.IsValid()) return false;
        const auto& diffuse=probe.diffuseRadiance.IsValid()?probe.diffuseRadiance:probe.radiance;
        const auto cached=std::find_if(m_probeCache.begin(),m_probeCache.end(),[&](const ProbeCache& value) {
            return value.source.GetPixels().data()==probe.radiance.GetPixels().data() &&
                value.diffuseSource.GetPixels().data()==diffuse.GetPixels().data() &&
                value.visibility.GetPixels().data()==probe.visibility.GetPixels().data() &&
                value.maxReflectionResolution==probe.maxReflectionResolution && value.width==width && value.height==height;
        });
        if(cached!=m_probeCache.end()) pending.push_back(static_cast<size_t>(cached-m_probeCache.begin()));
        else
        {
            std::fprintf(stderr,"dyf: Preparing spatial environment probe %zu/%zu, SH/GGX...\n",pending.size()+1,probes.size());
            ProbeCache cache;cache.source=probe.radiance;cache.diffuseSource=diffuse;cache.visibility=probe.visibility;
            cache.width=width;cache.height=height;cache.maxReflectionResolution=probe.maxReflectionResolution;
            cache.sh=IntegrateDiffuse(diffuse);
            cache.prefilter=PrefilterRadiance(probe.radiance,width,height,probe.maxReflectionResolution,preparationThreads);
            cache.prefilterFinite=true;
            for(const auto& level:cache.prefilter) for(size_t pixel=0;pixel<level.size();pixel+=4)
                for(size_t channel=0;channel<3;++channel)
                {
                    const double value=level[pixel+channel];
                    if(!std::isfinite(value)) cache.prefilterFinite=false;
                    else cache.prefilterMaxMagnitude=std::max(cache.prefilterMaxMagnitude,std::abs(value));
                }
            cache.moments=VisibilityMoments(probe.visibility,width,height,cache.distanceScale);
            if(cache.moments.empty()) return false;
            pending.push_back(m_probeCache.size());m_probeCache.push_back(std::move(cache));
        }
    }
    TextureDesc description;description.width=width;description.height=height;
    description.depthOrArraySize=static_cast<uint32_t>(layers);description.mipLevels=MipCount(width,height);
    description.format=Format::R32G32B32A32_FLOAT;description.usage=TextureUsage::ShaderResource;
    auto* image=m_device->CreateTexture(description);if(!image) return false;
    ResourceBarrierDesc barrier{nullptr,image,ResourceState::Undefined,ResourceState::CopyDestination,{}};
    commands.ResourceBarrier(&barrier,1);
    const float black[4]={0,0,0,1};
    for(uint32_t layer=0;layer<description.depthOrArraySize;++layer)
    {
        uint32_t mipWidth=description.width,mipHeight=description.height;
        for(uint32_t mip=0;mip<description.mipLevels;++mip)
        {
            const auto* pixels=black;
            if(!probes.empty())
            {
                const auto& cache=m_probeCache[pending[layer%probes.size()]];
                pixels=layer<count ? cache.prefilter[mip].data() : cache.moments[mip].data();
            }
            const uint32_t pitch=mipWidth*4*sizeof(float),size=pitch*mipHeight;
            if(!m_device->UpdateTexture(commands,image,mip,layer,pixels,size,pitch,size))
            {m_device->DestroyTexture(image);return false;}
            mipWidth=std::max(1u,mipWidth/2);mipHeight=std::max(1u,mipHeight/2);
        }
    }
    barrier.before=ResourceState::CopyDestination;barrier.after=ResourceState::ShaderResource;
    commands.ResourceBarrier(&barrier,1);
    m_device->DestroyResourceSet(m_diffuseSet);m_diffuseSet=nullptr;
    m_device->DestroyResourceSet(m_specularSet);m_specularSet=nullptr;
    m_device->DestroyTexture(m_probePrefilter);m_probePrefilter=image;m_probeIndices=std::move(pending);
    return true;
}

bool EnvironmentLighting::UploadConstants(RHI::ICommandList& commands,RHI::BufferHandle& buffer,bool& ready,
    std::vector<uint8_t>& cachedBytes,const void* bytes,uint32_t size,RHI::BufferUsage usage)
{
    using namespace RHI;
    if(buffer && ready && cachedBytes.size()==size && std::memcmp(cachedBytes.data(),bytes,size)==0) return true;
    if(buffer && buffer->GetDesc().size!=size)
    {m_device->DestroyBuffer(buffer);buffer=nullptr;ready=false;}
    const auto state=usage==BufferUsage::Storage?ResourceState::ShaderResource:ResourceState::ConstantBuffer;
    if(!buffer) buffer=m_device->CreateBuffer({size,usage==BufferUsage::Storage?uint32_t(sizeof(ProbeGPU)):size,usage,ResourceState::CopyDestination});
    if(!buffer) return false;
    ResourceBarrierDesc barrier{buffer,nullptr,state,ResourceState::CopyDestination,{}};
    if(ready) commands.ResourceBarrier(&barrier,1);
    if(!m_device->UpdateBuffer(commands,buffer,0,bytes,size)) return false;
    barrier.before=ResourceState::CopyDestination;barrier.after=state;
    commands.ResourceBarrier(&barrier,1);
    const auto* begin=static_cast<const uint8_t*>(bytes);
    cachedBytes.assign(begin,begin+size);ready=true;return true;
}

bool EnvironmentLighting::Prepare(RHI::IDevice& device,RHI::ICommandList& uploads,const EnvironmentDesc& environment,
    const Camera& camera,uint32_t width,uint32_t height,bool ambientOcclusion,bool active,
    const Math::float4x4* backgroundProjection,uint32_t preparationThreads)
{
    using namespace RHI;
    if(m_device && m_device!=&device) Reset();
    if(!m_device) m_linearFiltering=device.Supports(Format::R32G32B32A32_FLOAT,RHI::FormatFeature::LinearFiltering);
    m_device=&device;m_prepared=false;m_specularRecorded=m_diffuseRecorded=false;
    if(!width || !height || !std::isfinite(environment.intensity) || environment.intensity<0 ||
        !std::isfinite(environment.rotationRadians)) return false;
    const auto finite=[](Math::float3 v) {return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);};
    for(const auto& probe:environment.probes)
        if(!probe.radiance.IsValid() || !finite(probe.position) || !finite(probe.boxMin) || !finite(probe.boxMax) ||
            !std::isfinite(probe.fadeDistance) || probe.fadeDistance<=0 || !std::isfinite(probe.intensity) || probe.intensity<0 ||
            !(probe.boxMin.x<probe.position.x && probe.position.x<probe.boxMax.x &&
              probe.boxMin.y<probe.position.y && probe.position.y<probe.boxMax.y &&
              probe.boxMin.z<probe.position.z && probe.position.z<probe.boxMax.z)) return false;
    m_description=environment;m_active=active && (environment.radiance.IsValid() || !environment.probes.empty());
    m_diffuseEnabled=m_active && environment.diffuse;
    m_specularEnabled=m_active && environment.specular;
    if(!m_radiance && !BuildEnvironment(uploads,environment.radiance,environment.diffuseRadiance,preparationThreads)) return false;
    const auto& diffuseSource=environment.diffuseRadiance.IsValid() ? environment.diffuseRadiance : environment.radiance;
    if(m_active && (m_source.GetPixels().data()!=environment.radiance.GetPixels().data() ||
        m_diffuseSource.GetPixels().data()!=diffuseSource.GetPixels().data()) &&
        !BuildEnvironment(uploads,environment.radiance,environment.diffuseRadiance,preparationThreads)) return false;
    if(!BuildProbes(uploads,environment.probes,preparationThreads)) return false;
    if(!m_blackSpecular)
    {
        TextureDesc desc;desc.width=desc.height=1;desc.format=Format::R16G16B16A16_FLOAT;
        desc.usage=TextureUsage::ShaderResource;
        m_blackSpecular=device.CreateTexture(desc);
        if(!m_blackSpecular) return false;
        const uint16_t black[4]={0,0,0,0};
        ResourceBarrierDesc barrier{nullptr,m_blackSpecular,ResourceState::Undefined,ResourceState::CopyDestination,{}};
        uploads.ResourceBarrier(&barrier,1);
        if(!device.UpdateTexture(uploads,m_blackSpecular,0,0,black,sizeof(black),sizeof(black),sizeof(black))) return false;
        barrier.before=ResourceState::CopyDestination;barrier.after=ResourceState::ShaderResource;
        uploads.ResourceBarrier(&barrier,1);
    }
    // Keep the reflection target and all HDRI data while disabled. A black
    // sampled image substitutes for it without clearing or registering a pass.
    for(uint32_t effect=0;effect<2;++effect)
    {
        if(!(effect ? m_specularEnabled : m_diffuseEnabled)) continue;
        auto& target=effect ? m_specular : m_diffuse;
        auto& state=effect ? m_specularState : m_diffuseState;
        auto& set=effect ? m_specularSet : m_diffuseSet;
        if(target && (target->GetDesc().width!=width || target->GetDesc().height!=height))
        {
            device.DestroyResourceSet(set);set=nullptr;
            device.DestroyTexture(target);target=nullptr;
        }
        if(!target)
        {
            TextureDesc desc;desc.width=width;desc.height=height;desc.format=Format::R16G16B16A16_FLOAT;
            desc.usage=TextureUsage::RenderTarget|TextureUsage::ShaderResource;
            target=device.CreateTexture(desc);state=ResourceState::Undefined;
            if(!target) return false;
            m_width=width;m_height=height;
        }
    }
    m_data={};
    if(m_diffuseEnabled) for(uint32_t i=0;i<9;++i) m_data.sh[i]={m_irradiance[i].x*environment.diffuseColor.x*environment.diffuseIntensity,
        m_irradiance[i].y*environment.diffuseColor.y*environment.diffuseIntensity,
        m_irradiance[i].z*environment.diffuseColor.z*environment.diffuseIntensity,0};
    m_data.settings={m_active ? 1.0f : 0.0f,environment.intensity,environment.rotationRadians,float(m_prefilter->GetDesc().mipLevels-1)};
    m_data.screen={1.0f/width,1.0f/height,ambientOcclusion ? 1.0f : 0.0f,
        float((m_diffuseEnabled ? 1u : 0u)|(m_specularEnabled ? 2u : 0u))};
    const auto& p=environment.probePosition;
    const auto& lo=environment.probeBoxMin;
    const auto& hi=environment.probeBoxMax;
    const bool local=environment.localProbe && finite(p) && finite(lo) && finite(hi) &&
        lo.x<p.x && p.x<hi.x && lo.y<p.y && p.y<hi.y && lo.z<p.z && p.z<hi.z;
    if(local)
    {
        m_data.probePosition={p.x,p.y,p.z,1};
        m_data.probeBoxMin={lo.x,lo.y,lo.z,0};
        m_data.probeBoxMax={hi.x,hi.y,hi.z,0};
    }
    std::vector<ProbeGPU> probeData(std::max<size_t>(1,environment.probes.size()));
    for(size_t i=0;i<environment.probes.size();++i)
    {
        const auto& probe=environment.probes[i];auto& data=probeData[i];
        data.positionIntensity={probe.position.x,probe.position.y,probe.position.z,probe.intensity};
        data.boxMinFade={probe.boxMin.x,probe.boxMin.y,probe.boxMin.z,probe.fadeDistance};
        data.boxMaxMip={probe.boxMax.x,probe.boxMax.y,probe.boxMax.z,float(m_probePrefilter->GetDesc().mipLevels-1)};
        if(m_diffuseEnabled) for(uint32_t coefficient=0;coefficient<9;++coefficient)
        {
            const auto& sh=m_probeCache[m_probeIndices[i]].sh[coefficient];
            data.sh[coefficient]={sh.x*environment.diffuseColor.x*environment.diffuseIntensity,
                sh.y*environment.diffuseColor.y*environment.diffuseIntensity,
                sh.z*environment.diffuseColor.z*environment.diffuseIntensity,0};
        }
        const auto& cache=m_probeCache[m_probeIndices[i]];
        data.sh[0].w=cache.distanceScale>0 ? 1.f : 0.f;data.sh[1].w=cache.distanceScale;
    }
    // Keep headroom for signed bilinear/trilinear intermediates and intensity scaling.
    const double safeMagnitude=double(std::numeric_limits<float>::max())/16.0;
    const bool finiteFallback=m_prefilterFinite && m_prefilterMaxMagnitude<=safeMagnitude &&
        m_prefilterMaxMagnitude*double(environment.intensity)<=safeMagnitude;
    // Native filtering has a narrower conservative range than shader FP32.
    // Eight signed terms of magnitude 2^28 leave headroom below 2^32.
    constexpr double hardwareMagnitude=268435456.0;
    bool hardwareFiltering=m_linearFiltering && finiteFallback && m_prefilterMaxMagnitude<=hardwareMagnitude &&
        environment.probes.size()<=16777216u;
    for(size_t i=0;i<environment.probes.size() && hardwareFiltering;++i)
    {
        const auto& cache=m_probeCache[m_probeIndices[i]];
        hardwareFiltering=cache.prefilterFinite && cache.prefilterMaxMagnitude<=hardwareMagnitude &&
            cache.prefilterMaxMagnitude*double(environment.probes[i].intensity)<=safeMagnitude;
    }
    // x=count, y=finite fallback skip, z=native HDR, w=reserved.
    const std::array<uint32_t,4> probeParameters={m_active?static_cast<uint32_t>(environment.probes.size()):0u,
        finiteFallback?1u:0u,hardwareFiltering?1u:0u,0u};
    if(!UploadConstants(uploads,m_probeData,m_probeReady,m_probeBytes,probeData.data(),
        static_cast<uint32_t>(probeData.size()*sizeof(ProbeGPU)),BufferUsage::Storage) ||
        !UploadConstants(uploads,m_probeParameters,m_probeParametersReady,m_probeParameterBytes,
            probeParameters.data(),sizeof(probeParameters))) return false;
    if(!Math::Inverse(camera.projection*camera.view,m_frame.inverseViewProjection)) return false;
    // The sky has no surface motion/depth for temporal reprojection. Render it
    // on the current camera sample grid and avoid subtracting large world positions.
    auto backgroundView=camera.view;
    backgroundView.m[12]=backgroundView.m[13]=backgroundView.m[14]=0;
    if(!Math::Inverse((backgroundProjection ? *backgroundProjection : camera.projection)*backgroundView,
        m_frame.backgroundRayToWorld)) return false;
    m_frame.cameraPosition=CameraViewPosition(camera);
    m_frame.environment=m_data;
    m_frame.specularColor={environment.specularColor.x*environment.specularIntensity,
        environment.specularColor.y*environment.specularIntensity,environment.specularColor.z*environment.specularIntensity,0};
    if(!UploadConstants(uploads,m_constants,m_constantsReady,m_constantBytes,&m_data,sizeof(m_data))) return false;
    // Neutral environment bindings remain available without uploading a camera
    // buffer that only the environment's own GPU passes consume.
    const bool needsGpuPass=m_diffuseEnabled || m_specularEnabled || IsBackgroundEnabled();
    if(needsGpuPass && (!UploadConstants(uploads,m_frameBuffer,m_frameReady,m_frameBytes,&m_frame,sizeof(m_frame)) ||
        !CreatePipelines())) return false;
    m_prepared=true;return true;
}

bool EnvironmentLighting::CreatePipelines()
{
    using namespace RHI;
    if(m_specularPipeline && m_backgroundPipeline && m_diffusePipeline) return true;
    std::array<ShaderDesc,4> source={};
#if defined(ENABLE_METAL)
    source={ShaderDesc{ShaderStage::Vertex,"environmentVertex",kEnvironmentLibrary,kEnvironmentLibrarySize},
        ShaderDesc{ShaderStage::Fragment,"environmentSpecular",kEnvironmentLibrary,kEnvironmentLibrarySize},
        ShaderDesc{ShaderStage::Fragment,"environmentBackground",kEnvironmentLibrary,kEnvironmentLibrarySize},
        ShaderDesc{ShaderStage::Fragment,"environmentDiffuse",kEnvironmentLibrary,kEnvironmentLibrarySize}};
#elif defined(ENABLE_VULKAN) || defined(ENABLE_D3D12)
    source={ShaderDesc{ShaderStage::Vertex,"main",kEnvironmentVertex,kEnvironmentVertexSize},
        ShaderDesc{ShaderStage::Fragment,"main",kEnvironmentSpecularFragment,kEnvironmentSpecularFragmentSize},
        ShaderDesc{ShaderStage::Fragment,"main",kEnvironmentBackgroundFragment,kEnvironmentBackgroundFragmentSize},
        ShaderDesc{ShaderStage::Fragment,"main",kEnvironmentDiffuseFragment,kEnvironmentDiffuseFragmentSize}};
#else
    return false;
#endif
    for(uint32_t i=0;i<m_shaders.size();++i)
    {if(!m_shaders[i]) m_shaders[i]=m_device->CreateShader(source[i]);if(!m_shaders[i]) return false;}
    // RGBA32F filtering is optional on some mobile devices. Shaders reconstruct
    // bilinear/trilinear filtering with texel reads, preserving the HDR range.
    SamplerDesc linear;linear.minFilter=linear.magFilter=linear.mipFilter=SamplerFilter::Nearest;
    linear.addressU=SamplerAddressMode::Repeat;linear.addressV=linear.addressW=SamplerAddressMode::ClampToEdge;
    linear.mipLodBias=linear.minLod=0;linear.maxLod=32;
    SamplerDesc nearest=linear;
    nearest.addressU=SamplerAddressMode::ClampToEdge;nearest.maxLod=0;
    SamplerDesc aoSampler=nearest;
    aoSampler.minFilter=aoSampler.magFilter=SamplerFilter::Linear;
    std::vector<ResourceBindingLayout> bindings;
    for(uint32_t i=0;i<6;++i) bindings.push_back({i,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}});
    bindings.push_back({7,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,linear});
    bindings.push_back({8,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,nearest});
    bindings.push_back({9,ResourceBindingType::ConstantBuffer,1,ShaderStageFlags::Fragment,{}});
    bindings.push_back({10,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,aoSampler});
    auto environmentLinear=linear;
    if(m_linearFiltering) environmentLinear.minFilter=environmentLinear.magFilter=environmentLinear.mipFilter=SamplerFilter::Linear;
    bindings.push_back({15,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,environmentLinear});
    bindings.push_back({22,ResourceBindingType::ReadOnlyStorageBuffer,1,ShaderStageFlags::Fragment,{}});
    bindings.push_back({23,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}});
    bindings.push_back({24,ResourceBindingType::ConstantBuffer,1,ShaderStageFlags::Fragment,{}});
    const ColorAttachmentDesc color{Format::R16G16B16A16_FLOAT,{},ColorWriteMask::All};
    GraphicsPipelineDesc desc;desc.vertexShader=m_shaders[0];desc.fragmentShader=m_shaders[1];
    desc.topology=PrimitiveTopology::TriangleList;
    desc.raster={FillMode::Solid,CullMode::None,FrontFace::CounterClockwise,0,0,0};
    desc.colorAttachments=&color;desc.colorAttachmentCount=1;
    desc.layout={bindings.data(),static_cast<uint32_t>(bindings.size()),0,ShaderStageFlags::None,0};
    if(!m_specularPipeline) m_specularPipeline=m_device->CreateGraphicsPipeline(desc);
    if(!m_specularPipeline) return false;
    bindings={{0,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}},
        {6,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}},
        {7,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,linear},
        {9,ResourceBindingType::ConstantBuffer,1,ShaderStageFlags::Fragment,{}}};
    desc.fragmentShader=m_shaders[2];
    desc.layout={bindings.data(),static_cast<uint32_t>(bindings.size()),0,ShaderStageFlags::None,0};
    if(!m_backgroundPipeline) m_backgroundPipeline=m_device->CreateGraphicsPipeline(desc);
    if(!m_backgroundPipeline) return false;
    bindings={{0,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}},
        {1,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}},
        {3,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}},
        {8,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,nearest},
        {10,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,aoSampler},
        {9,ResourceBindingType::ConstantBuffer,1,ShaderStageFlags::Fragment,{}},
        {22,ResourceBindingType::ReadOnlyStorageBuffer,1,ShaderStageFlags::Fragment,{}},
        {23,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}},
        {24,ResourceBindingType::ConstantBuffer,1,ShaderStageFlags::Fragment,{}}};
    desc.fragmentShader=m_shaders[3];
    desc.layout={bindings.data(),static_cast<uint32_t>(bindings.size()),0,ShaderStageFlags::None,0};
    if(!m_diffusePipeline) m_diffusePipeline=m_device->CreateGraphicsPipeline(desc);
    return m_diffusePipeline!=nullptr;
}

bool EnvironmentLighting::Import(RHI::RenderGraph& graph)
{
    if(!m_prepared) return false;
    m_specularHandle=graph.ImportTexture("EnvironmentSpecular",GetSpecular(),
        m_specularEnabled ? m_specularState : RHI::ResourceState::ShaderResource,RHI::ResourceState::ShaderResource);
    m_diffuseHandle=graph.ImportTexture("EnvironmentDiffuse",GetDiffuse(),
        m_diffuseEnabled ? m_diffuseState : RHI::ResourceState::ShaderResource,RHI::ResourceState::ShaderResource);
    return m_specularHandle.IsValid() && m_diffuseHandle.IsValid();
}

void EnvironmentLighting::ReadProbes(RHI::RenderGraph& graph,RHI::RenderGraphPass& pass)
{
    using namespace RHI;
    pass.Read(graph.ImportBuffer("Spatial probe SH and bounds",m_probeData,ResourceState::ShaderResource,ResourceState::ShaderResource),ResourceState::ShaderResource)
        .Read(graph.ImportBuffer("Spatial probe count",m_probeParameters,ResourceState::ConstantBuffer,ResourceState::ConstantBuffer),ResourceState::ConstantBuffer)
        .Read(graph.ImportTexture("Spatial probe GGX panoramas",m_probePrefilter,ResourceState::ShaderResource,ResourceState::ShaderResource),ResourceState::ShaderResource);
}

void EnvironmentLighting::Draw(RHI::ICommandList& commands,RHI::PipelineHandle pipeline,RHI::ResourceSetHandle resources,
    RHI::TextureHandle target,bool load)
{
    using namespace RHI;
    ColorAttachment color;color.texture=target;color.loadOp=load ? LoadOp::Load : LoadOp::Discard;color.storeOp=StoreOp::Store;
    commands.BeginRendering({&color,1,nullptr});commands.BindGraphicsPipeline(pipeline);commands.BindResourceSet(resources);
    commands.SetViewport({0,0,float(target->GetDesc().width),float(target->GetDesc().height),0,1});
    commands.SetScissor({0,0,target->GetDesc().width,target->GetDesc().height});
    commands.DrawInstanced(3,1,0,0);commands.EndRendering();
}

bool EnvironmentLighting::AddDiffuse(RHI::RenderGraph& graph,RHI::RGResourceHandle depth,RHI::TextureHandle depthTexture,
    RHI::RGResourceHandle normal,RHI::TextureHandle normalTexture,RHI::RGResourceHandle ao,RHI::TextureHandle aoTexture)
{
    using namespace RHI;
    if(!m_prepared) return false;
    if(!m_diffuseEnabled) return true;
    if(!m_diffuseHandle.IsValid() || !depthTexture || !normalTexture) return false;
    const std::vector<ResourceBinding> bindings={{0,0,nullptr,depthTexture,0,0,{}},{1,0,nullptr,normalTexture,0,0,{}},
        {3,0,nullptr,aoTexture ? aoTexture : m_white,0,0,{}},{9,0,m_frameBuffer,nullptr,0,sizeof(m_frame),{}},
        {22,0,m_probeData,nullptr,0,m_probeData->GetDesc().size,{}},{23,0,nullptr,m_probePrefilter,0,0,{}},
        {24,0,m_probeParameters,nullptr,0,16,{}}};
    if(!UpdateSet(*m_device,m_diffusePipeline,m_diffuseSet,bindings)) return false;
    auto& pass=graph.AddPass("EnvironmentDiffuse", RHI::RGRecordingMode::Independent);
    ReadProbes(graph,pass);
    pass.Read(depth,ResourceState::ShaderResource).Read(normal,ResourceState::ShaderResource)
        .Write(m_diffuseHandle,ResourceState::RenderTarget);
    if(ao.IsValid() && aoTexture) pass.Read(ao,ResourceState::ShaderResource);
    const auto set=m_diffuseSet;
    pass.SetExecute([this,set](ICommandList* commands) {Draw(*commands,m_diffusePipeline,set,m_diffuse,false);});
    m_diffuseRecorded=true;return true;
}

bool EnvironmentLighting::UsesNativeFiltering() const noexcept
{
    uint32_t flag=0;
    if(m_probeParameterBytes.size()>=3*sizeof(uint32_t))
        std::memcpy(&flag,m_probeParameterBytes.data()+2*sizeof(uint32_t),sizeof(flag));
    return flag==1u;
}

bool EnvironmentLighting::AddSpecular(RHI::RenderGraph& graph,RHI::RGResourceHandle depth,RHI::TextureHandle depthTexture,
    RHI::RGResourceHandle normal,RHI::TextureHandle normalTexture,RHI::RGResourceHandle albedo,RHI::TextureHandle albedoTexture,
    RHI::RGResourceHandle ao,RHI::TextureHandle aoTexture)
{
    using namespace RHI;
    if(!m_prepared) return false;
    if(!m_specularEnabled) return true;
    if(!m_specularHandle.IsValid() || !depthTexture || !normalTexture || !albedoTexture) return false;
    std::vector<ResourceBinding> bindings={{0,0,nullptr,depthTexture,0,0,{}},{1,0,nullptr,normalTexture,0,0,{}},
        {2,0,nullptr,albedoTexture,0,0,{}},{3,0,nullptr,aoTexture ? aoTexture : m_white,0,0,{}},
        {4,0,nullptr,m_prefilter,0,0,{}},{5,0,nullptr,m_brdf,0,0,{}},
        {9,0,m_frameBuffer,nullptr,0,sizeof(m_frame),{}},
        {22,0,m_probeData,nullptr,0,m_probeData->GetDesc().size,{}},{23,0,nullptr,m_probePrefilter,0,0,{}},
        {24,0,m_probeParameters,nullptr,0,16,{}}};
    if(!UpdateSet(*m_device,m_specularPipeline,m_specularSet,bindings)) return false;
    auto& pass=graph.AddPass(UsesNativeFiltering()?"EnvironmentSpecular Native filter":"EnvironmentSpecular",
        RHI::RGRecordingMode::Independent);
    ReadProbes(graph,pass);
    pass.Read(depth,ResourceState::ShaderResource).Read(normal,ResourceState::ShaderResource)
        .Read(albedo,ResourceState::ShaderResource).Write(m_specularHandle,ResourceState::RenderTarget);
    if(ao.IsValid() && aoTexture) pass.Read(ao,ResourceState::ShaderResource);
    const auto set=m_specularSet;
    pass.SetExecute([this,set](ICommandList* commands) {Draw(*commands,m_specularPipeline,set,m_specular,false);});
    m_specularRecorded=true;return true;
}

bool EnvironmentLighting::AddBackground(RHI::RenderGraph& graph,RHI::RGResourceHandle hdr,RHI::TextureHandle hdrTexture,
    RHI::RGResourceHandle depth,RHI::TextureHandle depthTexture)
{
    using namespace RHI;
    if(!m_prepared) return false;
    if(!IsBackgroundEnabled()) return true;
    if(!hdrTexture || !depthTexture) return false;
    const std::vector<ResourceBinding> bindings={{0,0,nullptr,depthTexture,0,0,{}},{6,0,nullptr,m_radiance,0,0,{}},
        {9,0,m_frameBuffer,nullptr,0,sizeof(m_frame),{}}};
    if(!UpdateSet(*m_device,m_backgroundPipeline,m_backgroundSet,bindings)) return false;
    const auto set=m_backgroundSet;
    graph.AddPass("EnvironmentSky", RHI::RGRecordingMode::Independent).Read(depth,ResourceState::ShaderResource).Write(hdr,ResourceState::RenderTarget)
        .SetExecute([this,set,hdrTexture](ICommandList* commands) {Draw(*commands,m_backgroundPipeline,set,hdrTexture,true);});
    return true;
}

void EnvironmentLighting::CommitFrame()
{
    if(m_specularRecorded) m_specularState=RHI::ResourceState::ShaderResource;
    if(m_diffuseRecorded) m_diffuseState=RHI::ResourceState::ShaderResource;
    m_specularRecorded=m_diffuseRecorded=false;
}
}

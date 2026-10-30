#include "TemporalPostProcess.h"
#include "ExposureMeter.h"
#include "OutputColor.h"

#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/Pipeline.h"
#include "dyf/RHI/Shader.h"
#include "dyf/RHI/Texture.h"
#include "dyf/RHI/ResourceSet.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#if defined(ENABLE_METAL) || defined(ENABLE_D3D12) || defined(ENABLE_VULKAN)
#include "TemporalVertex.h"
#include "TemporalResolve.h"
#include "BloomDown.h"
#include "BloomUp.h"
#include "TemporalFilmic.h"
#endif

namespace dyf
{
    namespace
    {
        float Halton(uint64_t index, uint32_t base)
        {
            float result = 0, fraction = 1;
            while(index)
            {
                fraction /= static_cast<float>(base);
                result += fraction * static_cast<float>(index % base);
                index /= base;
            }
            return result;
        }

        std::array<RHI::ShaderDesc, 5> Sources()
        {
            using namespace RHI;
#if defined(ENABLE_METAL)
            constexpr const char* vertex = "vertexMain";
            constexpr const char* fragment = "fragmentMain";
#elif defined(ENABLE_D3D12) || defined(ENABLE_VULKAN)
            constexpr const char* vertex = "main";
            constexpr const char* fragment = "main";
#endif
#if defined(ENABLE_METAL) || defined(ENABLE_D3D12) || defined(ENABLE_VULKAN)
            return {{{ShaderStage::Vertex, vertex, kTemporalVertex, kTemporalVertexSize},
                {ShaderStage::Fragment, fragment, kTemporalResolve, kTemporalResolveSize},
                {ShaderStage::Fragment, fragment, kBloomDown, kBloomDownSize},
                {ShaderStage::Fragment, fragment, kBloomUp, kBloomUpSize},
                {ShaderStage::Fragment, fragment, kTemporalFilmic, kTemporalFilmicSize}}};
#else
            std::array<ShaderDesc, 5> result{};
            result[0].stage = ShaderStage::Vertex;
            for(uint32_t i = 1; i < result.size(); ++i) result[i].stage = ShaderStage::Fragment;
            return result;
#endif
        }
    }

    TemporalPostProcess::TemporalPostProcess(RHI::IDevice& device) : m_device(device) {}

    TemporalPostProcess::~TemporalPostProcess()
    {
        m_exposure.reset();
        ReleaseTargets();
        for(auto pipeline : m_pipelines) if(pipeline) m_device.DestroyPipeline(pipeline);
        for(auto shader : m_shaders) if(shader) m_device.DestroyShader(shader);
    }

    void TemporalPostProcess::ReleaseSets()
    {
        for(auto& entry : m_sets) m_device.DestroyResourceSet(entry.set);
        m_sets.clear();
    }

    void TemporalPostProcess::ReleaseTargets()
    {
        ReleaseSets();
        for(auto& target : m_history)
        {
            if(target.image) m_device.DestroyTexture(target.image);
            target = {};
        }
        for(auto& target : m_down) if(target.image) m_device.DestroyTexture(target.image);
        for(auto& target : m_up) if(target.image) m_device.DestroyTexture(target.image);
        m_down.clear();
        m_up.clear();
        m_width = m_height = 0;
        InvalidateHistory();
    }

    bool TemporalPostProcess::CreateTarget(Target& target, uint32_t width, uint32_t height)
    {
        if(target.image) return true;
        RHI::TextureDesc desc;
        desc.width = width;
        desc.height = height;
        desc.format = RHI::Format::R16G16B16A16_FLOAT;
        desc.usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::ShaderResource;
        target.image = m_device.CreateTexture(desc);
        target.state = RHI::ResourceState::Undefined;
        return target.image != nullptr;
    }

    bool TemporalPostProcess::PreparePrograms(RHI::Format outputFormat)
    {
        using namespace RHI;
        if(m_outputFormat != outputFormat && m_pipelines[Filmic])
        {
            ReleaseSets();
            m_device.DestroyPipeline(m_pipelines[Filmic]);
            m_pipelines[Filmic] = nullptr;
        }
        const auto sources = Sources();
        for(uint32_t i = 0; i < m_shaders.size(); ++i)
        {
            if(!m_shaders[i]) m_shaders[i] = m_device.CreateShader(sources[i]);
            if(!m_shaders[i]) return false;
        }
        SamplerDesc sampler;
        sampler.minFilter = sampler.magFilter = SamplerFilter::Linear;
        sampler.mipFilter = SamplerFilter::Nearest;
        sampler.addressU = sampler.addressV = sampler.addressW = SamplerAddressMode::ClampToEdge;
        sampler.mipLodBias = sampler.minLod = sampler.maxLod = 0;
        for(uint32_t program = 0; program < ProgramCount; ++program)
        {
            if(m_pipelines[program]) continue;
            const uint32_t inputCount = program == Resolve ? 4 : program == Downsample ? 1 : program == Filmic ? 3 : 2;
            std::vector<ResourceBindingLayout> bindings;
            for(uint32_t binding = 0; binding < inputCount; ++binding)
                bindings.push_back({binding, ResourceBindingType::SampledTexture, 1, ShaderStageFlags::Fragment, {}});
            bindings.push_back({8, ResourceBindingType::StaticSampler, 1, ShaderStageFlags::Fragment, sampler});
            const ColorAttachmentDesc color{program == Filmic ? outputFormat : Format::R16G16B16A16_FLOAT,
                {}, ColorWriteMask::All};
            GraphicsPipelineDesc desc;
            desc.vertexShader = m_shaders[0];
            desc.fragmentShader = m_shaders[program + 1];
            desc.topology = PrimitiveTopology::TriangleList;
            desc.raster = {FillMode::Solid, CullMode::None, FrontFace::CounterClockwise, 0, 0, 0};
            desc.colorAttachments = &color;
            desc.colorAttachmentCount = 1;
            desc.layout = {bindings.data(), static_cast<uint32_t>(bindings.size()),
                program == Resolve || program == Filmic ? 112u : 32u, ShaderStageFlags::Fragment, 15};
            m_pipelines[program] = m_device.CreateGraphicsPipeline(desc);
            if(!m_pipelines[program]) return false;
        }
        m_outputFormat = outputFormat;
        return true;
    }

    bool TemporalPostProcess::Prepare(uint32_t width, uint32_t height,
        RHI::Format outputFormat, const Settings& settings)
    {
        if(!width || !height || outputFormat == RHI::Format::Unknown ||
            !std::isfinite(settings.bloomStrength) || settings.bloomStrength < 0 ||
            !std::isfinite(settings.bloomThreshold) || settings.bloomThreshold < 0 ||
            !std::isfinite(settings.bloomKnee) || settings.bloomKnee < 0 ||
            !std::isfinite(settings.historyWeight) || settings.historyWeight < 0 || settings.historyWeight >= 1 ||
            !std::isfinite(settings.exposureKey) || settings.exposureKey <= 0 ||
            !std::isfinite(settings.exposureAdaptationSpeed) || settings.exposureAdaptationSpeed < 0 ||
            !ValidateOutputConfig(settings.output))
            return false;
        if(width != m_width || height != m_height) ReleaseTargets();
        if(settings.taa != m_settings.taa || settings.bloom != m_settings.bloom || settings.debugColor != m_settings.debugColor ||
            settings.autoExposure != m_settings.autoExposure || settings.exposureKey != m_settings.exposureKey ||
            settings.exposureAdaptationSpeed != m_settings.exposureAdaptationSpeed ||
            settings.bloomStrength != m_settings.bloomStrength ||
            settings.bloomThreshold != m_settings.bloomThreshold ||
            settings.bloomKnee != m_settings.bloomKnee || settings.historyWeight != m_settings.historyWeight)
            InvalidateHistory();
        m_settings = settings;
        m_pending = m_pendingHistory = m_pendingBloom = false;
        if(!PreparePrograms(outputFormat)) return false;
        m_width = width;
        m_height = height;
        if(settings.autoExposure && !settings.debugColor)
        {
            if(!m_exposure) m_exposure=std::make_unique<ExposureMeter>(m_device);
            if(!m_exposure->Prepare(width,height,m_shaders[0],settings.exposureAdaptationSpeed)) return false;
        }
        if(settings.taa)
            for(auto& target : m_history) if(!CreateTarget(target, width, height)) return false;
        if(settings.bloom && m_down.empty())
        {
            uint32_t levelWidth = width, levelHeight = height;
            do
            {
                levelWidth = std::max(1u, (levelWidth + 1) / 2);
                levelHeight = std::max(1u, (levelHeight + 1) / 2);
                m_down.emplace_back();
                if(!CreateTarget(m_down.back(), levelWidth, levelHeight))
                {
                    ReleaseTargets();
                    return false;
                }
            } while(m_down.size() < 6 && (levelWidth > 1 || levelHeight > 1));
            m_up.resize(m_down.size() - 1);
            for(uint32_t i = 0; i < m_up.size(); ++i)
                if(!CreateTarget(m_up[i], m_down[i].image->GetDesc().width, m_down[i].image->GetDesc().height))
                {
                    ReleaseTargets();
                    return false;
                }
        }
        ++m_cacheFrame;
        const uint64_t retainedFrames = std::max(2u, m_device.GetDesc().maxFramesInFlight);
        m_sets.erase(std::remove_if(m_sets.begin(), m_sets.end(), [&](const CachedSet& entry) {
            if(m_cacheFrame - entry.used <= retainedFrames) return false;
            m_device.DestroyResourceSet(entry.set);
            return true;
        }), m_sets.end());
        return true;
    }

    Math::float2 TemporalPostProcess::JitterPixels() const
    {
        if(!m_settings.taa) return {0, 0};
        const auto sample = m_frame % 16 + 1;
        return {Halton(sample, 2) - 0.5f, Halton(sample, 3) - 0.5f};
    }

    void TemporalPostProcess::ApplyJitter(Camera& camera, Math::float2 pixels, uint32_t width, uint32_t height)
    {
        if(!width || !height) return;
        const float x = 2 * pixels.x / static_cast<float>(width);
        const float y = -2 * pixels.y / static_cast<float>(height);
        // clip.xy += jitterNDC * clip.w; works for perspective and orthographic.
        for(uint32_t column = 0; column < 4; ++column)
        {
            camera.projection.m[column * 4] += x * camera.projection.m[column * 4 + 3];
            camera.projection.m[column * 4 + 1] += y * camera.projection.m[column * 4 + 3];
        }
    }

    void TemporalPostProcess::SetSkyReprojection(const Camera& current,const Math::float4x4& previousView,
        const Math::float4x4& previousProjection,bool validHistory)
    {
        m_skyCurrentToPreviousClip={};
        if(!validHistory || !m_settings.taa || std::abs(current.projection.m[15])>1e-6f ||
            std::abs(previousProjection.m[15])>1e-6f) return;
        auto rotation=current.view,previousRotation=previousView;
        rotation.m[12]=rotation.m[13]=rotation.m[14]=0;
        previousRotation.m[12]=previousRotation.m[13]=previousRotation.m[14]=0;
        if(std::memcmp(rotation.m,previousRotation.m,sizeof(rotation.m))==0 &&
            std::memcmp(current.projection.m,previousProjection.m,sizeof(previousProjection.m))==0)
        {
            m_skyCurrentToPreviousClip=Math::float4x4::Identity();
            return;
        }
        Math::float4x4 inverseCurrent;
        if(Math::Inverse(current.projection*rotation,inverseCurrent))
            m_skyCurrentToPreviousClip=previousProjection*previousRotation*inverseCurrent;
    }

    RHI::ResourceSetHandle TemporalPostProcess::ResourceSet(Program program,
        const std::array<RHI::TextureHandle, 4>& images)
    {
        for(auto& entry : m_sets)
            if(entry.pipeline == m_pipelines[program] && entry.images == images)
            {
                entry.used = m_cacheFrame;
                return entry.set;
            }
        std::vector<RHI::ResourceBinding> bindings;
        for(uint32_t i = 0; i < images.size(); ++i)
        {
            if(!images[i]) continue;
            RHI::ResourceBinding binding;
            binding.binding = i;
            binding.texture = images[i];
            bindings.push_back(binding);
        }
        auto* set = m_device.CreateResourceSet({m_pipelines[program], bindings.data(), static_cast<uint32_t>(bindings.size())});
        if(!set) return nullptr;
        try { m_sets.push_back({set, m_pipelines[program], images, m_cacheFrame}); }
        catch(...) { m_device.DestroyResourceSet(set); throw; }
        return set;
    }

    bool TemporalPostProcess::AddDraw(RHI::RenderGraph& graph, const char* name, Program program,
        const std::array<RHI::TextureHandle, 4>& images,
        const std::array<RHI::RGResourceHandle, 4>& reads,
        RHI::TextureHandle output, RHI::RGResourceHandle outputGraph,
        const std::array<float, 8>& settings)
    {
        auto* set = ResourceSet(program, images);
        if(!set || !output || !outputGraph.IsValid()) return false;
        const auto pipeline = m_pipelines[program];
        const bool temporal = program == Resolve;
        const bool filmic = program == Filmic;
        const auto outputConstants=filmic?OutputConstants(m_settings.output,output->GetDesc().width,
            output->GetDesc().height,m_settings.debugColor):std::array<float,20>{};
        const auto skyReprojection=m_skyCurrentToPreviousClip;
        std::array<float, 4> jitter = {};
        if(temporal)
        {
            const auto current = JitterPixels();
            jitter = {current.x / m_width, current.y / m_height,
                m_previousJitterPixels.x / m_width, m_previousJitterPixels.y / m_height};
        }
        auto& pass = graph.AddPass(name, RHI::RGRecordingMode::Independent);
        for(const auto& read : reads) if(read.IsValid()) pass.Read(read, RHI::ResourceState::ShaderResource);
        pass.Write(outputGraph, RHI::ResourceState::RenderTarget).SetPipeline(pipeline);
        pass.SetExecute([pipeline, set, output, settings, temporal, filmic,outputConstants,jitter,skyReprojection](RHI::ICommandList* commands) {
            const auto& desc = output->GetDesc();
            RHI::ColorAttachment color;
            color.texture = output;
            color.loadOp = RHI::LoadOp::Discard;
            color.storeOp = RHI::StoreOp::Store;
            commands->BeginRendering({&color, 1, nullptr});
            commands->BindGraphicsPipeline(pipeline);
            commands->BindResourceSet(set);
            commands->SetViewport({0, 0, static_cast<float>(desc.width), static_cast<float>(desc.height), 0, 1});
            commands->SetScissor({0, 0, desc.width, desc.height});
            commands->SetInlineConstants(0, sizeof(settings), settings.data());
            if(filmic) commands->SetInlineConstants(sizeof(settings),sizeof(outputConstants),outputConstants.data());
            if(temporal) commands->SetInlineConstants(sizeof(settings), sizeof(jitter), jitter.data());
            if(temporal) commands->SetInlineConstants(sizeof(settings)+sizeof(jitter),sizeof(skyReprojection.m),skyReprojection.m);
            commands->DrawInstanced(3, 1, 0, 0);
            commands->EndRendering();
        });
        return true;
    }

    bool TemporalPostProcess::AddPasses(RHI::RenderGraph& graph,
        RHI::TextureHandle hdr, RHI::RGResourceHandle hdrGraph,
        RHI::TextureHandle motionDepth, RHI::RGResourceHandle motionGraph,
        RHI::TextureHandle output, RHI::RGResourceHandle outputGraph, float exposure,
        RHI::TextureHandle reactive, RHI::RGResourceHandle reactiveGraph)
    {
        using namespace RHI;
        if(m_pending || !hdr || !hdrGraph.IsValid() || !output || !outputGraph.IsValid() ||
            hdr->GetDesc().width != m_width || hdr->GetDesc().height != m_height ||
            output->GetDesc().format != m_outputFormat || !std::isfinite(exposure) || exposure <= 0)
            return false;
        TextureHandle resolved = hdr;
        RGResourceHandle resolvedGraph = hdrGraph;
        if(m_settings.taa)
        {
            if(!motionDepth || !motionGraph.IsValid() || !reactive || !reactiveGraph.IsValid() ||
                motionDepth->GetDesc().width != m_width || motionDepth->GetDesc().height != m_height)
                return false;
            std::array<RGResourceHandle, 2> history;
            for(uint32_t i = 0; i < history.size(); ++i)
            {
                history[i] = graph.ImportTexture("TAAHistory" + std::to_string(i),
                    m_history[i].image, m_history[i].state, ResourceState::ShaderResource);
                if(!history[i].IsValid()) return false;
            }
            const auto previous = 1u - m_historyWrite;
            const std::array<float, 8> settings = {1.f / m_width, 1.f / m_height,
                m_settings.historyWeight, m_historyValid ? 1.f : 0.f,
                static_cast<float>(m_width), static_cast<float>(m_height), 0.02f, 0.02f};
            if(!AddDraw(graph, "TemporalResolve", Resolve,
                {hdr, motionDepth, m_history[previous].image, reactive},
                {hdrGraph, motionGraph, history[previous], reactiveGraph},
                m_history[m_historyWrite].image, history[m_historyWrite], settings)) return false;
            resolved = m_history[m_historyWrite].image;
            resolvedGraph = history[m_historyWrite];
            m_pendingHistory = true;
        }
        TextureHandle exposureImage=resolved;
        RGResourceHandle exposureGraph=resolvedGraph;
        const bool autoExposure=m_settings.autoExposure && !m_settings.debugColor;
        if(autoExposure)
        {
            if(!m_exposure || !m_exposure->AddPasses(graph,resolved,resolvedGraph)) return false;
            exposureImage=m_exposure->GetImage();exposureGraph=m_exposure->GetHandle();
        }
        TextureHandle bloom = resolved;
        RGResourceHandle bloomGraph = resolvedGraph;
        if(m_settings.bloom)
        {
            if(m_down.empty()) return false;
            std::vector<RGResourceHandle> down(m_down.size()), up(m_up.size());
            for(uint32_t i = 0; i < down.size(); ++i)
            {
                down[i] = graph.ImportTexture("BloomDown" + std::to_string(i),
                    m_down[i].image, m_down[i].state, ResourceState::ShaderResource);
                if(!down[i].IsValid()) return false;
            }
            for(uint32_t i = 0; i < up.size(); ++i)
            {
                up[i] = graph.ImportTexture("BloomUp" + std::to_string(i),
                    m_up[i].image, m_up[i].state, ResourceState::ShaderResource);
                if(!up[i].IsValid()) return false;
            }
            TextureHandle source = resolved;
            auto sourceGraph = resolvedGraph;
            for(uint32_t i = 0; i < down.size(); ++i)
            {
                const std::array<float, 8> settings = {1.f / source->GetDesc().width,
                    1.f / source->GetDesc().height, m_settings.bloomThreshold, m_settings.bloomKnee,
                    i == 0 ? 1.f : 0.f, 0, 0, 0};
                if(!AddDraw(graph, "BloomDownsample", Downsample, {source, nullptr, nullptr},
                    {sourceGraph, {}, {}}, m_down[i].image, down[i], settings)) return false;
                source = m_down[i].image;
                sourceGraph = down[i];
            }
            for(uint32_t i = static_cast<uint32_t>(m_up.size()); i-- > 0;)
            {
                const std::array<float, 8> settings = {1.f / source->GetDesc().width,
                    1.f / source->GetDesc().height, 0, 0, 0, 0, 0, 0};
                if(!AddDraw(graph, "BloomUpsample", Upsample, {m_down[i].image, source, nullptr},
                    {down[i], sourceGraph, {}}, m_up[i].image, up[i], settings)) return false;
                source = m_up[i].image;
                sourceGraph = up[i];
            }
            bloom = source;
            bloomGraph = sourceGraph;
            m_pendingBloom = true;
        }
        const std::array<float, 8> tone = {exposure, m_settings.bloomStrength,
            IsSrgbFormat(m_outputFormat) ? 0.f : 1.f, m_settings.bloom ? 1.f : 0.f,
            m_settings.debugColor ? 1.f : 0.f, autoExposure ? 1.f : 0.f, m_settings.exposureKey, 0};
        if(!AddDraw(graph, "BloomFilmicToneMap", Filmic, {resolved, bloom, exposureImage},
            {resolvedGraph, bloomGraph, exposureGraph}, output, outputGraph, tone)) return false;
        m_pending = true;
        return true;
    }

    void TemporalPostProcess::CommitFrame()
    {
        if(!m_pending) return;
        if(m_exposure) m_exposure->CommitFrame();
        if(m_pendingHistory)
        {
            m_previousJitterPixels = JitterPixels();
            for(auto& target : m_history) target.state = RHI::ResourceState::ShaderResource;
            m_historyWrite = 1u - m_historyWrite;
            m_historyValid = true;
        }
        if(m_pendingBloom)
        {
            for(auto& target : m_down) target.state = RHI::ResourceState::ShaderResource;
            for(auto& target : m_up) target.state = RHI::ResourceState::ShaderResource;
        }
        ++m_frame;
        m_pending = m_pendingHistory = m_pendingBloom = false;
    }

    void TemporalPostProcess::InvalidateHistory()
    {
        if(m_exposure) m_exposure->Invalidate();
        m_historyValid = false;
        m_frame = 0;
        m_previousJitterPixels = {0, 0};
        m_skyCurrentToPreviousClip={};
    }
}

#include "dyf/ImGui.h"
#include "dyf/Platform/Window.h"
#include "dyf/RHI/Buffer.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/Pipeline.h"
#include "dyf/RHI/ResourceSet.h"
#include "dyf/RHI/Shader.h"
#include "dyf/RHI/Texture.h"
#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "GuiVertex.h"
#include "GuiFragment.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace dyf
{
namespace
{
ImGuiContext* activeContext = nullptr;
constexpr ImTextureID fontTextureId = 1;
uint64_t nextTextureId = 2;
struct Vertex { float x, y, u, v, r, g, b, a; };
static_assert(sizeof(Vertex) == 32 && (sizeof(ImDrawIdx) == 2 || sizeof(ImDrawIdx) == 4));
bool Fail(const char* message) { std::fprintf(stderr, "dyf::Gui: %s\n", message); return false; }
}

struct Gui::Impl
{
    explicit Impl(RHI::IDevice& value) : device(value) {}
    RHI::IDevice& device;
    ImGuiContext* context = nullptr;
    bool platformReady = false, frameStarted = false, drawReady = false;
    RHI::TextureHandle fontTexture = nullptr;
    ImFontAtlas* fontAtlas = nullptr;
    ImTextureData* fontData = nullptr;
    RHI::ShaderHandle vertexShader = nullptr, fragmentShader = nullptr;
    RHI::PipelineHandle pipeline = nullptr;
    RHI::ResourceSetHandle fontSet = nullptr;
    struct RegisteredTexture { RHI::TextureHandle texture; RHI::ResourceSetHandle set; };
    std::unordered_map<uint64_t, RegisteredTexture> textures;
    RHI::Format format = RHI::Format::Unknown;
    GuiStats stats;

    RHI::ResourceSetHandle CreateTextureSet(RHI::TextureHandle texture)
    {
        RHI::ResourceBinding binding; binding.binding = 0; binding.texture = texture;
        return device.CreateResourceSet({pipeline, &binding, 1});
    }

    void ReleasePipeline()
    {
        for(auto& entry : textures)
        {
            device.DestroyResourceSet(entry.second.set);
            entry.second.set = nullptr;
        }
        device.DestroyResourceSet(fontSet); fontSet = nullptr;
        device.DestroyPipeline(pipeline); pipeline = nullptr;
    }

    bool InitializeFont()
    {
        using namespace RHI;
        auto* atlas = ImGui::GetIO().Fonts;
        if(fontTexture)
            return (atlas == fontAtlas && atlas->IsBuilt() && atlas->TexData == fontData &&
                atlas->TexRef.GetTexID() == fontTextureId) || Fail("The font atlas cannot change after the first BeginFrame.");
        // ponytail: static atlas; implement RendererHasTextures updates only for runtime font changes.
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        atlas->GetTexDataAsRGBA32(&pixels, &width, &height);
        const uint64_t bytes = uint64_t(std::max(width, 0)) * std::max(height, 0) * 4;
        if(!pixels || !width || !height || bytes > UINT32_MAX) return Fail("Invalid font atlas dimensions.");
        auto* texture = device.CreateTexture({static_cast<uint32_t>(width), static_cast<uint32_t>(height),
            1, 1, Format::R8G8B8A8_UNORM, TextureUsage::ShaderResource});
        if(!texture) return Fail("Font texture creation failed.");
        auto* commands = device.AcquireCommandList();
        bool uploaded = false;
        if(commands)
        {
            ResourceBarrierDesc barrier{nullptr, texture, ResourceState::Undefined, ResourceState::CopyDestination, {}};
            commands->ResourceBarrier(&barrier, 1);
            uploaded = device.UpdateTexture(*commands, texture, 0, 0, pixels, static_cast<uint32_t>(bytes),
                static_cast<uint32_t>(width) * 4, static_cast<uint32_t>(bytes));
            barrier.before = ResourceState::CopyDestination;
            barrier.after = ResourceState::ShaderResource;
            commands->ResourceBarrier(&barrier, 1);
            uploaded = uploaded && commands->Close() && device.Submit(&commands, 1);
            device.DestroyCommandList(commands);
        }
        if(!uploaded) { device.DestroyTexture(texture); return Fail("Font upload submission failed."); }
        fontTexture = texture;
        fontAtlas = atlas;
        fontData = atlas->TexData;
        atlas->SetTexID(fontTextureId);
        stats.atlasBytes = bytes;
        return true;
    }

    bool InitializePipeline(RHI::Format targetFormat)
    {
        using namespace RHI;
        if(pipeline && format == targetFormat) return true;
        ReleasePipeline();
        if(!vertexShader) vertexShader = device.CreateShader({ShaderStage::Vertex, ShaderData::GuiVertexEntryPoint,
            ShaderData::GuiVertex, ShaderData::GuiVertexSize});
        if(!fragmentShader) fragmentShader = device.CreateShader({ShaderStage::Fragment, ShaderData::GuiFragmentEntryPoint,
            ShaderData::GuiFragment, ShaderData::GuiFragmentSize});
        if(!vertexShader || !fragmentShader) return Fail("Canvas shader creation failed.");
        const VertexBufferLayout buffer{0, sizeof(Vertex), VertexStepMode::Vertex};
        const std::array<VertexAttribute, 3> attributes = {{{0, 0, Format::R32G32_FLOAT, 0},
            {1, 0, Format::R32G32_FLOAT, 8}, {2, 0, Format::R32G32B32A32_FLOAT, 16}}};
        SamplerDesc sampler;
        sampler.minFilter = sampler.magFilter = sampler.mipFilter = SamplerFilter::Linear;
        sampler.addressU = sampler.addressV = sampler.addressW = SamplerAddressMode::ClampToEdge;
        sampler.mipLodBias = sampler.minLod = sampler.maxLod = 0;
        const std::array<ResourceBindingLayout, 2> bindings = {{{0, ResourceBindingType::SampledTexture, 1,
            ShaderStageFlags::Fragment, {}}, {1, ResourceBindingType::StaticSampler, 1, ShaderStageFlags::Fragment, sampler}}};
        const ColorAttachmentDesc color{targetFormat, {true, BlendFactor::SourceAlpha, BlendFactor::OneMinusSourceAlpha,
            BlendOp::Add, BlendFactor::One, BlendFactor::OneMinusSourceAlpha, BlendOp::Add}, ColorWriteMask::All};
        GraphicsPipelineDesc desc;
        desc.vertexShader = vertexShader; desc.fragmentShader = fragmentShader;
        desc.topology = PrimitiveTopology::TriangleList;
        desc.vertexBuffers = &buffer; desc.vertexBufferCount = 1;
        desc.vertexAttributes = attributes.data(); desc.vertexAttributeCount = static_cast<uint32_t>(attributes.size());
        desc.raster = {FillMode::Solid, CullMode::None, FrontFace::CounterClockwise, 0, 0, 0};
        desc.colorAttachments = &color; desc.colorAttachmentCount = 1;
        desc.layout = {bindings.data(), static_cast<uint32_t>(bindings.size()), 32,
            ShaderStageFlags::Vertex | ShaderStageFlags::Fragment, 15};
        pipeline = device.CreateGraphicsPipeline(desc);
        if(!pipeline) return Fail("Graphics pipeline creation failed.");
        fontSet = CreateTextureSet(fontTexture);
        if(!fontSet) { ReleasePipeline(); return Fail("Font binding creation failed."); }
        for(auto& entry : textures)
        {
            entry.second.set = CreateTextureSet(entry.second.texture);
            if(!entry.second.set) { ReleasePipeline(); return Fail("Registered texture binding creation failed."); }
        }
        format = targetFormat;
        return true;
    }
};

Gui::Gui(RHI::IDevice& device) : impl(new Impl(device)) {}

std::unique_ptr<Gui> Gui::Create(Platform::Window& window, RHI::IDevice& device)
{
    if(activeContext || ImGui::GetCurrentContext()) { Fail("Only one Gui/ImGui context is supported."); return nullptr; }
    if(!window.GetGlfwHandle()) { Fail("A valid Window is required."); return nullptr; }
    IMGUI_CHECKVERSION();
    std::unique_ptr<Gui> gui(new Gui(device));
    gui->impl->context = ImGui::CreateContext();
    if(!gui->impl->context) return nullptr;
    activeContext = gui->impl->context;
    auto& io = ImGui::GetIO();
    io.BackendRendererName = "dyf_rhi";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    gui->impl->platformReady = ImGui_ImplGlfw_InitForOther(window.GetGlfwHandle(), true);
    if(!gui->impl->platformReady) { Fail("GLFW backend initialization failed."); return nullptr; }
    return gui;
}

Gui::~Gui()
{
    auto& state = *impl;
    if(state.context)
    {
        auto* previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(state.context);
        if(state.frameStarted) ImGui::EndFrame();
        if(state.platformReady) ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext(state.context);
        activeContext = nullptr;
        ImGui::SetCurrentContext(previous == state.context ? nullptr : previous);
    }
    // The RHI retains resources referenced by recorded/in-flight command lists.
    state.ReleasePipeline();
    state.device.DestroyShader(state.vertexShader);
    state.device.DestroyShader(state.fragmentShader);
    state.device.DestroyTexture(state.fontTexture);
}

bool Gui::BeginFrame()
{
    auto& state = *impl;
    if(ImGui::GetCurrentContext() != state.context || state.frameStarted)
        return Fail("BeginFrame requires this Gui's current context and a finished previous frame.");
    if(ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        return Fail("Secondary OS viewports are unsupported; disable ImGuiConfigFlags_ViewportsEnable.");
    state.drawReady = false;
    state.stats = {0, 0, 0, 0, state.stats.atlasBytes};
    if(!state.InitializeFont()) return false;
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    state.frameStarted = true;
    return true;
}

void Gui::EndFrame()
{
    if(ImGui::GetCurrentContext() != impl->context || !impl->frameStarted)
    { Fail("EndFrame requires a successful BeginFrame."); return; }
    ImGui::Render();
    impl->frameStarted = false;
    impl->drawReady = true;
}

bool Gui::Record(RHI::ICommandList& commands, RHI::TextureHandle target)
{
    using namespace RHI;
    auto& state = *impl;
    if(ImGui::GetCurrentContext() != state.context || !state.drawReady || !target)
        return Fail("Record requires EndFrame and a target in Present state.");
    state.stats = {0, 0, 0, 0, state.stats.atlasBytes};
    const auto* data = ImGui::GetDrawData();
    if(!data || !data->Valid) return Fail("No valid ImGui draw data.");
    const float width = data->DisplaySize.x * data->FramebufferScale.x;
    const float height = data->DisplaySize.y * data->FramebufferScale.y;
    if(width <= 0 || height <= 0 || data->TotalVtxCount == 0) return true;
    if(!std::isfinite(width) || !std::isfinite(height) || !std::isfinite(data->DisplayPos.x) ||
        !std::isfinite(data->DisplayPos.y) || data->FramebufferScale.x <= 0 || data->FramebufferScale.y <= 0 ||
        data->TotalVtxCount < 0 || data->TotalIdxCount <= 0) return Fail("Invalid ImGui geometry dimensions.");
    const uint64_t vertexBytes = uint64_t(data->TotalVtxCount) * sizeof(Vertex);
    const uint64_t indexBytes = uint64_t(data->TotalIdxCount) * sizeof(ImDrawIdx);
    if(vertexBytes > UINT32_MAX || indexBytes > UINT32_MAX) return Fail("ImGui geometry exceeds RHI buffer limits.");
    const float clipWidth = std::min(width, static_cast<float>(target->GetDesc().width));
    const float clipHeight = std::min(height, static_cast<float>(target->GetDesc().height));
    if(clipWidth > INT32_MAX || clipHeight > INT32_MAX) return Fail("Target exceeds signed scissor coordinates.");

    // Validate before appending any commands. Arbitrary IDs must never become RHI pointers.
    uint64_t vertexCount = 0, indexCount = 0;
    for(const auto* list : data->CmdLists)
    {
        vertexCount += list->VtxBuffer.Size; indexCount += list->IdxBuffer.Size;
        for(const auto& draw : list->CmdBuffer)
        {
            if(draw.UserCallback == ImDrawCallback_ResetRenderState) continue;
            if(draw.UserCallback) return Fail("Custom ImDraw callbacks are unsupported; only ResetRenderState is accepted.");
            if(draw.ElemCount && draw.GetTexID() != fontTextureId)
            {
                const auto found = state.textures.find(draw.GetTexID());
                if(found == state.textures.end()) return Fail("Unknown or unregistered ImTextureID.");
                if(found->second.texture == target) return Fail("A GUI target cannot also be sampled by its own draw.");
            }
            if(uint64_t(draw.IdxOffset) + draw.ElemCount > static_cast<uint64_t>(list->IdxBuffer.Size) ||
                draw.VtxOffset > static_cast<uint32_t>(list->VtxBuffer.Size) ||
                !std::isfinite(draw.ClipRect.x) || !std::isfinite(draw.ClipRect.y) ||
                !std::isfinite(draw.ClipRect.z) || !std::isfinite(draw.ClipRect.w))
                return Fail("Invalid draw range or clip rectangle.");
        }
    }
    if(vertexCount != static_cast<uint64_t>(data->TotalVtxCount) || indexCount != static_cast<uint64_t>(data->TotalIdxCount))
        return Fail("ImGui geometry totals do not match the command lists.");
    if(!state.InitializePipeline(target->GetDesc().format)) return false;

    std::vector<Vertex> vertices; vertices.reserve(data->TotalVtxCount);
    std::vector<ImDrawIdx> indices; indices.reserve(data->TotalIdxCount);
    for(const auto* list : data->CmdLists)
    {
        for(const auto& vertex : list->VtxBuffer)
        {
            const auto color = ImGui::ColorConvertU32ToFloat4(vertex.col);
            vertices.push_back({vertex.pos.x, vertex.pos.y, vertex.uv.x, vertex.uv.y, color.x, color.y, color.z, color.w});
        }
        indices.insert(indices.end(), list->IdxBuffer.begin(), list->IdxBuffer.end());
    }
    auto* vb = state.device.CreateBuffer({static_cast<uint32_t>(vertexBytes), sizeof(Vertex), BufferUsage::Vertex, ResourceState::CopyDestination});
    auto* ib = state.device.CreateBuffer({static_cast<uint32_t>(indexBytes), sizeof(ImDrawIdx), BufferUsage::Index, ResourceState::CopyDestination});
    const auto release = [&] { state.device.DestroyBuffer(vb); state.device.DestroyBuffer(ib); };
    if(!vb || !ib) { release(); return Fail("Geometry buffer creation failed."); }
    if(!state.device.UpdateBuffer(commands, vb, 0, vertices.data(), static_cast<uint32_t>(vertexBytes)) ||
        !state.device.UpdateBuffer(commands, ib, 0, indices.data(), static_cast<uint32_t>(indexBytes)))
    { release(); return Fail("Geometry upload recording failed."); }
    const ResourceBarrierDesc barriers[] = {{vb, nullptr, ResourceState::CopyDestination, ResourceState::VertexBuffer, {}},
        {ib, nullptr, ResourceState::CopyDestination, ResourceState::IndexBuffer, {}},
        {nullptr, target, ResourceState::Present, ResourceState::RenderTarget, {}}};
    commands.ResourceBarrier(barriers, 3);
    ColorAttachment color; color.texture = target; color.loadOp = LoadOp::Load; color.storeOp = StoreOp::Store;
    commands.BeginRendering({&color, 1, nullptr});
    const float transform[] = {2 / data->DisplaySize.x, -2 / data->DisplaySize.y,
        -1 - data->DisplayPos.x * (2 / data->DisplaySize.x), 1 + data->DisplayPos.y * (2 / data->DisplaySize.y),
        IsSrgbFormat(target->GetDesc().format) ? 1.f : 0.f, 0, 0, 0};
    const auto setup = [&] {
        commands.BindGraphicsPipeline(state.pipeline);
        commands.BindVertexBuffer(0, vb, 0);
        commands.BindIndexBuffer(ib, sizeof(ImDrawIdx) == 2 ? Format::R16_UINT : Format::R32_UINT, 0);
        commands.BindResourceSet(state.fontSet);
        commands.SetViewport({0, 0, width, height, 0, 1});
        commands.SetScissor({0, 0, static_cast<uint32_t>(clipWidth), static_cast<uint32_t>(clipHeight)});
        commands.SetInlineConstants(0, sizeof(transform), transform);
    };
    setup();
    uint32_t firstIndex = 0, firstVertex = 0;
    for(const auto* list : data->CmdLists)
    {
        for(const auto& draw : list->CmdBuffer)
        {
            if(draw.UserCallback == ImDrawCallback_ResetRenderState) { setup(); continue; }
            const float left = std::clamp(std::floor((draw.ClipRect.x - data->DisplayPos.x) * data->FramebufferScale.x), 0.f, clipWidth);
            const float top = std::clamp(std::floor((draw.ClipRect.y - data->DisplayPos.y) * data->FramebufferScale.y), 0.f, clipHeight);
            const float right = std::clamp(std::ceil((draw.ClipRect.z - data->DisplayPos.x) * data->FramebufferScale.x), 0.f, clipWidth);
            const float bottom = std::clamp(std::ceil((draw.ClipRect.w - data->DisplayPos.y) * data->FramebufferScale.y), 0.f, clipHeight);
            if(!draw.ElemCount || right <= left || bottom <= top) continue;
            commands.BindResourceSet(draw.GetTexID() == fontTextureId ? state.fontSet : state.textures.at(draw.GetTexID()).set);
            commands.SetScissor({static_cast<int32_t>(left), static_cast<int32_t>(top),
                static_cast<uint32_t>(right - left), static_cast<uint32_t>(bottom - top)});
            commands.DrawIndexedInstanced(draw.ElemCount, 1, firstIndex + draw.IdxOffset,
                static_cast<int32_t>(firstVertex + draw.VtxOffset), 0);
            ++state.stats.drawCalls;
        }
        firstIndex += static_cast<uint32_t>(list->IdxBuffer.Size);
        firstVertex += static_cast<uint32_t>(list->VtxBuffer.Size);
    }
    commands.EndRendering();
    const ResourceBarrierDesc after{nullptr, target, ResourceState::RenderTarget, ResourceState::Present, {}};
    commands.ResourceBarrier(&after, 1);
    release();
    state.stats.vertices = static_cast<uint32_t>(data->TotalVtxCount);
    state.stats.indices = static_cast<uint32_t>(data->TotalIdxCount);
    state.stats.uploadBytes = vertexBytes + indexBytes;
    return true;
}

uint64_t Gui::RegisterTexture(RHI::TextureHandle texture)
{
    using namespace RHI;
    if(!texture) { Fail("Cannot register a null texture."); return 0; }
    const auto& desc = texture->GetDesc();
    const bool colorFormat = desc.format >= Format::R8G8B8A8_UNORM && desc.format <= Format::R32G32B32A32_FLOAT;
    if((desc.usage & TextureUsage::ShaderResource) == TextureUsage::None || desc.depthOrArraySize != 1 || !colorFormat)
    { Fail("Registered textures must be 2D color textures with ShaderResource usage."); return 0; }
    if(nextTextureId == UINT64_MAX) { Fail("Texture ID space exhausted."); return 0; }
    auto* set = impl->pipeline ? impl->CreateTextureSet(texture) : nullptr;
    if(impl->pipeline && !set) { Fail("Texture registration binding failed."); return 0; }
    const uint64_t id = nextTextureId++;
    impl->textures.emplace(id, Impl::RegisteredTexture{texture, set});
    return id;
}

void Gui::UnregisterTexture(uint64_t id)
{
    const auto found = impl->textures.find(id);
    if(found == impl->textures.end()) return;
    impl->device.DestroyResourceSet(found->second.set);
    impl->textures.erase(found);
}

bool Gui::WantsMouse() const { return ImGui::GetCurrentContext() == impl->context && ImGui::GetIO().WantCaptureMouse; }
bool Gui::WantsKeyboard() const { return ImGui::GetCurrentContext() == impl->context && ImGui::GetIO().WantCaptureKeyboard; }
const GuiStats& Gui::GetStats() const { return impl->stats; }
}

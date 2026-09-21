#include <dyf/Platform/ThreadPool.h>
#include <dyf/RHI.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>

namespace RHI = dyf::RHI;
using Clock = std::chrono::steady_clock;
namespace
{
void Check(bool value, const char* message) { if(!value) throw std::runtime_error(message); }
uint32_t Number(const std::string& value)
{
    char* end = nullptr;
    const auto number = std::strtoull(value.c_str(), &end, 10);
    Check(!value.empty() && value[0] != '-' && end && !*end && number <= UINT32_MAX, "Expected an unsigned integer");
    return uint32_t(number);
}
struct CommandLists
{
    RHI::IDevice& device;
    std::vector<RHI::ICommandList*> lists;
    explicit CommandLists(RHI::IDevice& owner) : device(owner) {}
    ~CommandLists() { for(auto* list : lists) device.DestroyCommandList(list); }
};
}

int main(int argc, char** argv)
{
    try
    {
        std::string mode = "parallel", capture;
        uint32_t frames = 3, workers = 2;
        bool validation = false;
        for(int i = 1; i < argc; ++i)
        {
            const std::string argument = argv[i];
            if(argument == "--help")
            {
                std::puts("AdvancedMultithreadRecording --mode serial|parallel --workers N --frames N --capture image.ppm --validation\n"
                    "Records two independent texture uploads and a dependent reader; submits in graph order and waits for a completion fence.\n"
                    "Native backends check GPU readback. Null validates command/state/fence contracts without GPU pixels.\n"
                    "--workers 0 selects hardware concurrency; 1 records on the calling thread. --frames must be positive.\n"
                    "CPU RHI recording is parallel. Native replay/submission remains serialized; no GPU speedup is claimed.");
                return 0;
            }
            if(argument == "--validation") validation = true;
            else if((argument == "--mode" || argument == "--workers" || argument == "--frames" || argument == "--capture") && i + 1 < argc)
            {
                const std::string value = argv[++i];
                if(argument == "--mode") mode = value;
                else if(argument == "--capture") capture = value;
                else if(argument == "--workers") workers = Number(value);
                else frames = Number(value);
            }
            else throw std::runtime_error("Unknown or incomplete argument; use --help");
        }
        Check(mode == "serial" || mode == "parallel", "Invalid --mode");
        Check(frames > 0 && workers <= 64, "--frames must be positive and --workers must be 0..64");
        RHI::DeviceDesc deviceDesc;
        deviceDesc.enableValidation = validation;
        std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create(deviceDesc));
        Check(bool(device), "Device creation failed");
        RHI::ResourceScope resources(*device);
        dyf::Platform::ThreadPool pool(mode == "parallel" ? workers : 1);
        constexpr uint32_t width = 256, height = 256;
        std::array<RHI::TextureHandle, 2> textures{};
        for(auto& texture : textures)
        {
            RHI::TextureDesc desc;
            desc.width = width;
            desc.height = height;
            desc.format = RHI::Format::R8G8B8A8_UNORM;
            desc.usage = RHI::TextureUsage::ShaderResource;
            texture = resources.Keep(device->CreateTexture(desc));
        }
        std::array<std::vector<uint8_t>, 2> inputs;
        std::set<std::thread::id> recordingThreads;
        std::mutex threadMutex;
        double recordMilliseconds = 0, submitMilliseconds = 0;
        size_t submittedLists = 0;
        for(uint32_t frame = 0; frame < frames; ++frame)
        {
            RHI::RenderGraph graph;
            std::array<RHI::RGResourceHandle, 2> imports;
            for(uint32_t i = 0; i < 2; ++i)
            {
                imports[i] = graph.ImportTexture(std::to_string(i), textures[i],
                    frame == 0 ? RHI::ResourceState::Undefined : RHI::ResourceState::ShaderResource,
                    RHI::ResourceState::ShaderResource);
                Check(imports[i].IsValid(), "Texture import failed");
                graph.AddPass("generate-and-upload-" + std::to_string(i))
                    .Write(imports[i], RHI::ResourceState::CopyDestination)
                    .SetExecute([&, i, frame](RHI::ICommandList* list) {
                        auto& pixels = inputs[i];
                        pixels.resize(size_t(width) * height * 4);
                        for(uint32_t y = 0; y < height; ++y)
                            for(uint32_t x = 0; x < width; ++x)
                            {
                                auto* pixel = pixels.data() + (size_t(y) * width + x) * 4;
                                pixel[0] = uint8_t(x + frame * 7);
                                pixel[1] = uint8_t(y + i * 71);
                                pixel[2] = uint8_t((x ^ y) + i * 113);
                                pixel[3] = 255;
                            }
                        Check(device->UpdateTexture(*list, textures[i], 0, 0, pixels.data(),
                            uint32_t(pixels.size()), width * 4, width * height * 4), "Worker texture upload failed");
                        std::lock_guard<std::mutex> lock(threadMutex);
                        recordingThreads.insert(std::this_thread::get_id());
                    });
            }
            graph.AddPass("join-and-read").GlobalBarrier()
                .Read(imports[0], RHI::ResourceState::ShaderResource)
                .Read(imports[1], RHI::ResourceState::ShaderResource)
                .SetExecute([](RHI::ICommandList*) {});
            Check(graph.Compile(), "RenderGraph compilation failed");
            CommandLists commands(*device);
            const auto recordStart = Clock::now();
            if(mode == "parallel")
                Check(graph.ExecuteParallel(device.get(), &pool, commands.lists), "Parallel recording failed");
            else
            {
                auto* list = device->AcquireCommandList();
                Check(list != nullptr, "Command list acquisition failed");
                commands.lists.push_back(list);
                Check(graph.Execute(list) && list->Close(), "Serial recording failed");
            }
            recordMilliseconds += std::chrono::duration<double, std::milli>(Clock::now() - recordStart).count();
            RHI::FenceHandle completion;
            const auto submitStart = Clock::now();
            Check(device->Submit({commands.lists.data(), uint32_t(commands.lists.size()), nullptr, 0}, completion), "Submit failed");
            submitMilliseconds += std::chrono::duration<double, std::milli>(Clock::now() - submitStart).count();
            Check(device->Wait(completion, 5000000000ull), "Submission did not complete within five seconds");
            submittedLists += commands.lists.size();
        }
        if(device->Supports(RHI::Feature::Rasterization))
        {
            std::array<RHI::TextureReadback, 2> readbacks;
            for(uint32_t i = 0; i < 2; ++i)
            {
                auto& pixels = readbacks[i];
                Check(device->ReadTexture(textures[i], pixels), "Texture readback failed");
                Check(pixels.width == width && pixels.height == height && pixels.format == RHI::Format::R8G8B8A8_UNORM,
                    "Unexpected readback dimensions/format");
                for(uint32_t y = 0; y < height; ++y)
                    for(uint32_t x = 0; x < width * 4; ++x)
                        Check(pixels.pixels[size_t(y) * pixels.rowPitch + x] == inputs[i][size_t(y) * width * 4 + x],
                            "GPU pixels differ from the recorded upload");
            }
            if(!capture.empty())
            {
                std::ofstream output(capture, std::ios::binary);
                Check(bool(output), "Cannot open capture path");
                output << "P6\n" << width * 2 << ' ' << height << "\n255\n";
                for(uint32_t y = 0; y < height; ++y)
                    for(const auto& pixels : readbacks)
                        for(uint32_t x = 0; x < width; ++x)
                            output.write(reinterpret_cast<const char*>(pixels.pixels.data() + size_t(y) * pixels.rowPitch + x * 4), 3);
                Check(bool(output), "Capture write failed");
            }
            std::puts("GPU readback matches both recorded texture uploads.");
        }
        else
        {
            Check(capture.empty(), "This backend cannot capture GPU pixels");
            std::puts("Null backend: command/state/fence contracts completed; GPU pixels were not rendered or read back.");
        }
        Check(device->WaitIdle(), "WaitIdle failed");
        std::printf("mode=%s frames=%u workers=%u observed_recording_threads=%zu command_lists=%zu cpu_record_ms=%.3f cpu_submit_ms=%.3f\n",
            mode.c_str(), frames, pool.GetThreadCount(), recordingThreads.size(), submittedLists, recordMilliseconds, submitMilliseconds);
        std::puts("CPU recording and CPU native replay/submission are measured separately. These are not GPU timings or a speedup claim.");
        return 0;
    }
    catch(const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}

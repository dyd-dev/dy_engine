#include <android/asset_manager.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include "Platform/Windows/AndroidApplication.h"
#include "dyf/Extends/Tracy.h"
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

int DyfRunExamples(int argc, char** argv, const char* assetRoot, const char* fontPath,
    void (*onExampleChanged)(bool));

namespace
{
using Asset = std::unique_ptr<AAsset, decltype(&AAsset_close)>;

// NativeActivity runs this entry on a native thread. Attach only while reading
// its launch intent; the no-extra launch still opens the example browser.
class IntentEnvironment
{
public:
    explicit IntentEnvironment(JavaVM& vm) : vm(vm)
    {
        const auto result = vm.GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if(result == JNI_EDETACHED)
        {
            if(vm.AttachCurrentThread(&env, nullptr) != JNI_OK)
                throw std::runtime_error("Cannot attach Android launch thread to Java.");
            attached = true;
        }
        else if(result != JNI_OK)
            throw std::runtime_error("Cannot access Android launch intent.");
        if(env->PushLocalFrame(8) < 0)
        {
            env->ExceptionClear();
            if(attached) vm.DetachCurrentThread();
            throw std::runtime_error("Cannot allocate Android launch intent references.");
        }
    }
    ~IntentEnvironment()
    {
        env->PopLocalFrame(nullptr);
        if(attached) vm.DetachCurrentThread();
    }
    JNIEnv* operator->() const { return env; }

private:
    JavaVM& vm;
    JNIEnv* env = nullptr;
    bool attached = false;
};

std::string LaunchExample(ANativeActivity& activity)
{
    IntentEnvironment env(*activity.vm);
    const auto activityClass = env->GetObjectClass(activity.clazz);
    const auto getIntent = env->GetMethodID(activityClass, "getIntent", "()Landroid/content/Intent;");
    const auto intent = env->CallObjectMethod(activity.clazz, getIntent);
    if(!intent) return {};
    const auto intentClass = env->GetObjectClass(intent);
    const auto getStringExtra = env->GetMethodID(intentClass, "getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;");
    const auto key = env->NewStringUTF("dy_example");
    const auto value = static_cast<jstring>(env->CallObjectMethod(intent, getStringExtra, key));
    if(env->ExceptionCheck())
    {
        env->ExceptionClear();
        throw std::runtime_error("dy_example must be a string naming an example target.");
    }
    if(!value) return {};
    const auto text = env->GetStringUTFChars(value, nullptr);
    if(!text)
    {
        env->ExceptionClear();
        throw std::runtime_error("Cannot read dy_example launch selection.");
    }
    std::string selection(text);
    env->ReleaseStringUTFChars(value, text);
    return selection;
}

thread_local const char* launchSelection = nullptr;

void ReportExampleState(bool running)
{
    if(launchSelection)
        __android_log_print(ANDROID_LOG_INFO, "dy_engine", "dy_example=%s state=%s",
            launchSelection, running ? "started" : "stopped");
}

std::string PrepareAssets(android_app& app)
{
    const auto root = std::filesystem::path(app.activity->internalDataPath) / "examples";
    Asset manifest(AAssetManager_open(app.activity->assetManager, "files.txt", AASSET_MODE_BUFFER), AAsset_close);
    if(!manifest) throw std::runtime_error("Packaged example asset list is missing.");
    const auto size = AAsset_getLength64(manifest.get());
    if(size < 0 || size > 16 * 1024 * 1024) throw std::runtime_error("Invalid example asset list.");
    std::string contents(static_cast<size_t>(size), '\0');
    size_t offset = 0;
    while(offset < contents.size())
    {
        const auto count = AAsset_read(manifest.get(), contents.data() + offset, contents.size() - offset);
        if(count <= 0) throw std::runtime_error("Cannot read example asset list.");
        offset += static_cast<size_t>(count);
    }
    std::filesystem::create_directories(root);
    std::istringstream paths(contents);
    std::string relative;
    while(std::getline(paths, relative))
    {
        if(!relative.empty() && relative.back() == '\r') relative.pop_back();
        if(relative.empty()) continue;
        const std::filesystem::path path(relative);
        if(path.is_absolute() || path.has_root_name())
            throw std::runtime_error("Invalid packaged asset path.");
        for(const auto& component : path)
            if(component == "..") throw std::runtime_error("Invalid packaged asset path.");
        Asset asset(AAssetManager_open(app.activity->assetManager, relative.c_str(), AASSET_MODE_STREAMING), AAsset_close);
        if(!asset) throw std::runtime_error("Packaged asset is missing: " + relative);
        const auto destination = root / path;
        std::filesystem::create_directories(destination.parent_path());
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        if(!output) throw std::runtime_error("Cannot extract example asset: " + relative);
        std::array<char, 64 * 1024> buffer;
        int count;
        while((count = AAsset_read(asset.get(), buffer.data(), buffer.size())) > 0)
            output.write(buffer.data(), count);
        output.close();
        if(count < 0 || !output) throw std::runtime_error("Cannot extract example asset: " + relative);
    }
    return root.string();
}
}

void android_main(android_app* state)
{
    dyf::Platform::AndroidApplication application(*state);
    try
    {
#ifdef DY_PROFILER_TRACING_ENABLED
        dyf::TracyAdapter tracy;
#endif
        const auto assets = PrepareAssets(*state);
        auto selection = LaunchExample(*state->activity);
        char name[] = "dy_engine examples";
        char* arguments[] = {name, selection.empty() ? nullptr : selection.data(), nullptr};
        launchSelection = selection.empty() ? nullptr : selection.c_str();
        const auto result = DyfRunExamples(selection.empty() ? 1 : 2, arguments,
            assets.c_str(), "/system/fonts/Roboto-Regular.ttf", ReportExampleState);
        __android_log_print(result == 0 ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
            "dy_engine", "Examples exited with code %d", result);
        launchSelection = nullptr;
    }
    catch(const std::exception& error)
    {
        __android_log_print(ANDROID_LOG_ERROR, "dy_engine", "Examples: %s", error.what());
    }
    if(!state->destroyRequested)
    {
        ANativeActivity_finish(state->activity);
        while(!state->destroyRequested)
        {
            int events = 0;
            android_poll_source* source = nullptr;
            if(ALooper_pollOnce(-1, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0 && source)
                source->process(state, source);
        }
    }
}

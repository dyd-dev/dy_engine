// Common example host compiled only by the native Android and iOS applications.
#include "dyf/Canvas.h"
#include "dyf/Font.h"
#include "dyf/Platform/Window.h"
#include "dyf/Renderer.h"
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef __ANDROID__
#include <android/asset_manager.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include "Platform/Windows/AndroidApplication.h"
#include <array>
#include <fstream>
#include <memory>
#include <sstream>
#include <sys/stat.h>
#endif
#ifdef DY_PROFILER_TRACING_ENABLED
#include "dyf/Extends/Tracy.h"
#endif

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#if !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IPHONE)
#ifdef _WIN32
#include <Windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif
#endif

namespace
{
struct Example
{
    const char* name;
    const char* executable;
    const char* category;
    int (*entry)(int, char**) = nullptr;
};
}

#include "ExampleTargets.inc"

namespace
{
void List()
{
    for(size_t i = 0; i < std::size(examples); ++i)
        std::cout << i + 1 << ". " << examples[i].name << '\n';
}

const Example* Find(std::string_view selection)
{
    size_t number = 0;
    const auto parsed = std::from_chars(selection.data(), selection.data() + selection.size(), number);
    if(parsed.ec == std::errc{} && parsed.ptr == selection.data() + selection.size() &&
        number > 0 && number <= std::size(examples)) return &examples[number - 1];
    for(const auto& example : examples)
        if(selection == example.name) return &example;
    return nullptr;
}

#if !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IPHONE)
class Process
{
public:
    ~Process()
    {
#ifdef _WIN32
        if(process) CloseHandle(process);
#endif
    }
    bool Running() const { return process != 0; }
    bool Start(const Example& example, const std::vector<std::string>& arguments, std::string& error)
    {
        if(Running()) return false;
        const std::filesystem::path executable(example.executable);
        std::error_code fileError;
        if(!std::filesystem::is_regular_file(executable, fileError))
        {
            error = std::string("Executable is missing: ") + example.name;
            return false;
        }
        const auto directory = executable.parent_path();
#ifdef _WIN32
        // Quote each argument according to the Windows C runtime's argv rules.
        const auto quote = [](const std::wstring& value)
        {
            std::wstring result = L"\"";
            size_t slashes = 0;
            for(const auto character : value)
            {
                if(character == L'\\') { ++slashes; continue; }
                result.append(slashes * (character == L'\"' ? 2 : 1), L'\\');
                slashes = 0;
                if(character == L'\"') result += L'\\';
                result += character;
            }
            result.append(slashes * 2, L'\\');
            return result + L'\"';
        };
        std::wstring command = quote(executable.wstring());
        for(const auto& argument : arguments)
            command += L" " + quote(std::filesystem::path(argument).wstring());
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        if(!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
            nullptr, directory.c_str(), &startup, &info))
        {
            error = "Cannot start example (Windows error " + std::to_string(GetLastError()) + ").";
            return false;
        }
        CloseHandle(info.hThread);
        process = info.hProcess;
#else
        std::vector<std::string> values{executable.string()};
        values.insert(values.end(), arguments.begin(), arguments.end());
        std::vector<char*> argv;
        for(auto& value : values) argv.push_back(value.data());
        argv.push_back(nullptr);
        // Prepare everything before fork: the GUI renderer may own worker threads.
        const auto* executablePath = executable.c_str();
        const auto* workingDirectory = directory.c_str();
        auto* const* childArguments = argv.data();
        const auto child = fork();
        if(child == -1) { error = std::strerror(errno); return false; }
        if(child == 0)
        {
            if(chdir(workingDirectory) == -1) _exit(126);
            execv(executablePath, childArguments);
            _exit(127);
        }
        process = child;
#endif
        return true;
    }
    // Poll never waits; the launcher remains responsive while an example runs.
    bool Finish(int& result, bool wait = false)
    {
        if(!Running()) return false;
#ifdef _WIN32
        const auto state = WaitForSingleObject(process, wait ? INFINITE : 0);
        if(state == WAIT_TIMEOUT) return false;
        DWORD exitCode = 1;
        if(state == WAIT_OBJECT_0) GetExitCodeProcess(process, &exitCode);
        CloseHandle(process);
        process = nullptr;
        result = static_cast<int>(exitCode);
#else
        int status = 0;
        pid_t finished;
        do { finished = waitpid(process, &status, wait ? 0 : WNOHANG); }
        while(finished == -1 && errno == EINTR);
        if(finished == 0) return false;
        process = 0;
        result = finished == -1 ? 1 : WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
#endif
        return true;
    }
private:
#ifdef _WIN32
    HANDLE process = nullptr;
#else
    pid_t process = 0;
#endif
};
#endif

std::unique_ptr<dyf::Font> LoadFont(float size)
{
    if(const auto* path = std::getenv("DY_EXAMPLES_FONT"))
        if(auto font = dyf::Font::Load(path, size)) return font;
#ifdef __ANDROID__
    for(const auto* path : {"/system/fonts/Roboto-Regular.ttf", "/system/fonts/NotoSans-Regular.ttf"})
        if(auto font = dyf::Font::Load(path, size)) return font;
#elif defined(_WIN32)
    wchar_t directory[MAX_PATH];
    const auto length = GetWindowsDirectoryW(directory, MAX_PATH);
    if(!length || length >= MAX_PATH) return nullptr;
    const auto fonts = std::filesystem::path(directory) / "Fonts";
    for(const auto* name : {"segoeui.ttf", "arial.ttf"})
        if(auto font = dyf::Font::Load((fonts / name).u8string(), size)) return font;
#elif defined(__APPLE__)
    for(const auto* path : {"/System/Library/Fonts/Helvetica.ttc", "/System/Library/Fonts/SFNS.ttf"})
        if(auto font = dyf::Font::Load(path, size)) return font;
#else
    for(const auto* path : {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf"})
        if(auto font = dyf::Font::Load(path, size)) return font;
#endif
    return nullptr;
}

std::string Title(const Example& example)
{
    std::string_view name(example.name);
    for(const auto prefix : {std::string_view("Advanced"), std::string_view("Dyf"), std::string_view("Rhi"), std::string_view("RHI")})
        if(name.substr(0, prefix.size()) == prefix) { name.remove_prefix(prefix.size()); break; }
    std::string result;
    for(size_t i = 0; i < name.size(); ++i)
    {
        const auto upper = [](char c) { return std::isupper(static_cast<unsigned char>(c)); };
        const auto lower = [](char c) { return std::islower(static_cast<unsigned char>(c)); };
        if(i && upper(name[i]) && (lower(name[i - 1]) || (i + 1 < name.size() && lower(name[i + 1]))))
            result += ' ';
        result += name[i];
    }
    return result;
}

using Color = dyf::Math::float4;
const Color white{.90f,.93f,.97f,1}, muted{.48f,.55f,.65f,1}, accent{.35f,.74f,.98f,1};

bool Contains(dyf::Rectangle rect, dyf::Platform::InputPoint point)
{
    return point.x >= rect.x && point.y >= rect.y && point.x < rect.x + rect.width && point.y < rect.y + rect.height;
}

struct View
{
    dyf::Canvas& canvas;
    dyf::Font& font;
    dyf::Font& heading;
    float scaleX, scaleY;
    dyf::Rectangle Pixels(dyf::Rectangle rect) const
    {
        return {rect.x * scaleX, rect.y * scaleY, rect.width * scaleX, rect.height * scaleY};
    }
    void Fill(dyf::Rectangle rect, Color color) { canvas.FillRect(Pixels(rect), color); }
    void Text(std::string_view text, float x, float y, Color color = white, bool large = false)
    {
        if(!canvas.Text(large ? heading : font, text, {x * scaleX, y * scaleY}, color))
            throw std::runtime_error("Cannot draw launcher text.");
    }
    void Clip(dyf::Rectangle rect) { canvas.SetClipRect(Pixels(rect)); }
};

int OpenGui([[maybe_unused]] const Example*& selected, std::string status)
{
    using namespace dyf::Platform;
    Window window(1080, 760, "dy_engine / Examples");
    if(!window.GetHandle()) return 1;
    dyf::RendererConfig config;
    config.recordingThreadCount = 1;
    auto renderer = dyf::Renderer::Create(window.GetHandle(), config);
    if(!renderer) return 1;
    const void* rendererWindow = window.GetHandle();
    auto rendererSurfaceGeneration = window.GetSurfaceGeneration();

#if !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IPHONE)
    Process child;
#endif
    const Example* active = nullptr;
    int category = 0, lastWidth = 0, lastHeight = 0;
    float scroll = 0, fontScale = 0, dragOffset = 0;
    bool dragging = false;
    std::unique_ptr<dyf::Canvas> canvas;
    std::unique_ptr<dyf::Font> font, heading;
    constexpr const char* categories[] = {
        "All examples", "dyf", "RHI", "advanced",
    };
    std::vector<size_t> visible;

    while(window.IsRunning())
    {
        Window::PollEvents();
        if(!window.IsRunning()) break;
#if !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IPHONE)
        int result = 0;
        if(child.Finish(result))
        {
            status = std::string(active->name) + (result == 0 ? " closed." : result == 77 ?
                ": unavailable on this backend, or not implemented." : ": exited with code " + std::to_string(result) + ".");
            active = nullptr;
        }
#endif
        const auto nativeWindow = window.GetHandle();
        const auto surfaceGeneration = window.GetSurfaceGeneration();
        if(renderer && (nativeWindow != rendererWindow || surfaceGeneration != rendererSurfaceGeneration))
            renderer.reset();
        const auto size = window.GetSize(), pixels = window.GetFramebufferSize();
        if(!nativeWindow || size.width <= 0 || size.height <= 0 || pixels.width <= 0 || pixels.height <= 0 ||
            window.IsMinimized() || !window.HasFocus())
        {
            Window::WaitEvents(.05);
            continue;
        }
        if(!renderer)
        {
            renderer = dyf::Renderer::Create(nativeWindow, config);
            if(!renderer) return 1;
            rendererWindow = nativeWindow;
            rendererSurfaceGeneration = surfaceGeneration;
        }
#ifdef __ANDROID__
        const float uiScale = std::max(1.f, static_cast<float>(size.width) / 1080.f);
#else
        const float uiScale = 1;
#endif
        const float width = size.width / uiScale, height = size.height / uiScale;
        const float sx = static_cast<float>(pixels.width) / width;
        const float sy = static_cast<float>(pixels.height) / height;
        if(fontScale != sy)
        {
            font = LoadFont(18 * sy);
            heading = LoadFont(28 * sy);
            if(!font || !heading) throw std::runtime_error("No supported system font found for the launcher.");
            fontScale = sy;
        }
        if(lastWidth != pixels.width || lastHeight != pixels.height)
        {
            canvas = std::make_unique<dyf::Canvas>(pixels.width, pixels.height);
            lastWidth = pixels.width;
            lastHeight = pixels.height;
        }
        // A skipped frame is not a presentation. Keep retrying while the native
        // window is active; the renderer uses VSync to pace the browser.
        canvas->Clear({.045f,.055f,.075f,1});
        View view{*canvas, *font, *heading, sx, sy};
        const auto& input = window.GetInput();
        auto pointer = input.GetCursorPosition();
        pointer.x /= uiScale;
        pointer.y /= uiScale;
        const bool click = input.WasPressed(MouseButton::Left);
        const float sidebar = width >= 700 ? 190.f : 130.f, left = sidebar + 24;
        const float contentWidth = std::max(1.f, width - left - 24);

        view.Fill({0,0,sidebar,height}, {.065f,.08f,.105f,1});
        view.Text("dy_engine", 18, 24, accent, width >= 700);
        view.Text("EXAMPLES", 18, 68, muted);
        for(int i = 0; i < static_cast<int>(std::size(categories)); ++i)
        {
            const dyf::Rectangle button{10, 112.f + i * 48, sidebar - 20, 40};
            if(click && Contains(button, pointer)) { category = i; scroll = 0; }
            const bool hover = Contains(button, pointer);
            if(category == i || hover) view.Fill(button, category == i ? Color{.10f,.22f,.31f,1} : Color{.10f,.13f,.17f,1});
            view.Clip(button);
            view.Text(categories[i], button.x + 12, button.y + 10, category == i ? accent : white);
            canvas->ResetClipRect();
        }
        view.Clip({left,0,contentWidth,96});
        view.Text(category ? categories[category] : "All examples", left, 24, white, true);
        view.Text("Select an example to launch it.", left, 65, muted);
        canvas->ResetClipRect();

        if(input.WasPressed(Key::Escape)) window.RequestClose();

        visible.clear();
        for(size_t i = 0; i < std::size(examples); ++i)
            if(!category || std::string_view(examples[i].category) == categories[category])
                visible.push_back(i);
        view.Text(std::to_string(visible.size()) + " examples", left, 108, muted);
        const dyf::Rectangle list{left,136,contentWidth,std::max(0.f,height - 212)};
        constexpr float rowHeight = 64;
        const float total = visible.size() * rowHeight;
        const float maxScroll = std::max(0.f, total - list.height);
        const dyf::Rectangle previous{left + contentWidth - 132,100,60,30};
        const dyf::Rectangle next{left + contentWidth - 64,100,64,30};
        view.Fill(previous, {.10f,.14f,.19f,1});
        view.Fill(next, {.10f,.14f,.19f,1});
        view.Text("UP", previous.x + 16, previous.y + 5, scroll > 0 ? accent : muted);
        view.Text("DOWN", next.x + 7, next.y + 5, scroll < maxScroll ? accent : muted);
        if(click && Contains(previous, pointer)) scroll -= list.height;
        if(click && Contains(next, pointer)) scroll += list.height;
        if(Contains(list, pointer)) scroll -= static_cast<float>(input.GetScrollDelta().y) * rowHeight * 2;
        if(input.WasPressed(Key::PageDown)) scroll += list.height;
        if(input.WasPressed(Key::PageUp)) scroll -= list.height;
        if(input.WasPressed(Key::Home)) scroll = 0;
        if(input.WasPressed(Key::End)) scroll = maxScroll;
        scroll = std::clamp(scroll, 0.f, maxScroll);
        if(maxScroll > 0 && list.height > 0)
        {
            const dyf::Rectangle track{list.x + list.width - 12,list.y,12,list.height};
            const float thumbHeight = std::min(list.height, std::max(28.f, list.height * list.height / total));
            const float travel = list.height - thumbHeight;
            const float thumbY = list.y + scroll / maxScroll * travel;
            if(click && Contains(track, pointer))
            {
                dragging = true;
                dragOffset = pointer.y >= thumbY && pointer.y < thumbY + thumbHeight ? static_cast<float>(pointer.y) - thumbY : thumbHeight / 2;
            }
            if(!input.IsDown(MouseButton::Left)) dragging = false;
            if(dragging && travel > 0) scroll = std::clamp((static_cast<float>(pointer.y) - list.y - dragOffset) / travel * maxScroll, 0.f, maxScroll);
            view.Fill(track, {.08f,.10f,.13f,1});
            view.Fill({track.x,list.y + scroll / maxScroll * travel,track.width,thumbHeight}, dragging ? accent : muted);
        }
        else dragging = false;
        view.Clip(list);
        if(visible.empty()) view.Text("No examples in this category.", left + 16, list.y + 24, muted);
        for(size_t row = 0; row < visible.size(); ++row)
        {
            const auto index = visible[row];
            const auto& example = examples[index];
            const dyf::Rectangle rect{left,list.y + row * rowHeight - scroll,contentWidth - 16,rowHeight - 6};
            if(rect.y + rect.height <= list.y || rect.y >= list.y + list.height) continue;
            const bool hover = Contains(list, pointer) && Contains(rect, pointer);
            view.Fill(rect, active == &example ? Color{.09f,.22f,.28f,1} : hover ? Color{.11f,.16f,.22f,1} : Color{.075f,.095f,.125f,1});
            view.Text(std::to_string(index + 1), rect.x + 12, rect.y + 18, muted);
            view.Clip({rect.x + 46,std::max(rect.y,list.y),std::max(0.f,rect.width - 130),
                std::max(0.f,std::min(rect.y + rect.height,list.y + list.height) - std::max(rect.y,list.y))});
            view.Text(Title(example), rect.x + 48, rect.y + 7);
            view.Text(example.name, rect.x + 48, rect.y + 32, muted);
            view.Clip(list);
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
            view.Text(example.entry ? "RUN" : "N/A", rect.x + rect.width - 62, rect.y + 18,
                example.entry ? accent : muted);
            if(click && hover)
            {
                if(!example.entry) status = "This example requires a desktop-only extension.";
                else { selected = &example; return 0; }
            }
#else
            view.Text(active == &example ? "OPEN" : "RUN", rect.x + rect.width - 62, rect.y + 18,
                child.Running() ? muted : accent);
            if(click && hover && !child.Running())
            {
                std::string error;
                if(child.Start(example, {}, error))
                {
                    active = &example;
                    status = std::string("Running: ") + example.name + ". Close its window to run another example.";
                }
                else status = error;
            }
#endif
        }
        canvas->ResetClipRect();
        view.Fill({sidebar,height - 60,width - sidebar,60}, {.06f,.08f,.10f,1});
        view.Clip({left,height - 60,contentWidth,60});
        view.Text(status, left, height - 44, active ? accent : white);
        view.Text("Scroll to browse. Escape closes this window.", left, height - 22, muted);
        canvas->ResetClipRect();
        if(!renderer->Render(*canvas)) return 1;
    }
    return 0;
}
}

namespace
{
int RunExamples(int argc, char** argv, const char* assetRoot, const char* fontPath,
    void (*onExampleChanged)(bool), const std::function<void(const Example&)>& prepareAssets = {}) try
{
#ifdef DY_PROFILER_TRACING_ENABLED
    dyf::TracyAdapter tracy;
#endif
    if(fontPath && *fontPath)
    {
#ifdef _WIN32
        _putenv_s("DY_EXAMPLES_FONT", fontPath);
#else
        setenv("DY_EXAMPLES_FONT", fontPath, 1);
#endif
    }
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    const auto root = std::filesystem::absolute(assetRoot ? assetRoot : ".");
    const auto invoke = [&](const Example& example, const std::vector<std::string>& arguments)
    {
        if(!example.entry) return 77;
        int result = 1;
        bool started = false;
        try
        {
            // The browser and --list need no model assets. Prepare only the
            // selected entry's directory.
            if(prepareAssets) prepareAssets(example);
            auto directory = root / example.executable;
            if(!std::filesystem::is_directory(directory)) directory = root;
            std::filesystem::current_path(directory);
            std::vector<std::string> values{example.name};
            values.insert(values.end(), arguments.begin(), arguments.end());
            std::vector<char*> pointers;
            for(auto& value : values) pointers.push_back(value.data());
            pointers.push_back(nullptr);
            if(onExampleChanged) onExampleChanged(true);
            started = true;
            result = example.entry(static_cast<int>(values.size()), pointers.data());
        }
        catch(const std::exception& error) { std::cerr << example.name << ": " << error.what() << '\n'; }
        if(started && onExampleChanged) onExampleChanged(false);
        std::filesystem::current_path(root);
        return result;
    };
#else
    (void)assetRoot;
    (void)onExampleChanged;
    (void)prepareAssets;
#endif
    if(argc > 1)
    {
        const std::string_view selection(argv[1]);
        if(selection == "--list") { List(); return 0; }
        if(selection == "--help")
        {
            std::cout << "examples [--list | number | target-name] [example arguments...]\n"
                "Without arguments, open the GUI example browser.\n";
            return 0;
        }
        const auto* example = Find(selection);
        if(!example) { std::cerr << "Unknown example. Use --list.\n"; return 2; }
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
        return invoke(*example, {argv + 2, argv + argc});
#else
        Process process;
        std::string error;
        if(!process.Start(*example, {argv + 2, argv + argc}, error))
        {
            std::cerr << error << '\n';
            return 1;
        }
        int result = 1;
        process.Finish(result, true);
        return result;
#endif
    }
    std::string status = "Click an example to open its window.";
    while(true)
    {
        const Example* selected = nullptr;
        const int result = OpenGui(selected, status);
        if(result || !selected) return result;
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
        const int exitCode = invoke(*selected, {});
        status = std::string(selected->name) + (exitCode == 0 ? " closed." : exitCode == 77 ?
            ": unavailable on this backend, or not implemented." : ": exited with code " + std::to_string(exitCode) + ".");
#endif
    }
}
catch(const std::exception& error)
{
    std::cerr << error.what() << '\n';
    return 1;
}
}

extern "C" int DyfRunExamples(int argc, char** argv, const char* assetRoot, const char* fontPath,
    void (*onExampleChanged)(bool))
{
    return RunExamples(argc, argv, assetRoot, fontPath, onExampleChanged);
}

#ifdef __ANDROID__
namespace
{
using Asset = std::unique_ptr<AAsset, decltype(&AAsset_close)>;

// NativeActivity runs this entry on a native thread. Attach only while reading
// the launch intent or installed-package metadata.
class JavaEnvironment
{
public:
    explicit JavaEnvironment(JavaVM& vm) : vm(vm)
    {
        const auto result = vm.GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if(result == JNI_EDETACHED)
        {
            if(vm.AttachCurrentThread(&env, nullptr) != JNI_OK)
                throw std::runtime_error("Cannot attach Android example thread to Java.");
            attached = true;
        }
        else if(result != JNI_OK)
            throw std::runtime_error("Cannot access Android Java environment.");
        if(env->PushLocalFrame(16) < 0)
        {
            env->ExceptionClear();
            if(attached) vm.DetachCurrentThread();
            throw std::runtime_error("Cannot allocate Android example references.");
        }
    }
    ~JavaEnvironment()
    {
        env->PopLocalFrame(nullptr);
        if(attached) vm.DetachCurrentThread();
    }
    JNIEnv* operator->() const { return env; }
    void Require(bool condition, const char* message) const
    {
        if(env->ExceptionCheck())
        {
            env->ExceptionClear();
            throw std::runtime_error(message);
        }
        if(!condition) throw std::runtime_error(message);
    }

private:
    JavaVM& vm;
    JNIEnv* env = nullptr;
    bool attached = false;
};

std::string LaunchExample(ANativeActivity& activity)
{
    JavaEnvironment env(*activity.vm);
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

std::string InstalledApkIdentity(ANativeActivity& activity)
{
    JavaEnvironment env(*activity.vm);
    constexpr const char* error = "Cannot read installed Android package identity.";
    const auto activityClass = env->GetObjectClass(activity.clazz);
    env.Require(activityClass, error);
    const auto getPackageManager = env->GetMethodID(activityClass, "getPackageManager", "()Landroid/content/pm/PackageManager;");
    env.Require(getPackageManager, error);
    const auto getPackageName = env->GetMethodID(activityClass, "getPackageName", "()Ljava/lang/String;");
    env.Require(getPackageName, error);
    const auto getPackageCodePath = env->GetMethodID(activityClass, "getPackageCodePath", "()Ljava/lang/String;");
    env.Require(getPackageCodePath, error);
    const auto manager = env->CallObjectMethod(activity.clazz, getPackageManager);
    env.Require(manager, error);
    const auto name = env->CallObjectMethod(activity.clazz, getPackageName);
    env.Require(name, error);
    const auto managerClass = env->GetObjectClass(manager);
    env.Require(managerClass, error);
    const auto getPackageInfo = env->GetMethodID(managerClass, "getPackageInfo", "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;");
    env.Require(getPackageInfo, error);
    const auto info = env->CallObjectMethod(manager, getPackageInfo, name, jint{0});
    env.Require(info, error);
    const auto infoClass = env->GetObjectClass(info);
    env.Require(infoClass, error);
    const auto lastUpdateTime = env->GetFieldID(infoClass, "lastUpdateTime", "J");
    env.Require(lastUpdateTime, error);
    const auto updated = env->GetLongField(info, lastUpdateTime);
    env.Require(true, error);
    const auto path = static_cast<jstring>(env->CallObjectMethod(activity.clazz, getPackageCodePath));
    env.Require(path, error);
    const auto release = [&](const char* value) { if(value) env->ReleaseStringUTFChars(path, value); };
    const std::unique_ptr<const char, decltype(release)> text(env->GetStringUTFChars(path, nullptr), release);
    env.Require(text != nullptr, error);
    struct stat metadata{};
    if(::stat(text.get(), &metadata) != 0 || !S_ISREG(metadata.st_mode))
        throw std::runtime_error("Cannot read installed Android APK metadata.");
    // versionCode may remain unchanged across development installs. Package
    // update time and the actual immutable installed APK identify this cache.
    std::ostringstream identity;
    identity << text.get() << '\n' << updated << ' ' << metadata.st_dev << ' ' << metadata.st_ino << ' '
        << metadata.st_size << ' ' << metadata.st_mtim.tv_sec << ' ' << metadata.st_mtim.tv_nsec << '\n';
    return identity.str();
}

class AndroidAssets
{
public:
    explicit AndroidAssets(android_app& app) : m_app(app),
        m_root(std::filesystem::path(app.activity->internalDataPath) / "examples")
    {
        std::filesystem::create_directories(m_root);
    }

    const std::filesystem::path& Root() const { return m_root; }

    void Prepare(const Example& example)
    {
        const std::filesystem::path target(example.executable);
        if(target.empty() || target.has_parent_path() || target == "." || target == "..")
            throw std::runtime_error("Invalid example asset directory.");
        LoadManifest();
        const std::string prefix = target.string() + "/";
        std::vector<const std::string*> paths;
        std::string expected = "dyf-example-assets-v1\n" + m_identity;
        for(const auto& relative : m_paths)
            if(relative.compare(0, prefix.size(), prefix) == 0)
            {
                paths.push_back(&relative);
                expected += relative + '\n';
            }
        const auto directory = m_root / target;
        const auto marker = m_root / ".asset-cache" / (target.string() + ".ready");
        if(Matches(marker, expected) && Complete(paths)) return;

        // These are framework-owned extracted resources, not application user
        // storage. Clear only this target so removed/renamed APK assets cannot
        // survive an update or an interrupted extraction.
        std::filesystem::remove(marker);
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);
        for(const auto* relative : paths) Extract(*relative);
        std::filesystem::create_directories(marker.parent_path());
        auto pending = marker;
        pending += ".tmp";
        {
            std::ofstream output(pending, std::ios::binary | std::ios::trunc);
            output.write(expected.data(), static_cast<std::streamsize>(expected.size()));
            output.close();
            if(!output) throw std::runtime_error("Cannot record extracted example assets.");
        }
        // No valid marker exists until every selected asset has been closed.
        std::filesystem::rename(pending, marker);
    }

private:
    void LoadManifest()
    {
        if(!m_identity.empty()) return;
        const auto identity = InstalledApkIdentity(*m_app.activity);
        Asset manifest(AAssetManager_open(m_app.activity->assetManager, "files.txt", AASSET_MODE_BUFFER), AAsset_close);
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
        std::vector<std::string> entries;
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
                if(component == ".." || component == ".")
                    throw std::runtime_error("Invalid packaged asset path.");
            entries.push_back(std::move(relative));
        }
        m_paths = std::move(entries);
        m_identity = identity;
    }

    static bool Matches(const std::filesystem::path& path, const std::string& expected)
    {
        std::ifstream input(path, std::ios::binary);
        if(!input) return false;
        std::string existing(expected.size(), '\0');
        input.read(existing.data(), static_cast<std::streamsize>(existing.size()));
        return input.gcount() == static_cast<std::streamsize>(expected.size()) &&
            input.peek() == std::char_traits<char>::eof() && !input.bad() && existing == expected;
    }

    bool Complete(const std::vector<const std::string*>& paths) const
    {
        for(const auto* relative : paths)
        {
            Asset asset(AAssetManager_open(m_app.activity->assetManager, relative->c_str(), AASSET_MODE_STREAMING), AAsset_close);
            if(!asset) return false;
            const auto size = AAsset_getLength64(asset.get());
            const auto destination = m_root / *relative;
            std::error_code error;
            if(size < 0 || !std::filesystem::is_regular_file(destination, error) || error ||
                std::filesystem::file_size(destination, error) != static_cast<uintmax_t>(size) || error)
                return false;
        }
        return true;
    }

    void Extract(const std::string& relative)
    {
        Asset asset(AAssetManager_open(m_app.activity->assetManager, relative.c_str(), AASSET_MODE_STREAMING), AAsset_close);
        if(!asset) throw std::runtime_error("Packaged asset is missing: " + relative);
        const auto size = AAsset_getLength64(asset.get());
        if(size < 0) throw std::runtime_error("Invalid packaged asset size: " + relative);
        const auto expectedBytes = static_cast<uint64_t>(size);
        uint64_t copiedBytes = 0;
        const auto destination = m_root / relative;
        std::filesystem::create_directories(destination.parent_path());
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        if(!output) throw std::runtime_error("Cannot extract example asset: " + relative);
        std::array<char, 64 * 1024> buffer;
        int count;
        while((count = AAsset_read(asset.get(), buffer.data(), buffer.size())) > 0)
        {
            if(static_cast<uint64_t>(count) > expectedBytes - copiedBytes)
                throw std::runtime_error("Packaged asset size changed: " + relative);
            output.write(buffer.data(), count);
            if(!output) throw std::runtime_error("Cannot extract example asset: " + relative);
            copiedBytes += static_cast<uint64_t>(count);
            dyf::Platform::Window::PollEvents();
            if(m_app.destroyRequested) throw std::runtime_error("Example asset extraction interrupted.");
        }
        output.close();
        if(count < 0 || !output || copiedBytes != expectedBytes)
            throw std::runtime_error("Cannot extract complete example asset: " + relative);
    }

    android_app& m_app;
    std::filesystem::path m_root;
    std::string m_identity;
    std::vector<std::string> m_paths;
};
}

int DyfRunExamples(android_app* state)
{
    dyf::Platform::AndroidApplication application(*state);
    int result = 1;
    try
    {
        AndroidAssets assets(*state);
        auto selection = LaunchExample(*state->activity);
        char name[] = "dy_engine examples";
        char* arguments[] = {name, selection.empty() ? nullptr : selection.data(), nullptr};
        launchSelection = selection.empty() ? nullptr : selection.c_str();
        const auto root = assets.Root().string();
        result = RunExamples(selection.empty() ? 1 : 2, arguments,
            root.c_str(), "/system/fonts/Roboto-Regular.ttf", ReportExampleState,
            [&](const Example& example) { assets.Prepare(example); });
        launchSelection = nullptr;
        __android_log_print(result == 0 ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
            "dy_engine", "Application exited with code %d", result);
    }
    catch(const std::exception& error)
    {
        launchSelection = nullptr;
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
    return result;
}
#endif

#if !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IPHONE)
int main(int argc, char** argv)
{
    return DyfRunExamples(argc, argv, nullptr, nullptr, nullptr);
}
#endif

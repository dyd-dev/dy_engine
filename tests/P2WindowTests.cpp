#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <dyf/Platform/Window.h>
#include <dyf/Platform/RenderDocCapture.h>
#include <dyf/Platform/Log.h>
#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>
#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#endif
#include <GLFW/glfw3native.h>

static bool failNextAllocation = false;
void* operator new(std::size_t size)
{
    if(failNextAllocation) { failNextAllocation = false; throw std::bad_alloc(); }
    if(void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

struct GLFWwindow
{
    void* user = nullptr;
    GLFWkeyfun key = nullptr;
    GLFWmousebuttonfun mouse = nullptr;
    GLFWcursorposfun cursor = nullptr;
    GLFWscrollfun scroll = nullptr;
    GLFWcharfun text = nullptr;
    GLFWwindowfocusfun focus = nullptr;
    bool alive = false;
};
static GLFWwindow nativeWindows[4];
static int created = 0, destroyed = 0, terminated = 0;
static bool failCaptureInit = false, failCaptureTrigger = false;
struct CaptureFailure {};
namespace dyf::Platform
{
bool RenderDocCapture::Initialize() { if(failCaptureInit) throw CaptureFailure{}; return true; }
bool RenderDocCapture::TriggerNextFrame() { if(failCaptureTrigger) throw CaptureFailure{}; return true; }
namespace Log
{
void Initialize() noexcept {}
void Writef(LogLevel, std::string_view, const char*, int, const char*, ...) noexcept {}
}
}

// Replace the native dispatcher/capture service, while compiling the real Window
// and Input implementations. No OS windows or GPU work are started by this test.
extern "C"
{
int glfwInit() { return GLFW_TRUE; }
void glfwTerminate() { ++terminated; }
void glfwWindowHint(int, int) {}
GLFWwindow* glfwCreateWindow(int, int, const char*, GLFWmonitor*, GLFWwindow*)
{
    auto* result = &nativeWindows[created++]; *result = {}; result->alive = true; return result;
}
void glfwDestroyWindow(GLFWwindow* window) { if(window->alive) { window->alive = false; ++destroyed; } }
void glfwSetWindowUserPointer(GLFWwindow* w, void* p) { w->user = p; }
void* glfwGetWindowUserPointer(GLFWwindow* w) { return w->user; }
#define P2_GLFW_CALLBACK(name, field, type) type name(GLFWwindow* w, type f) { auto old = w->field; w->field = f; return old; }
P2_GLFW_CALLBACK(glfwSetKeyCallback, key, GLFWkeyfun)
P2_GLFW_CALLBACK(glfwSetMouseButtonCallback, mouse, GLFWmousebuttonfun)
P2_GLFW_CALLBACK(glfwSetCursorPosCallback, cursor, GLFWcursorposfun)
P2_GLFW_CALLBACK(glfwSetScrollCallback, scroll, GLFWscrollfun)
P2_GLFW_CALLBACK(glfwSetCharCallback, text, GLFWcharfun)
P2_GLFW_CALLBACK(glfwSetWindowFocusCallback, focus, GLFWwindowfocusfun)
#undef P2_GLFW_CALLBACK
void glfwGetCursorPos(GLFWwindow*, double* x, double* y) { *x = *y = 0; }
int glfwGetWindowAttrib(GLFWwindow*, int attrib) { return attrib == GLFW_FOCUSED ? GLFW_TRUE : GLFW_FALSE; }
int glfwWindowShouldClose(GLFWwindow*) { return GLFW_FALSE; }
void glfwSetWindowShouldClose(GLFWwindow*, int) {}
void glfwPollEvents() {}
void glfwWaitEventsTimeout(double) {}
void glfwSetWindowSize(GLFWwindow*, int, int) {}
void glfwGetWindowSize(GLFWwindow*, int* x, int* y) { *x = *y = 64; }
void glfwGetFramebufferSize(GLFWwindow*, int* x, int* y) { *x = *y = 64; }
void glfwGetWindowContentScale(GLFWwindow*, float* x, float* y) { *x = *y = 1; }
int glfwGetInputMode(GLFWwindow*, int) { return GLFW_CURSOR_NORMAL; }
void glfwSetInputMode(GLFWwindow*, int, int) {}
int glfwRawMouseMotionSupported() { return GLFW_FALSE; }
#if defined(_WIN32)
HWND glfwGetWin32Window(GLFWwindow* w) { return reinterpret_cast<HWND>(w); }
#elif defined(__APPLE__)
id glfwGetCocoaWindow(GLFWwindow*) { return nullptr; }
#endif
}

using namespace dyf::Platform;
int failures = 0;
void Check(bool value, const char* message)
{
    if(!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
void ConstructorRollback(bool keepAnotherWindow)
{
    std::unique_ptr<Window> first;
    if(keepAnotherWindow) first = std::make_unique<Window>(64, 64);
    failCaptureInit = true;
    bool propagated = false;
    try { Window rejected(64, 64); } catch(const CaptureFailure&) { propagated = true; }
    Check(propagated, "constructor must preserve original exception");
    Check(destroyed == 1, "failed constructor must destroy its native window");
    Check(terminated == (keepAnotherWindow ? 0 : 1), "rollback must terminate GLFW only after the last window");
    // A baseline failure exits without accessing its stale registry entry.
    if(failures) first.release();
}
void ReleaseOrFocus(bool focusLoss)
{
    Window window(64, 64);
    auto* native = window.GetGlfwHandle();
    native->key(native, GLFW_KEY_A, 0, GLFW_PRESS, 0);
    Window::PollEvents();
    Check(window.GetInput().IsDown(Key::A), "initial press not published");
    bool escapedCallback = false, reported = false;
    failNextAllocation = true;
    try
    {
        if(focusLoss) native->focus(native, GLFW_FALSE);
        else native->key(native, GLFW_KEY_A, 0, GLFW_RELEASE, 0);
    }
    catch(...) { escapedCallback = true; }
    failNextAllocation = false;
    try { Window::PollEvents(); } catch(const std::bad_alloc&) { reported = true; }
    Check(!escapedCallback, "input allocation exception must not escape native callback");
    Check(reported, "callback failure must be reported by PollEvents");
    Check(!window.GetInput().IsDown(Key::A) && window.GetInput().WasReleased(Key::A),
        "PollEvents must publish release/focus state before reporting callback failure");
    bool repeated = false;
    try { Window::PollEvents(); } catch(...) { repeated = true; }
    Check(!repeated, "already reported callback exception must not repeat");
}
void CaptureCallbackFailure()
{
    Window window(64, 64);
    auto* native = window.GetGlfwHandle();
    failCaptureTrigger = true;
    bool escapedCallback = false, reported = false;
    try { native->key(native, GLFW_KEY_F12, 0, GLFW_PRESS, 0); }
    catch(...) { escapedCallback = true; }
    try { Window::WaitEvents(.001); } catch(const CaptureFailure&) { reported = true; }
    Check(!escapedCallback && reported, "capture failure must move from native callback to WaitEvents");
    Window::PollEvents();
    Check(window.GetInput().WasPressed(Key::F12), "capture failure must preserve normal input event");
}
int main(int argc, char** argv)
{
    if(argc != 2) return 2;
    if(std::strcmp(argv[1], "constructor") == 0) ConstructorRollback(false);
    else if(std::strcmp(argv[1], "constructor-shared") == 0) ConstructorRollback(true);
    else if(std::strcmp(argv[1], "release") == 0) ReleaseOrFocus(false);
    else if(std::strcmp(argv[1], "focus") == 0) ReleaseOrFocus(true);
    else if(std::strcmp(argv[1], "capture") == 0) CaptureCallbackFailure();
    else return 2;
    std::printf("P2 window %s: %d failures\n", argv[1], failures);
    return failures ? 1 : 0;
}

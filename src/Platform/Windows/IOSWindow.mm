#include "WindowBackend.h"
#include "IOSApplication.h"
#import <UIKit/UIKit.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

namespace dyf::Platform
{
    struct IOSApplicationState
    {
        explicit IOSApplicationState(void* value) : view(value)
        {
            if(!view) throw std::invalid_argument("iOS application requires a UIKit view.");
            CFRetain(static_cast<CFTypeRef>(view));
        }
        ~IOSApplicationState()
        {
            const auto retained = static_cast<CFTypeRef>(view);
            if([NSThread isMainThread]) CFRelease(retained);
            else dispatch_sync(dispatch_get_main_queue(), ^{ CFRelease(retained); });
        }
        void* view;
        std::mutex mutex;
        std::condition_variable changed;
        WindowSize size;
        ContentScale scale;
        bool focused = false, visible = false, stopped = false;
        uint64_t latestWindow = 0, closeThrough = 0;
        std::vector<InputEvent> events;
    };

    namespace
    {
        std::mutex bindingMutex;
        std::weak_ptr<IOSApplicationState> application;
        thread_local IOSApplicationState* workerApplication = nullptr;
#if !__has_feature(objc_arc)
        thread_local NSAutoreleasePool* workerPool = nil;
        void DrainWorkerPool()
        {
            if(workerPool)
            {
                [workerPool drain];
                workerPool = [[NSAutoreleasePool alloc] init];
            }
        }
#else
        void DrainWorkerPool() {}
#endif
        std::shared_ptr<IOSApplicationState> CurrentApplication()
        {
            std::lock_guard<std::mutex> lock(bindingMutex);
            return application.lock();
        }
        void QueueInput(IOSApplicationState& state, const InputEvent& event)
        {
            if(state.events.size() >= 4096)
            {
                state.events.clear();
                InputEvent reset;
                reset.type = InputEventType::Focus;
                state.events.push_back(reset);
            }
            state.events.push_back(event);
        }
    }

    IOSApplication::IOSApplication(void* view) : state(std::make_shared<IOSApplicationState>(view))
    {
        if(![NSThread isMainThread])
            throw std::logic_error("Create the iOS application host on the UIKit thread.");
        std::lock_guard<std::mutex> lock(bindingMutex);
        if(!application.expired()) throw std::logic_error("An iOS application host is already active.");
        application = state;
    }
    IOSApplication::~IOSApplication() { Stop(); }
    void IOSApplication::Update(WindowSize size, ContentScale scale, bool focused, bool visible)
    {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->size = size;
            state->scale = scale;
            state->visible = visible;
            if(state->focused != focused)
            {
                state->focused = focused;
                InputEvent event;
                event.type = InputEventType::Focus;
                event.focused = focused;
                QueueInput(*state, event);
            }
        }
        state->changed.notify_all();
    }
    void IOSApplication::PushInput(const InputEvent& event)
    {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if(state->stopped) return;
            QueueInput(*state, event);
        }
        state->changed.notify_all();
    }
    void IOSApplication::RequestClose()
    {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->closeThrough = state->latestWindow;
            state->events.clear();
        }
        state->changed.notify_all();
    }
    void IOSApplication::Stop()
    {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->stopped = true;
        }
        state->changed.notify_all();
    }
    int IOSApplication::Run(const std::function<int()>& entry)
    {
        if([NSThread isMainThread] || workerApplication)
            throw std::logic_error("Run iOS examples on one application worker.");
        workerApplication = state.get();
#if !__has_feature(objc_arc)
        workerPool = [[NSAutoreleasePool alloc] init];
#endif
        struct Cleanup
        {
            ~Cleanup()
            {
#if !__has_feature(objc_arc)
                [workerPool drain];
                workerPool = nil;
#endif
                workerApplication = nullptr;
            }
        } cleanup;
        return entry();
    }

    class IOSWindow final : public WindowBackend
    {
    public:
        IOSWindow(Window& owner, const WindowDescription& desc) : WindowBackend(owner)
        {
            if(desc.borrowed.kind != NativeWindowKind::None && desc.borrowed.kind != NativeWindowKind::UIKit)
                throw std::invalid_argument("iOS Window requires a UIKit view.");
            state = CurrentApplication();
            if(!state)
            {
                if(![NSThread isMainThread] || !desc.borrowed.value)
                    throw std::invalid_argument("iOS Window needs the application host or a borrowed UIKit view.");
                state = std::make_shared<IOSApplicationState>(desc.borrowed.value);
                UIView* view = (__bridge UIView*)desc.borrowed.value;
                state->size = {int(view.bounds.size.width), int(view.bounds.size.height)};
                state->scale = {float(view.contentScaleFactor), float(view.contentScaleFactor)};
                state->visible = view.window && !view.window.hidden;
                state->focused = state->visible && UIApplication.sharedApplication.applicationState == UIApplicationStateActive;
            }
            if(desc.borrowed.value && desc.borrowed.value != state->view)
                throw std::invalid_argument("iOS Window belongs to another application view.");
            InputEvent focus;
            focus.type = InputEventType::Focus;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                generation = ++state->latestWindow;
                state->events.clear();
                focus.focused = state->focused;
            }
            WindowPlatformAccess::HandleInputEvent(owner, focus);
        }
        bool IsRunning() const override
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            return !closed && !state->stopped && generation > state->closeThrough;
        }
        void RequestClose() const override { closed = true; }
        void Resize(unsigned int, unsigned int) const override {}
        WindowSize GetSize() const override
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            return state->size;
        }
        WindowSize GetFramebufferSize() const override
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            return {int(state->size.width * state->scale.x), int(state->size.height * state->scale.y)};
        }
        ContentScale GetContentScale() const override
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            return state->scale;
        }
        bool HasFocus() const override
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            return state->focused;
        }
        bool IsMinimized() const override
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            return !state->visible || !state->focused;
        }
        void SetCursorMode(CursorMode) override {}
        CursorMode GetCursorMode() const override { return CursorMode::Normal; }
        bool IsRawMouseMotionSupported() const override { return false; }
        bool SetRawMouseMotion(bool value) override { return !value; }
        bool IsRawMouseMotionEnabled() const override { return false; }
        NativeWindowHandle GetNativeHandle() const override
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            return {NativeWindowKind::UIKit, state->stopped ? nullptr : state->view};
        }
    private:
        std::shared_ptr<IOSApplicationState> state;
        uint64_t generation = 0;
        mutable bool closed = false;
    };

    std::unique_ptr<WindowBackend> CreateWindowBackend(Window& owner, const WindowDescription& desc)
    {
        return std::make_unique<IOSWindow>(owner, desc);
    }
    void PollPlatformEvents()
    {
        DrainWorkerPool();
        auto* state = workerApplication;
        if(!state) return;
        std::vector<InputEvent> events;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->changed.wait(lock, [&] {
                return state->stopped || state->closeThrough >= state->latestWindow ||
                    (state->focused && state->visible);
            });
            events.swap(state->events);
        }
        for(const auto& event : events)
            for(auto* window : RegisteredWindows()) WindowPlatformAccess::HandleInputEvent(*window, event);
    }
    void WaitPlatformEvents(double seconds)
    {
        auto* state = workerApplication;
        if(!state) return;
        std::unique_lock<std::mutex> lock(state->mutex);
        state->changed.wait_for(lock, std::chrono::duration<double>(seconds), [&] {
            return state->stopped || state->closeThrough >= state->latestWindow || !state->events.empty();
        });
    }
}

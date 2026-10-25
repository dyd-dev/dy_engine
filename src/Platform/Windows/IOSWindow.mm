#include "WindowBackend.h"
#include "IOSApplication.h"
#include "../../RHI/WakeEvent.h"
#import <UIKit/UIKit.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace dyf::Platform
{
    namespace
    {
        static_assert(std::atomic<uint32_t>::is_always_lock_free &&
            std::atomic<uint64_t>::is_always_lock_free && std::atomic<bool>::is_always_lock_free,
            "The UIKit bridge requires lock-free indices, generations and flags.");

        // A single producer and consumer exchange ownership of three values.
        // Neither side reads a value while the other can overwrite it; getters
        // copy the owned front value without allocating or retrying a seqlock.
        template<class T>
        class PublishedSnapshot final
        {
            static constexpr uint32_t Dirty=4;
            std::array<T,3> values{};
            uint32_t back=2,front=0;
            std::atomic<uint32_t> middle{1};
        public:
            void Publish(const T& value) noexcept
            {
                values[back]=value;
                back=middle.exchange(back|Dirty,std::memory_order_acq_rel)&3u;
            }
            T Read() noexcept
            {
                if(middle.load(std::memory_order_acquire)&Dirty)
                    front=middle.exchange(front,std::memory_order_acq_rel)&3u;
                return values[front];
            }
        };
        struct WindowLayout
        {
            WindowSize size;
            ContentScale scale;
            bool focused=false,visible=false;
        };
        struct OverflowInput
        {
            uint64_t epoch=0;
            InputEvent event;
        };
    }

    struct IOSApplicationState
    {
        explicit IOSApplicationState(void* value) : view(value)
        {
            if(!view) throw std::invalid_argument("iOS application requires a UIKit view.");
            CFRetain(static_cast<CFTypeRef>(view));
        }
        ~IOSApplicationState()
        {
            const auto retained=static_cast<CFTypeRef>(view);
            if([NSThread isMainThread]) CFRelease(retained);
            // A UIKit completion may be joining the finished worker. Releasing
            // the last view reference must never synchronously wait back on it.
            else dispatch_async(dispatch_get_main_queue(), ^{CFRelease(retained);});
        }
        void* const view;
        PublishedSnapshot<WindowLayout> layout;
        WindowLayout producerLayout;
        std::atomic<bool> stopped{false},consumerActive{false},waiting{false};
        std::atomic<uint64_t> latestWindow{0},closeThrough{0};
        RHI::Detail::WakeEvent changed;

        static constexpr uint32_t InputCapacity=4096;
        std::array<InputEvent,InputCapacity> events;
        alignas(64) std::atomic<uint32_t> writeInput{0};
        alignas(64) std::atomic<uint32_t> readInput{0};
        PublishedSnapshot<OverflowInput> overflowInput;
        uint64_t producerOverflow=0,consumerOverflow=0;
        std::atomic<uint64_t> publishedOverflow{0},consumedOverflow{0};

        void Notify() noexcept
        {
            if(waiting.exchange(false,std::memory_order_acq_rel))changed.Signal();
        }
        bool Closed() const noexcept
        {
            return stopped.load(std::memory_order_acquire) ||
                closeThrough.load(std::memory_order_acquire)>=latestWindow.load(std::memory_order_acquire);
        }
        bool HasInput() const noexcept
        {
            return writeInput.load(std::memory_order_acquire)!=readInput.load(std::memory_order_relaxed) ||
                publishedOverflow.load(std::memory_order_acquire)>consumedOverflow.load(std::memory_order_relaxed);
        }
        // UIKit is the only producer. Full queues never overwrite unread slots.
        // Keep the latest overflow event separately until the consumer resets its
        // input state, preserving the old clear + Focus(false) + newest input path.
        void QueueInput(const InputEvent& event) noexcept
        {
            const auto write=writeInput.load(std::memory_order_relaxed);
            if(publishedOverflow.load(std::memory_order_relaxed)!=consumedOverflow.load(std::memory_order_acquire) ||
                uint32_t(write-readInput.load(std::memory_order_acquire))>=InputCapacity)
            {
                const OverflowInput value{++producerOverflow,event};
                overflowInput.Publish(value);
                publishedOverflow.store(value.epoch,std::memory_order_release);
                return;
            }
            events[write%InputCapacity]=event;
            writeInput.store(write+1,std::memory_order_release);
        }
        bool PopInput(InputEvent& event) noexcept
        {
            const auto read=readInput.load(std::memory_order_relaxed);
            if(read==writeInput.load(std::memory_order_acquire))return false;
            event=events[read%InputCapacity];
            readInput.store(read+1,std::memory_order_release);
            return true;
        }
        void DiscardInputs() noexcept
        {
            readInput.store(writeInput.load(std::memory_order_acquire),std::memory_order_release);
            consumerOverflow=overflowInput.Read().epoch;
            consumedOverflow.store(consumerOverflow,std::memory_order_release);
        }
    };

    namespace
    {
        // Registration belongs to UIKit. Workers acquire their strong owner in
        // Run, rather than locking a process-wide weak binding on every access.
        std::weak_ptr<IOSApplicationState> application;
        thread_local std::shared_ptr<IOSApplicationState> workerApplication;
#if !__has_feature(objc_arc)
        thread_local NSAutoreleasePool* workerPool=nil;
        void DrainWorkerPool()
        {
            if(workerPool)
            {
                [workerPool drain];
                workerPool=[[NSAutoreleasePool alloc] init];
            }
        }
#else
        void DrainWorkerPool() {}
#endif
        std::shared_ptr<IOSApplicationState> CurrentApplication()
        {
            if(workerApplication)return workerApplication;
            return [NSThread isMainThread]?application.lock():nullptr;
        }
        void BroadcastInput(const InputEvent& event)
        {
            for(auto* window:RegisteredWindows())WindowPlatformAccess::HandleInputEvent(*window,event);
        }
    }

    IOSApplication::IOSApplication(void* view)
    {
        if(![NSThread isMainThread])
            throw std::logic_error("Create the iOS application host on the UIKit thread.");
        if(!application.expired())throw std::logic_error("An iOS application host is already active.");
        state=std::make_shared<IOSApplicationState>(view);
        application=state;
    }
    IOSApplication::~IOSApplication() {Stop();}
    void IOSApplication::Update(WindowSize size,ContentScale scale,bool focused,bool visible)
    {
        if(![NSThread isMainThread])throw std::logic_error("Publish UIKit layout on its main thread.");
        const bool focusChanged=state->producerLayout.focused!=focused;
        state->producerLayout={size,scale,focused,visible};
        state->layout.Publish(state->producerLayout);
        if(focusChanged)
        {
            InputEvent event;
            event.type=InputEventType::Focus;
            event.focused=focused;
            state->QueueInput(event);
        }
        state->Notify();
    }
    void IOSApplication::PushInput(const InputEvent& event)
    {
        if(![NSThread isMainThread])throw std::logic_error("Publish UIKit input on its main thread.");
        if(state->stopped.load(std::memory_order_acquire))return;
        state->QueueInput(event);
        state->Notify();
    }
    void IOSApplication::RequestClose()
    {
        const auto latest=state->latestWindow.load(std::memory_order_acquire);
        auto closed=state->closeThrough.load(std::memory_order_relaxed);
        while(closed<latest && !state->closeThrough.compare_exchange_weak(
            closed,latest,std::memory_order_release,std::memory_order_relaxed)) {}
        state->Notify();
    }
    void IOSApplication::Stop()
    {
        state->stopped.store(true,std::memory_order_release);
        state->Notify();
    }
    int IOSApplication::Run(const std::function<int()>& entry)
    {
        if([NSThread isMainThread] || workerApplication)
            throw std::logic_error("Run iOS examples on one application worker.");
        const auto runningState=state;
        bool active=false;
        if(!runningState->consumerActive.compare_exchange_strong(active,true,std::memory_order_acq_rel))
            throw std::logic_error("The iOS application already has an input worker.");
        workerApplication=runningState;
#if !__has_feature(objc_arc)
        workerPool=[[NSAutoreleasePool alloc] init];
#endif
        struct Cleanup
        {
            ~Cleanup()
            {
#if !__has_feature(objc_arc)
                [workerPool drain];
                workerPool=nil;
#endif
                workerApplication->consumerActive.store(false,std::memory_order_release);
                workerApplication.reset();
            }
        } cleanup;
        return entry();
    }

    class IOSWindow final : public WindowBackend
    {
    public:
        IOSWindow(Window& owner,const WindowDescription& desc):WindowBackend(owner)
        {
            if(desc.borrowed.kind!=NativeWindowKind::None && desc.borrowed.kind!=NativeWindowKind::UIKit)
                throw std::invalid_argument("iOS Window requires a UIKit view.");
            state=CurrentApplication();
            if(!state)
            {
                if(![NSThread isMainThread] || !desc.borrowed.value)
                    throw std::invalid_argument("iOS Window needs the application host or a borrowed UIKit view.");
                state=std::make_shared<IOSApplicationState>(desc.borrowed.value);
                UIView* view=(__bridge UIView*)desc.borrowed.value;
                const bool visible=view.window && !view.window.hidden;
                state->producerLayout={{int(view.bounds.size.width),int(view.bounds.size.height)},
                    {float(view.contentScaleFactor),float(view.contentScaleFactor)},
                    visible && UIApplication.sharedApplication.applicationState==UIApplicationStateActive,visible};
                state->layout.Publish(state->producerLayout);
            }
            if(state->consumerActive.load(std::memory_order_acquire) && workerApplication.get()!=state.get())
                throw std::logic_error("The running iOS application owns its windows on its worker.");
            if(desc.borrowed.value && desc.borrowed.value!=state->view)
                throw std::invalid_argument("iOS Window belongs to another application view.");
            generation=state->latestWindow.fetch_add(1,std::memory_order_acq_rel)+1;
            state->DiscardInputs();
            InputEvent focus;
            focus.type=InputEventType::Focus;
            focus.focused=state->layout.Read().focused;
            WindowPlatformAccess::HandleInputEvent(owner,focus);
        }
        bool IsRunning() const override
        {
            return !closed && !state->stopped.load(std::memory_order_acquire) &&
                generation>state->closeThrough.load(std::memory_order_acquire);
        }
        void RequestClose() const override {closed=true;}
        void Resize(unsigned int,unsigned int) const override {}
        WindowSize GetSize() const override {return state->layout.Read().size;}
        WindowSize GetFramebufferSize() const override
        {
            const auto layout=state->layout.Read();
            return {int(layout.size.width*layout.scale.x),int(layout.size.height*layout.scale.y)};
        }
        ContentScale GetContentScale() const override {return state->layout.Read().scale;}
        bool HasFocus() const override {return state->layout.Read().focused;}
        bool IsMinimized() const override
        {
            const auto layout=state->layout.Read();
            return !layout.visible || !layout.focused;
        }
        void SetCursorMode(CursorMode) override {}
        CursorMode GetCursorMode() const override {return CursorMode::Normal;}
        bool IsRawMouseMotionSupported() const override {return false;}
        bool SetRawMouseMotion(bool value) override {return !value;}
        bool IsRawMouseMotionEnabled() const override {return false;}
        NativeWindowHandle GetNativeHandle() const override
        {return {NativeWindowKind::UIKit,state->stopped.load(std::memory_order_acquire)?nullptr:state->view};}
    private:
        std::shared_ptr<IOSApplicationState> state;
        uint64_t generation=0;
        mutable bool closed=false;
    };

    std::unique_ptr<WindowBackend> CreateWindowBackend(Window& owner,const WindowDescription& desc)
    {return std::make_unique<IOSWindow>(owner,desc);}

    void PollPlatformEvents()
    {
        DrainWorkerPool();
        auto* state=workerApplication.get();
        if(!state)return;
        for(;;)
        {
            const auto layout=state->layout.Read();
            if(state->Closed() || (layout.focused && layout.visible))break;
            state->waiting.exchange(true,std::memory_order_acq_rel);
            const auto refreshed=state->layout.Read();
            if(!state->Closed() && !(refreshed.focused && refreshed.visible))state->changed.Wait();
            state->waiting.store(false,std::memory_order_release);
        }
        if(state->Closed()) {state->DiscardInputs();return;}
        if(state->publishedOverflow.load(std::memory_order_acquire)>state->consumerOverflow)
        {
            const auto overflow=state->overflowInput.Read();
            // The snapshot can include a publication whose epoch store is still
            // in flight. Remember its actual epoch rather than replaying it later.
            if(overflow.epoch!=state->consumerOverflow)
            {
                state->readInput.store(state->writeInput.load(std::memory_order_acquire),std::memory_order_release);
                InputEvent reset;
                reset.type=InputEventType::Focus;
                BroadcastInput(reset);
                BroadcastInput(overflow.event);
                state->consumerOverflow=overflow.epoch;
                state->consumedOverflow.store(overflow.epoch,std::memory_order_release);
            }
        }
        InputEvent event;
        // Bound one poll even if UIKit keeps producing new pointer motion.
        const auto available=uint32_t(state->writeInput.load(std::memory_order_acquire)-
            state->readInput.load(std::memory_order_relaxed));
        for(uint32_t i=0;i<available && state->PopInput(event);++i)BroadcastInput(event);
    }

    void WaitPlatformEvents(double seconds)
    {
        auto* state=workerApplication.get();
        if(!state || !std::isfinite(seconds) || seconds<=0)return;
        using Clock=std::chrono::steady_clock;
        const auto start=Clock::now();
        const auto maximum=Clock::time_point::max()-start;
        const auto duration=std::chrono::duration<double>(seconds);
        const auto deadline=duration>=maximum?Clock::time_point::max():
            start+std::chrono::duration_cast<Clock::duration>(duration);
        while(!state->Closed() && !state->HasInput())
        {
            const auto now=Clock::now();
            if(now>=deadline)return;
            state->waiting.exchange(true,std::memory_order_acq_rel);
            if(!state->Closed() && !state->HasInput())
            {
                const auto beforeWait=Clock::now();
                if(beforeWait<deadline)
                {
                    const auto remaining=std::chrono::duration_cast<std::chrono::nanoseconds>(deadline-beforeWait).count();
                    state->changed.WaitFor(static_cast<uint64_t>(remaining));
                }
            }
            state->waiting.store(false,std::memory_order_release);
        }
    }
}

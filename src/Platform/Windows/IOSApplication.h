#pragma once
#include "dyf/Platform/Window.h"
#include <functional>
#include <memory>

namespace dyf::Platform
{
    struct IOSApplicationState;

    // Private UIKit bridge. Only one worker owns the engine windows and input.
    class IOSApplication final
    {
    public:
        explicit IOSApplication(void* view);
        ~IOSApplication();
        IOSApplication(const IOSApplication&) = delete;
        IOSApplication& operator=(const IOSApplication&) = delete;
        void Update(WindowSize, ContentScale, bool focused, bool visible);
        void PushInput(const InputEvent&);
        void RequestClose();
        void Stop();
        int Run(const std::function<int()>&);
    private:
        std::shared_ptr<IOSApplicationState> state;
    };
}

#include "WindowBackend.h"
#include "AndroidApplication.h"
#include <android/configuration.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace dyf::Platform
{
	namespace
	{
		android_app* application = nullptr;

		int32_t OnInput(android_app*, AInputEvent* native)
		{
			if(AInputEvent_getType(native) == AINPUT_EVENT_TYPE_MOTION)
			{
				const auto action = AMotionEvent_getAction(native) & AMOTION_EVENT_ACTION_MASK;
				const auto x = AMotionEvent_getX(native, 0), y = AMotionEvent_getY(native, 0);
				for(auto* window : RegisteredWindows())
				{
					InputEvent event;
					event.type = InputEventType::Cursor;
					event.x = x;
					event.y = y;
					WindowPlatformAccess::HandleInputEvent(*window,event);

					if(action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_POINTER_DOWN ||
						action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_POINTER_UP ||
						action == AMOTION_EVENT_ACTION_CANCEL)
					{
						event.type = InputEventType::MouseButton;
						event.code = static_cast<int>(MouseButton::Left);
						event.action = (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_POINTER_DOWN)
							? InputAction::Press : InputAction::Release;
						WindowPlatformAccess::HandleInputEvent(*window,event);
					}
				}
				return 1;
			}

			if(AInputEvent_getType(native) == AINPUT_EVENT_TYPE_KEY &&
				AKeyEvent_getKeyCode(native) == AKEYCODE_BACK)
			{
				if(AKeyEvent_getAction(native) == AKEY_EVENT_ACTION_UP)
					for(auto* window : RegisteredWindows()) window->RequestClose();
				return 1;
			}
			return 0;
		}

		void Dispatch(int timeout)
		{
			if(!application) return;

			int events = 0;
			android_poll_source* source = nullptr;
			if(ALooper_pollOnce(timeout, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0 && source)
				source->process(application, source);
		}

		class AndroidWindow final : public WindowBackend
		{
		public:
			AndroidWindow(Window& owner, const WindowDescription& desc)
				: WindowBackend(owner), app(desc.borrowed.kind == NativeWindowKind::None ? application : nullptr)
			{
				if(desc.borrowed.kind != NativeWindowKind::None && desc.borrowed.kind != NativeWindowKind::Android)
					throw std::invalid_argument("Android Window requires ANativeWindow.");

				borrowed = static_cast<ANativeWindow*>(desc.borrowed.value);
				while(app && !app->window && !app->destroyRequested) Dispatch(-1);

				auto* native = static_cast<ANativeWindow*>(GetNativeHandle().value);
				if(native) ANativeWindow_setBuffersGeometry(native, 0, 0, WINDOW_FORMAT_RGBA_8888);

				InputEvent event;
				event.type = InputEventType::Focus;
				event.focused = native != nullptr;
				WindowPlatformAccess::HandleInputEvent(owner,event);
			}

			bool IsRunning() const override { return !closed && (app ? !app->destroyRequested : borrowed != nullptr); }

			void RequestClose() const override
			{
				closed = true;
			}

			void Resize(unsigned int, unsigned int) const override {}

			WindowSize GetSize() const override
			{
				auto* native = static_cast<ANativeWindow*>(GetNativeHandle().value);
				return native ? WindowSize{ ANativeWindow_getWidth(native), ANativeWindow_getHeight(native) } : WindowSize{};
			}

			WindowSize GetFramebufferSize() const override { return GetSize(); }
			ContentScale GetContentScale() const override
			{
				if(!app || !app->config) return {};
				const int density = AConfiguration_getDensity(app->config);
				if(density <= 0 || density == ACONFIGURATION_DENSITY_ANY || density == ACONFIGURATION_DENSITY_NONE)
					return {};
				const float scale = float(density) / float(ACONFIGURATION_DENSITY_MEDIUM);
				return {scale, scale};
			}
			bool HasFocus() const override { return app ? app->activityState == APP_CMD_RESUME : GetNativeHandle().value != nullptr; }
			bool IsMinimized() const override { return !GetNativeHandle().value; }
			void SetCursorMode(CursorMode) override {}
			CursorMode GetCursorMode() const override { return CursorMode::Normal; }
			bool IsRawMouseMotionSupported() const override { return false; }
			bool SetRawMouseMotion(bool value) override { return !value; }
			bool IsRawMouseMotionEnabled() const override { return false; }

			NativeWindowHandle GetNativeHandle() const override
			{
				return { NativeWindowKind::Android, app ? app->window : borrowed };
			}

		private:
			android_app* app = nullptr;
			ANativeWindow* borrowed = nullptr;
			mutable bool closed = false;
		};
	}

	AndroidApplication::AndroidApplication(android_app& app) noexcept
	{
		if(application && application->onInputEvent == OnInput) application->onInputEvent = nullptr;
		application = &app;
		application->onInputEvent = OnInput;
	}

	AndroidApplication::~AndroidApplication()
	{
		if(application && application->onInputEvent == OnInput) application->onInputEvent = nullptr;
		application = nullptr;
	}

	std::unique_ptr<WindowBackend> CreateWindowBackend(Window& owner, const WindowDescription& desc)
	{
		return std::make_unique<AndroidWindow>(owner, desc);
	}

	void PollPlatformEvents()
	{
		if(!application) return;

		int events = 0;
		android_poll_source* source = nullptr;
		while(ALooper_pollOnce(0, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0)
		{
			auto* previousWindow = application->window;
			if(source) source->process(application, source);
			// Expose TERM_WINDOW before draining a following INIT_WINDOW, even
			// when Android reuses the same ANativeWindow address for the new surface.
			if(application->destroyRequested || application->window != previousWindow) break;
		}
	}

	void WaitPlatformEvents(double seconds)
	{
		const auto maximum = (std::numeric_limits<int>::max)();
		Dispatch(static_cast<int>((std::min)(std::ceil(seconds * 1000.0), double(maximum))));
	}
}

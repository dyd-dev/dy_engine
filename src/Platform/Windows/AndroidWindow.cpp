#include "WindowBackend.h"
#include "AndroidApplication.h"
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
		int32_t primaryPointerId = -1;

		int32_t OnInput(android_app*, AInputEvent* native)
		{
			if(AInputEvent_getType(native) == AINPUT_EVENT_TYPE_MOTION)
			{
				const auto nativeAction = AMotionEvent_getAction(native);
				const auto action = nativeAction & AMOTION_EVENT_ACTION_MASK;
				const auto count = AMotionEvent_getPointerCount(native);
				const auto changed = static_cast<size_t>((nativeAction & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
					AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
				size_t pointer = count;
				bool press = false, release = false;
				if(action == AMOTION_EVENT_ACTION_DOWN)
				{
					if(changed >= count) return 1;
					pointer = changed;
					primaryPointerId = AMotionEvent_getPointerId(native, pointer);
					press = true;
				}
				else if(action == AMOTION_EVENT_ACTION_POINTER_DOWN) return 1;
				else if(action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_POINTER_UP)
				{
					if(primaryPointerId < 0 || changed >= count ||
						AMotionEvent_getPointerId(native, changed) != primaryPointerId) return 1;
					pointer = changed;
					release = true;
				}
				else
				{
					// Pointer indices can change when another finger leaves. Keep the
					// first contact by ID; secondary contacts never steal its drag.
					for(size_t index = 0; primaryPointerId >= 0 && index < count; ++index)
						if(AMotionEvent_getPointerId(native, index) == primaryPointerId) { pointer = index; break; }
					if(action == AMOTION_EVENT_ACTION_CANCEL) release = primaryPointerId >= 0;
					else if(primaryPointerId < 0 && count &&
						((AInputEvent_getSource(native) & AINPUT_SOURCE_MOUSE) == AINPUT_SOURCE_MOUSE ||
						 action == AMOTION_EVENT_ACTION_HOVER_MOVE || action == AMOTION_EVENT_ACTION_HOVER_ENTER ||
						 action == AMOTION_EVENT_ACTION_HOVER_EXIT)) pointer = 0;
				}
				for(auto* window : RegisteredWindows())
				{
					InputEvent event;
					if(pointer < count)
					{
						event.type = InputEventType::Cursor;
						event.x = AMotionEvent_getX(native, pointer);
						event.y = AMotionEvent_getY(native, pointer);
						WindowPlatformAccess::HandleInputEvent(*window,event);
					}
					if(press || release)
					{
						event.type = InputEventType::MouseButton;
						event.code = static_cast<int>(MouseButton::Left);
						event.action = press ? InputAction::Press : InputAction::Release;
						WindowPlatformAccess::HandleInputEvent(*window,event);
					}
				}
				if(release) primaryPointerId = -1;
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
			ContentScale GetContentScale() const override { return {}; }
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
		primaryPointerId = -1;
		application->onInputEvent = OnInput;
	}

	AndroidApplication::~AndroidApplication()
	{
		if(application && application->onInputEvent == OnInput) application->onInputEvent = nullptr;
		application = nullptr;
		primaryPointerId = -1;
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
			if(source) source->process(application, source);
			if(application->destroyRequested) break;
		}
	}

	void WaitPlatformEvents(double seconds)
	{
		const auto maximum = (std::numeric_limits<int>::max)();
		Dispatch(static_cast<int>((std::min)(std::ceil(seconds * 1000.0), double(maximum))));
	}
}

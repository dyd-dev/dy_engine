#pragma once
#include <bitset>
#include <cstddef>

namespace dyf::Platform
{
	// Values match GLFW's physical-key tokens; no GLFW header is needed by callers.
	enum class Key : int
	{
		Unknown = -1, Space = 32, Apostrophe = 39, Comma = 44, Minus, Period, Slash,
		Num0 = 48, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
		Semicolon = 59, Equal = 61,
		A = 65, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
		LeftBracket = 91, Backslash, RightBracket, GraveAccent = 96, World1 = 161, World2,
		Escape = 256, Enter, Tab, Backspace, Insert, Delete, Right, Left, Down, Up,
		PageUp, PageDown, Home, End,
		CapsLock = 280, ScrollLock, NumLock, PrintScreen, Pause,
		F1 = 290, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12, F13,
		F14, F15, F16, F17, F18, F19, F20, F21, F22, F23, F24, F25,
		Keypad0 = 320, Keypad1, Keypad2, Keypad3, Keypad4, Keypad5, Keypad6,
		Keypad7, Keypad8, Keypad9, KeypadDecimal, KeypadDivide, KeypadMultiply,
		KeypadSubtract, KeypadAdd, KeypadEnter, KeypadEqual,
		LeftShift = 340, LeftControl, LeftAlt, LeftSuper,
		RightShift, RightControl, RightAlt, RightSuper, Menu
	};

	enum class MouseButton : int { Left, Right, Middle, Button4, Button5, Button6, Button7, Button8 };
	struct InputPoint { double x = 0, y = 0; };

	// Read-only snapshot, updated for every window by Window::PollEvents().
	// Pressed and Released can both be true for a short tap in one frame.
	class Input
	{
	public:
		[[nodiscard]] bool IsDown(Key key) const { return Test(m_frame.down, Index(key)); }
		[[nodiscard]] bool WasPressed(Key key) const { return Test(m_frame.pressed, Index(key)); }
		[[nodiscard]] bool WasReleased(Key key) const { return Test(m_frame.released, Index(key)); }
		[[nodiscard]] bool IsDown(MouseButton button) const { return Test(m_frame.down, Index(button)); }
		[[nodiscard]] bool WasPressed(MouseButton button) const { return Test(m_frame.pressed, Index(button)); }
		[[nodiscard]] bool WasReleased(MouseButton button) const { return Test(m_frame.released, Index(button)); }
		// Position/delta use window-content coordinates; scroll uses wheel offsets.
		[[nodiscard]] InputPoint GetCursorPosition() const { return m_frame.position; }
		[[nodiscard]] InputPoint GetCursorDelta() const { return m_frame.delta; }
		[[nodiscard]] InputPoint GetScrollDelta() const { return m_frame.scroll; }

	private:
		friend class Window;
		static constexpr std::size_t KeyCount = 349, ButtonCount = KeyCount + 8;
		using Buttons = std::bitset<ButtonCount>;
		struct State
		{
			Buttons down, pressed, released;
			InputPoint position, delta, scroll;
		};
		static std::size_t Index(Key key)
		{
			const int value = static_cast<int>(key);
			return value >= 0 && value < static_cast<int>(KeyCount) ? static_cast<std::size_t>(value) : ButtonCount;
		}
		static std::size_t Index(MouseButton button)
		{
			const int value = static_cast<int>(button);
			return value >= 0 && value < 8 ? KeyCount + static_cast<std::size_t>(value) : ButtonCount;
		}
		static bool Test(const Buttons& buttons, std::size_t index) { return index < ButtonCount && buttons[index]; }
		void OnButton(std::size_t index, bool down);
		void OnCursor(double x, double y);
		void OnFocusLost();
		void PublishFrame();
		bool ConsumeKeyPress(Key key);
		State m_frame, m_pending;
		Buttons m_consumed;
		bool m_hasCursorPosition = false;
	};
}

#pragma once
#include "dyf/Platform/Input.h"
#include <unordered_map>

namespace dyf::Platform
{
	using ActionId = unsigned int; // Application-defined IDs, e.g. an enum with unsigned underlying type.
	struct InputCapture { bool keyboard = false, mouse = false; };

	// Optional action layer. Input must outlive the map; each map belongs to one window.
	// Configure bindings before the first Update. Independent maps can read the same raw input.
	class ActionMap
	{
	public:
		explicit ActionMap(const Input& input) : m_input(input) {}
		void BindButton(ActionId action, Key key);
		void BindButton(ActionId action, MouseButton button);
		void BindAxis(ActionId action, Key negative, Key positive);
		// Call after PollEvents and after deciding GUI capture. A captured held button must
		// be released before reactivation. Focus loss also cancels actions. Capture enabled
		// during a frame stays enabled until the next PollEvents; re-evaluation preserves
		// unrelated action edges. Skipped frames recover held state, not historical taps.
		void Update(InputCapture capture = {});
		[[nodiscard]] bool IsDown(ActionId action) const { return GetState(action).value != 0; }
		[[nodiscard]] bool WasPressed(ActionId action) const { return GetState(action).pressed; }
		[[nodiscard]] bool WasReleased(ActionId action) const { return GetState(action).released; }
		[[nodiscard]] float GetAxis(ActionId action) const { return GetState(action).value; }
		[[nodiscard]] InputPoint GetCursorDelta() const;
		[[nodiscard]] InputPoint GetScrollDelta() const;

	private:
		struct State { float value = 0; bool pressed = false, released = false; };
		struct Binding { std::size_t index; float scale; };
		struct Action { bool axis = false; std::vector<Binding> bindings; State state; float startValue = 0; bool previousDown = false; };
		Action& AddAction(ActionId action, bool axis);
		State GetState(ActionId action) const;
		void Evaluate(const Input::Buttons& down, bool focused, InputCapture capture);
		const Input& m_input;
		std::unordered_map<ActionId, Action> m_actions;
		Input::Buttons m_suppressed, m_startSuppressed;
		InputCapture m_capture;
		uint64_t m_frameNumber = 0;
		bool m_updated = false;
	};
}

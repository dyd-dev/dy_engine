#include "dyf/Platform/ActionMap.h"
#include <algorithm>
#include <stdexcept>

using namespace dyf::Platform;

ActionMap::Action& ActionMap::AddAction(ActionId id, bool axis)
{
	if(m_updated) throw std::logic_error("Configure action bindings before the first Update.");
	auto found = m_actions.find(id);
	if(found != m_actions.end() && found->second.axis != axis)
		throw std::invalid_argument("An action cannot mix button and axis bindings.");
	auto& action = m_actions[id];
	action.axis = axis;
	return action;
}

void ActionMap::BindButton(ActionId action, Key key)
{
	const auto index = Input::Index(key);
	if(index >= Input::ButtonCount) throw std::invalid_argument("Invalid action key.");
	AddAction(action, false).bindings.push_back({index, 1});
}

void ActionMap::BindButton(ActionId action, MouseButton button)
{
	const auto index = Input::Index(button);
	if(index >= Input::ButtonCount) throw std::invalid_argument("Invalid action mouse button.");
	AddAction(action, false).bindings.push_back({index, 1});
}

void ActionMap::BindAxis(ActionId action, Key negative, Key positive)
{
	const auto a = Input::Index(negative), b = Input::Index(positive);
	if(a >= Input::ButtonCount || b >= Input::ButtonCount || a == b)
		throw std::invalid_argument("Axis bindings require two distinct valid keys.");
	auto& bindings = AddAction(action, true).bindings;
	bindings.push_back({a, -1});
	bindings.push_back({b, 1});
}

ActionMap::State ActionMap::GetState(ActionId id) const
{
	const auto found = m_actions.find(id);
	return found == m_actions.end() ? State{} : found->second.state;
}

InputPoint ActionMap::GetCursorDelta() const
{
	// Read the published value directly: cursor/raw mode switches can reset it after Update.
	return m_updated && m_frameNumber == m_input.m_frameNumber && !m_capture.mouse && m_input.m_frame.focused
		? m_input.GetCursorDelta() : InputPoint{};
}

InputPoint ActionMap::GetScrollDelta() const
{
	return m_updated && m_frameNumber == m_input.m_frameNumber && !m_capture.mouse && m_input.m_frame.focused
		? m_input.GetScrollDelta() : InputPoint{};
}

void ActionMap::Evaluate(const Input::Buttons& down, bool focused, InputCapture capture)
{
	// ponytail: linear scan for small action maps; index bindings by input only if profiling warrants it.
	for(auto& entry : m_actions)
	{
		auto& action = entry.second;
		float value = 0;
		for(const auto& binding : action.bindings)
		{
			const bool blocked = !focused || (binding.index < Input::KeyCount ? capture.keyboard : capture.mouse);
			if(blocked && down[binding.index]) m_suppressed.set(binding.index);
			if(!blocked && down[binding.index] && !m_suppressed[binding.index]) value += binding.scale;
		}
		value = action.axis ? std::clamp(value, -1.f, 1.f) : (value > 0 ? 1.f : 0.f);
		auto& state = action.state;
		state.pressed |= state.value == 0 && value != 0;
		state.released |= state.value != 0 && value == 0;
		state.value = value;
	}
}

void ActionMap::Update(InputCapture capture)
{
	const bool sameFrame = m_updated && m_frameNumber == m_input.m_frameNumber;
	if(sameFrame)
	{
		capture.keyboard |= m_capture.keyboard;
		capture.mouse |= m_capture.mouse;
		if(capture.keyboard == m_capture.keyboard && capture.mouse == m_capture.mouse) return;
	}
	else
	{
		// Current-frame releases are replayed below; only recover releases from omitted frames here.
		for(std::size_t index = 0; index < Input::ButtonCount; ++index)
			if(m_suppressed[index] && m_input.m_lastReleaseFrame[index] > m_frameNumber &&
				m_input.m_lastReleaseFrame[index] < m_input.m_frameNumber) m_suppressed.reset(index);
		m_startSuppressed = m_suppressed;
		for(auto& entry : m_actions) entry.second.startValue = entry.second.state.value;
	}
	m_updated = true;
	m_frameNumber = m_input.m_frameNumber;
	m_capture = capture;
	for(auto& entry : m_actions)
	{
		auto& action = entry.second;
		action.previousDown = action.state.value != 0;
		action.state = {action.startValue, false, false};
	}
	auto down = m_input.m_frameStartDown;
	bool focused = m_input.m_frameStartFocused;
	m_suppressed = m_startSuppressed;
	m_suppressed &= down;
	Evaluate(down, focused, capture);
	for(const auto& event : m_input.GetEvents())
	{
		if(event.type == InputEventType::Key || event.type == InputEventType::MouseButton)
		{
			if(event.action == InputAction::Repeat) continue;
			const auto index = event.type == InputEventType::Key ? Input::Index(static_cast<Key>(event.code))
				: Input::Index(static_cast<MouseButton>(event.code));
			if(index >= Input::ButtonCount) continue;
			down[index] = event.action == InputAction::Press;
			if(!down[index]) m_suppressed.reset(index);
		}
		else if(event.type == InputEventType::Focus)
		{
			focused = event.focused;
			if(!focused) { down.reset(); m_suppressed.reset(); }
		}
		else continue;
		Evaluate(down, focused, capture);
	}
	// Reconcile even when the application skipped an Update or changed cursor/focus state.
	m_suppressed &= m_input.m_frame.down;
	Evaluate(m_input.m_frame.down, m_input.m_frame.focused, capture);
	if(sameFrame)
		for(auto& entry : m_actions)
			entry.second.state.released |= entry.second.previousDown && entry.second.state.value == 0;
}

#include "dyf/Platform/Input.h"

using namespace dyf::Platform;

void Input::OnEvent(const InputEvent& event)
{
	if(event.type == InputEventType::Text &&
		(event.codepoint > 0x10FFFF || (event.codepoint >= 0xD800 && event.codepoint <= 0xDFFF))) return;
	m_pendingEvents.push_back(event);
	switch(event.type)
	{
	case InputEventType::Key:
		if(event.action != InputAction::Repeat)
			OnButton(Index(static_cast<Key>(event.code)), event.action == InputAction::Press);
		break;
	case InputEventType::MouseButton:
		OnButton(Index(static_cast<MouseButton>(event.code)), event.action == InputAction::Press);
		break;
	case InputEventType::Cursor: OnCursor(event.x, event.y); break;
	case InputEventType::Scroll:
		m_pending.scroll.x += event.x;
		m_pending.scroll.y += event.y;
		break;
	case InputEventType::Focus:
		m_pending.focused = event.focused;
		if(!event.focused) OnFocusLost();
		else { m_pending.delta = {}; m_hasCursorPosition = false; }
		break;
	case InputEventType::Text: break;
	}
}

void Input::OnButton(std::size_t index, bool down)
{
	if(index >= ButtonCount || m_pending.down[index] == down) return;
	m_pending.down[index] = down;
	(down ? m_pending.pressed : m_pending.released).set(index);
	if(!down) m_lastReleaseFrame[index] = m_frameNumber + 1;
}

void Input::OnCursor(double x, double y)
{
	if(m_hasCursorPosition)
	{
		m_pending.delta.x += x - m_pending.position.x;
		m_pending.delta.y += y - m_pending.position.y;
	}
	m_pending.position = {x, y};
	m_hasCursorPosition = true;
}

void Input::OnFocusLost()
{
	for(std::size_t index = 0; index < ButtonCount; ++index)
		if(m_pending.down[index]) m_lastReleaseFrame[index] = m_frameNumber + 1;
	m_pending.released |= m_pending.down;
	m_pending.down.reset();
	m_pending.delta = {};
	m_hasCursorPosition = false;
}

void Input::PublishFrame()
{
	m_frameStartDown = m_frame.down;
	m_frameStartFocused = m_frame.focused;
	m_frame = m_pending;
	m_events.swap(m_pendingEvents);
	m_pendingEvents.clear();
	m_text.clear();
	for(const auto& event : m_events)
		if(event.type == InputEventType::Text) m_text.push_back(static_cast<char32_t>(event.codepoint));
	++m_frameNumber;
	m_consumed.reset();
	m_pending.pressed.reset();
	m_pending.released.reset();
	m_pending.delta = {};
	m_pending.scroll = {};
}

bool Input::ConsumeKeyPress(Key key)
{
	const auto index = Index(key);
	if(!Test(m_frame.pressed, index) || m_consumed[index]) return false;
	m_consumed.set(index);
	return true;
}

#include "dyf/Platform/Input.h"

using namespace dyf::Platform;

void Input::OnButton(std::size_t index, bool down)
{
	if(index >= ButtonCount || m_pending.down[index] == down) return;
	m_pending.down[index] = down;
	(down ? m_pending.pressed : m_pending.released).set(index);
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
	m_pending.released |= m_pending.down;
	m_pending.down.reset();
	m_pending.delta = {};
	m_hasCursorPosition = false;
}

void Input::PublishFrame()
{
	m_frame = m_pending;
	m_events.swap(m_pendingEvents);
    m_pendingEvents.clear();
	m_pending.pressed.reset();
	m_pending.released.reset();
	m_pending.delta = {};
	m_pending.scroll = {};
}

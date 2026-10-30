#pragma once

#include "dyf/Math/Math.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace showcase::arena
{
// Scale the presentation by adding complete, separated formations. A cell's
// 64 actors retain the approved paths, timings and spacing at every load size.
class ArenaLayout
{
public:
    static constexpr uint32_t ActorsPerCell = 64;
    static constexpr uint32_t DefaultActors = 1024;
    static constexpr uint32_t MaxActors = 4096;
    static constexpr float CellPitch = 64;

    explicit ArenaLayout(uint32_t count) : m_count(std::clamp(count,12u,MaxActors))
    {
        m_cells = (m_count + ActorsPerCell - 1) / ActorsPerCell;
        m_columns = uint32_t(std::ceil(std::sqrt(double(m_cells))));
        m_rows = (m_cells + m_columns - 1) / m_columns;
    }

    [[nodiscard]] uint32_t ActorCount() const { return m_count; }
    [[nodiscard]] uint32_t PrototypeCount() const { return std::min(m_count,ActorsPerCell); }
    [[nodiscard]] uint32_t CellCount() const { return m_cells; }
    [[nodiscard]] uint32_t CellActorCount(uint32_t cell) const
    { return std::min(ActorsPerCell,m_count - cell * ActorsPerCell); }
    [[nodiscard]] dyf::Math::float3 Offset(uint32_t cell) const
    {
        return {(float(cell % m_columns) - float(m_columns - 1) * .5f) * CellPitch,
            (float(cell / m_columns) - float(m_rows - 1) * .5f) * CellPitch,0};
    }
    // Include the exterior ruins in the overview and camera clip range.
    [[nodiscard]] float WorldWidth() const { return float(m_columns - 1) * CellPitch + 74; }
    [[nodiscard]] float WorldHeight() const { return float(m_rows - 1) * CellPitch + 74; }
    [[nodiscard]] float ViewScale() const
    { return m_cells == 1 ? 1.f : std::max(WorldWidth(),WorldHeight()) / 37.f; }

private:
    uint32_t m_count, m_cells = 1, m_columns = 1, m_rows = 1;
};
}

#pragma once

#include "ArenaCombat.h"
#include "ArenaLayout.h"
#include "dyf/Scene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace showcase::arena
{
namespace arena_actor_detail
{
    using namespace dyf::Math;
    inline float4x4 Hidden() { return Translation({0,0,-1000}); }
    inline dyf::MaterialDesc Glow(float3 color, float emission)
    {
        dyf::MaterialDesc material;
        material.baseColor = {color.x,color.y,color.z,1};
        material.emissiveColor = color * emission;
        material.roughnessFactor = .4f;
        material.doubleSided = true;
        return material;
    }
    inline dyf::MeshData Arrow()
    {
        dyf::MeshData mesh;
        using dungeon_detail::Quad;
        using dungeon_detail::Triangle;
        constexpr float w = .025f, tail = -.56f, neck = .30f;
        Quad(mesh, {-w,tail,w}, {w,tail,w}, {w,neck,w}, {-w,neck,w}, {0,0,1});
        Quad(mesh, {-w,tail,-w}, {-w,neck,-w}, {w,neck,-w}, {w,tail,-w}, {0,0,-1});
        Quad(mesh, {-w,tail,-w}, {-w,tail,w}, {-w,neck,w}, {-w,neck,-w}, {-1,0,0});
        Quad(mesh, {w,tail,-w}, {w,neck,-w}, {w,neck,w}, {w,tail,w}, {1,0,0});
        const float3 tip{0,.68f,0};
        const float3 rim[] = {{-.15f,.25f,0}, {0,.25f,.085f}, {.15f,.25f,0}, {0,.25f,-.085f}};
        for(uint32_t i = 0; i < 4; ++i)
            Triangle(mesh, rim[i], rim[(i + 1) % 4], tip,
                {rim[i].x + rim[(i + 1) % 4].x,.1f,rim[i].z + rim[(i + 1) % 4].z});
        Quad(mesh, rim[3], rim[2], rim[1], rim[0], {0,-1,0});
        for(float side : {-1.f,1.f})
        {
            Quad(mesh, {0,tail,0}, {side*.14f,tail-.06f,0},
                {side*.12f,tail+.20f,0}, {0,tail+.30f,0}, {0,0,side});
            Quad(mesh, {0,tail,0}, {0,tail+.30f,0},
                {0,tail+.20f,side*.12f}, {0,tail-.06f,side*.14f}, {side,0,0});
        }
        return mesh;
    }
}

// Evaluate the authored closed group tracks at the display timestamp. Character
// roots, facing, animation phases and projectiles all use the same 16s clock.
class ArenaActors
{
    using float3 = dyf::Math::float3;
    using Handle = dyf::EntityHandle;
    struct Cast { Handle projectile; std::array<Handle,3> fragments; };

public:
    [[nodiscard]] static constexpr double LoopDuration() { return ArenaCombat::Duration; }

    ArenaActors(dyf::Scene& scene, const std::vector<ArenaObstacle>& obstacles, uint32_t count)
        : m_layout(count), m_combat(obstacles,m_layout.PrototypeCount()),
          m_casts(size_t(m_layout.CellCount()) * ArenaCombat::EffectCount)
    {
        CreateEffects(scene);
        SetTime(0);
    }

    void SetTime(double seconds)
    {
        const double time = std::isfinite(seconds) ? std::max(0.0,seconds) : 0;
        m_loopTime = std::fmod(time,LoopDuration());
        m_combat.Sample(m_loopTime,m_sample);
        m_arrows = m_spells = m_moving = 0;
        for(uint32_t cell = 0; cell < m_layout.CellCount(); ++cell)
        {
            const auto offset = m_layout.Offset(cell);
            const auto actors = m_layout.CellActorCount(cell);
            for(uint32_t i = 0; i < actors; ++i) m_moving += uint32_t(m_sample.enemies[i].moving);
            for(uint32_t i = 0; i < ArenaCombat::EffectCount; ++i)
            {
                auto effect = m_sample.effects[i];
                if(effect.owner >= actors) effect.active = false;
                effect.position = effect.position + offset;
                StageEffect(cell * ArenaCombat::EffectCount + i,effect);
                if(effect.active && !effect.impact)
                {
                    if(i < ArenaCombat::ArrowCount) ++m_arrows;
                    else if(i < ArenaCombat::ArrowCount + ArenaCombat::SpellCount) ++m_spells;
                }
            }
        }
    }

    [[nodiscard]] ArenaActorPose PlayerPose() const { return m_sample.hero; }
    [[nodiscard]] const std::vector<ArenaActorPose>& EnemyPoses() const { return m_sample.enemies; }
    [[nodiscard]] float3 PlayerPosition() const { return m_sample.hero.position; }
    [[nodiscard]] uint32_t ActiveArrowCount() const { return m_arrows; }
    [[nodiscard]] uint32_t ActiveSpellCount() const { return m_spells; }
    [[nodiscard]] uint32_t MovingEnemyCount() const { return m_moving; }
    [[nodiscard]] uint32_t AliveEnemyCount() const { return m_layout.ActorCount(); }
    [[nodiscard]] const ArenaLayout& Layout() const { return m_layout; }
    [[nodiscard]] double LoopTime() const { return m_loopTime; }
    [[nodiscard]] const char* PatternName() const { return m_combat.GroupName(m_loopTime); }

private:
    void CreateEffects(dyf::Scene& scene)
    {
        using namespace arena_actor_detail;
        const auto arrow = Arrow(), crystal = dungeon_detail::Crystal();
        std::array<Handle,4> projectileSource{}, fragmentSource{};
        const std::array<float3,4> colors = {{{1,.62f,.16f},{.8f,.2f,1},{.12f,.75f,1},{1,.2f,.06f}}};
        for(uint32_t i = 0; i < m_casts.size(); ++i)
        {
            const uint32_t local = i % ArenaCombat::EffectCount;
            const bool isArrow = local < ArenaCombat::ArrowCount;
            const bool spell = local >= ArenaCombat::ArrowCount && local < ArenaCombat::ArrowCount + ArenaCombat::SpellCount;
            const uint32_t palette = isArrow ? 0u : spell ? 1u : 2u + (i % 2);
            const auto add = [&](Handle& source, const dyf::MeshData& mesh, float emission) {
                auto entity = source ? scene.AddInstance(source,Hidden()) : scene.Add(mesh,Glow(colors[palette],emission),Hidden());
                if(!entity) throw std::runtime_error("Cannot create Arena combat effects.");
                if(!source) source = entity;
                entity.SetLighting({false,false});
                return entity;
            };
            m_casts[i].projectile = add(projectileSource[palette],isArrow ? arrow : crystal,spell ? 3.5f : 1.3f);
            for(auto& fragment : m_casts[i].fragments) fragment = add(fragmentSource[palette],crystal,2.5f);
        }
    }

    void StageEffect(uint32_t index, const ArenaEffectPose& effect)
    {
        using namespace dyf::Math;
        using arena_actor_detail::Hidden;
        auto& cast = m_casts[index];
        if(!effect.active)
        {
            cast.projectile.SetTransform(Hidden());
            for(auto& fragment : cast.fragments) fragment.SetTransform(Hidden());
            return;
        }
        if(!effect.impact)
        {
            const auto direction = effect.direction;
            const float pitch = std::atan2(direction.z,std::sqrt(direction.x * direction.x + direction.y * direction.y));
            const auto facing = RotationZ(combat_detail::Yaw(direction)) * RotationX(pitch);
            const bool spell = index % ArenaCombat::EffectCount >= ArenaCombat::ArrowCount;
            cast.projectile.SetTransform(Translation(effect.position) * facing * Scaling(spell ? float3{.25f,.45f,.25f} : float3{1,1,1}));
            cast.fragments[0].SetTransform(Translation(effect.position - direction * .65f) * facing * Scaling({.04f,.8f,.04f}));
            cast.fragments[1].SetTransform(Hidden());
            cast.fragments[2].SetTransform(Hidden());
            return;
        }
        cast.projectile.SetTransform(Hidden());
        for(uint32_t j = 0; j < cast.fragments.size(); ++j)
        {
            const float angle = float(j) * 2 * combat_detail::pi / 3 + float(index) * .17f;
            const float speed = 2.5f + float(j) * .4f;
            const float age = effect.age;
            auto position = effect.position + float3{std::cos(angle) * speed * age,std::sin(angle) * speed * age,age * 2 - age * age * 5};
            position.z = std::max(.08f,position.z);
            const float size = .18f * std::max(.02f,1 - age / .32f);
            cast.fragments[j].SetTransform(Translation(position) * RotationZ(angle + age * 8) * Scaling({size,size,size * 2}));
        }
    }

    ArenaLayout m_layout;
    ArenaCombat m_combat;
    ArenaCombatFrame m_sample;
    std::vector<Cast> m_casts;
    double m_loopTime = 0;
    uint32_t m_arrows = 0, m_spells = 0, m_moving = 0;
};
}

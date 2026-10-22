#pragma once

#include "DungeonScene.h"
#include "dyf/Scene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace showcase::arena
{
    // Scene poses only: XY is the floor, Z is height and yaw zero faces +Y.
    struct ArenaActorPose
    {
        dyf::Math::float3 position = {0, 0, 0};
        float yaw = 0;
        bool alive = true, moving = false, attacking = false;
        uint32_t kind = 0;
    };

    namespace arena_actor_detail
    {
        using namespace dyf::Math;
        constexpr double pi = 3.14159265358979323846;

        inline uint32_t Hash(uint32_t x)
        {
            x ^= x >> 16; x *= 0x7feb352du;
            x ^= x >> 15; x *= 0x846ca68bu;
            return x ^ (x >> 16);
        }
        inline float Unit(uint32_t x) { return float(Hash(x) & 65535u) / 65535.0f; }
        inline float LengthXY(float3 p) { return std::sqrt(p.x * p.x + p.y * p.y); }
        inline float3 Direction(float3 p)
        {
            p.z = 0;
            return NormalizeOr(p, {0, 1, 0});
        }
        inline float Yaw(float3 direction) { return std::atan2(-direction.x, direction.y); }
        inline double Cycle(double seconds, double period)
        {
            return seconds - std::floor(seconds / period) * period;
        }
        inline float4x4 Hidden() { return Translation({0, 0, -1000}); }
        inline float4x4 Transform(float3 position, float yaw, float3 scale)
        {
            return Translation(position) * RotationZ(yaw) * Scaling(scale);
        }

        inline dyf::MeshData Crystal()
        {
            const float3 points[] = {{0, 0, .5f}, {0, 0, -.5f},
                {-.5f, 0, 0}, {0, -.5f, 0}, {.5f, 0, 0}, {0, .5f, 0}};
            const uint32_t faces[][3] = {{0, 2, 3}, {0, 3, 4}, {0, 4, 5}, {0, 5, 2},
                {1, 3, 2}, {1, 4, 3}, {1, 5, 4}, {1, 2, 5}};
            dyf::MeshData result;
            for(const auto& face : faces)
            {
                const auto normal = Normalize(Cross(points[face[1]] - points[face[0]],
                    points[face[2]] - points[face[0]]));
                const auto tangent = Normalize(points[face[1]] - points[face[0]]);
                for(uint32_t j = 0; j < 3; ++j)
                {
                    dyf::Vertex vertex;
                    vertex.position = points[face[j]];
                    vertex.normal = normal;
                    vertex.tangent = {tangent.x, tangent.y, tangent.z, 1};
                    vertex.uv = {float(j == 1), float(j == 2)};
                    result.indices.push_back(static_cast<uint32_t>(result.vertices.size()));
                    result.vertices.push_back(vertex);
                }
            }
            return result;
        }

        inline dyf::MaterialDesc Glow(float3 color, float emission)
        {
            dyf::MaterialDesc result;
            result.baseColor = {color.x, color.y, color.z, 1};
            result.emissiveColor = color * emission;
            result.roughnessFactor = .4f;
            return result;
        }
    }

    // An absolute-time presentation scene. It has no input, combat rules or
    // persistent simulation state: returning to a timestamp restores the staging.
    class ArenaActors
    {
        using float3 = dyf::Math::float3;
        using Handle = dyf::EntityHandle;
        static constexpr uint32_t castCount = 24, fragmentsPerCast = 4;

        struct Cast
        {
            Handle projectile;
            std::array<Handle, fragmentsPerCast> fragments;
        };

    public:
        ArenaActors(dyf::Scene& scene, const std::vector<ArenaObstacle>& obstacles, uint32_t count)
            : m_obstacles(obstacles)
        {
            m_enemies.resize(std::clamp(count, 12u, 512u));
            for(uint32_t i = 0; i < m_enemies.size(); ++i)
            {
                m_enemies[i].kind = Kind(i);
                if(Kind(i) == 2) m_casterSlots.push_back(i);
            }
            CreateEffects(scene);
            SetTime(0);
        }

        void SetTime(double seconds)
        {
            using namespace arena_actor_detail;
            const double time = std::isfinite(seconds) ? std::max(0.0, seconds) : 0;
            const auto heroPosition = HeroPosition(time);
            for(uint32_t i = 0; i < m_enemies.size(); ++i)
                m_enemies[i] = EnemyPose(i, time, heroPosition);

            float3 look = {0, 1, 0};
            float nearest = 1000;
            for(const auto& enemy : m_enemies)
            {
                const auto delta = enemy.position - heroPosition;
                const float distance = LengthXY(delta);
                if(distance < nearest) { nearest = distance; look = Direction(delta); }
            }
            m_hero = {heroPosition, Yaw(look), true, true, Cycle(time, 1.5) < .55, 0};
            StageEffects(time);
        }

        [[nodiscard]] ArenaActorPose PlayerPose() const { return m_hero; }
        [[nodiscard]] const std::vector<ArenaActorPose>& EnemyPoses() const { return m_enemies; }
        [[nodiscard]] float3 PlayerPosition() const { return m_hero.position; }

    private:
        static uint32_t Kind(uint32_t i) { return i % 5 < 3 ? 0 : i % 5 == 3 ? 1 : 2; }

        void KeepOnFloor(float3& position, float radius) const
        {
            using namespace arena_actor_detail;
            for(int pass = 0; pass < 2; ++pass)
                for(const auto& obstacle : m_obstacles)
                {
                    const auto away = position - obstacle.center;
                    const float distance = LengthXY(away), clearance = radius + obstacle.radius;
                    if(distance < clearance)
                    {
                        const auto correction = Direction(away) * clearance;
                        position.x = obstacle.center.x + correction.x;
                        position.y = obstacle.center.y + correction.y;
                    }
                }
            position.x = std::clamp(position.x, -13.7f, 13.7f);
            position.y = std::clamp(position.y, -13.7f, 13.7f);
            position.z = 0;
        }

        float3 HeroPosition(double time) const
        {
            const double angle = time * .13 - arena_actor_detail::pi * .5;
            float3 position = {float(std::cos(angle) * 4.8), float(std::sin(angle) * 4.8), 0};
            KeepOnFloor(position, .65f);
            return position;
        }

        ArenaActorPose EnemyPose(uint32_t index, double time, float3 hero) const
        {
            using namespace arena_actor_detail;
            const uint32_t band = index % 3, slot = index / 3;
            const uint32_t count = (static_cast<uint32_t>(m_enemies.size()) + 2 - band) / 3;
            const bool standing = band == 1 && index % 2 == 0;
            const double phase = Unit(index * 101 + 7) * 2 * pi;
            const double baseAngle = 2 * pi * (double(slot) / double(count) + band * .17);
            const double patrol = standing ? 0 : band == 2 ? time * .035 : std::sin(time * .24 + phase) * .11;
            const double angle = baseAngle + patrol;
            const float radius = (band == 0 ? 4.7f : band == 1 ? 8.2f : 11.7f) +
                float(std::sin(time * .36 + phase)) * (standing ? 0 : band == 0 ? .65f : .35f);
            float3 position = {float(std::cos(angle)) * radius, float(std::sin(angle)) * radius, 0};
            // Leave room around the knight so the central silhouette stays readable.
            const auto away = position - hero;
            const float distance = LengthXY(away);
            if(distance < 1.65f) position = hero + Direction(away) * 1.65f;
            KeepOnFloor(position, Kind(index) == 1 ? .75f : .5f);
            const auto facing = band == 2 ? float3{float(-std::sin(angle)), float(std::cos(angle)), 0} :
                Direction(hero - position);
            const bool attacking = !standing && band != 2 && Cycle(time + phase, 2.8 + Kind(index) * .3) < .68;
            return {position, Yaw(facing), true, !standing && !attacking, attacking, Kind(index)};
        }

        void CreateEffects(dyf::Scene& scene)
        {
            using namespace arena_actor_detail;
            const auto crystal = Crystal();
            std::array<Handle, 2> projectileSource, fragmentSource;
            for(uint32_t i = 0; i < m_casts.size(); ++i)
            {
                const uint32_t palette = i % 2;
                const auto color = palette ? float3{1, .26f, .055f} : float3{.12f, .67f, 1};
                auto add = [&](Handle& source, float emission) {
                    auto entity = source ? scene.AddInstance(source, Hidden()) :
                        scene.Add(crystal, Glow(color, emission), Hidden());
                    if(!entity) throw std::runtime_error("Cannot create Arena scene effects.");
                    if(!source) source = entity;
                    entity.SetLighting({false, false});
                    return entity;
                };
                m_casts[i].projectile = add(projectileSource[palette], 4);
                for(auto& fragment : m_casts[i].fragments)
                    fragment = add(fragmentSource[palette], 2.5f);
            }
        }

        void StageEffects(double time)
        {
            using namespace arena_actor_detail;
            for(uint32_t i = 0; i < m_casts.size(); ++i)
            {
                auto& cast = m_casts[i];
                const bool hostile = i % 2 != 0;
                const double period = hostile ? 7.8 : 6;
                const double offset = double(i / 2) * period / 12;
                const double age = Cycle(time + offset, period);
                const double launchTime = time - age;
                const auto hero = HeroPosition(launchTime);
                const uint32_t actorIndex = hostile ? m_casterSlots[(i / 2) % m_casterSlots.size()] :
                    (i * 7 + 1) % static_cast<uint32_t>(m_enemies.size());
                const auto actor = EnemyPose(actorIndex, launchTime, hero);
                float3 start = hostile ? actor.position : hero;
                float3 end = hostile ? hero : actor.position;
                start.z = hostile ? 1.35f : 1.05f;
                end.z = .85f;
                const auto direction = Direction(end - start);
                start = start + direction * .6f;
                const float duration = std::clamp(LengthXY(end - start) / (hostile ? 7.0f : 12.0f), .28f, 1.35f);
                if(age < duration)
                {
                    const float fraction = float(age) / duration;
                    auto position = start * (1 - fraction) + end * fraction;
                    position.z += std::sin(fraction * float(pi)) * .35f;
                    cast.projectile.SetTransform(Transform(position, Yaw(direction),
                        hostile ? float3{.2f, .35f, .2f} : float3{.13f, .5f, .13f}));
                    if(hostile && age < .45) m_enemies[actorIndex].attacking = true;
                }
                else cast.projectile.SetTransform(Hidden());

                const float burstAge = float(age) - duration;
                for(uint32_t j = 0; j < cast.fragments.size(); ++j)
                {
                    if(burstAge < 0 || burstAge > .48f)
                    {
                        cast.fragments[j].SetTransform(Hidden());
                        continue;
                    }
                    const float angle = float(Unit(i * 137 + j * 41) * 2 * pi);
                    const float speed = 1.5f + Unit(i + j * 13) * 1.8f;
                    auto position = end + float3{std::cos(angle) * speed * burstAge,
                        std::sin(angle) * speed * burstAge, burstAge * 2 - burstAge * burstAge * 4};
                    position.z = std::max(.08f, position.z);
                    const float size = .16f * std::max(.04f, 1 - burstAge / .48f);
                    cast.fragments[j].SetTransform(Transform(position, angle + burstAge * 7,
                        {size, size, size * 2}));
                }
            }
        }

        std::vector<ArenaObstacle> m_obstacles;
        std::vector<ArenaActorPose> m_enemies;
        std::vector<uint32_t> m_casterSlots;
        ArenaActorPose m_hero;
        std::array<Cast, castCount> m_casts;
    };
}

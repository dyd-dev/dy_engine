#pragma once

#include "DungeonScene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace showcase::arena
{
struct ArenaActorPose
{
    dyf::Math::float3 position = {0,0,0};
    float yaw = 0;
    bool alive = true, moving = false, attacking = false;
    uint32_t kind = 0;
    float animationPhase = 0;
    bool hit = false, dying = false, dodging = false, walking = false;
};

struct ArenaEffectPose
{
    dyf::Math::float3 position = {}, direction = {0,1,0};
    float age = 0;
    uint32_t serial = 0;
    uint32_t owner = UINT32_MAX;
    bool active = false, impact = false;
};

struct ArenaCombatFrame
{
    ArenaActorPose hero;
    std::vector<ArenaActorPose> enemies;
    std::vector<ArenaEffectPose> effects;
    uint32_t alive = 0, moving = 0, arrows = 0, spells = 0;
};

namespace combat_detail
{
    using namespace dyf::Math;
    constexpr float pi = 3.14159265358979323846f;
    inline double Loop(double time, double period) { return time - std::floor(time / period) * period; }
    inline float Phase(double time) { return float(Loop(time,1)); }
    inline float Smooth(float t) { t = std::clamp(t,0.f,1.f); return t * t * (3 - 2 * t); }
    inline float3 Direction(float3 v) { v.z = 0; return NormalizeOr(v,{0,1,0}); }
    inline float Yaw(float3 v) { return std::atan2(-v.x,v.y); }
    inline float3 Rotate(float3 p, float angle)
    {
        const float c = std::cos(angle), s = std::sin(angle);
        return {p.x * c - p.y * s,p.x * s + p.y * c,p.z};
    }
    inline float Face(float from, float to, float weight)
    { return from + std::remainder(to - from,2 * pi) * weight; }

    // Accelerate/decelerate only at the ends, leaving clearance for the next
    // actor rather than spending half a path slowly leaving its attack point.
    inline float Travel(double elapsed, double duration, double ramp = .18)
    {
        elapsed = std::clamp(elapsed,0.0,duration);
        double distance;
        if(elapsed < ramp) distance = elapsed * elapsed / (2 * ramp);
        else if(elapsed > duration - ramp) distance = duration - ramp -
            (duration - elapsed) * (duration - elapsed) / (2 * ramp);
        else distance = elapsed - ramp * .5;
        return float(distance / (duration - ramp));
    }

    struct TrackSample { float3 position{}, tangent{1,0,0}; };

    // A cyclic Catmull-Rom track, sampled by arc distance. Unit spacing follows
    // actual track length, including its long outside return leg.
    class ClosedTrack
    {
    public:
        explicit ClosedTrack(std::array<float3,8> points) : m_points(points)
        {
            auto previous = Evaluate(0).position;
            for(uint32_t i = 1; i < m_lengths.size(); ++i)
            {
                const auto current = Evaluate(double(i) / (m_lengths.size() - 1)).position;
                m_lengths[i] = m_lengths[i - 1] + Length(current - previous);
                previous = current;
            }
        }
        TrackSample Sample(double distance) const
        {
            const float target = float(Loop(distance,1)) * m_lengths.back();
            const auto found = std::lower_bound(m_lengths.begin() + 1,m_lengths.end(),target);
            const uint32_t end = uint32_t(found - m_lengths.begin());
            const float span = m_lengths[end] - m_lengths[end - 1];
            const float fraction = span > 0 ? (target - m_lengths[end - 1]) / span : 0;
            return Evaluate((double(end - 1) + fraction) / (m_lengths.size() - 1));
        }
    private:
        TrackSample Evaluate(double phase) const
        {
            const double coordinate = Loop(phase,1) * m_points.size();
            const auto i = uint32_t(coordinate);
            const float t = float(coordinate - i), t2 = t * t, t3 = t2 * t;
            const auto& a = m_points[(i + 7) % 8];
            const auto& b = m_points[i % 8];
            const auto& c = m_points[(i + 1) % 8];
            const auto& d = m_points[(i + 2) % 8];
            const auto first = c - a;
            const auto second = a * 2 - b * 5 + c * 4 - d;
            const auto third = a * -1 + b * 3 - c * 3 + d;
            return {(b * 2 + first * t + second * t2 + third * t3) * .5f,
                Direction((first + second * (2 * t) + third * (3 * t2)) * .5f)};
        }
        std::array<float3,8> m_points;
        std::array<float,257> m_lengths{};
    };
}

// Authored group choreography, evaluated directly from time. Every path is
// closed, every movement/animation clock divides 16 seconds, and every shot
// retains its launch pose across the loop boundary. There is no simulation
// warm-up, random steering, respawn, reset frame or screen transition.
class ArenaCombat
{
    using float3 = dyf::Math::float3;
    using Track = combat_detail::ClosedTrack;
    struct Group
    {
        const char* name;
        float rotation, direction;
        double encounter, firstContact;
        bool heavy;
    };
    struct Member { uint32_t group = 0, ordinal = 0, count = 0; };
    struct Salvo { uint32_t owner = 0, burst = 0, ray = 0, effect = 0; bool spell = false; };
    struct RangedGroup { const char* name; double shotOffset; bool mirrored; };

public:
    static constexpr double Duration = 16;
    static constexpr uint32_t ArrowCount = 192, SpellCount = 32, SparkCount = 64;
    static constexpr uint32_t EffectCount = ArrowCount + SpellCount + SparkCount;

    ArenaCombat(const std::vector<ArenaObstacle>& obstacles, uint32_t count)
        : m_obstacles(obstacles),
          m_attack({{{0,6.2f,0},{2.8f,6.8f,0},{4.4f,8.8f,0},{4.1f,11.6f,0},
              {0,12,0},{-4.1f,11.6f,0},{-4.4f,8.8f,0},{-2.8f,6.8f,0}}}),
          m_guard({{{0,8.2f,0},{1.5f,8.4f,0},{2.1f,9,0},{1.7f,9.8f,0},
              {0,10,0},{-1.7f,9.8f,0},{-2.1f,9,0},{-1.5f,8.4f,0}}}),
          m_archers({{{-12.5f,9.5f,0},{-12,8.7f,0},{-10.6f,8.7f,0},{-10.1f,9.5f,0},
              {-10.1f,11.9f,0},{-10.6f,12.7f,0},{-12,12.7f,0},{-12.5f,11.9f,0}}})
    {
        count = std::clamp(count,12u,512u);
        constexpr uint32_t weights[] = {3,3,3,3,2,2};
        uint32_t prefix = 0, arrow = 0, spell = ArrowCount;
        for(uint32_t group = 0; group < 6; ++group)
        {
            const uint32_t begin = count * prefix / 16;
            prefix += weights[group];
            const uint32_t end = count * prefix / 16;
            m_groupCounts[group] = end - begin;
            for(uint32_t i = begin; i < end; ++i)
            {
                m_members.push_back({group,i - begin,end - begin});
                if(group < 4) continue;
                const bool magic = (i - begin) % 4 == 3;
                for(uint32_t burst = 0; burst < (magic ? 2u : 3u); ++burst)
                    for(uint32_t ray = 0; ray < 3; ++ray)
                    {
                        if(magic && spell < ArrowCount + SpellCount) m_salvos.push_back({i,burst,ray,spell++,true});
                        else if(!magic && arrow < ArrowCount) m_salvos.push_back({i,burst,ray,arrow++,false});
                    }
            }
        }
    }

    void Sample(double seconds, ArenaCombatFrame& out) const
    {
        using namespace combat_detail;
        const double time = Loop(seconds,Duration);
        out.hero = HeroPose(time);
        out.enemies.resize(m_members.size());
        out.effects.assign(EffectCount,ArenaEffectPose{});
        out.alive = uint32_t(m_members.size());
        out.moving = out.arrows = out.spells = 0;
        for(uint32_t i = 0; i < m_members.size(); ++i)
        {
            out.enemies[i] = MonsterPose(i,time);
            out.moving += uint32_t(out.enemies[i].moving);
            const auto& member = m_members[i];
            if(member.group >= 4 || member.ordinal >= FrontCount(member)) continue;
            const double contact = Contact(member);
            for(uint32_t lap = 0; lap < 2; ++lap)
            {
                const double start = Loop(contact,8) + lap * 8;
                if(!Engages(member,start)) continue;
                const float age = float(Loop(time - start - .44,Duration));
                if(age >= .32f) continue;
                auto& spark = out.effects[ArrowCount + SpellCount + i % SparkCount];
                spark.position = MonsterPose(i,Loop(start + .44,Duration)).position + float3{0,0,.9f};
                spark.age = age; spark.serial = i + 1; spark.owner = i; spark.active = spark.impact = true;
            }
        }
        for(const auto& salvo : m_salvos) StageSalvo(salvo,time,out);
    }

    [[nodiscard]] const char* GroupName(double time) const { return Groups()[uint32_t(combat_detail::Loop(time,Duration) / 4)].name; }

private:
    static const std::array<Group,4>& Groups()
    {
        // 0..2.4s of each four-second encounter: two front units attack in turn.
        // The knight travels to the next sector during 2.4..3.8s. Rear units
        // keep their own inner patrol; the outside lane is the return route.
        static const std::array<Group,4> groups = {{
            {"G1 / West flank",combat_detail::pi * .5f,1,0,.45,false},
            {"G2 / North charge",0,1,4,.25,true},
            {"G3 / East pincer",-combat_detail::pi * .5f,-1,8,.45,false},
            {"G4 / South counter",combat_detail::pi,-1,12,.20,true}
        }};
        return groups;
    }
    static uint32_t FrontCount(const Member& member) { return (member.count + 1) / 2; }
    static const RangedGroup& RangedPlan(uint32_t group)
    {
        // G5 moves for .25..1.85s and fires for 2.20..2.42s of each 4s beat.
        // G6 answers two seconds later from the opposite corner. Both wings
        // finish four prescribed moves and return to their original spacing.
        static const std::array<RangedGroup,2> groups = {{
            {"G5 / NW crossfire",0,false},{"G6 / SE reply",2,true}
        }};
        return groups[group - 4];
    }
    static double Contact(const Member& member)
    {
        const auto& group = Groups()[member.group];
        return group.encounter + group.firstContact - double(member.ordinal) * 8 / FrontCount(member);
    }
    static bool Engages(const Member& member, double start)
    {
        const auto& group = Groups()[member.group];
        const double local = start - group.encounter;
        return local >= .1 && local <= 2.15;
    }

    ArenaActorPose HeroPose(double seconds) const
    {
        using namespace combat_detail;
        const double time = Loop(seconds,Duration);
        const uint32_t groupIndex = uint32_t(time / 4);
        const auto& group = Groups()[groupIndex];
        const double local = time - group.encounter;
        const auto radial = Rotate({0,1,0},group.rotation);
        ArenaActorPose pose;
        pose.position = radial * 4.2f;
        pose.yaw = Yaw(radial);
        pose.animationPhase = Phase(time);
        if(local >= 2.4 && local < 3.8)
        {
            const float progress = Travel(local - 2.4,1.4);
            const float angle = group.rotation - progress * pi * .5f;
            const auto current = Rotate({0,1,0},angle);
            pose.position = current * 4.2f;
            const auto travelDirection = Rotate({1,0,0},angle);
            const float ready = Smooth(float((local - 3.5) / .3));
            pose.yaw = Face(Yaw(travelDirection),Yaw(Rotate({0,1,0},group.rotation - pi * .5f)),ready);
            pose.moving = true;
            pose.animationPhase = Phase(time * 2.5);
        }
        else if(local >= 3.8)
        {
            const auto next = Rotate({0,1,0},group.rotation - pi * .5f);
            pose.position = next * 4.2f;
            pose.yaw = Yaw(next);
        }
        else
        {
            const uint32_t front = (m_groupCounts[groupIndex] + 1) / 2;
            for(uint32_t turn = 0; turn < 2; ++turn)
            {
                const double contact = group.firstContact + (turn ? 8.0 / front : 0);
                const double age = local - contact;
                if(age >= .08 && age < .30)
                {
                    const float phase = float((age - .08) / .22);
                    pose.position = pose.position - radial * (std::sin(phase * pi) * .35f);
                    pose.dodging = true;
                    pose.animationPhase = phase;
                }
                else if(age >= .30 && age < .60)
                { pose.attacking = true; pose.animationPhase = float((age - .30) / .30); }
            }
        }
        return pose;
    }

    ArenaActorPose MonsterPose(uint32_t index, double seconds) const
    {
        using namespace combat_detail;
        const double time = Loop(seconds,Duration);
        const auto& member = m_members[index];
        if(member.group >= 4) return RangedPose(member,time);
        const auto& group = Groups()[member.group];
        const bool front = member.ordinal < FrontCount(member);
        const uint32_t slot = front ? member.ordinal : member.ordinal - FrontCount(member);
        const uint32_t count = front ? FrontCount(member) : member.count - FrontCount(member);
        ArenaActorPose pose;
        pose.kind = front && group.heavy ? 1u : 0u;
        pose.moving = true;
        pose.walking = !front;
        pose.animationPhase = Phase(time * (front ? 2 : .75) + double(slot) / count);
        double phase = 0;
        TrackSample sample;
        if(front)
        {
            phase = Loop(time - Contact(member),8);
            sample = m_attack.Sample(group.direction * Travel(phase - .70,7.30));
            pose.moving = phase >= .70;
        }
        else sample = m_guard.Sample(group.direction * (time / Duration + double(slot) / count));
        pose.position = Rotate(sample.position,group.rotation);
        const auto tangent = Rotate(sample.tangent * group.direction,group.rotation);
        pose.yaw = Yaw(tangent);
        if(front)
        {
            const float approach = Smooth(float((phase - 7.65) / .35));
            const float departure = 1 - Smooth(float((phase - .70) / .35));
            const float facing = phase < .70 ? 1 : std::max(approach,departure);
            pose.yaw = Face(pose.yaw,Yaw(Direction(HeroPose(time).position - pose.position)),facing);
            if(!pose.moving) pose.animationPhase = Phase(time);
            const double start = Loop(time - phase,Duration);
            if(Engages(member,start))
            {
                const auto radial = Rotate({0,1,0},group.rotation);
                if(phase < .40)
                {
                    pose.attacking = true;
                    pose.animationPhase = float(phase / .40);
                    pose.position = pose.position - radial * (std::sin(float(phase / .40) * pi) * (group.heavy ? .35f : .15f));
                }
                else if(phase >= .44 && phase < .66)
                {
                    pose.hit = true;
                    pose.animationPhase = float((phase - .44) / .22);
                    pose.position = pose.position + radial * (std::sin(pose.animationPhase * pi) * .25f);
                }
            }
        }
        return pose;
    }

    ArenaActorPose RangedPose(const Member& member, double seconds) const
    {
        using namespace combat_detail;
        const auto& plan = RangedPlan(member.group);
        const double time = Loop(seconds,Duration), offset = plan.shotOffset;
        const double clock = Loop(time - offset,Duration), beat = Loop(clock,4);
        const float progress = Travel(beat - .25,1.60);
        const double distance = (std::floor(clock / 4) + progress) / 4 + double(member.ordinal) / member.count;
        auto sample = m_archers.Sample(distance);
        if(plan.mirrored) { sample.position = sample.position * -1; sample.tangent = sample.tangent * -1; }
        ArenaActorPose pose;
        pose.position = sample.position;
        pose.kind = 2;
        pose.moving = beat >= .25 && beat < 1.85;
        pose.walking = pose.moving;
        pose.animationPhase = Phase(time * (pose.moving ? 1.25 : .5) + double(member.ordinal) / member.count);
        const float aiming = Yaw(Direction(HeroPose(time).position - pose.position));
        pose.yaw = aiming;
        if(pose.moving) pose.yaw = Face(aiming,Yaw(sample.tangent),std::sin(progress * pi) * std::sin(progress * pi));
        const double shotClock = Loop(time - offset - member.ordinal * .04,4);
        if(shotClock >= 2.05 && shotClock < 2.70)
        { pose.attacking = true; pose.animationPhase = float((shotClock - 2.05) / .65); }
        return pose;
    }

    float FlightRange(float3 start, float3 direction, float requested) const
    {
        float range = requested;
        if(std::abs(direction.x) > .0001f) range = std::min(range,((direction.x > 0 ? 16.f : -16.f) - start.x) / direction.x);
        if(std::abs(direction.y) > .0001f) range = std::min(range,((direction.y > 0 ? 16.f : -16.f) - start.y) / direction.y);
        for(const auto& obstacle : m_obstacles)
        {
            const auto delta = obstacle.center - start;
            const float projected = delta.x * direction.x + delta.y * direction.y;
            const float perpendicular = delta.x * delta.x + delta.y * delta.y - projected * projected;
            const float radius = obstacle.radius + .08f;
            if(projected <= 0 || perpendicular >= radius * radius) continue;
            const float hit = projected - std::sqrt(std::max(0.f,radius * radius - perpendicular));
            if(hit > .3f) range = std::min(range,hit);
        }
        return std::max(.3f,range);
    }

    void StageSalvo(const Salvo& salvo, double time, ArenaCombatFrame& out) const
    {
        using namespace combat_detail;
        const auto& member = m_members[salvo.owner];
        const double offset = RangedPlan(member.group).shotOffset + member.ordinal * .04;
        const double age = Loop(time - offset - 2.20 - salvo.burst * .11,4);
        auto& effect = out.effects[salvo.effect];
        effect.serial = salvo.effect + 1;
        effect.owner = salvo.owner;
        const float speed = salvo.spell ? 11.f : 21.f;
        // The authored flight range is at most 22; no flight or impact can
        // remain active after this bound, regardless of obstacle intersections.
        if(age >= double(22.f / speed) + .32) return;
        const double launch = Loop(time - age,Duration);
        auto start = RangedPose(member,launch).position;
        const auto target = HeroPose(launch + .25).position;
        const auto direction = Rotate(Direction(target - start),(float(salvo.ray) - 1) * (salvo.spell ? .15f : .10f));
        start = start + direction * .65f;
        start.z = 1.3f;
        const float range = FlightRange(start,direction,std::clamp(dyf::Math::Length(target - start) + 4,8.f,22.f));
        auto end = start + direction * range;
        end.z = .18f;
        const float duration = range / speed, arc = salvo.spell ? .6f : .28f;
        if(age < duration)
        {
            const float fraction = float(age) / duration;
            effect.position = start * (1 - fraction) + end * fraction;
            effect.position.z += std::sin(fraction * pi) * arc;
            effect.direction = dyf::Math::NormalizeOr(float3{direction.x * range,direction.y * range,
                end.z - start.z + std::cos(fraction * pi) * pi * arc},{0,1,0});
            effect.active = true;
            if(salvo.spell) ++out.spells; else ++out.arrows;
        }
        else if(age < duration + .32)
        { effect.position = end; effect.active = effect.impact = true; effect.age = float(age) - duration; }
    }

    const std::vector<ArenaObstacle>& m_obstacles;
    Track m_attack, m_guard, m_archers;
    std::vector<Member> m_members;
    std::vector<Salvo> m_salvos;
    std::array<uint32_t,6> m_groupCounts{};
};
}

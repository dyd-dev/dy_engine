#pragma once

#include "dyf/Canvas.h"
#include "dyf/Renderer.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>

namespace showcase
{
// Interactive presentation capture. No files, GPU waits or extra processes.
// The caller freezes the scene/camera and excludes only the selected optimisation
// from conditions. Both runs keep the same output size and rendering quality.
class PerformanceComparison
{
public:
    bool Begin(bool candidate, const std::string& conditions, const char* label)
    {
        if(candidate && (!m_hasBaseline || conditions != m_conditions))
        {
            std::fputs("Comparison: collect F5 baseline at the same view/settings first.\n", stderr);
            return false;
        }
        m_candidate = candidate;
        if(!candidate) { m_conditions = conditions; m_hasBaseline = false; }
        m_hasCandidate = false;
        m_label = label;
        m_current = {};
        m_elapsed = 0;
        m_frames = 0;
        m_measuring = false;
        m_running = true;
        m_lastSequence = 0;
        std::fprintf(stderr, "Comparison %c: %s. Frozen view; warm-up >=1 s/10 frames, capture >=3 s/30 frames.\n",
            candidate ? 'B' : 'A', label);
        return true;
    }

    bool IsRunning() const { return m_running; }
    bool HasBaseline() const { return m_hasBaseline; }
    void Cancel(const char* reason)
    {
        if(m_running) std::fprintf(stderr, "Comparison cancelled: %s\n", reason);
        m_running = false;
    }
    void Clear()
    {
        Cancel("cleared");
        m_hasBaseline = m_hasCandidate = false;
    }
    void CheckConditions(const std::string& conditions)
    {
        if(m_running && conditions != m_conditions) Cancel("view, resolution or quality changed");
    }

    void Update(double activeSeconds, bool submitted,
        const dyf::Platform::ProfilerTimingSnapshot& timing, const dyf::RendererStatistics& statistics)
    {
        if(!m_running) return;
        m_elapsed += std::max(0.0, activeSeconds);
        if(submitted) ++m_frames;
        const bool fresh = timing.sequence != m_lastSequence;
        m_lastSequence = timing.sequence;
        if(!m_measuring)
        {
            if(m_elapsed >= 1 && m_frames >= 10 && fresh)
            {
                m_measuring = true;
                m_elapsed = 0;
                m_frames = 0;
            }
            return;
        }
        if(submitted)
        {
            ++m_current.drawFrames;
            m_current.draws += statistics.mainDrawCount;
            m_current.shadowDraws += statistics.shadowDrawCount;
            if(statistics.gpuVisibility) ++m_current.gpuVisibilityFrames;
            else
            {
                ++m_current.visibleCountFrames;
                m_current.instances += statistics.mainInstanceCount;
                m_current.triangles += statistics.mainTriangleCount;
            }
        }
        if(fresh && timing.frameCount)
        {
            const double count = timing.frameCount;
            m_current.samples += timing.frameCount;
            m_current.frame += timing.frameAverageMilliseconds * count;
            m_current.cpu += timing.cpuRenderAverageMilliseconds * count;
            m_current.acquire += timing.cpuAcquireAverageMilliseconds * count;
            m_current.prepare += timing.cpuPrepareAverageMilliseconds * count;
            m_current.record += timing.cpuRecordAverageMilliseconds * count;
            m_current.submit += timing.cpuSubmitAverageMilliseconds * count;
            m_current.present += timing.cpuPresentAverageMilliseconds * count;
            m_current.maximum = std::max(m_current.maximum, timing.frameMaximumMilliseconds);
            if(timing.hasGpuFrame && timing.gpuFrameSampleCount)
            {
                m_current.gpuSamples += timing.gpuFrameSampleCount;
                m_current.gpu += timing.gpuFrameAverageMilliseconds * timing.gpuFrameSampleCount;
            }
            for(const auto& pass : timing.gpuPasses)
            {
                auto& total = m_current.passes[pass.name];
                total.first += pass.averageMilliseconds * pass.sampleCount;
                total.second += pass.sampleCount;
            }
        }
        if(m_elapsed < 3 || m_frames < 30 || !m_current.samples) return;
        m_running = false;
        if(m_candidate) { m_resultB = m_current; m_hasCandidate = true; }
        else { m_resultA = m_current; m_hasBaseline = true; }
        Print(m_candidate ? 'B' : 'A', m_current);
        if(m_hasBaseline && m_hasCandidate)
        {
            const auto before = Mean(m_resultA.frame, m_resultA.samples);
            const auto after = Mean(m_resultB.frame, m_resultB.samples);
            std::fprintf(stderr, "Comparison B-A: frame %+.3f ms (%+.1f%%). Negative means less time.\n",
                after - before, before > 0 ? 100 * (after / before - 1) : 0);
        }
    }

    void Draw(dyf::Canvas& canvas, uint32_t width, uint32_t height) const
    {
        if((!m_running && !m_hasBaseline && !m_hasCandidate) || width < 160 || height < 100) return;
        const float scale = width < 640 ? 1.f : 1.5f;
        const float y = float(height) - 98 * scale;
        canvas.FillRect({8, y, float(width) - 16, 90 * scale}, {.015f, .02f, .03f, .88f});
        char line[160];
        Text(canvas, m_label.c_str(), 16, y + 6 * scale, scale, {1, 1, 1, 1});
        if(m_running)
            std::snprintf(line, sizeof(line), "%c %s %.1f S  %u FRAMES  F7 CANCEL",
                m_candidate ? 'B' : 'A', m_measuring ? "CAPTURE" : "WARMUP", m_elapsed, m_frames);
        else std::snprintf(line, sizeof(line), "F5 BASELINE  F6 CANDIDATE  F7 CLEAR");
        Text(canvas, line, 16, y + 20 * scale, scale, {.65f, .8f, 1, 1});
        const auto resultLine = [&](char name, const Result& result, bool available, float row, dyf::Math::float4 color)
        {
            if(!available) return;
            char gpu[24];
            if(result.gpuSamples) std::snprintf(gpu, sizeof(gpu), "%.2f", Mean(result.gpu, result.gpuSamples));
            else std::snprintf(gpu, sizeof(gpu), "N/A");
            std::snprintf(line, sizeof(line), "%c FRAME %.2f  CPU %.2f  GPU %s MS",
                name, Mean(result.frame, result.samples), Mean(result.cpu, result.samples), gpu);
            Text(canvas, line, 16, y + row * scale, scale, color);
            const float availableWidth = std::max(0.f, float(width) - 40);
            canvas.FillRect({16, y + (row + 10) * scale,
                availableWidth * float(std::min(Mean(result.frame, result.samples) / 33.333, 1.0)), 3 * scale}, color);
        };
        if(!m_running)
        {
            resultLine('A', m_resultA, m_hasBaseline, 36, {1, .65f, .25f, 1});
            resultLine('B', m_resultB, m_hasCandidate, 58, {.25f, .9f, .65f, 1});
        }
    }

private:
    struct Result
    {
        uint64_t samples = 0, gpuSamples = 0, drawFrames = 0, visibleCountFrames = 0, gpuVisibilityFrames = 0;
        double frame = 0, cpu = 0, gpu = 0, maximum = 0;
        double acquire = 0, prepare = 0, record = 0, submit = 0, present = 0;
        double draws = 0, instances = 0, triangles = 0, shadowDraws = 0;
        std::map<std::string, std::pair<double, uint64_t>> passes;
    };
    static double Mean(double total, uint64_t count) { return count ? total / double(count) : 0; }
    void Print(char name, const Result& result) const
    {
        std::fprintf(stderr, "Comparison %c [%s]\nConditions: %s\n", name, m_label.c_str(), m_conditions.c_str());
        std::fprintf(stderr, "  frame avg %.3f ms, max %.3f ms, cadence %.1f/s, %llu CPU samples\n",
            Mean(result.frame, result.samples), result.maximum, 1000 / Mean(result.frame, result.samples),
            static_cast<unsigned long long>(result.samples));
        std::fprintf(stderr, "  CPU work %.3f = prepare %.3f + record %.3f + submit %.3f ms; acquire %.3f, present %.3f ms\n",
            Mean(result.cpu, result.samples), Mean(result.prepare, result.samples), Mean(result.record, result.samples),
            Mean(result.submit, result.samples), Mean(result.acquire, result.samples), Mean(result.present, result.samples));
        if(result.gpuSamples) std::fprintf(stderr, "  GPU frame %.3f ms (%llu completed samples)\n",
            Mean(result.gpu, result.gpuSamples), static_cast<unsigned long long>(result.gpuSamples));
        else std::fputs("  GPU frame N/A (unsupported or no completed timestamp samples)\n", stderr);
        std::fprintf(stderr, "  Main submitted draws %.1f; shadow draws %.1f\n",
            Mean(result.draws, result.drawFrames),Mean(result.shadowDraws, result.drawFrames));
        if(result.gpuVisibilityFrames)
            std::fputs("  Visible instances/triangles N/A (GPU-selected counts are not read back).\n",stderr);
        else std::fprintf(stderr,"  Main instances %.1f, triangles %.1f\n",
            Mean(result.instances,result.visibleCountFrames),Mean(result.triangles,result.visibleCountFrames));
        for(const auto& [name, total] : result.passes)
            std::fprintf(stderr, "  GPU %-36s %.3f ms (%llu samples)\n", name.c_str(),
                Mean(total.first, total.second), static_cast<unsigned long long>(total.second));
    }
    // Small presentation labels use the same 5x7 bitmap style as the profiler.
    static void Text(dyf::Canvas& canvas, const char* text, float x, float y, float scale, dyf::Math::float4 color)
    {
        for(; *text; ++text, x += 6 * scale)
        {
            const auto glyph = [](char c) -> std::array<uint8_t, 5>
            {
                switch(c)
                {
                case 'A': return {126,17,17,17,126}; case 'B': return {127,73,73,73,54};
                case 'C': return {62,65,65,65,34}; case 'D': return {127,65,65,34,28};
                case 'E': return {127,73,73,73,65}; case 'F': return {127,9,9,9,1};
                case 'G': return {62,65,73,73,122}; case 'H': return {127,8,8,8,127};
                case 'I': return {0,65,127,65,0}; case 'J': return {32,64,65,63,1};
                case 'K': return {127,8,20,34,65}; case 'L': return {127,64,64,64,64};
                case 'M': return {127,2,12,2,127}; case 'N': return {127,4,8,16,127};
                case 'O': return {62,65,65,65,62}; case 'P': return {127,9,9,9,6};
                case 'Q': return {62,65,81,33,94}; case 'R': return {127,9,25,41,70};
                case 'S': return {70,73,73,73,49}; case 'T': return {1,1,127,1,1};
                case 'U': return {63,64,64,64,63}; case 'V': return {31,32,64,32,31};
                case 'W': return {127,32,24,32,127}; case 'X': return {99,20,8,20,99};
                case 'Y': return {3,4,120,4,3}; case 'Z': return {97,81,73,69,67};
                case '0': return {62,81,73,69,62}; case '1': return {0,66,127,64,0};
                case '2': return {66,97,81,73,70}; case '3': return {33,65,69,75,49};
                case '4': return {24,20,18,127,16}; case '5': return {39,69,69,69,57};
                case '6': return {60,74,73,73,48}; case '7': return {1,113,9,5,3};
                case '8': return {54,73,73,73,54}; case '9': return {6,73,73,41,30};
                case '.': return {0,96,96,0,0}; case '/': return {32,16,8,4,2};
                case '-': return {8,8,8,8,8}; default: return {};
                }
            }(*text);
            for(uint32_t column = 0; column < 5; ++column)
                for(uint32_t row = 0; row < 7; ++row)
                    if(glyph[column] & (1u << row)) canvas.FillRect(
                        {x + column * scale, y + row * scale, scale, scale}, color);
        }
    }
    bool m_running = false, m_measuring = false, m_candidate = false;
    bool m_hasBaseline = false, m_hasCandidate = false;
    double m_elapsed = 0;
    uint32_t m_frames = 0;
    uint64_t m_lastSequence = 0;
    std::string m_conditions, m_label;
    Result m_current, m_resultA, m_resultB;
};
}

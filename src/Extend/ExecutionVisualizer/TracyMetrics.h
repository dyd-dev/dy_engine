#pragma once
#include "ViewerState.h"
#include <memory>
#include <string_view>

namespace tracy { class Worker; }
namespace viewer
{
enum class MetricOrigin { Engine, Tracy };
struct MetricEvidence
{
    D::LiveSessionId session;
    D::LiveId device=0, source=0, revision=0;
    uint64_t beginNs=0, endNs=0, frames=0, frameDigest=0;
    uint64_t tracyConnection=0;
    int64_t tracyFirstNs=0, tracyLastNs=0;
    std::vector<D::LiveFrameKey> originalFrames;
    std::string definition;
};
struct MetricChoice
{
    std::optional<double> value;
    MetricOrigin origin=MetricOrigin::Engine;
    std::string reason;
    uint64_t readyFrames=0;
    MetricEvidence evidence;
};
struct MetricPair { MetricChoice before, after; NumberChange change; };
struct TracyRawMessage { int64_t timeNs=0; std::string_view text; };
struct MetricHistoryPoint { MetricChoice choice; bool connectPrevious=false; };
struct MetricOriginRanges { D::LiveMetricRange engine, tracy; };
const char* OriginName(MetricOrigin);

class TracyMetrics
{
public:
    TracyMetrics();
    ~TracyMetrics();
    TracyMetrics(const TracyMetrics&)=delete;
    TracyMetrics& operator=(const TracyMetrics&)=delete;
    // Call only inside EmbeddedTracy::ReadWorker. The callback already holds the lock.
    // No Worker/container/string reference survives this call; no IO or extra connection.
    void Read(tracy::Worker&, const D::LiveSnapshot&);
    // Bind fallback provenance for every snapshot, including when Tracy is unavailable.
    void Observe(const D::LiveSnapshot&);
    // Decoder entry for bounded batches of original Tracy timestamped messages.
    // Plot values are not accepted. Views are borrowed only during the call.
    void ReadMessages(const std::vector<TracyRawMessage>&, const D::LiveSnapshot&);
    void Clear();
    MetricChoice Select(const D::LiveAggregate&, ComparisonMetric) const;
    MetricChoice SelectStage(const D::LiveAggregate&, D::LiveStage, StageMetric) const;
    MetricPair Compare(const D::LiveAggregate&, const D::LiveAggregate&, ComparisonMetric) const;
    MetricPair CompareStage(const D::LiveAggregate&, const D::LiveAggregate&, D::LiveStage, StageMetric) const;
    std::vector<MetricHistoryPoint> History(const std::vector<D::LiveAggregate>&, ComparisonMetric) const;
    MetricOriginRanges LowHigh(const std::vector<D::LiveAggregate>&, ComparisonMetric) const;
    // Completed-window extrema observed during this collection, independent of the 10s History.
    // Engine's producer-owned full-run range remains available in LiveSourceSnapshot::runRanges.
    MetricOriginRanges RunLowHigh(D::LiveId device,D::LiveId source,ComparisonMetric) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}

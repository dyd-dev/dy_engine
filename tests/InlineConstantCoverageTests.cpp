#include "RHI/InlineConstantCoverage.h"

#include <cstdio>
#include <stdexcept>

namespace
{
void Check(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
}

int main()
try
{
    dyf::RHI::Detail::InlineConstantCoverage coverage;
    Check(coverage.IsComplete(), "initial empty layout");
    coverage.Reset(0);
    Check(coverage.IsComplete(), "zero-size layout");

    coverage.Reset(16);
    Check(!coverage.IsComplete(), "missing constants after pipeline bind");
    coverage.Write(0, 8);
    Check(!coverage.IsComplete(), "partial write leaves a gap");
    coverage.Write(4, 8);
    Check(!coverage.IsComplete(), "overlap must not count twice");
    coverage.Write(4, 8);
    Check(!coverage.IsComplete(), "repeated partial write leaves the gap");
    coverage.Write(12, 4);
    Check(coverage.IsComplete(), "overlapping writes cover the whole layout");
    coverage.Write(0, 4);
    Check(coverage.IsComplete(), "partial update preserves complete coverage");

    coverage.Reset(16);
    Check(!coverage.IsComplete(), "same-layout pipeline rebind discards coverage");
    coverage.Write(0, 16);
    Check(coverage.IsComplete(), "full write");
    coverage.Write(8, 8);
    Check(coverage.IsComplete(), "partial update after full write");

    coverage.Reset(8);
    Check(!coverage.IsComplete(), "different-layout pipeline rebind discards coverage");
    coverage.Write(4, 4);
    Check(!coverage.IsComplete(), "out-of-order write leaves a leading gap");
    coverage.Write(0, 4);
    Check(coverage.IsComplete(), "out-of-order writes cover the whole layout");
    coverage.Reset(0);
    Check(coverage.IsComplete(), "rendering reset clears coverage");
    coverage.Reset(16);
    Check(!coverage.IsComplete(), "bind after reset requires new constants");
    coverage.Write(4, 4);
    coverage.Write(0, 16);
    Check(coverage.IsComplete(), "full write completes partial coverage");
    std::puts("InlineConstantCoverageTests: PASS");
    return 0;
}
catch (const std::exception& error)
{
    std::fprintf(stderr, "InlineConstantCoverageTests: FAIL: %s\n", error.what());
    return 1;
}

#include "QueryTransferBounds.h"

#include <iostream>
#include <limits>

namespace
{
bool Require(bool condition, const char* message)
{
    if(!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}
}

int main()
{
    using dyf::Detail::QueryTransferBounds;
    bool passed = true;
    QueryTransferBounds bounds;
    passed &= Require(!bounds.Complete() && !bounds.ConsumePart(), "an inactive transfer rejects a part");
    passed &= Require(!bounds.Begin(0), "an empty transfer is invalid");
    passed &= Require(!bounds.Begin((std::numeric_limits<uint64_t>::max)()), "padding must be representable");
    for(const uint64_t size : {1ull, 11ull, 12ull, 13ull, 23ull, 24ull, 25ull})
    {
        passed &= Require(bounds.Begin(size), "a small transfer starts");
        const auto parts = (size + 11) / 12;
        for(uint64_t part = 0; part < parts; ++part)
        {
            passed &= Require(!bounds.Complete(), "a partial transfer is incomplete");
            passed &= Require(bounds.ConsumePart(), "the declared number of padded parts is accepted");
        }
        passed &= Require(bounds.Complete() && !bounds.ConsumePart(), "completion rejects another part");
        bounds = {};
        passed &= Require(!bounds.Complete() && !bounds.ConsumePart(), "reset releases transfer state");
    }
    return passed ? 0 : 1;
}

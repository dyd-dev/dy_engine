#include "../src/Backends/Vulkan/VulkanValidation.h"

#include <cstdio>
#include <limits>

int main()
{
    struct Case { uint32_t requested, minimum, maximum, expected; };
    constexpr auto largest = std::numeric_limits<uint32_t>::max();
    const Case cases[] = {
        {4, 2, 3, 0},
        {0, 2, 0, 0},
        {2, 2, 2, 2},
        {2, 2, 3, 3},
        {3, 2, 3, 3},
        {4, 2, 0, 4},
        {2, 4, 0, 4},
        {2, 4, 3, 0},
        {largest, 2, 0, largest},
        {largest, 2, largest - 1, 0},
    };
    for(const auto& test : cases)
    {
        const auto selected = dyf::Backends::VulkanValidation::SelectSwapchainImageCount(
            test.requested, test.minimum, test.maximum);
        if(selected != test.expected)
        {
            std::fprintf(stderr, "FAIL image count: requested=%u surface=%u..%u expected=%u got=%u\n",
                test.requested, test.minimum, test.maximum, test.expected, selected);
            return 1;
        }
    }
    std::puts("PASS VulkanSwapchainImageCount: required minimum, surface limits, unlimited maximum, integer boundary");
}

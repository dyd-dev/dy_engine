#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dy::Core
{
// Advances one code point; malformed input produces U+FFFD. Call while at < size.
inline uint32_t DecodeUtf8(std::string_view text, size_t& at)
{
    if (at >= text.size()) return 0xFFFD;
    const auto first = static_cast<uint8_t>(text[at++]);
    if (first < 0x80) return first;
    const uint32_t count = first >= 0xF0 ? 3 : first >= 0xE0 ? 2 : first >= 0xC2 ? 1 : 0;
    if (!count || first > 0xF4 || count > text.size() - at) return 0xFFFD;
    uint32_t codepoint = first & ((1u << (6 - count)) - 1);
    for (uint32_t i = 0; i < count; ++i)
    {
        const auto next = static_cast<uint8_t>(text[at]);
        if ((next & 0xC0) != 0x80) return 0xFFFD;
        ++at;
        codepoint = (codepoint << 6) | (next & 0x3F);
    }
    if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)
        || (count == 1 && codepoint < 0x80) || (count == 2 && codepoint < 0x800)
        || (count == 3 && codepoint < 0x10000)) return 0xFFFD;
    return codepoint;
}
}

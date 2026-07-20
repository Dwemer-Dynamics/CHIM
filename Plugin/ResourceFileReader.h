#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace ResourceFileReader
{
    inline constexpr std::size_t kDefaultMaximumBytes = 128ULL * 1024ULL * 1024ULL;

    bool Read(const std::string& resourcePath, std::string& output, std::string& failureReason,
              std::size_t maximumBytes = kDefaultMaximumBytes);
}

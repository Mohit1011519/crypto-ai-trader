#pragma once

#include <cstddef>
#include <cstdint>

namespace crypto_trader::market {

struct ResearchMetadata {
    std::int64_t timestamp_ms;
    std::size_t sample_count;
    double quality;
};

}  // namespace crypto_trader::market

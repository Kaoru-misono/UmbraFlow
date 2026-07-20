#pragma once

#include "error.hpp"

#include <core/time/monotonic-time.hpp>

#include <cstdint>

namespace uf
{
    [[nodiscard]]
    auto checkedAddMonotonic(
        MonotonicInstant instant,
        MonotonicInstant::Duration duration
    ) -> Result<MonotonicInstant>;

    [[nodiscard]]
    auto elapsedNanosecondsSince(
        MonotonicInstant instant,
        MonotonicInstant origin
    ) noexcept -> std::uint64_t;
}

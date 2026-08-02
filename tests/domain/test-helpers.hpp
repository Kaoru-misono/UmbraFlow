#pragma once

#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <doctest/doctest.h>

// The three domain values a test builds most often. Each refuses to construct
// itself from bad numbers, so these remove that check from every call site.
namespace uf::test
{
    inline auto fingerprint(
        uint32 width  = 4,
        uint32 height = 4,
        uint32 dpiX   = 96,
        uint32 dpiY   = 96
    ) -> ProjectFingerprint
    {
        auto const result = ProjectFingerprint::create(width, height, dpiX, dpiY);
        REQUIRE(result.has_value());
        return *result;
    }

    inline auto pixelRect(
        uint32 x,
        uint32 y,
        uint32 width,
        uint32 height
    ) -> PixelRect
    {
        auto const result = PixelRect::create(x, y, width, height);
        REQUIRE(result.has_value());
        return *result;
    }

    inline auto instantAt(MonotonicInstant::Duration duration) -> MonotonicInstant
    {
        return MonotonicInstant::fromTimePoint(
            MonotonicInstant::TimePoint{duration}
        );
    }
}

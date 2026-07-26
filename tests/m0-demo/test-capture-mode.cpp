#include "test-helpers.hpp"

#include <args.hpp>
#include <capture-mode.hpp>

#include <core/types/integer.hpp>

#include <doctest/doctest.h>

#include <filesystem>
#include <optional>
#include <utility>

namespace uf::m0_demo
{
    namespace
    {
        [[nodiscard]]
        auto captureArgs(
            std::filesystem::path output,
            uint32 frames,
            std::filesystem::path log
        ) -> CaptureArgs
        {
            return CaptureArgs{
                .selector = {},
                .output   = std::move(output),
                .frames   = frames,
                .interval = k_defaultCaptureInterval,
                .log      = std::move(log),
            };
        }
    }

    TEST_CASE("m0 capture rejects a log path that aliases any output PNG")
    {
        for (auto const& args : {
            captureArgs("capture.png", 1U, "./capture.png"),
            captureArgs("capture.png", 3U, "./capture-2.png"),
        })
        {
            auto const result = validateCaptureOutputPaths(args);

            REQUIRE_FALSE(result.has_value());
            test_m0_demo::requireErrorKind(
                result.error(),
                AutomationErrorKind::InvalidResource
            );
            CHECK(result.error().message().contains("aliases output PNG"));
        }
    }

    TEST_CASE("m0 capture accepts distinct log and output PNG paths")
    {
        auto const args = captureArgs(
            "capture.png",
            3U,
            "capture.jsonl"
        );

        CHECK(validateCaptureOutputPaths(args));
    }
}

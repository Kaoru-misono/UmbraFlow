#include "test-helpers.hpp"

#include <args.hpp>
#include <capture-mode.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <utility>

namespace
{
    [[nodiscard]]
    auto captureArgs(
        std::filesystem::path output,
        std::uint32_t frames,
        std::filesystem::path log
    ) -> uf::m0_demo::CaptureArgs
    {
        return uf::m0_demo::CaptureArgs{
            .m_selector = {},
            .m_output = std::move(output),
            .m_frames = frames,
            .m_interval = uf::m0_demo::g_defaultCaptureInterval,
            .m_log = std::move(log),
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
        auto const result = uf::m0_demo::validateCaptureOutputPaths(args);

        REQUIRE_FALSE(result.has_value());
        test_m0_demo::requireErrorKind(
            result.error(),
            uf::AutomationErrorKind::InvalidResource
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

    CHECK(uf::m0_demo::validateCaptureOutputPaths(args));
}

#include "test-helpers.hpp"

#include <log-jsonl.hpp>

#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

namespace
{
    [[nodiscard]]
    auto writeFailure() -> std::error_code
    {
        return std::make_error_code(std::errc::permission_denied);
    }

    [[nodiscard]]
    auto flushFailure() -> std::error_code
    {
        return std::make_error_code(std::errc::no_space_on_device);
    }

    class WriteFails final : public uf::m0_demo::IJsonlSink
    {
    public:
        auto writeLine(std::string_view) -> std::error_code override
        {
            return writeFailure();
        }

        auto flush() -> std::error_code override
        {
            return {};
        }
    };

    class FlushFails final : public uf::m0_demo::IJsonlSink
    {
    public:
        auto writeLine(std::string_view) -> std::error_code override
        {
            return {};
        }

        auto flush() -> std::error_code override
        {
            return flushFailure();
        }
    };
}

TEST_CASE("m0 automation error detail uses the reflected domain kind")
{
    auto const failure = uf::fail(
        uf::AutomationErrorKind::StaleObservation,
        "frame lease expired"
    );
    CHECK(
        uf::m0_demo::formatAutomationError(failure.error())
        == "StaleObservation: frame lease expired"
    );
}

TEST_CASE("m0 JSONL serialization carries every field in schema order")
{
    auto const line = uf::m0_demo::LogLine{"action", "home_click"}
        .loopIndex(3)
        .confidence(1200)
        .leaseOk(true)
        .outcome("ok")
        .detail("clicked home");
    auto const json = uf::m0_demo::serializeLine(line);

    CHECK(
        json
        == "{\"elapsed_ns\":0,\"loop_idx\":3,\"phase\":\"action\",\"event\":\"home_click\",\"frame_id\":null,\"target_generation\":null,\"confidence\":1200,\"lease_ok\":true,\"outcome\":\"ok\",\"detail\":\"clicked home\"}"
    );
}

TEST_CASE("m0 JSONL absent optionals are null and outcome defaults to info")
{
    auto const json = uf::m0_demo::serializeLine(
        uf::m0_demo::LogLine{"setup", "dpi_declared"}
    );
    CHECK(
        json
        == "{\"elapsed_ns\":0,\"loop_idx\":null,\"phase\":\"setup\",\"event\":\"dpi_declared\",\"frame_id\":null,\"target_generation\":null,\"confidence\":null,\"lease_ok\":null,\"outcome\":\"info\",\"detail\":\"\"}"
    );
}

TEST_CASE("m0 JSONL write and flush failures are reported")
{
    auto writeFails = uf::m0_demo::JsonlLog{std::make_unique<WriteFails>()};
    auto const writeResult = writeFails.write(
        uf::m0_demo::LogLine{"test", "write_failure"}
    );
    REQUIRE_FALSE(writeResult.has_value());
    test_m0_demo::requireErrorKind(
        writeResult.error(),
        uf::AutomationErrorKind::InvalidResource
    );
    CHECK(writeResult.error().message().find("JSONL write failed") != std::string_view::npos);
    CHECK(
        writeResult.error().message().find(writeFailure().message())
        != std::string_view::npos
    );

    auto flushFails = uf::m0_demo::JsonlLog{std::make_unique<FlushFails>()};
    auto const writeBeforeFlush = flushFails.write(
        uf::m0_demo::LogLine{"test", "flush_failure"}
    );
    REQUIRE(writeBeforeFlush.has_value());
    auto const flushResult = flushFails.flush();
    REQUIRE_FALSE(flushResult.has_value());
    test_m0_demo::requireErrorKind(
        flushResult.error(),
        uf::AutomationErrorKind::InvalidResource
    );
    CHECK(flushResult.error().message().find("JSONL flush failed") != std::string_view::npos);
    CHECK(
        flushResult.error().message().find(flushFailure().message())
        != std::string_view::npos
    );
}

TEST_CASE("m0 JSONL log-open failures retain the operating-system cause")
{
    auto const directory = std::filesystem::temp_directory_path();
    auto const result = uf::m0_demo::JsonlLog::create(directory);
    REQUIRE_FALSE(result.has_value());
    test_m0_demo::requireErrorKind(
        result.error(),
        uf::AutomationErrorKind::InvalidResource
    );
    CHECK(result.error().message().starts_with("cannot open log file "));
    CHECK(result.error().message().rfind(": ") != std::string_view::npos);
}

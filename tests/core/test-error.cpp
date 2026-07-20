#include <core/error/result.hpp>

#include <doctest/doctest.h>

#include <string>

TEST_CASE("recoverable errors preserve structured context")
{
    using namespace umbra_flow;

    auto result = Result<int>{fail(ErrorCode::InvalidArgument, "invalid project input")};
    REQUIRE_FALSE(result.has_value());

    auto& error = result.error();
    error.addContext("parsing project configuration");

    CHECK(error.code() == ErrorCode::InvalidArgument);
    REQUIRE(error.context().size() == 1);
    CHECK(error.context().front() == "parsing project configuration");
    CHECK(toString(error).find("InvalidArgument") != std::string::npos);
}

TEST_CASE("result context helper preserves the original failure")
{
    auto result = umbra_flow::withContext(
        umbra_flow::Result<int>{
            umbra_flow::fail(umbra_flow::ErrorCode::NotFound, "missing")
        },
        "loading template"
    );

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code() == umbra_flow::ErrorCode::NotFound);
    REQUIRE(result.error().context().size() == 1);
    CHECK(result.error().context().front() == "loading template");
}

namespace
{
    [[nodiscard]]
    auto failValidation() -> umbra_flow::Status
    {
        return umbra_flow::fail(
            umbra_flow::ErrorCode::FailedPrecondition,
            "validation failed"
        );
    }

    [[nodiscard]]
    auto propagateValidation() -> umbra_flow::Status
    {
        UMBRA_FLOW_TRY_CONTEXT(failValidation(), "preparing project");
        return umbra_flow::ok();
    }

    [[nodiscard]]
    auto loadVersion(bool available) -> umbra_flow::Result<int>
    {
        if (!available)
        {
            return umbra_flow::fail(
                umbra_flow::ErrorCode::NotFound,
                "version is unavailable"
            );
        }

        return 41;
    }

    [[nodiscard]]
    auto propagateValue(bool available) -> umbra_flow::Result<int>
    {
        UMBRA_FLOW_TRY_VALUE_CONTEXT(version, loadVersion(available), "loading version");
        return version + 1;
    }
}

TEST_CASE("status propagation adds context without logging")
{
    auto result = propagateValidation();

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().context().size() == 1);
    CHECK(result.error().context().front() == "preparing project");
}

TEST_CASE("result propagation extracts a successful value")
{
    auto result = propagateValue(true);

    REQUIRE(result.has_value());
    CHECK(*result == 42);

    result = propagateValue(false);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().context().size() == 1);
    CHECK(result.error().context().front() == "loading version");
}

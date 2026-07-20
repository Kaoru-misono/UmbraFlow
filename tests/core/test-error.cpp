#include <core/error/result.hpp>

#include <doctest/doctest.h>

#include <string>

TEST_CASE("recoverable errors preserve structured context")
{
    using namespace uf;

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
    auto result = uf::withContext(
        uf::Result<int>{
            uf::fail(uf::ErrorCode::NotFound, "missing")
        },
        "loading template"
    );

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code() == uf::ErrorCode::NotFound);
    REQUIRE(result.error().context().size() == 1);
    CHECK(result.error().context().front() == "loading template");
}

namespace
{
    [[nodiscard]]
    auto failValidation() -> uf::Status
    {
        return uf::fail(
            uf::ErrorCode::FailedPrecondition,
            "validation failed"
        );
    }

    [[nodiscard]]
    auto propagateValidation() -> uf::Status
    {
        UF_TRY_CONTEXT(failValidation(), "preparing project");
        return uf::ok();
    }

    [[nodiscard]]
    auto loadVersion(bool available) -> uf::Result<int>
    {
        if (!available)
        {
            return uf::fail(
                uf::ErrorCode::NotFound,
                "version is unavailable"
            );
        }

        return 41;
    }

    [[nodiscard]]
    auto propagateValue(bool available) -> uf::Result<int>
    {
        UF_TRY_VALUE_CONTEXT(version, loadVersion(available), "loading version");
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

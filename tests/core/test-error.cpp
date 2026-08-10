#include <core/error/result.hpp>

#include <doctest/doctest.h>

#include <string>
#include <system_error>
#include <type_traits>

namespace uf
{
    namespace
    {
        [[nodiscard]]
        auto invalidArgument() noexcept -> std::error_code
        {
            return std::make_error_code(std::errc::invalid_argument);
        }

        [[nodiscard]]
        auto notFound() noexcept -> std::error_code
        {
            return std::make_error_code(std::errc::no_such_file_or_directory);
        }
    }

    // The single-owner contract is the reason Error holds a unique_ptr at all.
    // A clone test alone still passes if a copy constructor is reintroduced.
    static_assert(!std::is_copy_constructible_v<Error>);
    static_assert(std::is_nothrow_move_constructible_v<Error>);

    TEST_CASE("recoverable errors preserve structured context")
    {
        auto result = Result<int>{fail(invalidArgument(), "invalid project input")};
        REQUIRE_FALSE(result.has_value());

        auto& error = result.error();
        error.addContext("parsing project configuration");

        CHECK(error.detailCode() == invalidArgument());
        REQUIRE(error.context().size() == 1);
        CHECK(error.context().front() == "parsing project configuration");
    }

    TEST_CASE("cloning an error copies its payload instead of sharing it")
    {
        auto original = Error{invalidArgument(), "invalid project input"};
        auto copy     = original.clone();
        copy.addContext("only on the copy");

        CHECK(original.context().empty());
        REQUIRE(copy.context().size() == 1);
        CHECK(copy.message() == original.message());
        CHECK(copy.detailCode() == original.detailCode());
    }

    TEST_CASE("result context helper preserves the original failure")
    {
        auto result = withContext(
            Result<int>{
                fail(notFound(), "missing")
            },
            "loading template"
        );

        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().detailCode() == notFound());
        REQUIRE(result.error().context().size() == 1);
        CHECK(result.error().context().front() == "loading template");
    }

    namespace
    {
        [[nodiscard]]
        auto failValidation() -> Status
        {
            return fail(
                invalidArgument(),
                "validation failed"
            );
        }

        [[nodiscard]]
        auto propagateValidation() -> Status
        {
            UF_TRY_CONTEXT(failValidation(), "preparing project");
            return ok();
        }

        [[nodiscard]]
        auto propagatePlain() -> Status
        {
            UF_TRY(failValidation());
            return ok();
        }

        [[nodiscard]]
        auto loadVersion(bool available) -> Result<int>
        {
            if (!available)
            {
                return fail(
                    notFound(),
                    "version is unavailable"
                );
            }

            return 41;
        }

        [[nodiscard]]
        auto propagatePlainValue(bool available) -> Result<int>
        {
            UF_TRY_VALUE(version, loadVersion(available));
            return version + 1;
        }

        [[nodiscard]]
        auto propagateValue(bool available) -> Result<int>
        {
            UF_TRY_VALUE_CONTEXT(version, loadVersion(available), "loading version");
            return version + 1;
        }
    }

    TEST_CASE("plain propagation macros move the failure out unchanged")
    {
        auto const status = propagatePlain();
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().detailCode() == invalidArgument());
        CHECK(status.error().context().empty());

        auto const value = propagatePlainValue(true);
        REQUIRE(value.has_value());
        CHECK(*value == 42);

        auto const missing = propagatePlainValue(false);
        REQUIRE_FALSE(missing.has_value());
        CHECK(missing.error().detailCode() == notFound());
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
}

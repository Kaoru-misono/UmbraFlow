#include <shutdown.hpp>

#include <doctest/doctest.h>

#include <expected>
#include <ranges>
#include <string_view>
#include <vector>

namespace uf::m0_demo
{
    namespace
    {
        using TestResult = std::expected<void, std::string_view>;
    }

    TEST_CASE("m0 shutdown releases before close audit and flush")
    {
        auto order = std::vector<std::string_view>{};
        auto const result = runShutdown(
            order,
            [](auto& values) -> TestResult
            {
                values.emplace_back("release");
                return {};
            },
            [](auto& values) -> TestResult
            {
                values.emplace_back("close");
                return {};
            },
            [](auto& values) -> TestResult
            {
                values.emplace_back("audit");
                return {};
            },
            [](auto& values) -> TestResult
            {
                values.emplace_back("flush");
                return {};
            }
        );

        REQUIRE(result.has_value());
        auto const expected = std::vector<std::string_view>{
            "release",
            "close",
            "audit",
            "flush",
        };
        CHECK(order == expected);
        auto const release = std::ranges::find(order, "release");
        auto const flush = std::ranges::find(order, "flush");
        REQUIRE(release != order.end());
        REQUIRE(flush != order.end());
        CHECK(release < flush);
    }

    TEST_CASE("m0 shutdown runs later stages after an earlier error")
    {
        auto order = std::vector<std::string_view>{};
        auto const result = runShutdown(
            order,
            [](auto& values) -> TestResult
            {
                values.emplace_back("release");
                return std::unexpected{std::string_view{"release failed"}};
            },
            [](auto& values) -> TestResult
            {
                values.emplace_back("close");
                return {};
            },
            [](auto& values) -> TestResult
            {
                values.emplace_back("audit");
                return {};
            },
            [](auto& values) -> TestResult
            {
                values.emplace_back("flush");
                return {};
            }
        );

        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == "release failed");
        auto const expected = std::vector<std::string_view>{
            "release",
            "close",
            "audit",
            "flush",
        };
        CHECK(order == expected);
    }

    TEST_CASE("m0 module stop state starts clear")
    {
        CHECK_FALSE(stopRequested());
    }
}

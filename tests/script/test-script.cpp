#include <script/engine.hpp>

#include <domain/error.hpp>

#include <doctest/doctest.h>

namespace
{
    TEST_CASE("Engine runs a Luau script and returns its numeric result")
    {
        auto engine = uf::script::Engine::create();
        REQUIRE(engine.has_value());

        auto const result = engine->runNumber("return 1 + 2", "sum");
        REQUIRE(result.has_value());
        CHECK(*result == doctest::Approx(3.0));
    }

    TEST_CASE("Engine reports a compile error as a recoverable failure")
    {
        auto engine = uf::script::Engine::create();
        REQUIRE(engine.has_value());

        auto const result = engine->runNumber("return 1 +", "broken");
        REQUIRE_FALSE(result.has_value());

        auto const kind = uf::automationErrorKind(result.error());
        REQUIRE(kind.has_value());
        CHECK(*kind == uf::AutomationErrorKind::InvalidResource);
    }

    TEST_CASE("Engine reports a runtime error as a recoverable failure")
    {
        auto engine = uf::script::Engine::create();
        REQUIRE(engine.has_value());

        auto const result = engine->runNumber("error('boom')", "runtime");
        REQUIRE_FALSE(result.has_value());

        auto const kind = uf::automationErrorKind(result.error());
        REQUIRE(kind.has_value());
        CHECK(*kind == uf::AutomationErrorKind::InvalidResource);
    }

    TEST_CASE("Engine returns zero for a script without a numeric result")
    {
        auto engine = uf::script::Engine::create();
        REQUIRE(engine.has_value());

        auto const result = engine->runNumber("local x = 5", "noreturn");
        REQUIRE(result.has_value());
        CHECK(*result == doctest::Approx(0.0));
    }
}

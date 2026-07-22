#include <script/engine.hpp>

#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <utility>

namespace uf::script
{
    namespace
    {
        TEST_CASE("Engine runs a Luau script and returns its numeric result")
        {
            auto engine = Engine::create();
            REQUIRE(engine.has_value());

            auto const result = engine->runNumber("return 1 + 2", "sum");
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(3.0));
        }

        TEST_CASE("Engine reports a compile/load error as a recoverable failure")
        {
            auto engine = Engine::create();
            REQUIRE(engine.has_value());

            auto const result = engine->runNumber("return 1 +", "broken");
            REQUIRE_FALSE(result.has_value());

            auto const kind = automationErrorKind(result.error());
            REQUIRE(kind.has_value());
            CHECK(kind == AutomationErrorKind::InvalidResource);
        }

        TEST_CASE("Engine reports a runtime error as a recoverable failure")
        {
            auto engine = Engine::create();
            REQUIRE(engine.has_value());

            auto const result = engine->runNumber("error('boom')", "runtime");
            REQUIRE_FALSE(result.has_value());

            auto const kind = automationErrorKind(result.error());
            REQUIRE(kind.has_value());
            CHECK(kind == AutomationErrorKind::InvalidResource);
        }

        TEST_CASE("Engine returns zero when there is no numeric result")
        {
            auto engine = Engine::create();
            REQUIRE(engine.has_value());

            SUBCASE("no return value")
            {
                auto const result = engine->runNumber("local x = 5", "noreturn");
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(0.0));
            }
            SUBCASE("non-numeric return value")
            {
                auto const result = engine->runNumber("return 'text'", "nonnumeric");
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(0.0));
            }
        }

        TEST_CASE("Engine does not accumulate state across repeated runs")
        {
            auto engine = Engine::create();
            REQUIRE(engine.has_value());

            // A per-call thread/stack leak would eventually destabilize the main state.
            for (int i = 0; i < 500; ++i)
            {
                auto const ok = engine->runNumber("return 41 + 1", "loop");
                REQUIRE(ok.has_value());
                CHECK(*ok == doctest::Approx(42.0));
            }
        }

        TEST_CASE("Engine is move-only and usable after a move")
        {
            auto source = Engine::create();
            REQUIRE(source.has_value());

            auto moved = std::move(*source);
            auto const result = moved.runNumber("return 7", "moved");
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(7.0));
        }
    }
}

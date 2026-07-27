#include "../annotation/test-helpers.hpp"

#include <task/capability-surface.hpp>

#include <script/engine.hpp>

#include <annotation/catalog.hpp>

#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::task
{
    namespace
    {
        namespace at = annotation::test;

        constexpr auto k_anchorId  = "00000000-0000-0000-0000-000000000001";
        constexpr auto k_dailyId   = "00000000-0000-0000-0000-000000000002";
        constexpr auto k_battleId  = "00000000-0000-0000-0000-000000000003";
        constexpr auto k_pageId    = "00000000-0000-0000-0000-000000000101";

        // A minimal but complete catalog: one page anchor, two action targets,
        // and one page. Built from the annotation module's own test fixtures
        // (tests/annotation/test-helpers.hpp) so the surface is driven by exactly
        // the catalog shape the engine loads at runtime.
        auto buildCatalog() -> annotation::RecognitionCatalog
        {
            auto const fingerprint = at::fingerprint();
            auto const pageId      = at::pageId(k_pageId);

            auto recognizers = std::vector<annotation::RecognizerDefinition>{};
            recognizers.push_back(at::recognizer(
                fingerprint,
                at::recognizerId(k_anchorId),
                "home_marker",
                annotation::AnnotationType::PageAnchor,
                at::pixelRect(0, 0, 1, 1),
                at::pixelRect(0, 0, 4, 4)
            ));
            recognizers.push_back(at::recognizer(
                fingerprint,
                at::recognizerId(k_dailyId),
                "daily_button",
                annotation::AnnotationType::ActionTarget,
                at::pixelRect(1, 1, 1, 1),
                at::pixelRect(0, 0, 4, 4),
                {pageId}
            ));
            recognizers.push_back(at::recognizer(
                fingerprint,
                at::recognizerId(k_battleId),
                "battle",
                annotation::AnnotationType::ActionTarget,
                at::pixelRect(2, 2, 1, 1),
                at::pixelRect(0, 0, 4, 4),
                {pageId}
            ));

            return at::catalog(
                fingerprint,
                std::move(recognizers),
                {at::page(pageId, "home", {at::recognizerId(k_anchorId)})}
            );
        }

        // Builds an engine whose only host capability is the umbra table for the
        // catalog above.
        auto engineWithUmbra(CapabilitySurface const& surface)
            -> Result<script::Engine>
        {
            auto config              = script::EngineConfig{};
            config.installHostTables = surface.installer();
            return script::Engine::create(config);
        }

        // Runs `return (<expr>) and 1 or 0`; 1.0 means the expression was truthy.
        auto truthy(script::Engine& engine, std::string_view expr) -> double
        {
            auto const source = "return (" + std::string{expr} + ") and 1 or 0";
            auto const result = engine.runNumber(source, "umbra-expr");
            REQUIRE(result.has_value());
            return *result;
        }

        // Asserts `source` fails at runtime (a rejected write, a protected
        // metatable, and so on surface as InvalidResource).
        auto expectRejected(script::Engine& engine, std::string_view source) -> void
        {
            auto const result = engine.runNumber(std::string{source}, "umbra-reject");
            REQUIRE_FALSE(result.has_value());
            CHECK(
                automationErrorKind(result.error())
                == AutomationErrorKind::InvalidResource
            );
        }

        TEST_CASE("CapabilitySurface exposes only action targets and every page")
        {
            auto const catalog = buildCatalog();
            auto surface       = CapabilitySurface::create(catalog);
            REQUIRE(surface.has_value());

            // Page anchors are not findable, so only the two action targets are
            // exposed; every page is exposed.
            CHECK(surface->recognizerCount() == 2);
            CHECK(surface->pageCount() == 1);
        }

        TEST_CASE("Scripts read named handles and see nil for absent or anchor names")
        {
            auto const catalog = buildCatalog();
            auto surface       = CapabilitySurface::create(catalog);
            REQUIRE(surface.has_value());

            auto engine = engineWithUmbra(*surface);
            REQUIRE(engine.has_value());

            constexpr std::string_view present[] = {
                "umbra.recognizers.daily_button ~= nil",
                "umbra.recognizers.battle ~= nil",
                "umbra.pages.home ~= nil",
            };
            for (std::string_view const expr : present)
            {
                INFO("expression: ", expr);
                CHECK(truthy(*engine, expr) == doctest::Approx(1.0));
            }

            constexpr std::string_view absent[] = {
                // A page anchor is page-internal evidence, never a findable handle.
                "umbra.recognizers.home_marker == nil",
                // Names that do not exist resolve to nil, not an error.
                "umbra.recognizers.does_not_exist == nil",
                "umbra.pages.does_not_exist == nil",
            };
            for (std::string_view const expr : absent)
            {
                INFO("expression: ", expr);
                CHECK(truthy(*engine, expr) == doctest::Approx(1.0));
            }
        }

        TEST_CASE("The umbra tables and handles reject every write")
        {
            auto const catalog = buildCatalog();
            auto surface       = CapabilitySurface::create(catalog);
            REQUIRE(surface.has_value());

            auto engine = engineWithUmbra(*surface);
            REQUIRE(engine.has_value());

            SUBCASE("a new key on the umbra root is rejected")
            {
                expectRejected(*engine, "umbra.injected = 1\nreturn 0");
            }
            SUBCASE("a new key on umbra.recognizers is rejected")
            {
                expectRejected(*engine, "umbra.recognizers.injected = 1\nreturn 0");
            }
            SUBCASE("overwriting an existing recognizer handle is rejected")
            {
                expectRejected(*engine, "umbra.recognizers.daily_button = 1\nreturn 0");
            }
            SUBCASE("a new key on umbra.pages is rejected")
            {
                expectRejected(*engine, "umbra.pages.injected = 1\nreturn 0");
            }
            SUBCASE("writing a field on a handle is rejected")
            {
                expectRejected(
                    *engine,
                    "umbra.recognizers.daily_button.x = 1\nreturn 0"
                );
            }
        }

        TEST_CASE("Handles are opaque userdata that leak neither metatable nor fields")
        {
            auto const catalog = buildCatalog();
            auto surface       = CapabilitySurface::create(catalog);
            REQUIRE(surface.has_value());

            auto engine = engineWithUmbra(*surface);
            REQUIRE(engine.has_value());

            // A handle is userdata, and its metatable and tostring reveal only the
            // fixed kind label -- never the internal id or an address.
            constexpr std::string_view opaque[] = {
                "type(umbra.recognizers.daily_button) == 'userdata'",
                "type(umbra.pages.home) == 'userdata'",
                "tostring(umbra.recognizers.daily_button) == 'umbra.recognizer'",
                "tostring(umbra.pages.home) == 'umbra.page'",
                "getmetatable(umbra.recognizers.daily_button) == 'umbra.recognizer'",
                "getmetatable(umbra.pages.home) == 'umbra.page'",
                // A field read of a handle yields nil (the method table is empty
                // this wave), never any internal state.
                "umbra.recognizers.daily_button.id == nil",
                // pairs() cannot enumerate a userdata's contents.
                "pcall(function() for _ in pairs(umbra.recognizers.daily_button) do end end) == false",
            };
            for (std::string_view const expr : opaque)
            {
                INFO("expression: ", expr);
                CHECK(truthy(*engine, expr) == doctest::Approx(1.0));
            }
        }

        TEST_CASE("The handle metatable is protected against replacement")
        {
            auto const catalog = buildCatalog();
            auto surface       = CapabilitySurface::create(catalog);
            REQUIRE(surface.has_value());

            auto engine = engineWithUmbra(*surface);
            REQUIRE(engine.has_value());

            // setmetatable on a handle is refused: the guard cannot be lifted to
            // reopen the handle for indexing or writing.
            expectRejected(
                *engine,
                "setmetatable(umbra.recognizers.daily_button, {})\nreturn 0"
            );
        }

        TEST_CASE("Distinct handles carry distinct identity, equal handles compare equal")
        {
            auto const catalog = buildCatalog();
            auto surface       = CapabilitySurface::create(catalog);
            REQUIRE(surface.has_value());

            auto engine = engineWithUmbra(*surface);
            REQUIRE(engine.has_value());

            // Each named handle is one shared userdata object, so repeated lookups
            // are identical, while two different names are two different objects.
            CHECK(
                truthy(
                    *engine,
                    "umbra.recognizers.daily_button == umbra.recognizers.daily_button"
                )
                == doctest::Approx(1.0)
            );
            CHECK(
                truthy(
                    *engine,
                    "umbra.recognizers.daily_button ~= umbra.recognizers.battle"
                )
                == doctest::Approx(1.0)
            );
        }
    }
}

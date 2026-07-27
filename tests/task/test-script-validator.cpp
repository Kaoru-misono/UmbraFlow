#include "../annotation/test-helpers.hpp"

#include <task/capability-surface.hpp>
#include <task/script-validator.hpp>

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

        constexpr auto k_anchorId = "00000000-0000-0000-0000-000000000001";
        constexpr auto k_dailyId  = "00000000-0000-0000-0000-000000000002";
        constexpr auto k_battleId = "00000000-0000-0000-0000-000000000003";
        constexpr auto k_pageId   = "00000000-0000-0000-0000-000000000101";

        // The same minimal catalog the binding tests use: one page anchor (never
        // findable), two action targets (daily_button, battle), and one page
        // (home). The surface built from it is exactly what the script sees, so
        // the validator resolves against the identical name set.
        auto buildSurface() -> CapabilitySurface
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

            auto const catalog = at::catalog(
                fingerprint,
                std::move(recognizers),
                {at::page(pageId, "home", {at::recognizerId(k_anchorId)})}
            );

            auto surface = CapabilitySurface::create(catalog);
            REQUIRE(surface.has_value());
            return *std::move(surface);
        }

        // Validates `source` and asserts it was rejected as InvalidResource whose
        // message contains every `needle`. Substring assertions pin the failure to
        // the specific offending reference, not merely "some error", so a change
        // that rejects for the wrong reason is caught.
        auto expectRejected(
            CapabilitySurface const& surface,
            std::string_view source,
            std::vector<std::string_view> needles
        ) -> void
        {
            auto const report = validateScriptResources(source, "task", surface);
            REQUIRE_FALSE(report.has_value());
            CHECK(
                automationErrorKind(report.error())
                == AutomationErrorKind::InvalidResource
            );
            auto const message = std::string{report.error().message()};
            for (std::string_view const needle : needles)
            {
                INFO("needle: ", needle);
                INFO("message: ", message);
                CHECK(message.find(needle) != std::string::npos);
            }
        }

        TEST_CASE("A canonical task script validates and enumerates its references")
        {
            auto const surface = buildSurface();

            // Every approved shape at once: verb method calls (umbra:capture,
            // frame:resolve_page, outcome:resolved, page:is, frame:find,
            // umbra:click, umbra:wait_for_page, umbra:try) and two-level resource
            // literals, including one used inside a verb argument and one repeated.
            constexpr std::string_view source = R"lua(
                local frame = umbra:capture()
                local outcome = frame:resolve_page()
                local page = outcome:resolved()
                if page ~= nil and page:is(umbra.pages.home) then
                    local hit = frame:find(umbra.recognizers.battle)
                    if hit ~= nil then
                        umbra:click(page, hit)
                    end
                end
                local another = frame:find(umbra.recognizers.daily_button)
                local wait = umbra:wait_for_page(umbra.pages.home, { timeout_ms = 1000 })
                umbra:try(function()
                    umbra:click(page, another)
                end)
                return 0
            )lua";

            auto const report = validateScriptResources(source, "daily", surface);
            REQUIRE(report.has_value());
            CHECK(
                report->recognizers
                == std::vector<std::string>{"battle", "daily_button"}
            );
            CHECK(report->pages == std::vector<std::string>{"home"});
        }

        TEST_CASE("A script touching no resources validates with an empty report")
        {
            auto const surface = buildSurface();

            auto const report =
                validateScriptResources("local x = 1 + 2\nreturn x", "noop", surface);
            REQUIRE(report.has_value());
            CHECK(report->recognizers.empty());
            CHECK(report->pages.empty());
        }

        TEST_CASE("An unknown verb method call is not a resource and passes")
        {
            auto const surface = buildSurface();

            // The validator closes resource references, not the verb set: an
            // unknown verb is a runtime nil-call, out of this pass's scope.
            auto const report =
                validateScriptResources("umbra:frobnicate()\nreturn 0", "verb", surface);
            REQUIRE(report.has_value());
            CHECK(report->recognizers.empty());
            CHECK(report->pages.empty());
        }

        TEST_CASE("A reference to a missing recognizer is rejected by name")
        {
            auto const surface = buildSurface();
            expectRejected(
                surface,
                "return umbra:capture():find(umbra.recognizers.does_not_exist)",
                {"does_not_exist", "recognizer"}
            );
        }

        TEST_CASE("A reference to a missing page is rejected by name")
        {
            auto const surface = buildSurface();
            expectRejected(
                surface,
                "local p = umbra.pages.does_not_exist\nreturn p",
                {"does_not_exist", "page"}
            );
        }

        TEST_CASE("A page anchor is not exposed under umbra.recognizers")
        {
            auto const surface = buildSurface();
            // home_marker is a real catalog recognizer, but a page anchor, so it
            // is never a findable handle; the validator rejects it as missing.
            expectRejected(
                surface,
                "return umbra:capture():find(umbra.recognizers.home_marker)",
                {"home_marker", "recognizer"}
            );
        }

        TEST_CASE("Aliasing the namespace or a sub-table is rejected")
        {
            auto const surface = buildSurface();

            SUBCASE("binding the bare namespace to a local")
            {
                expectRejected(
                    surface,
                    "local u = umbra\nreturn u.recognizers.battle",
                    {"umbra"}
                );
            }
            SUBCASE("binding a sub-table to a local")
            {
                expectRejected(
                    surface,
                    "local r = umbra.recognizers\nreturn r.battle",
                    {"umbra"}
                );
            }
            SUBCASE("passing the namespace as an argument")
            {
                expectRejected(
                    surface,
                    "for _ in pairs(umbra) do end\nreturn 0",
                    {"umbra"}
                );
            }
            SUBCASE("returning the namespace")
            {
                expectRejected(surface, "return umbra", {"umbra"});
            }
        }

        TEST_CASE("Reaching the namespace through the _G global alias is rejected")
        {
            auto const surface = buildSurface();

            // `_G` is Luau's reflexive handle to the whole global table. Every one
            // of these chains reaches `umbra` WITHOUT a literal umbra root, so the
            // rest of the validator would classify them as unrelated code; the _G
            // rejection is what closes that alias door. This is the pre-VM twin of
            // installSandbox niling `_G` on the task thread. Each is pinned to the
            // '_G' offender so a chain that happened to fail for another reason
            // would not pass this test.
            SUBCASE("calling a verb through the _G alias")
            {
                expectRejected(surface, "return _G.umbra:capture()", {"_G"});
            }
            SUBCASE("aliasing the namespace off _G")
            {
                expectRejected(
                    surface,
                    "local u = _G.umbra\nreturn u.recognizers.battle",
                    {"_G"}
                );
            }
            SUBCASE("dynamically indexing a resource table off _G")
            {
                expectRejected(
                    surface,
                    "local n = 'bat' .. 'tle'\nreturn _G.umbra.recognizers[n]",
                    {"_G"}
                );
            }
            SUBCASE("iterating a resource table off _G")
            {
                expectRejected(
                    surface,
                    "for k in pairs(_G.umbra.recognizers) do end\nreturn 0",
                    {"_G"}
                );
            }
            SUBCASE("computed-indexing the namespace off _G")
            {
                expectRejected(surface, "return _G['umbra']", {"_G"});
            }
            SUBCASE("rawget past _G to the namespace")
            {
                expectRejected(surface, "return rawget(_G, 'umbra')", {"_G"});
            }
            SUBCASE("a missing resource off _G is rejected pre-VM, not left as runtime nil")
            {
                // Without the _G rejection this would sail through validation and
                // become a runtime nil, defeating the pre-VM missing-resource
                // promise; the alias door is closed before the leaf is ever reached.
                expectRejected(
                    surface,
                    "return _G.umbra.recognizers.does_not_exist",
                    {"_G"}
                );
            }
        }

        TEST_CASE("Computed indexing into a resource table is rejected")
        {
            auto const surface = buildSurface();
            expectRejected(
                surface,
                "local name = 'battle'\nreturn umbra.recognizers[name]",
                {"umbra", "two-level"}
            );
        }

        TEST_CASE("Indexing past a two-level handle literal is rejected")
        {
            auto const surface = buildSurface();
            // umbra.recognizers.battle is a valid handle, but the only permitted
            // spelling is the two-level literal itself; reading a field off it is
            // a deeper chain and rejected as the wrong shape.
            expectRejected(
                surface,
                "return umbra.recognizers.battle.foo",
                {"umbra", "two-level"}
            );
        }

        TEST_CASE("An unknown resource sub-namespace is rejected")
        {
            auto const surface = buildSurface();
            expectRejected(
                surface,
                "return umbra.templates.foo",
                {"umbra.templates", "namespace"}
            );
        }

        TEST_CASE("The dot-call spelling of a verb is rejected")
        {
            auto const surface = buildSurface();
            // Only the colon method call umbra:capture() is canonical; the dot
            // form touches umbra through a one-level field access.
            expectRejected(
                surface,
                "return umbra.capture(umbra)",
                {"umbra"}
            );
        }

        TEST_CASE("A syntax error is rejected as an invalid resource")
        {
            auto const surface = buildSurface();
            expectRejected(surface, "if x then", {"syntax error"});
        }
    }
}

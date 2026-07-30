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
                at::elementId(k_anchorId),
                "home_marker",
                annotation::AnnotationType::PageAnchor,
                at::pixelRect(0, 0, 1, 1),
                at::pixelRect(0, 0, 4, 4)
            ));
            recognizers.push_back(at::recognizer(
                fingerprint,
                at::elementId(k_dailyId),
                "daily_button",
                annotation::AnnotationType::ActionTarget,
                at::pixelRect(1, 1, 1, 1),
                at::pixelRect(0, 0, 4, 4),
                {pageId}
            ));
            recognizers.push_back(at::recognizer(
                fingerprint,
                at::elementId(k_battleId),
                "battle",
                annotation::AnnotationType::ActionTarget,
                at::pixelRect(2, 2, 1, 1),
                at::pixelRect(0, 0, 4, 4),
                {pageId}
            ));

            auto const catalog = at::catalog(
                fingerprint,
                std::move(recognizers),
                {at::page(pageId, "home", {at::elementId(k_anchorId)})}
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

            // A task the way one is written now that the framework owns policy:
            // an interrupt declared against a page, a step around a wait, and
            // the uf root appearing only as the two-level resource literals --
            // inside a declaration field, inside a ctx argument, inside a handle
            // method's argument, and once repeated.
            constexpr std::string_view source = R"lua(
                local popup = task.interrupt {
                    id = "battle_prompt",
                    when = uf.pages.home,
                    handle = function(ctx, cycle)
                        local close = cycle:find(uf.recognizers.battle)
                        if close ~= nil then
                            cycle:click(close)
                        end
                    end,
                }

                return task.define {
                    interrupts = { popup },
                    run = function(ctx)
                        ctx:step("daily", function()
                            ctx:retry({ attempts = 2 }, function()
                                ctx:wait_for_page(
                                    uf.pages.home,
                                    { timeout_ms = 1000 },
                                    function(home)
                                        local hit =
                                            home:find(uf.recognizers.daily_button)
                                        if hit ~= nil then
                                            home:click(hit)
                                        end
                                    end
                                )
                            end)
                        end)
                    end,
                }
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

        TEST_CASE("The retired root spelling resolves nothing and policing follows uf")
        {
            auto const surface = buildSurface();

            // `umbra` was this root's spelling before 2026-07-29. Nothing answers
            // to it now: it is an ordinary unknown global, so the validator walks
            // past it without resolving a resource -- the whole resource closure
            // keys on `uf` alone. Reverting the validator's k_namespace flips
            // both reports below and reddens this case. What the installer
            // registers as the global is pinned separately, on a real task VM,
            // by the binding suite's retired-root case.
            auto const retired = validateScriptResources(
                "return uf.pages.home ~= nil and umbra.recognizers.battle",
                "retired-root",
                surface
            );
            REQUIRE(retired.has_value());
            CHECK(retired->recognizers.empty());
            CHECK(retired->pages == std::vector<std::string>{"home"});

            auto const current = validateScriptResources(
                "return uf.recognizers.battle",
                "current-root",
                surface
            );
            REQUIRE(current.has_value());
            CHECK(current->recognizers == std::vector<std::string>{"battle"});
        }

        TEST_CASE("A method call on the uf root is rejected: there are no verbs")
        {
            auto const surface = buildSurface();

            // The root carries data alone now, so a colon call on it names
            // nothing that could exist. It is rejected here rather than left to
            // fail as a runtime nil call -- the same reason a misspelled error
            // kind is rejected.
            SUBCASE("a verb that used to exist")
            {
                expectRejected(
                    surface,
                    "return uf:cycle_open()",
                    {"uf", "two-level"}
                );
            }
            SUBCASE("a verb that never existed")
            {
                expectRejected(
                    surface,
                    "uf:frobnicate()\nreturn 0",
                    {"uf", "two-level"}
                );
            }
        }

        TEST_CASE("The framework context is not the uf namespace and is not policed")
        {
            auto const surface = buildSurface();

            // ctx is a project global the framework published; it exposes no
            // resource name, so there is nothing here for a closure pass to
            // resolve and it must not be mistaken for a uf reference. The
            // resource literals inside its arguments are still enumerated, which
            // is what the report below proves.
            auto const report = validateScriptResources(
                "local d = ctx:deadline(1000)\n"
                "local r = ctx:random(1, 6)\n"
                "ctx:wait_for_page(uf.pages.home, {}, function() end)\n"
                "return r",
                "context-calls",
                surface
            );
            REQUIRE(report.has_value());
            CHECK(report->recognizers.empty());
            CHECK(report->pages == std::vector<std::string>{"home"});
        }

        TEST_CASE("An error-kind literal validates and enumerates no resource")
        {
            auto const surface = buildSurface();

            // uf.errors.<kind> is the third approved two-level literal. It
            // names host vocabulary rather than a project resource, so it must
            // pass the namespace gate without appearing in the resource closure.
            auto const report = validateScriptResources(
                "local ok, err = ctx:try(function() end)\n"
                "if not ok and err.kind == uf.errors.stale_observation then\n"
                "    return 1\n"
                "end\n"
                "return 0",
                "errors",
                surface
            );
            REQUIRE(report.has_value());
            CHECK(report->recognizers.empty());
            CHECK(report->pages.empty());
        }

        TEST_CASE("A misspelled error kind is rejected before the VM exists")
        {
            auto const surface = buildSurface();
            // Left as a runtime nil this would make every comparison against it
            // silently false, which is exactly the failure the pre-VM pass closes
            // for a missing recognizer.
            expectRejected(
                surface,
                "return uf.errors.time_out",
                {"time_out", "error kind"}
            );
        }

        TEST_CASE("A reference to a missing recognizer is rejected by name")
        {
            auto const surface = buildSurface();
            expectRejected(
                surface,
                "return ctx:cycle_find(cycle, uf.recognizers.does_not_exist)",
                {"does_not_exist", "recognizer"}
            );
        }

        TEST_CASE("A reference to a missing page is rejected by name")
        {
            auto const surface = buildSurface();
            expectRejected(
                surface,
                "local p = uf.pages.does_not_exist\nreturn p",
                {"does_not_exist", "page"}
            );
        }

        TEST_CASE("A page anchor is not exposed under uf.recognizers")
        {
            auto const surface = buildSurface();
            // home_marker is a real catalog recognizer, but a page anchor, so it
            // is never a findable handle; the validator rejects it as missing.
            expectRejected(
                surface,
                "return ctx:cycle_find(cycle, uf.recognizers.home_marker)",
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
                    "local u = uf\nreturn u.recognizers.battle",
                    {"uf"}
                );
            }
            SUBCASE("binding a sub-table to a local")
            {
                expectRejected(
                    surface,
                    "local r = uf.recognizers\nreturn r.battle",
                    {"uf"}
                );
            }
            SUBCASE("passing the namespace as an argument")
            {
                expectRejected(
                    surface,
                    "for _ in pairs(uf) do end\nreturn 0",
                    {"uf"}
                );
            }
            SUBCASE("returning the namespace")
            {
                expectRejected(surface, "return uf", {"uf"});
            }
        }

        TEST_CASE("Reaching the namespace through the _G global alias is rejected")
        {
            auto const surface = buildSurface();

            // `_G` is Luau's reflexive handle to the whole global table. Every one
            // of these chains reaches `uf` WITHOUT a literal uf root, so the
            // rest of the validator would classify them as unrelated code; the _G
            // rejection is what closes that alias door. This is the pre-VM twin of
            // installSandbox niling `_G` on the task thread. Each is pinned to the
            // '_G' offender so a chain that happened to fail for another reason
            // would not pass this test.
            SUBCASE("indexing the namespace through the _G alias")
            {
                expectRejected(surface, "return _G.uf.pages.home", {"_G"});
            }
            SUBCASE("aliasing the namespace off _G")
            {
                expectRejected(
                    surface,
                    "local u = _G.uf\nreturn u.recognizers.battle",
                    {"_G"}
                );
            }
            SUBCASE("dynamically indexing a resource table off _G")
            {
                expectRejected(
                    surface,
                    "local n = 'bat' .. 'tle'\nreturn _G.uf.recognizers[n]",
                    {"_G"}
                );
            }
            SUBCASE("iterating a resource table off _G")
            {
                expectRejected(
                    surface,
                    "for k in pairs(_G.uf.recognizers) do end\nreturn 0",
                    {"_G"}
                );
            }
            SUBCASE("computed-indexing the namespace off _G")
            {
                expectRejected(surface, "return _G['uf']", {"_G"});
            }
            SUBCASE("rawget past _G to the namespace")
            {
                expectRejected(surface, "return rawget(_G, 'uf')", {"_G"});
            }
            SUBCASE("a missing resource off _G is rejected pre-VM, not left as runtime nil")
            {
                // Without the _G rejection this would sail through validation and
                // become a runtime nil, defeating the pre-VM missing-resource
                // promise; the alias door is closed before the leaf is ever reached.
                expectRejected(
                    surface,
                    "return _G.uf.recognizers.does_not_exist",
                    {"_G"}
                );
            }
        }

        TEST_CASE("Computed indexing into a resource table is rejected")
        {
            auto const surface = buildSurface();
            expectRejected(
                surface,
                "local name = 'battle'\nreturn uf.recognizers[name]",
                {"uf", "two-level"}
            );
        }

        TEST_CASE("Indexing past a two-level handle literal is rejected")
        {
            auto const surface = buildSurface();
            // uf.recognizers.battle is a valid handle, but the only permitted
            // spelling is the two-level literal itself; reading a field off it is
            // a deeper chain and rejected as the wrong shape.
            expectRejected(
                surface,
                "return uf.recognizers.battle.foo",
                {"uf", "two-level"}
            );
        }

        TEST_CASE("An unknown resource sub-namespace is rejected")
        {
            auto const surface = buildSurface();
            expectRejected(
                surface,
                "return uf.templates.foo",
                {"uf.templates", "namespace"}
            );
        }

        TEST_CASE("Calling a one-level field of the namespace is rejected")
        {
            auto const surface = buildSurface();
            // uf.recognizers as a value is a one-level field access, and it is
            // rejected whether it is called, aliased, or returned.
            expectRejected(
                surface,
                "return uf.recognizers(uf)",
                {"uf"}
            );
        }

        TEST_CASE("A syntax error is rejected as an invalid resource")
        {
            auto const surface = buildSurface();
            expectRejected(surface, "if x then", {"syntax error"});
        }
    }
}

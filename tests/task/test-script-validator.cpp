#include "../annotation/test-helpers.hpp"

#include <task/capability-surface.hpp>
#include <task/script-validator.hpp>

#include <annotation/capabilities.hpp>
#include <annotation/catalog.hpp>

#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <optional>
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

        // The same minimal catalog the binding tests use: one identify-only
        // element (never findable), two interactive ones (daily_button,
        // battle), and one page (home). The surface built from it is exactly
        // what the script sees, so the validator resolves against the identical
        // name set.
        auto buildSurface() -> CapabilitySurface
        {
            auto const fingerprint = at::fingerprint();
            auto const pageId      = at::pageId(k_pageId);
            auto const anchorId    = at::elementId(k_anchorId);
            auto const dailyId     = at::elementId(k_dailyId);
            auto const battleId    = at::elementId(k_battleId);

            auto elements = std::vector<annotation::CompiledElement>{};
            elements.push_back(at::element(
                fingerprint,
                anchorId,
                "home_marker",
                at::capabilities(annotation::Identify{}),
                at::pixelRect(0, 0, 4, 4),
                {at::compiledAppearance("default", at::pixelRect(0, 0, 1, 1))}
            ));
            elements.push_back(at::element(
                fingerprint,
                dailyId,
                "daily_button",
                at::capabilities(std::nullopt, annotation::Interact{}),
                at::pixelRect(0, 0, 4, 4),
                {at::compiledAppearance("default", at::pixelRect(1, 1, 1, 1))}
            ));
            elements.push_back(at::element(
                fingerprint,
                battleId,
                "battle",
                at::capabilities(std::nullopt, annotation::Interact{}),
                at::pixelRect(0, 0, 4, 4),
                {at::compiledAppearance("default", at::pixelRect(2, 2, 1, 1))}
            ));

            auto const catalog = at::catalog(
                fingerprint,
                std::move(elements),
                {at::page(pageId, "home")},
                {
                    at::reference(pageId, anchorId, at::identifiesAs()),
                    at::reference(pageId, dailyId, at::interacts()),
                    at::reference(pageId, battleId, at::interacts()),
                }
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
                        local close = cycle:find(uf.elements.battle)
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
                                            home:find(uf.elements.daily_button)
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
                report->elements
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
            CHECK(report->elements.empty());
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
                "return uf.pages.home ~= nil and umbra.elements.battle",
                "retired-root",
                surface
            );
            REQUIRE(retired.has_value());
            CHECK(retired->elements.empty());
            CHECK(retired->pages == std::vector<std::string>{"home"});

            auto const current = validateScriptResources(
                "return uf.elements.battle",
                "current-root",
                surface
            );
            REQUIRE(current.has_value());
            CHECK(current->elements == std::vector<std::string>{"battle"});
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
            CHECK(report->elements.empty());
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
            CHECK(report->elements.empty());
            CHECK(report->pages.empty());
        }

        TEST_CASE("A misspelled error kind is rejected before the VM exists")
        {
            auto const surface = buildSurface();
            // Left as a runtime nil this would make every comparison against it
            // silently false, which is exactly the failure the pre-VM pass closes
            // for a missing element.
            expectRejected(
                surface,
                "return uf.errors.time_out",
                {"time_out", "error kind"}
            );
        }

        TEST_CASE("A reference to a missing element is rejected by name")
        {
            auto const surface = buildSurface();
            expectRejected(
                surface,
                "return ctx:cycle_find(cycle, uf.elements.does_not_exist)",
                {"does_not_exist", "element"}
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

        TEST_CASE("A page anchor is not exposed under uf.elements")
        {
            auto const surface = buildSurface();
            // home_marker is a real catalog element, but a page anchor, so it
            // is never a findable handle; the validator rejects it as missing.
            expectRejected(
                surface,
                "return ctx:cycle_find(cycle, uf.elements.home_marker)",
                {"home_marker", "element"}
            );
        }

        TEST_CASE("Aliasing the namespace or a sub-table is rejected")
        {
            auto const surface = buildSurface();

            SUBCASE("binding the bare namespace to a local")
            {
                expectRejected(
                    surface,
                    "local u = uf\nreturn u.elements.battle",
                    {"uf"}
                );
            }
            SUBCASE("binding a sub-table to a local")
            {
                expectRejected(
                    surface,
                    "local r = uf.elements\nreturn r.battle",
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
                    "local u = _G.uf\nreturn u.elements.battle",
                    {"_G"}
                );
            }
            SUBCASE("dynamically indexing a resource table off _G")
            {
                expectRejected(
                    surface,
                    "local n = 'bat' .. 'tle'\nreturn _G.uf.elements[n]",
                    {"_G"}
                );
            }
            SUBCASE("iterating a resource table off _G")
            {
                expectRejected(
                    surface,
                    "for k in pairs(_G.uf.elements) do end\nreturn 0",
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
                    "return _G.uf.elements.does_not_exist",
                    {"_G"}
                );
            }
        }

        TEST_CASE("Computed indexing into a resource table is rejected")
        {
            auto const surface = buildSurface();
            expectRejected(
                surface,
                "local name = 'battle'\nreturn uf.elements[name]",
                {"uf", "two-level"}
            );
        }

        TEST_CASE("Indexing past a two-level handle literal is rejected")
        {
            auto const surface = buildSurface();
            // uf.elements.battle is a valid handle, but the only permitted
            // spelling is the two-level literal itself; reading a field off it is
            // a deeper chain and rejected as the wrong shape.
            expectRejected(
                surface,
                "return uf.elements.battle.foo",
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
            // uf.elements as a value is a one-level field access, and it is
            // rejected whether it is called, aliased, or returned.
            expectRejected(
                surface,
                "return uf.elements(uf)",
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

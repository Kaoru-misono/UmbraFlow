#include <task/page-model-file.hpp>
#include <task/script-validator.hpp>

#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <vector>

namespace uf::task
{
    namespace
    {
        // The page model the validator resolves against: three elements and one
        // page, written the way a project file writes them.
        //
        // IT IS THE PROJECT FILE AND NO LONGER A CATALOG. Which names a script may
        // spell is a fact about that file now
        // (docs/plans/2026-07-31-script-owned-page-model.md 6), so the fixture is
        // the text rather than a compiled catalog -- and every name the file
        // declares is resolvable, where the catalog surface exposed only elements
        // declaring interact. `home_marker` is here to prove exactly that: an
        // element that only identifies is a name a script may write.
        constexpr auto k_pageModel = std::string_view{R"toml(
schema = "umbraflow-project/l2-v1"
base_resolution = [4, 4]
base_dpi = [96, 96]

[[element]]
name = "home_marker"
capabilities = ["identify"]
rect = [0, 0, 4, 4]

[[element]]
name = "daily_button"
capabilities = ["interact"]
rect = [0, 0, 4, 4]

[element.extra]
name = "extra_alias"

[[element]]
name = "battle"
capabilities = ["interact"]
rect = [0, 0, 4, 4]

[[appearance]]
element = "battle"
name = "default"
source = "assets/templates/battle.png"
threshold = 9000

[[page]]
name = "home"

[[reference]]
page = "home"
element = "battle"
holding = "owned"
exercised = ["interact"]
)toml"};

        auto buildModel() -> PageModelFacts
        {
            auto model = parsePageModelFacts(k_pageModel);
            REQUIRE(model.has_value());
            return *std::move(model);
        }

        // Validates `source` and asserts it was rejected as InvalidResource whose
        // message contains every `needle`. Substring assertions pin the failure to
        // the specific offending reference, not merely "some error", so a change
        // that rejects for the wrong reason is caught.
        auto expectRejected(
            PageModelFacts const& model,
            std::string_view source,
            std::vector<std::string_view> needles
        ) -> void
        {
            auto const report = validateScriptResources(source, "task", model);
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

        TEST_CASE("A page model that names one thing twice is refused")
        {
            // A duplicate would leave a literal resolving against two different
            // rows, which is a model with no single answer about its own pixels.
            // The refusal names the offender, because the file is hand-edited.
            auto const duplicated = std::string{k_pageModel}
                + "\n[[element]]\nname = \"battle\"\n"
                  "capabilities = [\"interact\"]\nrect = [0, 0, 4, 4]\n";
            auto const refused = parsePageModelFacts(duplicated);
            REQUIRE_FALSE(refused.has_value());
            CHECK(
                automationErrorKind(refused.error())
                == AutomationErrorKind::InvalidResource
            );
            CHECK(std::string{refused.error().message()}.contains("battle"));

            // The control: the same file without the extra section loads.
            CHECK(parsePageModelFacts(k_pageModel).has_value());
        }

        TEST_CASE("A page model that states no geometry is refused")
        {
            // The fingerprint is what the engine's compatibility check compares
            // a live measurement against, so a model that states none has
            // nothing to be compared to. Guessing one would defeat the check
            // silently, which is why the absence is a refusal and not a default.
            auto       without = std::string{k_pageModel};
            auto const at      = without.find("base_resolution");
            REQUIRE(at != std::string::npos);
            without.erase(at, without.find('\n', at) - at);

            auto const refused = parsePageModelFacts(without);
            REQUIRE_FALSE(refused.has_value());
            CHECK(
                automationErrorKind(refused.error())
                == AutomationErrorKind::InvalidResource
            );
            CHECK(
                std::string{refused.error().message()}.contains("base_resolution")
            );

            // And the geometry it does state is the geometry it reports, rather
            // than one this reader chose.
            auto const model = parsePageModelFacts(k_pageModel);
            REQUIRE(model.has_value());
            CHECK(model->fingerprint.width() == 4);
            CHECK(model->fingerprint.height() == 4);
            CHECK(model->fingerprint.dpiX() == 96);
        }

        TEST_CASE("A canonical task script validates and enumerates its references")
        {
            auto const model = buildModel();

            // A task the way one is written now that the framework owns policy:
            // a step around a wait, and the uf root appearing only as the
            // two-level resource literals -- inside a table field, inside a ctx
            // argument, and once repeated.
            constexpr std::string_view source = R"lua(
                return task.define {
                    run = function(ctx)
                        ctx:step("daily", function()
                            ctx:retry({ attempts = 2 }, function()
                                observe.wait_until(ctx, {
                                    page = uf.pages.home,
                                    element = uf.elements.daily_button,
                                    consecutive = 2,
                                    timeout_ms = 1000,
                                    interval_ms = 100,
                                })
                                local other = uf.elements.battle
                                return other
                            end)
                        end)
                    end,
                }
            )lua";

            auto const report = validateScriptResources(source, "daily", model);
            REQUIRE(report.has_value());
            CHECK(
                report->elements
                == std::vector<std::string>{"battle", "daily_button"}
            );
            CHECK(report->pages == std::vector<std::string>{"home"});
        }

        TEST_CASE("A script touching no resources validates with an empty report")
        {
            auto const model = buildModel();

            auto const report =
                validateScriptResources("local x = 1 + 2\nreturn x", "noop", model);
            REQUIRE(report.has_value());
            CHECK(report->elements.empty());
            CHECK(report->pages.empty());
        }

        TEST_CASE("The retired root spelling resolves nothing and policing follows uf")
        {
            auto const model = buildModel();

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
                model
            );
            REQUIRE(retired.has_value());
            CHECK(retired->elements.empty());
            CHECK(retired->pages == std::vector<std::string>{"home"});

            auto const current = validateScriptResources(
                "return uf.elements.battle",
                "current-root",
                model
            );
            REQUIRE(current.has_value());
            CHECK(current->elements == std::vector<std::string>{"battle"});
        }

        TEST_CASE("A method call on the uf root is rejected: there are no verbs")
        {
            auto const model = buildModel();

            // The root carries data alone now, so a colon call on it names
            // nothing that could exist. It is rejected here rather than left to
            // fail as a runtime nil call -- the same reason a misspelled error
            // kind is rejected.
            SUBCASE("a verb that used to exist")
            {
                expectRejected(
                    model,
                    "return uf:cycle_open()",
                    {"uf", "two-level"}
                );
            }
            SUBCASE("a verb that never existed")
            {
                expectRejected(
                    model,
                    "uf:frobnicate()\nreturn 0",
                    {"uf", "two-level"}
                );
            }
        }

        TEST_CASE("The framework context is not the uf namespace and is not policed")
        {
            auto const model = buildModel();

            // ctx is a project global the framework published; it exposes no
            // resource name, so there is nothing here for a closure pass to
            // resolve and it must not be mistaken for a uf reference. The
            // resource literals inside its arguments are still enumerated, which
            // is what the report below proves.
            auto const report = validateScriptResources(
                "local d = ctx:deadline(1000)\n"
                "local r = ctx:random(1, 6)\n"
                "observe.wait_until(ctx, { page = uf.pages.home })\n"
                "return r",
                "context-calls",
                model
            );
            REQUIRE(report.has_value());
            CHECK(report->elements.empty());
            CHECK(report->pages == std::vector<std::string>{"home"});
        }

        TEST_CASE("An error-kind literal validates and enumerates no resource")
        {
            auto const model = buildModel();

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
                model
            );
            REQUIRE(report.has_value());
            CHECK(report->elements.empty());
            CHECK(report->pages.empty());
        }

        TEST_CASE("A misspelled error kind is rejected before the VM exists")
        {
            auto const model = buildModel();
            // Left as a runtime nil this would make every comparison against it
            // silently false, which is exactly the failure the pre-VM pass closes
            // for a missing element.
            expectRejected(
                model,
                "return uf.errors.time_out",
                {"time_out", "error kind"}
            );
        }

        TEST_CASE("A reference to a missing element is rejected by name")
        {
            auto const model = buildModel();
            expectRejected(
                model,
                "return uf.elements.does_not_exist",
                {"does_not_exist", "element"}
            );
        }

        TEST_CASE("A reference to a missing page is rejected by name")
        {
            auto const model = buildModel();
            expectRejected(
                model,
                "local p = uf.pages.does_not_exist\nreturn p",
                {"does_not_exist", "page"}
            );
        }

        TEST_CASE("Every element the project file declares is a name a script may write")
        {
            auto const model = buildModel();

            // home_marker only identifies. The retired capability surface exposed
            // element handles for interactive elements alone, so this name used to
            // be REJECTED here; the project file draws no such line, and neither
            // does this pass. The `name` under [element.extra] is not one of them:
            // a project's own fields are its business and never a resource.
            auto const report = validateScriptResources(
                "return uf.elements.home_marker",
                "anchor",
                model
            );
            REQUIRE(report.has_value());
            CHECK(report->elements == std::vector<std::string>{"home_marker"});

            // A project's own field under `extra` is never a resource, however
            // exactly it copies a key this layer owns. The fixture's element
            // carries `name = "extra_alias"` there, and the reader must not have
            // read it as the element's own.
            expectRejected(
                model,
                "return uf.elements.extra_alias",
                {"extra_alias", "declares no element"}
            );
        }

        TEST_CASE("Aliasing the namespace or a sub-table is rejected")
        {
            auto const model = buildModel();

            SUBCASE("binding the bare namespace to a local")
            {
                expectRejected(
                    model,
                    "local u = uf\nreturn u.elements.battle",
                    {"uf"}
                );
            }
            SUBCASE("binding a sub-table to a local")
            {
                expectRejected(
                    model,
                    "local r = uf.elements\nreturn r.battle",
                    {"uf"}
                );
            }
            SUBCASE("passing the namespace as an argument")
            {
                expectRejected(
                    model,
                    "for _ in pairs(uf) do end\nreturn 0",
                    {"uf"}
                );
            }
            SUBCASE("returning the namespace")
            {
                expectRejected(model, "return uf", {"uf"});
            }
        }

        TEST_CASE("Reaching the namespace through the _G global alias is rejected")
        {
            auto const model = buildModel();

            // `_G` is Luau's reflexive handle to the whole global table. Every one
            // of these chains reaches `uf` WITHOUT a literal uf root, so the
            // rest of the validator would classify them as unrelated code; the _G
            // rejection is what closes that alias door. This is the pre-VM twin of
            // installSandbox niling `_G` on the task thread. Each is pinned to the
            // '_G' offender so a chain that happened to fail for another reason
            // would not pass this test.
            SUBCASE("indexing the namespace through the _G alias")
            {
                expectRejected(model, "return _G.uf.pages.home", {"_G"});
            }
            SUBCASE("aliasing the namespace off _G")
            {
                expectRejected(
                    model,
                    "local u = _G.uf\nreturn u.elements.battle",
                    {"_G"}
                );
            }
            SUBCASE("dynamically indexing a resource table off _G")
            {
                expectRejected(
                    model,
                    "local n = 'bat' .. 'tle'\nreturn _G.uf.elements[n]",
                    {"_G"}
                );
            }
            SUBCASE("iterating a resource table off _G")
            {
                expectRejected(
                    model,
                    "for k in pairs(_G.uf.elements) do end\nreturn 0",
                    {"_G"}
                );
            }
            SUBCASE("computed-indexing the namespace off _G")
            {
                expectRejected(model, "return _G['uf']", {"_G"});
            }
            SUBCASE("rawget past _G to the namespace")
            {
                expectRejected(model, "return rawget(_G, 'uf')", {"_G"});
            }
            SUBCASE("a missing resource off _G is rejected pre-VM, not left as runtime nil")
            {
                // Without the _G rejection this would sail through validation and
                // become a runtime nil, defeating the pre-VM missing-resource
                // promise; the alias door is closed before the leaf is ever reached.
                expectRejected(
                    model,
                    "return _G.uf.elements.does_not_exist",
                    {"_G"}
                );
            }
        }

        TEST_CASE("Computed indexing into a resource table is rejected")
        {
            auto const model = buildModel();
            expectRejected(
                model,
                "local name = 'battle'\nreturn uf.elements[name]",
                {"uf", "two-level"}
            );
        }

        TEST_CASE("Indexing past a two-level handle literal is rejected")
        {
            auto const model = buildModel();
            // uf.elements.battle is a valid handle, but the only permitted
            // spelling is the two-level literal itself; reading a field off it is
            // a deeper chain and rejected as the wrong shape.
            expectRejected(
                model,
                "return uf.elements.battle.foo",
                {"uf", "two-level"}
            );
        }

        TEST_CASE("An unknown resource sub-namespace is rejected")
        {
            auto const model = buildModel();
            expectRejected(
                model,
                "return uf.templates.foo",
                {"uf.templates", "namespace"}
            );
        }

        TEST_CASE("Calling a one-level field of the namespace is rejected")
        {
            auto const model = buildModel();
            // uf.elements as a value is a one-level field access, and it is
            // rejected whether it is called, aliased, or returned.
            expectRejected(
                model,
                "return uf.elements(uf)",
                {"uf"}
            );
        }

        TEST_CASE("A syntax error is rejected as an invalid resource")
        {
            auto const model = buildModel();
            expectRejected(model, "if x then", {"syntax error"});
        }
    }
}

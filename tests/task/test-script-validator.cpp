#include <task/page-model-file.hpp>
#include <task/script-validator.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uf::task
{
    namespace
    {
        // The runtime model envelope the validator resolves against: three targets
        // and one surface, written the way a project file writes them. Which names
        // a script may spell is a fact about that file
        // may spell is a fact about that file
        // (docs/plans/2026-08-09-runtime-model-contract.md 7), so the fixture is
        // the text rather than a compiled catalog. `home_marker` is here because
        // a target that only identifies is still a name a script may write.
        constexpr auto k_pageModel = std::string_view{R"toml(
schema_version = 1
base_resolution = [4, 4]
base_dpi = [96, 96]

[[target]]
id = "home_marker"

[[target]]
id = "daily_button"

[target.extra]
id = "extra_alias"

[[target]]
id = "battle"

[[locator]]
id = "battle.default"
target = "battle"
kind = "template"
source = "assets/templates/battle.png"
threshold = 9000

[[surface]]
id = "home"

[[binding]]
surface = "home"
target = "battle"

[binding.identity]
locator = "battle.default"
polarity = "required"

[binding.actions.click]
locator = "battle.default"
)toml"};

        auto buildModel() -> RuntimeModelEnvelope
        {
            auto model = parseRuntimeModelEnvelope(k_pageModel);
            REQUIRE(model.has_value());
            return *std::move(model);
        }

        // Validates `source` and asserts it was rejected as InvalidResource whose
        // message contains every `needle` -- pinning the failure to the offending
        // resource access, so a change that rejects for the wrong reason is caught.
        auto expectRejected(
            RuntimeModelEnvelope const& model,
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

        TEST_CASE("A runtime model that names one thing twice is refused")
        {
            // A duplicate leaves a literal resolving against two different rows,
            // a model with no single answer about its own pixels. The refusal
            // names the offender, because the file is hand-edited.
            auto const duplicated = std::string{k_pageModel}
                + "\n[[target]]\nid = \"battle\"\n";
            auto const refused = parseRuntimeModelEnvelope(duplicated);
            REQUIRE_FALSE(refused.has_value());
            CHECK(
                automationErrorKind(refused.error())
                == AutomationErrorKind::InvalidResource
            );
            CHECK(std::string{refused.error().message()}.contains("battle"));

            // The control: the same file without the extra section loads.
            CHECK(parseRuntimeModelEnvelope(k_pageModel).has_value());

            auto const duplicatedSurface = std::string{k_pageModel}
                + "\n[[surface]]\nid = \"home\"\n";
            auto const refusedSurface = parseRuntimeModelEnvelope(duplicatedSurface);
            REQUIRE_FALSE(refusedSurface.has_value());
            CHECK(
                std::string{refusedSurface.error().message()}.contains("home")
            );
        }

        TEST_CASE("A runtime model without its envelope version is refused")
        {
            auto without = std::string{k_pageModel};
            auto const at = without.find("schema_version");
            REQUIRE(at != std::string::npos);
            without.erase(at, without.find('\n', at) - at);

            auto const refused = parseRuntimeModelEnvelope(without);
            REQUIRE_FALSE(refused.has_value());
            CHECK(
                std::string{refused.error().message()}.contains("schema_version")
            );
        }

        TEST_CASE("A runtime model that states no geometry is refused")
        {
            // The fingerprint is what the engine's compatibility check compares
            // a live measurement against, so guessing one would defeat the check
            // silently -- hence a refusal rather than a default.
            auto       without = std::string{k_pageModel};
            auto const at      = without.find("base_resolution");
            REQUIRE(at != std::string::npos);
            without.erase(at, without.find('\n', at) - at);

            auto const refused = parseRuntimeModelEnvelope(without);
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
            auto const model = parseRuntimeModelEnvelope(k_pageModel);
            REQUIRE(model.has_value());
            CHECK(model->schemaVersion == 1);
            CHECK(model->fingerprint.width() == 4);
            CHECK(model->fingerprint.height() == 4);
            CHECK(model->fingerprint.dpiX() == 96);
        }

        TEST_CASE("A runtime model is identified by its bytes and not by this reading")
        {
            // `run.started` stamps this hash, and a replay checker refuses a trace
            // whose model is no longer the one on disk. That refusal is only worth
            // anything if the hash covers the whole file: nearly everything in a
            // runtime model -- thresholds, bindings, transitions, the offline
            // claims -- is layer two's and skipped entirely by the scan above.
            auto const model = parseRuntimeModelEnvelope(k_pageModel);
            REQUIRE(model.has_value());

            auto const expected = sha256(std::as_bytes(std::span{k_pageModel}));
            REQUIRE(expected.has_value());
            CHECK(model->contentHash == *expected);

            // An edge is a section this reader does not recognise and does not
            // record, so it changes nothing the facts carry -- and must still
            // change the identity.
            auto const edited = std::string{k_pageModel}
                + "\n[[edge]]\nfrom = \"home\"\nto = [\"home\"]\n"
                  "via = \"key\"\nvia_key = \"E\"\nkind = \"navigate\"\n";
            auto const after = parseRuntimeModelEnvelope(edited);
            REQUIRE(after.has_value());

            CHECK(after->targetIds == model->targetIds);
            CHECK(after->surfaceIds == model->surfaceIds);
            CHECK(after->fingerprint == model->fingerprint);
            CHECK(after->contentHash != model->contentHash);
        }

        TEST_CASE("A canonical task script validates and enumerates its references")
        {
            auto const model = buildModel();

            // A task as one is written now the framework owns policy: a step
            // around a wait, with the uf root appearing only as two-level
            // literals -- in a table field, in a ctx argument, and once repeated.
            constexpr std::string_view source = R"lua(
                return task.define {
                    run = function(ctx)
                        ctx:step("daily", function()
                            ctx:retry({ attempts = 2 }, function()
                                observe.wait_until(ctx, {
                                    surface = uf.surfaces.home,
                                    target = uf.targets.daily_button,
                                    consecutive = 2,
                                    timeout_ms = 1000,
                                    interval_ms = 100,
                                })
                                local other = uf.targets.battle
                                return other
                            end)
                        end)
                    end,
                }
            )lua";

            auto const report = validateScriptResources(source, "daily", model);
            REQUIRE(report.has_value());
            CHECK(
                report->targets
                == std::vector<std::string>{"battle", "daily_button"}
            );
            CHECK(report->surfaces == std::vector<std::string>{"home"});
        }

        TEST_CASE("A script touching no resources validates with an empty report")
        {
            auto const model = buildModel();

            auto const report =
                validateScriptResources("local x = 1 + 2\nreturn x", "noop", model);
            REQUIRE(report.has_value());
            CHECK(report->targets.empty());
            CHECK(report->surfaces.empty());
        }

        TEST_CASE("The retired root spelling resolves nothing and policing follows uf")
        {
            auto const model = buildModel();

            // `umbra` was this root's spelling before 2026-07-29; it is an
            // ordinary unknown global now, so the closure keys on `uf` alone.
            // Reverting the validator's k_namespace flips both reports below and
            // reddens this case. What the installer registers as the global is
            // pinned separately by the binding suite's retired-root case.
            auto const retired = validateScriptResources(
                "return uf.surfaces.home ~= nil and umbra.targets.battle",
                "retired-root",
                model
            );
            REQUIRE(retired.has_value());
            CHECK(retired->targets.empty());
            CHECK(retired->surfaces == std::vector<std::string>{"home"});

            auto const current = validateScriptResources(
                "return uf.targets.battle",
                "current-root",
                model
            );
            REQUIRE(current.has_value());
            CHECK(current->targets == std::vector<std::string>{"battle"});
        }

        TEST_CASE("Retired resource namespaces are rejected before the VM")
        {
            auto const model = buildModel();

            expectRejected(
                model,
                "return uf.elements.battle",
                {"uf.elements", "namespace"}
            );
            expectRejected(
                model,
                "return uf.pages.home",
                {"uf.pages", "namespace"}
            );
        }

        TEST_CASE("A method call on the uf root is rejected: there are no verbs")
        {
            auto const model = buildModel();

            // The root carries data alone, so a colon call on it names nothing
            // that could exist, and is rejected here rather than left to fail as
            // a runtime nil call.
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

            // ctx is a project global the framework published and exposes no
            // resource name, so it must not be mistaken for a uf reference --
            // while the literals inside its arguments are still enumerated.
            auto const report = validateScriptResources(
                "local d = ctx:deadline(1000)\n"
                "local r = ctx:random(1, 6)\n"
                "observe.wait_until(ctx, { surface = uf.surfaces.home })\n"
                "return r",
                "context-calls",
                model
            );
            REQUIRE(report.has_value());
            CHECK(report->targets.empty());
            CHECK(report->surfaces == std::vector<std::string>{"home"});
        }

        TEST_CASE("An error-kind literal validates and enumerates no resource")
        {
            auto const model = buildModel();

            // uf.errors.<kind> names host vocabulary rather than a project
            // resource, so it must pass the namespace gate without appearing in
            // the resource closure.
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
            CHECK(report->targets.empty());
            CHECK(report->surfaces.empty());
        }

        TEST_CASE("A misspelled error kind is rejected before the VM exists")
        {
            auto const model = buildModel();
            // Left as a runtime nil this makes every comparison against it
            // silently false, the failure the pre-VM pass closes for a missing
            // target.
            expectRejected(
                model,
                "return uf.errors.time_out",
                {"time_out", "error kind"}
            );
        }

        TEST_CASE("A reference to a missing target is rejected by ID")
        {
            auto const model = buildModel();
            expectRejected(
                model,
                "return uf.targets.does_not_exist",
                {"does_not_exist", "target"}
            );
        }

        TEST_CASE("A reference to a missing surface is rejected by ID")
        {
            auto const model = buildModel();
            expectRejected(
                model,
                "local s = uf.surfaces.does_not_exist\nreturn s",
                {"does_not_exist", "surface"}
            );
        }

        TEST_CASE("Every target the project file declares is an ID a script may write")
        {
            auto const model = buildModel();

            // home_marker only identifies, and the project file draws no line
            // between that and an interactive target, so it is an ID a script
            // may write.
            auto const report = validateScriptResources(
                "return uf.targets.home_marker",
                "anchor",
                model
            );
            REQUIRE(report.has_value());
            CHECK(report->targets == std::vector<std::string>{"home_marker"});

            // A project's own field under `extra` is never a resource, however
            // exactly it copies a key this layer owns: the fixture's target
            // carries `name = "extra_alias"` there, and the reader must not have
            // read it as the target's own ID.
            expectRejected(
                model,
                "return uf.targets.extra_alias",
                {"extra_alias", "declares no target"}
            );
        }

        TEST_CASE("Aliasing the namespace or a sub-table is rejected")
        {
            auto const model = buildModel();

            SUBCASE("binding the bare namespace to a local")
            {
                expectRejected(
                    model,
                    "local u = uf\nreturn u.targets.battle",
                    {"uf"}
                );
            }
            SUBCASE("binding a sub-table to a local")
            {
                expectRejected(
                    model,
                    "local r = uf.targets\nreturn r.battle",
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

            // `_G` is Luau's reflexive handle to the whole global table. Every
            // chain here reaches `uf` WITHOUT a literal uf root, so the rest of
            // the validator would classify them as unrelated code and the _G
            // rejection is what closes that door -- the pre-VM twin of
            // installSandbox niling `_G` on the task thread. Each is pinned to
            // the '_G' offender so a chain failing for another reason would not
            // pass.
            SUBCASE("indexing the namespace through the _G alias")
            {
                expectRejected(model, "return _G.uf.surfaces.home", {"_G"});
            }
            SUBCASE("aliasing the namespace off _G")
            {
                expectRejected(
                    model,
                    "local u = _G.uf\nreturn u.targets.battle",
                    {"_G"}
                );
            }
            SUBCASE("dynamically indexing a resource table off _G")
            {
                expectRejected(
                    model,
                    "local n = 'bat' .. 'tle'\nreturn _G.uf.targets[n]",
                    {"_G"}
                );
            }
            SUBCASE("iterating a resource table off _G")
            {
                expectRejected(
                    model,
                    "for k in pairs(_G.uf.targets) do end\nreturn 0",
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
                // Without the _G rejection this sails through validation and
                // becomes a runtime nil, defeating the pre-VM missing-resource
                // promise.
                expectRejected(
                    model,
                    "return _G.uf.targets.does_not_exist",
                    {"_G"}
                );
            }
        }

        TEST_CASE("Computed indexing into a resource table is rejected")
        {
            auto const model = buildModel();
            expectRejected(
                model,
                "local name = 'battle'\nreturn uf.targets[name]",
                {"uf", "two-level"}
            );
        }

        TEST_CASE("Indexing past a two-level handle literal is rejected")
        {
            auto const model = buildModel();
            // uf.targets.battle is a valid resource ID, but the only permitted
            // spelling is the two-level literal itself; a field read off it is a
            // deeper chain and rejected as the wrong shape.
            expectRejected(
                model,
                "return uf.targets.battle.foo",
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
            // uf.targets as a value is a one-level field access, and it is
            // rejected whether it is called, aliased, or returned.
            expectRejected(
                model,
                "return uf.targets(uf)",
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

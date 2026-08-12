#include <operator/project-plugin.hpp>

#include <json/value.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace uf::operator_runtime
{
    namespace
    {
        constexpr auto k_input = std::string_view{"{\"request\":\"ok\"}"};
        constexpr auto k_catalogueOutput = std::string_view{"{\"kind\":\"catalogue\"}"};
        constexpr auto k_workflowOutput = std::string_view{"{\"kind\":\"workflow\"}"};

        // Artifact bytes. Every artifact is a JSON document now, so a fixture
        // spelling one as a bare token would be refused at admission and no
        // case using it would measure what it names.
        constexpr auto k_contentRoot = std::string_view{"{\"root\":\"content\"}"};
        constexpr auto k_otherRoot = std::string_view{"{\"root\":\"different\"}"};
        constexpr auto k_countBlob = std::string_view{"{\"blob\":\"count-1\"}"};
        constexpr auto k_safeBlob = std::string_view{"{\"blob\":\"safe-true\"}"};
        constexpr auto k_payloadBlob = std::string_view{"{\"nested\":[1,2],\"safe\":true}"};
        constexpr auto k_emptyBlob = std::string_view{"{}"};

        constexpr auto k_cataloguePlugin = std::string_view{R"LUAU(
return {
    plugin_id = "fixture.catalogue",
    derive = function(_input) return { kind = "catalogue" } end,
    plan = function(_input) return { kind = "catalogue" } end,
    next_step = function(_input) return { kind = "catalogue" } end,
    reconcile = function(_input) return { kind = "catalogue" } end,
    reduce = function(_input) return { kind = "catalogue" } end,
}
)LUAU"};

        constexpr auto k_workflowPlugin = std::string_view{R"LUAU(
return {
    plugin_id = "fixture.workflow",
    derive = function(_input) return { kind = "workflow" } end,
    plan = function(_input) return { kind = "workflow" } end,
    next_step = function(_input) return { kind = "workflow" } end,
    reconcile = function(_input) return { kind = "workflow" } end,
    reduce = function(_input) return { kind = "workflow" } end,
}
)LUAU"};

        struct RegistrationFixture final
        {
            VerifiedProjectRegistration registration;
            ProjectSchemaOwner          schemaOwner;
        };

        // A canonical validator answers with the value the accepted bytes
        // denote, because the ProjectPlugin boundary is that value. The fixtures
        // here judge by an allowlist rather than by RFC 8785, so the parse is
        // separate from the judgement.
        [[nodiscard]]
        auto parseCanonical(std::string_view exactJcs) -> Result<json::Value>
        {
            auto parsed = json::parse(exactJcs);
            if (!parsed.has_value())
            {
                return fail(AutomationErrorKind::InvalidResource,
                            "fixture canonical validator was handed non-JSON");
            }
            return *std::move(parsed);
        }

        [[nodiscard]]
        auto hashOf(std::string_view value) -> ContentHash
        {
            auto const result = sha256(std::as_bytes(std::span{value}));
            REQUIRE(result.has_value());
            return *result;
        }

        [[nodiscard]]
        auto artifactRoot(std::string name, std::string_view exactBytes) -> NamedArtifactRoot
        {
            return NamedArtifactRoot{
                .name     = std::move(name),
                .rootHash = hashOf(exactBytes),
            };
        }

        [[nodiscard]]
        auto artifactBlob(std::string name, std::string bytes)
            -> ProjectPluginRegistrar::ArtifactBlob
        {
            return ProjectPluginRegistrar::ArtifactBlob{
                .name  = std::move(name),
                .bytes = std::move(bytes),
            };
        }

        [[nodiscard]]
        auto registrationClaims(std::string pluginId,
                                ContentHash pluginHash,
                                ContentHash schemaHash,
                                std::vector<NamedArtifactRoot> artifactRoots = {})
            -> ProjectRegistrationClaims
        {
            return ProjectRegistrationClaims{
                .manifestSchemaHash                 = schemaHash,
                .pluginId                           = std::move(pluginId),
                .pluginHash                         = pluginHash,
                .toolCatalogHash                    = hashOf("catalogue"),
                .projectStateSchemaHash             = hashOf("state"),
                .projectObservationSchemaHash       = hashOf("observation"),
                .projectToolPreconditionSchemaHash  = hashOf("precondition"),
                .reconcilePayloadSchemaManifestHash = hashOf("reconcile"),
                .journalEventSchemaManifestHash     = hashOf("journal"),
                .baselineEventType                  = "fixture.baseline",
                .projectArtifactRoots               = std::move(artifactRoots),
            };
        }

        [[nodiscard]]
        auto registrationJcs(ProjectRegistrationClaims const& claims) -> std::string
        {
            auto result = std::string{"{\"baseline_event_type\":\"" + claims.baselineEventType +
                                      "\",\"journal_event_schema_manifest_hash\":\"" +
                                      claims.journalEventSchemaManifestHash.hex() +
                                      "\",\"manifest_schema_hash\":\"" +
                                      claims.manifestSchemaHash.hex() + "\",\"plugin_hash\":\"" +
                                      claims.pluginHash.hex() + "\",\"plugin_id\":\"" +
                                      claims.pluginId + "\",\"project_artifact_roots\":["};
            for (auto index = std::size_t{0}; index < claims.projectArtifactRoots.size(); ++index)
            {
                if (index != 0U)
                    result.push_back(',');
                auto const& root = claims.projectArtifactRoots[index];
                result += "{\"name\":\"" + root.name + "\",\"root_hash\":\"" + root.rootHash.hex() +
                          "\"}";
            }
            result += "],\"project_observation_schema_hash\":\"" +
                      claims.projectObservationSchemaHash.hex() +
                      "\",\"project_state_schema_hash\":\"" + claims.projectStateSchemaHash.hex() +
                      "\",\"project_tool_precondition_schema_hash\":\"" +
                      claims.projectToolPreconditionSchemaHash.hex() +
                      "\",\"reconcile_payload_schema_manifest_hash\":\"" +
                      claims.reconcilePayloadSchemaManifestHash.hex() +
                      "\",\"tool_catalog_hash\":\"" + claims.toolCatalogHash.hex() + "\"}";
            return result;
        }

        [[nodiscard]]
        auto registrationFixture(std::string pluginId,
                                 std::string_view pluginBytes,
                                 std::vector<NamedArtifactRoot> artifactRoots = {})
            -> RegistrationFixture
        {
            auto const schemaHash = hashOf("registration-schema");
            auto const pluginHash = hashOf(pluginBytes);
            auto claims =
                registrationClaims(std::move(pluginId), pluginHash, schemaHash,
                                   std::move(artifactRoots));
            auto const exactJcs = registrationJcs(claims);
            auto const rootHash = hashOf(exactJcs);
            auto ownerResult = ProjectRegistrationSchemaOwner::create(
                schemaHash,
                [exactJcs, claims = std::move(claims)](
                    std::string_view candidate) -> Result<ProjectRegistrationClaims> {
                    if (candidate != exactJcs)
                    {
                        return fail(AutomationErrorKind::InvalidResource,
                                    "fixture registration is not exact JCS");
                    }
                    return claims;
                });
            REQUIRE(ownerResult.has_value());
            auto registrationResult =
                ProjectRegistration::verifyExact(exactJcs, rootHash, *ownerResult);
            REQUIRE(registrationResult.has_value());

            auto schemaOwnerResult = ProjectSchemaOwner::create(
                *registrationResult,
                ProjectDocumentSchemaBytes{
                    .projectState       = "state",
                    .projectObservation = "observation",
                    .toolPrecondition   = "precondition",
                },
                [](std::string_view candidateJcs) -> Result<json::Value> {
                    constexpr auto accepted = std::array{
                        k_input,
                        k_catalogueOutput,
                        k_workflowOutput,
                        std::string_view{"{\"count\":1}"},
                        std::string_view{"{\"blob\":\"count-1\"}"},
                        std::string_view{"{\"blob\":\"safe-true\"}"},
                        std::string_view{"{\"safe\":true}"},
                        std::string_view{"{}"},
                    };
                    if (std::ranges::find(accepted, candidateJcs) == accepted.end())
                    {
                        return fail(AutomationErrorKind::InvalidResource,
                                    "fixture canonical validator rejected bytes");
                    }
                    return parseCanonical(candidateJcs);
                },
                [](ProjectPluginFunction,
                   ProjectDocumentDirection direction,
                   std::string_view candidateJcs) -> Status {
                    bool const valid = direction == ProjectDocumentDirection::Input
                                           ? candidateJcs == k_input
                                           : candidateJcs != "{}";
                    if (!valid)
                    {
                        return fail(AutomationErrorKind::InvalidResource,
                                    "fixture function schema rejected document");
                    }
                    return ok();
                });
            REQUIRE(schemaOwnerResult.has_value());
            return RegistrationFixture{
                .registration = *registrationResult,
                .schemaOwner  = *schemaOwnerResult,
            };
        }

        [[nodiscard]]
        auto inputFor(ProjectSchemaOwner const& owner) -> CanonicalJson
        {
            auto const result = owner.canonicalize(std::string{k_input});
            REQUIRE(result.has_value());
            return *result;
        }

        auto verifyFiveFunctions(ProjectPluginHandle const& plugin,
                                 CanonicalJson const& input,
                                 std::string_view expected) -> void
        {
            auto const outputs = std::array{
                plugin.derive(input),
                plugin.plan(input),
                plugin.nextStep(input),
                plugin.reconcile(input),
                plugin.reduce(input),
            };
            for (auto const& output : outputs)
            {
                REQUIRE(output.has_value());
                CHECK(output->bytes() == expected);
                CHECK(output->direction() == ProjectDocumentDirection::Output);
            }
        }

        [[nodiscard]]
        auto echoPlugin(std::string_view pluginId) -> std::string
        {
            return std::string{"return { plugin_id = \"" + std::string{pluginId} +
                               "\", derive = function(input) return input end, "
                               "plan = function(input) return input end, "
                               "next_step = function(input) return input end, "
                               "reconcile = function(input) return input end, "
                               "reduce = function(input) return input end }"};
        }

        template <typename Registry>
        concept HasPluginOnlyLookup =
            requires(Registry const& registry, std::string const& pluginId) {
                registry.findExact(pluginId);
            };
    } // namespace

    static_assert(!std::is_default_constructible_v<CanonicalJson>);
    static_assert(!std::is_default_constructible_v<ValidatedDocument>);
    static_assert(!std::is_default_constructible_v<ProjectPluginHandle>);
    static_assert(!std::is_polymorphic_v<ProjectPluginHandle>);
    static_assert(std::is_copy_constructible_v<ProjectPluginHandle>);
    static_assert(!HasPluginOnlyLookup<ProjectPluginRegistrar>);

    TEST_CASE("contract-product-p05-fixtures")
    {
        auto registrar = ProjectPluginRegistrar{};
        auto catalogue = registrationFixture("fixture.catalogue", k_cataloguePlugin);
        auto workflow  = registrationFixture("fixture.workflow", k_workflowPlugin);

        auto const cataloguePlugin = registrar.registerPlugin(catalogue.registration,
                                                              std::string{k_cataloguePlugin},
                                                              {},
                                                              catalogue.schemaOwner);
        auto const workflowPlugin = registrar.registerPlugin(workflow.registration,
                                                             std::string{k_workflowPlugin},
                                                             {},
                                                             workflow.schemaOwner);
        REQUIRE(cataloguePlugin.has_value());
        REQUIRE(workflowPlugin.has_value());

        verifyFiveFunctions(*cataloguePlugin, inputFor(catalogue.schemaOwner), k_catalogueOutput);
        verifyFiveFunctions(*workflowPlugin, inputFor(workflow.schemaOwner), k_workflowOutput);

        REQUIRE(
            registrar.findExact("fixture.catalogue", catalogue.registration.hash()).has_value());
        CHECK_FALSE(
            registrar.findExact("fixture.catalogue", workflow.registration.hash()).has_value());
        CHECK_FALSE(registrar
                        .registerPlugin(catalogue.registration,
                                        std::string{k_cataloguePlugin},
                                        {},
                                        catalogue.schemaOwner)
                        .has_value());
    }

    TEST_CASE("ProjectPlugin registrar binds verified identity and exact bytes")
    {
        SUBCASE("plugin bytes must match the verified registration")
        {
            auto registrar = ProjectPluginRegistrar{};
            auto fixture   = registrationFixture("fixture.catalogue", k_cataloguePlugin);
            CHECK_FALSE(registrar
                            .registerPlugin(fixture.registration,
                                            std::string{k_workflowPlugin},
                                            {},
                                            fixture.schemaOwner)
                            .has_value());
        }

        SUBCASE("module identity must match the verified registration")
        {
            auto const source = echoPlugin("fixture.actual");
            auto registrar = ProjectPluginRegistrar{};
            auto fixture   = registrationFixture("fixture.expected", source);
            CHECK_FALSE(
                registrar.registerPlugin(fixture.registration, source, {}, fixture.schemaOwner)
                    .has_value());
        }

        SUBCASE("module may export only the five data functions")
        {
            auto const source = std::string{R"LUAU(
return {
    plugin_id = "fixture.extra",
    derive = function(input) return input end,
    plan = function(input) return input end,
    next_step = function(input) return input end,
    reconcile = function(input) return input end,
    reduce = function(input) return input end,
    hidden_callback = function() return true end,
}
)LUAU"};
            auto registrar = ProjectPluginRegistrar{};
            auto fixture   = registrationFixture("fixture.extra", source);
            CHECK_FALSE(
                registrar.registerPlugin(fixture.registration, source, {}, fixture.schemaOwner)
                    .has_value());
        }
    }

    TEST_CASE("ProjectPlugin wrapper survives initialization pollution")
    {
        auto const source = std::string{R"LUAU(
type = function() return "poisoned" end
error = function() return nil end
pairs = function() return function() return nil end end
ipairs = function() return function() return nil end end

return {
    plugin_id = "fixture.pollution",
    derive = function(_input)
        -- Only names something could plausibly publish. Testing for globals no
        -- code path registers -- registry, native, Host, Receipt, operator_db --
        -- held whether or not the whitelist existed, so those disjuncts said
        -- nothing and are gone.
        -- Only names the whitelist actually excludes. Testing for globals no
        -- code path registers -- registry, native, Host, Receipt, operator_db --
        -- held whether or not the whitelist existed, so those disjuncts said
        -- nothing and are gone. setmetatable and the raw* family ARE published
        -- on purpose (k_pureGlobals), so asserting their absence would fail a
        -- correct VM.
        local forbidden = require ~= nil or load ~= nil or loadfile ~= nil
            or loadstring ~= nil or dofile ~= nil or debug ~= nil or _G ~= nil
            or os ~= nil or io ~= nil or coroutine ~= nil or newproxy ~= nil
            or collectgarbage ~= nil or string.dump ~= nil
            or math.random ~= nil or math.randomseed ~= nil
            or getfenv ~= nil or setfenv ~= nil
        if forbidden then return { safe = false } end
        return { safe = true }
    end,
    plan = function(input) return input end,
    next_step = function(input) return input end,
    reconcile = function(input) return input end,
    reduce = function(input) return input end,
}
)LUAU"};
        auto registrar = ProjectPluginRegistrar{};
        auto fixture   = registrationFixture("fixture.pollution", source);
        auto const plugin =
            registrar.registerPlugin(fixture.registration, source, {}, fixture.schemaOwner);
        REQUIRE(plugin.has_value());

        auto const result = plugin->derive(inputFor(fixture.schemaOwner));
        REQUIRE(result.has_value());
        CHECK(result->bytes() == "{\"safe\":true}");
    }

    TEST_CASE("ProjectPlugin invocation uses a fresh VM and immutable bytecode")
    {
        auto const source = std::string{R"LUAU(
local count = 0
return {
    plugin_id = "fixture.fresh",
    derive = function(_input)
        count += 1
        if count == 1 then return { count = 1 } end
        return canon.emptyObject
    end,
    plan = function(input) return input end,
    next_step = function(input) return input end,
    reconcile = function(input) return input end,
    reduce = function(input) return input end,
}
)LUAU"};
        auto registrar     = ProjectPluginRegistrar{};
        auto fixture       = registrationFixture("fixture.fresh", source);
        auto mutableSource = source;
        auto const plugin =
            registrar.registerPlugin(fixture.registration, mutableSource, {}, fixture.schemaOwner);
        REQUIRE(plugin.has_value());
        mutableSource.assign("return {}");

        auto const input = inputFor(fixture.schemaOwner);
        auto const first = plugin->derive(input);
        auto const second = plugin->derive(input);
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        CHECK(first->bytes() == "{\"count\":1}");
        CHECK(*first == *second);
    }

    TEST_CASE("ProjectPlugin validates complete schemas before and after execution")
    {
        SUBCASE("authorityless canonical input cannot be relabelled schema-valid")
        {
            auto registrar = ProjectPluginRegistrar{};
            auto fixture   = registrationFixture("fixture.catalogue", k_cataloguePlugin);
            auto const plugin = registrar.registerPlugin(fixture.registration,
                                                         std::string{k_cataloguePlugin},
                                                         {},
                                                         fixture.schemaOwner);
            REQUIRE(plugin.has_value());
            auto const empty = fixture.schemaOwner.canonicalize("{}");
            REQUIRE(empty.has_value());
            CHECK_FALSE(plugin->derive(*empty).has_value());
        }

        SUBCASE("schema-invalid output is rejected after plugin execution")
        {
            auto const source = echoPlugin("fixture.invalid-output");
            auto registrar = ProjectPluginRegistrar{};
            auto fixture   = registrationFixture("fixture.invalid-output", source);
            auto const plugin =
                registrar.registerPlugin(fixture.registration, source, {}, fixture.schemaOwner);
            REQUIRE(plugin.has_value());

            auto invalidOwnerResult = ProjectSchemaOwner::create(
                fixture.registration,
                ProjectDocumentSchemaBytes{
                    .projectState       = "state",
                    .projectObservation = "observation",
                    .toolPrecondition   = "precondition",
                },
                [](std::string_view candidateJcs) -> Result<json::Value> {
                    return parseCanonical(candidateJcs);
                },
                [](ProjectPluginFunction,
                   ProjectDocumentDirection direction,
                   std::string_view) -> Status {
                    if (direction == ProjectDocumentDirection::Output)
                    {
                        return fail(AutomationErrorKind::InvalidResource,
                                    "output schema rejected document");
                    }
                    return ok();
                });
            REQUIRE(invalidOwnerResult.has_value());
            auto secondRegistrar = ProjectPluginRegistrar{};
            auto const guarded = secondRegistrar.registerPlugin(fixture.registration,
                                                                source,
                                                                {},
                                                                *invalidOwnerResult);
            REQUIRE(guarded.has_value());
            auto const input = invalidOwnerResult->canonicalize(std::string{k_input});
            REQUIRE(input.has_value());
            CHECK_FALSE(guarded->derive(*input).has_value());
        }

        SUBCASE("non-canonical bytes never reach a document validator")
        {
            auto const source = echoPlugin("fixture.canonical");
            auto fixture = registrationFixture("fixture.canonical", source);
            CHECK_FALSE(fixture.schemaOwner.canonicalize("{ \"request\":\"ok\" }").has_value());

            auto const laxOwner = ProjectSchemaOwner::create(
                fixture.registration,
                ProjectDocumentSchemaBytes{
                    .projectState       = "state",
                    .projectObservation = "observation",
                    .toolPrecondition   = "precondition",
                },
                [](std::string_view candidateJcs) -> Result<json::Value> {
                    return parseCanonical(candidateJcs);
                },
                [](ProjectPluginFunction, ProjectDocumentDirection, std::string_view) -> Status {
                    return ok();
                });
            REQUIRE(laxOwner.has_value());
            auto const forged = laxOwner->canonicalize("{ \"request\":\"ok\" }");
            REQUIRE(forged.has_value());

            auto registrar = ProjectPluginRegistrar{};
            auto const plugin =
                registrar.registerPlugin(fixture.registration, source, {}, fixture.schemaOwner);
            REQUIRE(plugin.has_value());
            CHECK_FALSE(plugin->derive(*forged).has_value());
        }

        SUBCASE("the validating owner's parsed value is the execution input")
        {
            auto const source = echoPlugin("fixture.reparsed-input");
            auto fixture = registrationFixture("fixture.reparsed-input", source);

            auto foreignOwner = ProjectSchemaOwner::create(
                fixture.registration,
                ProjectDocumentSchemaBytes{
                    .projectState       = "state",
                    .projectObservation = "observation",
                    .toolPrecondition   = "precondition",
                },
                [](std::string_view candidateJcs) -> Result<json::Value> {
                    if (candidateJcs != k_input)
                    {
                        return fail(AutomationErrorKind::InvalidResource,
                                    "foreign owner rejected bytes");
                    }
                    return parseCanonical("{\"request\":\"foreign-cache\"}");
                },
                [](ProjectPluginFunction, ProjectDocumentDirection, std::string_view) -> Status {
                    return ok();
                }
            );
            REQUIRE(foreignOwner.has_value());
            auto const foreignInput = foreignOwner->canonicalize(std::string{k_input});
            REQUIRE(foreignInput.has_value());
            CHECK(json::canonicalBytes(foreignInput->value())
                  == "{\"request\":\"foreign-cache\"}");

            auto registrar = ProjectPluginRegistrar{};
            auto const plugin = registrar.registerPlugin(
                fixture.registration,
                source,
                {},
                fixture.schemaOwner
            );
            REQUIRE(plugin.has_value());

            auto const output = plugin->derive(*foreignInput);
            REQUIRE(output.has_value());
            CHECK(output->bytes() == k_input);
        }
    }

    // Every blob below is valid JSON even where the closure rule is what must
    // refuse it. An artifact that is not JSON is refused at admission, so
    // non-JSON fixtures here would leave each of these cases red for a reason
    // it does not name.
    TEST_CASE("ProjectPlugin registrar requires an exact artifact blob closure")
    {
        SUBCASE("missing blob is rejected")
        {
            auto registrar = ProjectPluginRegistrar{};
            auto fixture = registrationFixture("fixture.catalogue",
                                               k_cataloguePlugin,
                                               {artifactRoot("content", k_contentRoot)});
            CHECK_FALSE(registrar
                            .registerPlugin(fixture.registration,
                                            std::string{k_cataloguePlugin},
                                            {},
                                            fixture.schemaOwner)
                            .has_value());
        }

        SUBCASE("extra blob is rejected")
        {
            auto registrar = ProjectPluginRegistrar{};
            auto fixture   = registrationFixture("fixture.catalogue", k_cataloguePlugin);
            CHECK_FALSE(registrar
                            .registerPlugin(fixture.registration,
                                            std::string{k_cataloguePlugin},
                                            {artifactBlob("extra", std::string{k_contentRoot})},
                                            fixture.schemaOwner)
                            .has_value());
        }

        SUBCASE("wrong blob hash is rejected")
        {
            auto registrar = ProjectPluginRegistrar{};
            auto fixture = registrationFixture("fixture.catalogue",
                                               k_cataloguePlugin,
                                               {artifactRoot("content", k_contentRoot)});
            CHECK_FALSE(registrar
                            .registerPlugin(fixture.registration,
                                            std::string{k_cataloguePlugin},
                                            {artifactBlob("content", std::string{k_otherRoot})},
                                            fixture.schemaOwner)
                            .has_value());
        }

        SUBCASE("duplicate blob name is rejected")
        {
            auto registrar = ProjectPluginRegistrar{};
            auto fixture = registrationFixture("fixture.catalogue",
                                               k_cataloguePlugin,
                                               {artifactRoot("content", k_contentRoot)});
            CHECK_FALSE(registrar
                            .registerPlugin(fixture.registration,
                                            std::string{k_cataloguePlugin},
                                            {
                                                artifactBlob("content", std::string{k_contentRoot}),
                                                artifactBlob("content", std::string{k_contentRoot}),
                                            },
                                            fixture.schemaOwner)
                            .has_value());
        }
    }

    TEST_CASE("ProjectPlugin exposes only a frozen pathless artifact reader")
    {
        SUBCASE("initialization and calls read immutable blobs in fresh VMs")
        {
            // Each function returns the artifact's own value, so the assertions
            // below are red if what artifact.read hands over is anything but
            // the decoded document -- a byte string would canonicalize to a
            // quoted JSON string rather than to the object itself.
            auto const source = std::string{R"LUAU(
local initialized = artifact.read("initial")
local count = 0
return {
    plugin_id = "fixture.artifacts",
    derive = function(_input)
        count += 1
        if count ~= 1 then return canon.emptyObject end
        return initialized
    end,
    plan = function(_input) return artifact.read("runtime") end,
    next_step = function(input) return input end,
    reconcile = function(input) return input end,
    reduce = function(input) return input end,
}
)LUAU"};
            auto registrar = ProjectPluginRegistrar{};
            auto fixture = registrationFixture("fixture.artifacts",
                                               source,
                                               {
                                                   artifactRoot("initial", k_countBlob),
                                                   artifactRoot("runtime", k_safeBlob),
                                               });
            auto blobs = std::vector<ProjectPluginRegistrar::ArtifactBlob>{
                artifactBlob("runtime", std::string{k_safeBlob}),
                artifactBlob("initial", std::string{k_countBlob}),
            };
            auto const plugin =
                registrar.registerPlugin(fixture.registration, source, blobs, fixture.schemaOwner);
            REQUIRE(plugin.has_value());

            blobs[0].bytes.assign("moved");
            blobs[1].bytes.assign("moved");
            auto const input = inputFor(fixture.schemaOwner);
            auto const first = plugin->derive(input);
            auto const second = plugin->derive(input);
            auto const runtime = plugin->plan(input);
            REQUIRE(first.has_value());
            REQUIRE(second.has_value());
            REQUIRE(runtime.has_value());
            CHECK(first->bytes() == "{\"blob\":\"count-1\"}");
            CHECK(*first == *second);
            CHECK(runtime->bytes() == "{\"blob\":\"safe-true\"}");
        }

        SUBCASE("unknown root is rejected even when the root set is empty")
        {
            auto const source = std::string{R"LUAU(
return {
    plugin_id = "fixture.unknown-artifact",
    derive = function(_input) return artifact.read("../unknown") end,
    plan = function(input) return input end,
    next_step = function(input) return input end,
    reconcile = function(input) return input end,
    reduce = function(input) return input end,
}
)LUAU"};
            auto registrar = ProjectPluginRegistrar{};
            auto fixture   = registrationFixture("fixture.unknown-artifact", source);
            auto const plugin =
                registrar.registerPlugin(fixture.registration, source, {}, fixture.schemaOwner);
            REQUIRE(plugin.has_value());
            CHECK_FALSE(plugin->derive(inputFor(fixture.schemaOwner)).has_value());
        }

        SUBCASE("reader and returned value cannot be replaced or enumerated")
        {
            // The returned value is a frozen table, so the probe that used to
            // write into a Lua string writes into the table and its nested
            // array instead; a value that is merely immutable-by-type would
            // satisfy the first and not these.
            auto const source = std::string{R"LUAU(
local reader = artifact
local value = reader.read("payload")
local assign_ok = pcall(function()
    reader.read = function() return {} end
end)
local rawset_ok = pcall(function()
    rawset(reader, "payload", {})
end)
local value_write_ok = pcall(function()
    value.safe = false
end)
local value_rawset_ok = pcall(function()
    rawset(value, "extra", 1)
end)
local nested_write_ok = pcall(function()
    value.nested[1] = 9
end)
local leaked_root = false
for key in pairs(reader) do
    if key == "payload" then leaked_root = true end
end
artifact = {}

return {
    plugin_id = "fixture.frozen-artifact",
    derive = function(_input)
        local safe = not assign_ok and not rawset_ok and not value_write_ok
            and not value_rawset_ok and not nested_write_ok
            and not leaked_root
            and type(value) == "table" and value.safe == true
            and value.nested[1] == 1
            and rawequal(reader.read("payload"), value)
        if safe then return { safe = true } end
        return canon.emptyObject
    end,
    plan = function(input) return input end,
    next_step = function(input) return input end,
    reconcile = function(input) return input end,
    reduce = function(input) return input end,
}
)LUAU"};
            auto registrar = ProjectPluginRegistrar{};
            auto fixture = registrationFixture("fixture.frozen-artifact",
                                               source,
                                               {artifactRoot("payload", k_payloadBlob)});
            auto const plugin =
                registrar.registerPlugin(fixture.registration,
                                         source,
                                         {artifactBlob("payload", std::string{k_payloadBlob})},
                                         fixture.schemaOwner);
            REQUIRE(plugin.has_value());
            auto const result = plugin->derive(inputFor(fixture.schemaOwner));
            REQUIRE(result.has_value());
            CHECK(result->bytes() == "{\"safe\":true}");
        }
    }

    TEST_CASE("ProjectPlugin artifact and VM resources have hard ceilings")
    {
        SUBCASE("artifact root count is bounded")
        {
            auto roots = std::vector<NamedArtifactRoot>{};
            auto blobs = std::vector<ProjectPluginRegistrar::ArtifactBlob>{};
            for (auto index = std::size_t{0}; index < 65U; ++index)
            {
                auto const name =
                    std::string{"root-"} + (index < 10U ? "0" : "") + std::to_string(index);
                roots.emplace_back(artifactRoot(name, k_emptyBlob));
                blobs.emplace_back(artifactBlob(name, std::string{k_emptyBlob}));
            }
            auto registrar = ProjectPluginRegistrar{};
            auto fixture =
                registrationFixture("fixture.catalogue", k_cataloguePlugin, std::move(roots));
            CHECK_FALSE(registrar
                            .registerPlugin(fixture.registration,
                                            std::string{k_cataloguePlugin},
                                            std::move(blobs),
                                            fixture.schemaOwner)
                            .has_value());
        }

        SUBCASE("single artifact bytes are bounded")
        {
            // A JSON string document one byte past the per-artifact ceiling.
            // Repeated bytes alone are not a document, so a ceiling removed
            // here would leave admission refusing on the parse instead.
            auto oversized = "\"" + std::string(4U * 1024U * 1024U, 'x') + "\"";
            auto registrar = ProjectPluginRegistrar{};
            auto fixture = registrationFixture("fixture.catalogue",
                                               k_cataloguePlugin,
                                               {artifactRoot("oversized", oversized)});
            CHECK_FALSE(registrar
                            .registerPlugin(fixture.registration,
                                            std::string{k_cataloguePlugin},
                                            {artifactBlob("oversized", std::move(oversized))},
                                            fixture.schemaOwner)
                            .has_value());
        }

        SUBCASE("total artifact bytes are bounded")
        {
            // Five documents of exactly the per-artifact ceiling, so only the
            // total can refuse them, and each is a JSON string document so only
            // a ceiling can.
            auto const bytes =
                "\"" + std::string(std::size_t{4U} * 1024U * 1024U - 2U, 'x') + "\"";
            auto roots = std::vector<NamedArtifactRoot>{};
            auto blobs = std::vector<ProjectPluginRegistrar::ArtifactBlob>{};
            for (auto index = std::size_t{0}; index < 5U; ++index)
            {
                auto const name = std::string{"root-"} + std::to_string(index);
                roots.emplace_back(artifactRoot(name, bytes));
                blobs.emplace_back(artifactBlob(name, bytes));
            }
            auto registrar = ProjectPluginRegistrar{};
            auto fixture =
                registrationFixture("fixture.catalogue", k_cataloguePlugin, std::move(roots));
            CHECK_FALSE(registrar
                            .registerPlugin(fixture.registration,
                                            std::string{k_cataloguePlugin},
                                            std::move(blobs),
                                            fixture.schemaOwner)
                            .has_value());
        }

        SUBCASE("artifact reads remain inside the fresh VM memory quota")
        {
            // 400,000 empty arrays: 1.14 MiB of text, inside every byte
            // ceiling, and one Luau table each, which is past what the fresh
            // VM may allocate. The blobs this case used to carry were 4 MiB of
            // one repeated byte, which under a value boundary is refused by the
            // parse -- leaving the case green while never reaching the quota it
            // is named for.
            auto wide = std::string{"["};
            for (auto index = std::size_t{0}; index < 400000U; ++index)
            {
                wide += index == 0U ? "[]" : ",[]";
            }
            wide += "]";

            auto registrar = ProjectPluginRegistrar{};
            auto fixture   = registrationFixture("fixture.catalogue",
                                                 k_cataloguePlugin,
                                                 {artifactRoot("wide", wide)});
            auto const registered =
                registrar.registerPlugin(fixture.registration,
                                         std::string{k_cataloguePlugin},
                                         {artifactBlob("wide", std::move(wide))},
                                         fixture.schemaOwner);
            REQUIRE_FALSE(registered.has_value());
            CHECK(
                std::string{registered.error().message()}
                    .find("cannot be materialized inside its VM quota")
                != std::string::npos
            );
        }

        SUBCASE("registration initialization is instruction bounded")
        {
            auto const source =
                std::string{"while true do end\nreturn { plugin_id = \"fixture.loop\" }"};
            auto registrar = ProjectPluginRegistrar{};
            auto fixture   = registrationFixture("fixture.loop", source);
            CHECK_FALSE(
                registrar.registerPlugin(fixture.registration, source, {}, fixture.schemaOwner)
                    .has_value());
        }

        SUBCASE("function execution is instruction bounded")
        {
            auto const source = std::string{R"LUAU(
return {
    plugin_id = "fixture.invoke-loop",
    derive = function(_input) while true do end end,
    plan = function(input) return input end,
    next_step = function(input) return input end,
    reconcile = function(input) return input end,
    reduce = function(input) return input end,
}
)LUAU"};
            auto registrar = ProjectPluginRegistrar{};
            auto fixture   = registrationFixture("fixture.invoke-loop", source);
            auto const plugin =
                registrar.registerPlugin(fixture.registration, source, {}, fixture.schemaOwner);
            REQUIRE(plugin.has_value());
            CHECK_FALSE(plugin->derive(inputFor(fixture.schemaOwner)).has_value());
        }

        SUBCASE("source and input have hard byte ceilings")
        {
            auto const oversizedSource = std::string(256U * 1024U + 1U, 'x');
            auto registrar = ProjectPluginRegistrar{};
            auto fixture   = registrationFixture("fixture.oversized", oversizedSource);
            CHECK_FALSE(
                registrar
                    .registerPlugin(fixture.registration, oversizedSource, {}, fixture.schemaOwner)
                    .has_value());
            CHECK_FALSE(
                fixture.schemaOwner.canonicalize(std::string(1024U * 1024U + 1U, 'x')).has_value());
        }

        SUBCASE("artifact output bytes are bounded inside the trusted wrapper")
        {
            // An artifact this VM can hold and this boundary will not hand
            // back: one JSON string a byte past the returned value's text
            // ceiling. The refusal is asserted by name because the minted
            // canonical bytes carry a ceiling of their own, and a case that
            // asked only whether the call failed would be satisfied by that
            // one instead.
            auto const source = std::string{R"LUAU(
return {
    plugin_id = "fixture.large",
    derive = function(_input) return artifact.read("large") end,
    plan = function(input) return input end,
    next_step = function(input) return input end,
    reconcile = function(input) return input end,
    reduce = function(input) return input end,
}
)LUAU"};
            auto bytes     = "\"" + std::string(1024U * 1024U + 1U, 'x') + "\"";
            auto registrar = ProjectPluginRegistrar{};
            auto fixture =
                registrationFixture("fixture.large", source, {artifactRoot("large", bytes)});
            auto const plugin = registrar.registerPlugin(fixture.registration,
                                                         source,
                                                         {artifactBlob("large", std::move(bytes))},
                                                         fixture.schemaOwner);
            REQUIRE(plugin.has_value());
            auto const derived = plugin->derive(inputFor(fixture.schemaOwner));
            REQUIRE_FALSE(derived.has_value());
            CHECK(
                std::string{derived.error().message()}.find("fixed byte ceiling")
                != std::string::npos
            );
        }
    }
} // namespace uf::operator_runtime

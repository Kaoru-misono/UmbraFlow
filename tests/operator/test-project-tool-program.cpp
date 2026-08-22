#include <operator/project-plugin.hpp>
#include <operator/project-tool-program.hpp>
#include <operator/tool-invocation.hpp>

#include <script/scoped-tool-program.hpp>

#include <json/value.hpp>

#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <array>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Stage 2 of the unified Tool handler cut: the loader and registrar that bind a
// Project's closed module closure, its pinned Tool Catalog, its binding table,
// the scoped SDK generation and the Tool Runtime generation into ONE compiled
// program per registration generation, before any run starts.
//
// Everything here is production-unreachable by construction: nothing in
// ProductLifecycle registers a Project Tool program, and these cases are the
// only callers. What they have to prove is what a reader cannot see from a
// successful load -- that the program is one and not one per entry, that its
// entry points came from the binding table rather than from a caller, that the
// per-entry variance never reached the compiler, and that each of the three
// bind-time refusals is reachable through the loader rather than only through
// the join in isolation.
namespace uf::operator_runtime
{
    namespace
    {
        constexpr auto k_pluginId    = std::string_view{"chaos.project"};
        constexpr auto k_dismissTool = std::string_view{"chaos.project.dismiss"};
        constexpr auto k_sweepTool   = std::string_view{"chaos.project.sweep"};

        // The catalog bytes are opaque on purpose: the owner proves they hash
        // to what the registration pinned, and the descriptors it answers with
        // arrive through the reader. What is under test is the join between the
        // names those bytes declare and the entries a closure exports.
        constexpr auto k_toolCatalogBytes = std::string_view{
            R"({"schema":"umbraflow-tool-catalog/v1","tools":["dismiss","sweep"]})"
        };


        // Two entries, one closure. `dismiss` reaches the Tool Runtime through
        // the scoped facade, which is only loadable if the loader baked the
        // pinned catalog resource; `sweep` answers from the frozen discovery
        // table without spending a call.
        constexpr auto k_projectSource = std::string_view{R"LUAU(
local tools = require("@umbraflow/tools")

return {
    plugin_id = "chaos.project",
    dismiss = function(input)
        return tools.call("framework.audit.record", input)
    end,
    sweep = function(_input)
        return {
            catalog_hash = tools.catalog_hash,
            knows_sibling = tools.knows("chaos.project.dismiss"),
            knows_framework = tools.knows("framework.audit.record"),
        }
    end,
}
)LUAU"};

        // The same closure with `sweep` removed, for the case where the
        // registration binds an entry the code does not carry.
        constexpr auto k_dismissOnlySource = std::string_view{R"LUAU(
return {
    plugin_id = "chaos.project",
    dismiss = function(input)
        return input
    end,
}
)LUAU"};

        struct ScopedCall final
        {
            std::string toolName{};
            std::string arguments{};
            std::string parentPosition{};
            uint64      childIndex{0};
        };

        [[nodiscard]]
        auto hashOf(std::string_view value) -> ContentHash
        {
            auto const result = sha256(std::as_bytes(std::span{value}));
            REQUIRE(result.has_value());
            return *result;
        }

        // The durable position a run is anchored on. It stands for the row of
        // the call the run implements; a run is never anchored on nothing, and
        // there is no spelling of "no position" to reach for.
        [[nodiscard]]
        auto runPosition() -> ContentHash
        {
            return hashOf("run-position");
        }

        // These cases are about the join, not about result schemas, so the
        // validator accepts. The case that proves a refused answer is a refused
        // call belongs to the dispatcher.
        [[nodiscard]]
        auto acceptingResults() -> ToolResultValidator
        {
            return [](std::string_view, std::string_view) -> Status
            {
                return ok();
            };
        }

        // Answers in the exact shape @umbraflow/tools admits, derived from the
        // coordinate the seam assigned, so a script that returns one has proved
        // what the seam handed the runtime.
        [[nodiscard]]
        auto recordingRuntime(std::shared_ptr<std::vector<ScopedCall>> log)
            -> script::ToolRuntimeInvoke
        {
            return [log = std::move(log)](
                       std::string_view toolName,
                       json::Value const& arguments,
                       script::ToolCallCoordinate const& coordinate,
                       std::stop_token
                   ) -> Result<json::Value>
            {
                log->emplace_back(ScopedCall{
                    .toolName       = std::string{toolName},
                    .arguments      = json::canonicalBytes(arguments),
                    .parentPosition = coordinate.parentPosition.hex(),
                    .childIndex     = coordinate.childIndex,
                });
                return json::Value::ofObject({
                    {"call_identity",
                     json::Value::ofString(hashOf(toolName).hex())},
                    {"result", arguments},
                    {"state", json::Value::ofString("confirmed")},
                    {"tool", json::Value::ofString(std::string{toolName})},
                });
            };
        }

        [[nodiscard]]
        auto declaredDescriptor(
            std::vector<std::string> childToolNames,
            uint32 maximumChildCalls,
            uint64 maximumElapsedMillis
        ) -> ToolDescriptor
        {
            return ToolDescriptor{
                .toolVersion          = "1",
                .requiredCapabilities = {},
                .effectBounds         = {},
                .uiActionBounds       = {},
                .childEffects         = ChildEffectDeclaration{
                    .childToolNames         = std::move(childToolNames),
                    .maximumChildSurface    = ToolSurface::Semantic,
                    .maximumChildMutability = ToolMutability::ReadOnly,
                    .maximumChildRisk       = Risk::ReadOnly,
                    .maximumChildCalls      = maximumChildCalls,
                },
                .limits = WorkflowLimits{
                    .maximumSteps         = 1U,
                    .maximumDispatches    = 1U,
                    .maximumObservations  = 1U,
                    .maximumWaits         = 0U,
                    .maximumElapsedMillis = maximumElapsedMillis,
                },
                .timeout = TimeoutPolicy{
                    .maximumElapsedMillis = maximumElapsedMillis,
                    .onTimeout            = TimeoutAction::Stop,
                },
                .mutability  = ToolMutability::ReadOnly,
                .surface     = ToolSurface::Semantic,
                .idempotency = ToolIdempotency::ReadSafe,
            };
        }

        // Deliberately different per entry: a child set and a ceiling on one,
        // neither on the other. Nothing about this difference may reach the
        // compiler.
        [[nodiscard]]
        auto declaredTool(std::string_view name) -> ToolCatalogEntry
        {
            if (name == k_sweepTool)
            {
                return ToolCatalogEntry{
                    .name       = std::string{name},
                    .descriptor = declaredDescriptor(
                        {std::string{k_dismissTool}},
                        4U,
                        30'000U
                    ),
                };
            }
            return ToolCatalogEntry{
                .name       = std::string{name},
                .descriptor = declaredDescriptor({}, 0U, 5'000U),
            };
        }

        [[nodiscard]]
        auto moduleBlobs(std::string_view source)
            -> std::vector<ProjectPluginRegistrar::ModuleBlob>
        {
            auto blobs = std::vector<ProjectPluginRegistrar::ModuleBlob>{};
            blobs.emplace_back(ProjectPluginRegistrar::ModuleBlob{
                .name   = "main",
                .source = std::string{source},
            });
            return blobs;
        }

        [[nodiscard]]
        auto registrationJcs(ProjectRegistrationClaims const& claims) -> std::string
        {
            auto bindings = std::vector<json::Value>{};
            for (auto const& binding : claims.projectToolBindings)
            {
                bindings.emplace_back(json::Value::ofObject({
                    {"entry_point", json::Value::ofString(binding.entryPoint)},
                    {"tool_name", json::Value::ofString(binding.toolName)},
                }));
            }
            return json::canonicalBytes(json::Value::ofObject({
                {"baseline_event_type",
                 json::Value::ofString(claims.baselineEventType)},
                {"journal_event_schema_manifest_hash",
                 json::Value::ofString(claims.journalEventSchemaManifestHash.hex())},
                {"observed_instance_identity_schema_hashes",
                 json::Value::ofArray({})},
                {"plugin_environment_hash",
                 json::Value::ofString(claims.pluginEnvironmentHash.hex())},
                {"plugin_id", json::Value::ofString(claims.pluginId)},
                {"plugin_module_manifest_hash",
                 json::Value::ofString(claims.pluginModuleManifestHash.hex())},
                {"project_observation_schema_hash",
                 json::Value::ofString(claims.projectObservationSchemaHash.hex())},
                {"project_registration_format",
                 json::Value::ofNumber(
                     static_cast<double>(claims.projectRegistrationFormat)
                 )},
                {"project_resources", json::Value::ofArray({})},
                {"project_state_schema_hash",
                 json::Value::ofString(claims.projectStateSchemaHash.hex())},
                {"project_tool_bindings", json::Value::ofArray(std::move(bindings))},
                {"project_tool_precondition_schema_hash",
                 json::Value::ofString(
                     claims.projectToolPreconditionSchemaHash.hex()
                 )},
                {"reconcile_payload_schema_manifest_hash",
                 json::Value::ofString(
                     claims.reconcilePayloadSchemaManifestHash.hex()
                 )},
                {"tool_catalog_hash",
                 json::Value::ofString(claims.toolCatalogHash.hex())},
            }));
        }

        [[nodiscard]]
        auto verifiedRegistration(ProjectRegistrationClaims claims)
            -> VerifiedProjectRegistration
        {
            auto const exactJcs = registrationJcs(claims);
            auto owner          = ProjectRegistrationSchemaOwner::create(
                [exactJcs = exactJcs, claims = std::move(claims)](
                    std::string_view candidate
                ) -> Result<ProjectRegistrationClaims>
                {
                    if (candidate != exactJcs)
                    {
                        return fail(
                            AutomationErrorKind::InvalidResource,
                            "loader fixture registration is not exact JCS"
                        );
                    }
                    return claims;
                }
            );
            REQUIRE(owner.has_value());
            auto registration = ProjectRegistration::verifyExact(
                exactJcs,
                hashOf(exactJcs),
                *owner
            );
            REQUIRE(registration.has_value());
            return *std::move(registration);
        }

        // A registration over one exact closure and one exact binding table.
        // The module manifest and environment digests are derived rather than
        // stated, because a fixture that stated them would prove nothing about
        // the loader's own checks.
        [[nodiscard]]
        auto registrationOver(
            std::string_view source,
            std::vector<ProjectToolBinding> bindings
        ) -> VerifiedProjectRegistration
        {
            auto const modules            = moduleBlobs(source);
            auto const moduleManifestHash = derivePluginModuleManifestHash(
                "main",
                modules
            );
            REQUIRE(moduleManifestHash.has_value());
            auto const environmentHash = currentProjectPluginEnvironmentHash();
            REQUIRE(environmentHash.has_value());
            return verifiedRegistration(ProjectRegistrationClaims{
                .projectRegistrationFormat            = k_projectRegistrationFormat,
                .pluginId                             = std::string{k_pluginId},
                .pluginModuleManifestHash             = *moduleManifestHash,
                .pluginEnvironmentHash                = *environmentHash,
                .toolCatalogHash                      = hashOf(k_toolCatalogBytes),
                .projectStateSchemaHash               = hashOf("state"),
                .projectObservationSchemaHash         = hashOf("observation"),
                .projectToolPreconditionSchemaHash    = hashOf("precondition"),
                .reconcilePayloadSchemaManifestHash   = hashOf("reconcile"),
                .journalEventSchemaManifestHash       = hashOf("journal"),
                .baselineEventType                    = "chaos.baseline",
                .projectResources                     = {},
                .observedInstanceIdentitySchemaHashes = {},
                .projectToolBindings                  = std::move(bindings),
            });
        }

        [[nodiscard]]
        auto catalogOver(
            VerifiedProjectRegistration const& registration,
            std::vector<std::string> toolNames
        ) -> ProjectToolCatalogSchemaOwner
        {
            auto owner = ProjectToolCatalogSchemaOwner::create(
                registration,
                k_toolCatalogBytes,
                [names = std::move(toolNames)]()
                    -> Result<std::vector<ToolCatalogEntry>>
                {
                    auto entries = std::vector<ToolCatalogEntry>{};
                    for (auto const& name : names)
                    {
                        entries.emplace_back(declaredTool(name));
                    }
                    return entries;
                },
                [](std::string_view, std::string_view) -> Status { return ok(); }
            );
            REQUIRE(owner.has_value());
            return *std::move(owner);
        }

        [[nodiscard]]
        auto bothBindings() -> std::vector<ProjectToolBinding>
        {
            return {
                ProjectToolBinding{
                    .toolName   = std::string{k_dismissTool},
                    .entryPoint = "dismiss",
                },
                ProjectToolBinding{
                    .toolName   = std::string{k_sweepTool},
                    .entryPoint = "sweep",
                },
            };
        }

        [[nodiscard]]
        auto bothTools() -> std::vector<std::string>
        {
            return {std::string{k_dismissTool}, std::string{k_sweepTool}};
        }
    } // namespace

    TEST_CASE("a Project registration generation loads as one program over its bound entries")
    {
        auto const registration = registrationOver(k_projectSource, bothBindings());
        auto const catalog      = catalogOver(registration, bothTools());
        auto const exported     = std::array{
            std::string{"dismiss"},
            std::string{"sweep"},
        };
        auto const log = std::make_shared<std::vector<ScopedCall>>();

        auto registrar    = ProjectToolProgramRegistrar{};
        auto const loaded = registrar.registerProject(
            registration,
            catalog,
            "main",
            moduleBlobs(k_projectSource),
            {},
            exported,
            acceptingResults(),
            recordingRuntime(log)
        );
        REQUIRE(loaded.has_value());

        CHECK(loaded->pluginId() == std::string{k_pluginId});
        CHECK(loaded->projectRegistrationHash() == registration.hash());
        CHECK(loaded->toolCatalogHash() == registration.toolCatalogHash());

        // One program, and its entry points are the binding table's union
        // rather than anything a caller chose.
        CHECK(
            loaded->bindingTable().entryPoints()
            == std::vector<std::string>{"dismiss", "sweep"}
        );

        auto const framework = FrameworkToolCatalogOwner::create();
        REQUIRE(framework.has_value());
        CHECK(loaded->frameworkToolCatalogHash() == framework->toolCatalogHash());
        auto const scopedEnvironment = currentScopedToolEnvironmentHash();
        REQUIRE(scopedEnvironment.has_value());
        CHECK(loaded->environmentIdentity() == *scopedEnvironment);

        // Both entries run out of the one loaded value. A second program per
        // entry would need a second environment identity, and there is one.
        auto const dismissed = loaded->invokeBoundTool(
            k_dismissTool,
            json::Value::ofObject({{"note", json::Value::ofString("kept")}}),
            script::ScopedRunRequest{.parentPosition = runPosition()}
        );
        REQUIRE(dismissed.has_value());
        auto const swept = loaded->invokeBoundTool(
            k_sweepTool,
            json::Value::ofObject({}),
            script::ScopedRunRequest{.parentPosition = runPosition()}
        );
        REQUIRE(swept.has_value());

        REQUIRE(log->size() == 1U);
        CHECK((*log)[0].toolName == "framework.audit.record");
        CHECK((*log)[0].parentPosition == runPosition().hex());
        CHECK((*log)[0].childIndex == 1U);

        // Only the binding table names an entry, so a Tool it never bound is a
        // refusal rather than a run of something else.
        auto const unknown = loaded->invokeBoundTool(
            "chaos.unknown",
            json::Value::ofObject({}),
            script::ScopedRunRequest{.parentPosition = runPosition()}
        );
        REQUIRE_FALSE(unknown.has_value());
        CHECK(unknown.error().message().contains("binds no Tool named"));

        auto const found = registrar.findExact(
            std::string{k_pluginId},
            registration.hash()
        );
        REQUIRE(found.has_value());
        CHECK(found->environmentIdentity() == loaded->environmentIdentity());

        // A registration root is a generation, so the same root cannot be
        // loaded twice under two programs.
        auto const again = registrar.registerProject(
            registration,
            catalog,
            "main",
            moduleBlobs(k_projectSource),
            {},
            exported,
            acceptingResults(),
            recordingRuntime(log)
        );
        REQUIRE_FALSE(again.has_value());
        CHECK(again.error().message().contains("registration is immutable"));
    }

    TEST_CASE("the loader refuses a binding naming an entry the closure does not export")
    {
        auto const registration = registrationOver(k_projectSource, bothBindings());
        auto const catalog      = catalogOver(registration, bothTools());

        // The closure offers only `dismiss`, and the table binds `sweep` too.
        auto const exported = std::array{std::string{"dismiss"}};

        auto registrar     = ProjectToolProgramRegistrar{};
        auto const refused = registrar.registerProject(
            registration,
            catalog,
            "main",
            moduleBlobs(k_projectSource),
            {},
            exported,
            acceptingResults(),
            recordingRuntime(std::make_shared<std::vector<ScopedCall>>())
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message().contains(
            "bound to entry sweep, which the Project closure does not export"
        ));
    }

    TEST_CASE("the loader refuses a Project-provided descriptor with no binding")
    {
        auto const registration = registrationOver(
            k_projectSource,
            {
                ProjectToolBinding{
                    .toolName   = std::string{k_dismissTool},
                    .entryPoint = "dismiss",
                },
            }
        );
        auto const catalog  = catalogOver(registration, bothTools());
        auto const exported = std::array{
            std::string{"dismiss"},
            std::string{"sweep"},
        };

        auto registrar     = ProjectToolProgramRegistrar{};
        auto const refused = registrar.registerProject(
            registration,
            catalog,
            "main",
            moduleBlobs(k_projectSource),
            {},
            exported,
            acceptingResults(),
            recordingRuntime(std::make_shared<std::vector<ScopedCall>>())
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message().contains(
            "chaos.project.sweep is declared with no binding to a Project entry"
        ));
    }

    TEST_CASE("the loader refuses a binding with no descriptor")
    {
        auto const registration = registrationOver(k_projectSource, bothBindings());
        auto const catalog      = catalogOver(
            registration,
            {std::string{k_dismissTool}}
        );
        auto const exported = std::array{
            std::string{"dismiss"},
            std::string{"sweep"},
        };

        auto registrar     = ProjectToolProgramRegistrar{};
        auto const refused = registrar.registerProject(
            registration,
            catalog,
            "main",
            moduleBlobs(k_projectSource),
            {},
            exported,
            acceptingResults(),
            recordingRuntime(std::make_shared<std::vector<ScopedCall>>())
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message().contains(
            "names chaos.project.sweep, which this Tool Catalog does not declare"
        ));
    }

    TEST_CASE("a stated export set the closure does not honour survives nothing")
    {
        auto const log = std::make_shared<std::vector<ScopedCall>>();

        SUBCASE("an entry the code does not carry is refused at admission")
        {
            auto const registration = registrationOver(
                k_dismissOnlySource,
                bothBindings()
            );
            auto const catalog  = catalogOver(registration, bothTools());
            auto const exported = std::array{
                std::string{"dismiss"},
                std::string{"sweep"},
            };

            auto registrar     = ProjectToolProgramRegistrar{};
            auto const refused = registrar.registerProject(
                registration,
                catalog,
                "main",
                moduleBlobs(k_dismissOnlySource),
                {},
                exported,
                acceptingResults(),
                recordingRuntime(log)
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains("missing an entry point"));
        }

        SUBCASE("an entry no Tool binds is refused at admission")
        {
            auto const registration = registrationOver(
                k_projectSource,
                {
                    ProjectToolBinding{
                        .toolName   = std::string{k_dismissTool},
                        .entryPoint = "dismiss",
                    },
                }
            );
            auto const catalog  = catalogOver(
                registration,
                {std::string{k_dismissTool}}
            );
            // The closure offers both entries and the contract binds one, so
            // the two sets a program could be compiled with differ here. The
            // program is compiled with the BINDING TABLE's union, so `sweep`
            // is an export nothing declared and admission refuses it; a loader
            // that compiled the stated export set instead would load an entry
            // no Tool binds and this case would pass nothing.
            auto const exported = std::array{
                std::string{"dismiss"},
                std::string{"sweep"},
            };

            auto registrar     = ProjectToolProgramRegistrar{};
            auto const refused = registrar.registerProject(
                registration,
                catalog,
                "main",
                moduleBlobs(k_projectSource),
                {},
                exported,
                acceptingResults(),
                recordingRuntime(log)
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains("undeclared field"));
        }
    }

    TEST_CASE("per-entry ceilings and child sets are catalog data the compiler never sees")
    {
        auto const registration = registrationOver(k_projectSource, bothBindings());
        auto const catalog      = catalogOver(registration, bothTools());
        auto const exported     = std::array{
            std::string{"dismiss"},
            std::string{"sweep"},
        };

        auto registrar = ProjectToolProgramRegistrar{};
        auto const loaded = registrar.registerProject(
            registration,
            catalog,
            "main",
            moduleBlobs(k_projectSource),
            {},
            exported,
            acceptingResults(),
            recordingRuntime(std::make_shared<std::vector<ScopedCall>>())
        );
        REQUIRE(loaded.has_value());

        // Two entries whose declarations differ in exactly the values a
        // per-entry program would have had to be compiled with. They are read
        // from the catalog the handle carries, per call, and one program serves
        // both.
        auto const dismiss = loaded->catalog().describe(k_dismissTool);
        REQUIRE(dismiss.has_value());
        auto const sweep = loaded->catalog().describe(k_sweepTool);
        REQUIRE(sweep.has_value());

        CHECK(dismiss->childEffects.maximumChildCalls == 0U);
        CHECK(dismiss->childEffects.childToolNames.empty());
        CHECK(dismiss->timeout.maximumElapsedMillis == 5'000U);

        CHECK(sweep->childEffects.maximumChildCalls == 4U);
        CHECK(
            sweep->childEffects.childToolNames
            == std::vector<std::string>{std::string{k_dismissTool}}
        );
        CHECK(sweep->timeout.maximumElapsedMillis == 30'000U);

        CHECK(
            loaded->bindingTable().entryPoints()
            == std::vector<std::string>{"dismiss", "sweep"}
        );
    }

    TEST_CASE("a bound entry reads the pinned Tool catalog the loader baked into the program")
    {
        auto const registration = registrationOver(k_projectSource, bothBindings());
        auto const catalog      = catalogOver(registration, bothTools());
        auto const exported     = std::array{
            std::string{"dismiss"},
            std::string{"sweep"},
        };

        auto registrar = ProjectToolProgramRegistrar{};
        auto const loaded = registrar.registerProject(
            registration,
            catalog,
            "main",
            moduleBlobs(k_projectSource),
            {},
            exported,
            acceptingResults(),
            recordingRuntime(std::make_shared<std::vector<ScopedCall>>())
        );
        REQUIRE(loaded.has_value());

        auto const swept = loaded->invokeBoundTool(
            k_sweepTool,
            json::Value::ofObject({}),
            script::ScopedRunRequest{.parentPosition = runPosition()}
        );
        REQUIRE(swept.has_value());

        // Both catalogs are reachable from a scoped run: the Framework's are
        // how a handler reaches the world, the Project's own are how one Tool
        // composes another.
        auto const answer = json::canonicalBytes(*swept);
        CHECK(answer.contains(R"("knows_framework":true)"));
        CHECK(answer.contains(R"("knows_sibling":true)"));
        CHECK(answer.contains(R"("catalog_hash":")"));
    }

    TEST_CASE("the loader refuses a registration that binds no Tool at all")
    {
        auto const registration = registrationOver(k_dismissOnlySource, {});
        auto const catalog      = catalogOver(
            registration,
            {std::string{k_dismissTool}}
        );
        auto const exported = std::array{std::string{"dismiss"}};

        auto registrar     = ProjectToolProgramRegistrar{};
        auto const refused = registrar.registerProject(
            registration,
            catalog,
            "main",
            moduleBlobs(k_dismissOnlySource),
            {},
            exported,
            acceptingResults(),
            recordingRuntime(std::make_shared<std::vector<ScopedCall>>())
        );
        REQUIRE_FALSE(refused.has_value());

        // Every registration the project directory loader derives today states
        // the empty binding table, and this is what that means: a catalog that
        // declares a Tool nothing binds is refused, so no such registration can
        // load a Tool program at all.
        CHECK(refused.error().message().contains(
            "declared with no binding to a Project entry"
        ));
    }

    TEST_CASE("the loader refuses a closure or environment this registration never pinned")
    {
        auto const bindings = bothBindings();

        SUBCASE("a closure whose manifest is not the pinned one")
        {
            auto const registration = registrationOver(k_projectSource, bindings);
            auto const catalog      = catalogOver(registration, bothTools());
            auto const exported     = std::array{
                std::string{"dismiss"},
                std::string{"sweep"},
            };

            auto registrar     = ProjectToolProgramRegistrar{};
            auto const refused = registrar.registerProject(
                registration,
                catalog,
                "main",
                moduleBlobs(k_dismissOnlySource),
                {},
                exported,
                acceptingResults(),
                recordingRuntime(std::make_shared<std::vector<ScopedCall>>())
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "module closure does not match the verified registration"
            ));
        }

        SUBCASE("an environment other than the one running")
        {
            auto const modules = moduleBlobs(k_projectSource);
            auto const moduleManifestHash = derivePluginModuleManifestHash(
                "main",
                modules
            );
            REQUIRE(moduleManifestHash.has_value());
            auto const registration = verifiedRegistration(
                ProjectRegistrationClaims{
                    .projectRegistrationFormat            = k_projectRegistrationFormat,
                    .pluginId                             = std::string{k_pluginId},
                    .pluginModuleManifestHash             = *moduleManifestHash,
                    .pluginEnvironmentHash                = hashOf("another-environment"),
                    .toolCatalogHash                      = hashOf(k_toolCatalogBytes),
                    .projectStateSchemaHash               = hashOf("state"),
                    .projectObservationSchemaHash         = hashOf("observation"),
                    .projectToolPreconditionSchemaHash    = hashOf("precondition"),
                    .reconcilePayloadSchemaManifestHash   = hashOf("reconcile"),
                    .journalEventSchemaManifestHash       = hashOf("journal"),
                    .baselineEventType                    = "chaos.baseline",
                    .projectResources                     = {},
                    .observedInstanceIdentitySchemaHashes = {},
                    .projectToolBindings                  = bindings,
                }
            );
            auto const catalog  = catalogOver(registration, bothTools());
            auto const exported = std::array{
                std::string{"dismiss"},
                std::string{"sweep"},
            };

            auto registrar     = ProjectToolProgramRegistrar{};
            auto const refused = registrar.registerProject(
                registration,
                catalog,
                "main",
                moduleBlobs(k_projectSource),
                {},
                exported,
                acceptingResults(),
                recordingRuntime(std::make_shared<std::vector<ScopedCall>>())
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "environment does not match the verified registration"
            ));
        }

        SUBCASE("a program whose answers nothing would judge")
        {
            auto const registration = registrationOver(k_projectSource, bindings);
            auto const catalog      = catalogOver(registration, bothTools());
            auto const exported     = std::array{
                std::string{"dismiss"},
                std::string{"sweep"},
            };

            auto registrar     = ProjectToolProgramRegistrar{};
            auto const refused = registrar.registerProject(
                registration,
                catalog,
                "main",
                moduleBlobs(k_projectSource),
                {},
                exported,
                ToolResultValidator{},
                recordingRuntime(std::make_shared<std::vector<ScopedCall>>())
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "requires a result schema validator"
            ));
        }

        SUBCASE("a program with no Tool Runtime seam at all")
        {
            auto const registration = registrationOver(k_projectSource, bindings);
            auto const catalog      = catalogOver(registration, bothTools());
            auto const exported     = std::array{
                std::string{"dismiss"},
                std::string{"sweep"},
            };

            auto registrar     = ProjectToolProgramRegistrar{};
            auto const refused = registrar.registerProject(
                registration,
                catalog,
                "main",
                moduleBlobs(k_projectSource),
                {},
                exported,
                acceptingResults(),
                script::ToolRuntimeInvoke{}
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "requires a Tool Runtime"
            ));
        }
    }

    TEST_CASE("the scoped environment identity is its own, beside the pure plugin one")
    {
        auto const scoped = currentScopedToolEnvironmentHash();
        REQUIRE(scoped.has_value());
        auto const pure = currentProjectPluginEnvironmentHash();
        REQUIRE(pure.has_value());
        CHECK(*scoped != *pure);

        auto const material = currentScopedToolEnvironmentMaterial();
        REQUIRE(material.has_value());
        for (auto const scopedName : script::ScopedToolProgram::scopedModuleNames())
        {
            INFO("scoped module: ", scopedName);
            CHECK(material->contains(scopedName));
        }

        auto const pureMaterial = currentProjectPluginEnvironmentMaterial();
        REQUIRE(pureMaterial.has_value());
        for (auto const scopedName : script::ScopedToolProgram::scopedModuleNames())
        {
            INFO("scoped module: ", scopedName);
            CHECK_FALSE(pureMaterial->contains(scopedName));
        }
    }
}

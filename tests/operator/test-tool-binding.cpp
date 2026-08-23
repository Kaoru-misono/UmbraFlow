#include <operator/manifest.hpp>
#include <operator/tool-invocation.hpp>

#include <json/value.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::operator_runtime
{
    namespace
    {
        // The catalog bytes are opaque here on purpose: the owner proves they
        // hash to what the registration pinned, and the descriptors it answers
        // with arrive through the reader. What this file is about is the join
        // between the names those bytes declare and the entries the closure
        // exports, and neither side needs a real schema document to be wrong.
        constexpr auto k_toolCatalogBytes = std::string_view{
            R"({"schema":"umbraflow-tool-catalog/v1","tools":["dismiss","sweep"]})"
        };

        constexpr auto k_firstTool  = std::string_view{"chaos.project.dismiss"};
        constexpr auto k_secondTool = std::string_view{"chaos.project.sweep"};

        [[nodiscard]]
        auto hashOf(std::string_view value) -> ContentHash
        {
            auto const result = sha256(std::as_bytes(std::span{value}));
            REQUIRE(result.has_value());
            return *result;
        }

        [[nodiscard]]
        auto declaredDescriptor() -> ToolDescriptor
        {
            return ToolDescriptor{
                .toolVersion          = "1",
                .requiredCapabilities = {},
                .effectBounds         = {},
                .uiActionBounds       = {},
                .limits               = WorkflowLimits{
                    .maximumSteps        = 1U,
                    .maximumDispatches   = 1U,
                    .maximumObservations = 1U,
                    .maximumWaits        = 0U,
                    .maximumElapsedMillis = 1'000U,
                },
                .timeout = TimeoutPolicy{
                    .maximumElapsedMillis = 1'000U,
                    .onTimeout            = TimeoutAction::Stop,
                },
                .mutability  = ToolMutability::ReadOnly,
                .surface     = ToolSurface::Semantic,
                .idempotency = ToolIdempotency::ReadSafe,
            };
        }

        [[nodiscard]]
        auto closureValue(ProjectClosureClaims const& closure) -> json::Value
        {
            auto entries = std::vector<json::Value>{};
            entries.reserve(closure.exportedEntryPoints.size());
            for (auto const& entry : closure.exportedEntryPoints)
            {
                entries.emplace_back(json::Value::ofString(entry));
            }
            return json::Value::ofObject({
                {"exported_entry_points", json::Value::ofArray(std::move(entries))},
                {"module_manifest_hash",
                 json::Value::ofString(closure.moduleManifestHash.hex())},
            });
        }

        [[nodiscard]]
        auto generationJcs(ProjectGenerationClaims const& claims) -> std::string
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
                {"observed_instance_identity_schema_hashes",
                 json::Value::ofArray({})},
                {"plugin_environment_hash",
                 json::Value::ofString(claims.pluginEnvironmentHash.hex())},
                {"plugin_id", json::Value::ofString(claims.pluginId)},
                {"project_registration_format",
                 json::Value::ofNumber(
                     static_cast<double>(claims.projectRegistrationFormat)
                 )},
                {"project_resources", json::Value::ofArray({})},
                {"project_tool_bindings", json::Value::ofArray(std::move(bindings))},
                {"tool_catalog_hash",
                 json::Value::ofString(claims.toolCatalogHash.hex())},
                {"tool_closure", closureValue(claims.toolClosure)},
            }));
        }

        // The tool closure a generation carrying these bindings would state:
        // the sorted, unique union of the entries they name. It is derived here
        // because these cases are about the TABLE, and bind() takes the
        // exported set as its own parameter -- that set is what a loader
        // observed, and each case below states it deliberately.
        [[nodiscard]]
        auto declaredEntries(
            std::vector<ProjectToolBinding> const& bindings
        ) -> std::vector<std::string>
        {
            auto entries = std::vector<std::string>{};
            entries.reserve(bindings.size());
            for (auto const& binding : bindings)
            {
                entries.emplace_back(binding.entryPoint);
            }
            std::ranges::sort(entries);
            auto const repeated = std::ranges::unique(entries);
            entries.erase(repeated.begin(), repeated.end());
            return entries;
        }

        // A verified generation over exactly this binding table. The reader
        // answers for the bytes this fixture built and nothing else, which is
        // the same contract the deployment loader's validator satisfies.
        [[nodiscard]]
        auto registrationBinding(
            std::vector<ProjectToolBinding> bindings,
            std::string_view catalogBytes = k_toolCatalogBytes
        ) -> VerifiedProjectGeneration
        {
            auto entries = declaredEntries(bindings);
            auto claims  = ProjectGenerationClaims{
                .projectRegistrationFormat = k_projectGenerationFormat,
                .pluginId                  = "chaos.project",
                .toolClosure               = ProjectClosureClaims{
                    .moduleManifestHash  = hashOf("tool-modules"),
                    .exportedEntryPoints = std::move(entries),
                },
                .pluginEnvironmentHash = hashOf("environment"),
                .toolCatalogHash       = hashOf(catalogBytes),
                .projectResources      = {},
                .projectToolBindings   = std::move(bindings),
            };
            auto const exactJcs = generationJcs(claims);
            auto registration   = ProjectGeneration::verifyExact(
                exactJcs,
                hashOf(exactJcs),
                [exactJcs, claims](std::string_view candidate)
                    -> Result<ProjectGenerationClaims>
                {
                    if (candidate != exactJcs)
                    {
                        return fail(
                            AutomationErrorKind::InvalidResource,
                            "binding fixture registration is not exact JCS"
                        );
                    }
                    return claims;
                }
            );
            REQUIRE(registration.has_value());
            return *std::move(registration);
        }

        [[nodiscard]]
        auto catalogOver(
            VerifiedProjectGeneration const& registration,
            std::vector<std::string> toolNames,
            std::string_view catalogBytes = k_toolCatalogBytes
        ) -> ProjectToolCatalogSchemaOwner
        {
            auto owner = ProjectToolCatalogSchemaOwner::create(
                registration,
                catalogBytes,
                [names = std::move(toolNames)]()
                    -> Result<std::vector<ToolCatalogEntry>>
                {
                    auto entries = std::vector<ToolCatalogEntry>{};
                    entries.reserve(names.size());
                    for (auto const& name : names)
                    {
                        entries.emplace_back(ToolCatalogEntry{
                            .name       = name,
                            .descriptor = declaredDescriptor(),
                        });
                    }
                    return entries;
                },
                [](std::string_view, std::string_view) -> Status { return ok(); }
            );
            REQUIRE(owner.has_value());
            return *std::move(owner);
        }
    }

    TEST_CASE("a bound Tool table joins every declared Tool to an exported entry")
    {
        auto const registration = registrationBinding({
            ProjectToolBinding{
                .toolName   = std::string{k_firstTool},
                .entryPoint = "dismiss",
            },
            ProjectToolBinding{
                .toolName   = std::string{k_secondTool},
                .entryPoint = "sweep",
            },
        });
        auto const catalog = catalogOver(
            registration,
            {std::string{k_firstTool}, std::string{k_secondTool}}
        );
        auto const exported = std::array{
            std::string{"dismiss"},
            std::string{"sweep"},
        };

        auto const table = ProjectToolBindingTable::bind(
            registration,
            catalog,
            exported
        );
        REQUIRE(table.has_value());
        CHECK(table->projectRegistrationHash() == registration.hash());

        auto const first = table->entryPointFor(k_firstTool);
        REQUIRE(first.has_value());
        CHECK(*first == "dismiss");

        // The program a Project compiles per registration generation is
        // compiled with exactly this set, so it is the union of bound entries
        // rather than one program per entry.
        CHECK(
            table->entryPoints()
            == std::vector<std::string>{"dismiss", "sweep"}
        );

        // Only bind() mints a table, so a name it never bound is a refusal
        // rather than an absent entry a caller could read a default into.
        auto const unbound = table->entryPointFor("chaos.unknown");
        REQUIRE_FALSE(unbound.has_value());
        CHECK(unbound.error().message().contains("binds no Tool named"));
    }

    TEST_CASE("a binding naming an entry the closure does not export is refused")
    {
        auto const registration = registrationBinding({
            ProjectToolBinding{
                .toolName   = std::string{k_firstTool},
                .entryPoint = "dismiss",
            },
        });
        auto const catalog = catalogOver(registration, {std::string{k_firstTool}});
        auto const exported = std::array{std::string{"sweep"}};

        auto const refused = ProjectToolBindingTable::bind(
            registration,
            catalog,
            exported
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message().contains(
            "bound to entry dismiss, which the Project closure does not export"
        ));
    }

    TEST_CASE("a Project-provided descriptor with no binding is refused")
    {
        auto const registration = registrationBinding({
            ProjectToolBinding{
                .toolName   = std::string{k_firstTool},
                .entryPoint = "dismiss",
            },
        });
        auto const catalog = catalogOver(
            registration,
            {std::string{k_firstTool}, std::string{k_secondTool}}
        );
        auto const exported = std::array{std::string{"dismiss"}};

        auto const refused = ProjectToolBindingTable::bind(
            registration,
            catalog,
            exported
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message().contains(
            "chaos.project.sweep is declared with no binding to a Project entry"
        ));
    }

    TEST_CASE("a binding with no descriptor is refused")
    {
        auto const registration = registrationBinding({
            ProjectToolBinding{
                .toolName   = std::string{k_firstTool},
                .entryPoint = "dismiss",
            },
            ProjectToolBinding{
                .toolName   = std::string{k_secondTool},
                .entryPoint = "sweep",
            },
        });
        auto const catalog = catalogOver(registration, {std::string{k_firstTool}});
        auto const exported = std::array{
            std::string{"dismiss"},
            std::string{"sweep"},
        };

        auto const refused = ProjectToolBindingTable::bind(
            registration,
            catalog,
            exported
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message().contains(
            "names chaos.project.sweep, which this Tool Catalog does not declare"
        ));
    }

    TEST_CASE("a binding table refuses a catalog this registration never pinned")
    {
        auto const registration = registrationBinding({
            ProjectToolBinding{
                .toolName   = std::string{k_firstTool},
                .entryPoint = "dismiss",
            },
        });
        constexpr auto k_foreignBytes = std::string_view{
            R"({"schema":"umbraflow-tool-catalog/v1","tools":["dismiss"]})"
        };
        auto const foreign = registrationBinding(
            {
                ProjectToolBinding{
                    .toolName   = std::string{k_firstTool},
                    .entryPoint = "dismiss",
                },
            },
            k_foreignBytes
        );
        auto const foreignCatalog = catalogOver(
            foreign,
            {std::string{k_firstTool}},
            k_foreignBytes
        );
        auto const exported = std::array{std::string{"dismiss"}};

        auto const refused = ProjectToolBindingTable::bind(
            registration,
            foreignCatalog,
            exported
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message().contains(
            "requires the Tool Catalog this registration pinned"
        ));
    }

    TEST_CASE("a registration states its Tool bindings sorted and unique")
    {
        auto claims = ProjectGenerationClaims{
            .projectRegistrationFormat = k_projectGenerationFormat,
            .pluginId                  = "chaos.project",
            .toolClosure               = ProjectClosureClaims{
                .moduleManifestHash  = hashOf("tool-modules"),
                .exportedEntryPoints = {"dismiss", "sweep"},
            },
            .pluginEnvironmentHash = hashOf("environment"),
            .toolCatalogHash       = hashOf(k_toolCatalogBytes),
            .projectResources      = {},
            .projectToolBindings   = {
                ProjectToolBinding{
                    .toolName   = std::string{k_secondTool},
                    .entryPoint = "sweep",
                },
                ProjectToolBinding{
                    .toolName   = std::string{k_firstTool},
                    .entryPoint = "dismiss",
                },
            },
        };
        auto const exactJcs = generationJcs(claims);

        auto const refused = ProjectGeneration::verifyExact(
            exactJcs,
            hashOf(exactJcs),
            [claims](std::string_view) -> Result<ProjectGenerationClaims>
            {
                return claims;
            }
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message().contains(
            "Tool bindings must be unique and JCS-ordered by tool name"
        ));
    }

    // The binding carries the same Tool name the catalog declares, in the same
    // one spelling: namespaced. A registration stating a bare local name binds
    // a Tool no catalog could ever declare, and it is refused where the
    // registration is read rather than one document later.
    TEST_CASE("a registration's Tool binding names a namespaced Tool")
    {
        auto claims = ProjectGenerationClaims{
            .projectRegistrationFormat = k_projectGenerationFormat,
            .pluginId                  = "chaos.project",
            .toolClosure               = ProjectClosureClaims{
                .moduleManifestHash  = hashOf("tool-modules"),
                .exportedEntryPoints = {"dismiss"},
            },
            .pluginEnvironmentHash = hashOf("environment"),
            .toolCatalogHash       = hashOf(k_toolCatalogBytes),
            .projectResources      = {},
            .projectToolBindings   = {
                ProjectToolBinding{
                    .toolName   = "dismiss",
                    .entryPoint = "dismiss",
                },
            },
        };
        auto const exactJcs = generationJcs(claims);

        auto const refused = ProjectGeneration::verifyExact(
            exactJcs,
            hashOf(exactJcs),
            [claims](std::string_view) -> Result<ProjectGenerationClaims>
            {
                return claims;
            }
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message().contains(
            "tool binding name is not a canonical namespaced name"
        ));
    }
}

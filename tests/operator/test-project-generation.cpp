#include <operator/ledger.hpp>
#include <operator/manifest.hpp>
#include <operator/project-generation.hpp>
#include <operator/project-plugin.hpp>
#include <operator/project-tool-program.hpp>

#include "project-fixture.hpp"

#include <script/pure-data-program.hpp>
#include <script/scoped-tool-program.hpp>

#include <task/framework-bundle.hpp>

#include <json/schema.hpp>
#include <json/value.hpp>

#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The one-closure registration generation, which is now the only one there
// is: modules/deployment writes this document, the CLI and ProductLifecycle
// load it, and the two-closure reader it replaced is gone rather than kept
// beside it. Nothing inspects a document to decide which reader gets it,
// because there is one reader.
//
// The builder in this file's anonymous namespace writes the document by hand so
// that a case can state a claim no authoring path would produce. What these
// cases have to prove is what a successful load cannot show: that the closure
// slot is mandatory, that `exported_entry_points` is mandatory inside it, and
// that the export join fails on each of its legs independently -- a declaration
// that overstates, a declaration that understates, and a closure whose real
// exports disagree with a declaration that looked accurate.
namespace uf::operator_runtime
{
    namespace
    {
        // The format the two-closure registration document stated. It is a
        // number here rather than a constant because the constant that named
        // it died with its reader; keeping the number is what makes "this
        // reader accepts its own generation and nothing else" red against the
        // document generation the state cut replaced.
        constexpr auto k_twoClosureRegistrationFormat = uint64{6U};

        constexpr auto k_pluginId     = std::string_view{"chaos.project"};
        constexpr auto k_dismissTool  = std::string_view{"chaos.project.dismiss"};
        constexpr auto k_sweepTool    = std::string_view{"chaos.project.sweep"};
        constexpr auto k_schemaPath   = std::string_view{
            "schema/umbraflow-project-registration-v4.schema.json"
        };

        // The catalog bytes are opaque on purpose: the owner proves they hash
        // to what the generation pinned, and the descriptors it answers with
        // arrive through the reader. What is under test is the join between the
        // names those bytes declare and the entries a closure exports.
        constexpr auto k_toolCatalogBytes = std::string_view{
            R"({"schema":"umbraflow-tool-catalog/v1","tools":["dismiss","sweep"]})"
        };

        // A closure that exports nothing but its identity: the tool closure a
        // project that binds no Tool ships, which is a real closure with an
        // explicitly empty declared entry set and never an absent slot.
        constexpr auto k_identityOnlySource = std::string_view{R"LUAU(
return {
    plugin_id = "chaos.project",
}
)LUAU"};

        // The tool closure: two bound entries and nothing else.
        //
        // It requires @umbraflow/tools at the top, which is only loadable if
        // the registrar baked this generation's pinned discovery resource into
        // the scoped program. `sweep` answers out of that frozen table without
        // spending a Tool call, so a load that skipped the resource cannot
        // reach any assertion here.
        constexpr auto k_toolSource = std::string_view{R"LUAU(
local tools = require("@umbraflow/tools")

return {
    plugin_id = "chaos.project",
    dismiss = function(input)
        return { dismissed = input.note }
    end,
    sweep = function(_input)
        return {
            catalog_hash = tools.catalog_hash,
            knows_framework = tools.knows("framework.audit.record"),
            knows_sibling = tools.knows("chaos.project.dismiss"),
        }
    end,
}
)LUAU"};

        // The same closure with `sweep` removed, for the leg where an accurate
        // looking declaration names an entry the shipped bytes do not carry.
        constexpr auto k_dismissOnlyToolSource = std::string_view{R"LUAU(
return {
    plugin_id = "chaos.project",
    dismiss = function(input)
        return { dismissed = input.note }
    end,
}
)LUAU"};

        // The same closure with one field beyond the two it declares, for the
        // mirror leg: an export the declaration never named.
        constexpr auto k_extraFieldToolSource = std::string_view{R"LUAU(
return {
    plugin_id = "chaos.project",
    dismiss = function(input)
        return { dismissed = input.note }
    end,
    sweep = function(_input)
        return { swept = true }
    end,
    unbound = function(_input)
        return { reached = true }
    end,
}
)LUAU"};

        [[nodiscard]]
        auto hashOf(std::string_view value) -> ContentHash
        {
            auto const result = sha256(std::as_bytes(std::span{value}));
            REQUIRE(result.has_value());
            return *result;
        }

        // The durable position a run is anchored on. A run is never anchored
        // on nothing, so there is no spelling of "no position" here either.
        [[nodiscard]]
        auto runPosition() -> ContentHash
        {
            return hashOf("generation-run-position");
        }

        [[nodiscard]]
        auto repositoryRoot() -> std::filesystem::path
        {
            auto candidate = std::filesystem::path{__FILE__};
            if (candidate.is_relative())
            {
                candidate = std::filesystem::absolute(candidate);
            }
            candidate = candidate.parent_path();
            while (!candidate.empty())
            {
                if (std::filesystem::is_directory(candidate / "schema"))
                {
                    return candidate;
                }
                auto const parent = candidate.parent_path();
                if (parent == candidate)
                {
                    break;
                }
                candidate = parent;
            }
            FAIL("repository root containing schema/ was not found");
            return {};
        }

        // The authored bytes of the registration document contract, held for
        // the process because a compiled json::Schema is only as durable as
        // the bytes it was compiled from.
        [[nodiscard]]
        auto registrationSchemaBytes() -> std::string const&
        {
            static auto const bytes = [] {
                auto stream = std::ifstream{
                    repositoryRoot() / k_schemaPath,
                    std::ios::binary,
                };
                REQUIRE(stream.good());
                return std::string{
                    std::istreambuf_iterator<char>{stream},
                    std::istreambuf_iterator<char>{},
                };
            }();
            return bytes;
        }

        // The member named, or an empty value. The schema has already refused
        // any document missing a member this reader goes on to name, so the
        // empty case is unreachable; it exists so that extraction below needs
        // no branch of its own, and an empty value still lands on a refusal
        // rather than on undefined behaviour.
        [[nodiscard]]
        auto memberOf(
            json::Value const& document,
            std::string_view name
        ) -> json::Value
        {
            auto const* const found = document.find(name);
            if (found == nullptr)
            {
                return json::Value{};
            }
            return *found;
        }

        // The document spells a digest as bare lowercase hex. ContentHash's own
        // spelling carries the algorithm prefix, and this is the one place the
        // two meet.
        [[nodiscard]]
        auto hashMember(
            json::Value const& document,
            std::string_view name
        ) -> Result<ContentHash>
        {
            return ContentHash::parse(
                "sha256:" + std::string{memberOf(document, name).string()}
            );
        }

        [[nodiscard]]
        auto readClosure(
            json::Value const& document,
            std::string_view name
        ) -> Result<ProjectClosureClaims>
        {
            auto const closure = memberOf(document, name);
            UF_TRY_VALUE(
                moduleManifestHash,
                hashMember(closure, "module_manifest_hash")
            );
            auto entries = std::vector<std::string>{};
            for (
                auto const& entry
                : memberOf(closure, "exported_entry_points").items()
            )
            {
                entries.emplace_back(entry.string());
            }
            return ProjectClosureClaims{
                .moduleManifestHash  = moduleManifestHash,
                .exportedEntryPoints = std::move(entries),
            };
        }

        // The reader of the registration document, and the only one there is.
        // It accepts exactly what schema/umbraflow-project-registration-v4
        // describes and refuses everything else; the two-closure document has
        // no reader at all any more.
        //
        // It is this file's own rather than modules/deployment's because these
        // cases must hand the reader documents no authoring path writes. The
        // production reader lives in modules/deployment and is exercised
        // through it; what is compiled against the same schema bytes here is
        // the shape both must agree on.
        [[nodiscard]]
        auto readRegistrationDocument(
            std::string_view exactJcs
        ) -> Result<ProjectGenerationClaims>
        {
            UF_TRY(withContext(
                json::requireExactCanonical(exactJcs),
                "the registration is not exact RFC 8785 JCS"
            ));
            UF_TRY_VALUE(document, json::parse(exactJcs));
            UF_TRY_VALUE(
                schema,
                json::Schema::compile(json::Schema::Document{
                    .label      = k_schemaPath,
                    .exactBytes = registrationSchemaBytes(),
                })
            );
            UF_TRY(withContext(
                schema.validate(document),
                "validating the ProjectRegistration document"
            ));

            UF_TRY_VALUE(toolClosure, readClosure(document, "tool_closure"));
            UF_TRY_VALUE(
                environmentHash,
                hashMember(document, "plugin_environment_hash")
            );
            UF_TRY_VALUE(
                toolCatalogHash,
                hashMember(document, "tool_catalog_hash")
            );

            auto bindings = std::vector<ProjectToolBinding>{};
            for (
                auto const& row
                : memberOf(document, "project_tool_bindings").items()
            )
            {
                bindings.emplace_back(ProjectToolBinding{
                    .toolName   = std::string{memberOf(row, "tool_name").string()},
                    .entryPoint = std::string{memberOf(row, "entry_point").string()},
                });
            }

            return ProjectGenerationClaims{
                .projectRegistrationFormat = static_cast<uint64>(
                    memberOf(document, "project_registration_format").number()
                ),
                .pluginId    = std::string{memberOf(document, "plugin_id").string()},
                .toolClosure = std::move(toolClosure),

                .pluginEnvironmentHash                = environmentHash,
                .toolCatalogHash                      = toolCatalogHash,
                .projectResources                     = {},
                .observedInstanceIdentitySchemaHashes = {},
                .projectToolBindings                  = std::move(bindings),
            };
        }

        [[nodiscard]]
        auto moduleBlobs(std::string_view source)
            -> std::vector<ProjectModuleBlob>
        {
            auto blobs = std::vector<ProjectModuleBlob>{};
            blobs.emplace_back(ProjectModuleBlob{
                .name   = "main",
                .source = std::string{source},
            });
            return blobs;
        }

        [[nodiscard]]
        auto closureModules(std::string_view source)
            -> ProjectGenerationRegistrar::ClosureModules
        {
            return ProjectGenerationRegistrar::ClosureModules{
                .entryModule = "main",
                .modules     = moduleBlobs(source),
            };
        }

        [[nodiscard]]
        auto manifestHashOf(std::string_view source) -> ContentHash
        {
            auto const hash = derivePluginModuleManifestHash(
                "main",
                moduleBlobs(source)
            );
            REQUIRE(hash.has_value());
            return *hash;
        }

        [[nodiscard]]
        auto runningEnvironmentHash() -> ContentHash
        {
            auto const hash = currentProjectPluginEnvironmentHash();
            REQUIRE(hash.has_value());
            return *hash;
        }

        [[nodiscard]]
        auto entryPointArray(std::vector<std::string> const& entries)
            -> json::Value
        {
            auto items = std::vector<json::Value>{};
            items.reserve(entries.size());
            for (auto const& entry : entries)
            {
                items.emplace_back(json::Value::ofString(entry));
            }
            return json::Value::ofArray(std::move(items));
        }

        [[nodiscard]]
        auto closureValue(
            std::string_view source,
            std::vector<std::string> const& declaredEntryPoints
        ) -> json::Value
        {
            return json::Value::ofObject({
                {"exported_entry_points", entryPointArray(declaredEntryPoints)},
                {"module_manifest_hash",
                 json::Value::ofString(manifestHashOf(source).hex())},
            });
        }

        // The registration document, exactly as an authoring path would write
        // it. Every digest is derived from the bytes it describes rather than
        // stated, because a builder that stated them would prove nothing about
        // the loader's own checks; the declared entry set is the one thing a
        // caller states, which is the whole subject of these cases.
        [[nodiscard]]
        auto documentOver(
            std::string_view toolSource,
            std::vector<std::string> const& declaredToolEntries,
            std::vector<ProjectToolBinding> const& bindings
        ) -> json::Value
        {
            auto rows = std::vector<json::Value>{};
            rows.reserve(bindings.size());
            for (auto const& binding : bindings)
            {
                rows.emplace_back(json::Value::ofObject({
                    {"entry_point", json::Value::ofString(binding.entryPoint)},
                    {"tool_name", json::Value::ofString(binding.toolName)},
                }));
            }
            return json::Value::ofObject({
                {"observed_instance_identity_schema_hashes",
                 json::Value::ofArray({})},
                {"plugin_environment_hash",
                 json::Value::ofString(runningEnvironmentHash().hex())},
                {"plugin_id", json::Value::ofString(std::string{k_pluginId})},
                {"project_registration_format",
                 json::Value::ofNumber(
                     static_cast<double>(k_projectGenerationFormat)
                 )},
                {"project_resources", json::Value::ofArray({})},
                {"project_tool_bindings", json::Value::ofArray(std::move(rows))},
                {"tool_catalog_hash",
                 json::Value::ofString(hashOf(k_toolCatalogBytes).hex())},
                {"tool_closure", closureValue(toolSource, declaredToolEntries)},
            });
        }

        [[nodiscard]]
        auto withoutMember(
            json::Value const& document,
            std::string_view name
        ) -> json::Value
        {
            auto members = std::vector<json::Member>{};
            for (auto const& member : document.members())
            {
                if (member.first != name)
                {
                    members.emplace_back(member);
                }
            }
            return json::Value::ofObject(std::move(members));
        }

        [[nodiscard]]
        auto withMember(
            json::Value const& document,
            std::string name,
            json::Value value
        ) -> json::Value
        {
            auto stripped = withoutMember(document, name);
            auto members  = std::vector<json::Member>{
                stripped.members().begin(),
                stripped.members().end(),
            };
            members.emplace_back(json::Member{std::move(name), std::move(value)});
            return json::Value::ofObject(std::move(members));
        }

        [[nodiscard]]
        auto verifiedGeneration(
            json::Value const& document
        ) -> Result<VerifiedProjectGeneration>
        {
            auto exactJcs = json::canonicalBytes(document);
            auto const rootHash = hashOf(exactJcs);
            return ProjectGeneration::verifyExact(
                std::move(exactJcs),
                rootHash,
                readRegistrationDocument
            );
        }

        [[nodiscard]]
        auto generationOver(
            std::string_view toolSource,
            std::vector<std::string> const& declaredToolEntries,
            std::vector<ProjectToolBinding> const& bindings
        ) -> VerifiedProjectGeneration
        {
            auto generation = verifiedGeneration(documentOver(
                toolSource,
                declaredToolEntries,
                bindings
            ));
            REQUIRE(generation.has_value());
            return *std::move(generation);
        }

        // These cases are about the closure and the join, so the seam answers
        // anything. It is required all the same: a scoped program with no Tool
        // Runtime is not a scoped program at all.
        [[nodiscard]]
        auto acceptingRuntime() -> script::ToolRuntimeInvoke
        {
            return [](
                       std::string_view toolName,
                       json::Value const& arguments,
                       script::ToolCallCoordinate const&,
                       std::stop_token
                   ) -> Result<json::Value>
            {
                return json::Value::ofObject({
                    {"result", arguments},
                    {"tool", json::Value::ofString(std::string{toolName})},
                });
            };
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
        auto bothEntries() -> std::vector<std::string>
        {
            return {"dismiss", "sweep"};
        }

        [[nodiscard]]
        auto bothTools() -> std::vector<std::string>
        {
            return {std::string{k_dismissTool}, std::string{k_sweepTool}};
        }

        [[nodiscard]]
        auto declaredTool(std::string_view name) -> ToolCatalogEntry
        {
            return ToolCatalogEntry{
                .name       = std::string{name},
                .descriptor = ToolDescriptor{
                    .toolVersion = "1",
                    .childEffects = ChildEffectDeclaration{
                        .maximumChildSurface    = ToolSurface::Semantic,
                        .maximumChildMutability = ToolMutability::ReadOnly,
                        .maximumChildRisk       = Risk::ReadOnly,
                        .maximumChildCalls      = 0U,
                    },
                    .timeout = TimeoutPolicy{
                        .maximumElapsedMillis = 5'000U,
                        .onTimeout            = TimeoutAction::Stop,
                    },
                    .mutability  = ToolMutability::ReadOnly,
                    .surface     = ToolSurface::Semantic,
                    .idempotency = ToolIdempotency::ReadSafe,
                },
            };
        }

        // The declaration authority this generation pinned, over the exact
        // bytes its tool_catalog_hash names. The generation is handed in as
        // the registration identity every document generation projects into,
        // which is the whole of what this owner reads.
        [[nodiscard]]
        auto catalogOver(
            VerifiedProjectGeneration const& generation,
            std::vector<std::string> toolNames
        ) -> ProjectToolCatalogSchemaOwner
        {
            auto owner = ProjectToolCatalogSchemaOwner::create(
                generation,
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

        // Artifact bytes for the resource-closure legs below. Every blob is
        // valid JSON even where the closure rule is what must refuse it: a blob
        // that is not JSON is refused at admission, which would leave each of
        // those cases red for a reason it does not name.
        constexpr auto k_contentRoot = std::string_view{R"({"root":"content"})"};
        constexpr auto k_otherRoot = std::string_view{R"({"root":"different"})"};
        constexpr auto k_sameSizeWrongRoot = std::string_view{R"({"root":"changed"})"};

        [[nodiscard]]
        auto pinnedResource(
            std::string name,
            std::string_view exactBytes
        ) -> ProjectResource
        {
            return ProjectResource{
                .kind = ProjectResourceKind::Json,
                .name = std::move(name),
                .hash = hashOf(exactBytes),
                .size = static_cast<uint64>(exactBytes.size()),
            };
        }

        [[nodiscard]]
        auto resourceBlob(
            std::string name,
            std::string_view bytes,
            ProjectResourceKind kind = ProjectResourceKind::Json
        ) -> ProjectResourceBlob
        {
            return ProjectResourceBlob{
                .kind  = kind,
                .name  = std::move(name),
                .bytes = std::string{bytes},
            };
        }

        [[nodiscard]]
        auto loadOn(
            ProjectGenerationRegistrar& registrar,
            VerifiedProjectGeneration const& generation,
            std::string_view toolSource,
            std::vector<std::string> declaredTools
        ) -> Result<ProjectGenerationHandle>
        {
            return registrar.registerGeneration(
                generation,
                catalogOver(generation, std::move(declaredTools)),
                closureModules(toolSource),
                {},
                acceptingRuntime()
            );
        }
    } // namespace

    TEST_CASE("a generation loads its closure and runs the entries it bound")
    {
        SUBCASE("a generation that binds Tools")
        {
            auto const generation = generationOver(
                k_toolSource,
                bothEntries(),
                bothBindings()
            );

            auto       registrar = ProjectGenerationRegistrar{};
            auto const loaded    = loadOn(
                registrar,
                generation,
                k_toolSource,
                bothTools()
            );
            REQUIRE(loaded.has_value());

            CHECK(loaded->pluginId() == std::string{k_pluginId});
            CHECK(loaded->projectRegistrationHash() == generation.hash());

            // The manifest digest is the one the document pinned rather than
            // anything the caller chose, so moving the closure's code moves
            // this generation's identity with it.
            CHECK(
                loaded->toolModuleManifestHash() == manifestHashOf(k_toolSource)
            );

            auto const scopedEnvironment = currentScopedToolEnvironmentHash();
            REQUIRE(scopedEnvironment.has_value());
            CHECK(loaded->environmentIdentity() == *scopedEnvironment);

            // The closure runs the entries the binding table named, out of the
            // one loaded value.
            auto const dismissed = loaded->invokeBoundTool(
                k_dismissTool,
                json::Value::ofObject({
                    {"note", json::Value::ofString("kept")},
                }),
                script::ScopedRunRequest{.parentPosition = runPosition()}
            );
            REQUIRE(dismissed.has_value());
            CHECK(
                json::canonicalBytes(*dismissed) == R"({"dismissed":"kept"})"
            );

            // The discovery table the loader baked, read by the closure out of
            // the pinned resource rather than through a Tool call. Both
            // catalogs are in it: the Framework's, which is how a handler
            // reaches the world, and this generation's own.
            auto const swept = loaded->invokeBoundTool(
                k_sweepTool,
                json::Value::ofObject({}),
                script::ScopedRunRequest{.parentPosition = runPosition()}
            );
            REQUIRE(swept.has_value());
            CHECK(memberOf(*swept, "knows_sibling").boolean());
            CHECK(memberOf(*swept, "knows_framework").boolean());
            CHECK_FALSE(memberOf(*swept, "catalog_hash").string().empty());

            // The two joined authorities, held rather than re-derived: the
            // table the tool closure was compiled over, and the catalog the
            // bounds of a call are read from per call.
            CHECK(loaded->bindingTable().entryPoints() == bothEntries());
            CHECK(loaded->catalog().toolNames() == bothTools());
            auto const frameworkCatalog = FrameworkToolCatalogOwner::create();
            REQUIRE(frameworkCatalog.has_value());
            CHECK(
                loaded->frameworkToolCatalogHash()
                == frameworkCatalog->toolCatalogHash()
            );

            auto const unknown = loaded->invokeBoundTool(
                "chaos.project.unknown",
                json::Value::ofObject({}),
                script::ScopedRunRequest{.parentPosition = runPosition()}
            );
            REQUIRE_FALSE(unknown.has_value());
            CHECK(unknown.error().message().contains("binds no Tool named"));

            auto const found = registrar.findExact(
                std::string{k_pluginId},
                generation.hash()
            );
            REQUIRE(found.has_value());
            CHECK(found->environmentIdentity() == loaded->environmentIdentity());

            // A root the registry never took is an absence the caller is told
            // about, not a nearest match it is handed.
            auto const absent = registrar.findExact(
                std::string{k_pluginId},
                hashOf("another-generation")
            );
            REQUIRE_FALSE(absent.has_value());
            CHECK(absent.error().message().contains(
                "no exact Project generation registration is loaded"
            ));

            // A registration root is a generation, so the same root cannot be
            // loaded twice under two different programs.
            auto const again = loadOn(
                registrar,
                generation,
                k_toolSource,
                bothTools()
            );
            REQUIRE_FALSE(again.has_value());
            CHECK(again.error().message().contains("registration is immutable"));
        }

        SUBCASE("a generation that binds none still ships a tool closure")
        {
            // The whole of what a pure project declares: a tool closure whose
            // stated entry set is empty and a binding table that is empty.
            // Explicit emptiness is a value; there is no shorter spelling and
            // no absent slot to reach for.
            auto const generation = generationOver(
                k_identityOnlySource,
                {},
                {}
            );
            CHECK(generation.toolClosure().exportedEntryPoints.empty());
            CHECK(generation.projectToolBindings().empty());
            CHECK(generation.toolCatalogHash() == hashOf(k_toolCatalogBytes));

            // The catalog is stated and declares nothing, on the same terms
            // the entry set and the binding union are stated and are empty.
            // A catalog that had to declare a Tool would leave a pure project
            // with no legal document at all.
            auto       registrar = ProjectGenerationRegistrar{};
            auto const loaded    = loadOn(
                registrar,
                generation,
                k_identityOnlySource,
                {}
            );
            REQUIRE(loaded.has_value());
            CHECK(loaded->catalog().toolNames().empty());
            CHECK(loaded->bindingTable().entryPoints().empty());

            // The tool closure loaded and offers nothing, which is a different
            // fact from a generation that never carried one.
            auto const refused = loaded->invokeBoundTool(
                k_dismissTool,
                json::Value::ofObject({}),
                script::ScopedRunRequest{.parentPosition = runPosition()}
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains("binds no Tool named"));
        }
    }

    TEST_CASE("each leg of the export join refuses on its own")
    {
        auto registrar = ProjectGenerationRegistrar{};

        SUBCASE("a tool declaration that overstates what the contract binds")
        {
            // The closure really does export both, and the declaration says
            // so; what nothing says is that `sweep` implements a Tool. An
            // entry no binding names is a handler the dispatcher can never
            // address, so the loader refuses the document rather than
            // compiling it.
            auto const generation = generationOver(
                k_toolSource,
                bothEntries(),
                {
                    ProjectToolBinding{
                        .toolName   = std::string{k_dismissTool},
                        .entryPoint = "dismiss",
                    },
                }
            );
            auto const refused = loadOn(
                registrar,
                generation,
                k_toolSource,
                bothTools()
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "the tool closure declares entry sweep, which no Tool binds"
            ));
        }

        SUBCASE("a tool declaration that understates what the contract binds")
        {
            auto const generation = generationOver(
                k_toolSource,
                {"dismiss"},
                bothBindings()
            );
            auto const refused = loadOn(
                registrar,
                generation,
                k_toolSource,
                bothTools()
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "Project Tool chaos.project.sweep is bound to entry sweep, "
                "which the tool closure does not declare"
            ));
        }

        SUBCASE("an accurate-looking declaration the shipped bytes do not honour")
        {
            // Nothing about this document is inconsistent: the declaration is
            // exactly the binding union, and the manifest digest is the digest
            // of the bytes handed in. Only running the closure through the
            // bridge can find that `sweep` is not there, which is why the
            // load-time leg cannot stand in for this one.
            auto const generation = generationOver(
                k_dismissOnlyToolSource,
                bothEntries(),
                bothBindings()
            );
            auto const refused = loadOn(
                registrar,
                generation,
                k_dismissOnlyToolSource,
                bothTools()
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains("missing an entry point"));
        }

        SUBCASE("shipped bytes that export more than the declaration names")
        {
            auto const generation = generationOver(
                k_extraFieldToolSource,
                bothEntries(),
                bothBindings()
            );
            auto const refused = loadOn(
                registrar,
                generation,
                k_extraFieldToolSource,
                bothTools()
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains("undeclared field"));
        }

    }

    TEST_CASE("the registration reader accepts its own generation and nothing else")
    {
        auto const healthy = documentOver(
            k_toolSource,
            bothEntries(),
            bothBindings()
        );

        SUBCASE("a document stating the two-closure generation")
        {
            auto const stale = withMember(
                healthy,
                "project_registration_format",
                json::Value::ofNumber(
                    static_cast<double>(k_twoClosureRegistrationFormat)
                )
            );
            auto const refused = verifiedGeneration(stale);
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "refused project_registration_format: const is not the "
                "required value"
            ));
        }

        SUBCASE("a validator handing over another generation's claims")
        {
            // The identity assertion itself, reached past the schema. A
            // validator is trusted deployment code, and this is what refuses
            // one that hands this reader a generation it was not built to
            // read -- naming both numbers, because that is the diagnosis.
            auto exactJcs       = json::canonicalBytes(healthy);
            auto const rootHash = hashOf(exactJcs);
            auto const refused  = ProjectGeneration::verifyExact(
                std::move(exactJcs),
                rootHash,
                [](std::string_view) -> Result<ProjectGenerationClaims>
                {
                    return ProjectGenerationClaims{
                        .projectRegistrationFormat = k_twoClosureRegistrationFormat,
                        .pluginId                  = std::string{k_pluginId},
                        .toolClosure = ProjectClosureClaims{
                            .moduleManifestHash  = hashOf("tool"),
                            .exportedEntryPoints = {},
                        },

                        .pluginEnvironmentHash                = hashOf("environment"),
                        .toolCatalogHash                      = hashOf("catalog"),
                        .projectResources                     = {},
                        .observedInstanceIdentitySchemaHashes = {},
                        .projectToolBindings                  = {},
                    };
                }
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "the registration states 6 and this framework reads 7"
            ));
        }

        SUBCASE("a document carrying no closure")
        {
            auto const refused = verifiedGeneration(
                withoutMember(healthy, "tool_closure")
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "required has no member 'tool_closure'"
            ));
        }

        SUBCASE("a closure that states no export surface")
        {
            auto const silent = withMember(
                healthy,
                "tool_closure",
                withoutMember(
                    memberOf(healthy, "tool_closure"),
                    "exported_entry_points"
                )
            );
            auto const refused = verifiedGeneration(silent);
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "refused tool_closure: required has no member "
                "'exported_entry_points'"
            ));
        }

        SUBCASE("a declaration in an order no authoring path derives")
        {
            auto const unsorted = withMember(
                healthy,
                "tool_closure",
                withMember(
                    memberOf(healthy, "tool_closure"),
                    "exported_entry_points",
                    entryPointArray({"sweep", "dismiss"})
                )
            );
            auto const refused = verifiedGeneration(unsorted);
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "tool closure exported entry point must be unique and "
                "JCS-ordered"
            ));
        }

        SUBCASE("bytes this reader will not take at all")
        {
            auto const exactJcs = json::canonicalBytes(healthy);

            // The validator is the seam a deployment fills. An absent one is a
            // refusal rather than a call into nothing.
            auto const unvalidated = ProjectGeneration::verifyExact(
                exactJcs,
                hashOf(exactJcs),
                ProjectGenerationExactValidator{}
            );
            REQUIRE_FALSE(unvalidated.has_value());
            CHECK(unvalidated.error().message().contains(
                "requires an exact validator"
            ));

            // Bytes offered under a root they do not hash to. The root IS the
            // generation's identity, so this is the one check that makes a
            // generation's name mean its bytes.
            auto const misrooted = ProjectGeneration::verifyExact(
                exactJcs,
                hashOf("another-root"),
                readRegistrationDocument
            );
            REQUIRE_FALSE(misrooted.has_value());
            CHECK(misrooted.error().message().contains(
                "do not match the expected root hash"
            ));

            auto const empty = ProjectGeneration::verifyExact(
                {},
                hashOf(""),
                readRegistrationDocument
            );
            REQUIRE_FALSE(empty.has_value());
            CHECK(empty.error().message().contains(
                "non-empty bounded UTF-8 JCS"
            ));

            // A spelling RFC 8785 never produces, offered under the root those
            // bytes really have, so nothing before the reader refuses it.
            auto const loose   = std::string{R"({ "plugin_id": "chaos.project" })"};
            auto const spelled = ProjectGeneration::verifyExact(
                loose,
                hashOf(loose),
                readRegistrationDocument
            );
            REQUIRE_FALSE(spelled.has_value());
            CHECK(spelled.error().message().contains(
                "are not their own RFC 8785 form"
            ));
        }
    }

    TEST_CASE("the loader refuses a closure or environment this generation never pinned")
    {
        auto registrar = ProjectGenerationRegistrar{};

        SUBCASE("a tool closure whose manifest is not the pinned one")
        {
            auto const generation = generationOver(
                k_toolSource,
                bothEntries(),
                bothBindings()
            );
            auto const refused = loadOn(
                registrar,
                generation,
                k_dismissOnlyToolSource,
                bothTools()
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "the tool closure does not match the verified generation"
            ));
        }

        SUBCASE("an environment other than the one running")
        {
            auto const foreign = withMember(
                documentOver(
                    k_toolSource,
                    bothEntries(),
                    bothBindings()
                ),
                "plugin_environment_hash",
                json::Value::ofString(hashOf("another-environment").hex())
            );
            auto generation = verifiedGeneration(foreign);
            REQUIRE(generation.has_value());

            auto const refused = loadOn(
                registrar,
                *generation,
                k_toolSource,
                bothTools()
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "Project environment does not match the verified generation"
            ));
        }

        SUBCASE("a generation with no Tool Runtime seam")
        {
            auto const generation = generationOver(
                k_toolSource,
                bothEntries(),
                bothBindings()
            );

            // The seam is required, and its absence is refused at the loader
            // rather than discovered by a run that finds nothing there.
            auto const unrunnable = registrar.registerGeneration(
                generation,
                catalogOver(generation, bothTools()),
                closureModules(k_toolSource),
                {},
                script::ToolRuntimeInvoke{}
            );
            REQUIRE_FALSE(unrunnable.has_value());
            CHECK(
                unrunnable.error().message().contains("requires a Tool Runtime")
            );
        }
    }

    TEST_CASE("the catalog join refuses a contract and a binding table that disagree")
    {
        auto registrar = ProjectGenerationRegistrar{};

        SUBCASE("a Tool the catalog declares that no entry implements")
        {
            // Nothing about the closure is wrong: it exports exactly what it
            // declares, and every binding names an entry it exports. What the
            // export join cannot see is the Tool the CATALOG declares, because
            // the catalog is a second document and the join above never reads
            // it.
            auto const generation = generationOver(
                k_dismissOnlyToolSource,
                {"dismiss"},
                {
                    ProjectToolBinding{
                        .toolName   = std::string{k_dismissTool},
                        .entryPoint = "dismiss",
                    },
                }
            );
            auto const refused = loadOn(
                registrar,
                generation,
                k_dismissOnlyToolSource,
                bothTools()
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "Project Tool chaos.project.sweep is declared with no binding "
                "to a Project entry"
            ));
        }

        SUBCASE("a binding naming a Tool the catalog never declared")
        {
            auto const generation = generationOver(
                k_toolSource,
                bothEntries(),
                bothBindings()
            );
            auto const refused = loadOn(
                registrar,
                generation,
                k_toolSource,
                {std::string{k_dismissTool}}
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "Project Tool binding names chaos.project.sweep, which this "
                "Tool Catalog does not declare"
            ));
        }
    }

    // Provisioning and session pinning, reached from a verified generation.
    //
    // What it proves is not that a signature was renamed. The Operator's
    // registration row, its instance row and the session that chains to it are
    // all written here from a generation whose document generation the ledger
    // has no way to name: the same doors, the same rows, and no branch anywhere
    // that asks which registration document a project was deployed as.
    //
    // Nothing about an instance's contents is written with it. Provisioning
    // records that an instance exists under this key and nothing else, because
    // what an instance holds is the Project's.
    TEST_CASE("a generation provisions an instance and pins a session")
    {
        auto const directory = test_support::TemporaryDirectory{};
        auto const release   = test_support::runtimeRelease(
            directory.path() / "session-handoff"
        );
        auto store = OperatorCoordinator::open(directory.path() / "production");
        REQUIRE(store.has_value());
        auto const installed = store->installRuntimeArtifact(
            RuntimeArtifactInstallRequest{
                .handoffRoot                 = release.handoffRoot,
                .expectedReleaseManifestHash = release.releaseManifestHash,
                .expectedInstalledGeneration = 0U,
            }
        );
        REQUIRE(installed.has_value());

        auto const generation = generationOver(
            k_toolSource,
            bothEntries(),
            bothBindings()
        );
        auto const policyBytes = test_support::policyArtifactBytes();
        auto const manifest    = SessionManifest::create(SessionManifestSpec{
            .runtimeModelArtifactRootHash = installed->rootHash(),
            .operatorProtocolSchemaHash = hashOf("operator"),
            .projectRegistrationHash    = generation.hash(),
            .policyArtifactHash         = hashOf(policyBytes),
            .agentProfileHash           = hashOf("agent"),
        });
        REQUIRE(manifest.has_value());

        SUBCASE("the whole chain, from generation to bound controller")
        {
            REQUIRE(store->registerProject(generation).has_value());
            REQUIRE(
                store->provisionProjectInstance(generation, "instance-1")
                    .has_value()
            );

            auto const worldScope = ObservedInstanceWorldScope::run("target-1", 1);
            REQUIRE(worldScope.has_value());
            auto const pin = SessionPin{
                .sessionId                 = "session-1",
                .authenticatedControllerId = "controller-1",
                .idempotencyNamespace      = "controller-1",
                .projectRegistrationHash   = generation.hash(),
                .controllerCapabilities    = {},
                .controlledTargetId        = "target-1",
                .projectInstanceKey        = "instance-1",
                .mode                      = SessionMode::Read,
                .kind                      = ControllerKind::Human,
                .worldScope                = *worldScope,
            };
            REQUIRE(
                store->pinSession(pin, *manifest, std::nullopt).has_value()
            );

            // The session chains to the instance and the instance to the
            // generation, so a binding mints only because every link held.
            auto const controller = store->bindController("session-1");
            REQUIRE(controller.has_value());

            // Provisioning the same key again is the same statement, so it
            // is admitted rather than refused as a second instance.
            CHECK(
                store->provisionProjectInstance(generation, "instance-1")
                    .has_value()
            );
        }

        SUBCASE("a session for an instance this generation never provisioned")
        {
            REQUIRE(store->registerProject(generation).has_value());
            auto const worldScope = ObservedInstanceWorldScope::run("target-1", 1);
            REQUIRE(worldScope.has_value());
            auto const refused = store->pinSession(
                SessionPin{
                    .sessionId                 = "session-2",
                    .authenticatedControllerId = "controller-1",
                    .idempotencyNamespace      = "controller-1",
                    .projectRegistrationHash   = generation.hash(),
                    .controllerCapabilities    = {},
                    .controlledTargetId        = "target-1",
                    .projectInstanceKey        = "instance-1",
                    .mode                      = SessionMode::Read,
                    .kind                      = ControllerKind::Human,
                    .worldScope                = *worldScope,
                },
                *manifest,
                std::nullopt
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "requires an existing ProjectInstance"
            ));
        }
    }

    // The pure and scoped environment identities, each naming what its own
    // program type may reach. Retargeted here from the deleted five-function
    // and one-closure Tool program suites: neither accessor belonged to those
    // contracts, and a registration of either type is admitted only against
    // the digest of exactly these bytes.
    TEST_CASE("the two environment identities name what each program type reaches")
    {
        auto const material = currentProjectPluginEnvironmentMaterial();
        auto const digest   = currentProjectPluginEnvironmentHash();
        REQUIRE(material.has_value());
        REQUIRE(digest.has_value());
        auto const parsed = json::parse(*material);
        REQUIRE(parsed.has_value());
        CHECK(json::canonicalBytes(*parsed) == *material);
        REQUIRE(parsed->find("framework_module_freeze") != nullptr);
        REQUIRE(parsed->find("framework_module_budget") != nullptr);
        CHECK(
            parsed->find("framework_module_freeze")->string()
            == "deep-keys-and-values-v1"
        );
        CHECK(
            parsed->find("framework_module_budget")->string()
            == "separate-release-owned-quota-v1"
        );

        // Every reserved pure module, joined row by row against the SDK the
        // framework actually ships. A preimage over names alone would leave a
        // build that changed what a module RETURNS with an unmoved digest.
        auto const* const modules = parsed->find("framework_pure_modules");
        REQUIRE(modules != nullptr);
        REQUIRE(modules->items().size() == 8U);
        auto const sdk = task::pureFrameworkScriptModules();
        REQUIRE(sdk.has_value());
        for (auto const& row : modules->items())
        {
            auto const* const name = row.find("name");
            REQUIRE(name != nullptr);
            auto const reservedName = name->string();
            REQUIRE(reservedName.starts_with("@umbraflow/"));
            auto const module = std::ranges::find(
                *sdk,
                reservedName,
                &script::FrameworkModule::name
            );
            REQUIRE(module != sdk->end());
            CHECK(row.find("project_visible")->boolean() == module->projectVisible);
            auto const sourceHash = sha256(
                std::as_bytes(std::span{module->source})
            );
            REQUIRE(sourceHash.has_value());
            CHECK(row.find("source_hash")->string() == sourceHash->hex());
        }
        CHECK(*digest == hashOf(*material));

        // The scoped identity is its own, and the two sets of module names are
        // disjoint: that disjointness is the executable half of "two types, not
        // two spellings", and the resolver refusal that enforces it lives in
        // tests/script/test-scoped-tool-program.cpp.
        auto const scopedMaterial = currentScopedToolEnvironmentMaterial();
        auto const scopedDigest   = currentScopedToolEnvironmentHash();
        REQUIRE(scopedMaterial.has_value());
        REQUIRE(scopedDigest.has_value());
        CHECK(*scopedDigest != *digest);
        CHECK(*scopedDigest == hashOf(*scopedMaterial));
        for (auto const scopedName : script::ScopedToolProgram::scopedModuleNames())
        {
            INFO("scoped module: ", scopedName);
            CHECK(scopedMaterial->contains(scopedName));
            CHECK_FALSE(material->contains(scopedName));
        }
    }

    // The module closure digest both closures are held to. It is a free
    // function over authored blobs and belongs to neither program type, so it
    // is retargeted here rather than deleted with the registrar that used to
    // call it first.
    TEST_CASE("module manifest identity admits only runtime-canonical names")
    {
        auto tooManySegments = std::string{"a"};
        for (auto index = std::size_t{1U}; index < 17U; ++index)
        {
            tooManySegments += "/a";
        }
        for (auto const& invalidName : std::array{
                 std::string(65U, 'a'),
                 std::move(tooManySegments),
                 std::string{"../main"},
             })
        {
            auto const modules = std::array{
                ProjectModuleBlob{
                    .name   = invalidName,
                    .source = "return {}\n",
                },
            };
            CHECK_FALSE(
                derivePluginModuleManifestHash(invalidName, modules).has_value()
            );
        }
    }

    // The resource closure join, which is the Operator's own and not the
    // script layer's: the pinned rows and the supplied blobs must agree name
    // by name, kind by kind, size by size and digest by digest. A registration
    // pins ONE closure and both program types read it, so this is one function
    // and these are its legs.
    TEST_CASE("a pinned resource closure is verified blob by blob")
    {
        SUBCASE("a missing blob is refused by name")
        {
            // Two roots with one supplied, because a refusal that said only
            // "the closure is incomplete" would leave the reader to find which
            // of them is absent.
            auto const pinned = std::array{
                pinnedResource("attestations", k_otherRoot),
                pinnedResource("content", k_contentRoot),
            };
            auto const refused = verifyProjectResourceClosure(
                pinned,
                {resourceBlob("content", k_contentRoot)}
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(
                refused.error().message()
                == "Project resource is missing for registered name "
                   "'attestations'"
            );
        }

        SUBCASE("a blob no registration pinned is refused")
        {
            CHECK_FALSE(
                verifyProjectResourceClosure(
                    {},
                    {resourceBlob("extra", k_contentRoot)}
                ).has_value()
            );
        }

        SUBCASE("a blob whose bytes are not the pinned ones is refused")
        {
            auto const pinned = std::array{pinnedResource("content", k_contentRoot)};

            // Same size, different bytes: only the digest can refuse it.
            CHECK_FALSE(
                verifyProjectResourceClosure(
                    pinned,
                    {resourceBlob("content", k_sameSizeWrongRoot)}
                ).has_value()
            );

            // A different size, which the size column refuses ahead of it.
            CHECK_FALSE(
                verifyProjectResourceClosure(
                    pinned,
                    {resourceBlob("content", "{}")}
                ).has_value()
            );
        }

        SUBCASE("a blob offered under another kind is refused")
        {
            auto const pinned = std::array{pinnedResource("content", k_contentRoot)};
            CHECK_FALSE(
                verifyProjectResourceClosure(
                    pinned,
                    {
                        resourceBlob(
                            "content",
                            k_contentRoot,
                            ProjectResourceKind::Utf8
                        ),
                    }
                ).has_value()
            );
        }

        SUBCASE("one name offered twice is refused")
        {
            auto const pinned = std::array{pinnedResource("content", k_contentRoot)};
            CHECK_FALSE(
                verifyProjectResourceClosure(
                    pinned,
                    {
                        resourceBlob("content", k_contentRoot),
                        resourceBlob("content", k_contentRoot),
                    }
                ).has_value()
            );
        }

        SUBCASE("the exact closure is admitted in the registration's own order")
        {
            auto const pinned = std::array{
                pinnedResource("attestations", k_otherRoot),
                pinnedResource("content", k_contentRoot),
            };
            auto const admitted = verifyProjectResourceClosure(
                pinned,
                {
                    resourceBlob("content", k_contentRoot),
                    resourceBlob("attestations", k_otherRoot),
                }
            );
            REQUIRE(admitted.has_value());
            REQUIRE(admitted->size() == 2U);
            CHECK((*admitted)[0].name == "attestations");
            CHECK((*admitted)[1].name == "content");
        }
    }
}

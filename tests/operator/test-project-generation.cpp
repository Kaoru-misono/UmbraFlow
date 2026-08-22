#include <operator/manifest.hpp>
#include <operator/project-generation.hpp>
#include <operator/project-plugin.hpp>
#include <operator/project-tool-program.hpp>

#include <script/scoped-tool-program.hpp>

#include <json/schema.hpp>
#include <json/value.hpp>

#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Stage A of the two-closure cut: the registration shape a generation will one
// day be deployed as, landed with no production producer at all.
//
// Everything here is production-unreachable by construction. The document is
// written by the builder in this file's anonymous namespace and by nothing
// else -- modules/deployment writes the one-closure v2 document and has never
// heard of this one -- and ProjectGenerationRegistrar has no caller outside
// these cases. Two shapes exist in the tree; neither reader has both behind
// it, and no code inspects a document to decide which reader gets it.
//
// What these cases have to prove is what a successful load cannot show: that
// both closure slots are mandatory, that `exported_entry_points` is mandatory
// inside each of them, and that the export join fails on each of its legs
// independently -- a declaration that overstates, a declaration that
// understates, and a closure whose real exports disagree with a declaration
// that looked accurate.
namespace uf::operator_runtime
{
    namespace
    {
        constexpr auto k_pluginId     = std::string_view{"chaos.project"};
        constexpr auto k_dismissTool  = std::string_view{"chaos.project.dismiss"};
        constexpr auto k_sweepTool    = std::string_view{"chaos.project.sweep"};
        constexpr auto k_schemaPath   = std::string_view{
            "schema/umbraflow-project-registration-v3.schema.json"
        };

        // The reducer closure: the whole of the pure type's contract under the
        // two-closure shape, which is plugin_id and one entry.
        constexpr auto k_reducerSource = std::string_view{R"LUAU(
return {
    plugin_id = "chaos.project",
    reduce = function(input)
        return { folded = input.event }
    end,
}
)LUAU"};

        // A closure that exports nothing but its identity. It stands in twice:
        // as a reducer that does not carry the one entry a generation's
        // reducer must, and as the tool closure a project that binds no Tool
        // ships -- which is a real closure with an explicitly empty declared
        // entry set, never an absent slot.
        constexpr auto k_identityOnlySource = std::string_view{R"LUAU(
return {
    plugin_id = "chaos.project",
}
)LUAU"};

        // The tool closure: two bound entries and nothing else.
        constexpr auto k_toolSource = std::string_view{R"LUAU(
return {
    plugin_id = "chaos.project",
    dismiss = function(input)
        return { dismissed = input.note }
    end,
    sweep = function(_input)
        return { swept = true }
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

        // The authored bytes of the two-closure document contract, held for
        // the process because a compiled json::Schema is only as durable as
        // the bytes it was compiled from.
        [[nodiscard]]
        auto twoClosureSchemaBytes() -> std::string const&
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

        // The document spells a digest as bare lowercase hex, exactly as the
        // one-closure document does. ContentHash's own spelling carries the
        // algorithm prefix, and this is the one place the two meet.
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

        // The reader of the two-closure document, and the only one there is.
        // It accepts exactly what schema/umbraflow-project-registration-v3
        // describes and refuses everything else; the one-closure document is
        // not a degraded form it falls back to, it is a document with another
        // reader in modules/deployment that this one never consults.
        //
        // It lives in a test translation unit's anonymous namespace, which is
        // what makes the whole two-closure generation dark: no production
        // translation unit can name this function, and nothing in
        // ProductLifecycle mints a VerifiedProjectGeneration.
        [[nodiscard]]
        auto readTwoClosureDocument(
            std::string_view exactJcs
        ) -> Result<ProjectGenerationClaims>
        {
            UF_TRY(withContext(
                json::requireExactCanonical(exactJcs),
                "the two-closure registration is not exact RFC 8785 JCS"
            ));
            UF_TRY_VALUE(document, json::parse(exactJcs));
            UF_TRY_VALUE(
                schema,
                json::Schema::compile(json::Schema::Document{
                    .label      = k_schemaPath,
                    .exactBytes = twoClosureSchemaBytes(),
                })
            );
            UF_TRY(withContext(
                schema.validate(document),
                "validating the two-closure ProjectRegistration document"
            ));

            UF_TRY_VALUE(reducerClosure, readClosure(document, "reducer_closure"));
            UF_TRY_VALUE(toolClosure, readClosure(document, "tool_closure"));
            UF_TRY_VALUE(
                environmentHash,
                hashMember(document, "plugin_environment_hash")
            );
            UF_TRY_VALUE(
                toolCatalogHash,
                hashMember(document, "tool_catalog_hash")
            );
            UF_TRY_VALUE(
                stateSchemaHash,
                hashMember(document, "project_state_schema_hash")
            );
            UF_TRY_VALUE(
                observationSchemaHash,
                hashMember(document, "project_observation_schema_hash")
            );
            UF_TRY_VALUE(
                preconditionSchemaHash,
                hashMember(document, "project_tool_precondition_schema_hash")
            );
            UF_TRY_VALUE(
                reconcileSchemaHash,
                hashMember(document, "reconcile_payload_schema_manifest_hash")
            );
            UF_TRY_VALUE(
                journalSchemaHash,
                hashMember(document, "journal_event_schema_manifest_hash")
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
                .pluginId       = std::string{memberOf(document, "plugin_id").string()},
                .reducerClosure = std::move(reducerClosure),
                .toolClosure    = std::move(toolClosure),

                .pluginEnvironmentHash              = environmentHash,
                .toolCatalogHash                    = toolCatalogHash,
                .projectStateSchemaHash             = stateSchemaHash,
                .projectObservationSchemaHash       = observationSchemaHash,
                .projectToolPreconditionSchemaHash  = preconditionSchemaHash,
                .reconcilePayloadSchemaManifestHash = reconcileSchemaHash,
                .journalEventSchemaManifestHash     = journalSchemaHash,

                .baselineEventType = std::string{
                    memberOf(document, "baseline_event_type").string()
                },
                .projectResources                     = {},
                .observedInstanceIdentitySchemaHashes = {},
                .projectToolBindings                  = std::move(bindings),
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

        // The two-closure document, exactly as an authoring path would write
        // it. Every digest is derived from the bytes it describes rather than
        // stated, because a builder that stated them would prove nothing about
        // the loader's own checks; the declared entry sets are the one thing a
        // caller states, which is the whole subject of these cases.
        [[nodiscard]]
        auto documentOver(
            std::string_view reducerSource,
            std::vector<std::string> const& declaredReducerEntries,
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
                {"baseline_event_type",
                 json::Value::ofString("chaos.baseline")},
                {"journal_event_schema_manifest_hash",
                 json::Value::ofString(hashOf("journal").hex())},
                {"observed_instance_identity_schema_hashes",
                 json::Value::ofArray({})},
                {"plugin_environment_hash",
                 json::Value::ofString(runningEnvironmentHash().hex())},
                {"plugin_id", json::Value::ofString(std::string{k_pluginId})},
                {"project_observation_schema_hash",
                 json::Value::ofString(hashOf("observation").hex())},
                {"project_registration_format",
                 json::Value::ofNumber(
                     static_cast<double>(k_projectGenerationFormat)
                 )},
                {"project_resources", json::Value::ofArray({})},
                {"project_state_schema_hash",
                 json::Value::ofString(hashOf("state").hex())},
                {"project_tool_bindings", json::Value::ofArray(std::move(rows))},
                {"project_tool_precondition_schema_hash",
                 json::Value::ofString(hashOf("precondition").hex())},
                {"reconcile_payload_schema_manifest_hash",
                 json::Value::ofString(hashOf("reconcile").hex())},
                {"reducer_closure",
                 closureValue(reducerSource, declaredReducerEntries)},
                {"tool_catalog_hash",
                 json::Value::ofString(hashOf("tool-catalog").hex())},
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
                readTwoClosureDocument
            );
        }

        [[nodiscard]]
        auto generationOver(
            std::string_view reducerSource,
            std::vector<std::string> const& declaredReducerEntries,
            std::string_view toolSource,
            std::vector<std::string> const& declaredToolEntries,
            std::vector<ProjectToolBinding> const& bindings
        ) -> VerifiedProjectGeneration
        {
            auto generation = verifiedGeneration(documentOver(
                reducerSource,
                declaredReducerEntries,
                toolSource,
                declaredToolEntries,
                bindings
            ));
            REQUIRE(generation.has_value());
            return *std::move(generation);
        }

        // These cases are about the closures and the join, so the seam answers
        // anything. It is required all the same: a scoped program with no Tool
        // Runtime is a pure program wearing the wrong type.
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
        auto reducerEntry() -> std::vector<std::string>
        {
            return {std::string{k_reducerEntryPoint}};
        }
    } // namespace

    TEST_CASE("a two-closure generation loads both closures and runs each")
    {
        SUBCASE("a generation that binds Tools")
        {
            auto const generation = generationOver(
                k_reducerSource,
                reducerEntry(),
                k_toolSource,
                bothEntries(),
                bothBindings()
            );

            auto       registrar = ProjectGenerationRegistrar{};
            auto const loaded    = registrar.registerGeneration(
                generation,
                closureModules(k_reducerSource),
                closureModules(k_toolSource),
                {},
                acceptingRuntime()
            );
            REQUIRE(loaded.has_value());

            CHECK(loaded->pluginId() == std::string{k_pluginId});
            CHECK(loaded->projectRegistrationHash() == generation.hash());

            // Two closures, two manifest digests, and they are the ones the
            // document pinned rather than anything the caller chose. A single
            // digest over both would make either closure's code move the
            // other's identity.
            CHECK(
                loaded->reducerModuleManifestHash()
                == manifestHashOf(k_reducerSource)
            );
            CHECK(
                loaded->toolModuleManifestHash() == manifestHashOf(k_toolSource)
            );
            CHECK(
                loaded->reducerModuleManifestHash()
                != loaded->toolModuleManifestHash()
            );

            auto const scopedEnvironment = currentScopedToolEnvironmentHash();
            REQUIRE(scopedEnvironment.has_value());
            CHECK(loaded->environmentIdentity() == *scopedEnvironment);

            // The pure half runs the one entry the shrunk contract keeps.
            auto const folded = loaded->reduce(json::Value::ofObject({
                {"event", json::Value::ofString("admitted")},
            }));
            REQUIRE(folded.has_value());
            CHECK(
                json::canonicalBytes(*folded) == R"({"folded":"admitted"})"
            );

            // The scoped half runs the entries the binding table named, out of
            // the one loaded value.
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

            auto const swept = loaded->invokeBoundTool(
                k_sweepTool,
                json::Value::ofObject({}),
                script::ScopedRunRequest{.parentPosition = runPosition()}
            );
            REQUIRE(swept.has_value());
            CHECK(json::canonicalBytes(*swept) == R"({"swept":true})");

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
            // loaded twice under two pairs of programs.
            auto const again = registrar.registerGeneration(
                generation,
                closureModules(k_reducerSource),
                closureModules(k_toolSource),
                {},
                acceptingRuntime()
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
                k_reducerSource,
                reducerEntry(),
                k_identityOnlySource,
                {},
                {}
            );
            CHECK(generation.toolClosure().exportedEntryPoints.empty());
            CHECK(generation.projectToolBindings().empty());
            CHECK(
                generation.reducerClosure().exportedEntryPoints
                == reducerEntry()
            );
            CHECK(generation.toolCatalogHash() == hashOf("tool-catalog"));

            auto       registrar = ProjectGenerationRegistrar{};
            auto const loaded    = registrar.registerGeneration(
                generation,
                closureModules(k_reducerSource),
                closureModules(k_identityOnlySource),
                {},
                acceptingRuntime()
            );
            REQUIRE(loaded.has_value());

            auto const folded = loaded->reduce(json::Value::ofObject({
                {"event", json::Value::ofString("pure")},
            }));
            REQUIRE(folded.has_value());
            CHECK(json::canonicalBytes(*folded) == R"({"folded":"pure"})");

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
                k_reducerSource,
                reducerEntry(),
                k_toolSource,
                bothEntries(),
                {
                    ProjectToolBinding{
                        .toolName   = std::string{k_dismissTool},
                        .entryPoint = "dismiss",
                    },
                }
            );
            auto const refused = registrar.registerGeneration(
                generation,
                closureModules(k_reducerSource),
                closureModules(k_toolSource),
                {},
                acceptingRuntime()
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "the tool closure declares entry sweep, which no Tool of this "
                "generation binds"
            ));
        }

        SUBCASE("a tool declaration that understates what the contract binds")
        {
            auto const generation = generationOver(
                k_reducerSource,
                reducerEntry(),
                k_toolSource,
                {"dismiss"},
                bothBindings()
            );
            auto const refused = registrar.registerGeneration(
                generation,
                closureModules(k_reducerSource),
                closureModules(k_toolSource),
                {},
                acceptingRuntime()
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
                k_reducerSource,
                reducerEntry(),
                k_dismissOnlyToolSource,
                bothEntries(),
                bothBindings()
            );
            auto const refused = registrar.registerGeneration(
                generation,
                closureModules(k_reducerSource),
                closureModules(k_dismissOnlyToolSource),
                {},
                acceptingRuntime()
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains("missing an entry point"));
        }

        SUBCASE("shipped bytes that export more than the declaration names")
        {
            auto const generation = generationOver(
                k_reducerSource,
                reducerEntry(),
                k_extraFieldToolSource,
                bothEntries(),
                bothBindings()
            );
            auto const refused = registrar.registerGeneration(
                generation,
                closureModules(k_reducerSource),
                closureModules(k_extraFieldToolSource),
                {},
                acceptingRuntime()
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains("undeclared field"));
        }

        SUBCASE("a reducer declaration that is not the one entry the type keeps")
        {
            auto const generation = generationOver(
                k_reducerSource,
                {"derive", "reduce"},
                k_toolSource,
                bothEntries(),
                bothBindings()
            );
            auto const refused = registrar.registerGeneration(
                generation,
                closureModules(k_reducerSource),
                closureModules(k_toolSource),
                {},
                acceptingRuntime()
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "the reducer closure states it exports derive, reduce, and a "
                "generation's reducer closure exports exactly reduce"
            ));

            // The other side of the same refusal: an empty reducer
            // declaration is not the pure project's spelling either. Only the
            // TOOL closure has an empty entry set to state.
            auto const silent = generationOver(
                k_reducerSource,
                {},
                k_toolSource,
                bothEntries(),
                bothBindings()
            );
            auto const alsoRefused = registrar.registerGeneration(
                silent,
                closureModules(k_reducerSource),
                closureModules(k_toolSource),
                {},
                acceptingRuntime()
            );
            REQUIRE_FALSE(alsoRefused.has_value());
            CHECK(alsoRefused.error().message().contains(
                "the reducer closure states it exports nothing"
            ));
        }

        SUBCASE("a reducer whose shipped bytes carry no fold at all")
        {
            auto const generation = generationOver(
                k_identityOnlySource,
                reducerEntry(),
                k_toolSource,
                bothEntries(),
                bothBindings()
            );
            auto const refused = registrar.registerGeneration(
                generation,
                closureModules(k_identityOnlySource),
                closureModules(k_toolSource),
                {},
                acceptingRuntime()
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains("missing an entry point"));
        }
    }

    TEST_CASE("the two-closure reader accepts its own generation and nothing else")
    {
        auto const healthy = documentOver(
            k_reducerSource,
            reducerEntry(),
            k_toolSource,
            bothEntries(),
            bothBindings()
        );

        SUBCASE("a document stating the one-closure generation")
        {
            auto const stale = withMember(
                healthy,
                "project_registration_format",
                json::Value::ofNumber(
                    static_cast<double>(k_projectRegistrationFormat)
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
                        .projectRegistrationFormat = k_projectRegistrationFormat,
                        .pluginId                  = std::string{k_pluginId},
                        .reducerClosure = ProjectClosureClaims{
                            .moduleManifestHash  = hashOf("reducer"),
                            .exportedEntryPoints = {std::string{
                                k_reducerEntryPoint
                            }},
                        },
                        .toolClosure = ProjectClosureClaims{
                            .moduleManifestHash  = hashOf("tool"),
                            .exportedEntryPoints = {},
                        },

                        .pluginEnvironmentHash              = hashOf("environment"),
                        .toolCatalogHash                    = hashOf("catalog"),
                        .projectStateSchemaHash             = hashOf("state"),
                        .projectObservationSchemaHash       = hashOf("observation"),
                        .projectToolPreconditionSchemaHash  = hashOf("precondition"),
                        .reconcilePayloadSchemaManifestHash = hashOf("reconcile"),
                        .journalEventSchemaManifestHash     = hashOf("journal"),

                        .baselineEventType                    = "chaos.baseline",
                        .projectResources                     = {},
                        .observedInstanceIdentitySchemaHashes = {},
                        .projectToolBindings                  = {},
                    };
                }
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "the registration states 4 and this framework reads 5"
            ));
        }

        SUBCASE("a document carrying only one closure")
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
                readTwoClosureDocument
            );
            REQUIRE_FALSE(misrooted.has_value());
            CHECK(misrooted.error().message().contains(
                "do not match the expected root hash"
            ));

            auto const empty = ProjectGeneration::verifyExact(
                {},
                hashOf(""),
                readTwoClosureDocument
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
                readTwoClosureDocument
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

        SUBCASE("a reducer closure whose manifest is not the pinned one")
        {
            auto const generation = generationOver(
                k_reducerSource,
                reducerEntry(),
                k_toolSource,
                bothEntries(),
                bothBindings()
            );
            auto const refused = registrar.registerGeneration(
                generation,
                closureModules(k_identityOnlySource),
                closureModules(k_toolSource),
                {},
                acceptingRuntime()
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "the reducer closure does not match the verified generation"
            ));
        }

        SUBCASE("a tool closure whose manifest is not the pinned one")
        {
            auto const generation = generationOver(
                k_reducerSource,
                reducerEntry(),
                k_toolSource,
                bothEntries(),
                bothBindings()
            );
            auto const refused = registrar.registerGeneration(
                generation,
                closureModules(k_reducerSource),
                closureModules(k_dismissOnlyToolSource),
                {},
                acceptingRuntime()
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
                    k_reducerSource,
                    reducerEntry(),
                    k_toolSource,
                    bothEntries(),
                    bothBindings()
                ),
                "plugin_environment_hash",
                json::Value::ofString(hashOf("another-environment").hex())
            );
            auto generation = verifiedGeneration(foreign);
            REQUIRE(generation.has_value());

            auto const refused = registrar.registerGeneration(
                *generation,
                closureModules(k_reducerSource),
                closureModules(k_toolSource),
                {},
                acceptingRuntime()
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "Project environment does not match the verified generation"
            ));
        }

        SUBCASE("a tool closure with no Tool Runtime seam at all")
        {
            auto const generation = generationOver(
                k_reducerSource,
                reducerEntry(),
                k_toolSource,
                bothEntries(),
                bothBindings()
            );
            auto const refused = registrar.registerGeneration(
                generation,
                closureModules(k_reducerSource),
                closureModules(k_toolSource),
                {},
                script::ToolRuntimeInvoke{}
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains("requires a Tool Runtime"));
        }
    }
}

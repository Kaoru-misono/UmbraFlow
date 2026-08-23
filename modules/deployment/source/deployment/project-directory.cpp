#include "project-directory.hpp"

#include <json/schema.hpp>
#include <json/value.hpp>

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/safety/checked-access.hpp>
#include <core/text/json-text.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <image/png.hpp>

#include <schema/framework-schema-catalog.hpp>

#include <script/pure-data-program.hpp>

#include <task/runtime-model-file.hpp>
#include <task/platform/confined-file.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::deployment
{
    namespace
    {
        constexpr auto k_registrationSchemaPath = std::string_view{
            "schema/umbraflow-project-registration-v4.schema.json"
        };

        // umbraflow-project.json: the document production reads. Its shape is
        // stated once, under schema/, and reaches this loader as published
        // bytes through the framework schema catalog. The offline project kit
        // compiles the same bytes out of the same catalog
        // (modules/project/source/project/project-kit.cpp), which is the whole
        // reason the shape is not written here: uf::project cannot link
        // uf::deployment, and a second reading of this document inside the kit
        // was a weaker copy that accepted documents this one refused.
        constexpr auto k_projectSchemaPath = std::string_view{
            "schema/umbraflow-project-v3.schema.json"
        };

        // umbraflow-conformance.json: the document only a conformance run
        // reads. loadProductionProject never opens it, and that separation is
        // the point -- nothing in production wants a "tool the catalog does not
        // carry", and a project at a read-only phase has no mutating tool to
        // name in one.
        constexpr auto k_conformanceSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/project/conformance",
    "title": "umbraflow-conformance.json",
    "$comment": "There is no fingerprint member. The extent a probe frame is checked against is the one RuntimeModelBinding publishes out of the model, never a number this document restates. Tag v3 is v2 without a vocabulary's four journal entries and its provenance: the framework stopped interpreting a Project's state, so there is no Journal for a suite to drive and no fold for it to observe. The tag moved with them because a required member set that lost members is a different shape, and one tag naming two shapes leaves whoever meets an old copy unable to say which of them it is.",
    "type": "object",
    "additionalProperties": false,
    "required": ["foreign", "probe_frame", "schema", "under_test"],
    "properties": {
        "$comment": {"type": "string"},
        "schema": {"const": "umbraflow-conformance/v3"},
        "probe_frame": {"type": "string", "minLength": 1},
        "under_test": {"$ref": "#/$defs/Role"},
        "foreign": {"$ref": "#/$defs/Role"}
    },
    "$defs": {
        "Role": {
            "type": "object",
            "additionalProperties": false,
            "required": ["deployment", "vocabulary"],
            "properties": {
                "$comment": {"type": "string"},
                "deployment": {
                    "type": "string",
                    "minLength": 1,
                    "maxLength": 128,
                    "pattern": "^[a-z][a-z0-9-]*$"
                },
                "vocabulary": {"$ref": "#/$defs/Vocabulary"}
            }
        },
        "Document": {
            "$comment": "The project's exact bytes, carried as a JSON string rather than as a nested object. They are handed to CanonicalJson::parseExact, which refuses anything that is not exact RFC 8785 JCS; a nested object would make the loader choose a serialization and the bytes would stop being the project's.",
            "type": "string",
            "minLength": 1
        },
        "ToolName": {
            "$comment": "A Tool name, in the one spelling every document carrying one uses: namespaced under the namespace its registration owns. A vocabulary names Tools this project's own catalog declares, and a catalog can declare no other shape, so admitting a looser spelling here would only defer the refusal to the carried-tool cross-check the loader runs next.",
            "type": "string",
            "minLength": 3,
            "maxLength": 128,
            "pattern": "^[a-z][a-z0-9_-]*(\\.[a-z][a-z0-9_-]*)+$"
        },
        "Vocabulary": {
            "type": "object",
            "additionalProperties": false,
            "required": [
                "absent_tool",
                "approval_required_plan_tool",
                "mutating_tool",
                "other_mutating_tool",
                "read_only_tool",
                "refused_tool_arguments",
                "tool_arguments",
                "ui_action"
            ],
            "properties": {
                "$comment": {"type": "string"},
                "mutating_tool": {"$ref": "#/$defs/ToolName"},
                "other_mutating_tool": {"$ref": "#/$defs/ToolName"},
                "read_only_tool": {"$ref": "#/$defs/ToolName"},
                "tool_arguments": {"$ref": "#/$defs/Document"},
                "refused_tool_arguments": {"$ref": "#/$defs/Document"},
                "absent_tool": {"$ref": "#/$defs/ToolName"},
                "approval_required_plan_tool": {"$ref": "#/$defs/ToolName"},
                "ui_action": {
                    "type": "object",
                    "additionalProperties": false,
                    "required": ["action", "surface", "ui_target"],
                    "properties": {
                        "$comment": {"type": "string"},
                        "surface": {"type": "string", "minLength": 1},
                        "ui_target": {"type": "string", "minLength": 1},
                        "action": {"type": "string", "minLength": 1}
                    }
                }
            }
        }
    }
})json"};

        // Bounds, stated where the read happens. Each is a refusal rather than
        // a truncation: truncated schema bytes are not the bytes the derived
        // registration would pin.
        constexpr auto k_maximumDocumentBytes = std::size_t{1U << 20U};
        constexpr auto k_maximumPluginBytes =
            script::PureDataProgram::k_maximumModuleSourceBytes;
        constexpr auto k_maximumBlobBytes =
            script::PureDataProgram::k_maximumResourceBytes;
        constexpr auto k_maximumFrameBytes = std::size_t{1U << 26U};

        [[nodiscard]]
        auto refuse(std::string message) -> std::unexpected<Error>
        {
            return fail(AutomationErrorKind::InvalidResource, std::move(message));
        }

        [[nodiscard]]
        auto asText(std::span<std::byte const> bytes) -> std::string
        {
            auto text = std::string{};
            text.reserve(bytes.size());
            for (auto const value : bytes)
            {
                text.push_back(static_cast<char>(std::to_integer<uint8>(value)));
            }
            return text;
        }

        [[nodiscard]]
        auto compile(std::string_view label, std::string_view exactBytes)
            -> Result<json::Schema>
        {
            auto compiled = json::Schema::compile(
                json::Schema::Document{.label = label, .exactBytes = exactBytes}
            );
            if (!compiled.has_value())
            {
                return refuse(std::format(
                    "{} is not a schema this loader can apply: {}",
                    label,
                    compiled.error().message()
                ));
            }
            return *std::move(compiled);
        }

        // One published framework schema, by the path it is published under.
        // Absent means the build embedded a different set than this source
        // names, which is a defect in the build rather than in the project.
        [[nodiscard]]
        auto publishedSchema(std::string_view relativePath)
            -> Result<framework_schema::FrameworkSchemaDocument>
        {
            auto const published = framework_schema::findFrameworkSchema(
                relativePath
            );
            if (!published.has_value())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "generated framework schema catalog is missing "
                        + std::string{relativePath}
                );
            }
            return *published;
        }

        // Every reader below runs after json::Schema accepted the document, so
        // the member is present and of the stated kind. The contract check is
        // what makes that assumption falsifiable rather than undefined if a
        // schema is ever loosened.
        [[nodiscard]]
        auto member(json::Value const& object UF_LIFETIME_BOUND, std::string_view name)
            -> json::Value const&
        {
            auto const* const p_member = object.find(name);
            UF_CHECK(p_member != nullptr);
            return *p_member;
        }

        [[nodiscard]]
        auto text(json::Value const& object, std::string_view name) -> std::string
        {
            return std::string{member(object, name).string()};
        }

        // R3. ConfinedRoot guarantees only that what it opens is what it
        // checked; refusing a spelling it cannot confine is this caller's job,
        // because this caller is the one that knows what a manifest may say.
        [[nodiscard]]
        auto requireManifestSpelling(std::string_view what, std::string_view path)
            -> Status
        {
            auto const complain = [what, path](std::string_view why)
            {
                return refuse(std::format(
                    "{} names {}, which is not a manifest path: {}",
                    what,
                    path,
                    why
                ));
            };

            if (path.empty())
            {
                return complain("it is empty");
            }
            if (path.find('\\') != std::string_view::npos)
            {
                return complain("a manifest path separates with '/' only");
            }
            auto rest = path;
            while (true)
            {
                auto const slash     = rest.find('/');
                auto const component = rest.substr(0U, slash);
                if (component.empty())
                {
                    return complain("it has an empty component");
                }
                if (component == "." || component == "..")
                {
                    return complain("it has a '.' or '..' component");
                }
                if (slash == std::string_view::npos)
                {
                    return ok();
                }
                rest = rest.substr(slash + 1U);
            }
        }

        [[nodiscard]]
        auto openRoot(std::filesystem::path const& directory)
            -> Result<task_platform::ConfinedRoot>
        {
            auto opened = task_platform::ConfinedRoot::open(directory);
            if (!opened.has_value())
            {
                return refuse(std::format(
                    "{} is not a project directory this loader can open: {}",
                    directory.string(),
                    opened.error().message()
                ));
            }
            return *std::move(opened);
        }

        // R1. A root document is required at its exact name. Which reader wants
        // it is the caller's to say, because the two readers want different
        // sets: a refusal naming a document this load never opens would send a
        // reader after a file the project owes nobody.
        [[nodiscard]]
        auto readRootDocument(
            task_platform::ConfinedRoot const& root,
            std::filesystem::path const& directory,
            std::string_view name,
            std::string_view who
        ) -> Result<std::string>
        {
            auto bytes = root.readFile(name, k_maximumDocumentBytes);
            if (!bytes.has_value())
            {
                return refuse(std::format(
                    "{} needs {} at the root of a project directory, and {} "
                    "holds none",
                    who,
                    name,
                    directory.string()
                ));
            }
            return asText(*bytes);
        }

        // R4. A named file must exist and be readable inside the confined root.
        // Missing is a refusal naming both the manifest member and the path,
        // never a skip: skipping would make every rule below vacuous for
        // whatever was skipped.
        [[nodiscard]]
        auto readFile(
            task_platform::ConfinedRoot const& root,
            std::string_view what,
            std::string_view path,
            std::size_t maximumBytes
        ) -> Result<std::string>
        {
            UF_TRY(requireManifestSpelling(what, path));
            auto bytes = root.readFile(path, maximumBytes);
            if (!bytes.has_value())
            {
                return refuse(std::format(
                    "{} names {}, which this project directory does not hold: {}",
                    what,
                    path,
                    bytes.error().message()
                ));
            }
            return asText(*bytes);
        }

        [[nodiscard]]
        auto hashOf(std::string_view bytes) -> Result<ContentHash>
        {
            return sha256(std::as_bytes(std::span{bytes}));
        }

        [[nodiscard]]
        auto parseDocument(std::string_view what, std::string_view bytes)
            -> Result<json::Value>
        {
            auto parsed = json::parse(bytes);
            if (!parsed.has_value())
            {
                return refuse(std::format(
                    "{} is not JSON: {}",
                    what,
                    parsed.error().message()
                ));
            }
            return *std::move(parsed);
        }

        // R2. Every member of every framework-owned document is required and
        // every object is closed, so a missing member and an unknown member are
        // both refusals and no member has a default. The one member every
        // object also admits is $comment, which carries no meaning to any
        // reader here and exists so that a project can say why its own document
        // is shaped as it is.
        [[nodiscard]]
        auto readValidated(
            json::Schema const& schema,
            std::string_view what,
            std::string_view bytes
        ) -> Result<json::Value>
        {
            UF_TRY_VALUE(document, parseDocument(what, bytes));
            auto const judged = schema.validate(document);
            if (!judged.has_value())
            {
                return refuse(
                    std::format("{}: {}", what, judged.error().message())
                );
            }
            return document;
        }

        // Every digest one deployment's registration carries, computed from the
        // bytes this loader read.
        struct DerivedRegistration final
        {
            std::string                            pluginId{};
            operator_runtime::ProjectClosureClaims toolClosure;
            ContentHash                            pluginEnvironmentHash;
            ContentHash                            toolCatalogHash;
            std::vector<ContentHash>               observedInstanceIdentitySchemaHashes{};
        };

        // The closure, as the generation document states it: the digest of the
        // module graph and the entry points the deployment declared it exports.
        [[nodiscard]]
        auto closureValue(operator_runtime::ProjectClosureClaims const& closure)
            -> json::Value
        {
            auto entryPoints = std::vector<json::Value>{};
            entryPoints.reserve(closure.exportedEntryPoints.size());
            for (auto const& entryPoint : closure.exportedEntryPoints)
            {
                entryPoints.emplace_back(json::Value::ofString(entryPoint));
            }
            return json::Value::ofObject({
                {"exported_entry_points",
                 json::Value::ofArray(std::move(entryPoints))},
                {"module_manifest_hash",
                 json::Value::ofString(closure.moduleManifestHash.hex())},
            });
        }

        // The registration document, as exact RFC 8785 JCS. It is assembled as
        // a value and serialized by json::canonicalBytes rather than formatted,
        // so the one spelling of RFC 8785 in this tree is what produces the
        // document whose digest is a project's identity.
        [[nodiscard]]
        auto registrationJcs(
            DerivedRegistration const& derived,
            std::span<operator_runtime::ProjectResource const> projectResources,
            std::span<operator_runtime::ProjectToolBinding const> toolBindings
        ) -> std::string
        {
            auto const hash = [](ContentHash const& value)
            {
                return json::Value::ofString(value.hex());
            };

            auto resources = std::vector<json::Value>{};
            for (auto const& resource : projectResources)
            {
                auto kind = std::string_view{};
                switch (resource.kind)
                {
                case operator_runtime::ProjectResourceKind::Json:
                    kind = "json";
                    break;
                case operator_runtime::ProjectResourceKind::Utf8:
                    kind = "utf8";
                    break;
                case operator_runtime::ProjectResourceKind::Bytes:
                    kind = "bytes";
                    break;
                }
                resources.emplace_back(json::Value::ofObject({
                    {"kind", json::Value::ofString(std::string{kind})},
                    {"name", json::Value::ofString(resource.name)},
                    {"sha256", hash(resource.hash)},
                    {"size", json::Value::ofNumber(static_cast<double>(resource.size))},
                }));
            }

            auto identityHashes = std::vector<json::Value>{};
            for (auto const& identityHash : derived.observedInstanceIdentitySchemaHashes)
            {
                identityHashes.emplace_back(hash(identityHash));
            }

            // The Tool binding table, as the deployment block declared it and
            // in the order readToolBindings put it in. Rendered even when it is
            // empty, which is the whole statement "this registration binds no
            // Tool to an entry" and not an absence a reader fills in.
            auto bindings = std::vector<json::Value>{};
            for (auto const& binding : toolBindings)
            {
                bindings.emplace_back(json::Value::ofObject({
                    {"entry_point", json::Value::ofString(binding.entryPoint)},
                    {"tool_name", json::Value::ofString(binding.toolName)},
                }));
            }

            return json::canonicalBytes(json::Value::ofObject({
                {"observed_instance_identity_schema_hashes",
                 json::Value::ofArray(std::move(identityHashes))},
                {"plugin_environment_hash", hash(derived.pluginEnvironmentHash)},
                {"plugin_id", json::Value::ofString(derived.pluginId)},
                {"project_registration_format",
                 json::Value::ofNumber(
                     static_cast<double>(
                         operator_runtime::k_projectGenerationFormat
                     )
                 )},
                {"project_tool_bindings", json::Value::ofArray(std::move(bindings))},
                {"project_resources", json::Value::ofArray(std::move(resources))},
                {"tool_catalog_hash", hash(derived.toolCatalogHash)},
                {"tool_closure", closureValue(derived.toolClosure)},
            }));
        }

        [[nodiscard]]
        auto parseHash(json::Value const& object, std::string_view name)
            -> Result<ContentHash>
        {
            return ContentHash::parse("sha256:" + text(object, name));
        }

        // A contract generation, read after the schema judged the member as an
        // integer within the exactly representable range, so the double this
        // member() answers with holds the stated number and nothing else.
        [[nodiscard]]
        auto formatOf(json::Value const& object, std::string_view name) -> uint64
        {
            auto const& value = member(object, name);
            UF_CHECK(value.isInteger());
            return static_cast<uint64>(value.number());
        }

        [[nodiscard]]
        auto resourceKindOf(std::string_view value)
            -> operator_runtime::ProjectResourceKind
        {
            if (value == "json")
            {
                return operator_runtime::ProjectResourceKind::Json;
            }
            if (value == "utf8")
            {
                return operator_runtime::ProjectResourceKind::Utf8;
            }
            if (value == "bytes")
            {
                return operator_runtime::ProjectResourceKind::Bytes;
            }
            UF_UNREACHABLE_MSG("registration schema admitted an unknown resource kind");
        }

        // One closure, read back out of the document the schema has already
        // accepted. The declared export set is read as stated: this reader is
        // the second source the join needs, so it never repairs, sorts or
        // completes what the author wrote.
        [[nodiscard]]
        auto readClosureClaims(
            json::Value const& document,
            std::string_view name
        ) -> Result<operator_runtime::ProjectClosureClaims>
        {
            auto const& closure = member(document, name);
            UF_TRY_VALUE(
                moduleManifestHash,
                parseHash(closure, "module_manifest_hash")
            );
            auto entryPoints = std::vector<std::string>{};
            for (auto const& entryPoint :
                 member(closure, "exported_entry_points").items())
            {
                entryPoints.emplace_back(entryPoint.string());
            }
            return operator_runtime::ProjectClosureClaims{
                .moduleManifestHash  = moduleManifestHash,
                .exportedEntryPoints = std::move(entryPoints),
            };
        }

        // The framework's own reading of the document it just derived. It is a
        // whole ProjectGenerationExactValidator: the published registration
        // schema judges the members, and this reads back only what that schema
        // accepted.
        [[nodiscard]]
        auto registrationValidator(json::Schema schema)
            -> operator_runtime::ProjectGenerationExactValidator
        {
            return [judge = std::move(schema)](std::string_view exactJcs)
                       -> Result<operator_runtime::ProjectGenerationClaims>
            {
                auto const canonical = json::requireExactCanonical(exactJcs);
                if (!canonical.has_value())
                {
                    return refuse(std::format(
                        "a ProjectRegistration must be exact RFC 8785 JCS: {}",
                        canonical.error().message()
                    ));
                }
                UF_TRY_VALUE(
                    document,
                    readValidated(judge, "the derived ProjectRegistration", exactJcs)
                );
                UF_TRY_VALUE(
                    toolClosure,
                    readClosureClaims(document, "tool_closure")
                );
                UF_TRY_VALUE(
                    environmentHash,
                    parseHash(document, "plugin_environment_hash")
                );
                UF_TRY_VALUE(catalogHash, parseHash(document, "tool_catalog_hash"));

                auto resources = std::vector<operator_runtime::ProjectResource>{};
                for (auto const& resource : member(document, "project_resources").items())
                {
                    UF_TRY_VALUE(resourceHash, parseHash(resource, "sha256"));
                    resources.emplace_back(operator_runtime::ProjectResource{
                        .kind = resourceKindOf(text(resource, "kind")),
                        .name = text(resource, "name"),
                        .hash = resourceHash,
                        .size = formatOf(resource, "size"),
                    });
                }

                auto toolBindings =
                    std::vector<operator_runtime::ProjectToolBinding>{};
                for (auto const& binding :
                     member(document, "project_tool_bindings").items())
                {
                    toolBindings.emplace_back(
                        operator_runtime::ProjectToolBinding{
                            .toolName   = text(binding, "tool_name"),
                            .entryPoint = text(binding, "entry_point"),
                        }
                    );
                }

                auto identityHashes = std::vector<ContentHash>{};
                for (auto const& identityHash :
                     member(document, "observed_instance_identity_schema_hashes").items())
                {
                    UF_TRY_VALUE(
                        hash,
                        ContentHash::parse(
                            std::string{"sha256:"} + std::string{identityHash.string()}
                        )
                    );
                    identityHashes.emplace_back(hash);
                }

                return operator_runtime::ProjectGenerationClaims{
                    .projectRegistrationFormat             = formatOf(
                        document,
                        "project_registration_format"
                    ),
                    .pluginId                             = text(document, "plugin_id"),
                    .toolClosure                          = std::move(toolClosure),
                    .pluginEnvironmentHash                = environmentHash,
                    .toolCatalogHash                      = catalogHash,
                    .projectResources                     = std::move(resources),
                    .observedInstanceIdentitySchemaHashes = std::move(identityHashes),
                    .projectToolBindings                  = std::move(toolBindings),
                };
            };
        }

        // One observed-instance identity schema a deployment declared inline,
        // as owned canonical bytes. The name is the identity the compiled
        // authority answers under and the value an observation proposal's
        // identity_schema_id carries; the bytes are what the registration pins
        // by sha256.
        struct DeploymentIdentitySchema final
        {
            std::string name{};
            std::string schema{};
        };

        // Everything one deployment block supplies, as owned bytes. The views
        // ProjectDeploymentSources takes are taken from these, so they must
        // outlive the create call.
        //
        // Only the closure is read off disk. Every declarative member of a
        // deployment is inline, so what this holds beside the modules is the
        // canonical rendering of what the one document already said.
        struct DeploymentFiles final
        {
            DeploymentClosure toolClosure{};
            std::string       tools{};

            std::vector<DeploymentIdentitySchema> observedInstanceIdentitySchemas{};
        };

        // The deployment's closure, as bytes plus the export set the block
        // stated for it.
        [[nodiscard]]
        auto readPluginClosure(
            task_platform::ConfinedRoot const& root,
            json::Value const& block
        ) -> Result<DeploymentClosure>
        {
            auto const& plugin = member(block, "tool_closure");
            auto modules = std::vector<operator_runtime::ProjectModuleBlob>{};
            auto paths   = std::vector<std::string>{};
            auto const& declaredModules = member(plugin, "modules").items();
            if (declaredModules.size() > script::PureDataProgram::k_maximumModuleCount)
            {
                return refuse("a deployment's plugin module count exceeds its ceiling");
            }
            auto totalBytes = std::size_t{0};
            for (auto const& declared : declaredModules)
            {
                auto const path = text(declared, "path");
                UF_TRY_VALUE(
                    source,
                    readFile(
                        root,
                        "a deployment's plugin module",
                        path,
                        k_maximumPluginBytes
                    )
                );
                if (
                    totalBytes
                    > script::PureDataProgram::k_maximumModuleClosureSourceBytes
                        - source.size()
                )
                {
                    return refuse("a deployment's plugin module closure exceeds its byte ceiling");
                }
                totalBytes += source.size();
                modules.emplace_back(operator_runtime::ProjectModuleBlob{
                    .name   = text(declared, "name"),
                    .source = std::move(source),
                });
                paths.emplace_back(path);
            }
            std::ranges::sort(paths);
            if (std::ranges::adjacent_find(paths) != paths.end())
            {
                return refuse("a deployment's plugin module paths must be unique");
            }
            auto entry = text(plugin, "entry");
            UF_TRY(operator_runtime::derivePluginModuleManifestHash(entry, modules));

            // The stated export set, carried through unchanged. It is the
            // author's own second source and this loader never derives, sorts
            // or completes it; the registrar joins it against what a closure of
            // its kind may offer, and the bridge joins it against what the
            // shipped bytes actually export.
            auto declaredEntryPoints = std::vector<std::string>{};
            for (auto const& entryPoint :
                 member(plugin, "exported_entry_points").items())
            {
                declaredEntryPoints.emplace_back(entryPoint.string());
            }
            return DeploymentClosure{
                .entryModule         = std::move(entry),
                .modules             = std::move(modules),
                .declaredEntryPoints = std::move(declaredEntryPoints),
            };
        }

        // What the deployment block supplies, gathered once. The Tool
        // declarations and every identity schema are rendered canonically here
        // and nowhere else: the bytes a registration pins are this loader's own
        // arithmetic over the document it read, exactly as every other digest
        // is, so a project author never writes one and no second serialization
        // of the same declaration exists.
        [[nodiscard]]
        auto readDeploymentFiles(
            task_platform::ConfinedRoot const& root,
            json::Value const& block
        ) -> Result<DeploymentFiles>
        {
            UF_TRY_VALUE(toolClosure, readPluginClosure(root, block));

            auto identitySchemas = std::vector<DeploymentIdentitySchema>{};
            for (auto const& declared :
                 member(block, "observed_instance_identity_schemas").items())
            {
                identitySchemas.emplace_back(DeploymentIdentitySchema{
                    .name   = text(declared, "name"),
                    .schema = json::canonicalBytes(member(declared, "schema")),
                });
            }

            return DeploymentFiles{
                .toolClosure                     = std::move(toolClosure),
                .tools                           = json::canonicalBytes(member(block, "tools")),
                .observedInstanceIdentitySchemas = std::move(identitySchemas),
            };
        }

        // The join this deployment declares, as the registration states it. The
        // block names a Tool and the closure entry that answers it; every
        // digest around them is derived, and this is derived too in the one
        // sense that matters -- the author's order is discarded and the
        // canonical one is computed here, exactly as it is for resources.
        //
        // Nothing here asks whether the catalog declares the named Tool or
        // whether the closure exports the named entry. Those are the two halves
        // ProjectToolBindingTable::bind refuses a disagreement between, at the
        // moment both are in hand, and a second reading of either question here
        // would be a second authority over it.
        [[nodiscard]]
        auto readToolBindings(json::Value const& block)
            -> Result<std::vector<operator_runtime::ProjectToolBinding>>
        {
            auto bindings = std::vector<operator_runtime::ProjectToolBinding>{};
            for (auto const& declared : member(block, "tool_bindings").items())
            {
                bindings.emplace_back(operator_runtime::ProjectToolBinding{
                    .toolName   = text(declared, "tool_name"),
                    .entryPoint = text(declared, "entry_point"),
                });
            }
            std::ranges::sort(
                bindings,
                [](std::string_view left, std::string_view right)
                {
                    return jsonMemberNameLess(left, right);
                },
                &operator_runtime::ProjectToolBinding::toolName
            );
            for (auto index = std::size_t{1}; index < bindings.size(); ++index)
            {
                if (bindings[index - 1U].toolName == bindings[index].toolName)
                {
                    return refuse(std::format(
                        "a deployment binds the Tool {} twice",
                        bindings[index].toolName
                    ));
                }
            }
            return bindings;
        }

        [[nodiscard]]
        auto readProjectResources(
            task_platform::ConfinedRoot const& root,
            json::Value const& block
        ) -> Result<std::vector<operator_runtime::ProjectResourceBlob>>
        {
            auto blobs =
                std::vector<operator_runtime::ProjectResourceBlob>{};
            auto paths = std::vector<std::string>{};
            auto const& declaredResources = member(block, "resources").items();
            if (declaredResources.size() > script::PureDataProgram::k_maximumResourceCount)
            {
                return refuse("a deployment's resource count exceeds its ceiling");
            }
            auto totalBytes = std::size_t{0};
            for (auto const& declared : declaredResources)
            {
                auto const path = text(declared, "path");
                UF_TRY_VALUE(
                    bytes,
                    readFile(
                        root,
                        "a deployment's resources entry",
                        path,
                        k_maximumBlobBytes
                    )
                );
                if (
                    totalBytes
                    > script::PureDataProgram::k_maximumResourceClosureBytes
                        - bytes.size()
                )
                {
                    return refuse("a deployment's resource closure exceeds its byte ceiling");
                }
                totalBytes += bytes.size();
                blobs.emplace_back(
                    operator_runtime::ProjectResourceBlob{
                        .kind  = resourceKindOf(text(declared, "kind")),
                        .name  = text(declared, "name"),
                        .bytes = std::move(bytes),
                    }
                );
                paths.emplace_back(path);
            }
            // The registration states them in JCS order and validateClaims
            // refuses any other. Sorting here rather than asking the author to
            // is the same decision as deriving the digests: an order is not
            // something a project has an opinion about.
            std::ranges::sort(
                blobs,
                [](std::string_view left, std::string_view right)
                {
                    return jsonMemberNameLess(left, right);
                },
                &operator_runtime::ProjectResourceBlob::name
            );
            for (auto index = std::size_t{1}; index < blobs.size(); ++index)
            {
                if (blobs[index - 1U].name == blobs[index].name)
                {
                    return refuse(std::format(
                        "a deployment declares the resource {} twice",
                        blobs[index].name
                    ));
                }
            }
            std::ranges::sort(paths);
            if (std::ranges::adjacent_find(paths) != paths.end())
            {
                return refuse("a deployment's resource paths must be unique");
            }
            UF_TRY(operator_runtime::validateProjectResourceClosure(blobs));
            return blobs;
        }

        [[nodiscard]]
        auto projectResourceClaimsOf(
            std::span<operator_runtime::ProjectResourceBlob const>
                blobs
        ) -> Result<std::vector<operator_runtime::ProjectResource>>
        {
            auto resources = std::vector<operator_runtime::ProjectResource>{};
            for (auto const& blob : blobs)
            {
                UF_TRY_VALUE(resourceHash, hashOf(blob.bytes));
                resources.emplace_back(operator_runtime::ProjectResource{
                    .kind = blob.kind,
                    .name = blob.name,
                    .hash = resourceHash,
                    .size = static_cast<uint64>(blob.bytes.size()),
                });
            }
            return resources;
        }

        [[nodiscard]]
        auto readVocabulary(json::Value const& declared) -> ProjectVocabulary
        {
            auto const& action = member(declared, "ui_action");
            return ProjectVocabulary{
                .mutatingTool         = text(declared, "mutating_tool"),
                .otherMutatingTool    = text(declared, "other_mutating_tool"),
                .readOnlyTool         = text(declared, "read_only_tool"),
                .toolArguments        = text(declared, "tool_arguments"),
                .refusedToolArguments = text(declared, "refused_tool_arguments"),
                .absentTool           = text(declared, "absent_tool"),
                .approvalRequiredPlanTool =
                    text(declared, "approval_required_plan_tool"),
                .uiAction = ProjectUiAction{
                    .surface  = text(action, "surface"),
                    .uiTarget = text(action, "ui_target"),
                    .action   = text(action, "action"),
                },
            };
        }

        // R8. Every agreement whose two halves are both authored in this
        // directory, refused where they were written rather than where a suite
        // trips over them. A role's vocabulary is the second half of one of
        // them: the five Tool names this deployment's declarations either carry
        // or do not.
        //
        // Each is a case that would otherwise pass while proving nothing, and
        // the directory is the only place both halves exist. R8's one half that
        // cannot be here is the probe frame's extent: after the Q2 ruling it
        // does not exist until the Host has activated the artifact. See
        // project-as-data.md 2.7 R8.
        [[nodiscard]]
        auto requireVocabularyAgrees(
            std::string_view role,
            ProjectConformanceRole const& played,
            LoadedDeployment const& deployment
        ) -> Status
        {
            auto const& vocabulary = played.vocabulary;
            auto const& catalog    = deployment.catalog;

            // The four names the deployment must declare, and the mutability
            // each is provisioned to demonstrate. A name declared with the
            // other mutability is worse than a missing one: the suite still
            // runs, and what it proves is that mutability came from the
            // caller.
            struct CarriedToolClaim final
            {
                std::string_view                 member{};
                std::string_view                 name{};
                operator_runtime::ToolMutability mutability{};
            };

            using operator_runtime::ToolMutability;
            for (auto const& claim : std::array{
                     CarriedToolClaim{
                         .member     = "mutating_tool",
                         .name       = vocabulary.mutatingTool,
                         .mutability = ToolMutability::Mutating,
                     },
                     CarriedToolClaim{
                         .member     = "other_mutating_tool",
                         .name       = vocabulary.otherMutatingTool,
                         .mutability = ToolMutability::Mutating,
                     },
                     CarriedToolClaim{
                         .member     = "read_only_tool",
                         .name       = vocabulary.readOnlyTool,
                         .mutability = ToolMutability::ReadOnly,
                     },
                     CarriedToolClaim{
                         .member     = "approval_required_plan_tool",
                         .name       = vocabulary.approvalRequiredPlanTool,
                         .mutability = ToolMutability::Mutating,
                     },
                 })
            {
                auto const carried = catalog.carriedTool(claim.name);
                if (!carried.has_value())
                {
                    return refuse(std::format(
                        "{}'s {} names {}, which the deployment {} does "
                        "not declare",
                        role,
                        claim.member,
                        claim.name,
                        played.deployment
                    ));
                }
                if (carried->mutability != claim.mutability)
                {
                    return refuse(std::format(
                        "{}'s {} names {}, which the deployment {} declares "
                        "as {} rather than as {}",
                        role,
                        claim.member,
                        claim.name,
                        played.deployment,
                        operator_runtime::toolMutabilityWireName(
                            carried->mutability
                        ),
                        operator_runtime::toolMutabilityWireName(
                            claim.mutability
                        )
                    ));
                }
            }

            if (vocabulary.otherMutatingTool == vocabulary.mutatingTool)
            {
                return refuse(std::format(
                    "{}'s mutating_tool and other_mutating_tool both name {}; "
                    "the one-live-chain rule is proven by a second command "
                    "naming a different tool",
                    role,
                    vocabulary.mutatingTool
                ));
            }

            if (catalog.carriedTool(vocabulary.absentTool).has_value())
            {
                return refuse(std::format(
                    "{}'s absent_tool names {}, which the deployment {} "
                    "declares; the member exists so that the refusal of an "
                    "unknown tool is falsifiable, and a declared name leaves "
                    "that case passing with nothing red anywhere",
                    role,
                    vocabulary.absentTool,
                    played.deployment
                ));
            }

            return ok();
        }

        // The commitment one deployment is loaded against: what a caller
        // recorded earlier, or -- when no caller recorded anything -- the
        // digest just computed.
        [[nodiscard]]
        auto commitmentFor(
            std::span<ExpectedRegistration const> expected,
            std::string_view name,
            ContentHash computed
        ) -> ContentHash
        {
            for (auto const& candidate : expected)
            {
                if (candidate.deployment == name)
                {
                    return candidate.hash;
                }
            }
            return computed;
        }

        [[nodiscard]]
        auto requireUniqueCommitments(
            std::span<ExpectedRegistration const> expected
        ) -> Status
        {
            for (auto first = std::size_t{0}; first < expected.size(); ++first)
            {
                for (auto second = first + 1U; second < expected.size(); ++second)
                {
                    if (expected[first].deployment == expected[second].deployment)
                    {
                        return refuse(std::format(
                            "project_registration_hash was presented more than once for "
                            "the deployment {}",
                            expected[first].deployment
                        ));
                    }
                }
            }
            return ok();
        }

        class ProjectLoader final
        {
        public:
            [[nodiscard]]
            static auto load(
                std::filesystem::path const& directory,
                std::span<ExpectedRegistration const> expected
            ) -> Result<LoadedProject>;
        };
    }

    auto LoadedProject::findDeployment(std::string_view name) const
        -> LoadedDeployment const*
    {
        auto const found =
            std::ranges::find(deployments, name, &LoadedDeployment::name);
        return found == deployments.end() ? nullptr : &*found;
    }

    auto ProjectLoader::load(
        std::filesystem::path const& directory,
        std::span<ExpectedRegistration const> expected
    ) -> Result<LoadedProject>
    {
        UF_TRY(requireUniqueCommitments(expected));
        UF_TRY_VALUE(root, openRoot(directory));
        UF_TRY_VALUE(
            projectBytes,
            readRootDocument(
                root,
                directory,
                k_projectManifestFileName,
                "loading a project"
            )
        );

        UF_TRY_VALUE(publishedProject, publishedSchema(k_projectSchemaPath));
        UF_TRY_VALUE(
            projectSchema,
            compile(publishedProject.relativePath, publishedProject.exactBytes)
        );
        UF_TRY_VALUE(
            publishedRegistration,
            publishedSchema(k_registrationSchemaPath)
        );
        UF_TRY_VALUE(
            registrationSchema,
            compile(
                publishedRegistration.relativePath,
                publishedRegistration.exactBytes
            )
        );
        UF_TRY_VALUE(
            manifest,
            readValidated(projectSchema, k_projectManifestFileName, projectBytes)
        );

        // The registration reader, held as the validator it is. There is no
        // owner around it: ProjectGeneration::verifyExact takes the reader
        // directly, and a wrapper would only restate that this loader is the
        // one that reads.
        auto const registrationValidate =
            registrationValidator(std::move(registrationSchema));

        auto loaded = LoadedProject{
            .directory           = directory,
            .runtimeArtifactRoot = {},
            .primaryDeployment   = text(manifest, "primary_deployment"),
            .deployments         = {},
        };

        // The RuntimeArtifact root is named rather than fixed, and what proves
        // it is a root is the model the installer will read out of it. There is
        // no directory test here on purpose: ConfinedRoot exists so that a path
        // is not inspected and then opened again by name.
        auto const artifactRoot = text(manifest, "runtime_artifact");
        UF_TRY(requireManifestSpelling("runtime_artifact", artifactRoot));
        UF_TRY_VALUE(
            model,
            readFile(
                root,
                "the RuntimeArtifact runtime_artifact names",
                artifactRoot + "/" + std::string{task::k_runtimeModelFileName},
                k_maximumDocumentBytes
            )
        );
        if (model.empty())
        {
            return refuse(std::format(
                "the RuntimeArtifact at {} carries an empty {}",
                artifactRoot,
                task::k_runtimeModelFileName
            ));
        }
        loaded.runtimeArtifactRoot = directory / artifactRoot;

        for (auto const& block : member(manifest, "deployments").items())
        {
            auto const name     = text(block, "name");
            auto const pluginId = text(block, "plugin_id");
            if (loaded.findDeployment(name) != nullptr)
            {
                return refuse(std::format(
                    "{} declares the deployment {} twice",
                    k_projectManifestFileName,
                    name
                ));
            }

            UF_TRY_VALUE(files, readDeploymentFiles(root, block));
            UF_TRY_VALUE(resources, readProjectResources(root, block));
            UF_TRY_VALUE(resourceClaims, projectResourceClaimsOf(resources));
            UF_TRY_VALUE(toolBindings, readToolBindings(block));

            auto identityViews = std::vector<ProjectIdentitySchemaSource>{};
            identityViews.reserve(files.observedInstanceIdentitySchemas.size());
            for (auto const& declared : files.observedInstanceIdentitySchemas)
            {
                identityViews.emplace_back(ProjectIdentitySchemaSource{
                    .name   = declared.name,
                    .schema = declared.schema,
                });
            }
            auto const sources = ProjectDeploymentSources{
                .pluginId                        = pluginId,
                .tools                           = files.tools,
                .observedInstanceIdentitySchemas = identityViews,
            };
            // R5, R6 and R7 are all inside this call. It judges the Tool
            // declarations against the one published statement of their shape,
            // compiles every schema they and the identity set state inline
            // under the evaluator's closed keyword set, and refuses a Tool
            // named outside the namespace this deployment registered.
            auto deployed = ProjectDeployment::create(sources);
            if (!deployed.has_value())
            {
                return refuse(std::format(
                    "the deployment {} does not hold together: {}",
                    name,
                    deployed.error().message()
                ));
            }

            UF_TRY_VALUE(
                toolModuleManifestHash,
                operator_runtime::derivePluginModuleManifestHash(
                    files.toolClosure.entryModule,
                    files.toolClosure.modules
                )
            );
            UF_TRY_VALUE(
                pluginEnvironmentHash,
                operator_runtime::currentProjectPluginEnvironmentHash()
            );
            UF_TRY_VALUE(catalogHash, hashOf(files.tools));

            // The identity schema hashes, derived from the canonical bytes this
            // loader rendered and sorted and deduplicated by that derivation.
            // The registration schema cannot state sortedness, so the
            // framework's own reading of the derived document refuses any other
            // order; what a project author writes is never consulted, on the
            // same terms as every other digest here.
            auto identityHashes = std::vector<ContentHash>{};
            identityHashes.reserve(files.observedInstanceIdentitySchemas.size());
            for (auto const& declared : files.observedInstanceIdentitySchemas)
            {
                UF_TRY_VALUE(identityHash, hashOf(declared.schema));
                identityHashes.emplace_back(identityHash);
            }
            std::ranges::sort(identityHashes);
            auto const [identityUniqueBegin, identityUniqueEnd] =
                std::ranges::unique(identityHashes);
            identityHashes.erase(identityUniqueBegin, identityUniqueEnd);

            auto const derived = DerivedRegistration{
                .pluginId    = pluginId,
                .toolClosure = operator_runtime::ProjectClosureClaims{
                    .moduleManifestHash  = toolModuleManifestHash,
                    .exportedEntryPoints = files.toolClosure.declaredEntryPoints,
                },
                .pluginEnvironmentHash                = pluginEnvironmentHash,
                .toolCatalogHash                      = catalogHash,
                .observedInstanceIdentitySchemaHashes = std::move(identityHashes),
            };

            auto const canonicalJcs =
                registrationJcs(derived, resourceClaims, toolBindings);
            UF_TRY_VALUE(computed, hashOf(canonicalJcs));

            // The one comparison on this chain between two values produced at
            // two different times, and therefore the only one that can fail.
            // When a caller named this deployment the hash below is what a
            // stored SessionManifest recorded; when it did not, no prior
            // commitment exists and the comparison is deliberately with the
            // value just computed. It prints both sides because the two hashes
            // and the deployment are the whole of the diagnosis.
            auto const commitment = commitmentFor(expected, name, computed);
            if (commitment != computed)
            {
                return refuse(std::format(
                    "the deployment {} was recorded as project_registration_hash "
                    "{} and this directory derives {}: a pinned file has moved",
                    name,
                    commitment.hex(),
                    computed.hex()
                ));
            }

            // expectedRootHash here is the digest computed two lines up, from
            // the bytes being passed in. That is the legal and empty case
            // named in project-as-data.md 7.0: this call proves the schema and
            // the claims, and proves nothing at all about the root. What a
            // caller recorded earlier is compared above, once, where both
            // values can be named in the refusal.
            auto registration = operator_runtime::ProjectGeneration::verifyExact(
                canonicalJcs,
                computed,
                registrationValidate
            );
            if (!registration.has_value())
            {
                return refuse(std::format(
                    "the registration derived for {} is not one this framework "
                    "can verify: {}",
                    name,
                    registration.error().message()
                ));
            }

            auto catalogOwner =
                operator_runtime::ProjectToolCatalogSchemaOwner::create(
                    *registration,
                    files.tools,
                    deployed->toolCatalogReader(),
                    deployed->toolArgumentValidator()
                );
            if (!catalogOwner.has_value())
            {
                return std::unexpected{catalogOwner.error().clone()};
            }
            // The authority obeys the registration's own rule and refuses a
            // supplied set that is not the canonical sorted order, so the
            // bindings are sorted here, the same derivation that wrote the
            // registration -- the deployment block's declaration order is
            // never consulted, on the same terms as every other digest.
            auto identitySchemas = deployed->observedIdentitySchemas();
            std::ranges::sort(
                identitySchemas,
                {},
                &operator_runtime::ObservedInstanceIdentitySchema::schemaHash
            );
            auto identitySet =
                operator_runtime::ObservedInstanceIdentitySchemas::create(
                    *registration,
                    std::move(identitySchemas)
                );
            if (!identitySet.has_value())
            {
                return std::unexpected{identitySet.error().clone()};
            }

            loaded.deployments.emplace_back(LoadedDeployment{
                .name                            = name,
                .generation                      = *std::move(registration),
                .toolCatalogSchemaOwner          = *std::move(catalogOwner),
                .observedInstanceIdentitySchemas = *std::move(identitySet),
                .catalog                         = *std::move(deployed),
                .toolClosure                     = std::move(files.toolClosure),
                .projectResources                = std::move(resources),
            });
        }

        // A commitment naming a deployment this directory does not declare is a
        // refusal rather than a value nobody read. Without it a misspelled name
        // would disarm the one check on this chain that can fail, silently.
        for (auto const& commitment : expected)
        {
            if (loaded.findDeployment(commitment.deployment) == nullptr)
            {
                return refuse(std::format(
                    "a project_registration_hash was presented for the "
                    "deployment {}, which {} does not declare",
                    commitment.deployment,
                    k_projectManifestFileName
                ));
            }
        }

        if (loaded.findDeployment(loaded.primaryDeployment) == nullptr)
        {
            return refuse(std::format(
                "primary_deployment names {}, which is not one of this "
                "project's deployments",
                loaded.primaryDeployment
            ));
        }

        return loaded;
    }

    auto loadProductionProject(
        std::filesystem::path const& directory,
        std::span<ExpectedRegistration const> expected
    ) -> Result<LoadedProject>
    {
        return ProjectLoader::load(directory, expected);
    }

    auto loadConformanceProject(
        std::filesystem::path const& directory,
        std::span<ExpectedRegistration const> expected
    ) -> Result<ConformanceProject>
    {
        UF_TRY_VALUE(loaded, ProjectLoader::load(directory, expected));

        // The directory is confined a second time rather than threaded out of
        // the load above: what a load returns is what it read, and a project is
        // a value rather than a handle a caller keeps open. Every path below is
        // therefore resolved under the same confinement rules as every path the
        // production load resolved.
        UF_TRY_VALUE(root, openRoot(directory));
        UF_TRY_VALUE(
            conformanceBytes,
            readRootDocument(
                root,
                directory,
                k_conformanceManifestFileName,
                "running the conformance suite"
            )
        );
        UF_TRY_VALUE(
            conformanceSchema,
            compile("umbraflow-conformance/v3", k_conformanceSchema)
        );
        UF_TRY_VALUE(
            conformance,
            readValidated(
                conformanceSchema,
                k_conformanceManifestFileName,
                conformanceBytes
            )
        );

        auto const framePath = text(conformance, "probe_frame");
        UF_TRY_VALUE(
            frame,
            readFile(root, "probe_frame", framePath, k_maximumFrameBytes)
        );
        auto const frameBytes = std::as_bytes(std::span{frame});

        // R9. A named file must be what the member names it as, and for the one
        // member naming a capture that means the bytes decode. Only the decoded
        // extent is out of reach here (R8); whether there is an image at all is
        // not, and a project whose probe frame is not one is refused where it
        // was written rather than several minutes into a suite. The decoded
        // pixels are dropped on purpose: the capture the Host is handed is the
        // project's own bytes, and a second copy of them in another encoding
        // would be a second spelling of the frame.
        auto const decoded = image::decodePng(frameBytes, framePath);
        if (!decoded.has_value())
        {
            return refuse(std::format(
                "probe_frame names {}, which is not a PNG capture this "
                "framework can decode: {}",
                framePath,
                decoded.error().message()
            ));
        }

        auto const role =
            [&loaded, &conformance](
                std::string_view name
            ) -> Result<ProjectConformanceRole>
        {
            auto const& declared = member(conformance, name);
            auto const  played   = text(declared, "deployment");
            if (loaded.findDeployment(played) == nullptr)
            {
                return refuse(std::format(
                    "{} is played by the deployment {}, which {} does not "
                    "declare",
                    name,
                    played,
                    k_projectManifestFileName
                ));
            }
            return ProjectConformanceRole{
                .deployment = played,
                .vocabulary = readVocabulary(member(declared, "vocabulary")),
            };
        };
        UF_TRY_VALUE(underTest, role("under_test"));
        UF_TRY_VALUE(foreign, role("foreign"));
        if (underTest.deployment == foreign.deployment)
        {
            return refuse(std::format(
                "under_test and foreign are both played by {}; authority is per "
                "registration, so proving it does not cross needs a second one",
                underTest.deployment
            ));
        }

        auto project = ConformanceProject{
            .loaded     = std::move(loaded),
            .probeFrame = {frameBytes.begin(), frameBytes.end()},
            .underTest  = std::move(underTest),
            .foreign    = std::move(foreign),
        };

        // R8, the half a loader can answer, for each role in turn. The member
        // name is carried into the refusal because the two roles carry two
        // vocabularies and a disagreement in one of them says nothing about the
        // other.
        struct PlayedRole final
        {
            std::string_view              member{};
            ProjectConformanceRole const* p_role{};
        };

        for (auto const& played : std::array{
                 PlayedRole{"under_test", &project.underTest},
                 PlayedRole{"foreign", &project.foreign},
             })
        {
            auto const* const p_deployment =
                project.loaded.findDeployment(played.p_role->deployment);
            UF_CHECK(p_deployment != nullptr);
            UF_TRY(
                requireVocabularyAgrees(played.member, *played.p_role, *p_deployment)
            );
        }

        return project;
    }
}

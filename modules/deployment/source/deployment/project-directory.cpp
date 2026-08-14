#include "project-directory.hpp"

#include <json/schema.hpp>
#include <json/value.hpp>

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/safety/checked-access.hpp>
#include <core/text/json-text.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <image/png.hpp>

#include <schema/framework-schema-catalog.hpp>

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
            "schema/umbraflow-project-registration-v1.schema.json"
        };

        // umbraflow-project.json: the document production reads.
        //
        // Every path member is typed as a bare non-empty string and given its
        // spelling rule by requireManifestSpelling below, because that rule is
        // task_platform::ConfinedRoot's own (confined-file.hpp:52-55). Stating
        // it a second time as a regular expression would be two spellings of
        // one thing, and the weaker of the two would decide.
        constexpr auto k_projectSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/project/directory",
    "title": "umbraflow-project.json",
    "$comment": "A deployment block IS its registration, stated as intent: it names files, and the loader derives every digest from the bytes it read. No member here is a hash and no project author types one.",
    "type": "object",
    "additionalProperties": false,
    "required": [
        "deployments",
        "primary_deployment",
        "runtime_artifact",
        "schema"
    ],
    "properties": {
        "$comment": {"type": "string"},
        "schema": {"const": "umbraflow-project/v1"},
        "runtime_artifact": {
            "$comment": "A directory, not a file: the installer reads runtime-model.toml and runtime-artifact.manifest.json out of a root by those fixed names.",
            "$ref": "#/$defs/Path"
        },
        "policy_artifact": {
            "$comment": "Optional exact PolicyArtifact bytes. When absent, production uses the Operator-owned deny-all artifact; the project never evaluates policy.",
            "$ref": "#/$defs/Path"
        },
        "primary_deployment": {"$ref": "#/$defs/DeploymentName"},
        "deployments": {
            "type": "array",
            "minItems": 1,
            "items": {"$ref": "#/$defs/Deployment"}
        }
    },
    "$defs": {
        "Path": {"type": "string", "minLength": 1},
        "PluginJustification": {
            "$comment": "Why this deployment writes a whole five-function Luau module instead of a declarative-tools declaration: it must name the umbraflow-declarative-workflow-tool/v1 member or semantic that cannot express the behaviour. PRESENCE ONLY. No reader here judges whether the text is true, and no reader can; the pattern requires one non-whitespace character and nothing more. A justification that names the wrong member is a review finding at plugin acceptance, not a refusal -- see docs/pitfalls/checks-that-cannot-fail.md.",
            "type": "string",
            "minLength": 1,
            "pattern": "\\S"
        },
        "DeploymentName": {
            "type": "string",
            "minLength": 1,
            "maxLength": 128,
            "pattern": "^[a-z][a-z0-9-]*$"
        },
        "NamespacedName": {
            "type": "string",
            "minLength": 3,
            "maxLength": 128,
            "pattern": "^[a-z][a-z0-9_-]*(\\.[a-z][a-z0-9_-]*)+$"
        },
        "Deployment": {
            "type": "object",
            "additionalProperties": false,
            "required": [
                "artifact_blobs",
                "baseline_event_type",
                "effect_payload_schemas",
                "journal_event_schema_manifest",
                "journal_payload_schemas",
                "name",
                "plugin",
                "plugin_id",
                "plugin_justification",
                "project_observation_schema",
                "project_state_schema",
                "reconcile_manifest",
                "reconcile_schema",
                "tool_catalog",
                "tool_precondition_schema"
            ],
            "properties": {
                "$comment": {"type": "string"},
                "name": {"$ref": "#/$defs/DeploymentName"},
                "plugin_id": {"$ref": "#/$defs/NamespacedName"},
                "baseline_event_type": {"$ref": "#/$defs/NamespacedName"},
                "plugin": {"$ref": "#/$defs/Path"},
                "plugin_justification": {"$ref": "#/$defs/PluginJustification"},
                "project_state_schema": {"$ref": "#/$defs/Path"},
                "project_observation_schema": {"$ref": "#/$defs/Path"},
                "tool_precondition_schema": {"$ref": "#/$defs/Path"},
                "reconcile_schema": {"$ref": "#/$defs/Path"},
                "tool_catalog": {"$ref": "#/$defs/Path"},
                "journal_event_schema_manifest": {"$ref": "#/$defs/Path"},
                "reconcile_manifest": {"$ref": "#/$defs/Path"},
                "journal_payload_schemas": {
                    "$comment": "The payload schema files this deployment supplies. The journal event schema manifest names each of them by sha256 and never by path, so which file answers for which event type is decided by the bytes rather than by a name written down twice.",
                    "type": "array",
                    "minItems": 1,
                    "items": {"$ref": "#/$defs/Path"},
                    "uniqueItems": true
                },
                "effect_payload_schemas": {
                    "$comment": "The effect payload schema files this deployment supplies. No manifest names them: the Tool Catalog's effect_payload_sha256s names each by sha256, which is the only route their bytes have into tool_catalog_hash, and the payload_schema_hash inside an OP:ExpectedEffect is what selects which of them judges it.",
                    "type": "array",
                    "items": {"$ref": "#/$defs/Path"},
                    "uniqueItems": true
                },
                "artifact_blobs": {
                    "type": "array",
                    "items": {"$ref": "#/$defs/ArtifactBlob"}
                }
            }
        },
        "ArtifactBlob": {
            "type": "object",
            "additionalProperties": false,
            "required": ["name", "path"],
            "properties": {
                "$comment": {"type": "string"},
                "name": {
                    "type": "string",
                    "minLength": 1,
                    "maxLength": 128,
                    "pattern": "^[a-z][a-z0-9_-]*(\\.[a-z][a-z0-9_-]*)*$"
                },
                "path": {"$ref": "#/$defs/Path"}
            }
        }
    }
})json"};

        // umbraflow-conformance.json: the document only a conformance run
        // reads. loadProductionProject never opens it, and that separation is
        // the point -- nothing in production wants a "tool the catalog does not
        // carry", and a project at a read-only phase has no mutating tool to
        // name in one.
        constexpr auto k_conformanceSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/project/conformance",
    "title": "umbraflow-conformance.json",
    "$comment": "There is no fingerprint member. The extent a probe frame is checked against is the one RuntimeModelBinding publishes out of the model, never a number this document restates.",
    "type": "object",
    "additionalProperties": false,
    "required": ["foreign", "probe_frame", "schema", "under_test"],
    "properties": {
        "$comment": {"type": "string"},
        "schema": {"const": "umbraflow-conformance/v1"},
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
            "$comment": "The project's exact bytes, carried as a JSON string rather than as a nested object. They are handed to ProjectSchemaOwner::canonicalize, which refuses anything that is not exact RFC 8785 JCS; a nested object would make the loader choose a serialization and the bytes would stop being the project's.",
            "type": "string",
            "minLength": 1
        },
        "JournalDocument": {
            "type": "object",
            "additionalProperties": false,
            "required": ["event_type", "payload"],
            "properties": {
                "$comment": {"type": "string"},
                "event_type": {
                    "type": "string",
                    "minLength": 3,
                    "maxLength": 128,
                    "pattern": "^[a-z][a-z0-9_-]*(\\.[a-z][a-z0-9_-]*)+$"
                },
                "payload": {"$ref": "#/$defs/Document"}
            }
        },
        "Vocabulary": {
            "type": "object",
            "additionalProperties": false,
            "required": [
                "absent_tool",
                "ambiguous_input",
                "approval_required_plan_tool",
                "baseline_entry",
                "confirmed_entry",
                "confirmed_input",
                "continue_input",
                "mutating_tool",
                "other_mutating_tool",
                "progress_entry",
                "provenance",
                "read_only_tool",
                "refused_tool_arguments",
                "rejected_input",
                "superseded_entry",
                "tool_arguments",
                "ui_action"
            ],
            "properties": {
                "$comment": {"type": "string"},
                "mutating_tool": {"type": "string", "minLength": 1},
                "other_mutating_tool": {"type": "string", "minLength": 1},
                "read_only_tool": {"type": "string", "minLength": 1},
                "tool_arguments": {"$ref": "#/$defs/Document"},
                "refused_tool_arguments": {"$ref": "#/$defs/Document"},
                "absent_tool": {"type": "string", "minLength": 1},
                "baseline_entry": {"$ref": "#/$defs/JournalDocument"},
                "progress_entry": {"$ref": "#/$defs/JournalDocument"},
                "confirmed_entry": {"$ref": "#/$defs/JournalDocument"},
                "superseded_entry": {"$ref": "#/$defs/JournalDocument"},
                "provenance": {"$ref": "#/$defs/Document"},
                "continue_input": {"$ref": "#/$defs/Document"},
                "confirmed_input": {"$ref": "#/$defs/Document"},
                "rejected_input": {"$ref": "#/$defs/Document"},
                "ambiguous_input": {"$ref": "#/$defs/Document"},
                "approval_required_plan_tool": {"type": "string", "minLength": 1},
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
        constexpr auto k_maximumPluginBytes   = std::size_t{1U << 22U};
        constexpr auto k_maximumBlobBytes     = std::size_t{1U << 24U};
        constexpr auto k_maximumFrameBytes    = std::size_t{1U << 26U};

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
            std::string pluginId{};
            std::string baselineEventType{};
            ContentHash manifestSchemaHash;
            ContentHash pluginHash;
            ContentHash toolCatalogHash;
            ContentHash projectStateSchemaHash;
            ContentHash projectObservationSchemaHash;
            ContentHash projectToolPreconditionSchemaHash;
            ContentHash reconcilePayloadSchemaManifestHash;
            ContentHash journalEventSchemaManifestHash;
        };

        // The registration document, as exact RFC 8785 JCS. It is assembled as
        // a value and serialized by json::canonicalBytes rather than formatted,
        // so the one spelling of RFC 8785 in this tree is what produces the
        // document whose digest is a project's identity.
        [[nodiscard]]
        auto registrationJcs(
            DerivedRegistration const& derived,
            std::span<operator_runtime::NamedArtifactRoot const> artifactRoots
        ) -> std::string
        {
            auto const hash = [](ContentHash const& value)
            {
                return json::Value::ofString(value.hex());
            };

            auto roots = std::vector<json::Value>{};
            for (auto const& root : artifactRoots)
            {
                roots.emplace_back(json::Value::ofObject({
                    {"name", json::Value::ofString(root.name)},
                    {"root_hash", hash(root.rootHash)},
                }));
            }

            return json::canonicalBytes(json::Value::ofObject({
                {"baseline_event_type",
                 json::Value::ofString(derived.baselineEventType)},
                {"journal_event_schema_manifest_hash",
                 hash(derived.journalEventSchemaManifestHash)},
                {"manifest_schema_hash", hash(derived.manifestSchemaHash)},
                {"plugin_hash", hash(derived.pluginHash)},
                {"plugin_id", json::Value::ofString(derived.pluginId)},
                {"project_artifact_roots", json::Value::ofArray(std::move(roots))},
                {"project_observation_schema_hash",
                 hash(derived.projectObservationSchemaHash)},
                {"project_state_schema_hash", hash(derived.projectStateSchemaHash)},
                {"project_tool_precondition_schema_hash",
                 hash(derived.projectToolPreconditionSchemaHash)},
                {"reconcile_payload_schema_manifest_hash",
                 hash(derived.reconcilePayloadSchemaManifestHash)},
                {"tool_catalog_hash", hash(derived.toolCatalogHash)},
            }));
        }

        [[nodiscard]]
        auto parseHash(json::Value const& object, std::string_view name)
            -> Result<ContentHash>
        {
            return ContentHash::parse("sha256:" + text(object, name));
        }

        // The framework's own reading of the document it just derived. It is a
        // whole ProjectRegistrationExactValidator: the registration schema
        // judges the members, and this reads back only what that schema
        // accepted.
        [[nodiscard]]
        auto registrationValidator(json::Schema schema)
            -> operator_runtime::ProjectRegistrationExactValidator
        {
            return [judge = std::move(schema)](std::string_view exactJcs)
                       -> Result<operator_runtime::ProjectRegistrationClaims>
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
                    manifestSchemaHash,
                    parseHash(document, "manifest_schema_hash")
                );
                UF_TRY_VALUE(pluginHash, parseHash(document, "plugin_hash"));
                UF_TRY_VALUE(catalogHash, parseHash(document, "tool_catalog_hash"));
                UF_TRY_VALUE(
                    stateHash,
                    parseHash(document, "project_state_schema_hash")
                );
                UF_TRY_VALUE(
                    observationHash,
                    parseHash(document, "project_observation_schema_hash")
                );
                UF_TRY_VALUE(
                    preconditionHash,
                    parseHash(document, "project_tool_precondition_schema_hash")
                );
                UF_TRY_VALUE(
                    reconcileHash,
                    parseHash(document, "reconcile_payload_schema_manifest_hash")
                );
                UF_TRY_VALUE(
                    journalHash,
                    parseHash(document, "journal_event_schema_manifest_hash")
                );

                auto roots = std::vector<operator_runtime::NamedArtifactRoot>{};
                for (auto const& root :
                     member(document, "project_artifact_roots").items())
                {
                    UF_TRY_VALUE(rootHash, parseHash(root, "root_hash"));
                    roots.emplace_back(operator_runtime::NamedArtifactRoot{
                        .name     = text(root, "name"),
                        .rootHash = rootHash,
                    });
                }

                return operator_runtime::ProjectRegistrationClaims{
                    .manifestSchemaHash                 = manifestSchemaHash,
                    .pluginId                           = text(document, "plugin_id"),
                    .pluginHash                         = pluginHash,
                    .toolCatalogHash                    = catalogHash,
                    .projectStateSchemaHash             = stateHash,
                    .projectObservationSchemaHash       = observationHash,
                    .projectToolPreconditionSchemaHash  = preconditionHash,
                    .reconcilePayloadSchemaManifestHash = reconcileHash,
                    .journalEventSchemaManifestHash     = journalHash,
                    .baselineEventType                  = text(document, "baseline_event_type"),
                    .projectArtifactRoots               = std::move(roots),
                };
            };
        }

        // The document validator the five authorities are built on, wrapped so
        // that the exact bytes a Reduce or Derive input arrived as are kept.
        // The wrapper is the loader's because the thing that observes them is
        // the host's validator: a directory of data has nowhere to put a value
        // that exists only while a suite runs.
        [[nodiscard]]
        auto recordingValidator(
            operator_runtime::ProjectDocumentValidator judge,
            std::shared_ptr<ProjectDocumentInputLog> p_inputLog
        ) -> operator_runtime::ProjectDocumentValidator
        {
            return [
                judge    = std::move(judge),
                inputLog = std::move(p_inputLog)
            ](
                operator_runtime::ProjectPluginFunction function,
                operator_runtime::ProjectDocumentDirection direction,
                std::string_view exactJcs
            ) -> Status
            {
                using operator_runtime::ProjectDocumentDirection;

                if (direction == ProjectDocumentDirection::Input)
                {
                    inputLog->record(function, exactJcs);
                }
                return judge(function, direction, exactJcs);
            };
        }

        // Every file one deployment block names, as owned bytes. The views
        // ProjectDeploymentSources takes are taken from these, so they must
        // outlive the create call.
        struct DeploymentFiles final
        {
            std::string pluginBytes{};
            std::string projectState{};
            std::string projectObservation{};
            std::string toolPrecondition{};
            std::string reconcile{};
            std::string toolCatalog{};
            std::string journalEventManifest{};
            std::string reconcileManifest{};

            std::vector<std::string> journalPayloadSchemas{};
            std::vector<std::string> effectPayloadSchemas{};
        };

        [[nodiscard]]
        auto readList(
            task_platform::ConfinedRoot const& root,
            json::Value const& block,
            std::string_view name,
            std::string_view what
        ) -> Result<std::vector<std::string>>
        {
            auto bytes = std::vector<std::string>{};
            for (auto const& path : member(block, name).items())
            {
                UF_TRY_VALUE(
                    read,
                    readFile(root, what, path.string(), k_maximumDocumentBytes)
                );
                bytes.emplace_back(std::move(read));
            }
            return bytes;
        }

        [[nodiscard]]
        auto readDeploymentFiles(
            task_platform::ConfinedRoot const& root,
            json::Value const& block
        ) -> Result<DeploymentFiles>
        {
            auto const read =
                [&root, &block](std::string_view name, std::size_t bound)
            {
                return readFile(
                    root,
                    std::format("a deployment's {}", name),
                    member(block, name).string(),
                    bound
                );
            };

            UF_TRY_VALUE(plugin, read("plugin", k_maximumPluginBytes));
            UF_TRY_VALUE(state, read("project_state_schema", k_maximumDocumentBytes));
            UF_TRY_VALUE(
                observation,
                read("project_observation_schema", k_maximumDocumentBytes)
            );
            UF_TRY_VALUE(
                precondition,
                read("tool_precondition_schema", k_maximumDocumentBytes)
            );
            UF_TRY_VALUE(reconcile, read("reconcile_schema", k_maximumDocumentBytes));
            UF_TRY_VALUE(catalog, read("tool_catalog", k_maximumDocumentBytes));
            UF_TRY_VALUE(
                journal,
                read("journal_event_schema_manifest", k_maximumDocumentBytes)
            );
            UF_TRY_VALUE(
                reconcileManifest,
                read("reconcile_manifest", k_maximumDocumentBytes)
            );
            UF_TRY_VALUE(
                journalPayloads,
                readList(
                    root,
                    block,
                    "journal_payload_schemas",
                    "a deployment's journal_payload_schemas entry"
                )
            );
            UF_TRY_VALUE(
                effectPayloads,
                readList(
                    root,
                    block,
                    "effect_payload_schemas",
                    "a deployment's effect_payload_schemas entry"
                )
            );

            return DeploymentFiles{
                .pluginBytes           = std::move(plugin),
                .projectState          = std::move(state),
                .projectObservation    = std::move(observation),
                .toolPrecondition      = std::move(precondition),
                .reconcile             = std::move(reconcile),
                .toolCatalog           = std::move(catalog),
                .journalEventManifest  = std::move(journal),
                .reconcileManifest     = std::move(reconcileManifest),
                .journalPayloadSchemas = std::move(journalPayloads),
                .effectPayloadSchemas  = std::move(effectPayloads),
            };
        }

        [[nodiscard]]
        auto readArtifactBlobs(
            task_platform::ConfinedRoot const& root,
            json::Value const& block
        ) -> Result<std::vector<operator_runtime::ProjectPluginRegistrar::ArtifactBlob>>
        {
            auto blobs =
                std::vector<operator_runtime::ProjectPluginRegistrar::ArtifactBlob>{};
            for (auto const& declared : member(block, "artifact_blobs").items())
            {
                UF_TRY_VALUE(
                    bytes,
                    readFile(
                        root,
                        "a deployment's artifact_blobs entry",
                        member(declared, "path").string(),
                        k_maximumBlobBytes
                    )
                );
                blobs.emplace_back(
                    operator_runtime::ProjectPluginRegistrar::ArtifactBlob{
                        .name  = text(declared, "name"),
                        .bytes = std::move(bytes),
                    }
                );
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
                &operator_runtime::ProjectPluginRegistrar::ArtifactBlob::name
            );
            for (auto index = std::size_t{1}; index < blobs.size(); ++index)
            {
                if (blobs[index - 1U].name == blobs[index].name)
                {
                    return refuse(std::format(
                        "a deployment declares the artifact root {} twice",
                        blobs[index].name
                    ));
                }
            }
            return blobs;
        }

        [[nodiscard]]
        auto artifactRootsOf(
            std::span<operator_runtime::ProjectPluginRegistrar::ArtifactBlob const>
                blobs
        ) -> Result<std::vector<operator_runtime::NamedArtifactRoot>>
        {
            auto roots = std::vector<operator_runtime::NamedArtifactRoot>{};
            for (auto const& blob : blobs)
            {
                UF_TRY_VALUE(rootHash, hashOf(blob.bytes));
                roots.emplace_back(operator_runtime::NamedArtifactRoot{
                    .name     = blob.name,
                    .rootHash = rootHash,
                });
            }
            return roots;
        }

        [[nodiscard]]
        auto views(std::vector<std::string> const& owned UF_LIFETIME_BOUND)
            -> std::vector<std::string_view>
        {
            return std::vector<std::string_view>{owned.begin(), owned.end()};
        }

        [[nodiscard]]
        auto readVocabulary(json::Value const& declared) -> ProjectVocabulary
        {
            auto const entry = [](json::Value const& document)
            {
                return ProjectJournalDocument{
                    .eventType = text(document, "event_type"),
                    .payload   = text(document, "payload"),
                };
            };
            auto const& action = member(declared, "ui_action");
            return ProjectVocabulary{
                .mutatingTool         = text(declared, "mutating_tool"),
                .otherMutatingTool    = text(declared, "other_mutating_tool"),
                .readOnlyTool         = text(declared, "read_only_tool"),
                .toolArguments        = text(declared, "tool_arguments"),
                .refusedToolArguments = text(declared, "refused_tool_arguments"),
                .absentTool           = text(declared, "absent_tool"),
                .baselineEntry        = entry(member(declared, "baseline_entry")),
                .progressEntry        = entry(member(declared, "progress_entry")),
                .confirmedEntry       = entry(member(declared, "confirmed_entry")),
                .supersededEntry      = entry(member(declared, "superseded_entry")),
                .provenance           = text(declared, "provenance"),
                .continueInput        = text(declared, "continue_input"),
                .confirmedInput       = text(declared, "confirmed_input"),
                .rejectedInput        = text(declared, "rejected_input"),
                .ambiguousInput       = text(declared, "ambiguous_input"),
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
        // trips over them. A role's vocabulary is the second half of three of
        // them: the deployment's registered baseline event type, the five tool
        // names its Tool Catalog either carries or does not, and the four
        // journal payloads a suite has to be able to tell apart.
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
            auto const  registered = deployment.registration.baselineEventType();
            if (vocabulary.baselineEntry.eventType != registered)
            {
                return refuse(std::format(
                    "the deployment {} is registered with baseline_event_type "
                    "{}, and its vocabulary provisions {}",
                    played.deployment,
                    registered,
                    vocabulary.baselineEntry.eventType
                ));
            }

            // The four names the catalog must carry, and the mutability each is
            // provisioned to demonstrate. A name the catalog carries as the
            // other mutability is worse than a missing one: the suite still
            // runs, and what it proves is that mutability came from the caller.
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
                        "{}'s {} names {}, which the deployment {}'s Tool "
                        "Catalog does not carry",
                        role,
                        claim.member,
                        claim.name,
                        played.deployment
                    ));
                }
                if (carried->mutability != claim.mutability)
                {
                    return refuse(std::format(
                        "{}'s {} names {}, which the deployment {}'s Tool "
                        "Catalog carries as {} rather than as {}",
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
                    "{}'s absent_tool names {}, which the deployment {}'s Tool "
                    "Catalog carries; the member exists so that the catalog's "
                    "refusal of an unknown tool is falsifiable, and a carried "
                    "name leaves that case passing with nothing red anywhere",
                    role,
                    vocabulary.absentTool,
                    played.deployment
                ));
            }

            // All four journal payloads must differ. The reducer-input case
            // proves that an entry a commit did not name never reaches the
            // reducer, and two entries carrying one payload cannot show which
            // of them arrived.
            struct ProvisionedEntry final
            {
                std::string_view              member{};
                ProjectJournalDocument const* p_entry{};
            };

            auto const entries = std::array{
                ProvisionedEntry{"baseline_entry", &vocabulary.baselineEntry},
                ProvisionedEntry{"progress_entry", &vocabulary.progressEntry},
                ProvisionedEntry{"confirmed_entry", &vocabulary.confirmedEntry},
                ProvisionedEntry{"superseded_entry", &vocabulary.supersededEntry},
            };
            for (auto first = std::size_t{0}; first < entries.size(); ++first)
            {
                for (auto second = first + 1U; second < entries.size(); ++second)
                {
                    auto const& earlier = checkedAt(entries, first);
                    auto const& later   = checkedAt(entries, second);
                    if (earlier.p_entry->payload != later.p_entry->payload)
                    {
                        continue;
                    }
                    return refuse(std::format(
                        "{}'s {} and {} carry one payload; an entry a commit "
                        "did not name is provably absent from the reducer's "
                        "input only while the four payloads differ",
                        role,
                        earlier.member,
                        later.member
                    ));
                }
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
                std::span<ExpectedRegistration const> expected,
                std::shared_ptr<ProjectDocumentInputLog> const& p_inputLog
            ) -> Result<LoadedProject>;
        };
    }

    auto ProjectDocumentInputLog::record(
        operator_runtime::ProjectPluginFunction function,
        std::string_view exactJcs
    ) -> void
    {
        using operator_runtime::ProjectPluginFunction;

        // Only the state-reducing and observation-deriving inputs are contract
        // evidence. The table makes that subset explicit without a conditional
        // chain over the closed ProjectPluginFunction vocabulary.
        constexpr auto targets = std::array{
            std::pair{
                ProjectPluginFunction::Derive,
                &ProjectDocumentInputLog::m_lastDeriveInput
            },
            std::pair{
                ProjectPluginFunction::Reduce,
                &ProjectDocumentInputLog::m_lastReduceInput
            },
        };
        auto const target = std::ranges::find_if(
            targets,
            [function](auto const& candidate)
            {
                return candidate.first == function;
            }
        );
        if (target == targets.end())
        {
            return;
        }

        auto lock = std::lock_guard{m_mutex};
        (this->*target->second) = std::string{exactJcs};
    }

    auto ProjectDocumentInputLog::lastReduceInput() const -> std::string
    {
        auto lock = std::lock_guard{m_mutex};
        return m_lastReduceInput;
    }

    auto ProjectDocumentInputLog::lastDeriveInput() const -> std::string
    {
        auto lock = std::lock_guard{m_mutex};
        return m_lastDeriveInput;
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
        std::span<ExpectedRegistration const> expected,
        std::shared_ptr<ProjectDocumentInputLog> const& p_inputLog
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

        UF_TRY_VALUE(projectSchema, compile("umbraflow-project/v1", k_projectSchema));
        auto const publishedRegistration =
            framework_schema::findFrameworkSchema(k_registrationSchemaPath);
        if (!publishedRegistration.has_value())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "generated framework schema catalog is missing "
                    + std::string{k_registrationSchemaPath}
            );
        }
        UF_TRY_VALUE(
            registrationSchema,
            compile(
                publishedRegistration->relativePath,
                publishedRegistration->exactBytes
            )
        );
        UF_TRY_VALUE(
            manifest,
            readValidated(projectSchema, k_projectManifestFileName, projectBytes)
        );

        UF_TRY_VALUE(
            manifestSchemaHash,
            hashOf(publishedRegistration->exactBytes)
        );
        auto registrationOwner =
            operator_runtime::ProjectRegistrationSchemaOwner::create(
                manifestSchemaHash,
                registrationValidator(std::move(registrationSchema))
            );
        if (!registrationOwner.has_value())
        {
            return std::unexpected{registrationOwner.error().clone()};
        }

        auto loaded = LoadedProject{
            .directory           = directory,
            .runtimeArtifactRoot = {},
            .policyArtifactBytes = std::nullopt,
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

        if (auto const* const p_policy = manifest.find("policy_artifact"))
        {
            auto const policyPath = std::string{p_policy->string()};
            UF_TRY(requireManifestSpelling("policy_artifact", policyPath));
            UF_TRY_VALUE(
                policyBytes,
                readFile(
                    root,
                    "the PolicyArtifact policy_artifact names",
                    policyPath,
                    k_maximumDocumentBytes
                )
            );
            loaded.policyArtifactBytes = std::move(policyBytes);
        }

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
            UF_TRY_VALUE(blobs, readArtifactBlobs(root, block));
            UF_TRY_VALUE(artifactRoots, artifactRootsOf(blobs));

            auto const journalViews = views(files.journalPayloadSchemas);
            auto const effectViews  = views(files.effectPayloadSchemas);
            auto const sources      = ProjectDeploymentSources{
                     .pluginId              = pluginId,
                     .projectState          = files.projectState,
                     .projectObservation    = files.projectObservation,
                     .toolPrecondition      = files.toolPrecondition,
                     .reconcile             = files.reconcile,
                     .toolCatalog           = files.toolCatalog,
                     .journalEventManifest  = files.journalEventManifest,
                     .reconcileManifest     = files.reconcileManifest,
                     .journalPayloadSchemas = journalViews,
                     .effectPayloadSchemas  = effectViews,
            };
            // R5, R6 and R7 are all inside this call. It compiles every schema
            // under the evaluator's closed keyword set, holds the journal
            // manifest's per-schema sha256 and the reconcile manifest's schema
            // digest to the bytes they name, and refuses a catalog row that
            // omits mutability or surface.
            auto deployed = ProjectDeployment::create(sources);
            if (!deployed.has_value())
            {
                return refuse(std::format(
                    "the deployment {} does not hold together: {}",
                    name,
                    deployed.error().message()
                ));
            }

            UF_TRY_VALUE(pluginHash, hashOf(files.pluginBytes));
            UF_TRY_VALUE(catalogHash, hashOf(files.toolCatalog));
            UF_TRY_VALUE(stateHash, hashOf(files.projectState));
            UF_TRY_VALUE(observationHash, hashOf(files.projectObservation));
            UF_TRY_VALUE(preconditionHash, hashOf(files.toolPrecondition));
            UF_TRY_VALUE(reconcileHash, hashOf(files.reconcileManifest));
            UF_TRY_VALUE(journalHash, hashOf(files.journalEventManifest));
            auto const derived = DerivedRegistration{
                .pluginId                           = pluginId,
                .baselineEventType                  = text(block, "baseline_event_type"),
                .manifestSchemaHash                 = manifestSchemaHash,
                .pluginHash                         = pluginHash,
                .toolCatalogHash                    = catalogHash,
                .projectStateSchemaHash             = stateHash,
                .projectObservationSchemaHash       = observationHash,
                .projectToolPreconditionSchemaHash  = preconditionHash,
                .reconcilePayloadSchemaManifestHash = reconcileHash,
                .journalEventSchemaManifestHash     = journalHash,
            };

            auto const canonicalJcs = registrationJcs(derived, artifactRoots);
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
            auto registration = operator_runtime::ProjectRegistration::verifyExact(
                canonicalJcs,
                computed,
                *registrationOwner
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

            auto const documentSchemas =
                operator_runtime::ProjectDocumentSchemaBytes{
                    .projectState       = files.projectState,
                    .projectObservation = files.projectObservation,
                    .toolPrecondition   = files.toolPrecondition,
                };
            auto documentValidator = deployed->documentValidator();
            if (p_inputLog != nullptr)
            {
                documentValidator = recordingValidator(
                    std::move(documentValidator),
                    p_inputLog
                );
            }
            auto projectSchemaOwner = operator_runtime::ProjectSchemaOwner::create(
                *registration,
                documentSchemas,
                canonicalJsonValidator(),
                std::move(documentValidator)
            );
            if (!projectSchemaOwner.has_value())
            {
                return std::unexpected{projectSchemaOwner.error().clone()};
            }
            auto journalOwner = operator_runtime::ProjectJournalSchemaOwner::create(
                *registration,
                files.journalEventManifest,
                deployed->journalPayloadValidator()
            );
            if (!journalOwner.has_value())
            {
                return std::unexpected{journalOwner.error().clone()};
            }
            auto catalogOwner =
                operator_runtime::ProjectToolCatalogSchemaOwner::create(
                    *registration,
                    files.toolCatalog,
                    deployed->toolCatalogReader(),
                    deployed->toolArgumentValidator()
                );
            if (!catalogOwner.has_value())
            {
                return std::unexpected{catalogOwner.error().clone()};
            }
            auto reconcileOwner =
                operator_runtime::ProjectReconcileSchemaOwner::create(
                    *registration,
                    files.reconcileManifest,
                    deployed->reconcileDispositionReader()
                );
            if (!reconcileOwner.has_value())
            {
                return std::unexpected{reconcileOwner.error().clone()};
            }

            loaded.deployments.emplace_back(LoadedDeployment{
                .name                   = name,
                .registration           = *std::move(registration),
                .schemaOwner            = *std::move(projectSchemaOwner),
                .journalSchemaOwner     = *std::move(journalOwner),
                .toolCatalogSchemaOwner = *std::move(catalogOwner),
                .reconcileSchemaOwner   = *std::move(reconcileOwner),
                .catalog                = *std::move(deployed),
                .pluginBytes            = std::move(files.pluginBytes),
                .artifactBlobs          = std::move(blobs),
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
        return ProjectLoader::load(directory, expected, {});
    }

    auto loadConformanceProject(
        std::filesystem::path const& directory,
        std::span<ExpectedRegistration const> expected
    ) -> Result<ConformanceProject>
    {
        auto inputLog = std::make_shared<ProjectDocumentInputLog>();
        UF_TRY_VALUE(loaded, ProjectLoader::load(directory, expected, inputLog));

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
            compile("umbraflow-conformance/v1", k_conformanceSchema)
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
            .loaded           = std::move(loaded),
            .documentInputLog = std::move(inputLog),
            .probeFrame       = {frameBytes.begin(), frameBytes.end()},
            .underTest        = std::move(underTest),
            .foreign          = std::move(foreign),
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

#pragma once

#include <operator/effective-plan.hpp>
#include <operator/journal-entry.hpp>
#include <operator/project-plugin.hpp>
#include <operator/reconcile-outcome.hpp>
#include <operator/tool-invocation.hpp>

#include <core/error/result.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace uf::deployment
{
    // Exact RFC 8785 and nothing else, behind operator's
    // CanonicalJsonValidator signature. It is a free function because it holds
    // no schema authority and can hold none: canonical bytes are canonical bytes
    // whatever project minted them, which is the whole of what
    // ProjectSchemaOwner::canonicalize is allowed to prove.
    [[nodiscard]]
    auto canonicalJsonValidator() -> operator_runtime::CanonicalJsonValidator;

    // The two documents a ProjectPlugin returns that the Operator itself acts
    // on: OP:`PlanProposal`, and OP:`UIActionIntent` or OP:`WaitIntent`. They
    // are free functions for the reason canonicalJsonValidator is one: the
    // operator protocol is the Operator's own schema and is the same for every
    // project, so neither reader consults anything a ProjectRegistration
    // pinned.
    //
    // Each is a whole PlanProposalReader or StepIntentReader
    // (operator/effective-plan.hpp): it reads a document the schema owner that
    // minted it has already held to exact RFC 8785 and to the complete
    // definition, so it refuses only what the ValidatedDocument type cannot
    // state -- that the document was stamped as this function's output.
    [[nodiscard]]
    auto readPlanProposal(operator_runtime::ValidatedDocument const& proposal)
        -> Result<operator_runtime::PlanProposalClaims>;

    // The two intents carry no discriminator, so the schema told them apart by
    // their complete member sets under oneOf: a document satisfying both would
    // be a step of two kinds and was refused rather than read as either. What
    // is left here is reading back which branch matched.
    [[nodiscard]]
    auto readStepIntent(operator_runtime::ValidatedDocument const& intent)
        -> Result<operator_runtime::StepIntentClaims>;

    // The $id each project schema document must declare. The Operator-owned
    // envelope schemas this module carries reference the project's documents by
    // these identities, so a document that declares another one is refused when
    // the deployment is created rather than skipped when a document is judged.
    //
    // Two deployments may declare the same four identities, and the two a
    // project directory needs necessarily do. Each deployment's schemas are
    // compiled into a closed set of their own, so an identity is only ever
    // resolved among the documents of the deployment that declared it.
    inline constexpr auto k_projectStateSchemaId =
        std::string_view{"https://umbraflow.dev/schema/project/state"};
    inline constexpr auto k_projectObservationSchemaId =
        std::string_view{"https://umbraflow.dev/schema/project/observation"};
    inline constexpr auto k_toolPreconditionSchemaId =
        std::string_view{"https://umbraflow.dev/schema/project/tool-precondition"};
    inline constexpr auto k_reconcileSchemaId =
        std::string_view{"https://umbraflow.dev/schema/project/reconcile"};

    // Everything one deployment holds for one ProjectRegistration, as exact
    // bytes. Nothing here is a name, a path or a label: each member is the
    // document whose sha256 the registration pinned, or a document some other
    // member names by sha256.
    //
    // Views, not owned strings: create() parses every one of them and retains
    // nothing, so the bytes need only outlive the call.
    struct ProjectDeploymentSources final
    {
        // The registration's plugin id. Each of the three documents below
        // declares the plugin it belongs to, and a document declaring another
        // one is refused: a catalog that answered for whichever registration
        // presented it would be a catalog no registration owns.
        std::string_view pluginId{};

        // JSON Schema documents, each declaring the matching identity above.
        // The first three are the ProjectDocumentSchemaBytes the registration
        // pins directly; reconcile is named by the reconcile manifest.
        std::string_view projectState{};
        std::string_view projectObservation{};
        std::string_view toolPrecondition{};
        std::string_view reconcile{};

        // Documents this module reads rather than evaluates. Each is pinned by
        // a hash of the registration's own, and each names the schemas it
        // governs by sha256 -- so the chain from registration to schema bytes
        // has no link that is only a convention.
        std::string_view toolCatalog{};
        std::string_view journalEventManifest{};
        std::string_view reconcileManifest{};

        // One complete JSON Schema per namespaced event type the journal
        // manifest lists, in any order: each is matched to its manifest entry
        // by its own sha256, which is the payload_schema_hash the Operator
        // records beside every entry the schema accepted.
        std::span<std::string_view const> journalPayloadSchemas{};

        // One complete JSON Schema per OP:`ExpectedEffect` payload the project
        // can propose, matched to an effect by the payload_schema_hash the
        // effect itself carries, and an effect naming a hash no schema in this
        // set has is refused.
        //
        // No member of ProjectRegistrationClaims pins one directly. What puts
        // their bytes inside a registration is the Tool Catalog's
        // effect_payload_sha256s, which create() holds to this set both ways --
        // so editing a pinned effect payload schema moves tool_catalog_hash and
        // therefore project_registration_hash, rather than moving no hash at
        // all and surfacing as a Plan refused much later.
        std::span<std::string_view const> effectPayloadSchemas{};
    };

    // One document whose format is the framework's rather than the project's --
    // the tool catalog, the journal event schema manifest or the reconcile
    // payload schema manifest -- judged against the framework schema that
    // governs it. Which one it is comes from the document's own `schema`
    // member, so a caller states no choice and cannot state the wrong one.
    //
    // It is published for one reason. docs/archive/plans/2026-08-11-project-as-data.md
    // 2.4 specifies these three by worked example, and an example that nothing
    // holds to the bytes that decide drifts from them: the last time this
    // format was stated only as C++ string constants, the first consumer to
    // write the six documents guessed CamelCase for two wire words and was
    // wrong. tests/deployment extracts each example from that document and
    // requires this to accept it.
    [[nodiscard]]
    auto validateFrameworkFormat(std::string_view exactBytes) -> Status;

    // The Tool Catalog document's own word for a mutability. That document's
    // vocabulary belongs to this module -- the framework schema that judges the
    // catalog and the table that reads it are both here -- so a caller whose
    // refusal must name a mutability spells it through this rather than as a
    // third copy of the two words.
    [[nodiscard]]
    auto toolMutabilityWireName(operator_runtime::ToolMutability mutability) noexcept
        -> std::string_view;

    // The four schema-bearing validators one ProjectRegistration's authorities
    // are built from. Immutable and copyable: each accessor hands out a
    // std::function that keeps this state alive, so an authority outlives the
    // ProjectDeployment it was taken from.
    class ProjectDeployment final
    {
        class State;

        std::shared_ptr<State const> m_state;

        explicit ProjectDeployment(std::shared_ptr<State const> p_state) noexcept;

    public:
        ProjectDeployment(ProjectDeployment const&) noexcept = default;
        ProjectDeployment(ProjectDeployment&&) noexcept = default;
        auto operator=(ProjectDeployment const&) noexcept -> ProjectDeployment& = default;
        auto operator=(ProjectDeployment&&) noexcept -> ProjectDeployment& = default;
        ~ProjectDeployment() = default;

        // Compiles every schema and reads every manifest. It refuses a schema
        // this evaluator cannot apply, a manifest naming a schema this set does
        // not carry, and a catalog whose tool names an argument definition the
        // tool-precondition schema does not declare -- at startup, where a
        // deployment builds its authorities, rather than years later when a
        // document reaches the hole.
        [[nodiscard]]
        static auto create(ProjectDeploymentSources const& sources)
            -> Result<ProjectDeployment>;

        // What this deployment's Tool Catalog carries under one name, or
        // nothing at all.
        //
        // Every other reader reaches the catalog through toolCatalogValidator(),
        // which judges a call and therefore needs its arguments. A document
        // that names tools without calling them -- a conformance vocabulary --
        // has no arguments to offer, so this is what lets such a document and a
        // catalog be held to each other where both were written.
        [[nodiscard]]
        auto carriedTool(std::string_view name) const
            -> std::optional<operator_runtime::ToolDescriptor>;

        [[nodiscard]]
        auto documentValidator() const
            -> operator_runtime::ProjectDocumentValidator;

        [[nodiscard]]
        auto journalPayloadValidator() const
            -> operator_runtime::JournalPayloadSchemaValidator;

        [[nodiscard]]
        auto toolCatalogValidator() const
            -> operator_runtime::ToolCatalogValidator;

        [[nodiscard]]
        auto reconcileDispositionReader() const
            -> operator_runtime::ReconcileDispositionReader;
    };
}

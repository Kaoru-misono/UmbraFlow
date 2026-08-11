#pragma once

#include <operator/effective-plan.hpp>
#include <operator/journal-entry.hpp>
#include <operator/project-plugin.hpp>
#include <operator/reconcile-outcome.hpp>
#include <operator/tool-invocation.hpp>

#include <core/error/result.hpp>

#include <memory>
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
    // (operator/effective-plan.hpp:119-122): it refuses bytes that are not
    // exact RFC 8785, then refuses anything the complete definition does not
    // accept, before reading a member. A deployment whose ProjectSchemaOwner
    // already judged the same document judges it twice; that is the price of a
    // reader an OperatorPlanAuthority can be built from without one.
    [[nodiscard]]
    auto readPlanProposal(std::string_view exactProposalJcs)
        -> Result<operator_runtime::PlanProposalClaims>;

    // The two intents carry no discriminator, so the schema tells them apart by
    // their complete member sets under oneOf: a document satisfying both would
    // be a step of two kinds and is refused rather than read as either.
    [[nodiscard]]
    auto readStepIntent(std::string_view exactStepJcs)
        -> Result<operator_runtime::StepIntentClaims>;

    // The $id each project schema document must declare. The Operator-owned
    // envelope schemas this module carries reference the project's documents by
    // these identities, so a document that declares another one is refused when
    // the deployment is created rather than skipped when a document is judged.
    //
    // Two projects may declare the same three identities. Each project's
    // schemas are compiled into a closed set of their own, so the identity is
    // only ever resolved among that project's documents.
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
        // effect itself carries. There is no manifest for these and there
        // cannot be one: no member of ProjectRegistrationClaims pins an effect
        // payload schema, so the hash inside the document is the only pin, and
        // an effect naming a hash no schema in this set has is refused.
        std::span<std::string_view const> effectPayloadSchemas{};
    };

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

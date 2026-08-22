#pragma once

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace uf::operator_runtime
{
    // The generation of the ProjectRegistration document contract this
    // framework derives and reads. It is a compatibility statement: a
    // registration declares the number and this binary decides whether it
    // understands what that number describes. Its shape is
    // schema/umbraflow-project-registration-v3.schema.json.
    //
    // It is a generation rather than the digest of that schema, because the
    // digest made every cosmetic edit to the file move every registration
    // root -- and a registration root is a project's identity, pinned by
    // consumers. The digest also compared a value with itself: the loader
    // derived it once and handed the same local to both the document and the
    // schema owner that judged the document.
    //
    // There is ONE reader and it accepts exactly this number.
    // validateGenerationClaims refuses every other value rather than choosing
    // a behaviour from it, so the integer is an identity assertion inside the
    // reader and never a dispatch key. Code that inspected a document to pick
    // a reader would be the selector this constant exists to avoid.
    inline constexpr auto k_projectGenerationFormat = uint64{5U};

    // The one entry point the pure program type keeps under the two-closure
    // contract. `derive`, `plan`, `next_step` and `reconcile` are the
    // five-function contract's, and no closure of a generation exports them.
    inline constexpr auto k_reducerEntryPoint = std::string_view{"reduce"};

    // The namespace the Framework owns. Every Framework Tool is named inside
    // it, and no other registrant may claim it: a ProjectRegistration whose
    // plugin_id fell inside this namespace would own Tool names the Framework
    // catalog already owns, so validateSharedClaims refuses the claim where a
    // registrant's namespace is stated rather than once per Tool name.
    inline constexpr auto k_frameworkToolNamespace = std::string_view{"framework"};

    // The one spelling of a Tool name, wherever one is written: a namespaced
    // dotted name whose namespace is its owner's registered namespace and whose
    // local name is what follows the dot that ends it. A Tool Catalog `name`, a
    // ProjectRegistration `tool_name`, and every child Tool name a
    // ChildEffectDeclaration grants are this one type; a document admitting any
    // other spelling would declare a Tool no authoring tier could bind.
    [[nodiscard]]
    auto validateToolName(std::string_view name, std::string_view field) -> Status;

    // Whether this Tool name is one the registrant owning `ownedNamespace` may
    // declare. Ownership is the whole of the rule: Framework owns `framework`,
    // a Project owns its registered namespace -- its plugin_id -- and a name
    // outside the owner's namespace is somebody else's Tool. There is no
    // negative half beside this: a namespace a registrant does not own is
    // refused here whether it belongs to the Framework or to another Project.
    [[nodiscard]]
    auto validateToolNameOwnership(
        std::string_view name,
        std::string_view ownedNamespace
    ) -> Status;

    enum class ProjectResourceKind : uint8
    {
        Json,
        Utf8,
        Bytes,
    };

    struct ProjectResource final
    {
        ProjectResourceKind kind{ProjectResourceKind::Json};
        std::string         name{};
        ContentHash         hash;
        uint64              size{};
    };

    // One row of the Project Tool binding table: the catalog name of a Tool,
    // and the entry point of that project's closure that implements it.
    //
    // It is a registration member, beside the Tool Catalog rather than inside
    // it. The registration already splits identity into contract
    // (tool_catalog_hash) and code (plugin_environment_hash), and a binding is
    // the JOIN of the two -- it names a contract name and a code entry and
    // references both sides -- so it belongs beside both and inside neither.
    // Three things break if it moves into the catalog:
    //
    // 1. Exact-bytes verifiability dies. The catalog's exact bytes are hashed,
    //    and a caller may legitimately need existence and schema without being
    //    entitled to know what implements a Tool. With the binding inside,
    //    describing the catalog must either leak it or serve a redacted
    //    projection whose bytes no longer hash to tool_catalog_hash -- so the
    //    caller can no longer verify the served catalog against the pinned
    //    digest, which is that digest's entire job.
    // 2. Contract churn on refactor. Renaming a handler entry is a pure
    //    implementation change; with the binding in the catalog it would move
    //    tool_catalog_hash, invalidating every pin that cares only about the
    //    contract -- child admission intersects descriptors, and approvals and
    //    policy reference catalog authority. Implementation motion would force
    //    contract re-approval.
    // 3. Audience mismatch. Every catalog field is consumed by callers and by
    //    admission. A binding is consumed by exactly one party at one moment:
    //    the dispatcher, at dispatch.
    //
    // It is nonetheless INSIDE the registration root, and that is not
    // negotiable either. Replay compares execution identity field by field. If
    // the binding sat outside the root digest, a re-registration could rebind a
    // Tool to different code mid-history with no detectable identity change,
    // and "the exact pinned environment" would stop pinning which code answers
    // a call. Inside the root, registration refusal on digest inequality covers
    // rebinding for free, and a mid-run rebind surfaces as a named-field
    // mismatch that stops the run.
    struct ProjectToolBinding final
    {
        std::string toolName{};
        std::string entryPoint{};

        auto operator==(ProjectToolBinding const&) const -> bool = default;
    };

    class VerifiedProjectGeneration;

    // What a verified Project registration IS to everything downstream of the
    // document that stated it: the root it is named by, the namespace it owns,
    // the exact bytes that root digests, the code digest the ledger records as
    // its provenance, the schemas it pinned, and the Tools it bound.
    //
    // It is a projection and never a document. It carries no authority to mint
    // one and cannot be verified into existence: its only constructor takes an
    // already-verified generation, so a caller holding one of these has already
    // been through the reader. What it removes is the document: a consumer that
    // takes this names no document type at all, which is what let these
    // consumers survive the flip that deleted the one-closure document without
    // a branch of their own.
    //
    // It is deliberately not every claim a registration carries. The resource
    // closure, the environment digest and the journal and reconcile manifests
    // stay with the verified documents, because the seams that read them are
    // the loaders, which hold the document itself.
    class ProjectIdentity final
    {
        ContentHash                     m_projectRegistrationHash;
        std::string                     m_pluginId;
        std::string                     m_canonicalJcs;
        ContentHash                     m_moduleIdentityHash;
        ContentHash                     m_toolCatalogHash;
        ContentHash                     m_projectStateSchemaHash;
        ContentHash                     m_projectObservationSchemaHash;
        ContentHash                     m_projectToolPreconditionSchemaHash;
        ContentHash                     m_reconcilePayloadSchemaManifestHash;
        ContentHash                     m_journalEventSchemaManifestHash;
        std::string                     m_baselineEventType;
        std::vector<ContentHash>        m_observedInstanceIdentitySchemaHashes;
        std::vector<ProjectToolBinding> m_projectToolBindings;

    public:
        // The projection is implicit, because that is what it is: a narrowing
        // from a verified document to the part of it that outlives the
        // document. Naming the narrowing at every call site would say only
        // which loader the caller happens to hold, which is exactly the fact
        // these consumers must not depend on.
        ProjectIdentity(VerifiedProjectGeneration const& generation);

        [[nodiscard]] auto hash() const -> ContentHash;
        [[nodiscard]] auto pluginId() const -> std::string;

        [[nodiscard]]
        auto canonicalJcs() const noexcept UF_LIFETIME_BOUND
            -> std::string const&;

        // The digest of the module closure the ledger records this
        // registration's durable provenance under. It is the REDUCER closure's,
        // because the rows this digest guards -- the instance baseline, the
        // project state and the Journal prefix they fold -- are produced by the
        // fold and by nothing else. The tool closure is not lost by that
        // choice: it is pinned inside the same root, whose exact bytes the same
        // ledger row stores.
        [[nodiscard]] auto moduleIdentityHash() const -> ContentHash;

        [[nodiscard]] auto toolCatalogHash() const -> ContentHash;
        [[nodiscard]] auto projectStateSchemaHash() const -> ContentHash;
        [[nodiscard]] auto projectObservationSchemaHash() const -> ContentHash;
        [[nodiscard]] auto projectToolPreconditionSchemaHash() const -> ContentHash;
        [[nodiscard]] auto reconcilePayloadSchemaManifestHash() const -> ContentHash;
        [[nodiscard]] auto journalEventSchemaManifestHash() const -> ContentHash;
        [[nodiscard]] auto baselineEventType() const -> std::string;

        // The closed set of observed-instance identity schema documents this
        // registration owns, in the sorted-unique order the loader derived. It
        // is here rather than left with the verified documents because the
        // authority that judges an observed instance is built once per
        // deployment and must be keyed on the identity rather than on which
        // document generation stated it.
        [[nodiscard]]
        auto observedInstanceIdentitySchemaHashes() const noexcept UF_LIFETIME_BOUND
            -> std::vector<ContentHash> const&;

        [[nodiscard]]
        auto projectToolBindings() const noexcept UF_LIFETIME_BOUND
            -> std::vector<ProjectToolBinding> const&;
    };

    // One compiled closure of a two-closure registration generation: the exact
    // module closure its manifest digest was taken over, and what the authoring
    // path observed that closure to export.
    //
    // exportedEntryPoints is a STATEMENT and never a derivation. A loader may
    // not compute it from the binding table, because the load-time check would
    // then compare the table with itself; and it may not run the module to
    // observe it, because the bridge would then compare exports with exports.
    // Either collapse leaves a check no test can make fail and deletes the
    // property the member exists to pin.
    //
    // It names entry points only. `plugin_id` is a field of every closure and
    // is never listed here, so an empty vector is the whole statement that this
    // closure offers no entry point at all.
    struct ProjectClosureClaims final
    {
        ContentHash              moduleManifestHash;
        std::vector<std::string> exportedEntryPoints{};

        auto operator==(ProjectClosureClaims const&) const -> bool = default;
    };

    // Values extracted only after a validator has accepted the exact
    // two-closure ProjectRegistration JCS bytes. This is not a construction
    // spec: no caller mints a generation from one.
    struct ProjectGenerationClaims final
    {
        uint64      projectRegistrationFormat{};
        std::string pluginId{};

        // Both slots, always. There is no absent-means-pure reading and no
        // absent-means-scoped reading: a document carrying one closure is not a
        // generation, and a project that binds no Tool states an empty tool
        // closure rather than omitting one.
        ProjectClosureClaims reducerClosure;
        ProjectClosureClaims toolClosure;

        ContentHash pluginEnvironmentHash;
        ContentHash toolCatalogHash;
        ContentHash projectStateSchemaHash;
        ContentHash projectObservationSchemaHash;
        ContentHash projectToolPreconditionSchemaHash;
        ContentHash reconcilePayloadSchemaManifestHash;
        ContentHash journalEventSchemaManifestHash;

        std::string                  baselineEventType{};
        std::vector<ProjectResource> projectResources{};

        // Both sorted and unique by the derivation the loader performs before
        // it writes. The order is the framework's own reading rather than
        // something JSON Schema can state, so the claims check refuses any
        // other order here instead of leaving it to the document's shape.
        std::vector<ContentHash> observedInstanceIdentitySchemaHashes{};

        std::vector<ProjectToolBinding> projectToolBindings{};
    };

    // The implementation must parse the complete generation document, validate
    // it against the exact two-closure JSON Schema, and reject bytes that are
    // not the exact RFC 8785 JCS serialization. Returning claims without doing
    // all three is a validator bug, never an extension point for project code.
    using ProjectGenerationExactValidator = std::function<
        Result<ProjectGenerationClaims>(std::string_view exactJcs)
    >;

    // Authority-bearing identity of one two-closure registration generation.
    // Its constructor is unreachable except from ProjectGeneration::verifyExact
    // after exact JCS validation, exact schema validation, and root
    // verification have all succeeded.
    class VerifiedProjectGeneration final
    {
        ProjectGenerationClaims m_claims;
        std::string             m_canonicalJcs;
        ContentHash             m_rootHash;

        VerifiedProjectGeneration(
            ProjectGenerationClaims claims,
            std::string canonicalJcs,
            ContentHash rootHash
        );

        friend class ProjectGeneration;
        friend class ProjectIdentity;

    public:
        [[nodiscard]]
        auto canonicalJcs() const noexcept UF_LIFETIME_BOUND
            -> std::string const&;

        [[nodiscard]] auto hash() const -> ContentHash;
        [[nodiscard]] auto pluginId() const -> std::string;
        [[nodiscard]] auto pluginEnvironmentHash() const -> ContentHash;
        [[nodiscard]] auto toolCatalogHash() const -> ContentHash;

        // The two closures, each answered by its own accessor. There is
        // deliberately no accessor asking whether a generation has a tool
        // closure: one that could answer "no" would be the optional slot this
        // contract refuses.
        [[nodiscard]]
        auto reducerClosure() const noexcept UF_LIFETIME_BOUND
            -> ProjectClosureClaims const&;

        [[nodiscard]]
        auto toolClosure() const noexcept UF_LIFETIME_BOUND
            -> ProjectClosureClaims const&;

        // One resource closure, read by both compiled closures.
        [[nodiscard]]
        auto projectResources() const noexcept UF_LIFETIME_BOUND
            -> std::vector<ProjectResource> const&;

        [[nodiscard]]
        auto projectToolBindings() const noexcept UF_LIFETIME_BOUND
            -> std::vector<ProjectToolBinding> const&;
    };

    // The sole mint for VerifiedProjectGeneration, and the only reader of a
    // ProjectRegistration document there is. It accepts the two-closure shape
    // and refuses everything else, including the one-closure document the flip
    // deleted: nothing inspects a document to decide which reader should have
    // it, because there is no second reader to decide between.
    class ProjectGeneration final
    {
    public:
        [[nodiscard]]
        static auto verifyExact(
            std::string canonicalJcs,
            ContentHash expectedRootHash,
            ProjectGenerationExactValidator const& validate
        ) -> Result<VerifiedProjectGeneration>;
    };

    struct SessionManifestSpec final
    {
        ContentHash runtimeModelArtifactRootHash;
        ContentHash operatorProtocolSchemaHash;
        ContentHash projectRegistrationHash;
        ContentHash policyArtifactHash;
        ContentHash agentProfileHash;
    };

    class SessionManifest final
    {
        SessionManifestSpec m_spec;
        std::string         m_canonicalBytes;
        ContentHash         m_hash;

        SessionManifest(
            SessionManifestSpec spec,
            std::string canonicalBytes,
            ContentHash hash
        );

    public:
        [[nodiscard]]
        static auto create(
            SessionManifestSpec const& spec
        ) -> Result<SessionManifest>;

        [[nodiscard]] auto canonicalBytes() const -> std::string;
        [[nodiscard]] auto hash() const -> ContentHash;
        [[nodiscard]] auto projectRegistrationHash() const -> ContentHash;
        [[nodiscard]] auto runtimeModelArtifactRootHash() const -> ContentHash;

        // The operator protocol schema this session is pinned to. It is
        // exposed because OperatorPlanAuthority must satisfy it with exact
        // bytes: an authority that merely named a hash would be a convention.
        [[nodiscard]] auto operatorProtocolSchemaHash() const -> ContentHash;

        // The PolicyArtifact this session is pinned to, exposed for the same
        // reason: VerifiedPolicyArtifact::verifyExact must satisfy it with
        // exact bytes, so the rules a plan is judged under are the ones this
        // manifest attests to and not ones a caller handed in as a hash.
        [[nodiscard]] auto policyArtifactHash() const -> ContentHash;

        // The AgentProfile this session is pinned to. Exposed for the same
        // reason: AgentProfile::verifyExact must satisfy it with exact bytes,
        // so that the ceilings an Agent binding runs under are the ones this
        // manifest attests to and not ones a caller stated.
        [[nodiscard]] auto agentProfileHash() const -> ContentHash;

    };
}

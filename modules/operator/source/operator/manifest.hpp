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
    // understands what that number describes.
    //
    // It is a generation rather than the digest of
    // schema/umbraflow-project-registration-v2.schema.json, because the digest
    // made every cosmetic edit to that file move every registration root -- and
    // a registration root is a project's identity, pinned by consumers. The
    // digest also compared a value with itself: the loader derived it once and
    // handed the same local to both the document and the schema owner that
    // judged the document.
    inline constexpr auto k_projectRegistrationFormat = uint64{4U};

    // The generation of the TWO-CLOSURE ProjectRegistration document contract,
    // stated in the same `project_registration_format` member and read by a
    // different reader. Its shape is
    // schema/umbraflow-project-registration-v3.schema.json.
    //
    // Two constants over one member is what a dark generation looks like, and
    // it is safe for exactly one reason: no reader has both shapes behind it.
    // validateClaims below accepts 4 and refuses everything else;
    // validateGenerationClaims accepts 5 and refuses everything else. Neither
    // reads the number to choose a behaviour, so the integer is an identity
    // assertion and never a dispatch key. Code that inspected a document to
    // pick a reader would be the selector both of them exist to avoid.
    inline constexpr auto k_projectGenerationFormat = uint64{5U};

    // The one entry point the pure program type keeps under the two-closure
    // contract. `derive`, `plan`, `next_step` and `reconcile` are the
    // five-function contract's, and no closure of a generation exports them.
    inline constexpr auto k_reducerEntryPoint = std::string_view{"reduce"};

    // The namespace the Framework owns. Every Framework Tool is named inside
    // it, and no other registrant may claim it: a ProjectRegistration whose
    // plugin_id fell inside this namespace would own Tool names the Framework
    // catalog already owns, so validateClaims refuses the claim where a
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

    class VerifiedProjectRegistration;
    class VerifiedProjectGeneration;

    // What a verified Project registration IS to everything downstream of the
    // document that stated it: the root it is named by, the namespace it owns,
    // the exact bytes that root digests, the code digest the ledger records as
    // its provenance, the schemas it pinned, and the Tools it bound.
    //
    // It is a projection and never a document. It carries no authority to mint
    // one and cannot be verified into existence: the only two constructors take
    // an already-verified registration of one document generation or the other,
    // so a caller holding one of these has already been through a reader. What
    // it removes is the document generation. A consumer that takes this cannot
    // tell whether a one-closure registration or a two-closure generation stated
    // it, which is the point -- consumers keyed on this survive the flip that
    // deletes the older document type, and they need no branch to do it.
    //
    // Being generation-neutral is not the same as being a selector. Nothing
    // here inspects a value to decide what a registration is; each constructor
    // reads exactly one document type and knows which at compile time.
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
        std::string                     m_baselineEventType;
        std::vector<ProjectToolBinding> m_projectToolBindings;

    public:
        // Both projections are implicit, because that is what they are: a
        // narrowing from a verified document to the part of it that outlives
        // the document's generation. Naming the narrowing at every call site
        // would say only which document generation the caller happens to hold,
        // which is exactly the fact these consumers must not depend on.
        ProjectIdentity(VerifiedProjectRegistration const& registration);
        ProjectIdentity(VerifiedProjectGeneration const& generation);

        [[nodiscard]] auto hash() const -> ContentHash;
        [[nodiscard]] auto pluginId() const -> std::string;

        [[nodiscard]]
        auto canonicalJcs() const noexcept UF_LIFETIME_BOUND
            -> std::string const&;

        // The digest of the module closure the ledger records this
        // registration's durable provenance under. A one-closure registration
        // has one closure and states it; a two-closure generation projects its
        // REDUCER closure, because the rows this digest guards -- the instance
        // baseline, the project state and the Journal prefix they fold -- are
        // produced by the fold and by nothing else. The tool closure is not
        // lost by that choice: it is pinned inside the same root, whose exact
        // bytes the same ledger row stores.
        [[nodiscard]] auto moduleIdentityHash() const -> ContentHash;

        [[nodiscard]] auto toolCatalogHash() const -> ContentHash;
        [[nodiscard]] auto projectStateSchemaHash() const -> ContentHash;
        [[nodiscard]] auto projectObservationSchemaHash() const -> ContentHash;
        [[nodiscard]] auto projectToolPreconditionSchemaHash() const -> ContentHash;
        [[nodiscard]] auto baselineEventType() const -> std::string;

        [[nodiscard]]
        auto projectToolBindings() const noexcept UF_LIFETIME_BOUND
            -> std::vector<ProjectToolBinding> const&;
    };

    // Values extracted only after the schema owner has accepted the exact
    // ProjectRegistration JCS bytes. This is not a construction spec: callers
    // cannot pass it to the registrar or mint a registration from it.
    struct ProjectRegistrationClaims final
    {
        uint64                       projectRegistrationFormat{};
        std::string                  pluginId{};
        ContentHash                  pluginModuleManifestHash;
        ContentHash                  pluginEnvironmentHash;
        ContentHash                  toolCatalogHash;
        ContentHash                  projectStateSchemaHash;
        ContentHash                  projectObservationSchemaHash;
        ContentHash                  projectToolPreconditionSchemaHash;
        ContentHash                  reconcilePayloadSchemaManifestHash;
        ContentHash                  journalEventSchemaManifestHash;
        std::string                  baselineEventType{};
        std::vector<ProjectResource> projectResources{};

        // The closed set of observed-instance identity schema documents this
        // registration owns, as the sha256 of each exact document, sorted and
        // unique. The deployment that supplied the documents derives it; the
        // claims check refuses any other order.
        std::vector<ContentHash> observedInstanceIdentitySchemaHashes{};

        // The Project Tool binding table, sorted by tool name and unique. An
        // empty table is the whole statement "this registration binds no Tool
        // to an entry", which is what a project that ships no handler declares;
        // there is deliberately no second spelling of that and no absent form.
        std::vector<ProjectToolBinding> projectToolBindings{};
    };

    // The implementation must parse the complete registration, validate it
    // against the owner's exact JSON Schema, and reject bytes that are not the
    // exact RFC 8785 JCS serialization. Returning claims without doing all
    // three is a schema-owner bug, never an extension point for project code.
    using ProjectRegistrationExactValidator = std::function<
        Result<ProjectRegistrationClaims>(std::string_view exactJcs)
    >;

    class ProjectRegistrationSchemaOwner final
    {
        ProjectRegistrationExactValidator m_validate;

        explicit ProjectRegistrationSchemaOwner(
            ProjectRegistrationExactValidator validate
        );

        friend class ProjectRegistration;

        [[nodiscard]]
        auto validate(std::string_view exactJcs) const
            -> Result<ProjectRegistrationClaims>;

    public:
        [[nodiscard]]
        static auto create(
            ProjectRegistrationExactValidator validate
        ) -> Result<ProjectRegistrationSchemaOwner>;
    };

    // Authority-bearing registration identity. Its constructor is unreachable
    // except from ProjectRegistration::verifyExact after exact JCS validation,
    // exact schema validation, and root verification have all succeeded.
    class VerifiedProjectRegistration final
    {
        ProjectRegistrationClaims m_claims;
        std::string               m_canonicalJcs;
        ContentHash               m_rootHash;

        VerifiedProjectRegistration(
            ProjectRegistrationClaims claims,
            std::string canonicalJcs,
            ContentHash rootHash
        );

        friend class ProjectRegistration;
        friend class ProjectIdentity;

    public:
        [[nodiscard]]
        auto canonicalJcs() const noexcept UF_LIFETIME_BOUND
            -> std::string const&;

        [[nodiscard]] auto hash() const -> ContentHash;
        [[nodiscard]] auto pluginId() const -> std::string;
        [[nodiscard]] auto pluginModuleManifestHash() const -> ContentHash;
        [[nodiscard]] auto pluginEnvironmentHash() const -> ContentHash;
        [[nodiscard]] auto projectStateSchemaHash() const -> ContentHash;
        [[nodiscard]] auto toolCatalogHash() const -> ContentHash;

        // Each schema-owning authority is bound to the exact bytes the
        // registration names here, so that an owner cannot answer for a schema
        // this registration never pinned.
        [[nodiscard]] auto projectObservationSchemaHash() const -> ContentHash;
        [[nodiscard]] auto projectToolPreconditionSchemaHash() const -> ContentHash;
        [[nodiscard]] auto reconcilePayloadSchemaManifestHash() const -> ContentHash;
        [[nodiscard]] auto journalEventSchemaManifestHash() const -> ContentHash;

        [[nodiscard]] auto baselineEventType() const -> std::string;

        [[nodiscard]]
        auto projectResources() const noexcept UF_LIFETIME_BOUND
            -> std::vector<ProjectResource> const&;

        [[nodiscard]]
        auto observedInstanceIdentitySchemaHashes() const noexcept
            UF_LIFETIME_BOUND -> std::vector<ContentHash> const&;

        // The binding table this registration pinned. See ProjectToolBinding
        // for why these bytes are inside this root and outside the catalog.
        [[nodiscard]]
        auto projectToolBindings() const noexcept UF_LIFETIME_BOUND
            -> std::vector<ProjectToolBinding> const&;
    };

    // The sole mint for VerifiedProjectRegistration. There is deliberately no
    // loose field spec and no API that canonicalizes caller-provided fields.
    class ProjectRegistration final
    {
    public:
        [[nodiscard]]
        static auto verifyExact(
            std::string canonicalJcs,
            ContentHash expectedRootHash,
            ProjectRegistrationSchemaOwner const& schemaOwner
        ) -> Result<VerifiedProjectRegistration>;
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
    // two-closure ProjectRegistration JCS bytes. Like ProjectRegistrationClaims
    // it is not a construction spec: no caller mints a generation from one.
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
        // it writes, exactly as the one-closure document requires; see
        // ProjectRegistrationClaims for why the order is the framework's own
        // reading rather than something JSON Schema can state.
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

    // The sole mint for VerifiedProjectGeneration, and the reader that accepts
    // the two-closure shape and nothing else. It shares no code path with
    // ProjectRegistration::verifyExact and neither of them inspects a document
    // to decide which of the two should read it.
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

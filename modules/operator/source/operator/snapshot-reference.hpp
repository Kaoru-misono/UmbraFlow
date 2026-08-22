#pragma once

#include "project-plugin.hpp"

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uf::operator_runtime
{
    class SnapshotObservationAuthority;

    // The one argument member an observation-consuming Tool carries the
    // reference in. It is stated once, here, because three boundaries read it:
    // the Framework catalog validating the arguments, the seam joining the
    // presented bytes back to this run's authority, and the provider resolving
    // them. A second spelling would be a second place the member name can be
    // wrong, and only one of them would be the one the minted document uses.
    inline constexpr auto k_observationReferenceArgument =
        std::string_view{"observation_reference"};

    // Why one presented observation reference was refused.
    //
    // Every enumerator is one named attack from experiment E5 plus the two the
    // resolution boundary itself owns, and each is one comparison with one
    // diagnostic. A single lumped "invalid reference" verdict is deliberately
    // absent: the experiment attacks each channel separately, and a test that
    // cannot tell which channel closed proves nothing about the other seven.
    enum class ObservationRefusal : uint8
    {
        // These exact bytes are not a reference this Framework minted. It is
        // the refusal a caller-authored or caller-edited observation document
        // earns, because recognition is byte equality against the recorded
        // wire form and nothing else.
        Unminted,

        // One observation authority was already spent by one native input.
        AlreadyConsumed,

        Stale,
        ForeignTarget,
        ForeignRegistration,

        // The RuntimeArtifact the observation was read through is not the one
        // the consuming call stands on. It is separate from ForeignTarget
        // because the same controlled target can be re-bound to another
        // runtime model, and separate from ChangedGeneration because the Host
        // generation can move while the artifact does not.
        ForeignRuntimeArtifact,

        ChangedGeneration,

        // The consuming call is not issued from the position the observation
        // was issued from: another root, another parent, or no parent where the
        // observation had one. Per R4 a call arriving under a different parent
        // is a coordinate mismatch, and this is that mismatch seen from the
        // observation side.
        MissingParent,

        // The observation declares the named snapshot-local semantic target
        // more than once, so no single target can be resolved from it.
        DuplicateLocal,

        // The observation never declared the named snapshot-local semantic
        // target at all.
        UnknownLocalTarget,

        // The observation's evidence does not support the requested UI action.
        // A refusal here delivers nothing and therefore spends nothing, which
        // is what keeps a rejected action from publishing stronger observation
        // or Journal facts than its evidence proved.
        ActionRefused,
    };

    [[nodiscard]]
    auto observationRefusalWireName(ObservationRefusal refusal) noexcept
        -> std::string_view;

    // The sentence a refused consumption carries. It sits beside the wire name
    // rather than being built at each refusal site, so that one refusal has one
    // spelling wherever it is reported.
    [[nodiscard]]
    auto observationRefusalDiagnostic(ObservationRefusal refusal) noexcept
        -> std::string_view;

    // What `framework.screen.observe` binds one observation to. Section 6 of
    // the cycle SPI plan names six bindings, and all six are here because a
    // reference missing any one of them is a reference some later call can
    // present against a world it never observed.
    //
    // No in-class initializers for the hashes: ContentHash has no default
    // state, and per the observed-instance minting ruling a scope that cannot
    // be established is refused rather than guessed. An invented sentinel here
    // would be exactly that guess.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct SnapshotObservationSpec final
    {
        // The controlled target the frame was read from.
        std::string controlledTargetId{};

        ContentHash runtimeArtifactRootHash;
        ContentHash projectRegistrationHash;

        // The frame this observation is of. It is the observation's own
        // identity and is deliberately not restated by a consuming call: a
        // consumer that could name the frame could name a frame it never saw.
        ContentHash frameIdentityHash;

        uint64 hostGeneration{};

        // The issuing coordinate a consuming call must stand at. The parent is
        // optional because a root-context call has none, and the difference
        // between "no parent" and "some parent" is itself part of the
        // comparison.
        ContentHash                rootIdentity;
        std::optional<ContentHash> issuingParentIdentity{};

        // Expiry and budget state. The consumption budget is exactly one and is
        // deliberately not a counter here: single use is enforced by the
        // authority's spent set, so there is no second place a budget could be
        // read from and disagree.
        uint64 expiresAtUnixMillis{};

        // The snapshot-local semantic targets Project interpretation may name
        // against this observation. A repeated entry is accepted here and
        // refused at resolution, because the whole matrix is answered at the
        // one boundary a consumer actually crosses.
        std::vector<std::string> localSemanticTargets{};

        // The UI actions this observation's evidence supports.
        std::vector<std::string> authorizedUiActions{};
    };

    // What one consuming call presents. It carries the reference bytes rather
    // than a parsed reference, because the bytes are what the authority
    // recognises and a parsed shape would be the caller's reading of them.
    //
    // No in-class initializers for the hashes, for the reason
    // SnapshotObservationSpec states.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct SnapshotObservationConsumption final
    {
        std::string exactReferenceJcs{};

        std::string controlledTargetId{};
        ContentHash runtimeArtifactRootHash;
        ContentHash projectRegistrationHash;
        uint64      hostGeneration{};

        ContentHash                rootIdentity;
        std::optional<ContentHash> issuingParentIdentity{};

        std::string localSemanticTarget{};
        std::string uiAction{};

        uint64 presentedAtUnixMillis{};
    };

    // The opaque handle a Tool result carries. Per R2 a Tool result is a
    // reference and not a payload, and per R3 a handle stays plain JSON data:
    // the wire form is exact RFC 8785 JCS produced by this type, so replay
    // equality is byte equality of recorded JSON and a script can hold it,
    // return it, and record it without any host object crossing the boundary.
    //
    // It lives in modules/operator rather than modules/domain because every one
    // of its bindings is an authority fact -- a controlled target, a Project
    // registration, a Host generation, an issuing call coordinate, an expiry --
    // and refusing a presented reference is an Operator verdict. modules/domain
    // owns values that carry no authority and has no vocabulary to refuse one,
    // so a reference declared there would be a handle nothing in its own module
    // could judge.
    //
    // Only SnapshotObservationAuthority can mint one, which is what makes
    // "Project Tools consume the reference through Framework resolution"
    // structural rather than documented.
    class SnapshotObservationReference final
    {
        friend class SnapshotObservationAuthority;

        SnapshotObservationSpec m_spec;
        CanonicalJson           m_wire;

        SnapshotObservationReference(
            SnapshotObservationSpec spec,
            CanonicalJson wire
        );

    public:
        // sha256 of the exact wire bytes. Two references are the same reference
        // exactly when their bytes are.
        [[nodiscard]] auto identity() const -> ContentHash;

        [[nodiscard]]
        auto wire() const noexcept UF_LIFETIME_BOUND -> CanonicalJson const&;

        [[nodiscard]]
        auto spec() const noexcept UF_LIFETIME_BOUND
            -> SnapshotObservationSpec const&;
    };

    // What Framework resolution hands a native input provider. It is a class
    // with a private constructor rather than an aggregate so that a provider
    // taking one cannot be handed caller-copied observation JSON at all: the
    // only way to hold one is to have spent an authority the Framework minted.
    class ResolvedSnapshotObservation final
    {
        friend class SnapshotObservationAuthority;

        ContentHash m_referenceIdentity;
        ContentHash m_frameIdentityHash;
        std::string m_controlledTargetId;
        std::string m_localSemanticTarget;
        std::string m_uiAction;
        uint64      m_hostGeneration;

        ResolvedSnapshotObservation(
            ContentHash referenceIdentity,
            ContentHash frameIdentityHash,
            std::string controlledTargetId,
            std::string localSemanticTarget,
            std::string uiAction,
            uint64 hostGeneration
        );

    public:
        [[nodiscard]] auto referenceIdentity() const -> ContentHash;
        [[nodiscard]] auto frameIdentityHash() const -> ContentHash;

        [[nodiscard]]
        auto controlledTargetId() const noexcept UF_LIFETIME_BOUND
            -> std::string const&;

        [[nodiscard]]
        auto localSemanticTarget() const noexcept UF_LIFETIME_BOUND
            -> std::string const&;

        [[nodiscard]]
        auto uiAction() const noexcept UF_LIFETIME_BOUND -> std::string const&;

        [[nodiscard]] auto hostGeneration() const noexcept -> uint64;
    };

    // The Framework's observation-authority boundary: the one thing that mints
    // a reference, and the one thing that turns presented bytes back into
    // authority.
    //
    // It is run-scoped state. An observation authority is meaningful only
    // inside the run whose coordinates it names, so the owner of this object is
    // the run context. References are values and hold no pointer back into it,
    // so the lifetime contract binds the spent set rather than the handles: a
    // reference outliving its authority is inert, because nothing else can
    // resolve one.
    class SnapshotObservationAuthority final
    {
        std::vector<SnapshotObservationReference> m_minted{};

        // The identities one native input each has already spent. It is a
        // separate sequence from m_minted so that "was minted here" and "was
        // already spent" stay two questions with two answers; folding the flag
        // into the reference would make a copied reference carry a stale answer
        // to the second one.
        std::vector<ContentHash> m_spent{};

        // A non-owning observation of one minted reference, or nullptr. It
        // points into m_minted and stays valid until the next mint; every
        // caller here consumes it before returning.
        [[nodiscard]]
        auto findMinted(std::string_view exactReferenceJcs) const noexcept
            UF_LIFETIME_BOUND -> SnapshotObservationReference const*;

    public:
        [[nodiscard]]
        auto mint(SnapshotObservationSpec spec)
            -> Result<SnapshotObservationReference>;

        // The observation one call's canonical arguments present, turned back
        // into the reference this authority minted.
        //
        // This is the join between a reference a caller holds as DATA and the
        // authority that can spend it. An Agent, a person and a Luau automation
        // script all hold the same thing: the exact bytes an observation Tool
        // answered with, which they may copy, store and pass along like any
        // other JSON. None of them can hold an observation AUTHORITY, because
        // only a mint produces a SnapshotObservationReference and only this
        // object mints one. Recognition is byte equality against what was
        // minted here and nothing else, so a caller-authored or caller-edited
        // document is refused before a durable coordinate exists for it rather
        // than inside a provider that would then have to explain a row nobody
        // should have been able to open.
        //
        // std::nullopt is "this Tool consumes no observation", which is what a
        // call whose arguments name no reference states. Bytes that name one
        // this authority never minted are an error carrying the Unminted
        // verdict; asking is free and spends nothing.
        [[nodiscard]]
        auto presented(CanonicalJson const& canonicalArgs) const
            -> Result<std::optional<SnapshotObservationReference>>;

        // The complete refusal matrix, answered without spending anything.
        // std::nullopt is "this consumption would be admitted"; every other
        // answer names exactly which binding closed.
        [[nodiscard]]
        auto refuse(SnapshotObservationConsumption const& consumption) const
            -> std::optional<ObservationRefusal>;

        // The same matrix, plus the single spend. A refusal spends nothing, so
        // an action refused on its bounds leaves the authority available to the
        // call that is entitled to it.
        [[nodiscard]]
        auto resolve(SnapshotObservationConsumption const& consumption)
            -> Result<ResolvedSnapshotObservation>;
    };
}

#pragma once

#include <core/safety/annotations.hpp>

#include <domain/content-hash.hpp>
#include <domain/ids.hpp>

#include <string>

namespace uf::task
{
    class TaskHost;

    // One completed observation cycle, as the trusted Runtime resolved it.
    // Only the Host can mint one: C++ interprets no RuntimeModel field, and the
    // resolver runs behind the Host's private capability surface, so there is
    // no second producer to disagree with this one.
    //
    // It carries no frame, no native handle and no Receipt. It is what the
    // Operator may plan on, not what anything may act with; the exact-cycle
    // Receipt stays inside Host storage and dies with its cycle.
    class UiObservationSnapshot final
    {
        friend class TaskHost;

        std::string      m_observationId;
        GenerationId     m_generation;
        TargetGeneration m_targetGeneration;
        ContentHash      m_artifactRootHash;
        ContentHash      m_semanticHash;
        std::string      m_canonicalJcs;

        UiObservationSnapshot(
            std::string observationId,
            GenerationId generation,
            TargetGeneration targetGeneration,
            ContentHash artifactRootHash,
            ContentHash semanticHash,
            std::string canonicalJcs
        );

    public:
        UiObservationSnapshot(UiObservationSnapshot const&) = default;
        UiObservationSnapshot(UiObservationSnapshot&&) noexcept = default;
        auto operator=(UiObservationSnapshot const&)
            -> UiObservationSnapshot& = default;
        auto operator=(UiObservationSnapshot&&) noexcept
            -> UiObservationSnapshot& = default;
        ~UiObservationSnapshot() = default;

        // Per-capture identity, minted by the Host rather than read off the
        // resolution. It is diagnostic only, and deliberately not part of any
        // decision basis: two captures that resolved to the same state must not
        // force a caller to re-plan or re-approve.
        //
        // It is the Host's because the resolver names only a resolved state,
        // while every observation -- unknown and ambiguous included -- needs one
        // identity, and a second spelling for the failing kinds is exactly the
        // drift this value exists to prevent.
        [[nodiscard]]
        auto observationId() const noexcept UF_LIFETIME_BOUND
            -> std::string const&;

        [[nodiscard]] auto generation() const noexcept -> GenerationId;
        [[nodiscard]] auto targetGeneration() const noexcept -> TargetGeneration;

        [[nodiscard]]
        auto artifactRootHash() const noexcept UF_LIFETIME_BOUND
            -> ContentHash const&;

        [[nodiscard]]
        auto semanticHash() const noexcept UF_LIFETIME_BOUND -> ContentHash const&;

        // sha256 of canonicalJcs(). Named for the schema member it becomes, and
        // it forwards rather than storing a second truth.
        [[nodiscard]] auto stateResolutionHash() const -> ContentHash;

        // The exact RFC 8785 JCS StateResolution document, produced by the
        // trusted Luau resolver and opaque to every C++ reader. This, and never
        // observationId(), is what reaches the plugin.
        //
        // It carries the resolution's CONTENT and nothing that names the
        // occasion: the kind, the ordered surface stack of a resolved state, and
        // the failure reason of the others. The resolver's own state-resolution
        // id is a per-VM counter, so serializing it would put the number of
        // preceding resolutions inside every decision basis and make two
        // identical readings of one world disagree.
        [[nodiscard]]
        auto canonicalJcs() const noexcept UF_LIFETIME_BOUND
            -> std::string const&;
    };
}

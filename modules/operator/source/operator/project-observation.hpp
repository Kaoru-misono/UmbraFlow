#pragma once

#include "project-plugin.hpp"

#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <string>

namespace uf::operator_runtime
{
    class OperatorCoordinator;

    // The project's reading of one UI observation: the opaque payload
    // ProjectPlugin.derive returned, bound to the exact inputs it was derived
    // from. It is a state kind rather than a document because s01 gives it its
    // own revision line and exactly one writer, and the writer is derive.
    //
    // The three input members are separate values rather than one combined
    // hash, because a stale answer has to name the dimension that moved.
    //
    // There is exactly one friend. A second -- a test accessor, a builder --
    // would let a caller claim a reading was derived against a ProjectState
    // revision it was not, which is the whole property this type carries.
    class ProjectObservation final
    {
        friend class OperatorCoordinator;

        ContentHash       m_projectRegistrationHash;
        ContentHash       m_pluginHash;
        std::string       m_projectInstanceKey;
        ContentHash       m_stateResolutionHash;
        uint64            m_projectStateRevision;
        ContentHash       m_projectStateHash;
        uint64            m_revision;
        ValidatedDocument m_payload;

        ProjectObservation(
            ContentHash projectRegistrationHash,
            ContentHash pluginHash,
            std::string projectInstanceKey,
            ContentHash stateResolutionHash,
            uint64 projectStateRevision,
            ContentHash projectStateHash,
            uint64 revision,
            ValidatedDocument payload
        );

    public:
        ProjectObservation(ProjectObservation const&) = default;
        ProjectObservation(ProjectObservation&&) noexcept = default;
        auto operator=(ProjectObservation const&) -> ProjectObservation& = default;
        auto operator=(ProjectObservation&&) noexcept
            -> ProjectObservation& = default;
        ~ProjectObservation() = default;

        [[nodiscard]] auto projectRegistrationHash() const -> ContentHash;
        [[nodiscard]] auto pluginHash() const -> ContentHash;

        [[nodiscard]]
        auto projectInstanceKey() const noexcept UF_LIFETIME_BOUND
            -> std::string const&;

        [[nodiscard]] auto stateResolutionHash() const -> ContentHash;
        [[nodiscard]] auto projectStateRevision() const noexcept -> uint64;
        [[nodiscard]] auto projectStateHash() const -> ContentHash;

        [[nodiscard]] auto revision() const noexcept -> uint64;

        // project_observation_hash. Forwards to the payload's content hash;
        // there is no second digest to disagree with the bytes.
        [[nodiscard]] auto hash() const -> ContentHash;

        [[nodiscard]]
        auto payload() const noexcept UF_LIFETIME_BOUND
            -> ValidatedDocument const&;
    };
}

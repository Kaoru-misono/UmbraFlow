#include "project-observation.hpp"

#include <string>
#include <utility>

namespace uf::operator_runtime
{
    ProjectObservation::ProjectObservation(
        ContentHash projectRegistrationHash,
        ContentHash pluginHash,
        std::string projectInstanceKey,
        ContentHash stateResolutionHash,
        uint64 projectStateRevision,
        ContentHash projectStateHash,
        uint64 revision,
        ValidatedDocument payload
    )
        : m_projectRegistrationHash{projectRegistrationHash}
        , m_pluginHash{pluginHash}
        , m_projectInstanceKey{std::move(projectInstanceKey)}
        , m_stateResolutionHash{stateResolutionHash}
        , m_projectStateRevision{projectStateRevision}
        , m_projectStateHash{projectStateHash}
        , m_revision{revision}
        , m_payload{std::move(payload)}
    {
    }

    auto ProjectObservation::projectRegistrationHash() const -> ContentHash
    {
        return m_projectRegistrationHash;
    }

    auto ProjectObservation::pluginHash() const -> ContentHash
    {
        return m_pluginHash;
    }

    auto ProjectObservation::projectInstanceKey() const noexcept
        -> std::string const&
    {
        return m_projectInstanceKey;
    }

    auto ProjectObservation::stateResolutionHash() const -> ContentHash
    {
        return m_stateResolutionHash;
    }

    auto ProjectObservation::projectStateRevision() const noexcept -> uint64
    {
        return m_projectStateRevision;
    }

    auto ProjectObservation::projectStateHash() const -> ContentHash
    {
        return m_projectStateHash;
    }

    auto ProjectObservation::revision() const noexcept -> uint64
    {
        return m_revision;
    }

    auto ProjectObservation::hash() const -> ContentHash
    {
        return m_payload.contentHash();
    }

    auto ProjectObservation::payload() const noexcept -> ValidatedDocument const&
    {
        return m_payload;
    }
}

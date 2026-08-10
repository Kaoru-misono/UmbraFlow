#include "ui-observation.hpp"

#include <core/error/contracts.hpp>

#include <cstddef>
#include <span>
#include <utility>

namespace uf::task
{
    UiObservationSnapshot::UiObservationSnapshot(
        std::string observationId,
        GenerationId generation,
        TargetGeneration targetGeneration,
        ContentHash artifactRootHash,
        ContentHash semanticHash,
        std::string canonicalJcs
    )
        : m_observationId{std::move(observationId)}
        , m_generation{generation}
        , m_targetGeneration{targetGeneration}
        , m_artifactRootHash{artifactRootHash}
        , m_semanticHash{semanticHash}
        , m_canonicalJcs{std::move(canonicalJcs)}
    {
    }

    auto UiObservationSnapshot::observationId() const noexcept -> std::string const&
    {
        return m_observationId;
    }

    auto UiObservationSnapshot::generation() const noexcept -> GenerationId
    {
        return m_generation;
    }

    auto UiObservationSnapshot::targetGeneration() const noexcept -> TargetGeneration
    {
        return m_targetGeneration;
    }

    auto UiObservationSnapshot::artifactRootHash() const noexcept -> ContentHash const&
    {
        return m_artifactRootHash;
    }

    auto UiObservationSnapshot::semanticHash() const noexcept -> ContentHash const&
    {
        return m_semanticHash;
    }

    auto UiObservationSnapshot::stateResolutionHash() const -> ContentHash
    {
        auto const digest = sha256(
            std::as_bytes(std::span{m_canonicalJcs.data(), m_canonicalJcs.size()})
        );

        // sha256 refuses only content too large for its own length encoding, and
        // a canonical StateResolution document cannot reach 2^61 bytes.
        UF_CHECK(digest.has_value());
        return *digest;
    }

    auto UiObservationSnapshot::canonicalJcs() const noexcept -> std::string const&
    {
        return m_canonicalJcs;
    }
}

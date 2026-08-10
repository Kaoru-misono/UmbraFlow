#pragma once

#include "error.hpp"

#include <core/types/integer.hpp>
#include <core/types/strong-id.hpp>
#include <core/types/strong-value.hpp>

#include <compare>
#include <string>

namespace uf
{
    namespace detail
    {
        struct EngineRunIdTag;
        struct TaskRunIdTag;
        struct GenerationIdTag;
        struct CaptureSessionIdTag;
        struct FrameIdTag;
        struct StateIdTag;
        struct RecognitionIdTag;
        struct ActionIdTag;
        struct TargetGenerationTag;
    }

    using EngineRunId = StrongId<detail::EngineRunIdTag>;
    using TaskRunId = StrongId<detail::TaskRunIdTag>;
    // One loaded project instance of the script layer: every ticket, VM state,
    // and host resource a run holds belongs to exactly one generation, and
    // tearing the generation down releases all of them at once.
    using GenerationId = StrongId<detail::GenerationIdTag>;
    using CaptureSessionId = StrongId<detail::CaptureSessionIdTag>;
    using FrameId = StrongId<detail::FrameIdTag>;
    using StateId = StrongId<detail::StateIdTag>;
    using RecognitionId = StrongId<detail::RecognitionIdTag>;
    using ActionId = StrongId<detail::ActionIdTag>;

    class Label final
    {
    public:
        using ValueType = std::string;

    private:
        std::string m_value;

        explicit Label(std::string value) noexcept;

    public:
        auto operator<=>(Label const&) const = default;

        [[nodiscard]] static auto create(std::string value) -> Result<Label>;

        [[nodiscard]]
        auto value() const noexcept UF_LIFETIME_BOUND -> std::string const&;
    };

    [[nodiscard]]
    inline auto toString(Label const& label) -> std::string
    {
        return label.value();
    }

    class TargetGeneration final
    {
        using Storage = Generation<detail::TargetGenerationTag>;

        Storage m_generation;

        constexpr explicit TargetGeneration(Storage generation) noexcept
            : m_generation{generation}
        {
        }

    public:
        constexpr TargetGeneration() noexcept
            : m_generation{Storage::initial()}
        {
        }

        auto operator<=>(TargetGeneration const&) const -> std::strong_ordering = default;

        [[nodiscard]]
        static constexpr auto initial() noexcept -> TargetGeneration
        {
            return TargetGeneration{Storage::initial()};
        }

        [[nodiscard]]
        static constexpr auto fromValue(uint64 value) noexcept -> TargetGeneration
        {
            return TargetGeneration{Storage::fromValue(value)};
        }

        [[nodiscard]] constexpr auto value() const noexcept -> uint64
        {
            return m_generation.value();
        }

        [[nodiscard]] auto next() const -> Result<TargetGeneration>;
    };
}

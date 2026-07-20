#pragma once

#include "error.hpp"

#include <core/types/strong-id.hpp>
#include <core/types/strong-value.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace uf
{
    namespace detail
    {
        struct EngineRunIdTag;
        struct TaskRunIdTag;
        struct SessionIdTag;
        struct FrameIdTag;
        struct StateIdTag;
        struct RecognitionIdTag;
        struct ActionIdTag;
        struct TargetGenerationTag;
    }

    using EngineRunId = StrongId<detail::EngineRunIdTag>;
    using TaskRunId = StrongId<detail::TaskRunIdTag>;
    using SessionId = StrongId<detail::SessionIdTag>;
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
        auto value() const UF_LIFETIME_BOUND noexcept -> std::string const&;
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
        static constexpr auto fromValue(std::uint64_t value) noexcept -> TargetGeneration
        {
            return TargetGeneration{Storage::fromValue(value)};
        }

        [[nodiscard]] constexpr auto value() const noexcept -> std::uint64_t
        {
            return m_generation.value();
        }

        [[nodiscard]] auto next() const -> Result<TargetGeneration>;
    };

    struct TargetGenerationHash final
    {
        [[nodiscard]]
        auto operator()(TargetGeneration generation) const noexcept -> std::size_t
        {
            return std::hash<std::uint64_t>{}(generation.value());
        }
    };
}

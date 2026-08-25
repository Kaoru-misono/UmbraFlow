#pragma once

#include "strong-value.hpp"

#include <core/types/integer.hpp>

#include <concepts>
#include <limits>
#include <optional>

namespace uf
{
    template <typename Tag, std::unsigned_integral Representation = uint64>
    using StrongId = StrongValue<Tag, Representation>;

    // A monotonic incarnation number for a thing that exists.
    //
    // GENERATIONS COUNT FROM ONE. The first incarnation is 1 because it is an
    // incarnation: something was resolved, bound or installed, and that is the
    // event this number is counting. Zero is therefore not a generation at all,
    // and a caller that needs to say "there is no such thing yet" says it with
    // an absent `std::optional` rather than by reserving a value inside the
    // domain.
    //
    // It counted from zero until 2026-08-25, and the mismatch that exposed it is
    // worth keeping here: the Operator ledger records a snapshot against the
    // generation of the target it observed, under
    // `CHECK(target_generation > 0)` -- the same shape every identifying counter
    // in that schema carries. A freshly bound target therefore produced the one
    // generation the ledger could not record, so the FIRST observation of every
    // session failed and no later one did. Nothing caught it because a counter
    // starting at zero is only wrong where it meets a reader who knows zero
    // means absent, and the two had never met on one path before.
    template <typename Tag, std::unsigned_integral Representation = uint64>
    class Generation final
    {
        Representation m_value;

        constexpr explicit Generation(Representation value) noexcept
            : m_value{value}
        {
        }

    public:
        auto operator<=>(Generation const&) const = default;

        [[nodiscard]]
        static constexpr auto initial() noexcept -> Generation
        {
            return Generation{Representation{1}};
        }

        [[nodiscard]]
        static constexpr auto fromValue(Representation value) noexcept -> Generation
        {
            return Generation{value};
        }

        [[nodiscard]] constexpr auto value() const noexcept -> Representation { return m_value; }

        [[nodiscard]]
        constexpr auto next() const noexcept -> std::optional<Generation>
        {
            if (m_value == std::numeric_limits<Representation>::max())
            {
                return std::nullopt;
            }

            return Generation{static_cast<Representation>(m_value + Representation{1})};
        }
    };
}

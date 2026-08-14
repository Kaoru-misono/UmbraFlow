#pragma once

#include "annotations.hpp"

#include "core/error/contracts.hpp"

#include <array>
#include <cstddef>
#include <ranges>
#include <span>
#include <type_traits>

namespace uf
{
    namespace detail
    {
        template <typename Type>
        inline constexpr auto k_isSpan = false;

        template <typename Element, std::size_t Extent>
        inline constexpr auto k_isSpan<std::span<Element, Extent>> = true;

        // A span is excluded because it is ALREADY the form the range overload
        // converts to, so that overload has nothing to do for one.
        template <typename Range>
        concept CheckedAccessRange = (
            !k_isSpan<std::remove_cvref_t<Range>>
            && std::ranges::contiguous_range<Range>
            && std::ranges::sized_range<Range>
            && std::is_lvalue_reference_v<Range&&>
            && requires(Range&& values) { std::span{values}; }
        );

        static_assert(
            !CheckedAccessRange<std::span<int>&>,
            "the range overload must not apply to a span"
        );
        static_assert(
            CheckedAccessRange<std::array<int, 4>&>,
            "the range overload must still apply to an ordinary contiguous range"
        );

        // Both public overloads delegate here, and the range overload names THIS
        // in its trailing return type rather than naming `checkedAt` again.
        //
        // That is structural rather than stylistic. When the range overload's
        // return type said `decltype(checkedAt(std::span{values}, index))` and
        // the argument was already a span, the expression named the very
        // overload being declared: Clang substituted into it forever and stopped
        // at depth 1024 while parsing vision's frame analysis, which is why the
        // analysis preset had never produced a result anyone could read. MSVC's
        // overload resolution reached the span overload first, so the ordinary
        // build stayed green over it for as long as this header has existed.
        //
        // Constraining the concept away from spans is correct and is kept above,
        // but it leaves the recursion merely unreachable rather than unsayable,
        // and it did not in fact stop Clang. Naming a function that has exactly
        // one overload, taking a span, cannot resolve back here at all.
        template <typename Element, std::size_t Extent>
        [[nodiscard]]
        constexpr auto tryAtSpan(
            std::span<Element, Extent> values UF_LIFETIME_BOUND,
            std::size_t index
        ) noexcept -> Element*
        {
            if (index >= values.size())
            {
                return nullptr;
            }

            return &values[index];
        }

        template <typename Element, std::size_t Extent>
        [[nodiscard]]
        constexpr auto checkedAtSpan(
            std::span<Element, Extent> values UF_LIFETIME_BOUND,
            std::size_t index
        ) noexcept -> Element&
        {
            auto* const element = tryAtSpan(values, index);
            UF_CHECK(element != nullptr);
            return *element;
        }
    }

    template <typename Element, std::size_t Extent>
    [[nodiscard]]
    constexpr auto tryAt(
        std::span<Element, Extent> values UF_LIFETIME_BOUND,
        std::size_t index
    ) noexcept -> Element*
    {
        return detail::tryAtSpan(values, index);
    }

    template <detail::CheckedAccessRange Range>
    [[nodiscard]]
    constexpr auto tryAt(
        Range&& values UF_LIFETIME_BOUND,
        std::size_t index
    ) noexcept(noexcept(detail::tryAtSpan(std::span{values}, index)))
        -> decltype(detail::tryAtSpan(std::span{values}, index))
    {
        return detail::tryAtSpan(std::span{values}, index);
    }

    template <typename Element, std::size_t Extent>
    [[nodiscard]]
    constexpr auto checkedAt(
        std::span<Element, Extent> values UF_LIFETIME_BOUND,
        std::size_t index
    ) noexcept -> Element&
    {
        return detail::checkedAtSpan(values, index);
    }

    template <detail::CheckedAccessRange Range>
    [[nodiscard]]
    constexpr auto checkedAt(
        Range&& values UF_LIFETIME_BOUND,
        std::size_t index
    ) noexcept(noexcept(detail::checkedAtSpan(std::span{values}, index)))
        -> decltype(detail::checkedAtSpan(std::span{values}, index))
    {
        return detail::checkedAtSpan(std::span{values}, index);
    }
}

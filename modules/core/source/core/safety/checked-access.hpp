#pragma once

#include "annotations.hpp"

#include "core/error/contracts.hpp"

#include <cstddef>
#include <ranges>
#include <span>
#include <type_traits>

namespace uf
{
    namespace detail
    {
        template <typename Range>
        concept CheckedAccessRange = (
            std::ranges::contiguous_range<Range>
            && std::ranges::sized_range<Range>
            && std::is_lvalue_reference_v<Range&&>
            && requires(Range&& values) { std::span{values}; }
        );
    }

    template <typename Element, std::size_t Extent>
    [[nodiscard]]
    constexpr auto tryAt(
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

    template <detail::CheckedAccessRange Range>
    [[nodiscard]]
    constexpr auto tryAt(
        Range&& values UF_LIFETIME_BOUND,
        std::size_t index
    ) noexcept(noexcept(tryAt(std::span{values}, index))) -> decltype(tryAt(std::span{values}, index))
    {
        return tryAt(std::span{values}, index);
    }

    template <typename Element, std::size_t Extent>
    [[nodiscard]]
    constexpr auto checkedAt(
        std::span<Element, Extent> values UF_LIFETIME_BOUND,
        std::size_t index
    ) noexcept -> Element&
    {
        auto* const element = tryAt(values, index);
        UF_CHECK(element != nullptr);
        return *element;
    }

    template <detail::CheckedAccessRange Range>
    [[nodiscard]]
    constexpr auto checkedAt(
        Range&& values UF_LIFETIME_BOUND,
        std::size_t index
    ) noexcept(noexcept(checkedAt(std::span{values}, index))) -> decltype(checkedAt(std::span{values}, index))
    {
        return checkedAt(std::span{values}, index);
    }
}

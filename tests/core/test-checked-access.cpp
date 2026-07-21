#include <core/safety/checked-access.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace uf
{
    namespace
    {
        template <typename Range>
        concept SupportsTryAt = requires(Range&& values)
        {
            tryAt(std::forward<Range>(values), std::size_t{0});
        };
    }

    static_assert(SupportsTryAt<std::vector<int>&>);
    static_assert(SupportsTryAt<std::vector<int> const&>);
    static_assert(!SupportsTryAt<std::vector<int>>);

    TEST_CASE("checked access rejects invalid indices without accepting temporary owners")
    {
        auto values = std::vector<int>{4, 8, 15};

        auto* const p_value = tryAt(values, std::size_t{1});
        REQUIRE(p_value != nullptr);
        *p_value = 9;

        CHECK(checkedAt(values, std::size_t{1}) == 9);
        CHECK(tryAt(values, values.size()) == nullptr);

        auto const& constValues = values;
        static_assert(std::is_same_v<
            decltype(tryAt(constValues, std::size_t{0})),
            int const*
        >);
        CHECK(checkedAt(std::span{constValues}, std::size_t{2}) == 15);
    }
}

#include <core/types/strong-id.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace
{
    struct ProjectTag;
    struct ProjectNameTag;
    struct TaskTag;

    using ProjectId = uf::StrongId<ProjectTag>;
    using ProjectName = uf::StrongValue<ProjectNameTag, std::string>;
    using TaskId = uf::StrongId<TaskTag>;
}

static_assert(!std::is_default_constructible_v<ProjectId>);
static_assert(!std::is_convertible_v<std::uint64_t, ProjectId>);
static_assert(!std::is_convertible_v<ProjectId, std::uint64_t>);
static_assert(!std::is_same_v<ProjectId, TaskId>);
static_assert(
    std::is_same_v<
        decltype(std::declval<ProjectId const&>().value()),
        std::uint64_t
    >
);
static_assert(
    std::is_same_v<
        decltype(std::declval<ProjectName const&>().value()),
        std::string const&
    >
);

TEST_CASE("strong identifiers do not mix domains")
{
    auto const first = ProjectId{std::uint64_t{7}};
    auto const second = ProjectId{std::uint64_t{8}};

    CHECK(first < second);

    auto names = std::unordered_map<
        ProjectId,
        char const*,
        uf::StrongValueHash<ProjectId>
    >{};
    names.emplace(first, "template");
    CHECK(std::string_view{names.at(first)} == "template");
}

TEST_CASE("generation overflow is explicit")
{
    using Generation = uf::Generation<ProjectTag, std::uint8_t>;

    auto const initial = Generation::initial();
    auto const next = initial.next();
    if (!next.has_value())
    {
        FAIL("The initial generation did not have a successor");
        return;
    }
    CHECK(next->value() == std::uint8_t{1});

    auto const exhausted = Generation::fromValue(std::numeric_limits<std::uint8_t>::max());
    CHECK_FALSE(exhausted.next().has_value());
}

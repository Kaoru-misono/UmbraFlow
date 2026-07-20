#include <core/types/strong-id.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>
#include <unordered_map>

namespace
{
    struct ProjectTag;
    struct TaskTag;

    using ProjectId = umbra_flow::StrongId<ProjectTag>;
    using TaskId = umbra_flow::StrongId<TaskTag>;
}

static_assert(!std::is_default_constructible_v<ProjectId>);
static_assert(!std::is_convertible_v<std::uint64_t, ProjectId>);
static_assert(!std::is_convertible_v<ProjectId, std::uint64_t>);
static_assert(!std::is_same_v<ProjectId, TaskId>);

TEST_CASE("strong identifiers do not mix domains")
{
    auto const first = ProjectId{std::uint64_t{7}};
    auto const second = ProjectId{std::uint64_t{8}};

    CHECK(first < second);

    auto names = std::unordered_map<
        ProjectId,
        char const*,
        umbra_flow::StrongValueHash<ProjectId>
    >{};
    names.emplace(first, "template");
    CHECK(std::string_view{names.at(first)} == "template");
}

TEST_CASE("generation overflow is explicit")
{
    using Generation = umbra_flow::Generation<ProjectTag, std::uint8_t>;

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

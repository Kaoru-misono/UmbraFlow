#include <core/types/integer.hpp>
#include <core/types/strong-id.hpp>

#include <doctest/doctest.h>

#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace uf
{
    namespace
    {
        struct ProjectTag;
        struct ProjectNameTag;
        struct TaskTag;

        using ProjectId = StrongId<ProjectTag>;
        using ProjectName = StrongValue<ProjectNameTag, std::string>;
        using TaskId = StrongId<TaskTag>;
    }

    static_assert(!std::is_default_constructible_v<ProjectId>);
    static_assert(!std::is_convertible_v<uint64, ProjectId>);
    static_assert(!std::is_convertible_v<ProjectId, uint64>);
    static_assert(!std::is_same_v<ProjectId, TaskId>);
    static_assert(
        std::is_same_v<
            decltype(std::declval<ProjectId const&>().value()),
            uint64
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
        auto const first = ProjectId{uint64{7}};
        auto const second = ProjectId{uint64{8}};

        CHECK(first < second);

        auto names = std::unordered_map<
            ProjectId,
            char const*,
            StrongValueHash<ProjectId>
        >{};
        names.emplace(first, "template");
        CHECK(std::string_view{names.at(first)} == "template");
    }

    TEST_CASE("generation overflow is explicit")
    {
        using Generation = Generation<ProjectTag, uint8>;

        auto const initial = Generation::initial();

        // The first incarnation is 1, not 0. Zero is not a generation: the
        // Operator ledger records an identifying counter under CHECK(> 0), and
        // a first generation of zero is the one value it cannot store. Absence
        // is spelled with an absent optional, never with a value inside the
        // domain.
        CHECK_MESSAGE(
            initial.value() == uint8{1},
            "a generation counts incarnations, so the first one is 1"
        );

        auto const next = initial.next();
        if (!next.has_value())
        {
            FAIL("The initial generation did not have a successor");
            return;
        }
        CHECK(next->value() == uint8{2});

        auto const exhausted = Generation::fromValue(std::numeric_limits<uint8>::max());
        CHECK_FALSE(exhausted.next().has_value());
    }
}

#include <core/types/enum-reflection.hpp>
#include <core/types/integer.hpp>
#include <core/utility/scope-exit.hpp>
#include <core/utility/variant-match.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <variant>

namespace uf
{
    namespace test_types
    {
        enum class BuildState : uint8
        {
            Idle = 1,
            Running = 4,
            Failed = 9
        };
    }
}

UF_REFLECT_ENUM(
    uf::test_types::BuildState,
    uf::test_types::BuildState::Idle,
    uf::test_types::BuildState::Running,
    uf::test_types::BuildState::Failed
);

static_assert(uf::ReflectedEnum<uf::test_types::BuildState>);

namespace uf
{
    TEST_CASE("variant matching requires one explicit handler per state")
    {
        auto state = std::variant<int, std::string>{std::string{"ready"}};
        auto const size = matchVariant(
            state,
            [](int) -> std::size_t { return 0; },
            [](std::string const& value) -> std::size_t { return value.size(); }
        );

        CHECK(size == 5);
    }

    TEST_CASE("enum reflection round-trips sparse values and rejects unknown names")
    {
        auto const name = enumName(test_types::BuildState::Running);
        if (!name.has_value())
        {
            FAIL("The reflected enum value did not have a name");
            return;
        }
        CHECK(*name == "Running");

        auto const value = enumFromName<test_types::BuildState>("Failed");
        if (!value.has_value())
        {
            FAIL("A known enum name did not produce a value");
            return;
        }
        CHECK(*value == test_types::BuildState::Failed);

        CHECK_FALSE(enumName(static_cast<test_types::BuildState>(2)).has_value());
        CHECK_FALSE(enumFromName<test_types::BuildState>("running").has_value());
        CHECK(enumEntries<test_types::BuildState>().size() == 3);
    }

    TEST_CASE("scope exit executes exactly while it remains armed")
    {
        auto executions = std::make_shared<std::atomic<int>>(0);
        {
            [[maybe_unused]] auto cleanup = scopeExit(
                [executions]() noexcept -> void
                {
                    executions->fetch_add(1, std::memory_order_relaxed);
                }
            );
        }

        {
            auto cleanup = scopeExit(
                [executions]() noexcept -> void
                {
                    executions->fetch_add(1, std::memory_order_relaxed);
                }
            );
            cleanup.release();
        }

        CHECK(executions->load(std::memory_order_relaxed) == 1);
    }
}

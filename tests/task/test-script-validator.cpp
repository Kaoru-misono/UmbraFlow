#include <task/framework-bundle.hpp>
#include <task/page-model-file.hpp>

#include <doctest/doctest.h>

#include <type_traits>

namespace uf::task
{
    TEST_CASE("C++ exposes no RuntimeModel semantic validator")
    {
        CHECK(frameworkProjectGlobals().empty());
        CHECK_FALSE(std::is_default_constructible_v<RuntimeModelBinding>);
        CHECK_FALSE(std::is_aggregate_v<RuntimeModelBinding>);
    }
}

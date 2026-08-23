#include "project-schemas.hpp"

#include <project/project-kit.hpp>

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/safety/checked-access.hpp>

#include <domain/error.hpp>

#include <array>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace uf::operator_runtime::conformance::expedition
{
    namespace
    {
        [[nodiscard]]
        auto requireGeneratedClosure(
            std::filesystem::path const& buildDirectory,
            std::span<std::string_view const> expected
        ) -> Status
        {
            for (auto const relative : expected)
            {
                auto error = std::error_code{};
                auto const path = buildDirectory / relative;
                if (!std::filesystem::is_regular_file(path, error))
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "example generation omitted manifest-declared closure file "
                            + path.string()
                    );
                }
            }
            return ok();
        }

        [[nodiscard]]
        auto generateExample(
            std::filesystem::path const& sourceDirectory,
            std::filesystem::path const& buildDirectory
        ) -> Status
        {
            auto const spec = project::ProjectBuildSpec{
                .sourceDirectory = sourceDirectory,
                .buildDirectory  = buildDirectory,
            };
            UF_TRY(project::initProject(spec));
            UF_TRY(project::buildProject(spec, {}));
            UF_TRY(project::checkProject(spec, {}));
            constexpr auto expected = std::array{
                std::string_view{"generated/modules/expedition/tool/main.luau"},
                std::string_view{"generated/modules/rival/tool/main.luau"},
                std::string_view{"generated/resources/expedition/map.blob"},
                std::string_view{"generated/resources/rival/map.blob"},
                std::string_view{"generated/registrations/expedition.json"},
                std::string_view{"generated/registrations/rival.json"},
            };
            return requireGeneratedClosure(buildDirectory, expected);
        }
    }
}

namespace
{
    // Reporting must not itself become the reason the process dies: std::cerr's
    // inserters are not noexcept, and a throw out of a catch handler in main
    // leaves nowhere to report it. The exit code carries the outcome alone.
    [[nodiscard]]
    auto report(std::string_view message) noexcept -> int
    {
        try
        {
            std::cerr << message << '\n';
        }
        catch (...)
        {
        }
        return 1;
    }
}

auto main(int argumentCount, char const* const* p_arguments) -> int
{
    try
    {
        if (argumentCount != 3)
        {
            return report("usage: generate-arcana-example SOURCE BUILD");
        }
        // SAFETY: a hosted entry point receives argumentCount argument pointers
        // followed by a null one ([basic.start.main]/2). That count arrives
        // beside the pointer rather than within it, so no expression can
        // restate the bound; this is the single place the C contract becomes a
        // span.
        UF_UNSAFE_BUFFER_BEGIN
        auto const arguments = std::span<char const* const>{
            p_arguments,
            static_cast<std::size_t>(argumentCount)
        };
        UF_UNSAFE_BUFFER_END

        auto const generated =
            uf::operator_runtime::conformance::expedition::generateExample(
                std::filesystem::path{uf::checkedAt(arguments, 1U)},
                std::filesystem::path{uf::checkedAt(arguments, 2U)}
            );
        if (!generated.has_value())
        {
            return report(generated.error().message());
        }
        return 0;
    }
    catch (std::exception const& error)
    {
        return report(error.what());
    }
    catch (...)
    {
        return report("unknown failure");
    }
}

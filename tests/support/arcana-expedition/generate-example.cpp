#include "project-schemas.hpp"

#include <project/project-kit.hpp>

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/safety/checked-access.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <array>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace uf::operator_runtime::conformance::expedition
{
    namespace
    {
        [[nodiscard]]
        auto requiredFileHash(
            std::filesystem::path const& path
        ) -> Result<ContentHash>
        {
            auto stream = std::ifstream{path, std::ios::binary};
            if (!stream.is_open())
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    "cannot open example schema " + path.string()
                );
            }
            auto const bytes = std::string{
                std::istreambuf_iterator<char>{stream},
                std::istreambuf_iterator<char>{},
            };
            if (stream.bad())
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    "cannot read example schema " + path.string()
                );
            }
            return sha256(std::as_bytes(std::span{bytes}));
        }

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
            UF_TRY_VALUE(
                toolPreconditionHash,
                requiredFileHash(
                    sourceDirectory / "schema/tool-precondition-v1.schema.json"
                )
            );
            UF_TRY_VALUE(
                effectPayloadHash,
                requiredFileHash(
                    sourceDirectory
                        / "schema/effect/expedition.march-v1.schema.json"
                )
            );
            UF_TRY(project::initProject(project::ProjectInitSpec{
                .sourceDirectory = sourceDirectory,
                .buildDirectory  = buildDirectory,
                .inputs          = {"umbraflow-project.json"},
            }));
            auto const spec = project::ProjectBuildSpec{
                .sourceDirectory = sourceDirectory,
                .buildDirectory  = buildDirectory,
                .toolCatalogs = {
                    exampleToolCatalogDeclaration(
                        "arcana.expedition",
                        toolPreconditionHash,
                        effectPayloadHash
                    ),
                    exampleToolCatalogDeclaration(
                        "arcana.rival",
                        toolPreconditionHash,
                        effectPayloadHash
                    ),
                },
            };
            UF_TRY(project::buildProject(spec, {}));
            UF_TRY(project::checkProject(spec, {}));
            constexpr auto expected = std::array{
                std::string_view{"generated/modules/expedition/main.luau"},
                std::string_view{"generated/modules/expedition/plans.luau"},
                std::string_view{"generated/modules/expedition/state/reducer.luau"},
                std::string_view{"generated/modules/rival/main.luau"},
                std::string_view{"generated/modules/rival/plans.luau"},
                std::string_view{"generated/modules/rival/state/reducer.luau"},
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

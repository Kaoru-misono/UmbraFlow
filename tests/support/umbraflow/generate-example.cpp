#include "project-schemas.hpp"

#include <project/project-kit.hpp>

#include <core/error/result.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>

namespace uf::operator_runtime::test_support
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
                    sourceDirectory / "schema/effect/fixture.write-v1.schema.json"
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
                        "fixture.alpha",
                        toolPreconditionHash,
                        effectPayloadHash
                    ),
                    exampleToolCatalogDeclaration(
                        "fixture.foreign",
                        toolPreconditionHash,
                        effectPayloadHash
                    ),
                },
            };
            UF_TRY(project::buildProject(spec));
            return project::checkProject(spec);
        }
    }
}

auto main(int argc, char** argv) -> int
{
    if (argc != 3)
    {
        std::cerr << "usage: generate-umbraflow-example SOURCE BUILD\n";
        return 1;
    }
    auto const generated = uf::operator_runtime::test_support::generateExample(
        std::filesystem::path{argv[1]},
        std::filesystem::path{argv[2]}
    );
    if (!generated.has_value())
    {
        std::cerr << generated.error().message() << '\n';
        return 1;
    }
    return 0;
}

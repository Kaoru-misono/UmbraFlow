#include "suite-support.hpp"

#include <operator/project-plugin.hpp>

#include <script/pure-data-program.hpp>

#include <doctest/doctest.h>

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace uf::operator_runtime::conformance
{
    TEST_CASE("registration and VM admission share one artifact byte ceiling")
    {
        auto const oversized =
            '"' + std::string(script::PureDataProgram::k_maximumResourceBytes, 'x') + '"';

        constexpr auto entryPoints = std::array<std::string_view, 1U>{"probe"};
        auto const vmAdmission = script::PureDataProgram::compile(
            "artifact-limit-probe",
            "main",
            {
                script::PureDataProgram::Module{
                    .name   = "main",
                    .source = "return { probe = function(input) return input end }",
                },
            },
            entryPoints,
            {
                script::PureDataProgram::Resource{
                    .kind  = script::PureDataProgram::ResourceKind::Json,
                    .name  = "oversized",
                    .bytes = oversized,
                },
            }
        );
        REQUIRE_FALSE(vmAdmission.has_value());
        CHECK_MESSAGE(
            vmAdmission.error().message()
                == std::string_view{"pure data resource exceeds its fixed byte ceiling"},
            "VM admission must use PureDataProgram::k_maximumResourceBytes"
        );

        auto const project    = loadedProject();
        auto const& underTest = deploymentFor(project, ProjectRole::UnderTest);
        auto blobs            = underTest.projectResources;
        blobs.emplace_back(
            ProjectPluginRegistrar::ResourceBlob{
                .kind  = ProjectResourceKind::Bytes,
                .name  = "oversized",
                .bytes = oversized,
            }
        );

        auto registrar = ProjectPluginRegistrar{};
        auto const registration = registrar.registerPlugin(
            underTest.registration,
            underTest.pluginEntryModule,
            underTest.pluginModules,
            std::move(blobs),
            underTest.schemaOwner
        );
        REQUIRE_FALSE(registration.has_value());
        CHECK_MESSAGE(
            registration.error().message() == vmAdmission.error().message(),
            "registration and VM admission must share the same resource boundary"
        );
    }
}

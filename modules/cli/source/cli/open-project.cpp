#include "open-project.hpp"

#include "cli-result.hpp"

#include <deployment/project-directory.hpp>

#include <operator/project-plugin.hpp>

#include <task/platform/confined-file.hpp>
#include <task/runtime-model-file.hpp>

#include <core/error/result.hpp>

#include <domain/content-hash.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::cli
{
    namespace
    {
        // Why the Operator's registrar refused this deployment, or nothing at
        // all when the registry holds its plugin afterwards.
        //
        // The registrar is mutable because registering into it is the whole
        // operation: an exact registration is startup-only state the caller
        // owns, and there is no value form of "the registry now holds this".
        //
        // Nothing here reads the registry back. registerPlugin returns the map
        // entry it just inserted, so a findExact over the identity that handle
        // reports is a comparison whose two sides are one value -- the shape
        // docs/pitfalls/checks-that-cannot-fail.md catalogues.
        [[nodiscard]]
        auto refusalRegistering(
            operator_runtime::ProjectPluginRegistrar& registrar,
            deployment::LoadedDeployment const& loaded
        ) -> std::optional<std::string>
        {
            auto const registered = registrar.registerPlugin(
                loaded.registration,
                loaded.pluginBytes,
                loaded.artifactBlobs,
                loaded.schemaOwner
            );
            if (!registered)
            {
                return formatError(registered.error());
            }
            return std::nullopt;
        }

        // The RuntimeArtifact the project names, opened the way the installer
        // opens it: both schema digests against this binary's pins, page_model
        // at its fixed name and non-empty, the directory's file closure, and
        // every declared size and sha256.
        //
        // The expected root hash is the digest of the manifest read here,
        // because no project document states a prior commitment to it. Reading
        // those bytes twice is safe in the only direction that matters: a
        // manifest swapped between the two reads mismatches and is refused, and
        // no swap can turn a refusal into an acceptance.
        [[nodiscard]]
        auto verifiedArtifact(
            std::filesystem::path const& artifactRoot
        ) -> Result<OpenedArtifact>
        {
            UF_TRY_VALUE(root, task_platform::ConfinedRoot::open(artifactRoot));
            UF_TRY_VALUE(
                manifestBytes,
                root.readFile(
                    task::k_runtimeArtifactManifestFileName,
                    task::k_maximumRuntimeManifestBytes
                )
            );
            UF_TRY_VALUE(rootHash, sha256(manifestBytes));
            UF_TRY_VALUE(verified, task::loadRuntimeArtifact(artifactRoot, rootHash));

            return OpenedArtifact{
                .rootHash               = verified.rootHash().hex(),
                .manifestSchemaHash     = verified.manifestSchemaHash().hex(),
                .runtimeModelSchemaHash = verified.runtimeModelSchemaHash().hex(),
                .modelBytes             = verified.modelBytes().size(),
                .assets                 = verified.assetPaths().size(),
            };
        }

        [[nodiscard]]
        auto describeRole(
            deployment::ProjectConformanceRole const& role
        ) -> OpenedRole
        {
            auto const& vocabulary = role.vocabulary;
            return OpenedRole{
                .deployment               = role.deployment,
                .mutatingTool             = vocabulary.mutatingTool,
                .otherMutatingTool        = vocabulary.otherMutatingTool,
                .readOnlyTool             = vocabulary.readOnlyTool,
                .absentTool               = vocabulary.absentTool,
                .approvalRequiredPlanTool = vocabulary.approvalRequiredPlanTool,
                .baselineEvent            = vocabulary.baselineEntry.eventType,
                .progressEvent            = vocabulary.progressEntry.eventType,
                .confirmedEvent           = vocabulary.confirmedEntry.eventType,
                .supersededEvent          = vocabulary.supersededEntry.eventType,
                .uiSurface                = vocabulary.uiAction.surface,
                .uiTarget                 = vocabulary.uiAction.uiTarget,
                .uiAction                 = vocabulary.uiAction.action,
            };
        }

        [[nodiscard]]
        auto describeRoleBlock(
            std::string_view heading,
            OpenedRole const& role
        ) -> std::string
        {
            return std::format(
                "\n{:<12}{}\n"
                "  {:<16}{}\n"
                "  {:<16}{}\n"
                "  {:<16}{}\n"
                "  {:<16}{}\n"
                "  {:<16}{}\n"
                "  {:<16}{}, {}, {}, {}\n"
                "  {:<16}{} / {} / {}\n",
                heading,
                role.deployment,
                "mutating",
                role.mutatingTool,
                "other mutating",
                role.otherMutatingTool,
                "read only",
                role.readOnlyTool,
                "absent",
                role.absentTool,
                "approval plan",
                role.approvalRequiredPlanTool,
                "journal",
                role.baselineEvent,
                role.progressEvent,
                role.confirmedEvent,
                role.supersededEvent,
                "ui action",
                role.uiSurface,
                role.uiTarget,
                role.uiAction
            );
        }
    }

    auto openProjectProduct(OpenArgs const& args) -> Result<OpenedProject>
    {
        UF_TRY_VALUE(loaded, deployment::loadProject(args.project, {}));
        UF_TRY_VALUE_CONTEXT(
            artifact,
            verifiedArtifact(loaded.runtimeArtifactRoot),
            std::format(
                "the RuntimeArtifact at {} is not one this binary can start",
                loaded.runtimeArtifactRoot.string()
            )
        );

        auto registrar   = operator_runtime::ProjectPluginRegistrar{};
        auto deployments = std::vector<OpenedDeployment>{};
        deployments.reserve(loaded.deployments.size());
        for (auto const& one : loaded.deployments)
        {
            auto refusal = refusalRegistering(registrar, one);
            deployments.emplace_back(OpenedDeployment{
                .name             = one.name,
                .pluginId         = one.registration.pluginId(),
                .registrationHash = one.registration.hash().hex(),
                .artifactBlobs    = one.artifactBlobs.size(),
                .refusal          = std::move(refusal),
            });
        }

        return OpenedProject{
            .directory           = loaded.directory,
            .runtimeArtifactRoot = loaded.runtimeArtifactRoot,
            .artifact            = std::move(artifact),
            .probeFrameBytes     = loaded.probeFrame.size(),
            .primaryDeployment   = std::move(loaded.primaryDeployment),
            .deployments         = std::move(deployments),
            .underTest           = describeRole(loaded.underTest),
            .foreign             = describeRole(loaded.foreign),
        };
    }

    auto formatOpenedProject(OpenedProject const& opened) -> std::string
    {
        auto text = std::format(
            "{:<18}{}\n"
            "{:<18}{} bytes\n"
            "{:<18}{}\n",
            "project",
            opened.directory.string(),
            "probe frame",
            opened.probeFrameBytes,
            "primary",
            opened.primaryDeployment
        );

        // The whole point of printing the two schema digests is that a reader
        // sees which artifact this binary accepted and on what grounds, so each
        // says it was accepted rather than leaving that to be inferred from the
        // absence of a refusal.
        text += std::format(
            "\nruntime artifact {}\n"
            "  {:<16}{}\n"
            "  {:<16}{} (accepted by this binary)\n"
            "  {:<16}{} (accepted by this binary)\n"
            "  {:<16}{} bytes\n"
            "  {:<16}{}\n",
            opened.runtimeArtifactRoot.string(),
            "root hash",
            opened.artifact.rootHash,
            "manifest schema",
            opened.artifact.manifestSchemaHash,
            "runtime model",
            opened.artifact.runtimeModelSchemaHash,
            "model",
            opened.artifact.modelBytes,
            "assets",
            opened.artifact.assets
        );

        for (auto const& one : opened.deployments)
        {
            text += std::format(
                "\ndeployment {}\n"
                "  {:<16}{}\n"
                "  {:<16}{}\n"
                "  {:<16}{}\n"
                "  {:<16}{}\n",
                one.name,
                "plugin id",
                one.pluginId,
                "registration",
                one.registrationHash,
                "artifact blobs",
                one.artifactBlobs,
                "plugin",
                one.refusal
                    ? std::format("NOT REGISTERED: {}", *one.refusal)
                    : std::string{"registered"}
            );
        }

        text += describeRoleBlock("under test", opened.underTest);
        text += describeRoleBlock("foreign", opened.foreign);
        return text;
    }

    auto everyPluginRegistered(OpenedProject const& opened) noexcept -> bool
    {
        return std::ranges::none_of(
            opened.deployments,
            [](OpenedDeployment const& one)
            {
                return one.refusal.has_value();
            }
        );
    }
}

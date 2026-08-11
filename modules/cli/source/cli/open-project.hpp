#pragma once

#include "args.hpp"

#include <core/error/result.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace uf::cli
{
    // One deployment of an opened project, as this verb reports it.
    //
    // Plain strings deliberately, for the reason TargetListing restates the
    // controller's TargetCandidate: the authorities a load produces are held by
    // the registrar and by nothing here, so a report that carried them would
    // outlive the only thing entitled to answer for them.
    struct OpenedDeployment final
    {
        std::string name{};
        std::string pluginId{};
        std::string registrationHash{};

        std::size_t artifactBlobs{};

        // Why the Operator's registrar refused this deployment's plugin, or
        // nothing at all when the registry holds it afterwards. It is per
        // deployment rather than per project because the loader derives a
        // registration without ever compiling the plugin: a project whose Luau
        // does not compile, or does not define all five entry points, loads and
        // does not register, and the other deployments still do.
        std::optional<std::string> refusal{};
    };

    // One conformance role of an opened project. The five tool names, the four
    // journal event types and the UI action are what a reader can act on; the
    // payloads beside them in ProjectVocabulary are exact document bytes, which
    // a report cannot restate without choosing a serialization of its own.
    struct OpenedRole final
    {
        std::string deployment{};

        std::string mutatingTool{};
        std::string otherMutatingTool{};
        std::string readOnlyTool{};
        std::string absentTool{};
        std::string approvalRequiredPlanTool{};

        std::string baselineEvent{};
        std::string progressEvent{};
        std::string confirmedEvent{};
        std::string supersededEvent{};

        std::string uiSurface{};
        std::string uiTarget{};
        std::string uiAction{};
    };

    struct OpenedProject final
    {
        std::filesystem::path directory{};
        std::filesystem::path runtimeArtifactRoot{};

        // The decoded capture the conformance manifest names, as the byte count
        // of its file. A report has no use for the pixels; that the loader
        // decoded them at all is the fact worth printing.
        std::size_t probeFrameBytes{};

        std::string                   primaryDeployment{};
        std::vector<OpenedDeployment> deployments{};

        OpenedRole underTest{};
        OpenedRole foreign{};
    };

    // Loads the directory, registers every deployment's plugin through
    // ProjectPluginRegistrar, and reports both. The registrar is local to the
    // call: this verb answers whether the directory registers, and a registry
    // that outlived the answer would be a session, which is the next verb's
    // job rather than this one's.
    [[nodiscard]]
    auto openProjectProduct(OpenArgs const& args) -> Result<OpenedProject>;

    // Separate from the load so the shape an operator and a script both read is
    // testable without a directory on disk, as formatTargetListings is.
    [[nodiscard]]
    auto formatOpenedProject(OpenedProject const& opened) -> std::string;

    // Whether every deployment's plugin reached the registry. A project that
    // loaded and did not register is a failure the exit code has to carry, and
    // the report above prints the refusals rather than only their count.
    [[nodiscard]]
    auto everyPluginRegistered(OpenedProject const& opened) noexcept -> bool;
}

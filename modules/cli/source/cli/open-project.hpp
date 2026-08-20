#pragma once

#include "args.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

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

        std::size_t resources{};

        // Why the Operator's registrar refused this deployment's plugin, or
        // nothing at all when the registry holds it afterwards. It is per
        // deployment rather than per project because the loader derives a
        // registration without ever compiling the plugin: a project whose Luau
        // does not compile, or does not define all five entry points, loads and
        // does not register, and the other deployments still do.
        std::optional<std::string> refusal{};
    };

    // The RuntimeArtifact this project names, as task::loadRuntimeArtifact
    // verified it. deployment::loadProductionProject deliberately checks only
    // that the model file exists and is not empty, so without this block the
    // verb would report a clean open for a directory the same binary refuses
    // the moment it activates the artifact.
    struct OpenedArtifact final
    {
        // The digest of runtime-artifact.manifest.json, which is what every
        // other authority names this artifact by. A project directory records
        // no prior commitment to it, so the value handed to the verifier is
        // this verb's own arithmetic over the bytes it read -- the legal and
        // empty comparison of docs/archive/plans/2026-08-11-project-as-data.md 7.0. It
        // is reported so a later run can be held to the same artifact.
        std::string rootHash{};

        // The two generations the verifier held against this binary's own
        // numbers, task::k_runtimeArtifactFormat and k_runtimeModelFormat. A
        // verified artifact carries exactly those, so they are reported rather
        // than compared a second time here.
        uint64 runtimeArtifactFormat{};
        uint64 runtimeModelFormat{};

        // The frozen closure: the model at its fixed name, and every asset the
        // manifest declared, each at the size and digest it declared.
        std::size_t modelBytes{};
        std::size_t assets{};
    };

    struct OpenedProject final
    {
        std::filesystem::path directory{};
        std::filesystem::path runtimeArtifactRoot{};

        OpenedArtifact artifact{};

        std::string                   primaryDeployment{};
        std::vector<OpenedDeployment> deployments{};
    };

    // Loads the directory the way the product starts it, verifies the
    // RuntimeArtifact it names the way the Operator's installer verifies it,
    // registers every deployment's plugin through ProjectPluginRegistrar, and
    // reports all three. The registrar is local to the call: this verb answers
    // whether the directory registers, and a registry that outlived the answer
    // would be a session, which is the next verb's job rather than this one's.
    //
    // It is the production load, so it neither opens umbraflow-conformance.json
    // nor reports anything out of it. A project that ships no conformance
    // fixture is a project this verb opens, and whether a fixture holds
    // together is umbra-flow-conformance's answer rather than this verb's.
    //
    // Two failure shapes, and the split is deliberate. A project-wide fact --
    // the project document, the artifact -- refuses the whole call, because a
    // project that fails one cannot start at all. A per-deployment fact is
    // reported per deployment: a plugin the script substrate cannot compile
    // leaves this call successful with that deployment's `refusal` engaged, so
    // the report still names every other deployment that would register.
    //
    // What is still out of reach: RuntimeModel semantics, and the extent the
    // model declares. Both need a parsed model, and the trusted parser runs
    // only inside a Host generation, which task::InstalledRuntimeArtifact
    // reserves to the Operator's installed-generation CAS.
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

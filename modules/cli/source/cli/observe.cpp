#include "observe.hpp"

#include <deployment/project-directory.hpp>

#include <operator/ledger.hpp>
#include <operator/project-plugin.hpp>

#include <task/platform/confined-file.hpp>
#include <task/runtime-model-file.hpp>
#include <task/task-context.hpp>
#include <task/task-host.hpp>
#include <task/ui-observation.hpp>

#include <engine/session.hpp>

#include <trace/file-sink.hpp>
#include <trace/recorder.hpp>

#include <core/error/result.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace uf::cli
{
    namespace
    {
        // What this verb calls itself in the trace stream. One observation is
        // one stream, so there is no session identity to carry beyond the name
        // of the entry that opened it.
        constexpr auto k_observeSessionId = std::string_view{"umbra-flow-observe"};

        // The name every authority knows this project's RuntimeArtifact by, as
        // this binary's own arithmetic over the manifest bytes the project
        // carries. It is not a value the caller may state: a caller that named
        // the digest could ask the ledger to open an installed generation under
        // a hash the directory in front of it does not produce, and the check
        // that ties the two would compare the caller against itself.
        [[nodiscard]]
        auto projectArtifactRootHash(
            std::filesystem::path const& artifactRoot
        ) -> Result<ContentHash>
        {
            UF_TRY_VALUE(root, task_platform::ConfinedRoot::open(artifactRoot));
            UF_TRY_VALUE(
                manifestBytes,
                root.readFile(
                    task::k_runtimeArtifactManifestFileName,
                    task::k_maximumRuntimeManifestBytes
                )
            );
            return sha256(manifestBytes);
        }
    }

    auto observeProject(
        ObserveArgs const& args,
        ObserveSources sources
    ) -> Result<ObservedState>
    {
        // First, and before the project directory is opened. A Reader the model
        // declares reaches the resolver as a read that could not run, and the
        // resolution then reports a reason from a closed vocabulary that names
        // no flag. Refusing here is the only place the message can name what
        // the caller left out.
        if (!sources.ocrEngine)
        {
            return fail(
                AutomationErrorKind::UnsupportedCapability,
                "observe was given no OCR engine, so every Reader this "
                "project's model declares would report a failed read rather "
                "than what is written on the screen; pass --ocr-models"
            );
        }

        UF_TRY_VALUE(loaded, deployment::loadProject(args.project, {}));
        auto const* const p_deployment = loaded.findDeployment(
            loaded.primaryDeployment
        );
        if (p_deployment == nullptr)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "the project at {} declares no deployment named \"{}\"",
                    loaded.directory.string(),
                    loaded.primaryDeployment
                )
            );
        }

        // The registrar is local and nothing is invoked on the handle it
        // returns. What registering establishes is that the plugin bytes this
        // project pins compile and answer for this registration, which is the
        // one thing a reader of this report needs to know about the party the
        // resolution below would be handed to. Handing it over is not this
        // verb's: the derive envelope is assembled by trusted Operator code
        // inside OperatorCoordinator::createSnapshot, and reaching that needs a
        // provisioned instance and a pinned session -- neither of which a
        // project directory supplies.
        auto registrar = operator_runtime::ProjectPluginRegistrar{};
        UF_TRY_VALUE(
            plugin,
            registrar.registerPlugin(
                p_deployment->registration,
                p_deployment->pluginBytes,
                p_deployment->artifactBlobs,
                p_deployment->schemaOwner
            )
        );

        UF_TRY_VALUE_CONTEXT(
            rootHash,
            projectArtifactRootHash(loaded.runtimeArtifactRoot),
            std::format(
                "the RuntimeArtifact at {} could not be named",
                loaded.runtimeArtifactRoot.string()
            )
        );

        // Opened rather than installed. Installing a release publishes a CAS
        // object and advances the ledger's installed-generation counter, which
        // is a write to production state; this verb reads. The refusal a
        // mismatch produces is the check that ties this directory to what the
        // Operator actually holds, and both of its sides were produced by
        // different parties at different times.
        UF_TRY_VALUE(store, operator_runtime::OperatorCoordinator::open(args.runtime));
        UF_TRY_VALUE_CONTEXT(
            installed,
            store.openInstalledRuntimeArtifact(args.installedGeneration, rootHash),
            std::format(
                "the Operator root {} holds no installed generation {} pinned "
                "to the RuntimeArtifact this project names",
                args.runtime.string(),
                args.installedGeneration
            )
        );
        auto const installedGeneration = installed.installedGeneration();

        auto host = task::TaskHost{};
        UF_TRY_VALUE(generation, host.activateRuntimeArtifact(std::move(installed)));
        UF_TRY_VALUE(binding, host.runtimeModelBinding(generation));

        UF_TRY_VALUE(sink, trace::FileTraceSink::createNew(args.trace));

        // Declared before the session and the context that borrow it, so both
        // are destroyed first and neither outlives the recorder.
        UF_TRY_VALUE(
            recorder,
            trace::TraceRecorder::create(
                std::move(sink),
                trace::TraceStreamSpec{
                    .sessionId           = std::string{k_observeSessionId},
                    .sessionManifestHash = binding.semanticHash(),
                    .producer            = std::string{k_observeSessionId},
                }
            )
        );

        UF_TRY_VALUE(
            session,
            engine::EngineSession::create(
                std::move(sources.frameSource),
                std::move(sources.actionSink),
                recorder,
                engine::EngineSessionConfig{
                    .liveFingerprint         = sources.liveFingerprint,
                    .projectFingerprint      = binding.fingerprint(),
                    .maximumPixelComparisons = args.budget,
                    .recognitionTimeout      = args.recognitionTimeout,
                },
                std::move(sources.ocrEngine)
            )
        );

        auto context = task::TaskContext{std::move(session), recorder};
        UF_TRY_VALUE(snapshot, host.observe(generation, context));

        return ObservedState{
            .project             = loaded.directory,
            .runtimeArtifactRoot = loaded.runtimeArtifactRoot,
            .artifactRootHash    = snapshot.artifactRootHash().hex(),
            .installedGeneration = installedGeneration,
            .deployment          = p_deployment->name,
            .pluginId            = plugin.pluginId(),
            .registrationHash    = plugin.projectRegistrationHash().hex(),
            .modelSemanticHash   = snapshot.semanticHash().hex(),
            .modelWidth          = binding.fingerprint().width(),
            .modelHeight         = binding.fingerprint().height(),
            .liveWidth           = sources.liveFingerprint.width(),
            .liveHeight          = sources.liveFingerprint.height(),
            .observationId       = snapshot.observationId(),
            .targetGeneration    = snapshot.targetGeneration().value(),
            .stateResolutionHash = snapshot.stateResolutionHash().hex(),
            .stateResolution     = snapshot.canonicalJcs(),
            .trace               = args.trace,
        };
    }

    auto formatObservedState(ObservedState const& observed) -> std::string
    {
        auto text = std::format(
            "{:<20}{}\n"
            "{:<20}{}\n"
            "{:<20}{}\n",
            "project",
            observed.project.string(),
            "deployment",
            observed.deployment,
            "trace",
            observed.trace.string()
        );

        text += std::format(
            "\nruntime artifact {}\n"
            "  {:<18}{}\n"
            "  {:<18}{}\n"
            "  {:<18}{}\n",
            observed.runtimeArtifactRoot.string(),
            "root hash",
            observed.artifactRootHash,
            "installed gen",
            observed.installedGeneration,
            "semantic hash",
            observed.modelSemanticHash
        );

        // Both extents, always. The engine already refused a capture the model
        // cannot describe, so printing only one would leave a reader of a
        // resolution that found nothing unable to rule the geometry out.
        text += std::format(
            "\nplugin {}\n"
            "  {:<18}{}\n"
            "\ngeometry\n"
            "  {:<18}{}x{}\n"
            "  {:<18}{}x{}\n",
            observed.pluginId,
            "registration",
            observed.registrationHash,
            "model declares",
            observed.modelWidth,
            observed.modelHeight,
            "target presented",
            observed.liveWidth,
            observed.liveHeight
        );

        // The document last and whole. Its kind, its ordered surface stack, and
        // one entry per Reader every reporting Binding named are inside it, and
        // C++ interprets none of them: these are the bytes the resolver
        // produced and the bytes a plugin would be handed.
        text += std::format(
            "\nobservation {}\n"
            "  {:<18}{}\n"
            "  {:<18}{}\n"
            "\nstate resolution\n"
            "{}\n",
            observed.observationId,
            "target generation",
            observed.targetGeneration,
            "resolution hash",
            observed.stateResolutionHash,
            observed.stateResolution
        );
        return text;
    }
}

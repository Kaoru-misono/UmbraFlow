#include "observe.hpp"

#include <service/product-lifecycle.hpp>

#include <task/task-context.hpp>
#include <task/ui-observation.hpp>

#include <engine/session.hpp>

#include <trace/file-sink.hpp>
#include <trace/recorder.hpp>

#include <core/error/result.hpp>

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
        // The controller this verb authenticates as. It reaches the session pin
        // and the lease fence through ProductStart, so it names the entry point
        // rather than the run: every observation this process makes presents
        // the same one, and the Operator mints the session behind it.
        constexpr auto k_observeControllerId = std::string_view{"umbra-flow-observe"};

        // What wrote the trace stream. The stream's session identity is the
        // Operator's and arrives through ProductIdentity; the producer is the
        // only part of the stream header this verb may name after itself.
        constexpr auto k_observeProducer = std::string_view{"umbra-flow-observe"};

        // The observation itself, over a lifecycle the caller started and will
        // close. The lifecycle is mutable because observing is a mutation of
        // the caller's Operator session -- it publishes a snapshot row -- and
        // the split exists so that every failure below returns into the one
        // close observeProject performs, rather than into a path that has to
        // remember to perform it.
        [[nodiscard]]
        auto observeThroughLifecycle(
            ObserveArgs const& args,
            ObserveSources sources,
            service::ProductLifecycle& lifecycle
        ) -> Result<ObservedState>
        {
            auto const identity = lifecycle.identity();

            UF_TRY_VALUE(sink, trace::FileTraceSink::createNew(args.trace));

            // Declared before the session and the context that borrow it, so
            // both are destroyed first and neither outlives the recorder.
            UF_TRY_VALUE(
                recorder,
                trace::TraceRecorder::create(
                    std::move(sink),
                    trace::TraceStreamSpec{
                        .sessionId           = identity.sessionId,
                        .sessionManifestHash = identity.sessionManifestHash,
                        .producer            = std::string{k_observeProducer},
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
                        .projectFingerprint      = identity.runtimeModel.fingerprint(),
                        .maximumPixelComparisons = args.budget,
                        .recognitionTimeout      = args.recognitionTimeout,
                    },
                    std::move(sources.ocrEngine)
                )
            );

            auto context = task::TaskContext{std::move(session), recorder};
            UF_TRY_VALUE(observed, lifecycle.observe(context));
            auto const& snapshot = observed.ui;

            return ObservedState{
                .project             = identity.projectDirectory,
                .runtimeArtifactRoot = identity.runtimeArtifactRoot,
                .artifactRootHash    = snapshot.artifactRootHash().hex(),
                .installedGeneration = identity.installedGeneration,
                .deployment          = identity.deployment,
                .pluginId            = identity.pluginId,
                .registrationHash    = identity.registrationHash.hex(),
                .modelSemanticHash   = snapshot.semanticHash().hex(),
                .modelWidth          = identity.runtimeModel.fingerprint().width(),
                .modelHeight         = identity.runtimeModel.fingerprint().height(),
                .liveWidth           = sources.liveFingerprint.width(),
                .liveHeight          = sources.liveFingerprint.height(),
                .observationId       = snapshot.observationId(),
                .targetGeneration    = snapshot.targetGeneration().value(),
                .stateResolutionHash = snapshot.stateResolutionHash().hex(),
                .stateResolution     = snapshot.canonicalJcs(),
                .trace               = args.trace,
            };
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

        UF_TRY_VALUE(
            lifecycle,
            service::ProductLifecycle::start(
                service::ProductStart{
                    .projectDirectory          = args.project,
                    .runtimeDirectory          = args.runtime,
                    .authenticatedControllerId = std::string{k_observeControllerId},
                    .controllerCapabilities    = {},
                    .controlledTargetId = std::format(
                        "window-{}",
                        args.windowHandle
                    ),
                }
            )
        );

        // Started, therefore closed: the control lease this verb now holds is
        // the Operator's answer to "who may act on this target", and a process
        // that returns without releasing it leaves that answer standing with
        // nobody behind it. The close is unconditional and its result is
        // combined rather than propagated on the spot, so a failed observation
        // stays the failure the caller is told about.
        auto observed = observeThroughLifecycle(args, std::move(sources), lifecycle);
        auto closed   = lifecycle.shutdown();
        return service::reportAfterClose(std::move(observed), std::move(closed));
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

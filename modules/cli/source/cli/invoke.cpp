#include "invoke.hpp"

#include <service/product-lifecycle.hpp>

#include <operator/controller.hpp>
#include <operator/project-plugin.hpp>
#include <operator/tool-actor-adapters.hpp>
#include <operator/tool-runtime.hpp>

#include <task/task-context.hpp>

#include <engine/session.hpp>

#include <trace/file-sink.hpp>
#include <trace/recorder.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <json/value.hpp>

#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

namespace uf::cli
{
    namespace
    {
        // What wrote the trace stream. It names the entry point rather than
        // the run, for k_observeProducer's reason: the stream's session
        // identity is the Operator's and arrives through ProductIdentity, and
        // the producer is the only part of the header this verb may name after
        // itself.
        constexpr auto k_invokeProducer = std::string_view{"umbra-flow-invoke"};

        // The controller this verb authenticates as, composed from the kind's
        // own wire name.
        //
        // Composed rather than looked up, because a table pairing a kind with
        // a literal id is a table two entries can disagree in. And one id per
        // kind rather than one for the verb, because the durable root request
        // is idempotent inside the controller's own namespace: a single shared
        // id would make two different actors presenting the same --request-key
        // and the same preimage produce one bit-identical root identity, so
        // one actor's rerun would silently resolve into the other's call.
        [[nodiscard]]
        auto controllerIdOf(operator_runtime::ControllerKind kind) -> std::string
        {
            return std::format(
                "umbra-flow-invoke-{}",
                operator_runtime::controllerKindWireName(kind)
            );
        }

        // The principal one --actor word names, read off the transport the
        // parser already chose for it.
        //
        // Read off the transport rather than carried beside it on purpose.
        // --actor is one flag, and it decides both which of the three
        // ProductLifecycle transports the call is presented at and which
        // controller kind the session is pinned as. Deriving the second from
        // the first leaves one source for both, which is why there is no
        // cross-check here that the two agree: a check over one source is a
        // check nothing could ever make red.
        struct ActorPrincipal final
        {
            operator_runtime::ControllerKind kind{};

            // Where the AgentProfile document is, for the one kind whose
            // budgets the Operator holds. Absent for the two that stop on
            // their own, whose sessions the ledger refuses a profile from.
            std::optional<std::filesystem::path> agentProfile{};
        };

        [[nodiscard]]
        auto principalOf(AgentToolRequest const& material) -> ActorPrincipal
        {
            return ActorPrincipal{
                .kind         = operator_runtime::ControllerKind::Agent,
                .agentProfile = material.agentProfileDocument,
            };
        }

        [[nodiscard]]
        auto principalOf(HumanToolRequest const&) -> ActorPrincipal
        {
            return ActorPrincipal{
                .kind         = operator_runtime::ControllerKind::Human,
                .agentProfile = std::nullopt,
            };
        }

        // A Project's automation is a program, and a program stops when it
        // ends. Script is the Operator's name for that principal.
        [[nodiscard]]
        auto principalOf(ProjectAutomationRequest const&) -> ActorPrincipal
        {
            return ActorPrincipal{
                .kind         = operator_runtime::ControllerKind::Script,
                .agentProfile = std::nullopt,
            };
        }

        // An overload set over the closed set of transports rather than a
        // chain, for startActorTool's reason: a fourth alternative added to
        // ActorToolRequest must fail to compile here rather than be given some
        // kind by default.
        //
        // The visitor captures nothing and neither escapes nor outlives this
        // call.
        [[nodiscard]]
        auto actorPrincipal(ActorToolRequest const& request) -> ActorPrincipal
        {
            return std::visit(
                [](auto const& material)
                {
                    return principalOf(material);
                },
                request
            );
        }

        // The largest transport document this front end will read. A ceiling
        // rather than a policy: the objective and the arguments are one
        // tool-use block's two halves, and a file past this is a caller that
        // named the wrong path.
        constexpr auto k_maximumTransportDocumentBytes = uintmax{1} << 20U;

        [[nodiscard]]
        auto readTransportDocument(
            std::filesystem::path const& path,
            std::string_view flag
        ) -> Result<std::string>
        {
            auto sizeFailure = std::error_code{};
            auto const size  = std::filesystem::file_size(path, sizeFailure);
            if (sizeFailure)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format("cannot read {} \"{}\"", flag, path.string()),
                    sizeFailure
                );
            }
            if (size > k_maximumTransportDocumentBytes)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "{} \"{}\" holds {} bytes, past the {} one transport "
                        "document is read under",
                        flag,
                        path.string(),
                        size,
                        k_maximumTransportDocumentBytes
                    )
                );
            }

            auto stream = std::ifstream{path, std::ios::binary};
            if (!stream.is_open())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format("cannot open {} \"{}\"", flag, path.string())
                );
            }

            auto text = std::string{};
            text.resize(static_cast<std::size_t>(size));
            stream.read(text.data(), static_cast<std::streamsize>(size));
            if (stream.bad())
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format("cannot read {} \"{}\"", flag, path.string())
                );
            }
            text.resize(static_cast<std::size_t>(stream.gcount()));
            return text;
        }

        // The value a model's or a Project's transport would have delivered.
        // Parsed here rather than passed along as bytes, because both of those
        // transports carry a structure their runtime already holds; this front
        // end stands in for that runtime and hands over the same value.
        [[nodiscard]]
        auto readTransportValue(
            std::filesystem::path const& path,
            std::string_view flag
        ) -> Result<json::Value>
        {
            UF_TRY_VALUE(text, readTransportDocument(path, flag));
            UF_TRY_VALUE_CONTEXT(
                value,
                json::parse(text),
                std::format("{} \"{}\"", flag, path.string())
            );
            return value;
        }

        // What one started call reports beside its replay. The name comes back
        // because the three transports carry it under different members -- a
        // Project names an entry -- so reading it off the variant a second time
        // would be a second answer to a question the dispatch already answered.
        struct StartedTool final
        {
            std::string                      toolName{};
            operator_runtime::ToolCallReplay replay{};
        };

        [[nodiscard]]
        auto startTool(
            AgentToolRequest const& material,
            std::string const& requestKey,
            service::ProductLifecycle& lifecycle,
            task::TaskContext& context
        ) -> Result<StartedTool>
        {
            UF_TRY_VALUE(
                objective,
                readTransportValue(material.objectiveDocument, "--objective-file")
            );
            UF_TRY_VALUE(
                arguments,
                readTransportValue(material.argumentsDocument, "--arguments-file")
            );
            UF_TRY_VALUE(
                replay,
                lifecycle.invokeAgentTool(
                    operator_runtime::AgentToolUse{
                        .requestKey = requestKey,
                        .objective  = std::move(objective),
                        .toolName   = material.toolName,
                        .arguments  = std::move(arguments),
                    },
                    context
                )
            );
            return StartedTool{
                .toolName = material.toolName,
                .replay   = std::move(replay),
            };
        }

        [[nodiscard]]
        auto startTool(
            HumanToolRequest const& material,
            std::string const& requestKey,
            service::ProductLifecycle& lifecycle,
            task::TaskContext& context
        ) -> Result<StartedTool>
        {
            // Handed over exactly as it was typed, and deliberately not parsed
            // first. HumanToolAdapter parses and re-renders, which is what
            // makes a person's spacing, member order and number spelling mean
            // what the author meant rather than being refused as non-canonical.
            UF_TRY_VALUE(
                replay,
                lifecycle.invokeHumanTool(
                    operator_runtime::HumanToolCommand{
                        .requestKey    = requestKey,
                        .objectiveText = material.objectiveText,
                        .toolName      = material.toolName,
                        .argumentsText = material.argumentsText,
                    },
                    context
                )
            );
            return StartedTool{
                .toolName = material.toolName,
                .replay   = std::move(replay),
            };
        }

        [[nodiscard]]
        auto startTool(
            ProjectAutomationRequest const& material,
            std::string const& requestKey,
            service::ProductLifecycle& lifecycle,
            task::TaskContext& context
        ) -> Result<StartedTool>
        {
            UF_TRY_VALUE(
                objective,
                readTransportValue(material.objectiveDocument, "--objective-file")
            );
            UF_TRY_VALUE(
                arguments,
                readTransportValue(material.argumentsDocument, "--arguments-file")
            );
            UF_TRY_VALUE(
                replay,
                lifecycle.startProjectAutomation(
                    operator_runtime::ProjectAutomationStart{
                        .requestKey    = requestKey,
                        .objective     = std::move(objective),
                        .entryToolName = material.entryToolName,
                        .arguments     = std::move(arguments),
                    },
                    context
                )
            );
            return StartedTool{
                .toolName = material.entryToolName,
                .replay   = std::move(replay),
            };
        }

        // An overload set over the closed set of transports rather than a
        // chain: a fourth alternative added to ActorToolRequest must fail to
        // compile here rather than fall through to one of the three.
        //
        // The visitor captures by reference and neither escapes nor outlives
        // this call.
        [[nodiscard]]
        auto startActorTool(
            InvokeArgs const& args,
            service::ProductLifecycle& lifecycle,
            task::TaskContext& context
        ) -> Result<StartedTool>
        {
            return std::visit(
                [&](auto const& material)
                {
                    return startTool(
                        material,
                        args.requestKey,
                        lifecycle,
                        context
                    );
                },
                args.request
            );
        }

        [[nodiscard]]
        auto documentBytes(
            std::optional<operator_runtime::CanonicalJson> const& document
        ) -> std::optional<std::string>
        {
            if (!document) return std::nullopt;
            return document->bytes();
        }

        // The call itself, over a lifecycle the caller started and will close.
        // The split is observeThroughLifecycle's: every failure below returns
        // into the one close invokeTool performs, rather than into a path that has
        // to remember to perform it.
        [[nodiscard]]
        auto invokeThroughLifecycle(
            InvokeArgs const& args,
            ObserveSources sources,
            service::ProductLifecycle& lifecycle
        ) -> Result<ToolInvokeReport>
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
                        .producer            = std::string{k_invokeProducer},
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
            UF_TRY_VALUE(started, startActorTool(args, lifecycle, context));

            auto const& replay = started.replay;
            auto state         = std::string{
                operator_runtime::toolCallStateWireName(replay.state)
            };

            return ToolInvokeReport{
                .project                = identity.projectDirectory,
                .runtimeArtifactRoot    = identity.runtimeArtifactRoot,
                .deployment             = identity.deployment,
                .pluginId               = identity.pluginId,
                .actor                  = std::string{actorName(args.request)},
                .toolName               = started.toolName,
                .requestKey             = args.requestKey,
                .state                  = std::move(state),
                .revision               = replay.revision,
                .activeAdmissionAttempt = replay.activeAdmissionAttempt,
                .payload                = documentBytes(replay.payload),
                .evidence               = documentBytes(replay.evidence),
                .trace                  = args.trace,
            };
        }
    }

    auto invokeTool(
        InvokeArgs const& args,
        ObserveSources sources
    ) -> Result<ToolInvokeReport>
    {
        // First, and before the project directory is opened, for
        // observeProject's reason: a Reader this project's model declares
        // reaches the resolver as a read that could not run, and the resolution
        // then reports a reason from a closed vocabulary that names no flag.
        if (!sources.ocrEngine)
        {
            return fail(
                AutomationErrorKind::UnsupportedCapability,
                "invoke was given no OCR engine, so every Reader this "
                "project's model declares would report a failed read rather "
                "than what is written on the screen; pass --ocr-models"
            );
        }

        // The principal, and with it the profile document the Operator
        // requires of exactly that principal. Both come off the transport
        // --actor already chose, so nothing here can name one actor and pin
        // another.
        auto const principal = actorPrincipal(args.request);
        auto agentProfileJcs = std::optional<std::string>{};
        if (principal.agentProfile)
        {
            UF_TRY_VALUE(
                profile,
                readTransportDocument(*principal.agentProfile, "--agent-profile")
            );
            agentProfileJcs.emplace(std::move(profile));
        }

        // One invoke process is one run over its target window, so the session
        // observes in a fresh run scope named after that window.
        auto const controlledTargetId = std::format("window-{}", args.windowHandle);
        UF_TRY_VALUE(
            worldScope,
            operator_runtime::ObservedInstanceWorldScope::run(
                controlledTargetId,
                1
            )
        );
        UF_TRY_VALUE(
            lifecycle,
            service::ProductLifecycle::start(
                service::ProductStart{
                    .projectDirectory          = args.project,
                    .runtimeDirectory          = args.runtime,
                    .authenticatedControllerId = controllerIdOf(principal.kind),
                    .controllerCapabilities    = args.capabilities,
                    .controlledTargetId        = controlledTargetId,
                    .kind                      = principal.kind,
                    .agentProfileJcs           = std::move(agentProfileJcs),
                    .worldScope                = worldScope,
                }
            )
        );

        // Started, therefore closed, as observeProject closes: the control
        // lease this verb holds is the Operator's answer to "who may act on
        // this target", and a process that returned without releasing it would
        // leave that answer standing with nobody behind it. The close is
        // unconditional and its result is combined rather than propagated on
        // the spot, so a failed call stays the failure the caller is told about.
        auto report = invokeThroughLifecycle(args, std::move(sources), lifecycle);
        auto closed = lifecycle.shutdown();
        return service::reportAfterClose(std::move(report), std::move(closed));
    }

    auto formatToolInvoke(ToolInvokeReport const& report) -> std::string
    {
        auto text = std::format(
            "{:<20}{}\n"
            "{:<20}{}\n"
            "{:<20}{}\n",
            "project",
            report.project.string(),
            "deployment",
            report.deployment,
            "trace",
            report.trace.string()
        );

        text += std::format(
            "\nruntime artifact {}\n"
            "  {:<18}{}\n",
            report.runtimeArtifactRoot.string(),
            "plugin",
            report.pluginId
        );

        // The actor and the key beside the state, because those two are what a
        // reader needs to find this row again: the durable request is
        // idempotent on the key under the controller's own namespace, and a
        // rerun resolves to the same coordinate rather than a new one.
        text += std::format(
            "\ncall\n"
            "  {:<18}{}\n"
            "  {:<18}{}\n"
            "  {:<18}{}\n"
            "  {:<18}{}\n"
            "  {:<18}{}\n"
            "  {:<18}{}\n",
            "actor",
            report.actor,
            "tool",
            report.toolName,
            "request key",
            report.requestKey,
            "state",
            report.state,
            "revision",
            report.revision,
            "admission attempt",
            report.activeAdmissionAttempt
        );

        // Both documents last and whole, and both named even when the row
        // carries neither. A reader deciding whether a call concluded has to
        // tell "the provider recorded nothing" from "this report dropped it".
        text += std::format(
            "\nresult\n{}\n"
            "\nevidence\n{}\n",
            report.payload.value_or(std::string{"(none)"}),
            report.evidence.value_or(std::string{"(none)"})
        );
        return text;
    }
}

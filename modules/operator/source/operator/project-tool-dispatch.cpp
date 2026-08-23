#include "project-tool-dispatch.hpp"

#include <json/value.hpp>

#include <core/types/integer.hpp>
#include <core/utility/scope-exit.hpp>

#include <domain/error.hpp>

#include <format>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace uf::operator_runtime
{
    namespace
    {
        // One recorded outcome in the shape @umbraflow/tools admits: the tool
        // the seam ran, the position it was recorded at, its delivery
        // classification, and the Tool's own result and evidence when the
        // outcome carries them.
        //
        // A Tool that ran and failed is a VALUE here. Only a refusal -- a
        // protocol, authority or replay-mismatch fact about the run -- leaves
        // this function as an error, and only that tears the VM down.
        [[nodiscard]]
        auto scopedAnswer(
            std::string_view toolName,
            ContentHash callIdentity,
            ToolCallReplay const& replay
        ) -> json::Value
        {
            auto members = std::vector<json::Member>{};
            members.emplace_back(
                "call_identity",
                json::Value::ofString(callIdentity.hex())
            );
            if (replay.evidence)
            {
                members.emplace_back("evidence", replay.evidence->value());
            }
            if (replay.payload)
            {
                members.emplace_back("result", replay.payload->value());
            }
            members.emplace_back(
                "state",
                json::Value::ofString(
                    std::string{toolCallStateWireName(replay.state)}
                )
            );
            members.emplace_back(
                "tool",
                json::Value::ofString(std::string{toolName})
            );
            return json::Value::ofObject(std::move(members));
        }
    } // namespace

    // The dispatcher's own state, and the receiver every step of a dispatch
    // runs on. It is a class rather than a bag of free functions because the
    // steps share it: the live-run table, the ledger and the Framework catalog
    // are one subject, and passing it by hand three times would be a receiver
    // in disguise.
    class ProjectToolDispatcher::State final
    {
    public:
        // One live handler invocation. Every member but the context is a borrow
        // owned by the dispatch frame that built it, and that frame strictly
        // outlives the run: the run executes synchronously inside it, and the
        // table entry is erased before it returns.
        struct ActiveRun final
        {
            ProjectGenerationHandle const& program;
            ControllerBinding const&        controller;
            ControlLease const&             lease;
            ToolRootRequestIdentity const&  root;
            ToolCallPositionIdentity const& handlerCall;

            // Fresh on every entry, first dispatch and re-entry alike, and
            // numbering from 1. Nothing persists it and nothing resumes it.
            ToolCallIssuingContext context;
        };

    private:
        OperatorCoordinator& m_coordinator;

        // The run's observation authority, borrowed. It is the Framework
        // providers' own, so what it recognises is exactly what they minted.
        SnapshotObservationAuthority& m_observations;

        OperatorPolicyAuthority   m_policyAuthority;
        ToolProvider              m_frameworkTools;
        FrameworkToolCatalogOwner m_frameworkCatalog;

        std::map<ContentHash, std::reference_wrapper<ActiveRun>> m_runs{};

    public:
        State(
            OperatorCoordinator& coordinator,
            SnapshotObservationAuthority& observations,
            OperatorPolicyAuthority policyAuthority,
            ToolProvider frameworkTools,
            FrameworkToolCatalogOwner frameworkCatalog
        )
            : m_coordinator{coordinator}
            , m_observations{observations}
            , m_policyAuthority{std::move(policyAuthority)}
            , m_frameworkTools{std::move(frameworkTools)}
            , m_frameworkCatalog{std::move(frameworkCatalog)}
        {
        }

        [[nodiscard]]
        auto dispatchCall(
            ProjectGenerationHandle const& program,
            ToolAdmissionRequest const& request,
            std::stop_token const& cancellation
        ) -> Result<ToolCallReplay>
        {
            // A synchronous, non-escaping callback: the executor runs it inside
            // this frame, after the dispatch boundary is durable, or not at all.
            auto const runHandler =
                [this, &program, &request, &cancellation](
                    ToolCallPositionIdentity const& dispatched
                ) -> Result<ToolCallCompletion>
            {
                return runBoundEntry(
                    program,
                    request.controller,
                    request.lease,
                    request.root,
                    dispatched,
                    cancellation
                );
            };
            return ToolRuntimeExecutor{m_coordinator}.invoke(request, runHandler);
        }

        // One child call, arriving from inside a running handler's VM.
        [[nodiscard]]
        auto issueChild(
            std::string_view toolName,
            json::Value const& arguments,
            script::ToolCallCoordinate const& coordinate,
            std::stop_token const& cancellation
        ) -> Result<json::Value>
        {
            auto const found = m_runs.find(coordinate.parentPosition);
            if (found == m_runs.end())
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "no live issuing context is anchored on the durable position "
                        + coordinate.parentPosition.hex()
                );
            }
            auto& run = found->second.get();

            // The VM counted this call inside the run, and the issuing context
            // counted it inside the ledger. They are two implementations of one
            // number and this is the only place they meet, so a disagreement is
            // a divergence rather than a detail.
            auto const expected =
                static_cast<uint64>(run.context.issuedChildren()) + 1U;
            if (coordinate.childIndex != expected)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    std::format(
                        "the scoped run numbered a Tool call {} where its issuing "
                        "context is at {}",
                        coordinate.childIndex,
                        expected
                    )
                );
            }

            UF_TRY_VALUE(
                canonicalArguments,
                CanonicalJson::parseExact(json::canonicalBytes(arguments))
            );
            auto const bound = run.program.bindingTable().entryPointFor(toolName);
            auto validated = bound.has_value()
                ? run.program.catalog().validate(
                      std::string{toolName},
                      std::move(canonicalArguments)
                  )
                : m_frameworkCatalog.validate(
                      std::string{toolName},
                      std::move(canonicalArguments)
                  );
            UF_TRY_VALUE(invocation, std::move(validated));

            // The observation this child spends, if it spends one. The script
            // handed the seam an ordinary JSON value it received from an
            // earlier call; only the authority the Framework providers mint
            // into can say those bytes are a reference at all, and only holding
            // the reference it returns opens a coordinate against it. So the
            // script supplies data and the host supplies authority, which is
            // the only division that lets a script spend what observe returned
            // without ever holding what spending it requires.
            UF_TRY_VALUE(
                observation,
                m_observations.presented(invocation.canonicalArgs())
            );
            UF_TRY_VALUE(
                child,
                observation.has_value()
                    ? run.context.issueAgainstObservation(
                          invocation,
                          *observation
                      )
                    : run.context.issue(invocation)
            );

            // Re-derived from the durable parent row and the parent's own
            // descriptor on every child, never accumulated: the grant id is the
            // content address of the handler execution it authorises, so a
            // re-entered handler mints the same authority rather than a second
            // one.
            UF_TRY_VALUE(
                grant,
                m_coordinator.issueToolDelegationGrant(run.handlerCall)
            );

            // What a mutating child proposes is derived, never stated: one
            // effect per bound the CHILD'S OWN descriptor declares, scoped to
            // the controlled target this dispatcher's binding holds the lease
            // on. The script named a Tool and an argument value and nothing
            // else, so the effect set, its scope and its risk are all the
            // catalog's and the session's. Every ceiling above it -- the
            // parent's child-effect declaration, the admitted root envelope,
            // the pinned policy -- is then judged inside admission, which is
            // where widening is refused.
            auto const childRequest = ToolAdmissionRequest{
                .controller = run.controller,
                .lease      = run.lease,
                .root       = run.root,
                .call       = child,
                .mutation   = proposedToolMutation(
                    invocation,
                    m_policyAuthority,
                    run.controller.controlledTargetId()
                ),
                .delegation = std::move(grant),
            };
            if (bound.has_value())
            {
                UF_TRY_VALUE(
                    replay,
                    dispatchCall(run.program, childRequest, cancellation)
                );
                return scopedAnswer(toolName, child.identity(), replay);
            }
            UF_TRY_VALUE(
                replay,
                ToolRuntimeExecutor{m_coordinator}.invoke(
                    childRequest,
                    m_frameworkTools
                )
            );
            return scopedAnswer(toolName, child.identity(), replay);
        }

    private:
        [[nodiscard]]
        auto runBoundEntry(
            ProjectGenerationHandle const& program,
            ControllerBinding const& controller,
            ControlLease const& lease,
            ToolRootRequestIdentity const& root,
            ToolCallPositionIdentity const& call,
            std::stop_token const& cancellation
        ) -> Result<ToolCallCompletion>
        {
            UF_TRY_VALUE(arguments, json::parse(call.canonicalArgs()));
            auto run = ActiveRun{
                .program     = program,
                .controller  = controller,
                .lease       = lease,
                .root        = root,
                .handlerCall = call,
                .context     = ToolCallIssuingContext::forHandler(call),
            };
            // Review-only: no reachable path fails this. A run is keyed on the
            // position of the call it implements, and a child call is a
            // different position by construction, so recursion cannot collide.
            // It stays because ignoring a failed insert would leave the seam
            // resolving a stale run, which is worse than a refusal nothing can
            // trigger.
            auto const registered =
                m_runs.emplace(call.identity(), std::ref(run)).second;
            if (!registered)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "a Tool call is already dispatching in this process at "
                        + call.identity().hex()
                );
            }
            auto const closeRun = scopeExit(
                [this, identity = call.identity()]() noexcept
                {
                    m_runs.erase(identity);
                }
            );

            UF_TRY_VALUE(
                answer,
                program.invokeBoundTool(
                    call.toolName(),
                    arguments,
                    script::ScopedRunRequest{
                        .parentPosition = call.identity(),
                        .cancellation   = cancellation,
                    }
                )
            );
            UF_TRY_VALUE(
                result,
                CanonicalJson::parseExact(json::canonicalBytes(answer))
            );
            // A handler that terminated leaving recorded calls unconsumed for
            // this context issued a different sequence than the one on record,
            // which is divergence even though every call it did issue matched.
            UF_TRY(m_coordinator.sealToolCallContext(root, run.context));
            return ToolCallCompletion::confirmed(std::move(result));
        }
    };

    ProjectToolDispatcher::ProjectToolDispatcher(
        std::shared_ptr<State> p_state
    ) noexcept
        : m_state{std::move(p_state)}
    {
    }

    auto ProjectToolDispatcher::create(
        OperatorCoordinator& coordinator,
        SnapshotObservationAuthority& observations,
        OperatorPolicyAuthority policyAuthority,
        ToolProvider frameworkTools
    ) -> Result<ProjectToolDispatcher>
    {
        if (!frameworkTools)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Project Tool dispatch requires a Framework Tool provider"
            );
        }
        UF_TRY_VALUE(frameworkCatalog, FrameworkToolCatalogOwner::create());
        return ProjectToolDispatcher{std::make_shared<State>(
            coordinator,
            observations,
            std::move(policyAuthority),
            std::move(frameworkTools),
            std::move(frameworkCatalog)
        )};
    }

    auto ProjectToolDispatcher::toolRuntimeSeam() const -> script::ToolRuntimeInvoke
    {
        // Owns a share of the state and captures nothing else: the seam a
        // program is compiled with must survive every run that program starts,
        // and must carry no run of its own.
        return [state = m_state](
                   std::string_view toolName,
                   json::Value const& arguments,
                   script::ToolCallCoordinate const& coordinate,
                   std::stop_token cancellation
               ) -> Result<json::Value>
        {
            return state->issueChild(
                toolName,
                arguments,
                coordinate,
                cancellation
            );
        };
    }

    auto ProjectToolDispatcher::dispatch(
        ProjectGenerationHandle const& program,
        ToolAdmissionRequest const& request,
        std::stop_token cancellation
    ) -> Result<ToolCallReplay>
    {
        return m_state->dispatchCall(program, request, cancellation);
    }
}

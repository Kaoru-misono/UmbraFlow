#include "declarative-workflow-tool.hpp"

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <json/error.hpp>
#include <json/schema.hpp>
#include <json/value.hpp>

#include <schema/framework-schema-catalog.hpp>

#include <algorithm>
#include <format>
#include <map>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::project
{
    namespace
    {
        enum class StateKind : uint8
        {
            Wait,
            UiAction,
        };

        struct WorkflowState final
        {
            std::string stateKey{};
            StateKind   kind{StateKind::Wait};
            std::string uiAction{};
            uint32      observationBudget{};
            uint64      timeoutMillis{};
        };

        struct WorkflowBounds final
        {
            uint32 maximumStates{};
            uint32 maximumSteps{};
            uint32 maximumDispatches{};
            uint32 maximumObservations{};
            uint32 maximumWaits{};
            uint64 maximumElapsedMillis{};
        };

        struct WorkflowTool final
        {
            std::string                toolName{};
            std::string                targetArgument{};
            std::vector<std::string>   allowedInstanceKinds{};
            std::string                requiredSurface{};
            std::string                findingKind{};
            std::vector<WorkflowState> steps{};
            WorkflowBounds             bounds{};
        };

        [[nodiscard]]
        auto refuse(
            std::string_view code,
            std::string_view detail
        ) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format("{}: {}", code, detail)
            );
        }

        [[nodiscard]]
        auto isAsciiLetter(char value) noexcept -> bool
        {
            return (
                (value >= 'A' && value <= 'Z')
                || (value >= 'a' && value <= 'z')
            );
        }

        [[nodiscard]]
        auto isAsciiDigit(char value) noexcept -> bool
        {
            return value >= '0' && value <= '9';
        }

        [[nodiscard]]
        auto isNamespacedIdentifier(std::string_view value) noexcept -> bool
        {
            if (value.empty() || !isAsciiLetter(value.front()))
            {
                return false;
            }
            auto sawSeparator   = false;
            auto atSegmentStart = false;
            for (auto const character : value)
            {
                if (character == '.')
                {
                    if (atSegmentStart)
                    {
                        return false;
                    }
                    sawSeparator   = true;
                    atSegmentStart = true;
                    continue;
                }
                if (
                    !isAsciiLetter(character)
                    && !isAsciiDigit(character)
                    && character != '_'
                    && character != '-'
                )
                {
                    return false;
                }
                if (
                    atSegmentStart
                    && !isAsciiLetter(character)
                    && !isAsciiDigit(character)
                )
                {
                    return false;
                }
                atSegmentStart = false;
            }
            return sawSeparator && !atSegmentStart;
        }

        // A member the published schema has already declared present, so its
        // absence would be a defect in this reader rather than in the document.
        [[nodiscard]]
        auto member(
            json::Value const& object,
            std::string_view name
        ) -> json::Value const&
        {
            auto const* const p_value = object.find(name);
            UF_CHECK(p_value != nullptr);
            return *p_value;
        }

        constexpr auto k_workflowSchemaPath = std::string_view{
            "schema/umbraflow-declarative-workflow-tool-v1.schema.json"
        };

        // The published schema is the single authority over the declaration's
        // shape: closure, membership, types, patterns and value bounds all come
        // from it. Its refusal kinds map onto the lock's codes -- a closed
        // object carrying an undeclared member is ClosedSchema, every other
        // schema rejection is MalformedWorkflowTool. A schema problem (the
        // compiled catalog missing the document, a keyword this evaluator does
        // not implement) is a deployment defect no declaration can fix and is
        // reported without a code.
        [[nodiscard]]
        auto adoptSchema(Status outcome) -> Status
        {
            if (outcome.has_value())
            {
                return ok();
            }
            auto const kind = json::errorKind(outcome.error());
            if (kind == json::ErrorKind::DocumentClosureRejected)
            {
                return refuse("ClosedSchema", outcome.error().message());
            }
            if (kind == json::ErrorKind::DocumentRejected)
            {
                return refuse("MalformedWorkflowTool", outcome.error().message());
            }
            return fail(
                AutomationErrorKind::InvalidResource,
                std::string{outcome.error().message()}
            );
        }

        [[nodiscard]]
        auto validateDeclaration(json::Value const& root) -> Status
        {
            auto const published = framework_schema::findFrameworkSchema(
                k_workflowSchemaPath
            );
            if (!published.has_value())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "generated framework schema catalog is missing "
                        + std::string{k_workflowSchemaPath}
                );
            }
            auto compiled = json::Schema::compile(json::Schema::Document{
                .label      = published->relativePath,
                .exactBytes = published->exactBytes,
            });
            if (!compiled.has_value())
            {
                // A schema the catalog holds that does not compile is the
                // same class of defect as one this evaluator refuses: a
                // deployment problem no declaration can fix, mapped like the
                // validate outcome below and reported without a lock code.
                return adoptSchema(std::unexpected<Error>{
                    std::move(compiled.error())
                });
            }
            return adoptSchema(compiled->validate(root));
        }

        // Readers, not validators: the published schema has already accepted
        // the whole document, so every member is present with the type and
        // range the schema declares. The kind dispatch below re-chooses the
        // oneOf branch the schema chose, and the check documents that the
        // dispatch is total rather than silently treating an unknown kind as a
        // ui_action state.
        [[nodiscard]]
        auto parseBounds(json::Value const& value) -> WorkflowBounds
        {
            return WorkflowBounds{
                .maximumStates = static_cast<uint32>(
                    member(value, "maximum_states").number()
                ),
                .maximumSteps = static_cast<uint32>(
                    member(value, "maximum_steps").number()
                ),
                .maximumDispatches = static_cast<uint32>(
                    member(value, "maximum_dispatches").number()
                ),
                .maximumObservations = static_cast<uint32>(
                    member(value, "maximum_observations").number()
                ),
                .maximumWaits = static_cast<uint32>(
                    member(value, "maximum_waits").number()
                ),
                .maximumElapsedMillis = static_cast<uint64>(
                    member(value, "maximum_elapsed_ms").number()
                ),
            };
        }

        [[nodiscard]]
        auto parseState(json::Value const& value) -> WorkflowState
        {
            auto const kind = member(value, "kind").string();
            if (kind == "wait")
            {
                return WorkflowState{
                    .stateKey = std::string{member(value, "state_key").string()},
                    .kind     = StateKind::Wait,
                    .observationBudget = static_cast<uint32>(
                        member(value, "observation_budget").number()
                    ),
                    .timeoutMillis = static_cast<uint64>(
                        member(value, "timeout_ms").number()
                    ),
                };
            }
            UF_CHECK(kind == "ui_action");
            return WorkflowState{
                .stateKey = std::string{member(value, "state_key").string()},
                .kind     = StateKind::UiAction,
                .uiAction = std::string{member(value, "ui_action").string()},
                .timeoutMillis = static_cast<uint64>(
                    member(value, "timeout_ms").number()
                ),
            };
        }

        [[nodiscard]]
        auto parseWorkflowTool(std::string_view declarationBytes) -> Result<WorkflowTool>
        {
            auto parsed = json::parse(declarationBytes);
            if (!parsed.has_value())
            {
                return refuse("MalformedWorkflowTool", "declaration is not JSON");
            }
            auto const& root = *parsed;
            UF_TRY(validateDeclaration(root));

            // The published schema just accepted the document, so every member
            // read below is present with the type and range the schema
            // declares. Only relationships no JSON Schema can state remain
            // checked by hand: state_key uniqueness across the states array,
            // and the schedule naming declared states.
            auto const& fresh   = member(root, "fresh_observation");
            auto const& finding = member(root, "ui_finding");
            auto const& kinds   = member(root, "allowed_instance_kinds");
            auto const& states  = member(root, "states");
            auto const& steps   = member(root, "steps");
            auto const& bounds  = member(root, "bounds");

            auto const toolName       = member(root, "tool_name").string();
            auto const targetArgument = member(root, "target_argument").string();
            auto const requiredSurface = member(fresh, "required_surface").string();
            auto const findingKind     = member(finding, "kind").string();

            auto allowedKinds = std::vector<std::string>{};
            for (auto const& kind : kinds.items())
            {
                allowedKinds.emplace_back(kind.string());
            }
            // The schema's uniqueItems makes the set unique; sorting makes the
            // rendered adapter deterministic.
            std::ranges::sort(allowedKinds);

            auto stateByKey = std::map<std::string, WorkflowState>{};
            for (auto const& stateValue : states.items())
            {
                auto state = parseState(stateValue);
                if (!stateByKey.emplace(state.stateKey, std::move(state)).second)
                {
                    return refuse("MalformedWorkflowTool", "state_key values must be unique");
                }
            }

            auto scheduled = std::vector<WorkflowState>{};
            for (auto const& step : steps.items())
            {
                if (!stateByKey.contains(std::string{step.string()}))
                {
                    return refuse("MalformedWorkflowTool", "step names no declared state");
                }
                scheduled.emplace_back(stateByKey.at(std::string{step.string()}));
            }
            auto const parsedBounds = parseBounds(bounds);

            auto const dispatches = std::ranges::count(
                scheduled,
                StateKind::UiAction,
                &WorkflowState::kind
            );
            auto const waits = std::ranges::count(
                scheduled,
                StateKind::Wait,
                &WorkflowState::kind
            );
            auto elapsed = uint64{};
            for (auto const& state : scheduled)
            {
                elapsed += state.timeoutMillis;
            }
            if (stateByKey.size() > parsedBounds.maximumStates)
            {
                return refuse("WorkflowStateBound", "workflow exceeds maximum_states");
            }
            if (scheduled.size() > parsedBounds.maximumSteps)
            {
                return refuse("WorkflowStepBound", "workflow exceeds maximum_steps");
            }
            if (dispatches > parsedBounds.maximumDispatches)
            {
                return refuse("WorkflowDispatchBound", "workflow exceeds maximum_dispatches");
            }
            if (scheduled.size() > parsedBounds.maximumObservations)
            {
                return refuse("WorkflowObservationBound", "workflow exceeds maximum_observations");
            }
            if (waits > parsedBounds.maximumWaits)
            {
                return refuse("WorkflowWaitBound", "workflow exceeds maximum_waits");
            }
            if (elapsed > parsedBounds.maximumElapsedMillis)
            {
                return refuse("WorkflowElapsedBound", "workflow exceeds maximum_elapsed_ms");
            }
            return WorkflowTool{
                .toolName             = std::string{toolName},
                .targetArgument       = std::string{targetArgument},
                .allowedInstanceKinds = std::move(allowedKinds),
                .requiredSurface      = std::string{requiredSurface},
                .findingKind          = std::string{findingKind},
                .steps                = std::move(scheduled),
                .bounds               = parsedBounds,
            };
        }

        auto appendQuoted(std::string& output, std::string_view value) -> void
        {
            output.push_back('"');
            output += value;
            output.push_back('"');
        }

        // The reducer closure a declared workflow tool is generated into:
        // the whole of the pure program type's contract, which is `plugin_id`
        // and one entry.
        //
        // Nothing of the declaration reaches it. A declaration states a UI
        // schedule; a fold answers a Journal prefix. The two never shared a
        // value, and the five-function module only made them look related by
        // shipping them in one file.
        [[nodiscard]]
        auto renderReducerModule(std::string_view pluginId) -> std::string
        {
            auto source = std::string{"local plugin_id = "};
            appendQuoted(source, pluginId);
            source += R"luau(

return {
    plugin_id = plugin_id,
    reduce = function(_input)
        return canon.emptyObject
    end,
}
)luau";
            return source;
        }

        // The tool closure a declared workflow tool is generated into.
        //
        // It exports its identity and nothing else, because a declaration
        // carries no tool binding: a tool closure's exported entry set is
        // exactly the binding union its deployment states, and an empty union
        // is an explicitly empty closure rather than an absent one. The
        // declaration is still parsed and refused in full; what it has no
        // renderer for is a bound entry, and the declared name is carried here
        // as a comment so the generated bytes still say what they came from.
        [[nodiscard]]
        auto renderToolModule(
            std::string_view pluginId,
            WorkflowTool const& tool
        ) -> std::string
        {
            auto source = std::string{"-- declared workflow tool "};
            appendQuoted(source, tool.toolName);
            source += "\nlocal plugin_id = ";
            appendQuoted(source, pluginId);
            source += R"luau(

return {
    plugin_id = plugin_id,
}
)luau";
            return source;
        }
    }

    auto generateDeclarativeWorkflowAdapter(
        std::string_view pluginId,
        std::string_view declarationBytes
    ) -> Result<DeclarativeWorkflowAdapter>
    {
        if (!isNamespacedIdentifier(pluginId))
        {
            return refuse(
                "MalformedWorkflowTool",
                "generated ProjectPlugin id must be namespaced"
            );
        }
        UF_TRY_VALUE(tool, parseWorkflowTool(declarationBytes));
        return DeclarativeWorkflowAdapter{
            .reducerModule = renderReducerModule(pluginId),
            .toolModule    = renderToolModule(pluginId, tool),
        };
    }
}

#include "declarative-single-step-tool.hpp"

#include <json/schema.hpp>
#include <json/value.hpp>

#include <core/error/result.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::project
{
    namespace
    {
        constexpr auto k_declarativeSingleStepToolSchema = std::string_view{R"json({
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://umbraflow.dev/schema/declarative-single-step-tool/v1",
  "title": "Umbraflow Declarative Single Step Tool v1",
  "type": "object",
  "additionalProperties": false,
  "required": [
    "schema",
    "tool_name",
    "target_argument",
    "allowed_instance_kinds",
    "ui_action",
    "fresh_observation",
    "ui_finding",
    "bounds"
  ],
  "properties": {
    "schema": {
      "const": "umbraflow-declarative-single-step-tool/v1"
    },
    "tool_name": {
      "type": "string",
      "pattern": "^[A-Za-z][A-Za-z0-9_-]*(?:\\.[A-Za-z0-9][A-Za-z0-9_-]*)+$"
    },
    "target_argument": {
      "type": "string",
      "pattern": "^[A-Za-z_][A-Za-z0-9_]{0,63}$"
    },
    "allowed_instance_kinds": {
      "type": "array",
      "minItems": 1,
      "uniqueItems": true,
      "items": {
        "type": "string",
        "pattern": "^[A-Za-z][A-Za-z0-9_-]*(?:\\.[A-Za-z0-9][A-Za-z0-9_-]*)+$"
      }
    },
    "ui_action": {
      "type": "string",
      "pattern": "^[A-Za-z][A-Za-z0-9_-]*(?:\\.[A-Za-z0-9][A-Za-z0-9_-]*)+$"
    },
    "fresh_observation": {
      "type": "object",
      "additionalProperties": false,
      "required": [
        "required_surface",
        "require_unambiguous"
      ],
      "properties": {
        "required_surface": {
          "type": "string",
          "pattern": "^[A-Za-z][A-Za-z0-9_-]*(?:\\.[A-Za-z0-9][A-Za-z0-9_-]*)+$"
        },
        "require_unambiguous": {
          "const": true
        }
      }
    },
    "ui_finding": {
      "type": "object",
      "additionalProperties": false,
      "required": [
        "kind"
      ],
      "properties": {
        "kind": {
          "enum": [
            "observed_instance_absent",
            "observed_instance_present"
          ]
        }
      }
    },
    "bounds": {
      "type": "object",
      "additionalProperties": false,
      "required": [
        "max_dispatches",
        "max_observations",
        "timeout_ms"
      ],
      "properties": {
        "max_dispatches": {
          "const": 1
        },
        "max_observations": {
          "type": "integer",
          "minimum": 1,
          "maximum": 16
        },
        "timeout_ms": {
          "type": "integer",
          "minimum": 1,
          "maximum": 60000
        }
      }
    }
  }
}
)json"};

        constexpr auto k_rootMembers = std::array{
            std::string_view{"schema"},
            std::string_view{"tool_name"},
            std::string_view{"target_argument"},
            std::string_view{"allowed_instance_kinds"},
            std::string_view{"ui_action"},
            std::string_view{"fresh_observation"},
            std::string_view{"ui_finding"},
            std::string_view{"bounds"},
        };
        constexpr auto k_freshObservationMembers = std::array{
            std::string_view{"required_surface"},
            std::string_view{"require_unambiguous"},
        };
        constexpr auto k_uiFindingMembers = std::array{
            std::string_view{"kind"},
        };
        constexpr auto k_boundsMembers = std::array{
            std::string_view{"max_dispatches"},
            std::string_view{"max_observations"},
            std::string_view{"timeout_ms"},
        };

        struct SingleStepTool final
        {
            std::string              toolName{};
            std::string              targetArgument{};
            std::vector<std::string> allowedInstanceKinds{};
            std::string              uiAction{};
            std::string              requiredSurface{};
            std::string              findingKind{};
            std::string              maximumObservations{};
            std::string              timeoutMillis{};
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
        auto isIdentifierTail(char value) noexcept -> bool
        {
            return (
                isAsciiLetter(value)
                || isAsciiDigit(value)
                || value == '_'
                || value == '-'
            );
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
                if (!isIdentifierTail(character))
                {
                    return false;
                }
                if (atSegmentStart && !isAsciiLetter(character) && !isAsciiDigit(character))
                {
                    return false;
                }
                atSegmentStart = false;
            }
            return sawSeparator && !atSegmentStart;
        }

        [[nodiscard]]
        auto isArgumentName(std::string_view value) noexcept -> bool
        {
            if (
                value.empty()
                || value.size() > 64U
                || (!isAsciiLetter(value.front()) && value.front() != '_')
            )
            {
                return false;
            }
            return std::ranges::all_of(
                value.substr(1U),
                [](char character)
                {
                    return (
                        isAsciiLetter(character)
                        || isAsciiDigit(character)
                        || character == '_'
                    );
                }
            );
        }

        template <std::size_t Size>
        [[nodiscard]]
        auto hasUndeclaredMember(
            json::Value const& value,
            std::array<std::string_view, Size> const& allowed
        ) -> bool
        {
            if (value.kind() != json::ValueKind::Object)
            {
                return false;
            }
            return std::ranges::any_of(
                value.members(),
                [&allowed](json::Member const& member)
                {
                    return !std::ranges::contains(allowed, member.first);
                }
            );
        }

        [[nodiscard]]
        auto carriesUndeclaredMember(json::Value const& root) -> bool
        {
            if (hasUndeclaredMember(root, k_rootMembers))
            {
                return true;
            }

            auto const nested = std::array{
                std::pair{
                    root.find("fresh_observation"),
                    std::span<std::string_view const>{
                        k_freshObservationMembers
                    }
                },
                std::pair{
                    root.find("ui_finding"),
                    std::span<std::string_view const>{k_uiFindingMembers}
                },
                std::pair{
                    root.find("bounds"),
                    std::span<std::string_view const>{k_boundsMembers}
                },
            };
            return std::ranges::any_of(
                nested,
                [](auto const& entry)
                {
                    if (
                        entry.first == nullptr
                        || entry.first->kind() != json::ValueKind::Object
                    )
                    {
                        return false;
                    }
                    return std::ranges::any_of(
                        entry.first->members(),
                        [&entry](json::Member const& member)
                        {
                            return !std::ranges::contains(
                                entry.second,
                                member.first
                            );
                        }
                    );
                }
            );
        }

        template <std::size_t Size>
        [[nodiscard]]
        auto missesMember(
            json::Value const& value,
            std::array<std::string_view, Size> const& required
        ) -> bool
        {
            if (value.kind() != json::ValueKind::Object)
            {
                return false;
            }
            return std::ranges::any_of(
                required,
                [&value](std::string_view name)
                {
                    return value.find(name) == nullptr;
                }
            );
        }

        [[nodiscard]]
        auto missesRequiredMember(json::Value const& root) -> bool
        {
            if (missesMember(root, k_rootMembers))
            {
                return true;
            }

            auto const* fresh = root.find("fresh_observation");
            auto const* finding = root.find("ui_finding");
            auto const* bounds = root.find("bounds");
            return (
                fresh != nullptr
                && missesMember(*fresh, k_freshObservationMembers)
            ) || (
                finding != nullptr
                && missesMember(*finding, k_uiFindingMembers)
            ) || (
                bounds != nullptr
                && missesMember(*bounds, k_boundsMembers)
            );
        }

        [[nodiscard]]
        auto isStringMember(
            json::Value const& object,
            std::string_view name
        ) -> bool
        {
            auto const* value = object.find(name);
            return value != nullptr && value->kind() == json::ValueKind::String;
        }

        [[nodiscard]]
        auto isIntegerMember(
            json::Value const& object,
            std::string_view name
        ) -> bool
        {
            auto const* value = object.find(name);
            return (
                value != nullptr
                && value->kind() == json::ValueKind::Number
                && value->isInteger()
            );
        }

        [[nodiscard]]
        auto hasMalformedShape(json::Value const& root) -> bool
        {
            if (root.kind() != json::ValueKind::Object)
            {
                return true;
            }

            auto const* fresh = root.find("fresh_observation");
            auto const* finding = root.find("ui_finding");
            auto const* bounds = root.find("bounds");
            auto const* kinds = root.find("allowed_instance_kinds");
            if (
                fresh == nullptr
                || finding == nullptr
                || bounds == nullptr
                || kinds == nullptr
            )
            {
                return false;
            }
            if (
                fresh->kind() != json::ValueKind::Object
                || finding->kind() != json::ValueKind::Object
                || bounds->kind() != json::ValueKind::Object
                || kinds->kind() != json::ValueKind::Array
            )
            {
                return true;
            }
            if (missesRequiredMember(root))
            {
                return false;
            }

            auto const stringsAreWellFormed = (
                isStringMember(root, "schema")
                && isStringMember(root, "tool_name")
                && isStringMember(root, "target_argument")
                && isStringMember(root, "ui_action")
                && isStringMember(*fresh, "required_surface")
                && isStringMember(*finding, "kind")
            );
            auto const boundsAreIntegers = (
                isIntegerMember(*bounds, "max_dispatches")
                && isIntegerMember(*bounds, "max_observations")
                && isIntegerMember(*bounds, "timeout_ms")
            );
            auto const* unambiguous = fresh->find("require_unambiguous");
            if (
                !stringsAreWellFormed
                || !boundsAreIntegers
                || unambiguous == nullptr
                || unambiguous->kind() != json::ValueKind::Boolean
            )
            {
                return true;
            }

            if (
                root.find("schema")->string()
                    != "umbraflow-declarative-single-step-tool/v1"
                || !isNamespacedIdentifier(root.find("tool_name")->string())
                || !isArgumentName(root.find("target_argument")->string())
                || !isNamespacedIdentifier(root.find("ui_action")->string())
                || !isNamespacedIdentifier(fresh->find("required_surface")->string())
            )
            {
                return true;
            }

            auto names = std::vector<std::string_view>{};
            names.reserve(kinds->items().size());
            for (auto const& kind : kinds->items())
            {
                if (
                    kind.kind() != json::ValueKind::String
                    || !isNamespacedIdentifier(kind.string())
                )
                {
                    return true;
                }
                names.emplace_back(kind.string());
            }
            std::ranges::sort(names);
            if (std::ranges::adjacent_find(names) != names.end())
            {
                return true;
            }

            auto const findingKind = finding->find("kind")->string();
            return (
                findingKind != "observed_instance_absent"
                && findingKind != "observed_instance_present"
            );
        }

        [[nodiscard]]
        auto boundedIntegerText(json::Value const& value) -> std::string
        {
            return std::format("{:.0f}", value.number());
        }

        [[nodiscard]]
        auto classifySchemaRefusal(json::Value const& root)
            -> std::unexpected<Error>
        {
            if (carriesUndeclaredMember(root))
            {
                return refuse(
                    "ClosedSchema",
                    "single-step declaration carries an undeclared member"
                );
            }
            if (hasMalformedShape(root))
            {
                return refuse(
                    "MalformedSingleStepTool",
                    "single-step declaration has a malformed value"
                );
            }
            if (missesRequiredMember(root))
            {
                return refuse(
                    "IncompleteSingleStep",
                    "single-step declaration is missing a required member"
                );
            }

            auto const& kinds = *root.find("allowed_instance_kinds");
            auto const& fresh = *root.find("fresh_observation");
            auto const& bounds = *root.find("bounds");
            if (kinds.items().empty())
            {
                return refuse(
                    "SingleStepInstanceKindsEmpty",
                    "allowed_instance_kinds must contain at least one kind"
                );
            }
            if (bounds.find("max_dispatches")->number() != 1.0)
            {
                return refuse(
                    "SingleStepDispatchBound",
                    "max_dispatches must equal one"
                );
            }

            auto const observations = bounds.find("max_observations")->number();
            if (observations < 1.0 || observations > 16.0)
            {
                return refuse(
                    "SingleStepObservationBound",
                    "max_observations must be between one and sixteen"
                );
            }

            auto const timeout = bounds.find("timeout_ms")->number();
            if (timeout < 1.0 || timeout > 60000.0)
            {
                return refuse(
                    "SingleStepTimeoutBound",
                    "timeout_ms must be between one and sixty thousand"
                );
            }
            if (!fresh.find("require_unambiguous")->boolean())
            {
                return refuse(
                    "AmbiguousObservationAllowed",
                    "require_unambiguous must equal true"
                );
            }
            return refuse(
                "MalformedSingleStepTool",
                "single-step declaration does not satisfy its locked schema"
            );
        }

        [[nodiscard]]
        auto singleStepSchema() -> Result<json::Schema>
        {
            static auto const s_schema = json::Schema::compile(
                json::Schema::Document{
                    .label      = "umbraflow-declarative-single-step-tool-v1",
                    .exactBytes = k_declarativeSingleStepToolSchema,
                }
            );
            if (!s_schema.has_value())
            {
                return std::unexpected{s_schema.error().clone()};
            }
            return *s_schema;
        }

        [[nodiscard]]
        auto parseSingleStepTool(std::string_view declarationBytes)
            -> Result<SingleStepTool>
        {
            auto parsed = json::parse(declarationBytes);
            if (!parsed.has_value())
            {
                return refuse(
                    "MalformedSingleStepTool",
                    "single-step declaration is not a JSON document"
                );
            }

            UF_TRY_VALUE(schema, singleStepSchema());
            auto const validation = schema.validate(*parsed);
            if (!validation.has_value())
            {
                return classifySchemaRefusal(*parsed);
            }

            auto const& fresh = *parsed->find("fresh_observation");
            auto const& finding = *parsed->find("ui_finding");
            auto const& bounds = *parsed->find("bounds");
            auto allowedKinds = std::vector<std::string>{};
            for (auto const& kind : parsed->find("allowed_instance_kinds")->items())
            {
                allowedKinds.emplace_back(kind.string());
            }
            return SingleStepTool{
                .toolName               = std::string{parsed->find("tool_name")->string()},
                .targetArgument         = std::string{parsed->find("target_argument")->string()},
                .allowedInstanceKinds   = std::move(allowedKinds),
                .uiAction               = std::string{parsed->find("ui_action")->string()},
                .requiredSurface        = std::string{fresh.find("required_surface")->string()},
                .findingKind            = std::string{finding.find("kind")->string()},
                .maximumObservations = boundedIntegerText(*bounds.find("max_observations")),
                .timeoutMillis       = boundedIntegerText(*bounds.find("timeout_ms")),
            };
        }

        auto appendQuoted(std::string& output, std::string_view value) -> void
        {
            output += '"';
            output += value;
            output += '"';
        }

        [[nodiscard]]
        auto renderAdapter(
            std::string_view pluginId,
            SingleStepTool const& tool
        ) -> std::string
        {
            auto source = std::string{};
            source += "local plugin_id = ";
            appendQuoted(source, pluginId);
            source += "\nlocal tool_name = ";
            appendQuoted(source, tool.toolName);
            source += "\nlocal target_argument = ";
            appendQuoted(source, tool.targetArgument);
            source += "\nlocal ui_action = ";
            appendQuoted(source, tool.uiAction);
            source += "\nlocal required_surface = ";
            appendQuoted(source, tool.requiredSurface);
            source += "\nlocal finding_kind = ";
            appendQuoted(source, tool.findingKind);
            source += "\nlocal maximum_observations = ";
            source += tool.maximumObservations;
            source += "\nlocal timeout_ms = ";
            source += tool.timeoutMillis;
            source += "\nlocal allowed_instance_kinds = {\n";
            for (auto const& kind : tool.allowedInstanceKinds)
            {
                source += "    [";
                appendQuoted(source, kind);
                source += "] = true,\n";
            }
            source += R"luau(}

local function target_of(input)
    if type(input.canonical_args) ~= "table" then
        error("SingleStepCanonicalArgsMissing: canonical_args must be an object")
    end
    local target = input.canonical_args[target_argument]
    if type(target) ~= "string" or target == "" then
        error("SingleStepTargetMissing: canonical args do not name the observed-instance target")
    end
    return target
end

local function find_target(observation, target)
    if type(observation) ~= "table" or type(observation.observed_instances) ~= "table" then
        error("MalformedObservation: observed_instances must be an array")
    end
    for _, instance in ipairs(observation.observed_instances) do
        if instance.observed_instance_id == target then
            if allowed_instance_kinds[instance.kind] ~= true then
                error("SingleStepTargetKindRejected: target kind is outside allowed_instance_kinds")
            end
            return instance
        end
    end
    return nil
end

local function require_fresh_target(observation, target)
    local instance = find_target(observation, target)
    if instance == nil then
        error("ObservedInstanceStale: target is absent from the fresh observation")
    end
    return instance
end

local function require_fresh_surface(observation)
    if type(observation) ~= "table"
        or type(observation.canonical_opaque_payload) ~= "table"
        or type(observation.canonical_opaque_payload.surface_observations) ~= "table" then
        error("FreshSurfaceUnresolved: fresh observation carries no Surface evidence")
    end
    for _, surface in ipairs(observation.canonical_opaque_payload.surface_observations) do
        if surface.surface_id == required_surface then
            if surface.fresh ~= true then
                error("StaleObservation: required Surface evidence is stale")
            end
            if surface.resolution ~= "resolved" then
                error("FreshSurfaceUnresolved: required Surface is not resolved")
            end
            if surface.unambiguous ~= true then
                error("FreshSurfaceAmbiguous: required Surface is ambiguous")
            end
            return
        end
    end
    error("FreshSurfaceUnresolved: required Surface is absent")
end

local function finding_for(observation, target)
    local present = find_target(observation, target) ~= nil
    local matches = (finding_kind == "observed_instance_present" and present)
        or (finding_kind == "observed_instance_absent" and not present)
    if not matches then
        return {}
    end
    return {
        {
            kind = finding_kind,
            observed_instance_id = target,
        },
    }
end

return {
    plugin_id = plugin_id,
    derive = function(_input)
        return {
            schema = "umbraflow-project-observation-proposal/v1",
            canonical_opaque_payload = { surface_observations = {} },
            project_tool_preconditions = {},
            observed_instance_proposals = {},
        }
    end,
    plan = function(input)
        if input.tool_name ~= tool_name then
            error("SingleStepToolMismatch: plan input names another tool")
        end
        local target = target_of(input)
        require_fresh_target(input.project_observation, target)
        require_fresh_surface(input.project_observation)
        return {
            allowed_ui_actions = { ui_action },
            canonical_args = input.canonical_args,
            effects = {},
            tool_name = tool_name,
            tool_version = input.tool_version,
            workflow_limits = {
                maximum_dispatches = 1,
                maximum_elapsed_ms = timeout_ms,
                maximum_observations = maximum_observations,
                maximum_steps = 1,
                maximum_waits = 0,
            },
        }
    end,
    next_step = function(input)
        local target = target_of(input)
        require_fresh_target(input.project_observation, target)
        require_fresh_surface(input.project_observation)
        return {
            action = {
                action_id = ui_action,
                canonical_parameters = { [target_argument] = target },
                surface_id = required_surface,
                ui_target_id = target,
            },
            binding_variant_constraints = {},
            delivery_class = "delivery_safe",
            expected_ui_postconditions = {},
            required_ui_preconditions = {
                {
                    kind = "fresh_unambiguous_surface",
                    required_surface = required_surface,
                },
            },
            step_key = ui_action,
            timeout_policy = {
                maximum_elapsed_ms = timeout_ms,
                on_timeout = "reconcile",
            },
        }
    end,
    reconcile = function(input)
        local target = target_of(input)
        require_fresh_surface(input.project_observation)
        return {
            disposition = "continue",
            findings = finding_for(input.project_observation, target),
            journal_events = {},
            observed_outcomes = {},
        }
    end,
    reduce = function(_input)
        return canon.emptyObject
    end,
}
)luau";
            return source;
        }
    }

    auto declarativeSingleStepToolSchemaBytes() noexcept -> std::string_view
    {
        return k_declarativeSingleStepToolSchema;
    }

    auto generateDeclarativeSingleStepAdapter(
        std::string_view pluginId,
        std::string_view declarationBytes
    ) -> Result<std::string>
    {
        if (!isNamespacedIdentifier(pluginId))
        {
            return refuse(
                "MalformedSingleStepTool",
                "generated ProjectPlugin id must be namespaced"
            );
        }
        UF_TRY_VALUE(tool, parseSingleStepTool(declarationBytes));
        return renderAdapter(pluginId, tool);
    }
}

#include "tool-catalog.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <json/value.hpp>

#include <operator/tool-descriptor.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::project
{
    namespace
    {
        constexpr auto k_largestExactJsonInteger = uint64{9'007'199'254'740'991};

        // The largest value a WorkflowLimits count member holds, stated where
        // the narrowing happens, the same way the deployment reader states it
        // (project-deployment.cpp k_workflowCountBound).
        constexpr auto k_workflowCountBound = uint64{0xFFFF'FFFF};

        [[nodiscard]]
        auto refuse(std::string message) -> std::unexpected<Error>
        {
            return fail(AutomationErrorKind::InvalidResource, std::move(message));
        }

        [[nodiscard]]
        auto stringValues(std::vector<std::string> values)
            -> Result<std::vector<json::Value>>
        {
            std::ranges::sort(values);
            if (std::ranges::adjacent_find(values) != values.end())
            {
                return refuse("a generated Tool Catalog declaration contains a duplicate name");
            }

            auto rendered = std::vector<json::Value>{};
            rendered.reserve(values.size());
            for (auto& value : values)
            {
                if (value.empty())
                {
                    return refuse("a generated Tool Catalog declaration contains an empty name");
                }
                rendered.emplace_back(json::Value::ofString(std::move(value)));
            }
            return rendered;
        }

        [[nodiscard]]
        auto hashValues(std::vector<ContentHash> hashes)
            -> Result<std::vector<json::Value>>
        {
            std::ranges::sort(
                hashes,
                {},
                [](ContentHash const& hash)
                {
                    return hash.hex();
                }
            );
            auto rendered = std::vector<json::Value>{};
            rendered.reserve(hashes.size());
            auto previous = std::string{};
            for (auto const& hash : hashes)
            {
                auto const hex = hash.hex();
                if (hex == previous)
                {
                    return refuse(
                        "a generated Tool Catalog declaration names one effect "
                        "payload schema more than once"
                    );
                }
                previous = hex;
                rendered.emplace_back(json::Value::ofString(hex));
            }
            return rendered;
        }

        [[nodiscard]]
        auto integerValue(uint64 value, std::string_view name)
            -> Result<json::Value>
        {
            if (value > k_largestExactJsonInteger)
            {
                return refuse(std::format(
                    "a generated Tool Catalog's {} exceeds JSON's exact integer range",
                    name
                ));
            }
            return json::Value::ofNumber(static_cast<double>(value));
        }

        [[nodiscard]]
        auto renderEffectBound(operator_runtime::EffectBound const& bound)
            -> Result<json::Value>
        {
            if (bound.namespacedType.empty() || bound.scopeKind.empty())
            {
                return refuse(
                    "a generated Tool Catalog effect bound contains an empty name"
                );
            }
            return json::Value::ofObject({
                {
                    "maximum_risk",
                    json::Value::ofString(
                        std::string{operator_runtime::riskWireName(bound.maximumRisk)}
                    ),
                },
                {"namespaced_type", json::Value::ofString(bound.namespacedType)},
                {
                    "payload_schema_hash",
                    json::Value::ofString(bound.payloadSchemaHash.hex()),
                },
                {"scope_kind", json::Value::ofString(bound.scopeKind)},
            });
        }

        [[nodiscard]]
        auto renderEffectBounds(
            std::vector<operator_runtime::EffectBound> bounds
        ) -> Result<std::vector<json::Value>>
        {
            std::ranges::sort(
                bounds,
                {},
                [](operator_runtime::EffectBound const& bound)
                {
                    return std::format(
                        "{}\n{}\n{}\n{}",
                        bound.namespacedType,
                        bound.scopeKind,
                        bound.payloadSchemaHash.hex(),
                        operator_runtime::riskWireName(bound.maximumRisk)
                    );
                }
            );
            auto rendered = std::vector<json::Value>{};
            rendered.reserve(bounds.size());
            auto previous = std::string{};
            for (auto const& bound : bounds)
            {
                auto const identity = std::format(
                    "{}\n{}\n{}\n{}",
                    bound.namespacedType,
                    bound.scopeKind,
                    bound.payloadSchemaHash.hex(),
                    operator_runtime::riskWireName(bound.maximumRisk)
                );
                if (identity == previous)
                {
                    return refuse(
                        "a generated Tool Catalog declaration contains a duplicate "
                        "effect bound"
                    );
                }
                previous = identity;
                UF_TRY_VALUE(value, renderEffectBound(bound));
                rendered.emplace_back(std::move(value));
            }
            return rendered;
        }

        [[nodiscard]]
        auto renderTool(DeclaredTool const& tool) -> Result<json::Value>
        {
            if (
                tool.name.empty()
                || tool.argumentSchema.empty()
                || tool.descriptor.toolVersion.empty()
            )
            {
                return refuse(
                    "a generated Tool Catalog tool contains an empty required name"
                );
            }
            auto const& limits = tool.descriptor.limits;
            if (
                limits.maximumSteps == 0U
                || limits.maximumDispatches == 0U
                || limits.maximumObservations == 0U
                || limits.maximumElapsedMillis == 0U
                || tool.descriptor.timeout.maximumElapsedMillis == 0U
            )
            {
                return refuse(
                    "a generated Tool Catalog tool contains a zero positive bound"
                );
            }

            UF_TRY_VALUE(effectBounds, renderEffectBounds(tool.descriptor.effectBounds));
            UF_TRY_VALUE(
                requiredCapabilities,
                stringValues(tool.descriptor.requiredCapabilities)
            );
            UF_TRY_VALUE(uiActionBounds, stringValues(tool.descriptor.uiActionBounds));
            UF_TRY_VALUE(
                maximumElapsed,
                integerValue(limits.maximumElapsedMillis, "maximum_elapsed_ms")
            );
            UF_TRY_VALUE(
                timeoutElapsed,
                integerValue(
                    tool.descriptor.timeout.maximumElapsedMillis,
                    "timeout_policy.maximum_elapsed_ms"
                )
            );

            return json::Value::ofObject({
                {"argument_schema", json::Value::ofString(tool.argumentSchema)},
                {"effect_bounds", json::Value::ofArray(std::move(effectBounds))},
                {
                    "idempotency",
                    json::Value::ofString(std::string{
                        operator_runtime::toolIdempotencyWireName(
                            tool.descriptor.idempotency
                        )
                    }),
                },
                {
                    "mutability",
                    json::Value::ofString(std::string{
                        operator_runtime::toolMutabilityWireName(
                            tool.descriptor.mutability
                        )
                    }),
                },
                {"name", json::Value::ofString(tool.name)},
                {
                    "required_capabilities",
                    json::Value::ofArray(std::move(requiredCapabilities)),
                },
                {
                    "surface",
                    json::Value::ofString(std::string{
                        operator_runtime::toolSurfaceWireName(tool.descriptor.surface)
                    }),
                },
                {
                    "timeout_policy",
                    json::Value::ofObject({
                        {"maximum_elapsed_ms", std::move(timeoutElapsed)},
                        {
                            "on_timeout",
                            json::Value::ofString(std::string{
                                operator_runtime::timeoutActionWireName(
                                    tool.descriptor.timeout.onTimeout
                                )
                            }),
                        },
                    }),
                },
                {
                    "ui_action_bounds",
                    json::Value::ofArray(std::move(uiActionBounds)),
                },
                {"version", json::Value::ofString(tool.descriptor.toolVersion)},
                {
                    "workflow_limits",
                    json::Value::ofObject({
                        {
                            "maximum_dispatches",
                            json::Value::ofNumber(limits.maximumDispatches),
                        },
                        {"maximum_elapsed_ms", std::move(maximumElapsed)},
                        {
                            "maximum_observations",
                            json::Value::ofNumber(limits.maximumObservations),
                        },
                        {
                            "maximum_steps",
                            json::Value::ofNumber(limits.maximumSteps),
                        },
                        {
                            "maximum_waits",
                            json::Value::ofNumber(limits.maximumWaits),
                        },
                    }),
                },
            });
        }

        [[nodiscard]]
        auto renderTools(std::vector<DeclaredTool> tools)
            -> Result<std::vector<json::Value>>
        {
            if (tools.empty())
            {
                return refuse("a generated Tool Catalog requires at least one tool");
            }
            std::ranges::sort(tools, {}, &DeclaredTool::name);
            auto rendered = std::vector<json::Value>{};
            rendered.reserve(tools.size());
            auto previous = std::string{};
            for (auto const& tool : tools)
            {
                if (tool.name == previous)
                {
                    return refuse(std::format(
                        "a generated Tool Catalog declares tool {} more than once",
                        tool.name
                    ));
                }
                previous = tool.name;
                UF_TRY_VALUE(value, renderTool(tool));
                rendered.emplace_back(std::move(value));
            }
            return rendered;
        }

        [[nodiscard]]
        auto validateEffectPayloadClosure(
            ToolCatalogDeclaration const& declaration
        ) -> Status
        {
            auto declared = std::set<std::string>{};
            for (auto const& hash : declaration.effectPayloadSchemaHashes)
            {
                declared.emplace(hash.hex());
            }
            auto bounded = std::set<std::string>{};
            for (auto const& tool : declaration.tools)
            {
                for (auto const& bound : tool.descriptor.effectBounds)
                {
                    bounded.emplace(bound.payloadSchemaHash.hex());
                }
            }
            if (declared != bounded)
            {
                return refuse(
                    "a generated Tool Catalog's effect payload hashes must exactly "
                    "match its declared tools' effect bounds"
                );
            }
            return ok();
        }

        // One member read out of a declared Tool Catalog document. Unlike the
        // project root document, no published schema judged this value before
        // this reader saw it -- the shape authority is the deployment loader's
        // embedded schema, which this module cannot compile -- so a member
        // that is absent or mistyped is a refusal here rather than a contract
        // check.
        [[nodiscard]]
        auto stringMember(
            json::Value const& object,
            std::string_view name,
            std::string_view where
        ) -> Result<std::string_view>
        {
            auto const* const p_member = object.find(name);
            if (p_member == nullptr)
            {
                return refuse(std::format(
                    "{} is missing \"{}\"",
                    where,
                    name
                ));
            }
            if (p_member->kind() != json::ValueKind::String)
            {
                return refuse(std::format(
                    "{} \"{}\" is not a string",
                    where,
                    name
                ));
            }
            return p_member->string();
        }

        // One exact integer member, narrowed. The deployment schema bounds
        // these members from above and a double stops being exact at 2^53, so
        // the ceiling is stated where the narrowing happens: a number the
        // document spelled outside it is refused here rather than silently
        // truncated by a cast.
        [[nodiscard]]
        auto exactCountMember(
            json::Value const& object,
            std::string_view name,
            std::string_view where,
            uint64 ceiling
        ) -> Result<uint64>
        {
            auto const* const p_member = object.find(name);
            if (p_member == nullptr)
            {
                return refuse(std::format(
                    "{} is missing \"{}\"",
                    where,
                    name
                ));
            }
            if (p_member->kind() != json::ValueKind::Number)
            {
                return refuse(std::format(
                    "{} \"{}\" is not a number",
                    where,
                    name
                ));
            }
            auto const number = p_member->number();
            if (
                !p_member->isInteger()
                || number < 0.0
                || number > static_cast<double>(ceiling)
            )
            {
                return refuse(std::format(
                    "{} \"{}\" is not an exact integer in the range 0..{}",
                    where,
                    name,
                    ceiling
                ));
            }
            return static_cast<uint64>(number);
        }

        // One array-of-strings member, read in document order; the generator
        // sorts and deduplicates when it renders.
        [[nodiscard]]
        auto stringArrayMember(
            json::Value const& object,
            std::string_view name,
            std::string_view where
        ) -> Result<std::vector<std::string>>
        {
            auto const* const p_member = object.find(name);
            if (p_member == nullptr)
            {
                return refuse(std::format(
                    "{} is missing \"{}\"",
                    where,
                    name
                ));
            }
            if (p_member->kind() != json::ValueKind::Array)
            {
                return refuse(std::format(
                    "{} \"{}\" is not an array",
                    where,
                    name
                ));
            }
            auto values = std::vector<std::string>{};
            values.reserve(p_member->items().size());
            for (auto const& item : p_member->items())
            {
                if (item.kind() != json::ValueKind::String)
                {
                    return refuse(std::format(
                        "{} \"{}\" holds a non-string entry",
                        where,
                        name
                    ));
                }
                values.emplace_back(item.string());
            }
            return values;
        }

        // One sha256 member. The document spells a hash the way every other
        // published document under schema/ does -- lowercase hex without the
        // prefix -- and the prefix is added where the hash is parsed, as the
        // reader in modules/deployment does.
        [[nodiscard]]
        auto contentHashMember(
            json::Value const& object,
            std::string_view name,
            std::string_view where
        ) -> Result<ContentHash>
        {
            UF_TRY_VALUE(wire, stringMember(object, name, where));
            UF_TRY_VALUE_CONTEXT(
                hash,
                ContentHash::parse(
                    std::string{"sha256:"} + std::string{wire}
                ),
                std::format(
                    "reading \"{}\" of a declared Tool Catalog document",
                    name
                )
            );
            return hash;
        }

        // One enum member, parsed by the inverse of the wire name the operator
        // spells. std::nullopt is the parser's "no enumerator is spelled this
        // way", which is a refusal here rather than a contract check because
        // no schema judged the document first.
        template <typename Enum, typename ParseWireName>
        [[nodiscard]]
        auto wireNameMember(
            json::Value const& object,
            std::string_view name,
            std::string_view where,
            ParseWireName parseWireName
        ) -> Result<Enum>
        {
            UF_TRY_VALUE(wire, stringMember(object, name, where));
            auto const parsed = parseWireName(wire);
            if (!parsed.has_value())
            {
                return refuse(std::format(
                    "{} declares unknown {} \"{}\"",
                    where,
                    name,
                    wire
                ));
            }
            return *parsed;
        }

        // additionalProperties: false, stated where the deployment schema
        // states it. A member this reader does not know would be dropped by
        // the generator and regenerate a document that differs from the one
        // declared, which is the one loss a byte-for-byte check cannot see.
        [[nodiscard]]
        auto refuseUnknownMembers(
            json::Value const& object,
            std::initializer_list<std::string_view> allowed,
            std::string_view where
        ) -> Status
        {
            for (auto const& member : object.members())
            {
                if (std::ranges::contains(allowed, member.first))
                {
                    continue;
                }
                return refuse(std::format(
                    "{} carries unknown member \"{}\"",
                    where,
                    member.first
                ));
            }
            return ok();
        }

        [[nodiscard]]
        auto effectBoundMember(
            json::Value const& bound
        ) -> Result<operator_runtime::EffectBound>
        {
            auto constexpr where = std::string_view{
                "a declared Tool Catalog effect bound"
            };
            UF_TRY(refuseUnknownMembers(
                bound,
                {
                    "maximum_risk",
                    "namespaced_type",
                    "payload_schema_hash",
                    "scope_kind",
                },
                where
            ));
            UF_TRY_VALUE(namespacedType, stringMember(bound, "namespaced_type", where));
            UF_TRY_VALUE(scopeKind, stringMember(bound, "scope_kind", where));
            UF_TRY_VALUE(
                payloadSchemaHash,
                contentHashMember(bound, "payload_schema_hash", where)
            );
            UF_TRY_VALUE(
                maximumRisk,
                wireNameMember<operator_runtime::Risk>(
                    bound,
                    "maximum_risk",
                    where,
                    operator_runtime::parseRisk
                )
            );
            return operator_runtime::EffectBound{
                .namespacedType    = std::string{namespacedType},
                .scopeKind         = std::string{scopeKind},
                .payloadSchemaHash = payloadSchemaHash,
                .maximumRisk       = maximumRisk,
            };
        }

        [[nodiscard]]
        auto effectBoundsMember(
            json::Value const& tool,
            std::string_view where
        ) -> Result<std::vector<operator_runtime::EffectBound>>
        {
            auto const* const p_bounds = tool.find("effect_bounds");
            if (p_bounds == nullptr)
            {
                return refuse(std::format(
                    "{} is missing \"effect_bounds\"",
                    where
                ));
            }
            if (p_bounds->kind() != json::ValueKind::Array)
            {
                return refuse(std::format(
                    "{} \"effect_bounds\" is not an array",
                    where
                ));
            }
            auto bounds = std::vector<operator_runtime::EffectBound>{};
            bounds.reserve(p_bounds->items().size());
            for (auto const& item : p_bounds->items())
            {
                if (item.kind() != json::ValueKind::Object)
                {
                    return refuse(
                        "a declared Tool Catalog effect bound is not an object"
                    );
                }
                UF_TRY_VALUE(bound, effectBoundMember(item));
                bounds.emplace_back(std::move(bound));
            }
            return bounds;
        }

        [[nodiscard]]
        auto timeoutPolicyMember(
            json::Value const& tool,
            std::string_view where
        ) -> Result<operator_runtime::TimeoutPolicy>
        {
            auto const* const p_policy = tool.find("timeout_policy");
            if (p_policy == nullptr)
            {
                return refuse(std::format(
                    "{} is missing \"timeout_policy\"",
                    where
                ));
            }
            if (p_policy->kind() != json::ValueKind::Object)
            {
                return refuse(std::format(
                    "{} \"timeout_policy\" is not an object",
                    where
                ));
            }
            auto constexpr policyWhere = std::string_view{
                "a declared Tool Catalog timeout_policy"
            };
            UF_TRY(refuseUnknownMembers(
                *p_policy,
                {"maximum_elapsed_ms", "on_timeout"},
                policyWhere
            ));
            UF_TRY_VALUE(
                maximumElapsed,
                exactCountMember(
                    *p_policy,
                    "maximum_elapsed_ms",
                    policyWhere,
                    k_largestExactJsonInteger
                )
            );
            UF_TRY_VALUE(
                onTimeout,
                wireNameMember<operator_runtime::TimeoutAction>(
                    *p_policy,
                    "on_timeout",
                    policyWhere,
                    operator_runtime::parseTimeoutAction
                )
            );
            return operator_runtime::TimeoutPolicy{
                .maximumElapsedMillis = maximumElapsed,
                .onTimeout            = onTimeout,
            };
        }

        [[nodiscard]]
        auto workflowLimitsMember(
            json::Value const& tool,
            std::string_view where
        ) -> Result<operator_runtime::WorkflowLimits>
        {
            auto const* const p_limits = tool.find("workflow_limits");
            if (p_limits == nullptr)
            {
                return refuse(std::format(
                    "{} is missing \"workflow_limits\"",
                    where
                ));
            }
            if (p_limits->kind() != json::ValueKind::Object)
            {
                return refuse(std::format(
                    "{} \"workflow_limits\" is not an object",
                    where
                ));
            }
            auto constexpr limitsWhere = std::string_view{
                "a declared Tool Catalog workflow_limits"
            };
            UF_TRY(refuseUnknownMembers(
                *p_limits,
                {
                    "maximum_dispatches",
                    "maximum_elapsed_ms",
                    "maximum_observations",
                    "maximum_steps",
                    "maximum_waits",
                },
                limitsWhere
            ));
            UF_TRY_VALUE(
                maximumSteps,
                exactCountMember(*p_limits, "maximum_steps", limitsWhere, k_workflowCountBound)
            );
            UF_TRY_VALUE(
                maximumDispatches,
                exactCountMember(
                    *p_limits,
                    "maximum_dispatches",
                    limitsWhere,
                    k_workflowCountBound
                )
            );
            UF_TRY_VALUE(
                maximumObservations,
                exactCountMember(
                    *p_limits,
                    "maximum_observations",
                    limitsWhere,
                    k_workflowCountBound
                )
            );
            UF_TRY_VALUE(
                maximumWaits,
                exactCountMember(*p_limits, "maximum_waits", limitsWhere, k_workflowCountBound)
            );
            UF_TRY_VALUE(
                maximumElapsed,
                exactCountMember(
                    *p_limits,
                    "maximum_elapsed_ms",
                    limitsWhere,
                    k_largestExactJsonInteger
                )
            );
            return operator_runtime::WorkflowLimits{
                .maximumSteps         = static_cast<uint32>(maximumSteps),
                .maximumDispatches    = static_cast<uint32>(maximumDispatches),
                .maximumObservations  = static_cast<uint32>(maximumObservations),
                .maximumWaits         = static_cast<uint32>(maximumWaits),
                .maximumElapsedMillis = maximumElapsed,
            };
        }

        [[nodiscard]]
        auto declaredTool(json::Value const& tool) -> Result<DeclaredTool>
        {
            auto constexpr where = std::string_view{
                "a declared Tool Catalog tool"
            };
            UF_TRY(refuseUnknownMembers(
                tool,
                {
                    "argument_schema",
                    "effect_bounds",
                    "idempotency",
                    "mutability",
                    "name",
                    "required_capabilities",
                    "surface",
                    "timeout_policy",
                    "ui_action_bounds",
                    "version",
                    "workflow_limits",
                },
                where
            ));
            UF_TRY_VALUE(name, stringMember(tool, "name", where));
            UF_TRY_VALUE(argumentSchema, stringMember(tool, "argument_schema", where));
            UF_TRY_VALUE(version, stringMember(tool, "version", where));
            UF_TRY_VALUE(
                mutability,
                wireNameMember<operator_runtime::ToolMutability>(
                    tool,
                    "mutability",
                    where,
                    operator_runtime::parseToolMutability
                )
            );
            UF_TRY_VALUE(
                surface,
                wireNameMember<operator_runtime::ToolSurface>(
                    tool,
                    "surface",
                    where,
                    operator_runtime::parseToolSurface
                )
            );
            UF_TRY_VALUE(
                idempotency,
                wireNameMember<operator_runtime::ToolIdempotency>(
                    tool,
                    "idempotency",
                    where,
                    operator_runtime::parseToolIdempotency
                )
            );
            UF_TRY_VALUE(
                requiredCapabilities,
                stringArrayMember(tool, "required_capabilities", where)
            );
            UF_TRY_VALUE(
                uiActionBounds,
                stringArrayMember(tool, "ui_action_bounds", where)
            );
            UF_TRY_VALUE(effectBounds, effectBoundsMember(tool, where));
            UF_TRY_VALUE(timeout, timeoutPolicyMember(tool, where));
            UF_TRY_VALUE(limits, workflowLimitsMember(tool, where));
            return DeclaredTool{
                .name           = std::string{name},
                .argumentSchema = std::string{argumentSchema},
                .descriptor      = operator_runtime::ToolDescriptor{
                    .toolVersion          = std::string{version},
                    .requiredCapabilities = std::move(requiredCapabilities),
                    .effectBounds         = std::move(effectBounds),
                    .uiActionBounds       = std::move(uiActionBounds),
                    .limits               = limits,
                    .timeout              = timeout,
                    .mutability           = mutability,
                    .surface              = surface,
                    .idempotency          = idempotency,
                },
            };
        }
    }

    auto generateToolCatalog(
        ToolCatalogDeclaration const& declaration
    ) -> Result<std::string>
    {
        auto const pluginPath = std::filesystem::path{declaration.pluginId};
        if (
            declaration.pluginId.empty()
            || pluginPath.has_parent_path()
            || pluginPath.filename() != pluginPath
            || declaration.pluginId == "."
            || declaration.pluginId == ".."
        )
        {
            return refuse(
                "a generated Tool Catalog plugin id must be one path component"
            );
        }
        UF_TRY(validateEffectPayloadClosure(declaration));
        UF_TRY_VALUE(
            effectPayloadHashes,
            hashValues(declaration.effectPayloadSchemaHashes)
        );
        UF_TRY_VALUE(tools, renderTools(declaration.tools));

        auto members = std::vector<json::Member>{};
        if (!declaration.comment.empty())
        {
            members.emplace_back(
                "$comment",
                json::Value::ofString(declaration.comment)
            );
        }
        members.emplace_back(
            "effect_payload_sha256s",
            json::Value::ofArray(std::move(effectPayloadHashes))
        );
        members.emplace_back(
            "plugin_id",
            json::Value::ofString(declaration.pluginId)
        );
        members.emplace_back(
            "schema",
            json::Value::ofString("umbraflow-tool-catalog/v1")
        );
        members.emplace_back(
            "tool_precondition_sha256",
            json::Value::ofString(declaration.toolPreconditionSchemaHash.hex())
        );
        members.emplace_back("tools", json::Value::ofArray(std::move(tools)));
        return json::canonicalBytes(json::Value::ofObject(std::move(members)));
    }

    auto parseToolCatalogDeclaration(
        json::Value const& document
    ) -> Result<ToolCatalogDeclaration>
    {
        auto constexpr where = std::string_view{
            "a declared Tool Catalog document"
        };
        UF_TRY(refuseUnknownMembers(
            document,
            {
                "$comment",
                "effect_payload_sha256s",
                "plugin_id",
                "schema",
                "tool_precondition_sha256",
                "tools",
            },
            where
        ));
        UF_TRY_VALUE(schema, stringMember(document, "schema", where));
        if (schema != "umbraflow-tool-catalog/v1")
        {
            return refuse(std::format(
                "{} carries schema \"{}\" where umbraflow-tool-catalog/v1 "
                "is required",
                where,
                schema
            ));
        }
        UF_TRY_VALUE(pluginId, stringMember(document, "plugin_id", where));
        UF_TRY_VALUE(
            toolPrecondition,
            contentHashMember(document, "tool_precondition_sha256", where)
        );

        // $comment is the one optional member, rendered only when the
        // declaration carries one and read back the same way.
        auto comment = std::string{};
        if (auto const* const p_comment = document.find("$comment");
            p_comment != nullptr)
        {
            if (p_comment->kind() != json::ValueKind::String)
            {
                return refuse(std::format(
                    "{} \"$comment\" is not a string",
                    where
                ));
            }
            comment = std::string{p_comment->string()};
        }

        auto const* const p_effectPayloads = document.find("effect_payload_sha256s");
        if (p_effectPayloads == nullptr)
        {
            return refuse(std::format(
                "{} is missing \"effect_payload_sha256s\"",
                where
            ));
        }
        if (p_effectPayloads->kind() != json::ValueKind::Array)
        {
            return refuse(std::format(
                "{} \"effect_payload_sha256s\" is not an array",
                where
            ));
        }
        auto effectPayloadHashes = std::vector<ContentHash>{};
        effectPayloadHashes.reserve(p_effectPayloads->items().size());
        for (auto const& item : p_effectPayloads->items())
        {
            if (item.kind() != json::ValueKind::String)
            {
                return refuse(std::format(
                    "{} \"effect_payload_sha256s\" holds a non-string entry",
                    where
                ));
            }
            UF_TRY_VALUE_CONTEXT(
                hash,
                ContentHash::parse(
                    std::string{"sha256:"} + std::string{item.string()}
                ),
                std::format(
                    "reading \"effect_payload_sha256s\" of a declared Tool "
                    "Catalog document"
                )
            );
            effectPayloadHashes.emplace_back(hash);
        }

        auto const* const p_tools = document.find("tools");
        if (p_tools == nullptr)
        {
            return refuse(std::format("{} is missing \"tools\"", where));
        }
        if (p_tools->kind() != json::ValueKind::Array)
        {
            return refuse(std::format("{} \"tools\" is not an array", where));
        }
        auto tools = std::vector<DeclaredTool>{};
        tools.reserve(p_tools->items().size());
        for (auto const& item : p_tools->items())
        {
            if (item.kind() != json::ValueKind::Object)
            {
                return refuse("a declared Tool Catalog tool is not an object");
            }
            UF_TRY_VALUE(declared, declaredTool(item));
            tools.emplace_back(std::move(declared));
        }

        return ToolCatalogDeclaration{
            .comment                    = std::move(comment),
            .pluginId                   = std::string{pluginId},
            .toolPreconditionSchemaHash = toolPrecondition,
            .effectPayloadSchemaHashes  = std::move(effectPayloadHashes),
            .tools                      = std::move(tools),
        };
    }
}

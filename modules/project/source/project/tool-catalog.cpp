#include "tool-catalog.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <json/value.hpp>

#include <operator/tool-descriptor.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
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
}

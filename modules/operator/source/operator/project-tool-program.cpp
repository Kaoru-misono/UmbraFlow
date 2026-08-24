#include "project-tool-program.hpp"

#include <script/pure-data-program.hpp>
#include <script/scoped-tool-program.hpp>

#include <task/framework-bundle.hpp>

#include <json/value.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::operator_runtime
{
    namespace
    {
        [[nodiscard]]
        auto refuse(std::string message) -> std::unexpected<Error>
        {
            return fail(AutomationErrorKind::InvalidResource, std::move(message));
        }

        // The discovery projection one Tool contributes to the pinned catalog
        // resource the scoped facades read. It carries what discovery needs and
        // stops there: the name a call is spelled by, the version it answers
        // under, the per-call elapsed ceiling any duration a caller states must
        // lie within, and how many children the descriptor admits.
        //
        // That ceiling is the WORKFLOW limit and not the timeout policy's wall
        // clock. The two were one number while nothing enforced the timeout;
        // once ToolRuntimeExecutor judges a returning call against it, the wall
        // clock has to leave room for the work a stated duration names, so a
        // duration bounded by it would make the longest legal call time out.
        //
        // It is deliberately not the whole descriptor. Every remaining bound is
        // ENFORCEMENT data, evaluated by admission on the exact catalog bytes
        // per call; republishing it here would be a second copy of the
        // authority, in a resource, that nothing judges a call against.
        [[nodiscard]]
        auto discoveryEntry(ToolCatalogEntry const& tool) -> json::Value
        {
            auto const argumentContract = json::Value::ofObject({
                {"maximum_duration_ms",
                 json::Value::ofNumber(
                     static_cast<double>(tool.descriptor.limits.maximumElapsedMillis)
                 )},
            });
            auto const childEffects = json::Value::ofObject({
                {"maximum_child_calls",
                 json::Value::ofNumber(
                     static_cast<double>(tool.descriptor.childEffects.maximumChildCalls)
                 )},
            });
            return json::Value::ofObject({
                {"argument_contract", argumentContract},
                {"child_effects", childEffects},
                {"name", json::Value::ofString(tool.name)},
                {"tool_version", json::Value::ofString(tool.descriptor.toolVersion)},
            });
        }

        // Everything one Tool Catalog owner declares, as entries. It is a
        // template over the two owners rather than one function each because
        // the two are the same reading of two authorities that share no base
        // and need none; the instantiation set is these two and is closed here.
        template <typename Catalog>
        [[nodiscard]]
        auto catalogEntries(Catalog const& catalog)
            -> Result<std::vector<ToolCatalogEntry>>
        {
            auto entries = std::vector<ToolCatalogEntry>{};
            for (auto const& name : catalog.toolNames())
            {
                UF_TRY_VALUE(descriptor, catalog.describe(name));
                entries.emplace_back(ToolCatalogEntry{
                    .name       = name,
                    .descriptor = std::move(descriptor),
                });
            }
            return entries;
        }

        [[nodiscard]]
        auto scriptModules(std::vector<ProjectModuleBlob> blobs)
            -> std::vector<script::PureDataProgram::Module>
        {
            auto modules = std::vector<script::PureDataProgram::Module>{};
            modules.reserve(blobs.size());
            for (auto& blob : blobs)
            {
                modules.emplace_back(script::PureDataProgram::Module{
                    .name   = std::move(blob.name),
                    .source = std::move(blob.source),
                });
            }
            return modules;
        }
    } // namespace

    [[nodiscard]]
    auto pinnedToolCatalogResource(
        FrameworkToolCatalogOwner const& frameworkCatalog,
        ProjectToolCatalogSchemaOwner const& projectCatalog
    ) -> Result<script::PureDataProgram::Resource>
    {

        UF_TRY_VALUE(entries, catalogEntries(frameworkCatalog));
        UF_TRY_VALUE(projectEntries, catalogEntries(projectCatalog));
        for (auto& entry : projectEntries)
        {
            entries.emplace_back(std::move(entry));
        }

        // Sorted by name, which is the order @umbraflow/tools requires and
        // refuses by name if it does not get. Uniqueness needs no check
        // here and must not grow one: the Project catalog owner already
        // refuses a repeated name, and every name it admits is inside the
        // namespace its own plugin_id owns -- which a registration inside
        // `framework.` is refused from claiming -- so no Project entry can
        // collide with a Framework one.
        std::ranges::sort(entries, {}, &ToolCatalogEntry::name);

        auto rows = std::vector<json::Value>{};
        rows.reserve(entries.size());
        for (auto const& entry : entries)
        {
            rows.emplace_back(discoveryEntry(entry));
        }
        auto tools               = json::Value::ofArray(std::move(rows));
        auto const exactToolBytes = json::canonicalBytes(tools);
        UF_TRY_VALUE(
            catalogHash,
            sha256(std::as_bytes(std::span{exactToolBytes}))
        );
        return script::PureDataProgram::Resource{
            .kind = script::PureDataProgram::ResourceKind::Json,
            .name = std::string{task::scopedToolCatalogResourceName()},
            .bytes = json::canonicalBytes(json::Value::ofObject({
                {"catalog_hash", json::Value::ofString(catalogHash.hex())},
                {"tools", std::move(tools)},
            })),
        };
    }

    auto currentScopedToolEnvironmentMaterial() -> Result<std::string>
    {
        UF_TRY_VALUE(
            scopedEnvironment,
            json::parse(script::scopedToolEnvironmentMaterial())
        );
        UF_TRY_VALUE(frameworkModules, task::scopedFrameworkScriptModules());
        auto moduleRows = std::vector<json::Value>{};
        moduleRows.reserve(frameworkModules.size());
        for (auto const& module : frameworkModules)
        {
            UF_TRY_VALUE(
                sourceHash,
                sha256(std::as_bytes(std::span{module.source}))
            );
            moduleRows.emplace_back(json::Value::ofObject({
                {"dependency_depth",
                 json::Value::ofNumber(static_cast<double>(module.dependencyDepth))},
                {"name", json::Value::ofString(std::string{module.name})},
                {"project_visible", json::Value::ofBoolean(module.projectVisible)},
                {"source_hash", json::Value::ofString(sourceHash.hex())},
            }));
        }
        return json::canonicalBytes(json::Value::ofObject({
            {"framework_module_budget",
             json::Value::ofString("separate-release-owned-quota-v1")},
            {"framework_module_freeze",
             json::Value::ofString("deep-keys-and-values-v1")},
            {"framework_scoped_modules", json::Value::ofArray(std::move(moduleRows))},
            {"scoped_tool_catalog_resource",
             json::Value::ofString(
                 std::string{task::scopedToolCatalogResourceName()}
             )},
            {"scoped_tool_environment", std::move(scopedEnvironment)},
        }));
    }

    auto currentScopedToolEnvironmentHash() -> Result<ContentHash>
    {
        UF_TRY_VALUE(material, currentScopedToolEnvironmentMaterial());
        return sha256(std::as_bytes(std::span{material}));
    }
} // namespace uf::operator_runtime

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
                     static_cast<double>(tool.descriptor.timeout.maximumElapsedMillis)
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

        // The run's pinned Tool catalog, as the read-only Framework resource
        // @umbraflow/tools reads its discovery table from. It is a resource
        // rather than a native call because discovery must cost no Tool-call
        // budget and must be identical on replay: these bytes are fixed at
        // program construction, so a description cannot move under a running
        // script.
        //
        // It carries BOTH catalogs, because both are callable from a scoped
        // run: the Framework's Tools are how a handler reaches the world, and
        // the Project's own are how one Tool composes another.
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

        [[nodiscard]]
        auto scriptModules(std::vector<ProjectPluginRegistrar::ModuleBlob> blobs)
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

    class ProjectToolProgramHandle::State final
    {
    public:
        VerifiedProjectRegistration   registration;
        ProjectToolCatalogSchemaOwner catalog;
        ProjectToolBindingTable       bindings;
        script::ScopedToolProgram     program;
        ToolResultValidator           validateResults;
        ContentHash                   frameworkToolCatalogHash;
        ContentHash                   environmentIdentity;
    };

    ProjectToolProgramHandle::ProjectToolProgramHandle(
        std::shared_ptr<State const> p_state
    ) noexcept
        : m_state{std::move(p_state)}
    {
    }

    auto ProjectToolProgramHandle::pluginId() const -> std::string
    {
        return m_state->registration.pluginId();
    }

    auto ProjectToolProgramHandle::projectRegistrationHash() const -> ContentHash
    {
        return m_state->registration.hash();
    }

    auto ProjectToolProgramHandle::toolCatalogHash() const -> ContentHash
    {
        return m_state->registration.toolCatalogHash();
    }

    auto ProjectToolProgramHandle::frameworkToolCatalogHash() const -> ContentHash
    {
        return m_state->frameworkToolCatalogHash;
    }

    auto ProjectToolProgramHandle::environmentIdentity() const -> ContentHash
    {
        return m_state->environmentIdentity;
    }

    auto ProjectToolProgramHandle::bindingTable() const noexcept
        -> ProjectToolBindingTable const&
    {
        return m_state->bindings;
    }

    auto ProjectToolProgramHandle::catalog() const noexcept
        -> ProjectToolCatalogSchemaOwner const&
    {
        return m_state->catalog;
    }

    auto ProjectToolProgramHandle::invokeBoundTool(
        std::string_view toolName,
        json::Value const& canonicalArguments,
        script::ScopedRunRequest const& request
    ) const -> Result<json::Value>
    {
        UF_TRY_VALUE(entryPoint, m_state->bindings.entryPointFor(toolName));
        return withContext(
            m_state->program.invoke(entryPoint, canonicalArguments, request),
            "running the Project entry bound to " + std::string{toolName}
        );
    }

    auto ProjectToolProgramHandle::validateToolResult(
        std::string_view toolName,
        std::string_view exactResultJcs
    ) const -> Status
    {
        UF_TRY(m_state->bindings.entryPointFor(toolName));
        return withContext(
            m_state->validateResults(toolName, exactResultJcs),
            "validating the answer of the Project Tool " + std::string{toolName}
        );
    }

    auto ProjectToolProgramRegistrar::registerProject(
        VerifiedProjectRegistration const& registration,
        ProjectToolCatalogSchemaOwner catalog,
        std::string entryModule,
        std::vector<ProjectPluginRegistrar::ModuleBlob> exactModules,
        std::vector<ProjectPluginRegistrar::ResourceBlob> exactResources,
        std::span<std::string const> exportedEntryPoints,
        ToolResultValidator validateResults,
        script::ToolRuntimeInvoke invokeTool
    ) -> Result<ProjectToolProgramHandle>
    {
        if (!validateResults)
        {
            return refuse(
                "Project Tool program requires a result schema validator"
            );
        }
        UF_TRY_VALUE(pureEnvironmentHash, currentProjectPluginEnvironmentHash());
        if (pureEnvironmentHash != registration.pluginEnvironmentHash())
        {
            return refuse(
                "Project environment does not match the verified registration"
            );
        }
        UF_TRY_VALUE(
            moduleManifestHash,
            derivePluginModuleManifestHash(entryModule, exactModules)
        );
        if (moduleManifestHash != registration.pluginModuleManifestHash())
        {
            return refuse(
                "Project module closure does not match the verified registration"
            );
        }

        auto const key = std::pair{registration.pluginId(), registration.hash()};
        if (m_programs.contains(key))
        {
            return refuse("exact Project Tool program registration is immutable");
        }

        // The join, before anything is compiled. It is what makes each of its
        // three refusals reachable from here: an entry no closure exports, a
        // declared Tool nothing binds, and a binding no descriptor covers are
        // all disagreements between the contract and the code, and none of them
        // can be discovered by compiling the code alone.
        UF_TRY_VALUE(
            bindings,
            ProjectToolBindingTable::bind(registration, catalog, exportedEntryPoints)
        );
        auto const entryPoints = bindings.entryPoints();

        UF_TRY_VALUE(
            resources,
            verifyProjectResourceClosure(
                registration.projectResources(),
                std::move(exactResources)
            )
        );
        UF_TRY_VALUE(frameworkCatalog, FrameworkToolCatalogOwner::create());
        UF_TRY_VALUE(
            catalogResource,
            pinnedToolCatalogResource(frameworkCatalog, catalog)
        );
        UF_TRY_VALUE(frameworkModules, task::scopedFrameworkScriptModules());
        UF_TRY_VALUE(environmentIdentity, currentScopedToolEnvironmentHash());

        auto frameworkResources = std::vector<script::PureDataProgram::Resource>{};
        frameworkResources.emplace_back(std::move(catalogResource));

        auto entryPointViews = std::vector<std::string_view>{};
        entryPointViews.reserve(entryPoints.size());
        for (auto const& entryPoint : entryPoints)
        {
            entryPointViews.emplace_back(entryPoint);
        }

        // One program, over the union of bound entries and nothing else. No
        // budget, ceiling or child Tool set reaches this call: those are read
        // from the descriptor per admitted call, so policy can move without
        // moving what this compiles to.
        UF_TRY_VALUE_CONTEXT(
            program,
            script::ScopedToolProgram::compile(
                registration.pluginId(),
                entryModule,
                scriptModules(std::move(exactModules)),
                entryPointViews,
                std::move(resources),
                frameworkModules,
                std::move(frameworkResources),
                std::move(invokeTool)
            ),
            "compiling the Project Tool program for this registration generation"
        );

        auto state = std::make_shared<ProjectToolProgramHandle::State>(
            ProjectToolProgramHandle::State{
                .registration             = registration,
                .catalog                  = std::move(catalog),
                .bindings                 = std::move(bindings),
                .program                  = std::move(program),
                .validateResults          = std::move(validateResults),
                .frameworkToolCatalogHash = frameworkCatalog.toolCatalogHash(),
                .environmentIdentity      = environmentIdentity,
            }
        );
        auto handle = ProjectToolProgramHandle{
            std::shared_ptr<ProjectToolProgramHandle::State const>{std::move(state)}
        };
        auto const [position, inserted] = m_programs.emplace(key, handle);
        if (!inserted)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "Project Tool program registry changed during startup"
            );
        }
        return position->second;
    }

    auto ProjectToolProgramRegistrar::findExact(
        std::string const& pluginId,
        ContentHash projectRegistrationHash
    ) const -> Result<ProjectToolProgramHandle>
    {
        auto const found = m_programs.find(
            std::pair{pluginId, projectRegistrationHash}
        );
        if (found == m_programs.end())
        {
            return refuse("no exact Project Tool program registration is loaded");
        }
        return found->second;
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

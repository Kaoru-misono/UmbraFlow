#include "project-generation.hpp"

#include "project-tool-program.hpp"

#include <script/pure-data-program.hpp>
#include <script/scoped-tool-program.hpp>

#include <task/framework-bundle.hpp>

#include <json/value.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <memory>
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

        // The stated export set, as the bridge takes it. The views name the
        // generation's own strings and outlive nothing: the compile call they
        // are handed to reads them and keeps none.
        [[nodiscard]]
        auto entryPointViews(std::vector<std::string> const& declared)
            -> std::vector<std::string_view>
        {
            auto views = std::vector<std::string_view>{};
            views.reserve(declared.size());
            for (auto const& entryPoint : declared)
            {
                views.emplace_back(entryPoint);
            }
            return views;
        }

        [[nodiscard]]
        auto renderEntryPoints(std::vector<std::string> const& declared)
            -> std::string
        {
            if (declared.empty())
            {
                return "nothing";
            }
            auto rendered = std::string{};
            for (auto const& entryPoint : declared)
            {
                if (!rendered.empty())
                {
                    rendered += ", ";
                }
                rendered += entryPoint;
            }
            return rendered;
        }

        // The reducer's leg of the join. Its expected set is a constant,
        // because the pure program type keeps exactly one entry, and that is
        // what makes this a check on the DOCUMENT rather than on the code: a
        // generation may state anything here and this is what refuses it.
        [[nodiscard]]
        auto joinReducerDeclaration(
            ProjectClosureClaims const& reducerClosure
        ) -> Status
        {
            auto const expected = std::vector<std::string>{
                std::string{k_reducerEntryPoint},
            };
            if (reducerClosure.exportedEntryPoints != expected)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "the reducer closure states it exports "
                        + renderEntryPoints(reducerClosure.exportedEntryPoints)
                        + ", and a generation's reducer closure exports "
                          "exactly "
                        + std::string{k_reducerEntryPoint}
                );
            }
            return ok();
        }

        // The tool closure's leg of the join, refused from both sides. An
        // understatement leaves a bound Tool with no entry to run; an
        // overstatement leaves an entry nothing declared, which the bridge
        // would then compile into a reachable handler no Tool name addresses.
        //
        // The two sources stay apart here on purpose. Deriving the declaration
        // from the binding table would make this compare the table with
        // itself, and the refusals below could never fire.
        [[nodiscard]]
        auto joinToolDeclaration(
            ProjectClosureClaims const& toolClosure,
            std::vector<ProjectToolBinding> const& bindings
        ) -> Status
        {
            for (auto const& binding : bindings)
            {
                if (
                    !std::ranges::contains(
                        toolClosure.exportedEntryPoints,
                        binding.entryPoint
                    )
                )
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "Project Tool " + binding.toolName
                            + " is bound to entry " + binding.entryPoint
                            + ", which the tool closure does not declare"
                    );
                }
            }
            for (auto const& entryPoint : toolClosure.exportedEntryPoints)
            {
                if (
                    !std::ranges::contains(
                        bindings,
                        entryPoint,
                        &ProjectToolBinding::entryPoint
                    )
                )
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "the tool closure declares entry " + entryPoint
                            + ", which no Tool of this generation binds"
                    );
                }
            }
            return ok();
        }
    } // namespace

    class ProjectGenerationHandle::State final
    {
    public:
        VerifiedProjectGeneration     generation;
        ProjectToolCatalogSchemaOwner catalog;
        ProjectToolBindingTable       bindings;
        ProjectSchemaOwner            schemaOwner;
        script::PureDataProgram       reducer;
        script::ScopedToolProgram     toolProgram;
        ToolResultValidator           validateResults;
        ContentHash                   frameworkToolCatalogHash;
        ContentHash                   environmentIdentity;
    };

    ProjectGenerationHandle::ProjectGenerationHandle(
        std::shared_ptr<State const> p_state
    ) noexcept
        : m_state{std::move(p_state)}
    {
    }

    auto ProjectGenerationHandle::pluginId() const -> std::string
    {
        return m_state->generation.pluginId();
    }

    auto ProjectGenerationHandle::projectRegistrationHash() const -> ContentHash
    {
        return m_state->generation.hash();
    }

    auto ProjectGenerationHandle::reducerModuleManifestHash() const -> ContentHash
    {
        return m_state->generation.reducerClosure().moduleManifestHash;
    }

    auto ProjectGenerationHandle::toolModuleManifestHash() const -> ContentHash
    {
        return m_state->generation.toolClosure().moduleManifestHash;
    }

    auto ProjectGenerationHandle::frameworkToolCatalogHash() const -> ContentHash
    {
        return m_state->frameworkToolCatalogHash;
    }

    auto ProjectGenerationHandle::environmentIdentity() const -> ContentHash
    {
        return m_state->environmentIdentity;
    }

    auto ProjectGenerationHandle::bindingTable() const noexcept
        -> ProjectToolBindingTable const&
    {
        return m_state->bindings;
    }

    auto ProjectGenerationHandle::catalog() const noexcept
        -> ProjectToolCatalogSchemaOwner const&
    {
        return m_state->catalog;
    }

    auto ProjectGenerationHandle::canonicalize(std::string exactJcs) const
        -> Result<CanonicalJson>
    {
        return m_state->schemaOwner.canonicalize(std::move(exactJcs));
    }

    auto ProjectGenerationHandle::reduce(
        CanonicalJson const& input
    ) const -> Result<ValidatedDocument>
    {
        // Validation is repeated at the call boundary because a CanonicalJson
        // carries no schema authority: the value the fold runs on must be the
        // one this owner produced rather than one a caller cached.
        UF_TRY_VALUE(
            validatedInput,
            m_state->schemaOwner.validate(
                ProjectDocumentDirection::Input,
                input
            )
        );
        UF_TRY_VALUE_CONTEXT(
            folded,
            m_state->reducer.invoke(k_reducerEntryPoint, validatedInput),
            "running the reducer of this Project registration generation"
        );
        UF_TRY_VALUE(
            canonicalFold,
            m_state->schemaOwner.canonicalizeValue(std::move(folded))
        );
        return m_state->schemaOwner.validateOutput(std::move(canonicalFold));
    }

    auto ProjectGenerationHandle::invokeBoundTool(
        std::string_view toolName,
        json::Value const& canonicalArguments,
        script::ScopedRunRequest const& request
    ) const -> Result<json::Value>
    {
        UF_TRY_VALUE(entryPoint, m_state->bindings.entryPointFor(toolName));
        return withContext(
            m_state->toolProgram.invoke(
                entryPoint,
                canonicalArguments,
                request
            ),
            "running the Project entry bound to " + std::string{toolName}
        );
    }

    auto ProjectGenerationHandle::validateToolResult(
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

    ProjectBaselineReducer::ProjectBaselineReducer(
        ProjectGenerationHandle const& generation
    )
        : m_projectRegistrationHash{generation.projectRegistrationHash()}
        , m_canonicalize{
              [generation](std::string exactJcs) -> Result<CanonicalJson>
              {
                  return generation.canonicalize(std::move(exactJcs));
              }
          }
        , m_fold{
              [generation](CanonicalJson const& input) -> Result<ValidatedDocument>
              {
                  return generation.reduce(input);
              }
          }
    {
    }

    auto ProjectBaselineReducer::projectRegistrationHash() const -> ContentHash
    {
        return m_projectRegistrationHash;
    }

    auto ProjectBaselineReducer::canonicalize(std::string exactJcs) const
        -> Result<CanonicalJson>
    {
        return m_canonicalize(std::move(exactJcs));
    }

    auto ProjectBaselineReducer::reduce(
        CanonicalJson const& input
    ) const -> Result<ValidatedDocument>
    {
        return m_fold(input);
    }

    auto ProjectGenerationRegistrar::registerGeneration(
        VerifiedProjectGeneration const& generation,
        ProjectToolCatalogSchemaOwner catalog,
        ProjectSchemaOwner schemaOwner,
        ClosureModules reducerClosure,
        ClosureModules toolClosure,
        std::vector<ProjectResourceBlob> exactResources,
        ToolResultValidator validateResults,
        script::ToolRuntimeInvoke invokeTool
    ) -> Result<ProjectGenerationHandle>
    {
        if (!validateResults)
        {
            return refuse(
                "a Project generation requires a result schema validator"
            );
        }
        if (schemaOwner.projectRegistrationHash() != generation.hash())
        {
            return refuse(
                "the schema owner answers for another Project registration"
            );
        }
        UF_TRY_VALUE(pureEnvironmentHash, currentProjectPluginEnvironmentHash());
        if (pureEnvironmentHash != generation.pluginEnvironmentHash())
        {
            return refuse(
                "Project environment does not match the verified generation"
            );
        }

        UF_TRY_VALUE(
            reducerManifestHash,
            derivePluginModuleManifestHash(
                reducerClosure.entryModule,
                reducerClosure.modules
            )
        );
        if (reducerManifestHash != generation.reducerClosure().moduleManifestHash)
        {
            return refuse(
                "the reducer closure does not match the verified generation"
            );
        }
        UF_TRY_VALUE(
            toolManifestHash,
            derivePluginModuleManifestHash(
                toolClosure.entryModule,
                toolClosure.modules
            )
        );
        if (toolManifestHash != generation.toolClosure().moduleManifestHash)
        {
            return refuse(
                "the tool closure does not match the verified generation"
            );
        }

        auto const key = std::pair{generation.pluginId(), generation.hash()};
        if (m_generations.contains(key))
        {
            return refuse("exact Project generation registration is immutable");
        }

        // The join, before anything is compiled, and once per closure. Both
        // refusals below are about the DOCUMENT: what it claims each closure
        // offers against what a generation's closure of that kind may offer.
        // What the closures actually export is the bridge's question, asked
        // when each is compiled against the very set checked here.
        UF_TRY(joinReducerDeclaration(generation.reducerClosure()));
        UF_TRY(joinToolDeclaration(
            generation.toolClosure(),
            generation.projectToolBindings()
        ));

        // The other join, against the authority that DECLARES the Tools. The
        // one above holds the binding table to the code; this holds it to the
        // contract, and neither can stand in for the other: a declaration and
        // a catalog are two documents, and a Tool declared with no binding is
        // invisible to the first check while a binding for a Tool no catalog
        // declares is invisible to it too.
        UF_TRY_VALUE(
            bindings,
            ProjectToolBindingTable::bind(
                generation,
                catalog,
                generation.toolClosure().exportedEntryPoints
            )
        );

        UF_TRY_VALUE(
            reducerResources,
            verifyProjectResourceClosure(
                generation.projectResources(),
                std::move(exactResources)
            )
        );
        auto toolResources = reducerResources;

        UF_TRY_VALUE(pureFrameworkModules, task::pureFrameworkScriptModules());
        UF_TRY_VALUE(scopedFrameworkModules, task::scopedFrameworkScriptModules());
        UF_TRY_VALUE(frameworkCatalog, FrameworkToolCatalogOwner::create());
        UF_TRY_VALUE(
            catalogResource,
            pinnedToolCatalogResource(frameworkCatalog, catalog)
        );
        UF_TRY_VALUE(environmentIdentity, currentScopedToolEnvironmentHash());

        auto frameworkResources = std::vector<script::PureDataProgram::Resource>{};
        frameworkResources.emplace_back(std::move(catalogResource));

        auto const reducerEntries = entryPointViews(
            generation.reducerClosure().exportedEntryPoints
        );
        UF_TRY_VALUE_CONTEXT(
            reducer,
            script::PureDataProgram::compile(
                generation.pluginId(),
                reducerClosure.entryModule,
                scriptModules(std::move(reducerClosure.modules)),
                reducerEntries,
                std::move(reducerResources),
                pureFrameworkModules
            ),
            "compiling the reducer closure of this registration generation"
        );

        auto const toolEntries = entryPointViews(
            generation.toolClosure().exportedEntryPoints
        );
        UF_TRY_VALUE_CONTEXT(
            toolProgram,
            script::ScopedToolProgram::compile(
                generation.pluginId(),
                toolClosure.entryModule,
                scriptModules(std::move(toolClosure.modules)),
                toolEntries,
                std::move(toolResources),
                scopedFrameworkModules,
                std::move(frameworkResources),
                std::move(invokeTool)
            ),
            "compiling the tool closure of this registration generation"
        );

        auto state = std::make_shared<ProjectGenerationHandle::State>(
            ProjectGenerationHandle::State{
                .generation               = generation,
                .catalog                  = std::move(catalog),
                .bindings                 = std::move(bindings),
                .schemaOwner              = std::move(schemaOwner),
                .reducer                  = std::move(reducer),
                .toolProgram              = std::move(toolProgram),
                .validateResults          = std::move(validateResults),
                .frameworkToolCatalogHash = frameworkCatalog.toolCatalogHash(),
                .environmentIdentity      = environmentIdentity,
            }
        );
        auto handle = ProjectGenerationHandle{
            std::shared_ptr<ProjectGenerationHandle::State const>{std::move(state)}
        };

        // The key was proved absent above and nothing between here and there
        // touches the map, so the insertion is the one that happens and there
        // is no second outcome for a branch to describe.
        return m_generations.emplace(key, std::move(handle)).first->second;
    }

    auto ProjectGenerationRegistrar::findExact(
        std::string const& pluginId,
        ContentHash projectRegistrationHash
    ) const -> Result<ProjectGenerationHandle>
    {
        auto const found = m_generations.find(
            std::pair{pluginId, projectRegistrationHash}
        );
        if (found == m_generations.end())
        {
            return refuse("no exact Project generation registration is loaded");
        }
        return found->second;
    }
} // namespace uf::operator_runtime

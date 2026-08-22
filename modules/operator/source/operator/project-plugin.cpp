#include "project-plugin.hpp"

#include <script/pure-data-program.hpp>

#include <task/framework-bundle.hpp>

#include <json/value.hpp>

#include <core/error/contracts.hpp>
#include <core/text/utf8.hpp>

#include <domain/error.hpp>

#include <array>
#include <algorithm>
#include <cstddef>
#include <map>
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
        constexpr auto k_maximumCanonicalBytes = std::size_t{1024U} * 1024U;

        constexpr auto k_functions = std::array{
            std::pair{ProjectPluginFunction::Derive, std::string_view{"derive"}},
            std::pair{ProjectPluginFunction::Plan, std::string_view{"plan"}},
            std::pair{ProjectPluginFunction::NextStep, std::string_view{"next_step"}},
            std::pair{ProjectPluginFunction::Reconcile, std::string_view{"reconcile"}},
            std::pair{ProjectPluginFunction::Reduce, std::string_view{"reduce"}},
        };

        constexpr auto k_entryPoints = std::array{
            std::string_view{"derive"},
            std::string_view{"plan"},
            std::string_view{"next_step"},
            std::string_view{"reconcile"},
            std::string_view{"reduce"},
        };

        [[nodiscard]]
        auto refuse(std::string message) -> std::unexpected<Error>
        {
            return fail(AutomationErrorKind::InvalidResource, std::move(message));
        }

        [[nodiscard]]
        auto functionName(ProjectPluginFunction function) -> std::string_view
        {
            auto const found = std::ranges::find_if(
                k_functions,
                [function](auto const& entry)
                {
                    return entry.first == function;
                }
            );
            if (found != k_functions.end())
            {
                return found->second;
            }
            UF_UNREACHABLE_MSG("unknown ProjectPluginFunction");
        }

        [[nodiscard]]
        auto resourceKind(ProjectResourceKind kind)
            -> script::PureDataProgram::ResourceKind
        {
            switch (kind)
            {
            case ProjectResourceKind::Json:
                return script::PureDataProgram::ResourceKind::Json;
            case ProjectResourceKind::Utf8:
                return script::PureDataProgram::ResourceKind::Utf8;
            case ProjectResourceKind::Bytes:
                return script::PureDataProgram::ResourceKind::Bytes;
            }
            UF_UNREACHABLE_MSG("unknown ProjectResourceKind");
        }
    } // namespace

    auto verifyProjectResourceClosure(
        std::span<ProjectResource const> pinnedResources,
        std::vector<ProjectResourceBlob> exactBlobs
    ) -> Result<std::vector<script::PureDataProgram::Resource>>
    {
        UF_TRY(validateProjectResourceClosure(exactBlobs));
        if (
            pinnedResources.size() > script::PureDataProgram::k_maximumResourceCount
            || exactBlobs.size() > script::PureDataProgram::k_maximumResourceCount
        )
        {
            return refuse("Project resource count exceeds its ceiling");
        }

        auto blobsByName = std::map<
            std::string,
            std::pair<ProjectResourceKind, std::string>
        >{};
        auto totalBytes = std::size_t{0};
        for (auto& blob : exactBlobs)
        {
            if (blob.bytes.size() > script::PureDataProgram::k_maximumResourceBytes)
            {
                return refuse("Project resource exceeds its byte ceiling");
            }
            if (
                totalBytes
                > script::PureDataProgram::k_maximumResourceClosureBytes
                    - blob.bytes.size()
            )
            {
                return refuse("Project resources exceed their total ceiling");
            }
            totalBytes += blob.bytes.size();

            bool const inserted = blobsByName.try_emplace(
                std::move(blob.name),
                blob.kind,
                std::move(blob.bytes)
            ).second;
            if (!inserted)
            {
                return refuse("Project resource names must be unique");
            }
        }

        for (auto const& blob : blobsByName)
        {
            if (
                std::ranges::find(
                    pinnedResources,
                    blob.first,
                    &ProjectResource::name
                )
                == pinnedResources.end()
            )
            {
                return refuse("Project closure carries an unregistered resource");
            }
        }
        auto verified = std::vector<script::PureDataProgram::Resource>{};
        verified.reserve(pinnedResources.size());
        for (auto const& resource : pinnedResources)
        {
            auto const found = blobsByName.find(resource.name);
            if (found == blobsByName.end())
            {
                return refuse(
                    "Project resource is missing for registered name '"
                    + resource.name
                    + "'"
                );
            }
            if (found->second.first != resource.kind)
            {
                return refuse("Project resource kind does not match its registration");
            }
            if (found->second.second.size() != resource.size)
            {
                return refuse("Project resource size does not match its registration");
            }
            UF_TRY_VALUE(
                actualHash,
                sha256(std::as_bytes(std::span{found->second.second}))
            );
            if (actualHash != resource.hash)
            {
                return refuse("Project resource bytes do not match their registration");
            }
            verified.emplace_back(script::PureDataProgram::Resource{
                .kind = resourceKind(resource.kind),
                .name = resource.name,
                .bytes = std::move(found->second.second),
            });
        }
        return verified;
    }

    class ProjectSchemaOwner::State final
    {
    public:
        ContentHash              projectRegistrationHash;
        CanonicalJsonValidator   validateCanonicalJson{};
        ProjectDocumentValidator validateDocument{};
    };

    CanonicalJson::CanonicalJson(
        ContentHash contentHash,
        std::string bytes,
        json::Value value
    )
        : m_contentHash{contentHash}
        , m_bytes{std::move(bytes)}
        , m_value{std::move(value)}
    {
    }

    auto CanonicalJson::parseExact(std::string exactJcs) -> Result<CanonicalJson>
    {
        if (
            exactJcs.empty()
            || exactJcs.size() > k_maximumCanonicalBytes
            || !isValidUtf8(exactJcs)
        )
        {
            return refuse("canonical JSON must be non-empty bounded UTF-8");
        }
        UF_TRY_VALUE_CONTEXT(
            value,
            json::parse(exactJcs),
            "parsing canonical JSON"
        );
        if (json::canonicalBytes(value) != exactJcs)
        {
            return refuse("canonical JSON must be exact RFC 8785 JCS");
        }
        UF_TRY_VALUE(contentHash, sha256(std::as_bytes(std::span{exactJcs})));
        return CanonicalJson{contentHash, std::move(exactJcs), std::move(value)};
    }

    auto CanonicalJson::contentHash() const -> ContentHash
    {
        return m_contentHash;
    }

    auto CanonicalJson::bytes() const noexcept -> std::string const&
    {
        return m_bytes;
    }

    auto CanonicalJson::value() const noexcept -> json::Value const&
    {
        return m_value;
    }

    ValidatedDocument::ValidatedDocument(
        ContentHash projectRegistrationHash,
        ProjectPluginFunction function,
        ProjectDocumentDirection direction,
        CanonicalJson canonicalJson
    )
        : m_projectRegistrationHash{projectRegistrationHash}
        , m_function{function}
        , m_direction{direction}
        , m_canonicalJson{std::move(canonicalJson)}
    {
    }

    auto ValidatedDocument::projectRegistrationHash() const -> ContentHash
    {
        return m_projectRegistrationHash;
    }

    auto ValidatedDocument::function() const noexcept -> ProjectPluginFunction
    {
        return m_function;
    }

    auto ValidatedDocument::direction() const noexcept -> ProjectDocumentDirection
    {
        return m_direction;
    }

    auto ValidatedDocument::contentHash() const -> ContentHash
    {
        return m_canonicalJson.contentHash();
    }

    auto ValidatedDocument::bytes() const noexcept -> std::string const&
    {
        return m_canonicalJson.bytes();
    }

    auto ValidatedDocument::value() const noexcept -> json::Value const&
    {
        return m_canonicalJson.value();
    }

    ProjectSchemaOwner::ProjectSchemaOwner(std::shared_ptr<State const> p_state) noexcept
        : m_state{std::move(p_state)}
    {
    }

    auto ProjectSchemaOwner::create(
        ProjectIdentity const& project,
        ProjectDocumentSchemaBytes const& exactSchemas,
        CanonicalJsonValidator validateCanonicalJson,
        ProjectDocumentValidator validateDocument
    )
        -> Result<ProjectSchemaOwner>
    {
        if (!validateCanonicalJson || !validateDocument)
        {
            return refuse("ProjectSchemaOwner requires canonical and document validators");
        }
        auto const pinned = std::array{
            std::pair{exactSchemas.projectState, project.projectStateSchemaHash()},
            std::pair{
                exactSchemas.projectObservation,
                project.projectObservationSchemaHash(),
            },
            std::pair{
                exactSchemas.toolPrecondition,
                project.projectToolPreconditionSchemaHash(),
            },
        };
        for (auto const& [bytes, expected] : pinned)
        {
            UF_TRY_VALUE(actual, sha256(std::as_bytes(std::span{bytes})));
            if (actual != expected)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "ProjectSchemaOwner bytes do not match a hash this registration pinned"
                );
            }
        }
        auto state = std::make_shared<State>(State{
            .projectRegistrationHash = project.hash(),
            .validateCanonicalJson   = std::move(validateCanonicalJson),
            .validateDocument        = std::move(validateDocument),
        });
        return ProjectSchemaOwner{std::shared_ptr<State const>{std::move(state)}};
    }

    auto ProjectSchemaOwner::canonicalize(std::string exactJcs) const -> Result<CanonicalJson>
    {
        if (exactJcs.empty() || exactJcs.size() > k_maximumCanonicalBytes || !isValidUtf8(exactJcs))
        {
            return refuse("canonical JSON must be non-empty bounded UTF-8");
        }
        UF_TRY_VALUE_CONTEXT(
            value,
            m_state->validateCanonicalJson(exactJcs),
            "verifying exact RFC 8785 JCS"
        );
        UF_TRY_VALUE(contentHash, sha256(std::as_bytes(std::span{exactJcs})));
        return CanonicalJson{contentHash, std::move(exactJcs), std::move(value)};
    }

    auto ProjectSchemaOwner::canonicalizeValue(json::Value value) const -> Result<CanonicalJson>
    {
        auto exactJcs = json::canonicalBytes(value);
        if (exactJcs.empty() || exactJcs.size() > k_maximumCanonicalBytes)
        {
            return refuse("canonical JSON must be non-empty bounded UTF-8");
        }
        UF_TRY_VALUE(contentHash, sha256(std::as_bytes(std::span{exactJcs})));
        return CanonicalJson{contentHash, std::move(exactJcs), std::move(value)};
    }

    auto ProjectSchemaOwner::validate(
        ProjectPluginFunction function,
        ProjectDocumentDirection direction,
        CanonicalJson const& document
    ) const -> Result<json::Value>
    {
        // CanonicalJson carries no owner identity. Re-parsing here therefore
        // produces both the canonicality proof and the sole value execution may
        // consume; using document.value() would validate these bytes while
        // executing a value cached by some other, potentially laxer, owner.
        UF_TRY_VALUE_CONTEXT(
            validatedValue,
            m_state->validateCanonicalJson(document.bytes()),
            "revalidating exact JCS at the ProjectPlugin call boundary"
        );
        UF_TRY_CONTEXT(
            m_state->validateDocument(function, direction, document.bytes()),
            "validating complete ProjectPlugin document schema"
        );
        return validatedValue;
    }

    auto ProjectSchemaOwner::validateOutput(
        ProjectPluginFunction function,
        CanonicalJson document
    ) const
        -> Result<ValidatedDocument>
    {
        UF_TRY_VALUE(
            validatedValue,
            validate(function, ProjectDocumentDirection::Output, document)
        );
        document.m_value = std::move(validatedValue);
        return ValidatedDocument{
            m_state->projectRegistrationHash,
            function,
            ProjectDocumentDirection::Output,
            std::move(document),
        };
    }

    auto ProjectSchemaOwner::projectRegistrationHash() const -> ContentHash
    {
        return m_state->projectRegistrationHash;
    }

    auto derivePluginModuleManifestHash(
        std::string_view entryModule,
        std::span<ProjectModuleBlob const> modules
    ) -> Result<ContentHash>
    {
        auto admittedModules = std::vector<script::PureDataProgram::Module>{};
        admittedModules.reserve(modules.size());
        for (auto const& module : modules)
        {
            admittedModules.emplace_back(script::PureDataProgram::Module{
                .name   = module.name,
                .source = module.source,
            });
        }
        UF_TRY(script::PureDataProgram::validateModuleClosure(
            entryModule,
            admittedModules
        ));

        auto rows = std::vector<std::pair<std::string, ContentHash>>{};
        rows.reserve(modules.size());
        auto entryFound = false;
        for (auto const& module : modules)
        {
            UF_TRY_VALUE(sourceHash, sha256(std::as_bytes(std::span{module.source})));
            entryFound = entryFound || module.name == entryModule;
            rows.emplace_back(module.name, sourceHash);
        }
        std::ranges::sort(rows, {}, &std::pair<std::string, ContentHash>::first);
        for (auto index = std::size_t{1U}; index < rows.size(); ++index)
        {
            if (rows[index - 1U].first == rows[index].first)
            {
                return refuse("ProjectPlugin module names must be unique");
            }
        }
        if (!entryFound)
        {
            return refuse("ProjectPlugin entry module is not present in the closure");
        }

        auto manifestRows = std::vector<json::Value>{};
        manifestRows.reserve(rows.size());
        for (auto const& [name, sourceHash] : rows)
        {
            manifestRows.emplace_back(json::Value::ofObject({
                {"name", json::Value::ofString(name)},
                {"sha256", json::Value::ofString(sourceHash.hex())},
            }));
        }
        auto const manifest = json::canonicalBytes(json::Value::ofObject({
            {"entry", json::Value::ofString(std::string{entryModule})},
            {"modules", json::Value::ofArray(std::move(manifestRows))},
        }));
        return sha256(std::as_bytes(std::span{manifest}));
    }

    auto validateProjectResourceClosure(
        std::span<ProjectResourceBlob const> resources
    ) -> Status
    {
        auto admittedResources = std::vector<script::PureDataProgram::Resource>{};
        admittedResources.reserve(resources.size());
        for (auto const& resource : resources)
        {
            admittedResources.emplace_back(script::PureDataProgram::Resource{
                .kind  = resourceKind(resource.kind),
                .name  = resource.name,
                .bytes = resource.bytes,
            });
        }
        return script::PureDataProgram::validateResourceClosure(admittedResources);
    }

    auto currentProjectPluginEnvironmentMaterial() -> Result<std::string>
    {
        UF_TRY_VALUE(
            pureDataEnvironment,
            json::parse(script::pluginEnvironmentMaterial())
        );
        UF_TRY_VALUE(frameworkModules, task::pureFrameworkScriptModules());
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
            {"framework_pure_modules",
             json::Value::ofArray(std::move(moduleRows))},
            {"pure_data_environment", std::move(pureDataEnvironment)},
        }));
    }

    auto currentProjectPluginEnvironmentHash() -> Result<ContentHash>
    {
        UF_TRY_VALUE(material, currentProjectPluginEnvironmentMaterial());
        return sha256(std::as_bytes(std::span{material}));
    }
} // namespace uf::operator_runtime

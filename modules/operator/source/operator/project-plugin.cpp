#include "project-plugin.hpp"

#include <script/pure-data-program.hpp>

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

        [[nodiscard]]
        auto verifyResourceClosure(
            VerifiedProjectRegistration const& registration,
            std::vector<ProjectPluginRegistrar::ResourceBlob> exactBlobs
        ) -> Result<std::vector<script::PureDataProgram::Resource>>
        {
            UF_TRY(validateProjectResourceClosure(exactBlobs));
            auto const& resources = registration.projectResources();
            if (
                resources.size() > script::PureDataProgram::k_maximumResourceCount
                || exactBlobs.size() > script::PureDataProgram::k_maximumResourceCount
            )
            {
                return refuse("ProjectPlugin resource count exceeds its ceiling");
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
                    return refuse("ProjectPlugin resource exceeds its byte ceiling");
                }
                if (
                    totalBytes
                    > script::PureDataProgram::k_maximumResourceClosureBytes
                        - blob.bytes.size()
                )
                {
                    return refuse("ProjectPlugin resources exceed their total ceiling");
                }
                totalBytes += blob.bytes.size();

                bool const inserted = blobsByName.try_emplace(
                    std::move(blob.name),
                    blob.kind,
                    std::move(blob.bytes)
                ).second;
                if (!inserted)
                {
                    return refuse("ProjectPlugin resource names must be unique");
                }
            }

            for (auto const& blob : blobsByName)
            {
                if (
                    std::ranges::find(resources, blob.first, &ProjectResource::name)
                    == resources.end()
                )
                {
                    return refuse("ProjectPlugin received an unregistered resource");
                }
            }
            auto verified = std::vector<script::PureDataProgram::Resource>{};
            verified.reserve(resources.size());
            for (auto const& resource : resources)
            {
                auto const found = blobsByName.find(resource.name);
                if (found == blobsByName.end())
                {
                    return refuse(
                        "ProjectPlugin resource is missing for registered name '"
                        + resource.name
                        + "'"
                    );
                }
                if (found->second.first != resource.kind)
                {
                    return refuse("ProjectPlugin resource kind does not match its registration");
                }
                if (found->second.second.size() != resource.size)
                {
                    return refuse("ProjectPlugin resource size does not match its registration");
                }
                UF_TRY_VALUE(
                    actualHash,
                    sha256(std::as_bytes(std::span{found->second.second}))
                );
                if (actualHash != resource.hash)
                {
                    return refuse("ProjectPlugin resource bytes do not match their registration");
                }
                verified.emplace_back(script::PureDataProgram::Resource{
                    .kind = resourceKind(resource.kind),
                    .name = resource.name,
                    .bytes = std::move(found->second.second),
                });
            }
            return verified;
        }
    } // namespace

    class ProjectSchemaOwner::State final
    {
    public:
        ContentHash              projectRegistrationHash;
        CanonicalJsonValidator   validateCanonicalJson{};
        ProjectDocumentValidator validateDocument{};
    };

    class ProjectPluginHandle::State final
    {
    public:
        VerifiedProjectRegistration registration;
        script::PureDataProgram     program;
        ProjectSchemaOwner          schemaOwner;
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
        VerifiedProjectRegistration const& registration,
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
            std::pair{exactSchemas.projectState, registration.projectStateSchemaHash()},
            std::pair{
                exactSchemas.projectObservation,
                registration.projectObservationSchemaHash(),
            },
            std::pair{
                exactSchemas.toolPrecondition,
                registration.projectToolPreconditionSchemaHash(),
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
            .projectRegistrationHash = registration.hash(),
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

    ProjectPluginHandle::ProjectPluginHandle(std::shared_ptr<State const> p_state) noexcept
        : m_state{std::move(p_state)}
    {
    }

    auto ProjectPluginHandle::pluginId() const -> std::string
    {
        return m_state->registration.pluginId();
    }

    auto ProjectPluginHandle::projectRegistrationHash() const -> ContentHash
    {
        return m_state->registration.hash();
    }

    auto ProjectPluginHandle::pluginModuleManifestHash() const -> ContentHash
    {
        return m_state->registration.pluginModuleManifestHash();
    }

    auto ProjectPluginHandle::projectResourceHashes() const
        -> std::vector<ContentHash>
    {
        auto const& resources = m_state->registration.projectResources();
        auto hashes       = std::vector<ContentHash>{};
        hashes.reserve(resources.size());
        for (auto const& resource : resources)
        {
            hashes.emplace_back(resource.hash);
        }
        return hashes;
    }

    auto ProjectPluginHandle::projectObservationSchemaHash() const -> ContentHash
    {
        return m_state->registration.projectObservationSchemaHash();
    }

    auto ProjectPluginHandle::canonicalize(std::string exactJcs) const
        -> Result<CanonicalJson>
    {
        return m_state->schemaOwner.canonicalize(std::move(exactJcs));
    }

    auto ProjectPluginHandle::invoke(
        ProjectPluginFunction function,
        CanonicalJson const& input
    ) const -> Result<ValidatedDocument>
    {
        // Validation is intentionally repeated at the call boundary. A
        // CanonicalJson carries no schema authority and cannot be promoted by a
        // caller attaching a hash label.
        UF_TRY_VALUE(
            validatedInput,
            m_state->schemaOwner.validate(
                function,
                ProjectDocumentDirection::Input,
                input
            )
        );
        UF_TRY_VALUE_CONTEXT(
            outputValue,
            m_state->program.invoke(functionName(function), validatedInput),
            "running isolated ProjectPlugin data function"
        );
        UF_TRY_VALUE(
            canonicalOutput,
            m_state->schemaOwner.canonicalizeValue(std::move(outputValue))
        );
        return m_state->schemaOwner.validateOutput(function, std::move(canonicalOutput));
    }

    auto ProjectPluginHandle::derive(CanonicalJson const& input) const -> Result<ValidatedDocument>
    {
        return invoke(ProjectPluginFunction::Derive, input);
    }

    auto ProjectPluginHandle::plan(CanonicalJson const& input) const -> Result<ValidatedDocument>
    {
        return invoke(ProjectPluginFunction::Plan, input);
    }

    auto ProjectPluginHandle::nextStep(CanonicalJson const& input) const
        -> Result<ValidatedDocument>
    {
        return invoke(ProjectPluginFunction::NextStep, input);
    }

    auto ProjectPluginHandle::reconcile(CanonicalJson const& input) const
        -> Result<ValidatedDocument>
    {
        return invoke(ProjectPluginFunction::Reconcile, input);
    }

    auto ProjectPluginHandle::reduce(CanonicalJson const& input) const -> Result<ValidatedDocument>
    {
        return invoke(ProjectPluginFunction::Reduce, input);
    }

    auto ProjectPluginRegistrar::registerPlugin(
        VerifiedProjectRegistration const& registration,
        std::string entryModule,
        std::vector<ModuleBlob> exactModules,
        std::vector<ResourceBlob> exactResources,
        ProjectSchemaOwner schemaOwner
    )
        -> Result<ProjectPluginHandle>
    {
        if (schemaOwner.projectRegistrationHash() != registration.hash())
        {
            return refuse("ProjectSchemaOwner is not bound to the verified registration");
        }
        UF_TRY_VALUE(runningEnvironmentHash, script::pluginEnvironmentHash());
        if (runningEnvironmentHash != registration.pluginEnvironmentHash())
        {
            return refuse("ProjectPlugin environment does not match the verified registration");
        }
        UF_TRY_VALUE(
            actualModuleManifestHash,
            derivePluginModuleManifestHash(entryModule, exactModules)
        );
        if (actualModuleManifestHash != registration.pluginModuleManifestHash())
        {
            return refuse("ProjectPlugin module closure does not match the verified registration");
        }

        auto const key = std::pair{registration.pluginId(), registration.hash()};
        if (m_plugins.contains(key))
        {
            return refuse("exact ProjectPlugin registration is immutable");
        }

        UF_TRY_VALUE(
            verifiedResources,
            verifyResourceClosure(registration, std::move(exactResources))
        );

        auto modules = std::vector<script::PureDataProgram::Module>{};
        modules.reserve(exactModules.size());
        for (auto& module : exactModules)
        {
            modules.emplace_back(script::PureDataProgram::Module{
                .name   = std::move(module.name),
                .source = std::move(module.source),
            });
        }

        UF_TRY_VALUE_CONTEXT(
            program,
            script::PureDataProgram::compile(
                registration.pluginId(),
                entryModule,
                std::move(modules),
                k_entryPoints,
                std::move(verifiedResources)
            ),
            "precompiling exact ProjectPlugin bytes"
        );
        auto state = std::make_shared<ProjectPluginHandle::State>(ProjectPluginHandle::State{
            .registration = registration,
            .program      = std::move(program),
            .schemaOwner  = std::move(schemaOwner),
        });
        auto handle = ProjectPluginHandle{
            std::shared_ptr<ProjectPluginHandle::State const>{std::move(state)}
        };
        auto const [position, inserted] = m_plugins.emplace(key, handle);
        if (!inserted)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "ProjectPlugin registry changed during startup"
            );
        }
        return position->second;
    }

    auto ProjectPluginRegistrar::findExact(
        std::string const& pluginId,
        ContentHash projectRegistrationHash
    ) const
        -> Result<ProjectPluginHandle>
    {
        auto const found = m_plugins.find(std::pair{pluginId, projectRegistrationHash});
        if (found == m_plugins.end())
        {
            return refuse("no exact ProjectPlugin registration is loaded");
        }
        return found->second;
    }

    auto derivePluginModuleManifestHash(
        std::string_view entryModule,
        std::span<ProjectPluginRegistrar::ModuleBlob const> modules
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
        std::span<ProjectPluginRegistrar::ResourceBlob const> resources
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

    auto currentProjectPluginEnvironmentHash() -> Result<ContentHash>
    {
        return script::pluginEnvironmentHash();
    }
} // namespace uf::operator_runtime

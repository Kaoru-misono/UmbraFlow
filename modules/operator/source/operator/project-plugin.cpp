#include "project-plugin.hpp"

#include <script/pure-data-program.hpp>

#include <json/value.hpp>

#include <core/error/contracts.hpp>
#include <core/text/utf8.hpp>

#include <domain/error.hpp>

#include <array>
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
        auto verifyArtifactClosure(
            VerifiedProjectRegistration const& registration,
            std::vector<ProjectPluginRegistrar::ArtifactBlob> exactBlobs
        )
            -> Result<std::vector<script::PureDataProgram::Resource>>
        {
            auto const& roots = registration.projectArtifactRoots();
            if (
                roots.size() > script::PureDataProgram::k_maximumResourceCount
                || exactBlobs.size() > script::PureDataProgram::k_maximumResourceCount
            )
            {
                return refuse("ProjectPlugin artifact root count exceeds its ceiling");
            }

            auto blobsByName = std::map<std::string, std::string>{};
            auto totalBytes = std::size_t{0};
            for (auto& blob : exactBlobs)
            {
                if (blob.bytes.size() > script::PureDataProgram::k_maximumResourceBytes)
                {
                    return refuse("ProjectPlugin artifact exceeds its byte ceiling");
                }
                if (
                    totalBytes
                    > script::PureDataProgram::k_maximumResourceClosureBytes
                        - blob.bytes.size()
                )
                {
                    return refuse("ProjectPlugin artifacts exceed their total ceiling");
                }
                totalBytes += blob.bytes.size();

                bool const inserted =
                    blobsByName.try_emplace(std::move(blob.name), std::move(blob.bytes)).second;
                if (!inserted)
                {
                    return refuse("ProjectPlugin artifact blob names must be unique");
                }
            }

            for (auto const& blob : blobsByName)
            {
                if (std::ranges::find(roots, blob.first, &NamedArtifactRoot::name) == roots.end())
                {
                    return refuse("ProjectPlugin received an unregistered artifact blob");
                }
            }
            auto verified = std::vector<script::PureDataProgram::Resource>{};
            verified.reserve(roots.size());
            for (auto const& root : roots)
            {
                auto const found = blobsByName.find(root.name);
                if (found == blobsByName.end())
                {
                    return refuse(
                        "ProjectPlugin artifact blob is missing for registered "
                        "root '"
                        + root.name
                        + "'"
                    );
                }
                UF_TRY_VALUE(actualHash, sha256(std::as_bytes(std::span{found->second})));
                if (actualHash != root.rootHash)
                {
                    return refuse("ProjectPlugin artifact bytes do not match their verified root");
                }
                verified.emplace_back(script::PureDataProgram::Resource{
                    .kind = script::PureDataProgram::ResourceKind::Json,
                    .name = root.name,
                    .bytes = std::move(found->second),
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

    auto ProjectPluginHandle::pluginHash() const -> ContentHash
    {
        return m_state->registration.pluginHash();
    }

    auto ProjectPluginHandle::projectArtifactRootHashes() const
        -> std::vector<ContentHash>
    {
        auto const& roots = m_state->registration.projectArtifactRoots();
        auto hashes       = std::vector<ContentHash>{};
        hashes.reserve(roots.size());
        for (auto const& root : roots)
        {
            hashes.emplace_back(root.rootHash);
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
        std::string exactPluginBytes,
        std::vector<ArtifactBlob> exactArtifactBlobs,
        ProjectSchemaOwner schemaOwner
    )
        -> Result<ProjectPluginHandle>
    {
        if (schemaOwner.projectRegistrationHash() != registration.hash())
        {
            return refuse("ProjectSchemaOwner is not bound to the verified registration");
        }
        UF_TRY_VALUE(actualPluginHash, sha256(std::as_bytes(std::span{exactPluginBytes})));
        if (actualPluginHash != registration.pluginHash())
        {
            return refuse("ProjectPlugin bytes do not match the verified registration");
        }

        auto const key = std::pair{registration.pluginId(), registration.hash()};
        if (m_plugins.contains(key))
        {
            return refuse("exact ProjectPlugin registration is immutable");
        }

        UF_TRY_VALUE(
            verifiedArtifacts,
            verifyArtifactClosure(registration, std::move(exactArtifactBlobs))
        );

        UF_TRY_VALUE_CONTEXT(
            program,
            script::PureDataProgram::compile(
                registration.pluginId(),
                "main",
                {
                    script::PureDataProgram::Module{
                        .name   = "main",
                        .source = std::move(exactPluginBytes),
                    },
                },
                k_entryPoints,
                std::move(verifiedArtifacts)
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
} // namespace uf::operator_runtime

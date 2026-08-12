#pragma once

#include "project-deployment.hpp"

#include <operator/journal-entry.hpp>
#include <operator/manifest.hpp>
#include <operator/project-plugin.hpp>
#include <operator/reconcile-outcome.hpp>
#include <operator/tool-invocation.hpp>

#include <core/error/result.hpp>

#include <domain/content-hash.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uf::deployment
{
    // The document every project directory holds at its root, at this exact
    // name. Everything else a project owns is named BY it, project-relative,
    // and opened through task_platform::ConfinedRoot.
    inline constexpr auto k_projectManifestFileName =
        std::string_view{"umbraflow-project.json"};

    // The second document, which only a conformance run opens. A project that
    // ships no conformance fixture holds no such file, and loading it for
    // production never looks for one.
    inline constexpr auto k_conformanceManifestFileName =
        std::string_view{"umbraflow-conformance.json"};

    // The exact bytes of schema/umbraflow-project-registration-v1.schema.json,
    // which is the document manifest_schema_hash names. It is carried rather
    // than opened because a loader that read it would have to be told where the
    // framework's own tree is; tests/deployment holds the file and these bytes
    // to each other, so the published document and the enforcing one cannot
    // drift apart in silence.
    [[nodiscard]] auto projectRegistrationSchemaBytes() -> std::string_view;

    // One Journal entry a project's own event schemas accept, as
    // umbraflow-conformance.json spells it.
    struct ProjectJournalDocument final
    {
        std::string eventType{};
        std::string payload{};
    };

    // The one UI action a contract run drives, in the RuntimeModel's own
    // vocabulary.
    struct ProjectUiAction final
    {
        std::string surface{};
        std::string uiTarget{};
        std::string action{};
    };

    // Every project document a suite is allowed to use, read as strings and
    // nothing else. Each payload member below carries the project's exact bytes
    // rather than a nested object, because those bytes are handed to
    // ProjectSchemaOwner::canonicalize, which refuses anything that is not
    // exact RFC 8785 JCS. A nested object would make this loader choose a
    // serialization, and the bytes would stop being the project's.
    struct ProjectVocabulary final
    {
        std::string mutatingTool{};
        std::string otherMutatingTool{};
        std::string readOnlyTool{};
        std::string toolArguments{};
        std::string refusedToolArguments{};
        std::string absentTool{};

        ProjectJournalDocument baselineEntry{};
        ProjectJournalDocument progressEntry{};
        ProjectJournalDocument confirmedEntry{};
        ProjectJournalDocument supersededEntry{};
        std::string            provenance{};

        std::string continueInput{};
        std::string confirmedInput{};
        std::string rejectedInput{};
        std::string ambiguousInput{};

        std::string approvalRequiredPlanTool{};

        ProjectUiAction uiAction{};
    };

    // One role of a conformance run: which deployment plays it, and the
    // vocabulary that drives that deployment.
    struct ProjectConformanceRole final
    {
        std::string       deployment{};
        ProjectVocabulary vocabulary{};
    };

    // One deployment, loaded: the registration this loader derived from the
    // deployment's block and the digests of the files it read, and the five
    // authorities built from it.
    //
    // There is no authored registration document anywhere in a project
    // directory. The block states intent -- which plugin, which schemas, which
    // artifact roots, each by path -- and every digest in the registration is
    // this loader's own arithmetic.
    struct LoadedDeployment final
    {
        std::string name{};

        operator_runtime::VerifiedProjectRegistration   registration;
        operator_runtime::ProjectSchemaOwner            schemaOwner;
        operator_runtime::ProjectJournalSchemaOwner     journalSchemaOwner;
        operator_runtime::ProjectToolCatalogSchemaOwner toolCatalogSchemaOwner;
        operator_runtime::ProjectReconcileSchemaOwner   reconcileSchemaOwner;

        // The compiled schemas and read manifests the five authorities above
        // were built from, kept rather than dropped. Every authority judges a
        // call and therefore needs its arguments; a document that names tools
        // without calling them -- a conformance vocabulary -- has none to
        // offer, and carriedTool is what lets such a document and this
        // deployment's catalog be held to each other.
        ProjectDeployment catalog;

        // registerPlugin's other two arguments, as bytes.
        std::string pluginBytes{};

        std::vector<operator_runtime::ProjectPluginRegistrar::ArtifactBlob>
            artifactBlobs{};
    };

    // The exact ProjectPlugin input bytes observed by the loader's document
    // validator. Validator callbacks retain a shared owner because LoadedProject
    // may move after they are installed. Every mutation and read is serialized,
    // and reads return copies so no borrow escapes the lock.
    class ProjectDocumentInputLog final
    {
        mutable std::mutex m_mutex{};
        std::string        m_lastReduceInput{};
        std::string        m_lastDeriveInput{};

    public:
        auto record(
            operator_runtime::ProjectPluginFunction function,
            std::string_view exactJcs
        ) -> void;

        [[nodiscard]] auto lastReduceInput() const -> std::string;
        [[nodiscard]] auto lastDeriveInput() const -> std::string;
    };

    struct LoadedProject final
    {
        std::filesystem::path directory{};

        // The RuntimeArtifact root the installer is handed. One per project,
        // shared by every deployment: the model covers every surface a
        // deployment drives, and a second root would be a second world.
        std::filesystem::path runtimeArtifactRoot{};

        std::string                   primaryDeployment{};
        std::vector<LoadedDeployment> deployments{};

        // Shared with every deployment's retained document validator. The log
        // owns and synchronizes its mutable state; the shared pointer supplies
        // only the callback lifetime.
        std::shared_ptr<ProjectDocumentInputLog> documentInputLog{
            std::make_shared<ProjectDocumentInputLog>()
        };

        [[nodiscard]]
        auto findDeployment(std::string_view name) const
            -> LoadedDeployment const*;
    };

    // What a conformance run reads on top of a production load: the second root
    // document, the capture a suite observes through, and the two roles.
    //
    // It carries the production load rather than restating it, because a
    // conformance directory is a project directory that also ships a fixture.
    // Nothing here is a member production could be given a default for: a
    // project that implements nothing mutating has no vocabulary to state, and
    // a single-deployment project cannot fill two roles at all.
    struct ConformanceProject final
    {
        LoadedProject loaded{};

        // One capture of the project's target, as the project's own PNG bytes.
        // The load decoded them, so these are an image (2.7 R9); its extent is
        // NOT checked here and cannot be, because after the Q2 ruling the
        // extent it must match is published by RuntimeModelBinding, which does
        // not exist until the Host has activated the artifact. See
        // project-as-data.md 2.7 R8.
        std::vector<std::byte> probeFrame{};

        ProjectConformanceRole underTest{};
        ProjectConformanceRole foreign{};
    };

    // What a caller already knows one of this directory's deployments must
    // hash to, because something else recorded it earlier -- a stored
    // SessionManifest's project_registration_hash.
    //
    // This is the only comparison in the design between two values produced at
    // two different times, and therefore the only one on this chain that can
    // fail. Every other digest a load compares is one this loader computed on
    // both sides.
    struct ExpectedRegistration final
    {
        std::string deployment{};
        ContentHash hash;
    };

    // Reads a project directory the way the product reads it:
    // umbraflow-project.json, the RuntimeArtifact it names, and every
    // deployment's five authorities. One deployment is enough, no conformance
    // document is opened, and no tool is required to be mutating -- a project
    // that honestly implements nothing mutating is a project this starts.
    //
    // `expected` is required rather than optional so that a caller states which
    // case it is in. A resume passes what the stored session named; a first
    // load passes an empty span, which says "this caller holds no prior
    // commitment" rather than leaving a default to be read as one. A name that
    // matches no deployment is a refusal, so a misspelling cannot silently
    // disarm the check.
    [[nodiscard]]
    auto loadProductionProject(
        std::filesystem::path const& directory,
        std::span<ExpectedRegistration const> expected
    ) -> Result<LoadedProject>;

    // The production load above, plus umbraflow-conformance.json: the probe
    // frame, the two roles played by two different deployments, and every
    // agreement between a role's vocabulary and the deployment playing it.
    //
    // It is the production load plus a layer rather than a second reader of the
    // same document, so a directory a suite accepts is necessarily a directory
    // the product starts.
    [[nodiscard]]
    auto loadConformanceProject(
        std::filesystem::path const& directory,
        std::span<ExpectedRegistration const> expected
    ) -> Result<ConformanceProject>;
}

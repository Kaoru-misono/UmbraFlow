#pragma once

#include <operator/journal-entry.hpp>
#include <operator/manifest.hpp>
#include <operator/project-plugin.hpp>
#include <operator/reconcile-outcome.hpp>
#include <operator/tool-invocation.hpp>

#include <core/types/integer.hpp>

#include <memory>
#include <string>
#include <vector>

namespace uf::operator_runtime::contract
{
    // One Journal entry a project's own event schemas accept.
    struct JournalDocument final
    {
        std::string eventType{};
        std::string payload{};
    };

    // Every project document the suite is allowed to use. The suite invents no
    // project bytes: the schemas that judge them belong to the supplying
    // deployment, so the documents that satisfy them must come from there too.
    struct ProjectVocabulary final
    {
        // Two mutating tools and one read-only tool from this project's Tool
        // Catalog, arguments its descriptors accept, arguments they refuse, and
        // a name the catalog does not carry. Proving that the catalog rather
        // than the caller decides mutability needs all five.
        std::string mutatingTool{};
        std::string otherMutatingTool{};
        std::string readOnlyTool{};
        std::string toolArguments{};
        std::string refusedToolArguments{};
        std::string absentTool{};

        // The entry a ProjectInstance is provisioned with, and three more the
        // suite appends. Their payloads must differ from one another: the
        // reducer-input case proves that an entry a commit did not name never
        // reaches the reducer, which is unprovable when two payloads are equal.
        // `baselineEntry.eventType` must be the registration's own
        // baseline_event_type.
        JournalDocument baselineEntry{};
        JournalDocument progressEntry{};
        JournalDocument confirmedEntry{};
        JournalDocument supersededEntry{};
        std::string     provenance{};

        // Reconcile inputs whose output this project's disposition reader maps
        // to each disposition. Only the project can supply them, because its
        // own plugin decides the mapping; the suite must never assume the
        // disposition is spelled in the request.
        std::string continueInput{};
        std::string confirmedInput{};
        std::string rejectedInput{};
        std::string ambiguousInput{};
    };

    // Everything the suite needs from one project's trusted deployment. The
    // five authorities are values rather than callables because that is how the
    // Operator takes them: a deployment builds each one from the exact schema
    // bytes its registration pinned, and the suite is then unable to mint a
    // document any other way.
    //
    // No member carries an in-class initializer. Every one of them must come
    // from the deployment, and a defaulted authority would be an authority
    // nobody granted.
    struct ProjectUnderTest final
    {
        VerifiedProjectRegistration   registration;
        ProjectSchemaOwner            schemaOwner;
        ProjectJournalSchemaOwner     journalSchemaOwner;
        ProjectToolCatalogSchemaOwner toolCatalogSchemaOwner;
        ProjectReconcileSchemaOwner   reconcileSchemaOwner;

        // The exact plugin bytes and artifact blobs the registration pinned.
        // The suite loads the plugin itself, because a handle handed to it
        // would not prove that these bytes are the ones the registration names.
        std::string                                       pluginBytes;
        std::vector<ProjectPluginRegistrar::ArtifactBlob> artifactBlobs;

        // Where the deployment's document validator records the exact bytes it
        // last saw as a Reduce input. Shared and mutable because the validator
        // writes it and the suite reads it; the suite drives one project from
        // one thread, so no synchronization is implied or permitted.
        std::shared_ptr<std::string> observedReduceInput;

        ProjectVocabulary vocabulary;
    };

    // Which registration the suite is asking for. Authority is per registration,
    // so proving that it does not cross needs a second one that is complete
    // enough to mint documents of its own.
    enum class ProjectRole : uint8
    {
        UnderTest,
        Foreign,
    };

    // Defined by the repository that runs the suite. There is deliberately no
    // default and no registry: a consumer that does not define it fails to
    // link, rather than running a suite against nothing.
    [[nodiscard]]
    auto projectUnderTest(ProjectRole role) -> ProjectUnderTest;
}

#pragma once

#include <operator-contract/host-delivery-fixture.hpp>

#include <operator/journal-entry.hpp>
#include <operator/manifest.hpp>
#include <operator/project-plugin.hpp>
#include <operator/reconcile-outcome.hpp>
#include <operator/tool-invocation.hpp>

#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <cstddef>
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

    // One file inside a RuntimeArtifact: the artifact-relative path a locator
    // names, and the exact bytes stored there.
    struct ArtifactFile final
    {
        std::string            path{};
        std::vector<std::byte> bytes{};
    };

    // One project's published RuntimeArtifact: the RuntimeModel its annotation
    // front end produced, and the complete asset closure that model's locators
    // name. The suite installs these bytes and carries no model of its own,
    // because a suite that substituted one would answer whether ITS model
    // satisfies the contract rather than whether this project's does.
    //
    // This is not `artifactBlobs` below, and the two must not be merged. Those
    // are the PR:`project_artifact_roots` the registration pins: project content
    // the plugin reads, named by the registration hash. A RuntimeArtifact is
    // named by the session manifest's runtime_model_artifact_root_hash, is
    // installed by the Operator and is read by the Host, never by the plugin.
    //
    // The model is resolved against the ProjectProbeFrame below, which the same
    // project supplies: the suite captures no world of its own.
    struct ProjectRuntimeArtifact final
    {
        std::string               model{};
        std::vector<ArtifactFile> assets{};
    };

    // The world this project's RuntimeModel is resolved against.
    //
    // Both members are the project's because both are readable only from the
    // model, and the model is a Luau value: C++ parses no RuntimeModel field, so
    // the suite can derive neither the geometry the model was authored at nor a
    // frame that satisfies it. EngineSession refuses a capture whose extent
    // disagrees with the fingerprint it was given, which is why a suite that
    // substituted either one could only run against a model of its own.
    //
    // One frame, not a set: the suite drives exactly one UI action -- the
    // `uiAction` its vocabulary names -- against one surface, so a second frame
    // would be a parameter no case reads. A project whose model covers more
    // surfaces still supplies the one frame that resolves this action's.
    //
    // Only the UnderTest registration's frame is ever observed. The Foreign one
    // exists to mint documents that must not be accepted elsewhere, and no case
    // opens a Host on it, so a project may hand both roles the same frame.
    struct ProjectProbeFrame final
    {
        // base_resolution and base_dpi exactly as the model states them.
        ProjectFingerprint fingerprint;

        // One capture of this project's target, PNG-encoded, whose extent is
        // that fingerprint's. The suite decodes it, resolves the installed model
        // against it, and requires the resolution to name `uiAction.surface`;
        // the action's own binding must resolve on it too, or no dispatch the
        // suite reserves can be delivered.
        std::vector<std::byte> png{};
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

        // One more mutating tool, told apart by the OP:`PlanProposal` this
        // project's plugin returns for it: an effect whose risk requires a
        // human approval before the first dispatch. The proposal itself is not
        // listed here and must not be: freezePlan calls plugin.plan itself, so
        // a document the suite carried would be a document nothing produced.
        // What the suite needs is a name it can invoke to reach that shape.
        //
        // It must be Mutating in this project's Tool Catalog, and its proposal
        // must echo the tool it was invoked for; otherwise the case reaches the
        // tool check instead of the property it is about.
        std::string approvalRequiredPlanTool{};

        // The one UI action the suite drives against this project's
        // RuntimeModel, in that model's own vocabulary. Only the project can
        // name it: a suite could pick some clickable binding out of a model, but
        // which binding a contract run may drive is a project decision, and a
        // model offering none would then fail as "no binding anywhere" rather
        // than as the thing it is.
        //
        // The project's own next_step must name this surface and this target in
        // the OP:`UIActionIntent` it returns, or the suite drives something the
        // plan never asked for. Nothing downstream enforces that agreement:
        // task::DispatchAuthority carries no UI identifier, and the ledger
        // stores the intent bytes without reading their surface_id or
        // ui_target_id. Keeping the two the same is the project's own duty.
        task::UiActionUnderTest uiAction{};
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

        // The RuntimeArtifact this project's sessions are pinned to. The suite
        // publishes a release carrying exactly these bytes and installs it, so
        // every observation the suite composes a snapshot from was resolved
        // through this project's model.
        ProjectRuntimeArtifact runtimeArtifact;

        // The frame that model is resolved against, and the geometry it was
        // authored at. It carries no in-class initializer and cannot: a
        // ProjectFingerprint has no default state, so a project that omits it
        // fails to compile rather than observing a world nobody declared.
        ProjectProbeFrame probeFrame;

        // Where the deployment's document validator records the exact bytes it
        // last saw as a Reduce input. Shared and mutable because the validator
        // writes it and the suite reads it; the suite drives one project from
        // one thread, so no synchronization is implied or permitted.
        std::shared_ptr<std::string> observedReduceInput;

        // The same, for the Derive input. The Snapshot Coordinator assembles
        // that envelope rather than accepting one, so the only way to assert
        // what the plugin was handed is to record what the deployment's own
        // validator saw.
        std::shared_ptr<std::string> observedDeriveInput;

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

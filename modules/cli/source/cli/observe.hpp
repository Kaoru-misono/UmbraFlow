#pragma once

#include "args.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <engine/ports.hpp>

#include <ocr/engine.hpp>

#include <filesystem>
#include <memory>
#include <string>

namespace uf::cli
{
    // The three ports one observation runs over, plus the geometry the target
    // actually presented. Separated from ObserveArgs because they are what a
    // desktop produces and what a recorded capture stands in for: this is the
    // seam a test drives with TargetWorld::Recorded, and it is the same seam
    // modules/conformance drove the offline frame corpus through.
    //
    // liveFingerprint carries no in-class initializer for BoundTarget's reason:
    // ProjectFingerprint refuses to invent a geometry, so every construction
    // site states the one it measured.
    struct ObserveSources final
    {
        std::unique_ptr<engine::IFrameSource> frameSource{};
        std::unique_ptr<engine::IActionSink>  actionSink{};

        // Required, not optional. A null engine answers every Reader with "no
        // text", and this verb exists so a human can tell a screen with no text
        // from a host that could not read one.
        std::unique_ptr<ocr::IOcrEngine> ocrEngine{};

        ProjectFingerprint liveFingerprint;
    };

    // What one production observation established. Plain strings and integers
    // for OpenedProject's reason: the authorities and the snapshot that
    // produced these are owned by the Host and the registrar, and a report that
    // carried them would outlive the only things entitled to answer for them.
    struct ObservedState final
    {
        std::filesystem::path project{};
        std::filesystem::path runtimeArtifactRoot{};

        // The digest of the project's own runtime-artifact.manifest.json, which
        // is the name every authority knows this artifact by, and the CAS
        // generation the Operator had pinned it to.
        std::string artifactRootHash{};
        uint64      installedGeneration{};

        // The deployment whose plugin would be handed the resolution below.
        // Registering it proves the bytes this project pins compile and match
        // the registration; nothing is invoked on it, because the derive
        // envelope is trusted Operator code's to assemble.
        std::string deployment{};
        std::string pluginId{};
        std::string registrationHash{};

        // The model's own geometry, republished by the binding the Host parsed
        // it into, beside the geometry the target presented. The engine already
        // refused the pair if they disagree; they are reported because a reader
        // debugging a resolution needs to see which extent the model expected.
        std::string modelSemanticHash{};

        uint32 modelWidth{};
        uint32 modelHeight{};
        uint32 liveWidth{};
        uint32 liveHeight{};

        std::string observationId{};
        uint64      targetGeneration{};
        std::string stateResolutionHash{};

        // The exact RFC 8785 StateResolution document. It is carried and
        // printed whole rather than summarized: C++ interprets no field of it,
        // and these are the bytes a plugin would receive as ui_snapshot.
        std::string stateResolution{};

        std::filesystem::path trace{};
    };

    // Loads the project, opens and activates the RuntimeArtifact it names,
    // observes once through `sources`, and reports what the trusted resolver
    // concluded.
    //
    // Toward the target: no verb of the action sink is called, no Receipt is
    // minted and no plan is proposed, which is what makes this the entry a
    // project may run before it has met the conditions for its first mutation.
    //
    // Toward the Operator: ProductLifecycle is the single production
    // construction site. It completes restart recovery before exposing the
    // first observation and derives the active installed generation internally.
    //
    // The sources are consumed: EngineSession takes ownership of all three
    // ports, and a caller that kept one would hold a borrow of the session's
    // state with no contract keeping it alive.
    [[nodiscard]]
    auto observeProject(
        ObserveArgs const& args,
        ObserveSources sources
    ) -> Result<ObservedState>;

    // The composition root: binds the live window --hwnd names and the OCR
    // engine --ocr-models names, then runs the function above. Windows only,
    // as explore and targets are.
    [[nodiscard]]
    auto observeProduct(ObserveArgs const& args) -> Result<ObservedState>;

    // Separate from the observation so the shape an operator reads is testable
    // without a target, as formatOpenedProject and formatTargetListings are.
    [[nodiscard]]
    auto formatObservedState(ObservedState const& observed) -> std::string;
}

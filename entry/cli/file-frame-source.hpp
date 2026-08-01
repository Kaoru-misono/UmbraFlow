#pragma once

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <annotation/resource.hpp>

#include <domain/frame.hpp>

#include <engine/ports.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <vector>

namespace uf::cli
{
    // A frame source that replays PNG files instead of a live target.
    //
    // WHY IT EXISTS. The falsification matrix is a statement about the screens a
    // model was authored on, so its frames come from those screens' own files.
    // It is the frame source `umbra-flow check` binds, and it is the reason that
    // subcommand needs no window, no DPI awareness and no capture device -- the
    // whole of the platform in a `check` run is the filesystem.
    //
    // ONE FILE PER CAPTURE, IN FILE-NAME ORDER, AND NEVER TWICE. That order is a
    // contract with the trusted framework routine on the other side, which opens
    // one observation per declared screen in content-hash order and reads a
    // screen's pixels from `assets/screens/<hash>.png` -- so file-name order and
    // content-hash order are one order, and the Nth capture is the Nth screen.
    //
    // Running out is a FAILURE rather than a repeat of the last frame. A source
    // that repeated would let a routine which lost count keep measuring one
    // screen and report a matrix that passed; failing closed makes the same
    // mistake a run that stops and says so.
    class FileFrameSource final : public engine::IFrameSource
    {
        std::vector<std::filesystem::path> m_files;
        ProjectFingerprint                 m_fingerprint;
        std::size_t                        m_served{0};

    public:
        // Takes an already-ordered file list. `create` is how one is ordinarily
        // built and is where the list is established; this stays public because
        // the invariant is only "serve these, in this order", which construction
        // cannot violate. A source built with an empty list refuses its first
        // capture with the sentence below, which is the same fail-closed answer
        // as running out.
        FileFrameSource(
            std::vector<std::filesystem::path> files,
            ProjectFingerprint fingerprint
        ) noexcept;

        // Collects every `*.png` directly inside `directory`, sorted by file
        // name, and prepares to serve them at `fingerprint`'s geometry.
        //
        // A directory that holds none fails here rather than at the first
        // capture: a project with no screens cannot be falsified at all, and
        // saying so before a trace file is opened is the difference between a
        // usable message and an empty run.
        //
        // The fingerprint is the ANNOTATION PROJECT's, not a measurement of the
        // files. Every template in the project was cut at that geometry, so a
        // screen of another size is measuring one model's marks against another
        // model's pixels; capture() refuses such a file by name.
        [[nodiscard]]
        static auto create(
            std::filesystem::path const& directory,
            ProjectFingerprint const& fingerprint
        ) -> Result<std::unique_ptr<FileFrameSource>>;

        [[nodiscard]]
        auto capture(CaptureBudget const& budget) -> Result<Frame> override;

        // There is no bound target, so there is no instance to revalidate. It
        // succeeds rather than refusing because the check the engine performs
        // here -- "is the window I observed still the window I am about to act
        // on" -- has no subject in a run that acts on nothing.
        [[nodiscard]] auto validateTargetInstance() -> Status override;

        // How many files this source holds, read before it is handed over so a
        // caller can state the count it expects the routine to walk.
        [[nodiscard]] auto fileCount() const noexcept -> std::size_t;

        // The files, in the order they will be served. Borrows this object's
        // storage and stays valid only while it is alive.
        [[nodiscard]]
        auto files() const noexcept UF_LIFETIME_BOUND
            -> std::vector<std::filesystem::path> const&;
    };
}

#pragma once

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/frame.hpp>
#include <domain/space.hpp>

#include <engine/ports.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <vector>

namespace uf::cli
{
    // A frame source that replays PNG files instead of a live target. It is what
    // `umbra-flow check` binds, and the reason that subcommand needs no window, no
    // DPI awareness and no capture device.
    //
    // One file per capture, in file-name order, and never twice -- a contract with
    // the trusted framework routine, which opens one observation per declared screen
    // in content-hash order and reads its pixels from `assets/screens/<hash>.png`,
    // so the Nth capture is the Nth screen. Running out is a FAILURE rather than a
    // repeat of the last frame, which would let a routine that lost count keep
    // measuring one screen and report a matrix that passed.
    class FileFrameSource final : public engine::IFrameSource
    {
        std::vector<std::filesystem::path> m_files;
        ProjectFingerprint                 m_fingerprint;
        std::size_t                        m_served{0};

    public:
        // Takes an already-ordered file list. Public because the invariant is only
        // "serve these, in this order", which construction cannot violate. An empty
        // list refuses the first capture, as running out does.
        FileFrameSource(
            std::vector<std::filesystem::path> files,
            ProjectFingerprint fingerprint
        ) noexcept;

        // Collects every `*.png` directly inside `directory`, sorted by file name,
        // and prepares to serve them at `fingerprint`'s geometry. A directory that
        // holds none fails here rather than at the first capture. The fingerprint is
        // the ANNOTATION PROJECT's, not a measurement of the files: every template
        // was cut at that geometry, so a screen of another size measures one model's
        // marks against another's pixels, and capture() refuses it by name.
        [[nodiscard]]
        static auto create(
            std::filesystem::path const& directory,
            ProjectFingerprint const& fingerprint
        ) -> Result<std::unique_ptr<FileFrameSource>>;

        [[nodiscard]]
        auto capture(CaptureBudget const& budget) -> Result<Frame> override;

        // There is no bound target, so there is no instance to revalidate. It
        // succeeds rather than refusing because "is the window I observed still the
        // one I am about to act on" has no subject in a run that acts on nothing.
        [[nodiscard]] auto validateTargetInstance() -> Status override;

        // Read before the source is handed over, so a caller can state the count it
        // expects the routine to walk.
        [[nodiscard]] auto fileCount() const noexcept -> std::size_t;

        // In the order they will be served. Borrows this object's storage.
        [[nodiscard]]
        auto files() const noexcept UF_LIFETIME_BOUND
            -> std::vector<std::filesystem::path> const&;
    };
}

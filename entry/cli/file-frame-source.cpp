#include "file-frame-source.hpp"

#include <core/error/result.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>

#include <image/png.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

namespace uf::cli
{
    namespace
    {
        // The capture identity every replayed frame wears.
        //
        // A live source takes both from the bound window: the session id names
        // the capture device and the generation names the window instance, and
        // together they are what an observation lease is validated against.
        // There is no window here, so one fixed pair for the whole replay is the
        // honest answer -- every frame comes from the same place, which is this
        // directory, and nothing about it can change under the run.
        constexpr auto k_replaySessionId  = CaptureSessionId{1};
        constexpr auto k_replayGeneration = uint64{1};
    }

    FileFrameSource::FileFrameSource(
        std::vector<std::filesystem::path> files,
        ProjectFingerprint fingerprint
    ) noexcept
        : m_files{std::move(files)}
        , m_fingerprint{fingerprint}
    {
    }

    auto FileFrameSource::create(
        std::filesystem::path const& directory,
        ProjectFingerprint const& fingerprint
    ) -> Result<std::unique_ptr<FileFrameSource>>
    {
        auto error = std::error_code{};
        if (!std::filesystem::is_directory(directory, error))
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "this project holds no screens directory at \"{}\"; the "
                    "falsification matrix measures the screens a model was "
                    "authored on, so a project with none has nothing to check",
                    directory.string()
                )
            );
        }

        auto files = std::vector<std::filesystem::path>{};
        for (auto const& entry : std::filesystem::directory_iterator{directory, error})
        {
            if (entry.is_regular_file(error) && entry.path().extension() == ".png")
            {
                files.emplace_back(entry.path());
            }
        }
        if (error)
        {
            return fail(
                AutomationErrorKind::IoFailure,
                std::format("cannot list the screens in \"{}\"", directory.string())
            );
        }
        if (files.empty())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format("\"{}\" holds no PNG screens", directory.string())
            );
        }
        std::ranges::sort(files);

        return std::make_unique<FileFrameSource>(std::move(files), fingerprint);
    }

    auto FileFrameSource::capture(CaptureBudget const& budget) -> Result<Frame>
    {
        // The budget's cancellation is honoured and its deadline is not, because
        // nothing here can block on an external producer: a decode either
        // finishes or fails. A cancelled generation must still stop, and a stop
        // requested between two screens is the ordinary way a long matrix ends
        // early.
        if (budget.cancellation.stop_requested())
        {
            return fail(
                AutomationErrorKind::Cancelled,
                "cancelled before the next screen was decoded"
            );
        }

        if (m_served >= m_files.size())
        {
            return fail(
                AutomationErrorKind::CaptureUnavailable,
                std::format(
                    "this replay holds {} screens and a {}th was asked for; the "
                    "routine walking them and the directory serving them are not "
                    "walking one set",
                    m_files.size(),
                    m_served + 1U
                )
            );
        }

        auto const& path = m_files[m_served];
        UF_TRY_VALUE(decoded, image::loadPng(path));

        if (
            decoded.width != m_fingerprint.width()
            || decoded.height != m_fingerprint.height()
        )
        {
            return fail(
                AutomationErrorKind::TargetCompatibilityUnverified,
                std::format(
                    "screen \"{}\" is {}x{} and this project was authored at "
                    "{}x{}; every template it holds was cut at that geometry, so "
                    "measuring them here would compare one model's marks against "
                    "another model's pixels",
                    path.string(),
                    decoded.width,
                    decoded.height,
                    m_fingerprint.width(),
                    m_fingerprint.height()
                )
            );
        }

        UF_TRY_VALUE(
            transform,
            CoordinateTransform::create(
                Point<DesktopSpace>{0.0F, 0.0F},
                static_cast<float>(m_fingerprint.width()),
                static_cast<float>(m_fingerprint.height()),
                m_fingerprint.width(),
                m_fingerprint.height()
            )
        );

        // The decoder answers in RGBA and a frame carries BGRA, so the two outer
        // channels are swapped on the way in. Doing it here rather than teaching
        // the decoder a second output format keeps the swap beside the one
        // consumer that needs it.
        auto pixels = std::vector<std::byte>{};
        pixels.reserve(decoded.pixels.size());
        for (auto index = std::size_t{0}; index + 3U < decoded.pixels.size(); index += 4U)
        {
            pixels.emplace_back(decoded.pixels[index + 2U]);
            pixels.emplace_back(decoded.pixels[index + 1U]);
            pixels.emplace_back(decoded.pixels[index]);
            pixels.emplace_back(decoded.pixels[index + 3U]);
        }

        auto const width = checkedCast<std::size_t>(decoded.width);
        if (!width)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format("screen \"{}\" is too wide to address", path.string())
            );
        }
        auto const stride = checkedMultiply(
            *width,
            bytesPerPixel(PixelFormat::Bgra8)
        );
        if (!stride)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format("screen \"{}\" has an unrepresentable row", path.string())
            );
        }

        // The frame ordinal counts from one and never repeats, so two screens
        // never share a frame identity and every trace line can be read back to
        // the screen that produced it.
        ++m_served;

        return Frame::create(
            FrameId{static_cast<uint64>(m_served)},
            k_replaySessionId,
            TargetGeneration::fromValue(k_replayGeneration),
            MonotonicInstant::now(),
            decoded.width,
            decoded.height,
            *stride,
            PixelFormat::Bgra8,
            std::make_shared<FrameBuffer const>(std::move(pixels)),
            transform
        );
    }

    auto FileFrameSource::validateTargetInstance() -> Status
    {
        return ok();
    }

    auto FileFrameSource::fileCount() const noexcept -> std::size_t
    {
        return m_files.size();
    }

    auto FileFrameSource::files() const noexcept
        -> std::vector<std::filesystem::path> const&
    {
        return m_files;
    }
}

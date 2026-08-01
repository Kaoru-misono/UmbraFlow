#include "pixel-probe.hpp"

#include <core/error/result.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/space.hpp>

#include <image/pixels.hpp>
#include <image/png.hpp>

#include <vision/bgra-image.hpp>
#include <vision/frame-analysis.hpp>

#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace uf::task
{
    namespace
    {
        // How many dominant colours the census reports. One, because the caller
        // is choosing a colour key and a key is one colour: a longer list would
        // be a table this report has no shape for and no caller has asked for.
        constexpr auto k_dominantColourCount = uint32{1};

        [[nodiscard]]
        auto invalid(std::string message) -> std::unexpected<Error>
        {
            return fail(AutomationErrorKind::InvalidResource, std::move(message));
        }
    }

    auto probePngRegion(
        std::span<std::byte const> png,
        PixelRect rect,
        std::optional<ProbeColourKey> key
    ) -> Result<PixelProbeReport>
    {
        if (png.size() > k_maximumProbeBytes)
        {
            return invalid(
                std::format(
                    "a probe blob of {} bytes is beyond the {}-byte ceiling",
                    png.size(),
                    k_maximumProbeBytes
                )
            );
        }
        if (key.has_value() && key->tolerance > k_maximumColourKeyTolerance)
        {
            return invalid(
                std::format(
                    "a colour tolerance of {} is beyond {}, the widest the "
                    "summed per-channel distance can express",
                    key->tolerance,
                    k_maximumColourKeyTolerance
                )
            );
        }

        UF_TRY_VALUE(decoded, image::decodePng(png, "probe blob"));
        UF_TRY_VALUE(bgra, image::rgba8ToBgra8(std::move(decoded.pixels)));

        auto const widthSize = checkedCast<std::size_t>(decoded.width);
        if (!widthSize)
        {
            return invalid("the probe blob's width is beyond range");
        }
        auto const stride = *widthSize * 4U;

        UF_TRY_VALUE(
            view,
            BgraImage::create(bgra, decoded.width, decoded.height, stride)
        );

        if (rect.right() > view.width() || rect.bottom() > view.height())
        {
            return invalid(
                std::format(
                    "the probe region {}x{}+{}+{} does not fit inside the {}x{} "
                    "blob",
                    rect.width(),
                    rect.height(),
                    rect.x(),
                    rect.y(),
                    view.width(),
                    view.height()
                )
            );
        }

        UF_TRY_VALUE(
            census,
            censusColours(
                view,
                ColourCensusSpec{
                    .rect           = rect,
                    .maximumEntries = k_dominantColourCount,
                }
            )
        );

        auto report = PixelProbeReport{
            .imageWidth      = view.width(),
            .imageHeight     = view.height(),
            .rectPixels      = census.rectPixels,
            .distinctColours = census.distinctColours,
        };
        if (!census.dominant.empty())
        {
            auto const& dominant   = census.dominant.front();
            report.dominantRed    = dominant.red;
            report.dominantGreen  = dominant.green;
            report.dominantBlue   = dominant.blue;
            report.dominantPixels = dominant.count;
        }

        if (!key.has_value())
        {
            return report;
        }

        // probeColour is a MULTI-frame measurement: it reports how far each
        // pixel's grey moved across the set, and refuses fewer than two frames
        // because one frame is stable everywhere and answers nothing. The
        // selection half -- which pixels the key takes, and at what weight -- is
        // read off frame ZERO alone, so handing it the same view twice is not a
        // trick to satisfy a check: it is the honest way to ask for the half
        // that a single blob can answer, and it is exactly what the v4 authoring
        // line already does for its draw-time mask. The two spread means come
        // back zero and are dropped on the floor here; see PixelProbeReport for
        // why they are not fields.
        auto const frames = std::array<BgraImage, 2>{view, view};
        UF_TRY_VALUE(
            measured,
            probeColour(
                frames,
                ColourProbeSpec{
                    .rect      = rect,
                    .keyRed    = key->red,
                    .keyGreen  = key->green,
                    .keyBlue   = key->blue,
                    .tolerance = key->tolerance,
                }
            )
        );

        report.fullySelectedPixels = measured.fullySelectedPixels;
        report.rampSelectedPixels  = measured.rampSelectedPixels;
        report.selectedWeight      = measured.selectedWeight;
        return report;
    }
}

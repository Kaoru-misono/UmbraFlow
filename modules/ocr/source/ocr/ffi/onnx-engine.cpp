#include <ocr/onnx-engine.hpp>

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/space.hpp>

#include <vision/bgra-image.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// ONNX Runtime's headers are third-party and do not build clean under this
// project's /W4 /WX profile; a manifest-driven module has no CMakeLists to mark
// them external, so wrap the include exactly as the repo's other vendored FFI
// does (image/ffi/png-decoder.cpp, script/ffi/engine.cpp).
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
#include <onnxruntime_cxx_api.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace uf::ocr
{
    namespace
    {
        // The strip height PP-OCR recognition models are trained on. The width
        // is dynamic and follows the source aspect ratio; the height is not a
        // preference, it is what the graph's first convolution expects.
        constexpr auto k_recognitionHeight = uint32{48};

        // Bounds on the resized strip's width. The floor keeps a nearly-square
        // crop from collapsing to a few columns the model cannot read; the
        // ceiling matches the widest dynamic shape the release declares, past
        // which the runtime would allocate without bound on a caller's rect.
        constexpr auto k_minimumRecognitionWidth = uint32{16};
        constexpr auto k_maximumRecognitionWidth = uint32{3200};

        // Basis points, matching how this project already spells a similarity
        // threshold, so a confidence and a threshold are read on one scale.
        constexpr auto k_basisPointScale = double{10000.0};

        // The CTC blank occupies class 0, so a dictionary entry at index i is
        // class i + 1. This is the models' convention rather than a choice made
        // here, and decode below depends on it.
        constexpr auto k_ctcBlankClass = std::size_t{0};

        // Every detection number below is copied from the detection model's own
        // inference config at
        // modules/ocr/external/models/ppocr-v6-small-det/inference.yml, and each
        // names the key it was copied from.
        //
        // PostProcess.thresh: the probability above which one cell of the
        // model's output map counts as text at all.
        constexpr auto k_detectionMapThreshold = double{0.2};

        // PostProcess.box_thresh: the MEAN probability a candidate region has to
        // reach before it is offered as a line. It is the detector's own "is
        // there really text here", and it is a different question from the
        // recogniser's confidence, which judges what the text SAYS -- a caller
        // needs both and this adapter reports the second.
        constexpr auto k_detectionBoxThreshold = double{0.45};

        // PostProcess.unclip_ratio: DB's output map hugs the strokes, so a
        // candidate cut out at the threshold clips the ascenders and the first
        // and last glyph. Every candidate is therefore grown outward by
        // `area * ratio / perimeter` before it is cut, which is the offset
        // distance PaddleOCR's Vatti unclip uses.
        constexpr auto k_detectionUnclipRatio = double{1.4};

        // PostProcess.max_candidates: the ceiling on how many candidate regions
        // one map is allowed to yield, before any of them is scored.
        constexpr auto k_detectionMaxCandidates = std::size_t{3000};

        // DBPostProcess's min_size, which the shipped config leaves at
        // PaddleOCR's own default of 3. A candidate thinner than this in either
        // direction is a speck of noise rather than a line, and recognising one
        // costs an inference to be told so.
        constexpr auto k_detectionMinimumBoxSide = uint32{3};

        // PreProcess.NormalizeImage: (value / 255 - mean) / deviation, channel
        // by channel. The order is BGR because PreProcess.DecodeImage declares
        // img_mode BGR and nothing between it and the model reorders the
        // channels, so mean[0] belongs to BLUE. Those are the ImageNet RGB
        // statistics applied to BGR data, which looks like a bug and is not one:
        // the model was TRAINED through this same transform list, so
        // reproducing it exactly is the only thing that makes its weights mean
        // anything.
        constexpr auto k_detectionChannelMean      = std::array{0.485, 0.456, 0.406};
        constexpr auto k_detectionChannelDeviation = std::array{0.229, 0.224, 0.225};

        // PreProcess.DetResizeForTest is `null` in the shipped config, which
        // means PaddleOCR's own defaults for that op: limit_type "max" and
        // limit_side_len 960. A region whose longest side is already inside 960
        // is not scaled up; a longer one is scaled down until it fits.
        constexpr auto k_detectionLimitSide = uint32{960};

        // ...and then both sides are rounded to a multiple of 32, with 32 as the
        // floor. That is not a preference either: DB's backbone downsamples five
        // times, so an input whose sides are not multiples of 32 cannot be
        // reassembled into a map the size of its own input.
        constexpr auto k_detectionAlignment = uint32{32};

        [[nodiscard]]
        auto invalid(std::string message) -> std::unexpected<Error>
        {
            return fail(AutomationErrorKind::InvalidResource, std::move(message));
        }

        [[nodiscard]]
        auto external(std::string message) -> std::unexpected<Error>
        {
            return fail(AutomationErrorKind::ExternalFailure, std::move(message));
        }

        // The characters a recognition model can emit, read from the inference
        // config that ships beside its weights.
        //
        // A dictionary that fails to load is not a degraded engine, it is an
        // engine that would spell every output wrong while failing nothing, so
        // every path here refuses rather than returning what it managed to read.
        [[nodiscard]]
        auto loadCharacterDictionary(
            std::filesystem::path const& path
        ) -> Result<std::vector<std::string>>
        {
            auto stream = std::ifstream{path, std::ios::binary};
            if (!stream.is_open())
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    "cannot open the recognition config '" + path.string() + "'"
                );
            }

            auto characters = std::vector<std::string>{};
            auto line       = std::string{};
            auto collecting = false;
            while (std::getline(stream, line))
            {
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }

                if (!collecting)
                {
                    collecting = line.find("character_dict") != std::string::npos;
                    continue;
                }

                auto const first = line.find_first_not_of(" \t");
                if (first == std::string::npos || line[first] != '-')
                {
                    // The list ended. Stop rather than scan on: a later key
                    // could hold a dash of its own, and reading it as a
                    // character would poison every class index after it.
                    break;
                }

                auto entry = line.substr(first + 1);
                auto const start = entry.find_first_not_of(' ');
                entry = start == std::string::npos ? std::string{} : entry.substr(start);

                // A quoted scalar is how the config spells an entry YAML would
                // otherwise read as syntax -- a space, a dash, a quote.
                if (
                    entry.size() >= 2
                    && (entry.front() == '\'' || entry.front() == '"')
                    && entry.back() == entry.front()
                )
                {
                    auto const quote = entry.front();
                    entry            = entry.substr(1, entry.size() - 2);

                    // '' is the one escape a single-quoted YAML scalar has, and
                    // the shipped dictionary spells its apostrophe class that
                    // way. Left doubled it misspells every readout that holds an
                    // apostrophe, and fails nothing.
                    if (quote == '\'')
                    {
                        for (
                            auto found = entry.find("''");
                            found != std::string::npos;
                            found = entry.find("''", found + 1U)
                        )
                        {
                            entry.erase(found, 1U);
                        }
                    }
                }
                characters.emplace_back(std::move(entry));
            }

            if (characters.empty())
            {
                return invalid(
                    "the recognition config '" + path.string()
                    + "' carries no character_dict"
                );
            }
            return characters;
        }

        // One line of pixels prepared for the recognition model: BGR planar
        // float in [-1, 1], one row per channel, already resized to the height
        // the graph expects.
        struct RecognitionInput final
        {
            std::vector<float> tensor{};
            uint32             width{};
        };

        // Bilinear sample of `source` at a fractional position, per channel.
        //
        // Bilinear rather than nearest: a nearest sample of shrunk text drops
        // whole stems of a glyph.
        [[nodiscard]]
        auto sampleChannels(
            BgraImage const& source,
            PixelRect const& rect,
            double sourceX,
            double sourceY
        ) noexcept -> std::array<double, 3>
        {
            auto const clampAxis = [](double value, uint32 limit) noexcept -> double
            {
                auto const highest = static_cast<double>(limit) - 1.0;
                return std::clamp(value, 0.0, highest < 0.0 ? 0.0 : highest);
            };

            auto const x = clampAxis(sourceX, rect.width());
            auto const y = clampAxis(sourceY, rect.height());

            auto const leftIndex   = static_cast<uint32>(x);
            auto const topIndex    = static_cast<uint32>(y);
            auto const rightIndex  = std::min(leftIndex + 1U, rect.width() - 1U);
            auto const bottomIndex = std::min(topIndex + 1U, rect.height() - 1U);

            auto const xFraction = x - static_cast<double>(leftIndex);
            auto const yFraction = y - static_cast<double>(topIndex);

            auto const at = [&source, &rect](uint32 px, uint32 py) noexcept -> Bgra8Pixel
            {
                return source.pixelAt(rect.x() + px, rect.y() + py);
            };

            auto const topLeft     = at(leftIndex, topIndex);
            auto const topRight    = at(rightIndex, topIndex);
            auto const bottomLeft  = at(leftIndex, bottomIndex);
            auto const bottomRight = at(rightIndex, bottomIndex);

            auto const blend = [xFraction, yFraction](
                uint8 tl,
                uint8 tr,
                uint8 bl,
                uint8 br
            ) noexcept -> double
            {
                auto const top =
                    static_cast<double>(tl) * (1.0 - xFraction)
                    + static_cast<double>(tr) * xFraction;
                auto const bottom =
                    static_cast<double>(bl) * (1.0 - xFraction)
                    + static_cast<double>(br) * xFraction;
                return top * (1.0 - yFraction) + bottom * yFraction;
            };

            return {
                blend(topLeft.blue, topRight.blue, bottomLeft.blue, bottomRight.blue),
                blend(topLeft.green, topRight.green, bottomLeft.green, bottomRight.green),
                blend(topLeft.red, topRight.red, bottomLeft.red, bottomRight.red),
            };
        }

        // Resizes `rect` of `image` to the model's strip and normalizes it.
        //
        // The channel order is BGR and the normalization is (v / 255 - 0.5) /
        // 0.5. Both come from the training preprocessing rather than from taste:
        // PaddleOCR decodes with OpenCV, which yields BGR, and a model fed RGB
        // reads plausible nonsense rather than failing.
        [[nodiscard]]
        auto buildRecognitionInput(
            BgraImage const& image,
            PixelRect const& rect
        ) -> Result<RecognitionInput>
        {
            if (rect.width() == 0U || rect.height() == 0U)
            {
                return invalid("an ocr read needs a rect with area");
            }

            auto const aspect =
                static_cast<double>(rect.width()) / static_cast<double>(rect.height());
            auto const scaled = std::ceil(static_cast<double>(k_recognitionHeight) * aspect);
            auto const width  = std::clamp(
                static_cast<uint32>(scaled),
                k_minimumRecognitionWidth,
                k_maximumRecognitionWidth
            );

            auto const plane = static_cast<std::size_t>(width) * k_recognitionHeight;
            auto input = RecognitionInput{
                .tensor = std::vector<float>(plane * 3U, 0.0F),
                .width  = width,
            };

            auto const xStep =
                static_cast<double>(rect.width()) / static_cast<double>(width);
            auto const yStep =
                static_cast<double>(rect.height()) / static_cast<double>(k_recognitionHeight);

            for (auto row = uint32{0}; row < k_recognitionHeight; ++row)
            {
                // Sampling the pixel centre rather than its corner; a corner
                // sample shifts the whole strip half a pixel toward the origin.
                auto const sourceY = (static_cast<double>(row) + 0.5) * yStep - 0.5;
                for (auto column = uint32{0}; column < width; ++column)
                {
                    auto const sourceX = (static_cast<double>(column) + 0.5) * xStep - 0.5;
                    auto const channels = sampleChannels(image, rect, sourceX, sourceY);

                    auto const offset =
                        static_cast<std::size_t>(row) * width + column;
                    for (auto channel = std::size_t{0}; channel < 3U; ++channel)
                    {
                        auto const normalized = channels[channel] / 127.5 - 1.0;
                        input.tensor[channel * plane + offset] =
                            static_cast<float>(normalized);
                    }
                }
            }
            return input;
        }

        struct DecodedLine final
        {
            std::string text{};
            uint32      confidenceBp{};
        };

        // Greedy CTC decode: take each timestep's most likely class, drop the
        // blank, and collapse a run of one class into one character.
        //
        // Greedy rather than beam search: this reads short UI labels a model
        // scores at 0.99 and above; revisit against a measured case where the
        // top path is wrong and a lower one is right.
        [[nodiscard]]
        auto decodeCtc(
            std::span<float const> scores,
            std::size_t timesteps,
            std::size_t classes,
            std::vector<std::string> const& characters
        ) -> DecodedLine
        {
            auto decoded    = DecodedLine{};
            auto confidence = double{0.0};
            auto emitted    = std::size_t{0};
            auto previous   = classes;

            for (auto step = std::size_t{0}; step < timesteps; ++step)
            {
                auto const row = scores.subspan(step * classes, classes);
                auto const best =
                    static_cast<std::size_t>(
                        std::ranges::distance(row.begin(), std::ranges::max_element(row))
                    );

                if (best != k_ctcBlankClass && best != previous)
                {
                    auto const entry = best - 1U;
                    if (entry < characters.size())
                    {
                        decoded.text += characters[entry];
                    }
                    else
                    {
                        // The class past the dictionary is the space the models
                        // append. Spelling it here rather than padding the
                        // dictionary keeps the file on disk equal to the file
                        // the release published.
                        decoded.text += ' ';
                    }
                    confidence += static_cast<double>(row[best]);
                    ++emitted;
                }
                previous = best;
            }

            if (emitted != 0U)
            {
                auto const mean = confidence / static_cast<double>(emitted);
                decoded.confidenceBp =
                    static_cast<uint32>(std::lround(mean * k_basisPointScale));
            }
            return decoded;
        }

        // One region of pixels prepared for the detection model: BGR planar
        // float, normalized, at the size the resize rule below chose for it.
        struct DetectionInput final
        {
            std::vector<float> tensor{};
            uint32             width{};
            uint32             height{};
        };

        // The size DetResizeForTest scales a region to before the model sees it.
        struct DetectionExtent final
        {
            uint32 width{};
            uint32 height{};
        };

        [[nodiscard]]
        auto detectionExtentFor(PixelRect const& rect) noexcept -> DetectionExtent
        {
            auto const longest = std::max(rect.width(), rect.height());
            auto const ratio   = longest > k_detectionLimitSide
                ? static_cast<double>(k_detectionLimitSide) / static_cast<double>(longest)
                : 1.0;

            // Truncate the scaled extent, THEN round that to the alignment.
            // That is the order DetResizeForTest applies the two in, and it is
            // not interchangeable with rounding the product directly: the two
            // disagree by a whole 32-pixel step on some sizes, which moves every
            // box the map produces.
            auto const align = [](uint32 extent) noexcept -> uint32
            {
                // Cast through the project alias rather than using llround's own
                // return type: that is `long long`, which is a DIFFERENT type
                // from int64 on the platforms where int64 is `long`, and the
                // std::max below would then fail to deduce.
                auto const steps = static_cast<int64>(
                    std::llround(
                        static_cast<double>(extent)
                        / static_cast<double>(k_detectionAlignment)
                    )
                );
                auto const aligned = steps * static_cast<int64>(k_detectionAlignment);
                return static_cast<uint32>(
                    std::max(aligned, static_cast<int64>(k_detectionAlignment))
                );
            };
            auto const scaled = [ratio](uint32 extent) noexcept -> uint32
            {
                return static_cast<uint32>(static_cast<double>(extent) * ratio);
            };

            return DetectionExtent{
                .width  = align(scaled(rect.width())),
                .height = align(scaled(rect.height())),
            };
        }

        [[nodiscard]]
        auto buildDetectionInput(
            BgraImage const& image,
            PixelRect const& rect
        ) -> Result<DetectionInput>
        {
            if (rect.width() == 0U || rect.height() == 0U)
            {
                return invalid("an ocr read needs a rect with area");
            }

            auto const extent = detectionExtentFor(rect);
            auto const plane  = static_cast<std::size_t>(extent.width) * extent.height;
            auto input = DetectionInput{
                .tensor = std::vector<float>(plane * 3U, 0.0F),
                .width  = extent.width,
                .height = extent.height,
            };

            auto const xStep =
                static_cast<double>(rect.width()) / static_cast<double>(extent.width);
            auto const yStep =
                static_cast<double>(rect.height()) / static_cast<double>(extent.height);

            for (auto row = uint32{0}; row < extent.height; ++row)
            {
                // Pixel centre rather than corner, for the reason the
                // recognition resize samples centres: a corner sample shifts the
                // whole region half a pixel toward the origin, and every box the
                // map produces inherits the shift.
                auto const sourceY = (static_cast<double>(row) + 0.5) * yStep - 0.5;
                for (auto column = uint32{0}; column < extent.width; ++column)
                {
                    auto const sourceX = (static_cast<double>(column) + 0.5) * xStep - 0.5;
                    auto const channels = sampleChannels(image, rect, sourceX, sourceY);

                    auto const offset =
                        static_cast<std::size_t>(row) * extent.width + column;
                    for (auto channel = std::size_t{0}; channel < 3U; ++channel)
                    {
                        auto const normalized =
                            (channels[channel] / 255.0 - k_detectionChannelMean[channel])
                            / k_detectionChannelDeviation[channel];
                        input.tensor[channel * plane + offset] =
                            static_cast<float>(normalized);
                    }
                }
            }
            return input;
        }

        // One run of above-threshold cells in the probability map, and enough of
        // its statistics to score it. The edges are inclusive because they are
        // discovered cell by cell.
        struct MapComponent final
        {
            uint32 left{};
            uint32 top{};
            uint32 right{};
            uint32 bottom{};

            double probabilitySum{};
            uint64 cellCount{};
        };

        // Every connected run of above-threshold cells in `map`, in raster order.
        //
        // Diverges from DBPostProcess, which fits a minimum-area ROTATED
        // rectangle to each cv2.findContours contour: this keeps each run's
        // axis-aligned bounding box instead, which is the same box on the
        // axis-aligned desktop UI this reads and is what ocr::TextLine's
        // PixelRect can carry.
        [[nodiscard]]
        auto findMapComponents(
            std::span<float const> map,
            uint32 width,
            uint32 height
        ) -> std::vector<MapComponent>
        {
            auto const cells = static_cast<std::size_t>(width) * height;

            auto components = std::vector<MapComponent>{};
            // A flag per cell rather than a byte per cell: what is stored is
            // "seen or not", and uint8 is this project's spelling for a byte
            // with NUMERIC meaning.
            auto visited = std::vector<bool>(cells, false);
            auto pending = std::vector<std::size_t>{};

            for (auto seed = std::size_t{0}; seed < cells; ++seed)
            {
                if (visited[seed] || map[seed] <= k_detectionMapThreshold)
                {
                    continue;
                }
                if (components.size() >= k_detectionMaxCandidates)
                {
                    // PostProcess.max_candidates, applied before anything is
                    // scored exactly as PaddleOCR applies it. The order this
                    // truncates in is raster order rather than cv2's contour
                    // order, so which candidates survive a map that overflows
                    // differs -- but it is deterministic, which is the property
                    // this project needs from it.
                    break;
                }

                auto component = MapComponent{
                    .left   = width,
                    .top    = height,
                    .right  = 0,
                    .bottom = 0,
                };

                visited[seed] = true;
                pending.emplace_back(seed);
                while (!pending.empty())
                {
                    auto const cell = pending.back();
                    pending.pop_back();

                    auto const x = static_cast<uint32>(cell % width);
                    auto const y = static_cast<uint32>(cell / width);

                    component.left   = std::min(component.left, x);
                    component.top    = std::min(component.top, y);
                    component.right  = std::max(component.right, x);
                    component.bottom = std::max(component.bottom, y);
                    component.probabilitySum += static_cast<double>(map[cell]);
                    ++component.cellCount;

                    auto const lowX  = x == 0U ? uint32{0} : x - 1U;
                    auto const highX = std::min(x + 1U, width - 1U);
                    auto const lowY  = y == 0U ? uint32{0} : y - 1U;
                    auto const highY = std::min(y + 1U, height - 1U);
                    for (auto ny = lowY; ny <= highY; ++ny)
                    {
                        for (auto nx = lowX; nx <= highX; ++nx)
                        {
                            auto const neighbour =
                                static_cast<std::size_t>(ny) * width + nx;
                            if (
                                !visited[neighbour]
                                && map[neighbour] > k_detectionMapThreshold
                            )
                            {
                                visited[neighbour] = true;
                                pending.emplace_back(neighbour);
                            }
                        }
                    }
                }

                components.emplace_back(component);
            }

            return components;
        }

        // The rectangle `component` names on the image the region was read from,
        // or nothing when it is too small or too uncertain to be a line.
        //
        // The three steps and their order are DBPostProcess's.
        //
        // The score is the mean over the component's OWN cells, where PaddleOCR
        // takes the mean over the whole bounding box masked by the contour. For
        // an axis-aligned run they differ only by the background cells the box
        // encloses and the contour excludes, so this reads very slightly higher
        // and is therefore very slightly more permissive.
        [[nodiscard]]
        auto boxFromComponent(
            MapComponent const& component,
            DetectionExtent const& extent,
            PixelRect const& rect
        ) -> std::optional<PixelRect>
        {
            auto const mapWidth  = component.right - component.left + 1U;
            auto const mapHeight = component.bottom - component.top + 1U;
            if (
                std::min(mapWidth, mapHeight) < k_detectionMinimumBoxSide
                || component.cellCount == 0U
            )
            {
                return std::nullopt;
            }

            auto const score =
                component.probabilitySum / static_cast<double>(component.cellCount);
            if (score < k_detectionBoxThreshold)
            {
                return std::nullopt;
            }

            // The unclip offset for a rectangle: area * ratio / perimeter, moved
            // outward on all four edges. PaddleOCR re-fits a minimum-area box
            // afterwards and rejects a degenerate result; an axis-aligned
            // rectangle that only ever grows cannot become degenerate, so there
            // is no second size check here.
            auto const area      = static_cast<double>(mapWidth) * mapHeight;
            auto const perimeter = 2.0 * (static_cast<double>(mapWidth) + mapHeight);
            auto const distance  = area * k_detectionUnclipRatio / perimeter;

            auto const xScale =
                static_cast<double>(rect.width()) / static_cast<double>(extent.width);
            auto const yScale =
                static_cast<double>(rect.height()) / static_cast<double>(extent.height);

            auto const project = [](
                double mapEdge,
                double scale,
                uint32 limit
            ) noexcept -> uint32
            {
                // int64 rather than llround's own `long long`, for the reason
                // the alignment cast above states.
                auto const projected = static_cast<int64>(std::llround(mapEdge * scale));
                return static_cast<uint32>(
                    std::clamp(projected, int64{0}, static_cast<int64>(limit))
                );
            };

            auto const left = project(
                static_cast<double>(component.left) - distance,
                xScale,
                rect.width()
            );
            auto const top = project(
                static_cast<double>(component.top) - distance,
                yScale,
                rect.height()
            );
            auto const right = project(
                static_cast<double>(component.right + 1U) + distance,
                xScale,
                rect.width()
            );
            auto const bottom = project(
                static_cast<double>(component.bottom + 1U) + distance,
                yScale,
                rect.height()
            );
            if (right <= left || bottom <= top)
            {
                return std::nullopt;
            }

            // Back into the image's own coordinates, which is where TextLine
            // reports bounds and where a caller's click has to land.
            auto box = PixelRect::create(
                rect.x() + left,
                rect.y() + top,
                right - left,
                bottom - top
            );
            if (!box)
            {
                return std::nullopt;
            }
            return *box;
        }

        // The Readout ordering contract, applied to the boxes before any of them
        // is recognised: top to bottom, then left to right.
        //
        // No row-grouping tolerance, unlike PaddleOCR's sorted_boxes, which
        // treats two boxes within ten pixels vertically as one row and sorts
        // those by x. The screens this reads include a grid that scrolls
        // CONTINUOUSLY, so its rows land at arbitrary offsets and a tolerance
        // would group a different set of boxes on every frame -- which is
        // exactly the unordered traversal the determinism rule forbids. The
        // width and height break the remaining ties so the order is total on any
        // two distinct boxes.
        [[nodiscard]]
        auto sortedDetectedBoxes(std::vector<PixelRect> boxes) -> std::vector<PixelRect>
        {
            std::ranges::sort(
                boxes,
                [](PixelRect const& left, PixelRect const& right) noexcept -> bool
                {
                    return std::tuple{left.y(), left.x(), left.height(), left.width()}
                        < std::tuple{right.y(), right.x(), right.height(), right.width()};
                }
            );
            return boxes;
        }

        // The ONNX Runtime adapter.
        //
        // Ort's C++ API reports failures by throwing, and this project reports
        // them in a Result. Every entry point below therefore catches at this
        // boundary and converts; nothing above modules/ocr sees an Ort type or
        // an Ort exception.
        class OnnxEngine final : public IOcrEngine
        {
        public:
            // The detection half of one engine: the session and the two IO names
            // that go with it, which are useless apart. Nested and public only
            // because createOnnxEngine builds one and hands it to the
            // constructor; nothing else names it.
            struct Detection final
            {
                Ort::Session session;

                std::string inputName{};
                std::string outputName{};
            };

        private:
            // Declared before the session and destroyed after it: an Ort::Session
            // must not outlive the environment it was created in.
            Ort::Env                         m_environment;
            Ort::SessionOptions              m_sessionOptions;
            Ort::Session                     m_recognition;
            Ort::AllocatorWithDefaultOptions m_allocator{};

            // Empty for an engine built with no detection model, which is the
            // recognition-only engine every single-line caller already has.
            // Declared after m_recognition so it is torn down before the
            // environment both sessions were created in.
            std::optional<Detection> m_detection;

            std::vector<std::string> m_characters;

            // Owned copies of the model's own IO names. Ort hands these back in
            // allocator-owned storage whose lifetime is not the session's, so
            // holding the char* it returns would be a dangling read on the first
            // inference.
            std::string m_inputName;
            std::string m_outputName;

            // "onnxruntime/<recognition model file name>", plus the detection
            // model's DIRECTORY when there is one. The runtime and the weights
            // together are what decides how a line decodes, so a trace that names
            // only one of them still cannot explain a changed result -- and the
            // detection half is named by its directory because both models in the
            // release are files called inference.onnx, so the file name alone
            // would say nothing at all.
            std::string m_identity;

            [[nodiscard]]
            auto recognizeLine(
                BgraImage const& image,
                PixelRect const& rect
            ) -> Result<TextLine>
            {
                UF_TRY_VALUE(input, buildRecognitionInput(image, rect));

                auto const shape = std::array<int64_t, 4>{
                    1,
                    3,
                    static_cast<int64_t>(k_recognitionHeight),
                    static_cast<int64_t>(input.width),
                };
                auto memory = Ort::MemoryInfo::CreateCpu(
                    OrtDeviceAllocator,
                    OrtMemTypeCPU
                );
                auto tensor = Ort::Value::CreateTensor<float>(
                    memory,
                    input.tensor.data(),
                    input.tensor.size(),
                    shape.data(),
                    shape.size()
                );

                char const* inputNames[]  = {m_inputName.c_str()};
                char const* outputNames[] = {m_outputName.c_str()};

                auto outputs = m_recognition.Run(
                    Ort::RunOptions{nullptr},
                    inputNames,
                    &tensor,
                    1,
                    outputNames,
                    1
                );
                if (outputs.empty() || !outputs.front().IsTensor())
                {
                    return external("the recognition model returned no tensor");
                }

                auto const info      = outputs.front().GetTensorTypeAndShapeInfo();
                auto const dimensions = info.GetShape();
                if (dimensions.size() != 3U)
                {
                    return external(
                        "the recognition model returned a rank-"
                        + std::to_string(dimensions.size())
                        + " tensor; this adapter feeds a model shaped [1, T, C]"
                    );
                }

                auto const timesteps = static_cast<std::size_t>(dimensions[1]);
                auto const classes   = static_cast<std::size_t>(dimensions[2]);
                if (classes <= m_characters.size())
                {
                    return external(
                        "the recognition model emits " + std::to_string(classes)
                        + " classes but its config lists " + std::to_string(m_characters.size())
                        + " characters; the two files do not belong together"
                    );
                }

                if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
                {
                    return external(
                        "the recognition model emits a non-float tensor; this "
                        "adapter reads float32"
                    );
                }

                // SAFETY: the tensor was just confirmed to be rank 3 with
                // float32 elements, so the product of the reported dimensions is
                // the count of floats GetTensorData<float> points at; without
                // the element-type check it would count another type's elements.
                // The span is read before `outputs` leaves scope, so it never
                // outlives the storage Ort owns.
                auto const scores = std::span<float const>{
                    outputs.front().GetTensorData<float>(),
                    timesteps * classes,
                };

                auto decoded = decodeCtc(scores, timesteps, classes, m_characters);
                return TextLine{
                    .text         = std::move(decoded.text),
                    .bounds       = rect,
                    .confidenceBp = decoded.confidenceBp,
                };
            }

            // Every line the detector finds inside `rect`, in image coordinates
            // and in the Readout order, with nothing recognised yet.
            //
            // It is separate from readBlock so the caller's line budget can be
            // answered between the two inferences: locating is one run over the
            // region and recognising is one run PER line, so a region holding
            // more lines than the caller can pay for is refused having cost one
            // inference rather than one per line.
            [[nodiscard]]
            auto locateLines(
                BgraImage const& image,
                PixelRect const& rect
            ) -> Result<std::vector<PixelRect>>
            {
                UF_TRY_VALUE(input, buildDetectionInput(image, rect));

                auto const shape = std::array<int64_t, 4>{
                    1,
                    3,
                    static_cast<int64_t>(input.height),
                    static_cast<int64_t>(input.width),
                };
                auto memory = Ort::MemoryInfo::CreateCpu(
                    OrtDeviceAllocator,
                    OrtMemTypeCPU
                );
                auto tensor = Ort::Value::CreateTensor<float>(
                    memory,
                    input.tensor.data(),
                    input.tensor.size(),
                    shape.data(),
                    shape.size()
                );

                char const* inputNames[]  = {m_detection->inputName.c_str()};
                char const* outputNames[] = {m_detection->outputName.c_str()};

                auto outputs = m_detection->session.Run(
                    Ort::RunOptions{nullptr},
                    inputNames,
                    &tensor,
                    1,
                    outputNames,
                    1
                );
                if (outputs.empty() || !outputs.front().IsTensor())
                {
                    return external("the detection model returned no tensor");
                }

                auto const info       = outputs.front().GetTensorTypeAndShapeInfo();
                auto const dimensions = info.GetShape();
                if (dimensions.size() != 4U || dimensions[1] != 1)
                {
                    return external(
                        "the detection model returned a rank-"
                        + std::to_string(dimensions.size())
                        + " tensor; this adapter reads a model shaped "
                          "[1, 1, H, W]"
                    );
                }

                auto const mapHeight = static_cast<uint32>(dimensions[2]);
                auto const mapWidth  = static_cast<uint32>(dimensions[3]);

                if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
                {
                    return external(
                        "the detection model emits a non-float tensor; this "
                        "adapter reads float32"
                    );
                }

                // SAFETY: the value was just confirmed to be a rank-4 tensor of
                // float32 elements, so the product of the reported dimensions is
                // the count of floats GetTensorData<float> points at; without
                // the element-type check it would count another type's elements.
                // The span is consumed before `outputs` leaves scope, so it
                // never outlives the storage Ort owns.
                auto const map = std::span<float const>{
                    outputs.front().GetTensorData<float>(),
                    static_cast<std::size_t>(mapWidth) * mapHeight,
                };

                auto const extent = DetectionExtent{
                    .width  = mapWidth,
                    .height = mapHeight,
                };
                auto boxes = std::vector<PixelRect>{};
                for (auto const& component : findMapComponents(map, mapWidth, mapHeight))
                {
                    auto const box = boxFromComponent(component, extent, rect);
                    if (box.has_value())
                    {
                        boxes.emplace_back(*box);
                    }
                }
                return sortedDetectedBoxes(std::move(boxes));
            }

            [[nodiscard]]
            auto readSingleLine(
                BgraImage const& image,
                PixelRect const& rect
            ) -> Result<Readout>
            {
                UF_TRY_VALUE(line, recognizeLine(image, rect));
                auto readout = Readout{};
                if (!line.text.empty())
                {
                    readout.lines.emplace_back(std::move(line));
                }
                return readout;
            }

            [[nodiscard]]
            auto readBlock(
                BgraImage const& image,
                PixelRect const& rect,
                std::optional<uint32> maximumLines
            ) -> Result<Readout>
            {
                if (!m_detection.has_value())
                {
                    return fail(
                        AutomationErrorKind::UnsupportedCapability,
                        "this engine was built with no detection model, so it "
                        "cannot find the lines in a region; supply one, or pass "
                        "TextLayout::SingleLine with a rect around one line"
                    );
                }

                UF_TRY_VALUE(boxes, locateLines(image, rect));
                if (maximumLines.has_value() && boxes.size() > *maximumLines)
                {
                    return fail(
                        AutomationErrorKind::RecognitionIncomplete,
                        "the region holds " + std::to_string(boxes.size())
                        + " lines and this read may recognise at most "
                        + std::to_string(*maximumLines)
                        + "; a partly-read region establishes nothing about the "
                          "lines nobody looked at"
                    );
                }

                auto readout = Readout{};
                readout.lines.reserve(boxes.size());
                for (auto const& box : boxes)
                {
                    UF_TRY_VALUE(line, recognizeLine(image, box));
                    // KEPT EVEN WHEN IT DECODED TO NOTHING, which is the one
                    // place this layout deliberately differs from SingleLine.
                    // There, the caller asserted the rect holds a line, so an
                    // empty decode is the answer "there is no text here". Here
                    // the DETECTOR asserted it, so an empty decode is a failure
                    // to read something the frame says is there -- the most
                    // interesting thing that can happen to a block read, and
                    // dropping it would hide it. It also keeps the line count
                    // equal to the recognitions this call performed, which is
                    // what a caller paying for reads out of a budget charges.
                    readout.lines.emplace_back(std::move(line));
                }
                return readout;
            }

        public:
            OnnxEngine(
                Ort::Env environment,
                Ort::SessionOptions sessionOptions,
                Ort::Session recognition,
                std::optional<Detection> detection,
                std::vector<std::string> characters,
                std::string inputName,
                std::string outputName,
                std::string identity
            ) noexcept
                : m_environment{std::move(environment)}
                , m_sessionOptions{std::move(sessionOptions)}
                , m_recognition{std::move(recognition)}
                , m_detection{std::move(detection)}
                , m_characters{std::move(characters)}
                , m_inputName{std::move(inputName)}
                , m_outputName{std::move(outputName)}
                , m_identity{std::move(identity)}
            {
            }

            [[nodiscard]]
            auto identity() const noexcept -> std::string_view override
            {
                return m_identity;
            }

            [[nodiscard]]
            auto read(
                BgraImage const& image,
                ReadSpec const& spec
            ) -> Result<Readout> override
            {
                auto const whole = PixelRect::create(0, 0, image.width(), image.height());
                if (!whole)
                {
                    return invalid("the image handed to an ocr read has no area");
                }
                auto const rect = spec.rect.value_or(*whole);

                if (
                    rect.x() + rect.width() > image.width()
                    || rect.y() + rect.height() > image.height()
                )
                {
                    return invalid("the ocr rect does not fit inside the image");
                }

                try
                {
                    switch (spec.layout)
                    {
                    case TextLayout::SingleLine:
                        return readSingleLine(image, rect);
                    case TextLayout::Block:
                        return readBlock(image, rect, spec.maximumLines);
                    }

                    UF_UNREACHABLE_MSG("Unknown ocr::TextLayout value");
                }
                catch (Ort::Exception const& error)
                {
                    return external(
                        "onnxruntime failed during recognition: "
                        + std::string{error.what()}
                    );
                }
            }
        };
    }

    auto createOnnxEngine(
        OnnxEngineConfig const& config
    ) -> Result<std::unique_ptr<IOcrEngine>>
    {
        if (config.threadCount == 0U)
        {
            return invalid("an ocr engine needs at least one thread");
        }
        for (auto const& path : {config.recognitionModel, config.recognitionConfig})
        {
            if (!std::filesystem::is_regular_file(path))
            {
                return invalid("the ocr model file '" + path.string() + "' is missing");
            }
        }
        // A detection path that was SUPPLIED and is not there is a failure, on
        // the same terms as the recognition files. An absent path is the caller
        // saying it never wanted detection; a wrong one is the caller believing
        // it has it, and discovering otherwise on the first block read is
        // exactly the late discovery this construction-time check exists to
        // prevent.
        if (
            !config.detectionModel.empty()
            && !std::filesystem::is_regular_file(config.detectionModel)
        )
        {
            return invalid(
                "the ocr detection model '" + config.detectionModel.string()
                + "' is missing"
            );
        }

        UF_TRY_VALUE(characters, loadCharacterDictionary(config.recognitionConfig));

        try
        {
            auto environment = Ort::Env{ORT_LOGGING_LEVEL_WARNING, "uf-ocr"};

            auto sessionOptions = Ort::SessionOptions{};
            sessionOptions.SetIntraOpNumThreads(
                static_cast<int>(config.threadCount)
            );
            // One inference at a time by construction: this engine is called
            // from the thread that owns the run, and a second parallel op would
            // contend with it for the same cores rather than finishing sooner.
            sessionOptions.SetInterOpNumThreads(1);
            sessionOptions.SetGraphOptimizationLevel(
                GraphOptimizationLevel::ORT_ENABLE_ALL
            );

            auto session = Ort::Session{
                environment,
                config.recognitionModel.c_str(),
                sessionOptions,
            };

            if (session.GetInputCount() != 1U || session.GetOutputCount() < 1U)
            {
                return external(
                    "the recognition model does not have the single input and "
                    "output this adapter feeds"
                );
            }

            auto allocator  = Ort::AllocatorWithDefaultOptions{};
            auto inputName  = std::string{session.GetInputNameAllocated(0, allocator).get()};
            auto outputName = std::string{session.GetOutputNameAllocated(0, allocator).get()};

            auto identity  = "onnxruntime/" + config.recognitionModel.filename().string();
            auto detection = std::optional<OnnxEngine::Detection>{};
            if (!config.detectionModel.empty())
            {
                auto detectionSession = Ort::Session{
                    environment,
                    config.detectionModel.c_str(),
                    sessionOptions,
                };
                if (
                    detectionSession.GetInputCount() != 1U
                    || detectionSession.GetOutputCount() < 1U
                )
                {
                    return external(
                        "the detection model does not have the single input and "
                        "output this adapter feeds"
                    );
                }

                auto detectionInputName = std::string{
                    detectionSession.GetInputNameAllocated(0, allocator).get()
                };
                auto detectionOutputName = std::string{
                    detectionSession.GetOutputNameAllocated(0, allocator).get()
                };
                detection = OnnxEngine::Detection{
                    .session    = std::move(detectionSession),
                    .inputName  = std::move(detectionInputName),
                    .outputName = std::move(detectionOutputName),
                };
                identity +=
                    "+" + config.detectionModel.parent_path().filename().string();
            }

            return std::make_unique<OnnxEngine>(
                std::move(environment),
                std::move(sessionOptions),
                std::move(session),
                std::move(detection),
                std::move(characters),
                std::move(inputName),
                std::move(outputName),
                std::move(identity)
            );
        }
        catch (Ort::Exception const& error)
        {
            return external(
                "onnxruntime could not load '" + config.recognitionModel.string()
                + "': " + std::string{error.what()}
            );
        }
    }
}

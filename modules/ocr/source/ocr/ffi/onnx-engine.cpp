#include <ocr/onnx-engine.hpp>

#include <core/error/result.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/space.hpp>

#include <vision/bgra-image.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
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
        // Parsed line by line rather than through a YAML library. What is needed
        // is one list of scalars under one known key, and the file's own shape
        // is fixed by the release that produced it; a parser for the rest of
        // YAML would be a dependency bought to read eighteen thousand lines of
        // "  - X".
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
                    entry = entry.substr(1, entry.size() - 2);
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
        // Bilinear rather than nearest because the crop is usually shrunk, and a
        // nearest sample of shrunk text drops whole stems of a glyph. It is the
        // same reason the resize exists at all.
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
        // Greedy rather than beam search, and the reason is the input rather
        // than the algorithm's reputation: this reads short UI labels from a
        // model that scores them at 0.99 and above, where a beam explores
        // alternatives that are never chosen. Revisit against a measured case
        // where the top path is wrong and a lower one is right.
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

        // The ONNX Runtime adapter.
        //
        // Ort's C++ API reports failures by throwing, and this project reports
        // them in a Result. Every entry point below therefore catches at this
        // boundary and converts; nothing above modules/ocr sees an Ort type or
        // an Ort exception.
        class OnnxEngine final : public IOcrEngine
        {
            // Declared before the session and destroyed after it: an Ort::Session
            // must not outlive the environment it was created in.
            Ort::Env                         m_environment;
            Ort::SessionOptions              m_sessionOptions;
            Ort::Session                     m_recognition;
            Ort::AllocatorWithDefaultOptions m_allocator{};

            std::vector<std::string> m_characters;

            // Owned copies of the model's own IO names. Ort hands these back in
            // allocator-owned storage whose lifetime is not the session's, so
            // holding the char* it returns would be a dangling read on the first
            // inference.
            std::string m_inputName;
            std::string m_outputName;

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

                // SAFETY: the tensor was just confirmed to be a tensor of rank 3,
                // and Ort guarantees GetTensorData<float> yields its element
                // count as the product of the reported dimensions. The span is
                // read before `outputs` leaves scope, so it never outlives the
                // storage Ort owns.
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

        public:
            OnnxEngine(
                Ort::Env environment,
                Ort::SessionOptions sessionOptions,
                Ort::Session recognition,
                std::vector<std::string> characters,
                std::string inputName,
                std::string outputName
            ) noexcept
                : m_environment{std::move(environment)}
                , m_sessionOptions{std::move(sessionOptions)}
                , m_recognition{std::move(recognition)}
                , m_characters{std::move(characters)}
                , m_inputName{std::move(inputName)}
                , m_outputName{std::move(outputName)}
            {
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

                if (spec.layout == TextLayout::Block)
                {
                    return fail(
                        AutomationErrorKind::UnsupportedCapability,
                        "TextLayout::Block needs the detection model, which this "
                        "adapter does not run yet; pass TextLayout::SingleLine "
                        "with a rect around one line"
                    );
                }

                try
                {
                    UF_TRY_VALUE(line, recognizeLine(image, rect));
                    auto readout = Readout{};
                    if (!line.text.empty())
                    {
                        readout.lines.emplace_back(std::move(line));
                    }
                    return readout;
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

            return std::make_unique<OnnxEngine>(
                std::move(environment),
                std::move(sessionOptions),
                std::move(session),
                std::move(characters),
                std::move(inputName),
                std::move(outputName)
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

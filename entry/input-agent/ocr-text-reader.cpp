#include "ocr-text-reader.hpp"

#include "platform/windows-executable-path.hpp"
#include "text-reader.hpp"

#include <core/error/result.hpp>
#include <domain/frame.hpp>
#include <domain/space.hpp>
#include <ocr/engine.hpp>
#include <ocr/onnx-engine.hpp>

#include <expected>
#include <filesystem>
#include <memory>
#include <string_view>
#include <utility>

namespace uf::input_agent
{
    namespace
    {
        // The payload layout, spelled once here and once in
        // entry/CMakeLists.txt's staging rule. It is not in the header because
        // nothing outside this file resolves a model path: the reader takes the
        // directory it was given, and the composition root names it.
        constexpr auto k_payloadDirectoryName = std::string_view{"models"};
        constexpr auto k_recognitionDirectoryName = (
            std::string_view{"ppocr-v6-small-rec"}
        );
        constexpr auto k_recognitionModelName = (
            std::string_view{"inference.onnx"}
        );
        constexpr auto k_recognitionConfigName = (
            std::string_view{"inference.yml"}
        );
    }

    OcrTextReader::OcrTextReader(std::filesystem::path modelDirectory)
        : m_modelDirectory{std::move(modelDirectory)}
    {
    }

    auto OcrTextReader::read(
        Frame const& observation,
        PixelRect rect
    ) -> TextReadOutcome
    {
        if (!m_engine)
        {
            auto const recognition = (
                m_modelDirectory / k_recognitionDirectoryName
            );
            auto engine = ocr::createOnnxEngine(
                ocr::OnnxEngineConfig{
                    .recognitionModel = (
                        recognition / k_recognitionModelName
                    ),
                    .recognitionConfig = (
                        recognition / k_recognitionConfigName
                    ),
                }
            );
            if (!engine)
            {
                return TextReadOutcome{
                    .lines             = std::unexpected{std::move(engine).error()},
                    .readerUnavailable = true,
                };
            }
            m_engine = *std::move(engine);
        }

        auto image = frameAsBgraImage(observation);
        if (!image)
        {
            return TextReadOutcome{
                .lines             = std::unexpected{std::move(image).error()},
                .readerUnavailable = false,
            };
        }

        // SingleLine, and never Block: the caller has already said where the
        // text is, which skips detection entirely and is both the cheap path and
        // the one immune to a detector that splits one label in two. Block is
        // the layout this call grows the day the detection model is wired, and
        // the field that selects it is the one this verb grows with it.
        auto readout = m_engine->read(
            *image,
            ocr::ReadSpec{
                .rect   = rect,
                .layout = ocr::TextLayout::SingleLine,
            }
        );
        if (!readout)
        {
            return TextReadOutcome{
                .lines             = std::unexpected{std::move(readout).error()},
                .readerUnavailable = false,
            };
        }
        return TextReadOutcome{
            .lines             = std::move(readout->lines),
            .readerUnavailable = false,
        };
    }

    auto createOcrTextReader() -> Result<std::unique_ptr<IInputAgentTextReader>>
    {
        UF_TRY_VALUE(directory, platform::executableDirectory());
        return std::make_unique<OcrTextReader>(
            directory / k_payloadDirectoryName
        );
    }
}

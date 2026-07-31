#pragma once

#include "text-reader.hpp"

#include <core/error/result.hpp>
#include <domain/frame.hpp>
#include <domain/space.hpp>
#include <ocr/engine.hpp>

#include <filesystem>
#include <memory>

namespace uf::input_agent
{
    // The live text reader: PP-OCRv6_small over ONNX Runtime, loaded from the
    // model files under one directory: `<modelDirectory>/ppocr-v6-small-rec/`,
    // holding `inference.onnx` and `inference.yml`. entry/CMakeLists.txt stages
    // that tree and a release ships it, so nothing in the protocol or the
    // arguments names a filesystem layout.
    //
    // The engine is built on the FIRST READ rather than at construction, and
    // that is the whole reason this type exists rather than an engine held
    // directly. Bringing one up costs a 20 MB file and an ONNX Runtime session,
    // and two things follow from paying that lazily: a session that only
    // captures never pays it at all, and an agent whose payload is missing still
    // starts and still serves every other verb. Making it eager would turn an
    // absent model into a launch failure for uses that never asked to read.
    //
    // A creation FAILURE is deliberately not cached, only a success. Retrying
    // costs one existence check when the payload is absent, and it means a
    // payload restored beside a running agent is picked up by the next read
    // instead of requiring a restart.
    class OcrTextReader final : public IInputAgentTextReader
    {
        std::filesystem::path m_modelDirectory;

        std::unique_ptr<ocr::IOcrEngine> m_engine{};

    public:
        // The directory is stated rather than discovered, so a caller that has
        // no payload -- a test, or a future agent pointed at another install --
        // can say so instead of being handed whatever sits beside the binary.
        explicit OcrTextReader(std::filesystem::path modelDirectory);

        [[nodiscard]]
        auto read(
            Frame const& observation,
            PixelRect rect
        ) -> TextReadOutcome override;
    };

    // The reader a shipped agent runs with: the payload beside its own
    // executable.
    //
    // It returns a Result because naming the running image can fail, and that
    // failure is not the one this whole design routes around. A missing model is
    // answered on a results line and leaves the run serving; a process that
    // cannot say where it was loaded from has no payload path to try at all.
    [[nodiscard]]
    auto createOcrTextReader() -> Result<std::unique_ptr<IInputAgentTextReader>>;
}

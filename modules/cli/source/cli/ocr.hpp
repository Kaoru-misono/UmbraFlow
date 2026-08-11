#pragma once

#include "args.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <ocr/text.hpp>

#include <string>
#include <vector>

namespace uf::cli
{
    // What one read measured, and the extent every rectangle in it is measured
    // against. The extent travels with the lines rather than being left to the
    // caller's memory of the file: a consumer holding only this document has to
    // be able to tell a rectangle that hugs the right edge from one that would
    // have overrun it.
    struct ImageText final
    {
        uint32 width{};
        uint32 height{};

        // Top to bottom, then left to right, which is ocr::Readout's contract
        // rather than this verb's convenience. A line's position in the
        // sequence is therefore stable enough to name one by.
        std::vector<ocr::TextLine> lines{};
    };

    // Implemented per host, exactly as `explore` and `targets` are: the OCR
    // adapter is compiled only where the inference runtime payload exists.
    [[nodiscard]] auto ocrProduct(OcrArgs const& args) -> Result<ImageText>;

    // The RFC 8785 form of one measurement, which is the whole of what this
    // verb prints.
    //
    // Canonical rather than merely valid, because the consumer is expected to
    // hash and diff it. Member order is the half of that a hand-written emitter
    // gets wrong without failing anything, so the order here is computed by
    // json::canonicalBytes rather than written out.
    [[nodiscard]] auto formatImageText(ImageText const& text) -> std::string;
}

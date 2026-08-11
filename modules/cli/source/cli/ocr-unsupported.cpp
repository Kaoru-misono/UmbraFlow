#include "ocr.hpp"

#include <core/error/result.hpp>

#include <domain/error.hpp>

namespace uf::cli
{
    // Reading text goes through the OCR adapter, which is compiled only where
    // the inference runtime payload is. Every other host keeps the binary
    // buildable and reports the read as unsupported rather than failing to
    // link, exactly as `explore` and `targets` do. Formatting the answer is
    // host-neutral and stays in ocr.cpp, so what a caller would receive is
    // still tested here.
    auto ocrProduct(OcrArgs const&) -> Result<ImageText>
    {
        return fail(
            AutomationErrorKind::UnsupportedCapability,
            "umbra-flow ocr is unsupported on this host"
        );
    }
}

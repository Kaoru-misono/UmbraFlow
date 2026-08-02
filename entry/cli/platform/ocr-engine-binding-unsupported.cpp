#include "ocr-engine-binding.hpp"

#include <core/error/result.hpp>

#include <domain/error.hpp>

#include <ocr/engine.hpp>

#include <filesystem>
#include <memory>
#include <optional>

namespace uf::cli::platform
{
    // The recognition adapter builds only where its inference runtime does, so
    // every other host keeps the binary buildable and reports the request rather
    // than failing to link -- the same arrangement run-unsupported.cpp makes for
    // the target binding.
    //
    // An absent directory is still a null engine and NOT a failure, which is the
    // difference between this and the run path. `run`, `drive` and `explore` are
    // unsupported here outright; `check` is not, because its frames come from files
    // and its template half needs no adapter. So an operator who asked for nothing
    // gets a check that measures every template cell, and only one who claimed what
    // a region reads is told this host cannot answer that.
    auto bindOcrEngine(
        std::optional<std::filesystem::path> const& modelDirectory
    ) -> Result<std::unique_ptr<ocr::IOcrEngine>>
    {
        if (!modelDirectory.has_value())
        {
            return std::unique_ptr<ocr::IOcrEngine>{};
        }

        return fail(
            AutomationErrorKind::UnsupportedCapability,
            "--ocr-models is unsupported on this host: the text recognition "
            "adapter builds only where its inference runtime does"
        );
    }
}

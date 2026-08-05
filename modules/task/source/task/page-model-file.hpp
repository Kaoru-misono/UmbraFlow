#pragma once

#include <core/error/result.hpp>

#include <domain/content-hash.hpp>
#include <domain/space.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace uf::task
{
    // The project file layer two owns, named here because the host has to open it
    // before any VM exists. The trusted framework spells the same name on its own
    // side as `project.file_name`, and every case that loads a project fails
    // outright if the two disagree.
    inline constexpr auto k_pageModelFileName = std::string_view{"page-model.toml"};

    // A page model is a line-oriented text file of element rectangles, template
    // references and page rows. The largest one authored so far is fifteen
    // kilobytes; this ceiling refuses a file that is not one at all before any of
    // it is read into memory.
    inline constexpr auto k_maximumPageModelBytes = std::size_t{4} * 1024U * 1024U;

    // The only four things C++ reads out of the page model, each needed BEFORE a
    // VM exists: the fingerprint is what the engine's compatibility refusal
    // compares a live measurement against; the names are what the pre-VM AST pass
    // resolves a script's `uf.elements.<name>` and `uf.pages.<name>` literals
    // against, so a misspelled resource fails before a VM boots
    // (docs/plans/2026-07-31-script-owned-page-model.md 6); and the content hash
    // is what `run.started` stamps on the stream, so a later reader can refuse a
    // trace recorded against a model that is no longer this one.
    //
    // Everything else in the file -- capabilities, rectangles, thresholds,
    // references, edges, the falsification claims -- is layer two's to interpret,
    // and a second C++ reader would be a second opinion about what a page model
    // means. This reader knows that a section header opens a section and that a
    // `name` line names one; it knows nothing about what an element or a page IS.
    // The hash is the one fact here that does not depend on that reading at all:
    // it is over the bytes, so it identifies the whole file including everything
    // this parse skips.
    struct PageModelFacts final
    {
        // Neither of these can carry an in-class initializer: both are values with
        // no meaningful empty state, so every construction supplies them.
        ProjectFingerprint fingerprint;
        ContentHash        contentHash;

        // In declaration order, and each name distinct within its own list. The
        // pass below only ever asks whether a name is present, so order carries no
        // meaning; it is kept so a diagnostic that lists them reads like the file.
        std::vector<std::string> elementNames{};
        std::vector<std::string> pageNames{};
    };

    // Reads `text` as a page model and reports the facts above.
    //
    // It is a FLAT LINE SCAN and deliberately not a parser: every line is either a
    // section header, a `key = value` line of the section it is inside, or ignored.
    // Anything it does not recognise is skipped rather than refused, because
    // refusing would make this a second authority on the format and a newer build's
    // section would then fail to load in a way layer two would have handled. What
    // it does refuse is the absence of a fact it was asked for, and a duplicate
    // name, which would leave a literal resolving against two different rows.
    //
    // Exposed separately from the file read so it can be exercised on text.
    [[nodiscard]]
    auto parsePageModelFacts(std::string_view text) -> Result<PageModelFacts>;

    // Reads <projectRoot>/page-model.toml and parses it. The file is size-capped
    // before any bytes are read, so a project that is not one cannot force an
    // unbounded allocation.
    [[nodiscard]]
    auto readPageModelFacts(
        std::filesystem::path const& projectRoot
    ) -> Result<PageModelFacts>;
}

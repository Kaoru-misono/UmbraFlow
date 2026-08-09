#pragma once

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

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
    inline constexpr auto k_runtimeModelFileName = std::string_view{"page-model.toml"};

    // The runtime model is a line-oriented text file whose deployment envelope
    // names the targets and surfaces available to the trusted Luau layer. The
    // largest one authored so far is fifteen kilobytes; this ceiling refuses a
    // file that is not one at all before any of it is read into memory.
    inline constexpr auto k_maximumRuntimeModelBytes = std::size_t{4} * 1024U * 1024U;

    // The only five things C++ reads out of the runtime model, each needed BEFORE
    // a VM exists: the schema version selects the envelope contract; the
    // fingerprint is what the engine's compatibility refusal compares a live
    // measurement against; the target and surface names are the pre-VM resource
    // catalog; and the content hash is what `run.started` stamps on the stream.
    //
    // Everything else in the file -- locators, readers, bindings, actions,
    // transitions and offline claims -- is layer two's to interpret, and a
    // second C++ reader would be a second opinion about what a runtime model
    // means. This reader knows that a section header opens a section and that an
    // `id` line names one; it knows nothing about what a target or a surface IS.
    // The hash is the one fact here that does not depend on that reading at all:
    // it is over the bytes, so it identifies the whole file including everything
    // this parse skips.
    struct RuntimeModelEnvelope final
    {
        // The envelope contract selected by the file. This host currently
        // understands one version and still records it for diagnostics.
        uint32 schemaVersion;

        // Neither of these can carry an in-class initializer: both are values with
        // no meaningful empty state, so every construction supplies them.
        ProjectFingerprint fingerprint;
        ContentHash        contentHash;

        // In declaration order, and each ID distinct within its own list. The
        // pass below only ever asks whether an ID is present, so order carries no
        // meaning; it is kept so a diagnostic that lists them reads like the file.
        std::vector<std::string> targetIds{};
        std::vector<std::string> surfaceIds{};
    };

    // Reads `text` as the runtime model file and reports the envelope facts above.
    //
    // It is a FLAT LINE SCAN and deliberately not a parser: every line is either a
    // section header, a `key = value` line of the section it is inside, or ignored.
    // Anything it does not recognise is skipped rather than refused, because
    // refusing would make this a second authority on the format and a newer build's
    // section would then fail to load in a way layer two would have handled. What
    // it does refuse is the absence of a fact it was asked for, and a duplicate
    // name, which would leave a resource literal resolving against two different
    // rows.
    //
    // Exposed separately from the file read so it can be exercised on text.
    [[nodiscard]]
    auto parseRuntimeModelEnvelope(std::string_view text) -> Result<RuntimeModelEnvelope>;

    // Reads <projectRoot>/page-model.toml and parses it. The file is size-capped
    // before any bytes are read, so a project that is not one cannot force an
    // unbounded allocation.
    [[nodiscard]]
    auto readRuntimeModelEnvelope(
        std::filesystem::path const& projectRoot
    ) -> Result<RuntimeModelEnvelope>;
}

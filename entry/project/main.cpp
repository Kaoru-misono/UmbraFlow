#include <project/command.hpp>

#include <deployment/project-directory.hpp>

#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/safety/annotations.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <json/value.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::project_entry
{
    namespace
    {
        // The record of every declared file the build saw, as (path, sha256,
        // size) rows. It lives in the build tree beside the kit's own records
        // and is written by this executable, not by the kit: the kit cannot
        // link the loader, and the whole point of the record is that the
        // loader's confined read -- the read that will verify it later -- is
        // the one that produced it. Nothing the kit checks reads it, so the
        // kit's manifest comparisons cannot trip on a file it does not know.
        constexpr auto k_declaredFilesRecordName = std::string_view{
            "project-declared-files.json"
        };
        constexpr auto k_declaredFilesRecordSchema = std::string_view{
            "umbraflow-project-declared-files/v1"
        };

        [[nodiscard]]
        auto renderDeclaredFilesRecord(
            std::vector<uf::deployment::DeclaredProjectFile> const& rows
        ) -> std::string
        {
            auto values = std::vector<json::Value>{};
            values.reserve(rows.size());
            for (auto const& row : rows)
            {
                values.emplace_back(json::Value::ofObject({
                    {"path", json::Value::ofString(row.path)},
                    {"sha256", json::Value::ofString(row.digest.hex())},
                    {
                        "size",
                        json::Value::ofString(std::to_string(row.size)),
                    },
                }));
            }
            return json::canonicalBytes(json::Value::ofObject({
                {"files", json::Value::ofArray(std::move(values))},
                {
                    "schema",
                    json::Value::ofString(std::string{k_declaredFilesRecordSchema}),
                },
            }));
        }

        // The record is this executable's own shape, but a shape check still
        // gates every read: a record from an older build -- or one a build
        // never wrote -- must be refused rather than read into garbage.
        [[nodiscard]]
        auto readDeclaredFilesRecord(
            std::filesystem::path const& buildDirectory
        ) -> Result<std::vector<uf::deployment::DeclaredProjectFile>>
        {
            auto const path = buildDirectory / k_declaredFilesRecordName;
            auto stream     = std::ifstream{path, std::ios::binary};
            if (!stream.is_open())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "the build at \"{}\" holds no declared-file record "
                        "\"{}\": build the project before checking it",
                        buildDirectory.string(),
                        k_declaredFilesRecordName
                    )
                );
            }
            auto const text = std::string{
                std::istreambuf_iterator<char>{stream},
                std::istreambuf_iterator<char>{}
            };
            if (stream.bad())
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot read the build's declared-file record \"{}\"",
                        path.string()
                    )
                );
            }

            UF_TRY_VALUE(document, json::parse(text));
            auto const* const schema = document.find("schema");
            auto const* const files  = document.find("files");
            if (
                schema == nullptr
                || schema->string() != k_declaredFilesRecordSchema
                || files == nullptr
                || files->kind() != json::ValueKind::Array
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "the build's declared-file record \"{}\" has the wrong "
                        "shape",
                        path.string()
                    )
                );
            }

            // The record writes a size beside each digest for self-description,
            // and the read drops it, exactly as the kit's release reader drops
            // the size its artifact manifest writes (project-kit.cpp): the
            // digest pins the bytes, and a size that agrees with a digest it
            // was not derived from proves nothing.
            auto rows = std::vector<uf::deployment::DeclaredProjectFile>{};
            rows.reserve(files->items().size());
            for (auto const& item : files->items())
            {
                auto const* const rowPath   = item.find("path");
                auto const* const rowDigest = item.find("sha256");
                if (
                    rowPath == nullptr
                    || rowPath->kind() != json::ValueKind::String
                    || rowDigest == nullptr
                    || rowDigest->kind() != json::ValueKind::String
                )
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "the build's declared-file record \"{}\" has the "
                            "wrong shape",
                            path.string()
                        )
                    );
                }
                // Files in this repository spell digests as bare lowercase
                // hex -- the kit's artifact manifest, a template cut's
                // source_sha256s -- and the parsers prepend the "sha256:"
                // prefix ContentHash::parse demands (project-kit.cpp).
                UF_TRY_VALUE(
                    digest,
                    ContentHash::parse(
                        "sha256:" + std::string{rowDigest->string()}
                    )
                );
                rows.emplace_back(uf::deployment::DeclaredProjectFile{
                    .path   = std::string{rowPath->string()},
                    .digest = digest,
                });
            }
            return rows;
        }

        [[nodiscard]]
        auto writeDeclaredFilesRecord(
            std::filesystem::path const& buildDirectory,
            std::vector<uf::deployment::DeclaredProjectFile> const& rows
        ) -> Status
        {
            auto const path = buildDirectory / k_declaredFilesRecordName;
            auto stream     = std::ofstream{
                path,
                std::ios::binary | std::ios::trunc
            };
            if (!stream.is_open())
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot open the declared-file record \"{}\" for "
                        "writing",
                        path.string()
                    )
                );
            }
            stream << renderDeclaredFilesRecord(rows);
            if (!stream)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot write the declared-file record \"{}\"",
                        path.string()
                    )
                );
            }
            return ok();
        }

        // The build's side of the check: after the kit accepted the build, the
        // files the deployment declarations name are read through the loader
        // and pinned by digest. A declared file the tree does not hold refuses
        // the build too, like a missing declared tool catalog source does -- a
        // build that recorded nothing for a declared file would make the
        // check's comparison vacuous for it.
        [[nodiscard]]
        auto recordDeclaredFiles(
            std::span<std::string const> raw,
            std::string_view action
        ) -> Status
        {
            UF_TRY_VALUE(parsed, uf::project::parseProjectDirectories(raw, action));
            UF_TRY_VALUE(
                rows,
                uf::deployment::readDeclaredProjectFiles(parsed.sourceDirectory)
            );
            return writeDeclaredFilesRecord(parsed.buildDirectory, rows);
        }

        // The check's side of the wiring, which runs after the kit's own check
        // accepted the closure: every file a deployment declaration names is
        // opened again through the loader and held to the record the build
        // wrote. The refusal names the file -- a missing file is the loader's
        // refusal, an altered file this comparison's -- because the kit's
        // refusals name the generated artifact or the manifest, not the
        // declared file.
        [[nodiscard]]
        auto verifyDeclaredFiles(
            std::span<std::string const> raw,
            std::string_view action
        ) -> Status
        {
            UF_TRY_VALUE(parsed, uf::project::parseProjectDirectories(raw, action));
            UF_TRY_VALUE(
                recorded,
                readDeclaredFilesRecord(parsed.buildDirectory)
            );
            UF_TRY_VALUE(
                current,
                uf::deployment::readDeclaredProjectFiles(parsed.sourceDirectory)
            );

            for (auto const& recordedRow : recorded)
            {
                auto const found = std::ranges::find_if(
                    current,
                    [&recordedRow](uf::deployment::DeclaredProjectFile const& row)
                    {
                        return row.path == recordedRow.path;
                    }
                );
                if (found == current.end())
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "the build recorded declared file \"{}\", which "
                            "umbraflow-project.json no longer names: rebuild "
                            "the project",
                            recordedRow.path
                        )
                    );
                }
                if (found->digest != recordedRow.digest)
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "declared file \"{}\" was recorded as sha256 {} and "
                            "this source tree now derives sha256 {}: a pinned "
                            "file has moved",
                            recordedRow.path,
                            recordedRow.digest.hex(),
                            found->digest.hex()
                        )
                    );
                }
            }
            for (auto const& currentRow : current)
            {
                auto const found = std::ranges::find_if(
                    recorded,
                    [&currentRow](uf::deployment::DeclaredProjectFile const& row)
                    {
                        return row.path == currentRow.path;
                    }
                );
                if (found == recorded.end())
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "the deployment declaration names \"{}\", which "
                            "the build at \"{}\" did not record: rebuild the "
                            "project",
                            currentRow.path,
                            parsed.buildDirectory.string()
                        )
                    );
                }
            }
            return ok();
        }

        // The kit's table owns the whole command vocabulary; this executable
        // owes two of its actions a second half the kit cannot link, because
        // uf::project cannot link uf::deployment. build records the declared
        // files after the kit's build; check verifies them after the kit's
        // check. Every other action is the kit's alone, and a command added to
        // the kit's table needs no branch here until the executable owes it a
        // second half too.
        [[nodiscard]]
        auto runWired(std::span<std::string const> raw) -> uf::project::ProjectExitCode
        {
            if (raw.empty())
            {
                return uf::project::runProjectCommand(raw);
            }
            if (raw.front() == "build")
            {
                auto const built = uf::project::runProjectCommand(raw);
                if (built != uf::project::ProjectExitCode::Success)
                {
                    return built;
                }
                auto const recorded = recordDeclaredFiles(raw.subspan(1), "build");
                if (!recorded)
                {
                    std::cerr << recorded.error().message() << '\n';
                    return uf::project::ProjectExitCode::Failure;
                }
                return uf::project::ProjectExitCode::Success;
            }
            if (raw.front() == "check")
            {
                auto const checked = uf::project::runProjectCommand(raw);
                if (checked != uf::project::ProjectExitCode::Success)
                {
                    return checked;
                }
                auto const verified = verifyDeclaredFiles(raw.subspan(1), "check");
                if (!verified)
                {
                    std::cerr << verified.error().message() << '\n';
                    return uf::project::ProjectExitCode::Failure;
                }
                return uf::project::ProjectExitCode::Success;
            }
            return uf::project::runProjectCommand(raw);
        }
    }
}

namespace
{
    // Reporting a fatal exception must not itself become the reason the process
    // dies: std::cerr's inserters are not noexcept, and a throw out of a catch
    // handler in main leaves nowhere to report it. A stream that fails while
    // printing why an earlier failure happened has nothing further to say, so
    // the exit code carries the outcome on its own.
    [[nodiscard]]
    auto reportFatalException(std::string_view what) noexcept -> int
    {
        try
        {
            std::cerr << "project exception: " << what << '\n';
        }
        catch (...)
        {
        }
        return std::to_underlying(uf::project::ProjectExitCode::Failure);
    }
}

auto main(int argumentCount, char const* const* p_arguments) -> int
{
    try
    {
        auto const convertedArgumentCount = uf::checkedCast<std::size_t>(
            argumentCount
        );
        if (!convertedArgumentCount || *convertedArgumentCount == 0U)
        {
            std::cerr << "project error: invalid process argument vector\n";
            return std::to_underlying(uf::project::ProjectExitCode::Failure);
        }
        // SAFETY: a hosted entry point receives argumentCount argument pointers
        // followed by a null one ([basic.start.main]/2). That count arrives
        // beside the pointer rather than within it, so this is the only place
        // the C contract becomes a span.
        UF_UNSAFE_BUFFER_BEGIN
        auto const arguments = std::span<char const* const>{
            p_arguments,
            *convertedArgumentCount
        };
        UF_UNSAFE_BUFFER_END

        auto raw = std::vector<std::string>{};
        for (auto const* argument : arguments.subspan(1U))
        {
            raw.emplace_back(argument);
        }
        return std::to_underlying(uf::project_entry::runWired(raw));
    }
    catch (std::exception const& error)
    {
        return reportFatalException(error.what());
    }
    catch (...)
    {
        return reportFatalException("unknown failure");
    }
}

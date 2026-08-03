#pragma once

#include <core/error/result.hpp>

#include <domain/error.hpp>

#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>

namespace uf::task
{
    // Reads a whole file, refusing anything past `maximumBytes` so a malformed or
    // hostile project cannot force an unbounded read. The cap decision rests on
    // the bytes the stream actually yields rather than on a pre-read stat, so a
    // file that grows after the stat cannot slip past it; the stat survives only
    // as a fast-path early rejection.
    //
    // `capSubject` names what the cap is on and reads back as "the
    // <maximumBytes>-byte <capSubject> cap", so a refusal says which of this
    // module's caps refused. Over the cap is InvalidResource -- the file exists
    // and is not the resource it claims to be; an unopenable or unreadable file
    // is IoFailure.
    [[nodiscard]]
    inline auto readCappedFile(
        std::filesystem::path const& path,
        std::size_t maximumBytes,
        std::string_view capSubject
    ) -> Result<std::string>
    {
        auto sizeError       = std::error_code{};
        auto const fileBytes = std::filesystem::file_size(path, sizeError);
        if (!sizeError && fileBytes > maximumBytes)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "'{}' is {} bytes, exceeding the {}-byte {} cap",
                    path.string(),
                    fileBytes,
                    maximumBytes,
                    capSubject
                )
            );
        }

        auto stream = std::ifstream{path, std::ios::binary};
        if (!stream.is_open())
        {
            return fail(
                AutomationErrorKind::IoFailure,
                std::format("cannot open '{}'", path.string())
            );
        }

        constexpr auto chunkBytes = std::size_t{64} * 1024U;
        auto contents             = std::string{};
        for (;;)
        {
            auto const oldSize = contents.size();
            contents.resize(oldSize + chunkBytes);
            stream.read(
                contents.data() + oldSize,
                static_cast<std::streamsize>(chunkBytes)
            );
            contents.resize(oldSize + static_cast<std::size_t>(stream.gcount()));

            if (stream.bad())
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format("cannot read '{}'", path.string())
                );
            }
            if (contents.size() > maximumBytes)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "'{}' exceeds the {}-byte {} cap",
                        path.string(),
                        maximumBytes,
                        capSubject
                    )
                );
            }
            if (stream.eof())
            {
                break;
            }
        }
        return contents;
    }
}

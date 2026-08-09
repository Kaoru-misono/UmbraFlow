#include "recorder.hpp"

#include "event.hpp"
#include "sink.hpp"
#include "stream-validator.hpp"

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/text/utf8.hpp>
#include <core/types/integer.hpp>
#include <core/utility/variant-match.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>

namespace uf::trace
{
    namespace
    {
        constexpr auto k_maxIdentityBytes = std::size_t{256};
        constexpr auto k_maxNameBytes     = std::size_t{128};
        constexpr auto k_maxTextBytes     = std::size_t{4096};
        constexpr auto k_maxFields        = std::size_t{128};
        constexpr auto k_maxReferences    = std::size_t{64};

        [[nodiscard]]
        auto invalidInput(std::string message) -> std::unexpected<Error>
        {
            return fail(
                std::make_error_code(std::errc::invalid_argument),
                std::move(message)
            );
        }

        [[nodiscard]]
        auto validateText(
            std::string_view value,
            std::string_view field,
            std::size_t maximumBytes
        ) -> Status
        {
            if (value.empty())
            {
                return invalidInput(std::format("{} must not be empty", field));
            }
            if (value.size() > maximumBytes)
            {
                return invalidInput(
                    std::format("{} exceeds its byte limit", field)
                );
            }
            if (!isValidUtf8(value))
            {
                return invalidInput(std::format("{} is not valid UTF-8", field));
            }
            if (std::ranges::any_of(
                value,
                [](char character)
                {
                    auto const byte = static_cast<unsigned char>(character);
                    return byte < 0x20U || byte == 0x7FU;
                }
            ))
            {
                return invalidInput(
                    std::format("{} contains a control character", field)
                );
            }
            return ok();
        }

        [[nodiscard]]
        auto isCanonicalName(
            std::string_view value,
            bool requireNamespace
        ) noexcept -> bool
        {
            auto atSegmentStart = true;
            auto hasSeparator   = false;
            for (auto const character : value)
            {
                if (character == '.')
                {
                    if (atSegmentStart)
                    {
                        return false;
                    }
                    atSegmentStart = true;
                    hasSeparator   = true;
                    continue;
                }

                auto const lower = character >= 'a' && character <= 'z';
                auto const digit = character >= '0' && character <= '9';
                if (atSegmentStart)
                {
                    if (!lower)
                    {
                        return false;
                    }
                    atSegmentStart = false;
                    continue;
                }
                if (!lower && !digit && character != '_' && character != '-')
                {
                    return false;
                }
            }

            return (
                !value.empty()
                && !atSegmentStart
                && (!requireNamespace || hasSeparator)
            );
        }

        [[nodiscard]]
        auto validateName(
            std::string_view value,
            std::string_view field,
            bool requireNamespace
        ) -> Status
        {
            if (
                value.size() > k_maxNameBytes
                || !isCanonicalName(value, requireNamespace)
            )
            {
                return invalidInput(
                    std::format("{} is not a canonical lowercase name", field)
                );
            }
            return ok();
        }

        [[nodiscard]] auto compactAscii(std::string_view value) -> std::string
        {
            auto compact = std::string{};
            compact.reserve(value.size());
            for (auto character : value)
            {
                if (character >= 'A' && character <= 'Z')
                {
                    character = static_cast<char>(character - 'A' + 'a');
                }
                if (
                    (character >= 'a' && character <= 'z')
                    || (character >= '0' && character <= '9')
                )
                {
                    compact += character;
                }
            }
            return compact;
        }

        [[nodiscard]] auto isForbiddenFieldName(std::string_view name) -> bool
        {
            auto constexpr forbidden = std::array{
                std::string_view{"screenshot"},
                std::string_view{"screenshotbytes"},
                std::string_view{"screenshotdata"},
                std::string_view{"framebytes"},
                std::string_view{"framedata"},
                std::string_view{"pixelbytes"},
                std::string_view{"pixeldata"},
                std::string_view{"imagebytes"},
                std::string_view{"imagedata"},
                std::string_view{"pngbytes"},
                std::string_view{"jpegbytes"},
                std::string_view{"annotationworkspacepath"},
                std::string_view{"annotationworkspacesqlite"},
            };

            auto const compact = compactAscii(name);
            return std::ranges::any_of(
                forbidden,
                [&compact](std::string_view term)
                {
                    return compact.contains(term);
                }
            );
        }

        [[nodiscard]] auto validateSafeText(std::string_view value) -> Status
        {
            auto const compact = compactAscii(value);
            auto lower         = std::string{value};
            std::ranges::transform(
                lower,
                lower.begin(),
                [](char character)
                {
                    if (character >= 'A' && character <= 'Z')
                    {
                        return static_cast<char>(character - 'A' + 'a');
                    }
                    return character;
                }
            );
            if (
                compact.contains("annotationworkspacesqlite")
                || lower.contains("data:image/")
            )
            {
                return invalidInput(
                    "production Trace cannot carry frame data or an annotation "
                    "workspace path"
                );
            }
            return ok();
        }

        [[nodiscard]] auto looksLikeEncodedBytes(std::string_view value) -> bool
        {
            auto encodedCharacters = std::size_t{};
            for (auto const character : value)
            {
                if (character == ' ')
                {
                    continue;
                }
                auto const encoded = (
                    (character >= 'a' && character <= 'z')
                    || (character >= 'A' && character <= 'Z')
                    || (character >= '0' && character <= '9')
                    || character == '+'
                    || character == '/'
                    || character == '_'
                    || character == '-'
                    || character == '='
                );
                if (!encoded)
                {
                    return false;
                }
                ++encodedCharacters;
            }
            return encodedCharacters >= 64U;
        }

        [[nodiscard]] auto validateScalar(TraceScalar const& value) -> Status
        {
            return matchVariant(
                value,
                [](std::monostate) -> Status { return ok(); },
                [](bool) -> Status { return ok(); },
                [](int64) -> Status { return ok(); },
                [](uint64) -> Status { return ok(); },
                [](std::string const& text) -> Status
                {
                    UF_TRY(validateText(text, "trace field text", k_maxTextBytes));
                    UF_TRY(validateSafeText(text));
                    if (looksLikeEncodedBytes(text))
                    {
                        return invalidInput(
                            "trace text resembles encoded binary data; use an "
                            "audit reference instead"
                        );
                    }
                    return ok();
                }
            );
        }

        [[nodiscard]]
        auto normalizeEvent(TraceEventSpec const& spec) -> Result<TraceEventSpec>
        {
            UF_TRY(validateName(spec.eventType, "event_type", true));
            UF_TRY(validateName(spec.audit.actor, "audit actor", false));

            if (spec.audit.references.size() > k_maxReferences)
            {
                return invalidInput("audit reference count exceeds its limit");
            }
            if (spec.payload.fields.size() > k_maxFields)
            {
                return invalidInput("trace payload field count exceeds its limit");
            }

            auto normalized = spec;
            for (auto const& reference : normalized.audit.references)
            {
                UF_TRY(validateName(reference.type, "audit reference type", false));
                if (isForbiddenFieldName(reference.type))
                {
                    return invalidInput(
                        "production Trace forbids screenshot and frame-data references"
                    );
                }
                UF_TRY(
                    validateText(
                        reference.id,
                        "audit reference id",
                        k_maxIdentityBytes
                    )
                );
                UF_TRY(validateSafeText(reference.id));
            }
            std::ranges::sort(
                normalized.audit.references,
                [](TraceReference const& left, TraceReference const& right)
                {
                    auto const leftKey  = std::tie(left.type, left.id);
                    auto const rightKey = std::tie(right.type, right.id);
                    return leftKey < rightKey;
                }
            );
            for (
                auto index = std::size_t{1};
                index < normalized.audit.references.size();
                ++index
            )
            {
                auto const& previous = normalized.audit.references[index - 1U];
                auto const& current  = normalized.audit.references[index];
                if (previous.type == current.type && previous.id == current.id)
                {
                    return invalidInput("audit references must be unique");
                }
            }

            for (auto const& field : normalized.payload.fields)
            {
                UF_TRY(validateName(field.name, "trace payload field name", false));
                if (isForbiddenFieldName(field.name))
                {
                    return invalidInput(
                        "production Trace forbids screenshot, frame-byte, and "
                        "annotation-workspace fields"
                    );
                }
                UF_TRY(validateScalar(field.value));
            }
            std::ranges::sort(
                normalized.payload.fields,
                {},
                &TraceField::name
            );
            for (
                auto index = std::size_t{1};
                index < normalized.payload.fields.size();
                ++index
            )
            {
                if (
                    normalized.payload.fields[index - 1U].name
                    == normalized.payload.fields[index].name
                )
                {
                    return invalidInput(
                        "trace payload field names must be unique"
                    );
                }
            }

            return normalized;
        }

        [[nodiscard]] auto recordedAtUnixMillis() noexcept -> int64
        {
            using MillisecondRep = std::chrono::milliseconds::rep;
            static_assert(std::is_integral_v<MillisecondRep>);

            auto const sinceEpoch = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            );
            auto const converted = checkedCast<int64>(sinceEpoch.count());
            UF_CHECK(converted.has_value());
            return *converted;
        }
    }

    TraceRecorder::TraceRecorder(
        std::unique_ptr<ITraceSink> sink,
        TraceStreamSpec stream
    ) noexcept
        : m_sink{std::move(sink)}
        , m_stream{std::move(stream)}
    {
        UF_CHECK(m_sink != nullptr);
    }

    auto TraceRecorder::create(
        std::unique_ptr<ITraceSink> sink,
        TraceStreamSpec const& spec
    ) -> Result<TraceRecorder>
    {
        if (sink == nullptr)
        {
            return invalidInput("a trace recorder requires a sink");
        }
        UF_TRY(validateText(spec.sessionId, "session_id", k_maxIdentityBytes));
        UF_TRY(validateSafeText(spec.sessionId));
        UF_TRY(validateName(spec.producer, "trace producer", false));
        return TraceRecorder{std::move(sink), spec};
    }

    auto TraceRecorder::emit(TraceEventSpec const& spec) -> Status
    {
        if (m_faulted)
        {
            return fail(
                std::make_error_code(std::errc::state_not_recoverable),
                "trace recorder is faulted after a sink failure"
            );
        }
        if (m_exhausted)
        {
            return fail(
                std::make_error_code(std::errc::value_too_large),
                "trace sequence is exhausted"
            );
        }

        UF_TRY_VALUE(normalized, normalizeEvent(spec));
        auto const event = TraceEvent{
            std::move(normalized),
            m_stream,
            m_nextSequence,
            recordedAtUnixMillis(),
        };
        UF_TRY(m_validator.admit(event));

        auto appended = m_sink->append(event);
        if (!appended)
        {
            m_faulted = true;
            return withContext(
                std::move(appended),
                "appending audit trace event"
            );
        }

        auto const next = checkedAdd(m_nextSequence, uint64{1});
        if (next.has_value())
        {
            m_nextSequence = *next;
        }
        else
        {
            m_exhausted = true;
        }
        return ok();
    }
}

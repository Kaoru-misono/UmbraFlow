#pragma once

#include <core/error/result.hpp>
#include <core/types/enum-reflection.hpp>
#include <core/types/integer.hpp>

#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <system_error>

namespace uf
{
    enum class AutomationErrorKind : uint8
    {
        Cancelled,
        Timeout,
        InvalidResource,
        UnsupportedCapability,
        TargetCompatibilityUnverified,
        TargetUnavailable,
        CaptureUnavailable,
        CaptureStalled,
        // A recognition that never finished looking: a search stopped on its
        // comparison budget rather than deciding. It is deliberately not named
        // for a failed recognition, because a recognition that completes and
        // matches nothing is not a failure at all -- it is UnknownPage, or a
        // nil hit, and it carries no error.
        RecognitionIncomplete,
        StaleObservation,
        // A page-scoped operation ran on an observation cycle that has resolved
        // no page. It is deliberately NOT ActionRejected, because nothing was
        // attempted and nothing was judged.
        PageUnresolved,
        ActionRejected,
        ControllerDisconnected,
        InternalInvariant,
        IoFailure,
        ExternalFailure,
    };

    // How far a failure has to unwind. This is the axis callers branch on;
    // AutomationErrorKind stays the record of what went wrong. failureResponse
    // switches over every kind without a default, so a new kind does not
    // compile until its unwind scope has been chosen.
    enum class FailureResponse : uint8
    {
        // The same operation may still succeed later; the caller may repeat it
        // within its own budget.
        Retry,
        // This step will not succeed as specified, but the run can continue
        // with other work.
        StepFailed,
        // The run cannot continue.
        Abort,
        // An external stop was requested. Never retried and never masked.
        Cancelled,
    };

    // Wraps an operating-system status value in std::system_category(). The
    // conversion to int is a deliberate reinterpretation, not a narrowing: the
    // category itself stores the value as int and hands it back to the platform
    // unchanged, so codes with the high bit set (an HRESULT-shaped GetLastError
    // result, for example) must survive rather than be rejected.
    [[nodiscard]]
    auto systemErrorCode(uint32 value) noexcept -> std::error_code;

    [[nodiscard]]
    auto failureResponse(AutomationErrorKind kind) noexcept -> FailureResponse;

    // An error carrying no automation kind cannot be classified, so it takes
    // the conservative response instead of being treated as recoverable.
    [[nodiscard]]
    auto failureResponse(Error const& error) noexcept -> FailureResponse;

    [[nodiscard]]
    auto automationErrorKind(Error const& error) noexcept -> std::optional<AutomationErrorKind>;

    // The one snake_case spelling of an error kind outside C++. Every non-C++
    // surface names a kind with this string and no other: the `errorKind` field
    // of a trace line, the `kind` field of a Tier B error a script catches, and
    // the `uf.errors.<kind>` constant that script compares it against. It lives
    // in domain because domain owns AutomationErrorKind and is the only module
    // both trace and task already depend on.
    //
    // Deliberately independent of enum reflection, so renaming the C++ enumerator
    // cannot silently change a wire format and a script's comparisons. The switch
    // is total with no default: a new kind does not compile until it has been
    // given a spelling. The returned view names a string literal, so it outlives
    // every caller and is not bound to the argument.
    [[nodiscard]]
    auto automationErrorWireName(AutomationErrorKind kind) noexcept -> std::string_view;

    // The snake_case spelling of an unwind scope outside C++. Independent of
    // enum reflection for the same reason automationErrorWireName is, and
    // total with no default. The returned view names a string literal, so it
    // outlives every caller.
    [[nodiscard]]
    auto failureResponseWireName(FailureResponse response) noexcept -> std::string_view;

    [[nodiscard]]
    auto fail(
        AutomationErrorKind kind,
        std::string message,
        std::error_code nativeCode = {},
        std::source_location location = std::source_location::current()
    ) -> std::unexpected<Error>;
}

UF_REFLECT_ENUM(
    uf::AutomationErrorKind,
    uf::AutomationErrorKind::Cancelled,
    uf::AutomationErrorKind::Timeout,
    uf::AutomationErrorKind::InvalidResource,
    uf::AutomationErrorKind::UnsupportedCapability,
    uf::AutomationErrorKind::TargetCompatibilityUnverified,
    uf::AutomationErrorKind::TargetUnavailable,
    uf::AutomationErrorKind::CaptureUnavailable,
    uf::AutomationErrorKind::CaptureStalled,
    uf::AutomationErrorKind::RecognitionIncomplete,
    uf::AutomationErrorKind::StaleObservation,
    uf::AutomationErrorKind::PageUnresolved,
    uf::AutomationErrorKind::ActionRejected,
    uf::AutomationErrorKind::ControllerDisconnected,
    uf::AutomationErrorKind::InternalInvariant,
    uf::AutomationErrorKind::IoFailure,
    uf::AutomationErrorKind::ExternalFailure
);

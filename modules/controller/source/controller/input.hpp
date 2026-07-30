#pragma once

#include "discovery.hpp"

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>
#include <domain/detection.hpp>
#include <domain/key.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uf
{
    namespace controller_detail
    {
        struct AuditLogAccess;
        struct HeldInputsAccess;
    }

    inline constexpr auto k_forbiddenBackgroundApis = std::array<std::string_view, 6>{
        "SetForegroundWindow",
        "SetFocus",
        "SendInput",
        "mouse_event",
        "keybd_event",
        "SetCursorPos",
    };

    class ClientPixel final
    {
        int16 m_x;
        int16 m_y;

        constexpr ClientPixel(int16 x, int16 y) noexcept
            : m_x{x}
            , m_y{y}
        {
        }

    public:
        auto operator<=>(ClientPixel const&) const = default;

        [[nodiscard]]
        static auto create(int32 x, int32 y) -> Result<ClientPixel>;

        [[nodiscard]] constexpr auto x() const noexcept -> int32 { return m_x; }
        [[nodiscard]] constexpr auto y() const noexcept -> int32 { return m_y; }
    };

    // WM_MOUSEWHEEL counts in WHEEL_DELTA units, of which 120 are one detent:
    // the step a physical wheel clicks through and the step every Windows target
    // divides by. Callers name detents, so the raw unit appears only here and at
    // the message boundary.
    inline constexpr auto k_wheelUnitsPerNotch = int32{120};

    // wParam carries the raw delta in a signed 16-bit high word, so a notch
    // count is deliverable only while its raw form still fits there.
    inline constexpr auto k_maxWheelNotches = int32{
        std::numeric_limits<int16>::max() / k_wheelUnitsPerNotch
    };

    class WheelDelta final
    {
        int16 m_notches;

        constexpr explicit WheelDelta(int16 notches) noexcept
            : m_notches{notches}
        {
        }

    public:
        auto operator<=>(WheelDelta const&) const = default;

        // Positive scrolls away from the operator and negative toward them,
        // which is the sign every Windows target already reads. Zero is refused:
        // a wheel message that moves nothing is a mistyped command rather than a
        // no-op worth posting.
        [[nodiscard]]
        static auto create(int32 notches) -> Result<WheelDelta>;

        [[nodiscard]]
        constexpr auto notches() const noexcept -> int32 { return m_notches; }

        // create bounds the notch count, so this product always fits the word
        // wParam encodes it in.
        [[nodiscard]] constexpr auto rawUnits() const noexcept -> int16
        {
            return static_cast<int16>(m_notches * k_wheelUnitsPerNotch);
        }
    };

    class KeyInput final
    {
        uint16 m_virtualKey;
        bool   m_extended;

        constexpr KeyInput(uint16 virtualKey, bool extended) noexcept
            : m_virtualKey{virtualKey}
            , m_extended{extended}
        {
        }

    public:
        explicit KeyInput(uint16 virtualKey) noexcept;

        auto operator<=>(KeyInput const&) const = default;

        [[nodiscard]] static constexpr auto numpadEnter() noexcept -> KeyInput
        {
            return KeyInput{0x000DU, true};
        }

        // Resolves a key hint as a target's own UI prints it ("E", "1", "F3")
        // to a virtual key. Which names exist is domain::KeyName's single
        // definition, which this routes through rather than repeating, so the
        // accepted set cannot drift between the name a project may write and the
        // name this can resolve.
        [[nodiscard]]
        static auto fromName(std::string_view name) -> Result<KeyInput>;

        // The virtual key one already-validated name resolves to. Total: KeyName's
        // construction invariant admits only names this maps, so there is no
        // failure left to report.
        [[nodiscard]] static auto fromKeyName(KeyName name) noexcept -> KeyInput;

        [[nodiscard]]
        constexpr auto virtualKey() const noexcept -> uint16 { return m_virtualKey; }
        [[nodiscard]] constexpr auto isExtended() const noexcept -> bool { return m_extended; }
    };

    enum class PointerButton : uint8
    {
        Left,
    };

    class HeldPointerInput final
    {
        PointerButton m_button;
        ClientPixel   m_pixel;

    public:
        constexpr HeldPointerInput(PointerButton button, ClientPixel pixel) noexcept
            : m_button{button}
            , m_pixel{pixel}
        {
        }

        auto operator==(HeldPointerInput const&) const -> bool = default;

        [[nodiscard]]
        constexpr auto button() const noexcept -> PointerButton { return m_button; }
        [[nodiscard]] constexpr auto pixel() const noexcept -> ClientPixel { return m_pixel; }
    };

    using HeldInput = std::variant<KeyInput, HeldPointerInput>;

    class DeliveryTarget final
    {
        WindowHandle     m_windowHandle;
        CaptureSessionId m_sessionId;
        TargetGeneration m_generation;
        ClientSize       m_clientSize;

        constexpr DeliveryTarget(
            WindowHandle windowHandle,
            CaptureSessionId sessionId,
            TargetGeneration generation,
            ClientSize clientSize
        ) noexcept
            : m_windowHandle{windowHandle}
            , m_sessionId{sessionId}
            , m_generation{generation}
            , m_clientSize{clientSize}
        {
        }

    public:
        [[nodiscard]]
        static auto create(
            WindowHandle windowHandle,
            CaptureSessionId sessionId,
            TargetGeneration generation,
            uint32 clientWidth,
            uint32 clientHeight
        ) -> Result<DeliveryTarget>;

        [[nodiscard]]
        constexpr auto windowHandle() const noexcept -> WindowHandle { return m_windowHandle; }
        [[nodiscard]] constexpr auto sessionId() const noexcept -> CaptureSessionId { return m_sessionId; }
        [[nodiscard]]
        constexpr auto generation() const noexcept -> TargetGeneration { return m_generation; }
        [[nodiscard]]
        constexpr auto clientWidth() const noexcept -> uint32
        {
            return m_clientSize.width();
        }
        [[nodiscard]]
        constexpr auto clientHeight() const noexcept -> uint32
        {
            return m_clientSize.height();
        }
    };

    struct ReleaseOutcome final
    {
        HeldInput input;
        Status    result{};
    };

    struct AuditRecord final
    {
        uintptr target{};
        uint32  message{};
        uintptr wParam{};
        intptr  lParam{};

        MonotonicInstant at;
    };

    class AuditLog final
    {
        std::vector<AuditRecord> m_records{};

        auto record(
            WindowHandle windowHandle,
            uint32 message,
            uintptr wParam,
            intptr lParam
        ) -> void;

        friend struct controller_detail::AuditLogAccess;

    public:
        // CONSTRAINT: Any subsequent audited delivery may grow m_records and
        // invalidate this span. Do not retain it across further input calls.
        [[nodiscard]]
        auto records() const noexcept UF_LIFETIME_BOUND -> std::span<AuditRecord const>;
        [[nodiscard]] auto size() const noexcept -> std::size_t;
        [[nodiscard]] auto empty() const noexcept -> bool;
    };

    class HeldInputs final
    {
        struct DeliveryIdentity final
        {
            uintptr          window{};
            CaptureSessionId sessionId;
            TargetGeneration generation{};

            auto operator==(DeliveryIdentity const&) const -> bool = default;
        };

        std::set<KeyInput> m_keys{};
        std::map<PointerButton, ClientPixel> m_pointers{};
        std::optional<DeliveryIdentity> m_binding{};

        [[nodiscard]]
        static auto identity(DeliveryTarget const& target) noexcept -> DeliveryIdentity;
        [[nodiscard]]
        static auto describeIdentity(DeliveryIdentity const& identity) -> std::string;
        [[nodiscard]]
        static auto describeBinding(
            std::optional<DeliveryIdentity> const& binding
        ) -> std::string;

        [[nodiscard]] auto ensureTarget(DeliveryTarget const& target) const -> Status;
        [[nodiscard]] auto bind(DeliveryTarget const& target) -> Status;
        [[nodiscard]]
        auto onKeyDown(DeliveryTarget const& target, KeyInput key) -> Result<bool>;
        [[nodiscard]]
        auto onKeyUp(DeliveryTarget const& target, KeyInput key) -> Result<bool>;
        [[nodiscard]]
        auto onPointerDown(
            DeliveryTarget const& target,
            PointerButton button,
            ClientPixel pixel
        ) -> Status;
        [[nodiscard]]
        auto onPointerUp(
            DeliveryTarget const& target,
            PointerButton button
        ) -> Result<bool>;
        [[nodiscard]]
        auto releaseAll(
            DeliveryTarget const& target,
            std::move_only_function<Status(HeldInput)> postUp
        ) -> std::vector<ReleaseOutcome>;
        auto clearBindingIfEmpty() noexcept -> void;

        friend struct controller_detail::HeldInputsAccess;

    public:
        [[nodiscard]] auto empty() const noexcept -> bool;
        [[nodiscard]] auto size() const noexcept -> std::size_t;
        [[nodiscard]] auto holdsKey(KeyInput key) const -> bool;
        [[nodiscard]] auto holdsPointer(PointerButton button) const -> bool;
    };

    [[nodiscard]]
    auto movePointer(
        DeliveryTarget const& target,
        ObservationLease lease,
        Point<ClientSpace> point,
        HeldInputs const& held,
        AuditLog& audit
    ) -> Status;

    [[nodiscard]]
    auto click(
        DeliveryTarget const& target,
        ObservationLease lease,
        Point<ClientSpace> point,
        HeldInputs& held,
        AuditLog& audit
    ) -> Status;

    [[nodiscard]]
    auto pointerDown(
        DeliveryTarget const& target,
        ObservationLease lease,
        Point<ClientSpace> point,
        HeldInputs& held,
        AuditLog& audit
    ) -> Status;

    [[nodiscard]]
    auto pointerUp(
        DeliveryTarget const& target,
        ObservationLease lease,
        Point<ClientSpace> point,
        HeldInputs& held,
        AuditLog& audit
    ) -> Status;

    [[nodiscard]]
    auto longPress(
        DeliveryTarget const& target,
        ObservationLease lease,
        Point<ClientSpace> point,
        MonotonicInstant::Duration hold,
        HeldInputs& held,
        AuditLog& audit,
        std::move_only_function<Result<DeliveryTarget>()> refreshTarget
    ) -> Status;

    // Posts one WM_MOUSEWHEEL at a point the target hit-tests to decide which
    // control scrolls. That position is why the same pointer preconditions a
    // click carries apply here: a wheel aimed from an expired or foreign
    // observation reaches a control the caller never saw.
    //
    // Vertical only. WM_MOUSEHWHEEL would need its own axis in every layer down
    // to the wire, and nothing in this project scrolls sideways.
    [[nodiscard]]
    auto scroll(
        DeliveryTarget const& target,
        ObservationLease lease,
        Point<ClientSpace> point,
        WheelDelta delta,
        HeldInputs const& held,
        AuditLog& audit
    ) -> Status;

    [[nodiscard]]
    auto keyPress(
        DeliveryTarget const& target,
        TargetGeneration actionGeneration,
        KeyInput key,
        HeldInputs& held,
        AuditLog& audit
    ) -> Status;

    [[nodiscard]]
    auto keyDown(
        DeliveryTarget const& target,
        TargetGeneration actionGeneration,
        KeyInput key,
        HeldInputs& held,
        AuditLog& audit
    ) -> Status;

    [[nodiscard]]
    auto keyUp(
        DeliveryTarget const& target,
        TargetGeneration actionGeneration,
        KeyInput key,
        HeldInputs& held,
        AuditLog& audit
    ) -> Status;

    [[nodiscard]]
    auto inputText(
        DeliveryTarget const& target,
        TargetGeneration actionGeneration,
        std::string_view text,
        HeldInputs const& held,
        AuditLog& audit
    ) -> Status;

    [[nodiscard]]
    auto inputUnichar(
        DeliveryTarget const& target,
        TargetGeneration actionGeneration,
        char32_t codePoint,
        HeldInputs const& held,
        AuditLog& audit
    ) -> Status;

    [[nodiscard]]
    auto releaseHeld(
        DeliveryTarget const& target,
        HeldInputs& held,
        AuditLog& audit
    ) -> std::vector<ReleaseOutcome>;
}

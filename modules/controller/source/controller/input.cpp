#include "input.hpp"

#include "detail/input-held.hpp"
#include "detail/input-message.hpp"
#include "detail/input-revalidation.hpp"

#include <core/error/contracts.hpp>
#include <core/types/integer.hpp>
#include <core/utility/variant-match.hpp>
#include <domain/error.hpp>

#include <format>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    [[nodiscard]]
    auto keyDebug(uf::KeyInput key) -> std::string
    {
        return std::format(
            "KeyInput {{ vk: {}, extended: {} }}",
            key.virtualKey(),
            key.isExtended()
        );
    }

    [[nodiscard]]
    auto decodeUtf8ToUtf16(std::string_view text) -> uf::Result<std::vector<uf::uint16>>
    {
        auto codeUnits = std::vector<uf::uint16>{};
        codeUnits.reserve(text.size());

        auto codePoint = uf::uint32{};
        auto minimumCodePoint = uf::uint32{};
        auto continuationBytes = uf::uint8{};
        for (auto const character : text)
        {
            auto const byte = static_cast<uf::uint32>(
                static_cast<unsigned char>(character)
            );
            if (continuationBytes == 0U)
            {
                if (byte <= 0x7FU)
                {
                    codeUnits.emplace_back(static_cast<uf::uint16>(byte));
                    continue;
                }
                if (byte >= 0xC2U && byte <= 0xDFU)
                {
                    codePoint = byte & 0x1FU;
                    minimumCodePoint = 0x80U;
                    continuationBytes = 1U;
                    continue;
                }
                if (byte >= 0xE0U && byte <= 0xEFU)
                {
                    codePoint = byte & 0x0FU;
                    minimumCodePoint = 0x800U;
                    continuationBytes = 2U;
                    continue;
                }
                if (byte >= 0xF0U && byte <= 0xF4U)
                {
                    codePoint = byte & 0x07U;
                    minimumCodePoint = 0x10000U;
                    continuationBytes = 3U;
                    continue;
                }

                return uf::fail(
                    uf::AutomationErrorKind::ActionRejected,
                    "input text must contain valid UTF-8"
                );
            }

            if ((byte & 0xC0U) != 0x80U)
            {
                return uf::fail(
                    uf::AutomationErrorKind::ActionRejected,
                    "input text must contain valid UTF-8"
                );
            }

            codePoint = (codePoint << 6U) | (byte & 0x3FU);
            --continuationBytes;
            if (continuationBytes != 0U)
            {
                continue;
            }
            if (
                codePoint < minimumCodePoint
                || codePoint > 0x10FFFFU
                || (codePoint >= 0xD800U && codePoint <= 0xDFFFU)
            )
            {
                return uf::fail(
                    uf::AutomationErrorKind::ActionRejected,
                    "input text must contain valid UTF-8"
                );
            }

            if (codePoint <= 0xFFFFU)
            {
                codeUnits.emplace_back(static_cast<uf::uint16>(codePoint));
                continue;
            }

            auto const offset = codePoint - 0x10000U;
            codeUnits.emplace_back(
                static_cast<uf::uint16>(0xD800U + (offset >> 10U))
            );
            codeUnits.emplace_back(
                static_cast<uf::uint16>(0xDC00U + (offset & 0x03FFU))
            );
        }

        if (continuationBytes != 0U)
        {
            return uf::fail(
                uf::AutomationErrorKind::ActionRejected,
                "input text must contain valid UTF-8"
            );
        }
        return codeUnits;
    }

    [[nodiscard]]
    auto pointerPixel(
        uf::DeliveryTarget const& target,
        uf::ObservationLease lease,
        uf::Point<uf::ClientSpace> point
    ) -> uf::Result<uf::ClientPixel>
    {
        return uf::controller_detail::checkPointerPreconditions(
            lease,
            target.sessionId(),
            target.generation(),
            uf::MonotonicInstant::now(),
            point,
            target.clientWidth(),
            target.clientHeight()
        );
    }

    [[nodiscard]]
    auto deliverPointerDown(
        uf::DeliveryTarget const& target,
        uf::ClientPixel pixel,
        uf::HeldInputs& held,
        uf::AuditLog& audit
    ) -> uf::Status
    {
        UF_TRY(uf::controller_detail::HeldInputsAccess::ensureTarget(held, target));
        if (held.holdsPointer(uf::PointerButton::Left))
        {
            return uf::fail(
                uf::AutomationErrorKind::ActionRejected,
                "left pointer button is already held"
            );
        }
        UF_TRY(uf::controller_detail::ensureWindowAlive(target.windowHandle()));
        UF_TRY(
            uf::controller_detail::deliver(
                target.windowHandle(),
                uf::controller_detail::pointerSpec(
                    uf::controller_detail::PointerMessage::Move,
                    pixel
                ),
                audit
            )
        );
        UF_TRY(
            uf::controller_detail::deliver(
                target.windowHandle(),
                uf::controller_detail::pointerSpec(
                    uf::controller_detail::PointerMessage::LeftDown,
                    pixel
                ),
                audit
            )
        );
        return uf::controller_detail::HeldInputsAccess::onPointerDown(
            held,
            target,
            uf::PointerButton::Left,
            pixel
        );
    }

    [[nodiscard]]
    auto deliverPointerUp(
        uf::DeliveryTarget const& target,
        uf::ClientPixel pixel,
        uf::HeldInputs& held,
        uf::AuditLog& audit
    ) -> uf::Status
    {
        UF_TRY(uf::controller_detail::HeldInputsAccess::ensureTarget(held, target));
        if (!held.holdsPointer(uf::PointerButton::Left))
        {
            return uf::fail(
                uf::AutomationErrorKind::ActionRejected,
                "left pointer button is not held"
            );
        }
        UF_TRY(uf::controller_detail::ensureWindowAlive(target.windowHandle()));
        UF_TRY(
            uf::controller_detail::deliver(
                target.windowHandle(),
                uf::controller_detail::pointerSpec(
                    uf::controller_detail::PointerMessage::LeftUp,
                    pixel
                ),
                audit
            )
        );
        UF_TRY_VALUE(
            released,
            uf::controller_detail::HeldInputsAccess::onPointerUp(
                held,
                target,
                uf::PointerButton::Left
            )
        );
        UF_CHECK(released);
        return uf::ok();
    }
}

namespace uf
{
    auto DeliveryTarget::create(
        WindowHandle windowHandle,
        SessionId sessionId,
        TargetGeneration generation,
        uint32 clientWidth,
        uint32 clientHeight
    ) -> Result<DeliveryTarget>
    {
        if (clientWidth == 0U || clientHeight == 0U)
        {
            return fail(
                AutomationErrorKind::TargetUnavailable,
                std::format(
                    "delivery target client area is empty (minimized window?): {}x{}",
                    clientWidth,
                    clientHeight
                )
            );
        }
        return DeliveryTarget{
            windowHandle,
            sessionId,
            generation,
            ClientSize{clientWidth, clientHeight}
        };
    }

    auto movePointer(
        DeliveryTarget const& target,
        ObservationLease lease,
        Point<ClientSpace> point,
        HeldInputs const& held,
        AuditLog& audit
    ) -> Status
    {
        UF_TRY_VALUE(pixel, pointerPixel(target, lease, point));
        UF_TRY(controller_detail::HeldInputsAccess::ensureTarget(held, target));
        UF_TRY(controller_detail::ensureWindowAlive(target.windowHandle()));
        auto const pointerMessage = held.holdsPointer(PointerButton::Left)
            ? controller_detail::PointerMessage::MoveWithLeftButton
            : controller_detail::PointerMessage::Move;
        return controller_detail::deliver(
            target.windowHandle(),
            controller_detail::pointerSpec(pointerMessage, pixel),
            audit
        );
    }

    auto click(
        DeliveryTarget const& target,
        ObservationLease lease,
        Point<ClientSpace> point,
        HeldInputs& held,
        AuditLog& audit
    ) -> Status
    {
        UF_TRY_VALUE(pixel, pointerPixel(target, lease, point));
        UF_TRY(deliverPointerDown(target, pixel, held, audit));
        return deliverPointerUp(target, pixel, held, audit);
    }

    auto pointerDown(
        DeliveryTarget const& target,
        ObservationLease lease,
        Point<ClientSpace> point,
        HeldInputs& held,
        AuditLog& audit
    ) -> Status
    {
        UF_TRY_VALUE(pixel, pointerPixel(target, lease, point));
        return deliverPointerDown(target, pixel, held, audit);
    }

    auto pointerUp(
        DeliveryTarget const& target,
        ObservationLease lease,
        Point<ClientSpace> point,
        HeldInputs& held,
        AuditLog& audit
    ) -> Status
    {
        UF_TRY_VALUE(pixel, pointerPixel(target, lease, point));
        return deliverPointerUp(target, pixel, held, audit);
    }

    auto longPress(
        DeliveryTarget const& target,
        ObservationLease lease,
        Point<ClientSpace> point,
        MonotonicInstant::Duration hold,
        HeldInputs& held,
        AuditLog& audit,
        std::move_only_function<Result<DeliveryTarget>()> refreshTarget
    ) -> Status
    {
        if (!refreshTarget)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "long press requires a refresh-target callback"
            );
        }
        if (hold < MonotonicInstant::Duration::zero())
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "long press duration must be non-negative"
            );
        }

        UF_TRY_VALUE(pixel, pointerPixel(target, lease, point));
        UF_TRY(deliverPointerDown(target, pixel, held, audit));
        std::this_thread::sleep_for(hold);
        UF_TRY_VALUE(refreshed, refreshTarget());
        UF_TRY(controller_detail::ensureSameDeliveryIdentity(target, refreshed));
        return deliverPointerUp(refreshed, pixel, held, audit);
    }

    auto keyPress(
        DeliveryTarget const& target,
        TargetGeneration actionGeneration,
        KeyInput key,
        HeldInputs& held,
        AuditLog& audit
    ) -> Status
    {
        UF_TRY(
            controller_detail::checkKeyboardPreconditions(
                actionGeneration,
                target.generation()
            )
        );
        UF_TRY(controller_detail::HeldInputsAccess::ensureTarget(held, target));
        if (held.holdsKey(key))
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "key " + keyDebug(key) + " is already held"
            );
        }
        UF_TRY(controller_detail::ensureWindowAlive(target.windowHandle()));

        auto const scanCode = controller_detail::scanCodeFor(key.virtualKey());
        UF_TRY(
            controller_detail::deliver(
                target.windowHandle(),
                controller_detail::keySpec(
                    key,
                    scanCode,
                    controller_detail::KeyTransition::Down
                ),
                audit
            )
        );
        UF_TRY(controller_detail::HeldInputsAccess::onKeyDown(held, target, key));
        UF_TRY(
            controller_detail::deliver(
                target.windowHandle(),
                controller_detail::keySpec(
                    key,
                    scanCode,
                    controller_detail::KeyTransition::Up
                ),
                audit
            )
        );
        UF_TRY_VALUE(
            released,
            controller_detail::HeldInputsAccess::onKeyUp(held, target, key)
        );
        UF_CHECK(released);
        return ok();
    }

    auto keyDown(
        DeliveryTarget const& target,
        TargetGeneration actionGeneration,
        KeyInput key,
        HeldInputs& held,
        AuditLog& audit
    ) -> Status
    {
        UF_TRY(
            controller_detail::checkKeyboardPreconditions(
                actionGeneration,
                target.generation()
            )
        );
        UF_TRY(controller_detail::HeldInputsAccess::ensureTarget(held, target));
        if (held.holdsKey(key))
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "key " + keyDebug(key) + " is already held"
            );
        }
        UF_TRY(controller_detail::ensureWindowAlive(target.windowHandle()));

        auto const scanCode = controller_detail::scanCodeFor(key.virtualKey());
        UF_TRY(
            controller_detail::deliver(
                target.windowHandle(),
                controller_detail::keySpec(
                    key,
                    scanCode,
                    controller_detail::KeyTransition::Down
                ),
                audit
            )
        );
        UF_TRY(controller_detail::HeldInputsAccess::onKeyDown(held, target, key));
        return ok();
    }

    auto keyUp(
        DeliveryTarget const& target,
        TargetGeneration actionGeneration,
        KeyInput key,
        HeldInputs& held,
        AuditLog& audit
    ) -> Status
    {
        UF_TRY(
            controller_detail::checkKeyboardPreconditions(
                actionGeneration,
                target.generation()
            )
        );
        UF_TRY(controller_detail::HeldInputsAccess::ensureTarget(held, target));
        if (!held.holdsKey(key))
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "key " + keyDebug(key) + " is not held"
            );
        }
        UF_TRY(controller_detail::ensureWindowAlive(target.windowHandle()));

        auto const scanCode = controller_detail::scanCodeFor(key.virtualKey());
        UF_TRY(
            controller_detail::deliver(
                target.windowHandle(),
                controller_detail::keySpec(
                    key,
                    scanCode,
                    controller_detail::KeyTransition::Up
                ),
                audit
            )
        );
        UF_TRY_VALUE(
            released,
            controller_detail::HeldInputsAccess::onKeyUp(held, target, key)
        );
        UF_CHECK(released);
        return ok();
    }

    auto inputText(
        DeliveryTarget const& target,
        TargetGeneration actionGeneration,
        std::string_view text,
        HeldInputs const& held,
        AuditLog& audit
    ) -> Status
    {
        UF_TRY(
            controller_detail::checkKeyboardPreconditions(
                actionGeneration,
                target.generation()
            )
        );
        UF_TRY(controller_detail::HeldInputsAccess::ensureTarget(held, target));
        UF_TRY(controller_detail::ensureWindowAlive(target.windowHandle()));
        UF_TRY_VALUE(codeUnits, controller_detail::utf16CodeUnits(text));

        for (auto const codeUnit : codeUnits)
        {
            UF_TRY(
                controller_detail::deliver(
                    target.windowHandle(),
                    controller_detail::charSpec(codeUnit),
                    audit
                )
            );
        }
        return ok();
    }

    auto inputUnichar(
        DeliveryTarget const& target,
        TargetGeneration actionGeneration,
        char32_t codePoint,
        HeldInputs const& held,
        AuditLog& audit
    ) -> Status
    {
        UF_TRY(
            controller_detail::checkKeyboardPreconditions(
                actionGeneration,
                target.generation()
            )
        );
        UF_TRY(controller_detail::HeldInputsAccess::ensureTarget(held, target));
        UF_TRY(controller_detail::ensureWindowAlive(target.windowHandle()));
        if (
            codePoint > char32_t{0x10FFFFU}
            || (codePoint >= char32_t{0xD800U} && codePoint <= char32_t{0xDFFFU})
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                std::format(
                    "WM_UNICHAR requires a Unicode scalar value, got {:#x}",
                    static_cast<uint32>(codePoint)
                )
            );
        }
        return controller_detail::deliver(
            target.windowHandle(),
            controller_detail::unicharSpec(codePoint),
            audit
        );
    }

    auto releaseHeld(
        DeliveryTarget const& target,
        HeldInputs& held,
        AuditLog& audit
    ) -> std::vector<ReleaseOutcome>
    {
        return controller_detail::HeldInputsAccess::releaseAll(
            held,
            target,
            [&target, &audit](HeldInput input) -> Status
            {
                auto const spec = matchVariant(
                    input,
                    [](KeyInput key) -> controller_detail::PostSpec
                    {
                        auto const scanCode = controller_detail::scanCodeFor(
                            key.virtualKey()
                        );
                        return controller_detail::keySpec(
                            key,
                            scanCode,
                            controller_detail::KeyTransition::Up
                        );
                    },
                    [](HeldPointerInput pointer) -> controller_detail::PostSpec
                    {
                        switch (pointer.button())
                        {
                        case PointerButton::Left:
                            return controller_detail::pointerSpec(
                                controller_detail::PointerMessage::LeftUp,
                                pointer.pixel()
                            );
                        }
                        UF_UNREACHABLE_MSG("Unknown PointerButton value");
                    }
                );
                return controller_detail::deliver(target.windowHandle(), spec, audit);
            }
        );
    }
}

namespace uf::controller_detail
{
    auto utf16CodeUnits(std::string_view text) -> Result<std::vector<uint16>>
    {
        return decodeUtf8ToUtf16(text);
    }
}

#include "input.hpp"

#include "detail/input-held.hpp"
#include "detail/input-message.hpp"
#include "detail/input-revalidation.hpp"

#include <core/error/contracts.hpp>
#include <core/text/utf8.hpp>
#include <core/types/integer.hpp>
#include <core/utility/variant-match.hpp>
#include <domain/error.hpp>

#include <format>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace uf
{
    namespace
    {
        [[nodiscard]]
        auto keyDebug(KeyInput key) -> std::string
        {
            return std::format(
                "KeyInput {{ vk: {}, extended: {} }}",
                key.virtualKey(),
                key.isExtended()
            );
        }

        [[nodiscard]]
        auto decodeUtf8ToUtf16(std::string_view text) -> Result<std::vector<uint16>>
        {
            auto const scalars = decodeUtf8Scalars(text);
            if (!scalars)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "input text must contain valid UTF-8"
                );
            }

            auto codeUnits = std::vector<uint16>{};
            codeUnits.reserve(text.size());
            for (auto const codePoint : *scalars)
            {
                if (codePoint <= 0xFFFFU)
                {
                    codeUnits.emplace_back(static_cast<uint16>(codePoint));
                    continue;
                }

                auto const offset = codePoint - 0x10000U;
                codeUnits.emplace_back(
                    static_cast<uint16>(0xD800U + (offset >> 10U))
                );
                codeUnits.emplace_back(
                    static_cast<uint16>(0xDC00U + (offset & 0x03FFU))
                );
            }
            return codeUnits;
        }

        [[nodiscard]]
        auto pointerPixel(
            DeliveryTarget const& target,
            ObservationLease lease,
            Point<ClientSpace> point
        ) -> Result<ClientPixel>
        {
            return controller_detail::checkPointerPreconditions(
                lease,
                target.sessionId(),
                target.generation(),
                MonotonicInstant::now(),
                point,
                target.clientWidth(),
                target.clientHeight()
            );
        }

        [[nodiscard]]
        auto deliverPointerDown(
            DeliveryTarget const& target,
            ClientPixel pixel,
            HeldInputs& held,
            AuditLog& audit
        ) -> Status
        {
            UF_TRY(controller_detail::HeldInputsAccess::ensureTarget(held, target));
            if (held.holdsPointer(PointerButton::Left))
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "left pointer button is already held"
                );
            }
            UF_TRY(controller_detail::ensureWindowAlive(target.windowHandle()));
            UF_TRY(
                controller_detail::deliver(
                    target.windowHandle(),
                    controller_detail::pointerSpec(
                        controller_detail::PointerMessage::Move,
                        pixel
                    ),
                    audit
                )
            );
            UF_TRY(
                controller_detail::deliver(
                    target.windowHandle(),
                    controller_detail::pointerSpec(
                        controller_detail::PointerMessage::LeftDown,
                        pixel
                    ),
                    audit
                )
            );
            return controller_detail::HeldInputsAccess::onPointerDown(
                held,
                target,
                PointerButton::Left,
                pixel
            );
        }

        [[nodiscard]]
        auto deliverPointerUp(
            DeliveryTarget const& target,
            ClientPixel pixel,
            HeldInputs& held,
            AuditLog& audit
        ) -> Status
        {
            UF_TRY(controller_detail::HeldInputsAccess::ensureTarget(held, target));
            if (!held.holdsPointer(PointerButton::Left))
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "left pointer button is not held"
                );
            }
            UF_TRY(controller_detail::ensureWindowAlive(target.windowHandle()));
            UF_TRY(
                controller_detail::deliver(
                    target.windowHandle(),
                    controller_detail::pointerSpec(
                        controller_detail::PointerMessage::LeftUp,
                        pixel
                    ),
                    audit
                )
            );
            UF_TRY_VALUE(
                released,
                controller_detail::HeldInputsAccess::onPointerUp(
                    held,
                    target,
                    PointerButton::Left
                )
            );
            UF_CHECK(released);
            return ok();
        }

        [[nodiscard]]
        auto deliverKeyTransition(
            DeliveryTarget const& target,
            TargetGeneration actionGeneration,
            KeyInput key,
            controller_detail::KeyTransition transition,
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
            auto const isDown = transition == controller_detail::KeyTransition::Down;
            if (isDown && held.holdsKey(key))
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "key " + keyDebug(key) + " is already held"
                );
            }
            if (!isDown && !held.holdsKey(key))
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
                    controller_detail::keySpec(key, scanCode, transition),
                    audit
                )
            );
            if (isDown)
            {
                UF_TRY(
                    controller_detail::HeldInputsAccess::onKeyDown(held, target, key)
                );
                return ok();
            }
            UF_TRY_VALUE(
                released,
                controller_detail::HeldInputsAccess::onKeyUp(held, target, key)
            );
            UF_CHECK(released);
            return ok();
        }
    }
}

namespace uf
{
    auto DeliveryTarget::create(
        WindowHandle windowHandle,
        CaptureSessionId sessionId,
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
            deliverKeyTransition(
                target,
                actionGeneration,
                key,
                controller_detail::KeyTransition::Down,
                held,
                audit
            )
        );
        return deliverKeyTransition(
            target,
            actionGeneration,
            key,
            controller_detail::KeyTransition::Up,
            held,
            audit
        );
    }

    auto keyDown(
        DeliveryTarget const& target,
        TargetGeneration actionGeneration,
        KeyInput key,
        HeldInputs& held,
        AuditLog& audit
    ) -> Status
    {
        return deliverKeyTransition(
            target,
            actionGeneration,
            key,
            controller_detail::KeyTransition::Down,
            held,
            audit
        );
    }

    auto keyUp(
        DeliveryTarget const& target,
        TargetGeneration actionGeneration,
        KeyInput key,
        HeldInputs& held,
        AuditLog& audit
    ) -> Status
    {
        return deliverKeyTransition(
            target,
            actionGeneration,
            key,
            controller_detail::KeyTransition::Up,
            held,
            audit
        );
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

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

    auto drag(
        DeliveryTarget const& target,
        ObservationLease lease,
        Point<ClientSpace> start,
        Point<ClientSpace> end,
        MonotonicInstant::Duration travel,
        HeldInputs& held,
        AuditLog& audit,
        std::move_only_function<Result<DeliveryTarget>()> refreshTarget
    ) -> Status
    {
        // How many held moves the travel is cut into. It is a mechanism number,
        // not a tuning knob: one move would be a jump the target reads as a
        // teleport, and a very large count spends the lease posting messages. The
        // travel time is the caller's because how slowly a target must be dragged
        // is a fact about that target.
        constexpr auto k_dragMoves = uint32{32};

        // The gesture DECELERATES INTO THE RELEASE, and that is the whole reason
        // the moves are not evenly spaced. A target computes fling velocity from
        // the last few pointer deltas before the button comes up, so a drag laid
        // out evenly releases at full speed and the target keeps going -- measured
        // 2026-08-05 on the real machine, where an evenly spaced drag sent the
        // roster gliding well past where it was asked to stop, and the glide was
        // still running when the next chunk tried to measure.
        //
        // Positions follow 1 - (1 - t)^2 while the time steps stay uniform, so the
        // pointer covers most of the distance early and creeps the last few
        // pixels. What arrives at the target is a gesture that ends at rest.
        constexpr auto k_dragEase = [](float fraction) noexcept -> float
        {
            auto const remaining = 1.0F - fraction;
            return 1.0F - (remaining * remaining);
        };

        // How long the pointer sits still at the far end before the button comes
        // up. Deceleration alone leaves a small last delta, and a target sampling
        // velocity over a window rather than over the final delta still sees
        // motion; standing still for longer than that window drives what it
        // samples to zero. It is charged to the caller's travel rather than added
        // to it, so `travel` stays the whole duration of the gesture.
        constexpr auto k_dragSettleFraction = uint32{4};

        if (!refreshTarget)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "drag requires a refresh-target callback"
            );
        }
        if (travel < MonotonicInstant::Duration::zero())
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "drag travel time must be non-negative"
            );
        }

        // BOTH ends before anything is pressed. The far end is the one a caller
        // gets wrong -- it is start plus an offset the caller computed -- and
        // checking it here is what keeps a bad offset from becoming a button
        // pressed at a place the drag can never finish.
        UF_TRY_VALUE(startPixel, pointerPixel(target, lease, start));
        UF_TRY_VALUE(endPixel, pointerPixel(target, lease, end));

        UF_TRY(deliverPointerDown(target, startPixel, held, audit));

        // From here on the button is DOWN, so every early return below leaves it
        // down on purpose: this function cannot know whether releasing at a place
        // it could not reach is better than reporting. The caller owes the drain.
        auto const dwell  = travel / k_dragSettleFraction;
        auto const moving = travel - dwell;
        auto const pause  = moving / k_dragMoves;
        for (auto move = uint32{1}; move <= k_dragMoves; ++move)
        {
            auto const fraction = k_dragEase(
                static_cast<float>(move) / static_cast<float>(k_dragMoves)
            );
            auto const waypoint = Point<ClientSpace>{
                start.x() + ((end.x() - start.x()) * fraction),
                start.y() + ((end.y() - start.y()) * fraction),
            };

            // Re-checked per move rather than interpolated between two validated
            // pixels, so a window that dies or is re-targeted under a held button
            // stops the gesture instead of finishing it into whatever took its
            // place.
            //
            // FRESHNESS IS DELIBERATELY NOT RE-FENCED HERE. The lease starts
            // running at frame capture and its ceiling is 750 ms
            // (`k_defaultMaxActionFrameAge`; `--max-frame-age` only shortens it),
            // while a drag slow enough not to fling the target runs 600 ms and up
            // out of whatever is left of that budget once the script has done its
            // reading. Fencing per waypoint made every such drag fail partway
            // through with `stale_observation` and the button still down --
            // measured 2026-08-05 against the game's map screen. The observation
            // is stale by then BECAUSE THE DRAG IS WORKING; both endpoints were
            // authorised against a fresh one before the button went down, which is
            // the moment freshness actually means something.
            UF_TRY_VALUE(
                pixel,
                controller_detail::checkPointerTarget(
                    lease,
                    target.sessionId(),
                    target.generation(),
                    waypoint,
                    target.clientWidth(),
                    target.clientHeight()
                )
            );
            UF_TRY(
                controller_detail::deliver(
                    target.windowHandle(),
                    controller_detail::pointerSpec(
                        controller_detail::PointerMessage::MoveWithLeftButton,
                        pixel
                    ),
                    audit
                )
            );
            if (pause > MonotonicInstant::Duration::zero())
            {
                std::this_thread::sleep_for(pause);
            }
        }

        // Stand still at the far end before letting go; see k_dragSettleFraction.
        // It runs before the refresh rather than after, so the dwell is time the
        // target spends seeing a motionless pointer rather than time this host
        // spends enumerating windows.
        if (dwell > MonotonicInstant::Duration::zero())
        {
            std::this_thread::sleep_for(dwell);
        }

        UF_TRY_VALUE(refreshed, refreshTarget());
        UF_TRY(controller_detail::ensureSameDeliveryIdentity(target, refreshed));
        return deliverPointerUp(refreshed, endPixel, held, audit);
    }

    auto scroll(
        DeliveryTarget const& target,
        ObservationLease lease,
        Point<ClientSpace> point,
        WheelDelta delta,
        HeldInputs const& held,
        AuditLog& audit
    ) -> Status
    {
        // The same pointer preconditions a click gets, deliberately: the wheel
        // message carries a position the target hit-tests to pick which control
        // scrolls, so an expired lease or a point outside the client area aims it
        // exactly as wrongly as it would aim a click.
        UF_TRY_VALUE(pixel, pointerPixel(target, lease, point));
        UF_TRY(controller_detail::HeldInputsAccess::ensureTarget(held, target));
        UF_TRY(controller_detail::ensureWindowAlive(target.windowHandle()));

        // WM_MOUSEWHEEL is the one message here whose lParam is in screen
        // coordinates, so the client pixel every other pointer message posts
        // directly has to be translated by the window's client origin first.
        UF_TRY_VALUE(
            origin,
            controller_detail::clientOriginOnScreen(target.windowHandle())
        );
        UF_TRY_VALUE(screenPixel, controller_detail::screenPixelFor(origin, pixel));
        return controller_detail::deliver(
            target.windowHandle(),
            controller_detail::wheelSpec(
                screenPixel,
                delta,
                held.holdsPointer(PointerButton::Left)
            ),
            audit
        );
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

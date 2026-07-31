#pragma once

#include <controller/capture.hpp>
#include <controller/discovery.hpp>
#include <controller/input.hpp>
#include <controller/target.hpp>
#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <domain/detection.hpp>
#include <domain/frame.hpp>
#include <domain/space.hpp>

#include <optional>
#include <string_view>

namespace uf::m0_demo
{
    auto clearInputAgentCommandAudit(AuditLog& audit) noexcept -> void;

    // The coordinate fence every pointer action passes before the target is
    // revalidated, so a point the caller got wrong is answered as a rejected
    // action rather than as a target that vanished.
    [[nodiscard]]
    auto validateInputAgentPointerAction(
        DeliveryTarget const& target,
        ObservationLease lease,
        Point<ClientSpace> point,
        MonotonicInstant now
    ) -> Status;

    // What one delivery attempt did.
    //
    // There is deliberately no `delivered` member. Whether the input reached the
    // target is the same fact as whether there is an error, and the results line
    // reads its own `delivered` off this: a rejected op reporting delivered:true
    // becomes unrepresentable rather than merely documented, which is what the
    // two used to be when a hand-written assignment set the flag after the post.
    struct DriveOutcome final
    {
        // Absent exactly when the input reached the target.
        std::optional<Error> error{};

        // Whether the failure was the window itself rather than this input. It
        // is not a detail of the error: every other failure belongs to the one
        // command, while a window that is no longer the one the run was launched
        // against invalidates everything queued behind it too, so this is the
        // only failure that ends a run rather than a command.
        bool targetReplaced{};

        [[nodiscard]] auto delivered() const noexcept -> bool;
    };

    // Delivering an input to a window and getting a frame back, and nothing
    // else. It names no file, encodes no image, parses no command and writes no
    // results line: a drive hands over one input against one observation and
    // says what happened to it. What an authoring session then makes of the
    // frames is the annotation layer above, which is why a verb that layer grows
    // later -- reading a region, proposing an element -- adds nothing here.
    //
    // Deliberately not engine::IActionSink, and for the same reason the port
    // above it is not: that one speaks the engine's vocabulary of a single
    // already-authorized action against an annotated element, and lives in a
    // module this executable does not link. Nothing is annotated during an
    // annotation session -- measuring the screen is what produces the
    // annotations -- so there is no element for such a port to name.
    //
    // It sits behind a port because a test process has none of what the live
    // half needs -- no window to resolve, no per-monitor DPI context, no Windows
    // Graphics Capture session -- while every decision the layer above makes
    // around it can be exercised without them.
    class IInputAgentDrive
    {
    public:
        IInputAgentDrive() = default;

        IInputAgentDrive(IInputAgentDrive const&) = delete;
        IInputAgentDrive(IInputAgentDrive&&) = delete;
        auto operator=(IInputAgentDrive const&) -> IInputAgentDrive& = delete;
        auto operator=(IInputAgentDrive&&) -> IInputAgentDrive& = delete;

        virtual ~IInputAgentDrive() = default;

        // The client area of the window this run was launched against. It is
        // fixed for the run: a resize replaces the target, which every delivery
        // already refuses.
        [[nodiscard]] virtual auto clientSize() const noexcept -> ClientSize = 0;

        // One observation of the target.
        [[nodiscard]] virtual auto capture() -> Result<Frame> = 0;

        // The three inputs. Each is decided from `observation` rather than from
        // whatever the window shows by the time it is posted, which is what the
        // freshness and target-continuity fences behind them enforce.
        [[nodiscard]]
        virtual auto click(
            Frame const& observation,
            Point<ClientSpace> point
        ) -> DriveOutcome = 0;

        [[nodiscard]]
        virtual auto scroll(
            Frame const& observation,
            Point<ClientSpace> point,
            WheelDelta delta
        ) -> DriveOutcome = 0;

        [[nodiscard]]
        virtual auto key(
            Frame const& observation,
            KeyInput key
        ) -> DriveOutcome = 0;

        // Drops the audit records the deliveries so far produced. The layer
        // above calls this once per answered command, which is what keeps a
        // session of ten thousand commands from accumulating all of their
        // records.
        virtual auto clearAudit() noexcept -> void = 0;

        // Ends the capture session, so a finished run never leaves one attached
        // to a live window.
        [[nodiscard]] virtual auto close() -> Status = 0;
    };

    // The drive layer over one resolved window. It owns the target and its
    // capture session rather than borrowing them, so nothing here can outlive
    // the window it acts on, and the per-target input bookkeeping every delivery
    // threads through -- the held inputs and the audit log -- lives beside them.
    class WindowInputAgentDrive final : public IInputAgentDrive
    {
        ResolvedTarget    m_resolved;
        WgcCaptureSession m_session;
        DeliveryTarget    m_delivery;
        ClientSize        m_client;

        HeldInputs m_held{};
        AuditLog   m_audit{};

        // Proves the target is still the one an observation came from before
        // anything is posted. Every failure is terminal for the run: the window
        // it was launched against is gone or was replaced.
        [[nodiscard]] auto requireSameTarget() -> Status;

        // Turns a failed post into an outcome, releasing whatever the failure
        // may have left held. A failed post can leave a key or the pointer
        // button down, and the release has to reach a target the controller
        // still accepts.
        [[nodiscard]]
        auto compensate(
            std::string_view operation,
            Error failure
        ) -> DriveOutcome;

    public:
        WindowInputAgentDrive(
            ResolvedTarget resolved,
            WgcCaptureSession session,
            DeliveryTarget delivery,
            ClientSize client
        );

        [[nodiscard]] auto clientSize() const noexcept -> ClientSize override;

        [[nodiscard]] auto capture() -> Result<Frame> override;

        [[nodiscard]]
        auto click(
            Frame const& observation,
            Point<ClientSpace> point
        ) -> DriveOutcome override;

        [[nodiscard]]
        auto scroll(
            Frame const& observation,
            Point<ClientSpace> point,
            WheelDelta delta
        ) -> DriveOutcome override;

        [[nodiscard]]
        auto key(
            Frame const& observation,
            KeyInput key
        ) -> DriveOutcome override;

        auto clearAudit() noexcept -> void override;

        [[nodiscard]] auto close() -> Status override;
    };
}

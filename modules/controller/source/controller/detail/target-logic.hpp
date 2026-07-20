#pragma once

#include "controller/discovery.hpp"

#include <optional>

namespace uf::controller_detail
{
    enum class ProcessInstanceMatch
    {
        Same,
        Different,
        Unconfirmed,
    };

    [[nodiscard]]
    constexpr auto compareProcessInstance(
        ProcessId trackedProcess,
        std::optional<ProcessStartTime> trackedStartTime,
        ProcessId observedProcess,
        std::optional<ProcessStartTime> observedStartTime
    ) noexcept -> ProcessInstanceMatch
    {
        if (trackedProcess != observedProcess)
        {
            return ProcessInstanceMatch::Different;
        }

        if (!trackedStartTime || !observedStartTime)
        {
            return ProcessInstanceMatch::Unconfirmed;
        }

        return *trackedStartTime == *observedStartTime
            ? ProcessInstanceMatch::Same
            : ProcessInstanceMatch::Different;
    }
}

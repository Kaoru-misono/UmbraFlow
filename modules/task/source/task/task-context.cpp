#include <task/task-context.hpp>

#include <core/error/result.hpp>

#include <annotation/catalog.hpp>
#include <annotation/recognition.hpp>

#include <domain/error.hpp>

#include <engine/session.hpp>

#include <optional>
#include <utility>

namespace uf::task
{
    TaskContext::TaskContext(
        engine::EngineSession session,
        TaskContextConfig config
    ) noexcept
        : m_session{std::move(session)}
        , m_config{std::move(config)}
    {
    }

    auto TaskContext::capture() -> Result<ObservationSeq>
    {
        UF_TRY_VALUE(observation, m_session.observe());

        auto const seq = m_nextSeq;
        ++m_nextSeq;
        m_observations.try_emplace(seq, std::move(observation));
        return seq;
    }

    auto TaskContext::resolvePage(ObservationSeq seq) -> Result<annotation::PageOutcome>
    {
        auto const it = m_observations.find(seq);
        if (it == m_observations.end())
        {
            return fail(
                AutomationErrorKind::StaleObservation,
                "the observation for this frame was already consumed"
            );
        }

        return it->second.resolvePage();
    }

    auto TaskContext::findAction(
        ObservationSeq seq,
        annotation::RecognizerId recognizerId
    ) -> Result<std::optional<engine::ActionFound>>
    {
        auto const it = m_observations.find(seq);
        if (it == m_observations.end())
        {
            return fail(
                AutomationErrorKind::StaleObservation,
                "the observation for this frame was already consumed"
            );
        }

        return it->second.findAction(recognizerId);
    }

    auto TaskContext::click(
        ObservationSeq pageSeq,
        ObservationSeq hitSeq,
        annotation::ResolvedPage const& page,
        engine::ActionFound const& action
    ) -> Result<engine::ActReceipt>
    {
        if (pageSeq != hitSeq)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "click received a page and a target from different observations"
            );
        }

        auto const it = m_observations.find(pageSeq);
        if (it == m_observations.end())
        {
            return fail(
                AutomationErrorKind::StaleObservation,
                "the observation for this frame was already consumed"
            );
        }

        // act consumes the observation by rvalue, so the frame is spent whatever
        // the outcome: extract it first and drop the map entry, so any later use
        // of this frame fails StaleObservation above.
        auto observation = std::move(it->second);
        m_observations.erase(it);
        return m_session.act(std::move(observation), page, action);
    }

    auto TaskContext::waitForPage(
        annotation::PageId pageId,
        std::optional<MonotonicInstant::Duration> timeout,
        std::optional<MonotonicInstant::Duration> pollInterval
    ) -> Result<WaitResolved>
    {
        UF_TRY_VALUE(
            wait,
            m_session.waitForPage(
                pageId,
                timeout.value_or(m_config.defaultWaitTimeout),
                pollInterval.value_or(m_config.defaultWaitPollInterval)
            )
        );

        // The wait's observation joins the same retained-observation map a capture
        // uses, under a fresh sequence, so the paired frame and page returned to
        // the script are consumed and cross-checked exactly like a captured frame.
        auto const seq = m_nextSeq;
        ++m_nextSeq;
        m_observations.try_emplace(seq, std::move(wait.observation));
        return WaitResolved{.seq = seq, .page = std::move(wait.page)};
    }

    void TaskContext::release(ObservationSeq seq) noexcept
    {
        // Erase by key: a seq already consumed by click, or released by an
        // earlier collection of the same frame handle, is simply absent, so this
        // is a no-op rather than a double-erase.
        m_observations.erase(seq);
    }

    auto TaskContext::liveObservationCount() const noexcept -> uint64
    {
        // size_t is never wider than uint64 on any supported platform, so this
        // widening conversion cannot lose a count.
        return static_cast<uint64>(m_observations.size());
    }

    auto TaskContext::maxLiveObservations() const noexcept -> uint64
    {
        return m_config.maxLiveObservations;
    }

    auto TaskContext::cancelled() const noexcept -> bool
    {
        return m_config.cancellation.stop_requested();
    }

    void TaskContext::markFatal() noexcept
    {
        m_fatal = true;
    }

    auto TaskContext::fatal() const noexcept -> bool
    {
        return m_fatal;
    }
}

#include "model-check-job.hpp"

#include <cstddef>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>
#include <vector>

namespace uf::workbench
{
    // std::jthread's destructor requests a stop and joins, so declaring the
    // destructor is enough to guarantee no worker outlives its job.
    ModelCheckJob::~ModelCheckJob() = default;

    auto ModelCheckJob::start(
        annotation::AuthoringDocument const& document,
        std::span<annotation::AuthoringSourceAsset const> sourceAssets,
        std::span<std::byte const> liveFrameBytes,
        annotation::RecognitionPolicy const& policy
    ) -> void
    {
        // Everything the search reads is copied here, on the caller's thread,
        // before any worker exists. The author keeps editing while the check
        // runs and a committed edit replaces the document wholesale, so a
        // capture by reference would be reading freed storage by the time the
        // sweep reached it.
        startWith(
            [
                document = document,
                assets   = std::vector<annotation::AuthoringSourceAsset>{
                    sourceAssets.begin(),
                    sourceAssets.end(),
                },
                live = std::vector<std::byte>{
                    liveFrameBytes.begin(),
                    liveFrameBytes.end(),
                },
                policy
            ](std::stop_token stop) -> Result<ModelCheck>
            {
                // The worker's own token, so destroying the job cuts a search
                // that is still sweeping a template rather than waiting it out.
                auto workerPolicy         = policy;
                workerPolicy.cancellation = std::move(stop);
                return runModelCheck(document, assets, live, workerPolicy);
            }
        );
    }

    auto ModelCheckJob::startWith(Work work) -> void
    {
        if (!work || running())
        {
            return;
        }
        // Stop before joining. A discarded worker is still sweeping until it
        // notices the token, and joining without asking would wait it out.
        if (m_worker.joinable())
        {
            m_worker.request_stop();
            m_worker.join();
        }

        m_slot   = std::make_shared<Slot>();
        m_worker = std::jthread{
            [slot = m_slot, work = std::move(work)](std::stop_token stop) -> void
            {
                auto result = work(std::move(stop));

                auto const guard = std::scoped_lock{slot->mutex};
                slot->result   = std::move(result);
                slot->finished = true;
            }
        };
    }

    auto ModelCheckJob::running() const -> bool
    {
        if (!m_slot)
        {
            return false;
        }
        auto const guard = std::scoped_lock{m_slot->mutex};
        return !m_slot->finished;
    }

    auto ModelCheckJob::discard() -> void
    {
        if (!m_slot)
        {
            return;
        }
        m_worker.request_stop();
        m_slot.reset();
    }

    auto ModelCheckJob::takeResult() -> std::optional<Result<ModelCheck>>
    {
        if (!m_slot)
        {
            return std::nullopt;
        }

        auto taken = std::optional<Result<ModelCheck>>{};
        {
            auto const guard = std::scoped_lock{m_slot->mutex};
            if (!m_slot->finished)
            {
                return std::nullopt;
            }
            taken = std::move(m_slot->result);
            m_slot->result.reset();
        }

        if (m_worker.joinable())
        {
            m_worker.join();
        }
        m_slot.reset();
        return taken;
    }
}

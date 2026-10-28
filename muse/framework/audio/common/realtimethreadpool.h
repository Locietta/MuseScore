#pragma once

#include "audioworkgroup.h"
#include "concurrency/threadutils.h"
#include "runtime.h"
#include "concurrentqueue.h"
#include "global/functional/inplace_function.h"
#include "lightweightsemaphore.h"
#include "thirdparty/kors_logger/src/log_base.h"

#include <atomic>
#include <exception>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace muse::audio {
class RealtimeThreadPool
{
    struct InflightSemaphoreRelease {
        explicit InflightSemaphoreRelease(muse::LightweightSemaphore& semaphore)
            : m_semaphore(semaphore) {}

        InflightSemaphoreRelease(const InflightSemaphoreRelease&) = delete;
        InflightSemaphoreRelease& operator=(const InflightSemaphoreRelease&) = delete;

        ~InflightSemaphoreRelease() noexcept
        {
            m_semaphore.signal();
        }

        muse::LightweightSemaphore& m_semaphore;
    };

public:
    static constexpr int maxTaskCount = 10000;
    using Task = muse::functional::inplace_function<void (), 64>;
    RealtimeThreadPool(
        std::string name,
        int num_of_workers = std::thread::hardware_concurrency())
    {
        num_of_workers = std::max(0, num_of_workers);
        m_workers.reserve(num_of_workers);
        try {
            for (size_t i = 0; i < static_cast<size_t>(num_of_workers); ++i) {
                auto worker = std::make_unique<Worker>();
                Worker* workerPtr = worker.get();
                const size_t workerIndex = i;
                worker->m_thread = std::make_unique<std::thread>(
                    [this, workerPtr, workerIndex, name] {
                    muse::runtime::setThreadName(name + "_" + std::to_string(workerIndex));
                    AudioWorkgroupToken workgroupToken;
                    for (;;) {
                        Task task;

                        m_queue.wait_dequeue(task);
                        {
                            std::lock_guard lock(workerPtr->m_workgroupMutex);
                            workerPtr->m_workgroup.join(workgroupToken);
                        }

                        if (this->m_should_stop) {
                            return;
                        }

                        InflightSemaphoreRelease release(m_inflightSemaphore);
                        task();
                    }
                });
                m_workers.push_back(std::move(worker));
                muse::setThreadPriority(*m_workers.back()->m_thread, ThreadPriority::High);
            }
        } catch (...) {
            terminate();
            throw;
        }
    }

    void setAudioWorkgroup(muse::audio::AudioWorkGroup audioworkgroup)
    {
        for (auto& worker : m_workers) {
            std::lock_guard lock(worker->m_workgroupMutex);
            worker->m_workgroup = audioworkgroup;
        }
    }

    bool enqueue(const Task& func)
    {
        m_inflightSemaphore.wait();
        if (!m_queue.enqueue(func)) {
            m_inflightSemaphore.signal();
            return false;
        }
        return true;
    }

    void participateAndWait()
    {
        Task task;
        while (m_queue.try_dequeue(task)) {
            InflightSemaphoreRelease release(m_inflightSemaphore);
            task();
        }
        waitUntilFinished();
        m_inflightSemaphore.signal(maxTaskCount);
    }

    std::set<std::thread::id> threadIdSet() const
    {
        std::set<std::thread::id> result;

        for (const auto& worker : m_workers) {
            result.insert(worker->m_thread->get_id());
        }

        return result;
    }

    int getNumberOfWorkers() const
    {
        return static_cast<int>(m_workers.size());
    }

    ~RealtimeThreadPool()
    {
        terminate();
    }

private:
    void waitUntilFinished()
    {
        auto actuallyAwaited = m_inflightSemaphore.waitMany(maxTaskCount);
        for (size_t i = 0;
             i < static_cast<size_t>(maxTaskCount - actuallyAwaited); ++i) {
            m_inflightSemaphore.wait();
        }
    }

    void terminate()
    {
        m_should_stop = true;
        for (size_t i = 0; i < m_workers.size(); ++i) {
            IF_ASSERT_FAILED(m_queue.enqueue([] {})) {
                // this is _extremely_ unlikely to fail. But if it does the threads would hang indefinitely.
                std::terminate();
            }
        }

        for (auto& worker : m_workers) {
            if (worker->m_thread && worker->m_thread->joinable()) {
                worker->m_thread->join();
            }
        }
    }

    struct Worker {
        std::unique_ptr<std::thread> m_thread;
        AudioWorkGroup m_workgroup;
        std::mutex m_workgroupMutex;
    };

    muse::BlockingConcurrentQueue<Task > m_queue;
    std::vector<std::unique_ptr<Worker> > m_workers;
    muse::LightweightSemaphore m_inflightSemaphore { maxTaskCount };
    std::atomic<bool> m_should_stop{ false };
}; // namespace muse::audio
} // namespace muse::audio

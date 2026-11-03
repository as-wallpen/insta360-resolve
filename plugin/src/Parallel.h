#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// A pool of worker threads, started once and kept: starting threads is slow on Windows (about
// 0.1 ms each), and a frame used to start over a thousand. The pool is never destroyed: Resolve
// keeps plugins loaded until it quits, and joining threads while a DLL unloads would hang.
class WorkerPool
{
public:
    static WorkerPool& Get()
    {
        static WorkerPool* pool = new WorkerPool();
        return *pool;
    }

    int size() const { return int(m_Workers); }

    // Runs p_Task on a worker some time later.
    void submit(std::function<void()> p_Task)
    {
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Tasks.push_back(std::move(p_Task));
        }
        m_Wake.notify_one();
    }

    // Runs p_Body(i) for i in [0, p_Count) on the workers and the calling thread, in chunks of
    // p_Chunk handed out as threads become free; returns when all are done. Safe to call from
    // several threads at once and from inside a task: the caller works on its own job too, so it
    // finishes even when every worker is busy.
    void parallelFor(int p_Count, int p_Chunk, const std::function<void(int)>& p_Body)
    {
        if (p_Count <= 0)
        {
            return;
        }
        p_Chunk = std::max(1, p_Chunk);
        auto job = std::make_shared<Job>();
        job->count = p_Count;
        job->chunk = p_Chunk;
        job->chunks = (p_Count + p_Chunk - 1) / p_Chunk;
        job->body = &p_Body;
        const int helpers = std::min(job->chunks - 1, int(m_Workers));
        for (int i = 0; i < helpers; ++i)
        {
            submit([job]() { job->work(); });
        }
        job->work();
        std::unique_lock<std::mutex> lock(job->mutex);
        job->finished.wait(lock, [&]() { return job->done == job->chunks; });
    }

private:
    struct Job
    {
        int count = 0, chunk = 1, chunks = 0;
        const std::function<void(int)>* body = nullptr; // valid until done == chunks
        std::atomic<int> next{0};
        int done = 0;
        std::mutex mutex;
        std::condition_variable finished;

        void work()
        {
            int mine = 0;
            for (int c = next++; c < chunks; c = next++)
            {
                const int end = std::min(count, (c + 1) * chunk);
                for (int i = c * chunk; i < end; ++i)
                {
                    (*body)(i);
                }
                ++mine;
            }
            if (mine > 0)
            {
                std::lock_guard<std::mutex> lock(mutex);
                done += mine;
                if (done == chunks)
                {
                    finished.notify_all();
                }
            }
        }
    };

    WorkerPool()
    {
        m_Workers = std::max(1u, std::thread::hardware_concurrency()) - 1;
        for (unsigned i = 0; i < m_Workers; ++i)
        {
            std::thread([this]() { run(); }).detach();
        }
    }

    void run()
    {
        for (;;)
        {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(m_Mutex);
                m_Wake.wait(lock, [&]() { return !m_Tasks.empty(); });
                task = std::move(m_Tasks.front());
                m_Tasks.pop_front();
            }
            task();
        }
    }

    unsigned m_Workers = 0;
    std::mutex m_Mutex;
    std::condition_variable m_Wake;
    std::deque<std::function<void()>> m_Tasks;
};

// Runs p_Body(i) for i in [0, p_Count) on all cores, in chunks of p_Chunk; returns when done.
template <class Body>
void ParallelFor(int p_Count, int p_Chunk, const Body& p_Body)
{
    const std::function<void(int)> body = std::cref(p_Body);
    WorkerPool::Get().parallelFor(p_Count, p_Chunk, body);
}

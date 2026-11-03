#pragma once

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

// Runs p_Body(i) for i in [0, p_Count) on all cores, in chunks of p_Chunk handed out as threads
// become free. Returns when all are done. Used where the macOS original ran a Metal kernel.
template <class Body>
void ParallelFor(int p_Count, int p_Chunk, const Body& p_Body)
{
    if (p_Count <= 0)
    {
        return;
    }
    p_Chunk = std::max(1, p_Chunk);
    const int chunks = (p_Count + p_Chunk - 1) / p_Chunk;
    const int threads = std::max(1, std::min<int>(chunks, int(std::thread::hardware_concurrency())));
    std::atomic<int> next{0};
    auto work = [&]() {
        for (int c = next++; c < chunks; c = next++)
        {
            const int end = std::min(p_Count, (c + 1) * p_Chunk);
            for (int i = c * p_Chunk; i < end; ++i)
            {
                p_Body(i);
            }
        }
    };
    if (threads == 1)
    {
        work();
        return;
    }
    std::vector<std::thread> pool;
    pool.reserve(threads - 1);
    for (int t = 1; t < threads; ++t)
    {
        pool.emplace_back(work);
    }
    work();
    for (std::thread& t : pool)
    {
        t.join();
    }
}

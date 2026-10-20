// The replay thread (NX_REPLAY_THREAD=0 turns it off). nvnQueueSubmitCommands and nvnQueuePresentTexture used to
// replay into Vulkan and wait for the GPU on the game's render thread, and the game's main thread waits for that
// thread every frame: the frame was game logic + recording + replay + GPU, one after the other (~30 fps in a course
// with every part under 16 ms; docs/nvn/DEBUG.md #40). Like a real GPU, the work now goes to a queue that one host
// thread replays in order; the game only waits where NVN makes it wait: nvnSyncWait on a fenced sync, nvnQueueFinish,
// memory pool invalidation and finalization, and a present queue at most two presents deep.
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <system_error>
#include <thread>

#include "nvn.h"
#include "platform.h"

namespace nx::nvn {

namespace {

std::mutex g_lock;
std::condition_variable g_cv, g_done_cv;
std::deque<std::pair<uint64_t, std::function<void()>>> g_jobs;
uint64_t g_enqueued = 0, g_done = 0;
bool g_started = false;
// Where the replay thread's time goes, logged every 10 s: running jobs, or waiting for one (the game is behind), and
// how long the game's threads waited for it (worker_wait).
std::atomic<uint64_t> g_busy_ns{0}, g_idle_ns{0}, g_waited_ns{0};

bool enabled() {
    static const bool on = [] {
        // Off unless NX_REPLAY_THREAD=1: with the replay a frame behind, the course's render passes multiplied
        // (1.3 M ended by target changes against 125 k) and it ran slower (docs/nvn/DEBUG.md #40).
        const char* e = std::getenv("NX_REPLAY_THREAD");
        return e && e[0] == '1';
    }();
    return on;
}

void run() {
    plat::set_thread_name("nvn replay");
    uint64_t logged_at = plat::monotonic_ns();
    for (;;) {
        std::pair<uint64_t, std::function<void()>> job;
        const uint64_t t0 = plat::monotonic_ns();
        {
            std::unique_lock lk(g_lock);
            g_cv.wait(lk, [] { return !g_jobs.empty(); });
            job = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        const uint64_t t1 = plat::monotonic_ns();
        job.second();
        const uint64_t t2 = plat::monotonic_ns();
        g_idle_ns += t1 - t0, g_busy_ns += t2 - t1;
        if (t2 - logged_at >= 10'000'000'000ull) {
            const double span = double(t2 - logged_at);
            log("nvn replay thread: busy %.0f%%, waiting for work %.0f%%; the game waited for it %.0f ms/s",
                100.0 * double(g_busy_ns.exchange(0)) / span, 100.0 * double(g_idle_ns.exchange(0)) / span,
                double(g_waited_ns.exchange(0)) / 1e6 / (span / 1e9));
            logged_at = t2;
        }
        {
            std::lock_guard lk(g_lock);
            g_done = job.first;
        }
        g_done_cv.notify_all();
    }
}

}  // namespace

uint64_t worker_run(std::function<void()> job) {
    if (!enabled()) {
        job();
        return 0;
    }
    uint64_t serial = 0;
    {
        std::lock_guard lk(g_lock);
        if (!g_started) {
            try {
                std::thread(run).detach();
                g_started = true;
            } catch (const std::system_error&) {
                // No thread to be had (the PS4 out of memory): the job runs here, and the start is tried again next time.
            }
        }
        if (g_started) {
            serial = ++g_enqueued;
            g_jobs.push_back({serial, std::move(job)});
        }
    }
    if (serial) {
        g_cv.notify_one();
        return serial;
    }
    job();  // worker_wait(0) returns at once
    return 0;
}

void worker_wait(uint64_t serial) {
    if (!serial) return;
    const uint64_t t0 = plat::monotonic_ns();
    {
        std::unique_lock lk(g_lock);
        g_done_cv.wait(lk, [&] { return g_done >= serial; });
    }
    g_waited_ns += plat::monotonic_ns() - t0;
}

uint64_t worker_last() {
    std::lock_guard lk(g_lock);
    return g_enqueued;
}

void worker_finish() { worker_wait(worker_last()); }

}  // namespace nx::nvn

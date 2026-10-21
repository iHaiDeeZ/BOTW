// A janitor thread for work that need not hold up the replay (docs/nvn/android-perf/PHASE4.md C2b): resetting a
// retired slot's command pool and descriptor pool. On Adreno vkResetCommandPool frees the driver's per-pass objects,
// ~1.2 ms per course frame on the replay thread. A retired slot is not touched again until frame_begin takes it, so
// its pools can be reset here meanwhile (Vulkan's external synchronisation holds: one thread at a time), and
// frame_begin waits for the slot's flag, which is normally set long before. NX_JANITOR=0: the resets stay inline.
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <system_error>
#include <thread>

#include "nvn_vk.h"
#include "platform.h"

namespace nx::nvn {

namespace {

std::mutex g_lock;
std::condition_variable g_cv, g_done_cv;
std::deque<std::function<void()>> g_jobs;
bool g_started = false;

void run() {
    plat::set_thread_name("nvn janitor");
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lk(g_lock);
            g_cv.wait(lk, [] { return !g_jobs.empty(); });
            job = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        job();
        {
            std::lock_guard lk(g_lock);  // pairs with janitor_wait's predicate check
        }
        g_done_cv.notify_all();
    }
}

}  // namespace

bool janitor_on() {
    static const bool on = [] {
        const char* e = std::getenv("NX_JANITOR");
        return !(e && e[0] == '0');
    }();
    return on;
}

void janitor_run(std::function<void()> job) {
    bool queued;
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
        queued = g_started;
        if (queued) g_jobs.push_back(std::move(job));
    }
    if (queued)
        g_cv.notify_one();
    else
        job();
}

void janitor_wait(const std::atomic<bool>& done) {
    for (int k = 0; k < 200 && !done.load(std::memory_order_acquire); k++) plat::yield();  // normally done already
    if (done.load(std::memory_order_acquire)) return;
    std::unique_lock lk(g_lock);
    while (!g_done_cv.wait_for(lk, std::chrono::milliseconds(25),
                               [&] { return done.load(std::memory_order_acquire); })) {
    }  // re-checked every 25 ms: a lost notify (kernel.cpp wait_slice)
}

}  // namespace nx::nvn

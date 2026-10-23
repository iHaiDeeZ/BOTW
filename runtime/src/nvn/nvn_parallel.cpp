// Parallel passes (NX_PARALLEL=1). The replay thread walks a submit's commands as before, but the draws of a big pass
// (one whose targets had NX_PARALLEL_MIN draws or more the last time they were rendered, nvn_draw.cpp) are only
// resolved there: their textures with any upload or layout change, their image descriptors, the pipeline. Each is
// handed over as a job with a copy of the bound state, and worker threads copy its buffers and vertex inputs, build
// and bind its descriptor sets and record the draw into a secondary command buffer, NX_PARALLEL_SEGMENT jobs to a
// buffer. The pass executes the buffers in order when it ends. Breath of the Wild outdoors replayed ~3,000 draws a
// frame at ~33 us each on the one thread (9 fps), with most of the PS4's cores idle.
//
// The workers read guest memory after the walk has gone on: the replay's own writes into it (UpdateUniformBuffer,
// ReportCounter, nvn_draw_state.cpp) first wait until every job pushed before them is recorded (parallel_flush). A
// submit with dispatches is replayed on the one thread, as before (its compute results are read within the submit).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <vector>

#include "nvn_draw_state.h"
#include "platform.h"

namespace nx::nvn::replay {

using namespace nx::vk;

namespace {

struct Segment {
    std::vector<DrawJob*> jobs;
    VkCommandBuffer cb = VK_NULL_HANDLE;  // recorded (null: it could not be)
};

std::mutex g_lock;
std::condition_variable g_work_cv, g_done_cv;
std::deque<Segment*> g_queue;  // under g_lock
uint32_t g_unfinished = 0;     // segments queued and not recorded yet, under g_lock
int g_workers = 0;
bool g_submit_ok = false;  // this submit may (parallel_begin_submit)
PassFormats g_formats;     // the open pass's, set before its first segment is queued
uint32_t g_segment_size = 48;

// The replay thread's: the open pass's segments in order, and the jobs, kept for the next passes (deques: what was
// handed to a worker stays where it is while more are added).
std::deque<Segment> g_segments;
size_t g_segments_used = 0;
Segment* g_open = nullptr;  // being filled
std::deque<DrawJob> g_jobs;
size_t g_jobs_used = 0;
std::vector<VkCommandBuffer> g_execute;

thread_local int t_worker = -1;

// Per log period.
uint64_t g_passes = 0, g_jobs_done = 0, g_segments_done = 0, g_flushes = 0, g_wait_ns = 0, g_submits = 0;
std::atomic<uint64_t> g_worker_ns{0};

void worker_main(void* arg) {
    t_worker = int(reinterpret_cast<intptr_t>(arg));
    char name[16];
    std::snprintf(name, sizeof name, "nvn worker %d", t_worker);
    plat::set_thread_name(name);
    plat::place_frame_thread(2);
    sets_thread_init(), dedup_thread_init(), draw_thread_init();
    for (;;) {
        Segment* s = nullptr;
        {
            std::unique_lock lk(g_lock);
            // Re-checked every 25 ms: a lost notify (kernel.cpp wait_slice).
            while (g_queue.empty()) g_work_cv.wait_for(lk, std::chrono::milliseconds(25));
            s = g_queue.front();
            g_queue.pop_front();
        }
        const uint64_t t0 = plat::monotonic_ns();
        VkCommandBuffer cb = frame_secondary(t_worker);
        if (cb) {
            VkCommandBufferInheritanceRenderingInfo ri{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO};
            ri.colorAttachmentCount = g_formats.color_count;
            ri.pColorAttachmentFormats = g_formats.colors;
            ri.depthAttachmentFormat = g_formats.depth;
            ri.stencilAttachmentFormat = g_formats.stencil;
            ri.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkCommandBufferInheritanceInfo ii{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, &ri};
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
            bi.pInheritanceInfo = &ii;
            if (p_vkBeginCommandBuffer(cb, &bi) == VK_SUCCESS) {
                draw_segment_begin(cb);
                for (DrawJob* j : s->jobs) draw_job(*j, cb);
                p_vkEndCommandBuffer(cb);
            } else {
                cb = VK_NULL_HANDLE;
            }
        }
        if (!cb) {
            static std::atomic<int> logged{0};
            if (logged++ < 10) log("nvn: a parallel pass's segment of %zu draws could not be recorded", s->jobs.size());
        }
        g_worker_ns += plat::monotonic_ns() - t0;
        {
            std::lock_guard lk(g_lock);
            s->cb = cb;
            g_unfinished--;
        }
        g_done_cv.notify_all();
    }
}

bool start() {
    const char* e = std::getenv("NX_PARALLEL_WORKERS");
    int n = e ? std::atoi(e) : 3;
    n = n < 1 ? 1 : n > kMaxReplayWorkers ? kMaxReplayWorkers : n;
    if (const char* s = std::getenv("NX_PARALLEL_SEGMENT")) g_segment_size = uint32_t(std::max(1, std::atoi(s)));
    for (int k = 0; k < n; k++) plat::start_thread(worker_main, reinterpret_cast<void*>(intptr_t(k)), 1u << 20);
    g_workers = n;
    log("nvn: parallel passes: %d workers, %u draws to a secondary command buffer (NX_PARALLEL)", n, g_segment_size);
    return true;
}

void queue_open() {
    if (!g_open) return;
    {
        std::lock_guard lk(g_lock);
        g_queue.push_back(g_open);
        g_unfinished++;
    }
    g_work_cv.notify_one();
    g_open = nullptr;
    g_segments_done++;
}

void wait_all() {
    queue_open();
    const uint64_t t0 = plat::monotonic_ns();
    std::unique_lock lk(g_lock);
    while (g_unfinished) g_done_cv.wait_for(lk, std::chrono::milliseconds(25));  // re-checked: a lost notify
    g_wait_ns += plat::monotonic_ns() - t0;
}

}  // namespace

int parallel_worker() { return t_worker; }

void parallel_begin_submit(bool allowed) { g_submit_ok = allowed, g_submits++; }

bool parallel_on() {
    static const bool on = [] {
        const char* e = std::getenv("NX_PARALLEL");
        return e && e[0] == '1';
    }();
    if (!on || !g_submit_ok || !async_gpu()) return false;  // the workers' pools live in the submit's slot
    static const bool started = start();
    return started && g_workers > 0;
}

DrawJob& parallel_job() {
    if (g_jobs_used == g_jobs.size()) g_jobs.emplace_back();
    return g_jobs[g_jobs_used];
}

void parallel_pass_begin(const PassFormats& f) {
    g_formats = f;
    g_segments_used = 0, g_jobs_used = 0, g_open = nullptr;
    g_passes++;
}

void parallel_push(DrawJob& j) {
    g_jobs_used++;  // j is the job parallel_job() gave
    if (!g_open) {
        if (g_segments_used == g_segments.size()) g_segments.emplace_back();
        g_open = &g_segments[g_segments_used++];
        g_open->jobs.clear(), g_open->cb = VK_NULL_HANDLE;
    }
    g_open->jobs.push_back(&j);
    if (g_open->jobs.size() >= g_segment_size) queue_open();
}

void parallel_flush() {
    if (!g_segments_used) return;
    wait_all();
    g_flushes++;
}

void parallel_pass_end(VkCommandBuffer cb) {
    wait_all();
    g_execute.clear();
    for (size_t k = 0; k < g_segments_used; k++)
        if (g_segments[k].cb) g_execute.push_back(g_segments[k].cb);
    if (!g_execute.empty()) p_vkCmdExecuteCommands(cb, uint32_t(g_execute.size()), g_execute.data());
    g_jobs_done += g_jobs_used;
    g_segments_used = 0, g_jobs_used = 0;
}

void parallel_log() {
    if (!g_passes) {
        g_submits = 0;
        return;
    }
    const double n = double(std::max<uint64_t>(g_submits, 1));  // per submit (a frame in Breath of the Wild)
    log("nvn: parallel passes per submit over %llu: %.1f passes, %.0f draws in %.1f segments, %.1f flushes (uniform "
        "updates); workers busy %.2f ms, replay thread waiting %.2f ms",
        (unsigned long long)g_submits, double(g_passes) / n, double(g_jobs_done) / n, double(g_segments_done) / n,
        double(g_flushes) / n, double(g_worker_ns.exchange(0)) / 1e6 / n, double(g_wait_ns) / 1e6 / n);
    g_submits = 0;
    g_passes = g_jobs_done = g_segments_done = g_flushes = g_wait_ns = 0;
}

}  // namespace nx::nvn::replay

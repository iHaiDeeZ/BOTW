// The replay of recorded NVN command lists into a Vulkan command buffer (split from nvn_vk.cpp): render targets,
// clears, calls into other lists, copies; draws and state go to nvn_draw.cpp.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "nvn_draw_state.h"
#include "nvn_vk.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {
// The lists called during one submit, by handle (PHASE4.md C5): command_list() takes a mutex and walks a map, and a
// course frame calls hundreds of lists, each looked up twice (the dispatch count, then the replay). The first lookup
// of a handle in a submit is kept for the rest of it: a list re-recorded meanwhile on the game's recording thread
// is not mixed into a submit already being replayed. NX_CALL_TABLE=0: every call looks up.
// An open-addressed table emptied by a generation bump (no frees at each submit), the lists owned by g_called_owned
// until the next submit: the unordered_map (a node per list, freed at every reset) and the shared_ptr copied out of
// it at every CallList were ~2% of the PS4 replay thread in the cave.
struct CalledSlot {
    uint64_t handle = 0, generation = 0;
    const CmdList* list = nullptr;
};
constexpr uint32_t kCalledSlots = 8192;
CalledSlot g_called[kCalledSlots];
uint64_t g_called_generation = 1;
std::vector<std::shared_ptr<CmdList>> g_called_owned;
std::unordered_map<uint64_t, std::shared_ptr<CmdList>> g_called_overflow;  // past half of the table
}  // namespace

void submit_lists_reset() {
    g_called_generation++;
    g_called_owned.clear();
    if (!g_called_overflow.empty()) g_called_overflow.clear();
}

const CmdList* submit_command_list(uint64_t handle) {
    static const bool off = [] {
        const char* e = std::getenv("NX_CALL_TABLE");
        return e && e[0] == '0';
    }();
    if (off) {  // kept alive until the next submit, as the table's
        auto l = command_list(handle);
        if (l) g_called_owned.push_back(l);
        return l.get();
    }
    uint32_t at = uint32_t((handle * 0x9E3779B97F4A7C15ull) >> 51);  // 13 bits
    for (uint32_t k = 0; k < kCalledSlots; k++) {
        CalledSlot& s = g_called[(at + k) & (kCalledSlots - 1)];
        if (s.generation != g_called_generation) {
            if (g_called_owned.size() >= kCalledSlots / 2) break;  // full enough: the overflow map
            auto l = command_list(handle);
            if (!l) return nullptr;  // looked up again next time, as before
            s = {handle, g_called_generation, l.get()};
            g_called_owned.push_back(std::move(l));
            return s.list;
        }
        if (s.handle == handle) return s.list;
    }
    auto it = g_called_overflow.find(handle);
    if (it != g_called_overflow.end()) return it->second.get();
    auto l = command_list(handle);
    if (l) g_called_overflow.emplace(handle, l);
    return l.get();
}

void replay_list(VkCommandBuffer cb, Targets& st, const CmdList& list, int depth) {
    if (skin_lists_on()) skin_list_replayed(list, depth);
    for (const Cmd& c : list.cmds) {
        cb = frame_chunk(cb, replay::g_stats.done, pass_open());  // part of the submit queued early (PLAN.md R1)
        // NX_TRACE_CMDS=<first draw>: the ops replayed from that draw on (20000), with n, mask and a[0].
        static const char* tc = std::getenv("NX_TRACE_CMDS");
        static const uint64_t tc_from = tc ? std::strtoull(tc, nullptr, 10) : 0;
        static int tc_n = 0;
        if (tc && replay::g_stats.done >= tc_from && tc_n++ < 20000)
            log("cmd after draw %llu: op %u n %u mask %x a0 %llx a1 %llx i %d %d %d %d f %g %g %g %g depth %d",
                (unsigned long long)replay::g_stats.done, unsigned(c.op), c.n, c.mask, (unsigned long long)c.a[0],
                (unsigned long long)c.a[1], c.i[0], c.i[1], c.i[2], c.i[3], c.f[0], c.f[1], c.f[2], c.f[3], depth);
        // NX_DIAG_CMDS=1: the storyboard frame's commands that are not draws or clears, into diag/passes.txt (the
        // order of targets, state and clears around a pass).
        static const bool diag_cmds = std::getenv("NX_DIAG_CMDS") != nullptr;
        if (diag_cmds && diag_active() &&
            (c.op == Cmd::SetRenderTargets || c.op == Cmd::CallList || c.op == Cmd::SetDepthRange ||
             c.op == Cmd::BindDepthStencilState || c.op == Cmd::BindPolygonState || c.op == Cmd::SetRenderEnable ||
             c.op == Cmd::SetRasterizerDiscard || c.op == Cmd::CopyTextureToTexture || c.op == Cmd::SetDepthClamp ||
             c.op == Cmd::SetDepthBounds || c.op == Cmd::ReportCounter)) {
            char b[200];
            std::snprintf(b, sizeof b, "  cmd op %u n %u mask %x a %llx %llx a8 %llx i %d %d f %g %g depth %d", unsigned(c.op),
                          c.n, c.mask, (unsigned long long)c.a[0], (unsigned long long)c.a[1], (unsigned long long)c.a[8],
                          c.i[0], c.i[1], c.f[0], c.f[1], depth);
            diag_line(b);
        }
        switch (c.op) {
            case Cmd::SetRenderTargets: {  // the pass ends only when a draw or clear needs other targets
                static const bool always = std::getenv("NX_TARGETS_BREAK") != nullptr;  // bisecting: the old way
                if (always) draw_end_rendering(cb, 7);
                st.color_count = c.n;
                for (uint32_t k = 0; k < 8; k++) st.colors[k] = k < c.n ? c.a[k] : 0;
                st.depth = c.a[8];
                st.views = {};
                if (c.blob < list.blobs.size() && list.blobs[c.blob].size() == sizeof(TargetViews))
                    std::memcpy(&st.views, list.blobs[c.blob].data(), sizeof st.views);
                break;
            }
            case Cmd::ClearColor:
            case Cmd::ClearDepthStencil: draw_clear(cb, st, c); break;
            case Cmd::CallList:
                if (depth < 8) {
                    if (auto l = submit_command_list(c.a[0])) {
                        replay_list(cb, st, *l, depth + 1);
                        cb = frame_cb(cb);  // NX_PIPELINED: the called list may have split the submit
                    } else {
                        static uint64_t missing = 0;
                        if (++missing <= 20 || missing % 1000 == 0)
                            log("nvn: called handle %llx is unknown (%llu calls of unknown handles): %s",
                                (unsigned long long)c.a[0], (unsigned long long)missing,
                                explain_handle(c.a[0]).c_str());
                    }
                }
                break;
            case Cmd::CopyTextureToTexture:
                draw_end_rendering(cb, 5);
                if (gpu_profile_on()) gpu_profile_mark(cb, "texture copies");
                replay_copy(cb, c, list);
                if (gpu_profile_on()) gpu_profile_mark(cb, "outside passes (clears, uploads, copies)");
                break;
            case Cmd::CopyTextureToBuffer:
                draw_end_rendering(cb, 5);
                replay_copy_to_buffer(cb, c, list);
                break;
            case Cmd::ClearTexture: replay_clear_texture(cb, c, list); break;
            default:
                draw_command(cb, c, list, st);
                // NX_PIPELINED: after the submit's last dispatch the rest goes into a second command buffer.
                if (c.op == Cmd::DispatchCompute) cb = frame_after_dispatch(cb);
                break;
        }
    }
}

}  // namespace nx::nvn

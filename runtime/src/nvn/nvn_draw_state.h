// The draw replay's bound state (nvn_draw.cpp), shared with its debug traces (nvn_draw_trace.cpp).
#pragma once

#include <array>
#include <utility>
#include <vector>

#include "nvn_pipeline.h"

namespace nx::nvn::replay {

constexpr int kStages = 6;  // NVN shader stages: 0 vertex, 1 fragment, 2 geometry, 3/4 tessellation, 5 compute

struct Range {
    uint64_t address = 0, size = 0;
};

struct State {
    uint64_t program = 0;
    Range uniforms[kStages][18], storage[kStages][16], vertex_buffers[16];
    uint64_t textures[kStages][32] = {};  // NVN texture handles by sampler binding
    uint64_t images[kStages][16] = {};    // NVN image handles by image binding (BindImage)
    uint64_t texture_pool = 0, sampler_pool = 0;
    std::vector<VertexAttrib> attribs;
    std::vector<VertexStream> streams;
    BlendTarget blend[8];
    ColorStateInfo color;
    ChannelMaskInfo mask;
    DepthStencilInfo depth;
    PolygonInfo polygon;
    float blend_color[4] = {};
    int32_t viewport[4] = {}, scissor[4] = {};
    bool rasterizer_discard = false, render_enable = true;  // SetRasterizerDiscard / SetRenderEnable: draws skipped
    bool has_viewport = false, has_scissor = false;
    float depth_near = 0, depth_far = 1;
    // Command-buffer rasterizer state (nvn_raster.cpp), per face 0 front, 1 back. NVN's defaults: masks 0xFF, ref 0.
    uint8_t stencil_ref[2] = {0, 0}, stencil_compare_mask[2] = {0xFF, 0xFF}, stencil_write_mask[2] = {0xFF, 0xFF};
    float bias_factor = 0, bias_units = 0, bias_clamp = 0;  // SetPolygonOffsetClamp
    bool depth_bounds = false, depth_clamp = false;
    bool alpha_to_coverage = false;  // BindMultisampleState (G7)
    float depth_bounds_range[2] = {0, 1};
};

struct Stats {
    uint64_t total = 0, no_program = 0, no_vertex = 0, no_pipeline = 0, no_indices = 0, done = 0, to_window = 0;
    uint64_t dispatches = 0;
    uint64_t breaks_textures = 0, breaks_targets = 0;  // render passes ended by a draw: uploads/layouts, new targets
};

// The bound state the replay code reads: the walk's (g_walk_state, which the state commands change) on the replay
// thread, a draw's copy on a worker recording a parallel pass (nvn_parallel.cpp). g_state is that state.
extern State g_walk_state;
// Inline and constant-initialized, as t_replay_mark (nvn.h): an extern thread_local would be reached through a TLS
// init function the PS4 loader cannot resolve (create-fself: "missing module for symbol _ZTH...").
inline thread_local State* t_state = &g_walk_state;
#define g_state (*::nx::nvn::replay::t_state)
extern Stats g_stats;

const uint8_t* guest_bytes(uint64_t gpu);
uint32_t handle_texture(uint64_t handle, uint32_t* sampler);
// The texture and sampler a handle names in the bound pools.
void texture_for(uint64_t handle, uint64_t& texture, SamplerInfo& sampler, ViewInfo* view = nullptr);
std::string explain_texture_slot(uint64_t handle);  // NX_FRAME_SIG: the slot at the mark and now (nvn_sets.cpp)

// NX_SNAP_FOLLOW (nvn_draw_trace.cpp): log and snapshot this draw.
bool snap_follow(const Targets& t, const ShaderInfo* vs, const ShaderInfo* fs, const std::vector<uint64_t>& handles);
// NX_TRACE_NAN=1 (nvn_draw_trace.cpp): non-finite or huge floats in what compute shaders wrote, with the shader.
void trace_nan_dispatch(const ShaderInfo* cs);
void trace_nan_write_back(const uint8_t* from, const uint8_t* to, uint64_t size);
// NX_TRACE_PASSES=<first draw> (nvn_draw_trace.cpp): 4000 pass events from that draw on (begin with its load ops,
// end with its reason, clears deferred or done in the pass), each with its colour target 0 and depth.
void trace_pass(const char* what, const Targets& t, uint32_t a, uint32_t b);
using TexBindings = std::vector<std::pair<const ShaderInfo*, const DescriptorBinding*>>;
// NX_TRACE_DRAWS & co (docs/nvn/DEBUG.md): logs a replayed draw with its state, when the switches select it.
void trace_draw(const Targets& t, uint32_t mode, bool indexed, const Cmd& c, const ShaderInfo* vs,
                const ShaderInfo* fs, const TexBindings& tex_bindings, const std::vector<uint64_t>& handles);

}  // namespace nx::nvn::replay

namespace nx::nvn {
uint64_t submit_start_ns();  // host ns at the start of the submit being replayed
bool draw_call(VkCommandBuffer cb, const Targets& t, uint32_t mode, bool indexed, const Cmd& c);
void dispatch_call(VkCommandBuffer cb, const Cmd& c);
}  // namespace nx::nvn

namespace nx::nvn::replay {

// nvn_dedup.cpp: one copy per submit of the same guest bytes (padded to `padded` with zeros).
Upload dedup_upload(const uint8_t* p, uint64_t size, uint64_t padded, uint64_t align);
// What the next uploads are for (dedup_log counts the bytes copied per kind): 0 other, 1 vertex, 2 index, 3 uniform,
// 4 storage.
inline thread_local int g_upload_kind = 0;
// The next upload may be served by a longer copy of the same address (its first bytes): set around a uniform bind
// shortened to what the shader reads (nvn_sets.cpp), whose tail nothing reads.
inline thread_local bool g_upload_prefix_ok = false;
uint64_t dedup_copied_bytes();  // this thread's
struct UploadStats {
    uint64_t vertex_by_pool[8] = {}, uniform_by_pool[8] = {};  // bytes copied, by pool flags & 7
};
UploadStats& upload_stats();  // this thread's (dedup_log sums them)
void dedup_reset();                                     // at each submit
void dedup_invalidate(const uint8_t* p, uint64_t size);  // guest bytes written during the replay (every thread's copies)
void dedup_log();
// nvn_written.cpp: compute results within a submit (docs/nvn/GLITCH-PLAN.md G5), one copy per written guest range.
bool written_separate();  // NX_COMPUTE_SEPARATE=1: a copy per binding, as before
// A dispatch's storage binding it writes: the range's copy (made, reused or merged), kAlign aligned.
Upload written_bind(VkCommandBuffer cb, const uint8_t* p, uint64_t size, uint64_t padded);
bool written_graphics_bind(const uint8_t* p, uint64_t size, uint64_t padded, Upload& out);  // vertex/fragment writers
// Bytes read by later work: the written copy when [p, p + size) meets one (may end the render pass), else false.
bool written_read(VkCommandBuffer cb, const uint8_t* p, uint64_t size, uint64_t padded, uint64_t align, Upload& out);
Upload read_upload(VkCommandBuffer cb, const uint8_t* p, uint64_t size, uint64_t padded, uint64_t align);  // or dedup
// nvn_vcache.cpp: vertex data of CPU_CACHED pools kept from frame to frame.
bool vcache_bind(const uint8_t* p, uint64_t size, uint64_t padded, Upload& out);
void vcache_begin_submit();
void vcache_log();
bool written_overlaps(const uint8_t* p, uint64_t size);
// A draw's storage binding over a written region (it may write the copy): counted as written_bind calls are, so a
// pipelined split (nvn_frames.cpp) does not copy the region back before that draw ran.
void written_count_bind();
bool written_indirect(VkCommandBuffer cb, const uint8_t* p);  // vkCmdDispatchIndirect when a dispatch wrote {x, y, z}
// A draw's vertex buffers (the streams `vs` reads) and index buffer; false: its indices are not in guest memory.
struct DrawInputs {
    Upload vertex[15], index;
    uint32_t used = 0;  // bit s: stream s
    VkIndexType index_type = VK_INDEX_TYPE_UINT16;
};
bool draw_inputs(VkCommandBuffer cb, const ShaderInfo* vs, bool indexed, const Cmd& c, DrawInputs& in);
// NX_CHECK_VERTS=1: logs draws that fetch vertices or instances past their bound vertex buffer range (debugging).
void check_vertex_ranges(const ShaderInfo* vs, const ShaderInfo* fs, bool indexed, const Cmd& c, const DrawInputs& in);

// nvn_sets.cpp: the descriptor sets of a draw or dispatch, and the replay's CPU shortcuts (docs/nvn/PERF-PLAN.md).
// NX_DRAW_SLOW=1 turns every shortcut off, for bisecting; letters instead of 1 pick them: m the scratch memory kept
// from draw to draw, e empty descriptor sets not allocated, h the submit's handle cache, s state the command buffer
// already has not set again (nvn_draw.cpp), u uploads shared (indices, zero stream, support buffer, c[0]).
enum : uint32_t { kSlowScratch = 1, kSlowEmptySets = 2, kSlowHandles = 4, kSlowState = 8, kSlowUploads = 16 };
bool draw_slow(uint32_t what);
// What a texture binding of the draw being replayed samples.
struct ResolvedTexture {
    uint64_t texture = 0;  // guest NVNtexture*; 0: nothing behind the binding (the dummy)
    SamplerInfo sampler;
    ViewInfo view;
    VkSampler vk_sampler = VK_NULL_HANDLE;  // sampler_for(sampler, shadow) when the handle cache has it
};
struct DrawTextures {
    TexBindings bindings;  // the set 2 bindings of the program's stages
    std::vector<uint64_t> handles;
    std::vector<ResolvedTexture> resolved;
};
extern const ShaderInfo* g_last_draw_fs;  // nvn_draw.cpp: the last draw's fragment shader
extern DrawTextures g_textures;  // filled by gather_textures, read by build_sets and the traces of the same draw
// kTexturesPending: a texture is being decoded in the background (astc_pending): the draw is skipped this time.
enum : uint32_t { kNeedBreak = 1, kSampledPending = 2, kTexturesPending = 4 };
uint32_t gather_textures(const std::vector<const ShaderInfo*>& shaders, bool may_defer = false);
// Why gather_textures last asked for kNeedBreak (the pass statistics count them): kBreakStorage a storage image in the
// program, kBreakNotReady a texture to upload or transition, kBreakFeedback one the open pass wrote, kBreakClear one
// with a pending clear.
enum : uint32_t { kBreakStorage = 1, kBreakNotReady = 2, kBreakFeedback = 4, kBreakClear = 8 };
inline uint32_t g_break_why = 0;

// The sets being built for a draw: scratch kept from draw to draw, or a parallel pass's job's own.
struct SetWrite {  // one write of a set, as the set cache compares it
    uint32_t binding = 0, type = 0;
    uint64_t a = 0, b = 0, c = 0;  // buffer, offset, range; or view, sampler, layout
};
struct Descriptors {
    VkDescriptorSet sets[4] = {};
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<uint8_t> set_index;  // the set of each write (its dstSet is filled when the set is known)
    std::vector<VkDescriptorBufferInfo> buffers;
    std::vector<VkDescriptorImageInfo> images;
    std::vector<SetWrite> key;  // scratch for the set cache
    uint64_t dynamic = 0;              // set 0 bindings given a dynamic offset (ProgramLayout::dynamic)
    uint32_t dynamic_offset[64] = {};  // their ring offsets, by binding
};
// build_sets in its parts, for parallel passes: the walk makes a job's image descriptors (sets 2 and 3: what textures
// sample, with their uploads and layouts), the worker its buffer descriptors (sets 0 and 1: the guest bytes copied)
// and binds the sets. g_sets is `d` from sets_target(&d) on, this thread's own after sets_target(nullptr).
void sets_target(Descriptors* d);
void begin_sets(const std::vector<const ShaderInfo*>& shaders, const ProgramLayout& layout);
void build_buffers(VkCommandBuffer cb, const std::vector<const ShaderInfo*>& shaders, const ProgramLayout& layout);
void build_images(VkCommandBuffer cb, const std::vector<const ShaderInfo*>& shaders, const ProgramLayout& layout);
bool set_cache_on();
// The set caches, dedup copies and draw state of the calling worker: its own, from its first job on.
void sets_thread_init();
void dedup_thread_init();
void draw_thread_init();
// nvn_upload.cpp: true while `texture`'s ASTC texels are decoded in the background, which this starts when the draw
// may wait (may_defer); false when the upload can go ahead (it waits for a decode under way itself).
bool astc_pending(uint64_t texture, bool may_defer);
uint64_t astc_deferred_take();  // draws skipped for a texture being decoded, since the last call
void build_sets(VkCommandBuffer cb, const std::vector<const ShaderInfo*>& shaders, const ProgramLayout& layout);
// vkUpdateDescriptorSets for what build_sets made, then the sets bound at `point`.
void bind_sets(VkCommandBuffer cb, VkPipelineBindPoint point, const ProgramLayout& layout);
Upload zero_stream();  // 64 zero bytes for vertex binding 15
// draw_inputs, with 16- and 32-bit indices through dedup_upload.
bool draw_inputs_shared(VkCommandBuffer cb, const ShaderInfo* vs, bool indexed, const Cmd& c, DrawInputs& in);
std::vector<WriteBack>& separate_write_backs();  // NX_COMPUTE_SEPARATE=1: a write-back per binding
void sets_begin_submit();
void sets_log();
// nvn_draw.cpp: `img` is a target of the open pass that a draw or clear of the pass wrote.
bool pass_wrote(const GpuImage* img);
void written_cpu_write(const uint8_t* p, uint64_t size);       // replay-time guest writes (logged when they meet one)
std::vector<WriteBack> written_take();                         // one write-back per range, at the end of the submit
void written_reset();
void written_log();
// c[0], the NVN driver constants of a stage (nvn_dedup.cpp).
std::array<uint8_t, 0x1000> driver_constants(ShaderStage stage, int st);

bool debug_shader_overrides(const std::vector<const ShaderInfo*>*& shaders, const ShaderInfo*& fs,
                            std::vector<const ShaderInfo*>& swapped);
// nvn_raster.cpp: the stencil, polygon offset, depth bounds and depth clamp commands into g_state (false for other
// commands), and their dynamic state for the next draw (stencil reference and masks, depth bias).
bool raster_command(const Cmd& c);
void raster_dynamic(VkCommandBuffer cb, bool slack);  // slack: equal_slack_pass (nvn.h)

// NX_PROBE_FILL=1 (a test): a light probe drawn with all-zero sky and ground colours gets grey ones instead.
void probe_fill(const Targets& t, const ShaderInfo* fs);

void trace_compute_once(const ShaderInfo* cs, uint32_t x, uint32_t y, uint32_t z);

// NX_TRACE_SKIN: skinned draws' inputs, vertex uniform blocks and the bone rows they select.
void trace_skin(bool indexed, const Cmd& c, const ShaderInfo* vs, const ShaderInfo* fs);

// A draw (or a clear inside the pass) of a pass recorded by the workers (nvn_parallel.cpp): what the walk resolved
// for it (its bound state, its image descriptors, the pipeline, viewport and scissor) and what a worker then does with
// it (draw_job, nvn_draw.cpp): its buffer descriptors and inputs copied, binds and the draw call.
struct DrawJob {
    bool clear = false;  // vkCmdClearAttachments with `attachment` and `rect` instead of a draw
    VkClearAttachment attachment{};
    VkClearRect rect{};
    State state;
    Descriptors d;
    const std::vector<const ShaderInfo*>* shaders = nullptr;
    const ProgramLayout* layout = nullptr;
    const ShaderInfo* vs = nullptr;
    VkPipeline pipeline = VK_NULL_HANDLE;
    Cmd cmd;
    bool indexed = false;
    VkViewport vp{};
    VkRect2D sc{};
};
void draw_job(DrawJob& j, VkCommandBuffer cb);  // nvn_draw.cpp, on a worker
void draw_segment_begin(VkCommandBuffer cb);      // nvn_draw.cpp, on a worker: `cb` begun, nothing bound in it

// nvn_parallel.cpp. The open parallel pass's attachment formats (its secondary command buffers inherit them).
struct PassFormats {
    uint32_t color_count = 0;
    VkFormat colors[8] = {};
    VkFormat depth = VK_FORMAT_UNDEFINED, stencil = VK_FORMAT_UNDEFINED;
};
bool parallel_on();  // NX_PARALLEL=1, for this submit, with the workers started (on the first call)
DrawJob& parallel_job();  // the next job of the open parallel pass, to fill and then push (or not: the next call)
void parallel_pass_begin(const PassFormats& f);
void parallel_push(DrawJob& j);
// What was pushed is recorded: before the walk writes guest memory the workers may still read (UpdateUniformBuffer,
// ReportCounter).
void parallel_flush();
void parallel_pass_end(VkCommandBuffer cb);  // the pass's secondary command buffers executed into `cb`, in order
void parallel_log();

}  // namespace nx::nvn::replay

namespace nx::nvn {
bool pass_parallel();  // nvn_draw.cpp: the open pass is recorded by the workers
// A clear inside the open pass: into the workers' stream when it is a parallel one (nvn_clears.cpp).
void pass_clear_attachment(VkCommandBuffer cb, const VkClearAttachment& a, const VkClearRect& r);
// The open pass becomes one recorded here (a masked clear's fullscreen draw cannot go to the workers).
void pass_inline(VkCommandBuffer cb);
}  // namespace nx::nvn

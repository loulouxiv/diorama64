#include "vk_device.h"
#include <shaderc/shaderc.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>
#include "combiner.h"
#include "glsl_emitter.h"
#include "pipeline_warm.h"

namespace ar {
namespace {
// The MSL structs of render/metal (msl_emitter.cpp) and the GLSL blocks of glsl_emitter.cpp share these layouts.
struct DrawU { float prim[4]; float env[4]; float lod; float decal; float noise_seed; float fade; float clip[4]; float clip_on; uint32_t lflags;
               float pull; float talpha; };
static_assert(sizeof(DrawU) == 80, "DrawU matches the GLSL block");
struct LightU {
    float dir_on[4]; float tint_amb[4]; float contact[4]; float under_uv[16]; float under_range[4];
    float shadow[4]; float shadow_uv[16]; float cast[4];
    float relight[4]; float ao[4]; float ao_min[4]; float ao_inv[4]; float toy[4];
    float env0[4], env1[4], env2[4]; float env3[4], env4[4], env5[4];
    float liquid[4]; float liquid2[4]; float water_box[8][4]; float water_y[2][4]; float water_foot[4];
    float cut[4]; float cut2[4]; float trans[4]; float trans_rgb[4]; float trans_c[4]; float tex[4];
};
static_assert(sizeof(LightU) == 704, "LightU matches the GLSL block");
struct ViewU { float clip_from_model[16]; float eye_pull[4]; float eye_view[4]; uint32_t layer, pad[3]; float wclip_lo[4]; float wclip_hi[4];
               float interp[4]; };
static_assert(sizeof(ViewU) == 160, "ViewU matches the GLSL block");
struct DepthPC { float m[16]; float interp[4]; float wclip_lo[4]; float wclip_hi[4]; };
static_assert(sizeof(DepthPC) == 112, "DepthPC fits the guaranteed 128 push-constant bytes");

const uint32_t kListFormat = 70;           // MTLPixelFormatRGBA8Unorm: pipeline lists stay interchangeable with Metal's
const size_t kMaxWarmLines = 4096, kMaxWarmLineBytes = 96;
const uint32_t kUndersideSize = 256, kFootprintSize = 512, kShadowSize = 2048;
const int kSlots = 2;                      // frames in flight

enum StencilMode : uint8_t { kStencilKeep = 0, kStencilMarkFocus = 1, kStencilClearFocus = 2, kStencilSilhouette = 3, kStencilFaint = 4 };
const uint32_t kStencilDepthWritten = 0x80, kStencilFocusBits = 0x7F;
uint8_t stencil_mode(const DrawPacket &d) {
    if (d.silhouette) return kStencilSilhouette;
    if (d.depth_only == kDepthOnlyFaint || d.table_shadow) return kStencilFaint;
    if (!d.depth_write) return kStencilKeep;
    return d.focus ? kStencilMarkFocus : kStencilClearFocus;
}
uint32_t stencil_ref(uint8_t mode) {
    switch (mode) {
    case kStencilMarkFocus: return kStencilDepthWritten | 1u;
    case kStencilClearFocus: return kStencilDepthWritten;
    default: return 0u;
    }
}
// The depth and stencil state of a draw (Metal's depth_state): compare, write, stencil test and ops, masks.
struct DepthState {
    bool test = false, write = false, stencil = false;
    VkCompareOp compare = VK_COMPARE_OP_ALWAYS;
    VkStencilOpState st{};
};
DepthState depth_state(const DrawPacket &d, bool pass_depth) {
    DepthState s;
    if (!pass_depth) return s;   // Metal's no_depth_state: always, no write, no stencil
    const uint8_t mode = stencil_mode(d);
    const bool test = d.silhouette ? true : d.depth_test != 0, write = d.silhouette ? false : d.depth_write != 0;
    s.test = true;
    s.compare = d.silhouette ? VK_COMPARE_OP_LESS : (test ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_ALWAYS);
    s.write = write;
    if (mode != kStencilKeep) {
        s.stencil = true;
        const bool tests = mode == kStencilSilhouette || mode == kStencilFaint;
        s.st.compareOp = tests ? VK_COMPARE_OP_EQUAL : VK_COMPARE_OP_ALWAYS;
        s.st.failOp = VK_STENCIL_OP_KEEP;
        s.st.depthFailOp = VK_STENCIL_OP_KEEP;
        s.st.passOp = mode == kStencilSilhouette ? VK_STENCIL_OP_INCREMENT_AND_CLAMP : mode == kStencilFaint ? VK_STENCIL_OP_KEEP : VK_STENCIL_OP_REPLACE;
        s.st.compareMask = mode == kStencilSilhouette ? kStencilFocusBits : mode == kStencilFaint ? kStencilDepthWritten : 0xFF;
        s.st.writeMask = mode == kStencilFaint ? 0 : 0xFF;
        s.st.reference = stencil_ref(mode);
    }
    return s;
}
VkSamplerAddressMode address(uint8_t w) {
    return w == FC_WRAP_CLAMP ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : w == FC_WRAP_MIRROR ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT
                                                                     : VK_SAMPLER_ADDRESS_MODE_REPEAT;
}
// A silhouette copy's fragment is its PRIM colour whatever its combiner: one pipeline per clip state (Metal's pipeline_form).
DrawPacket pipeline_form(const DrawPacket &d) {
    if (!d.silhouette) return d;
    static const fc_combiner_key flat = normalize_combiner(fc_combiner_key{((uint64_t)FC_SRC_SHADE << 12) | ((uint64_t)FC_SRC_SHADE << 28), 0, 0});
    DrawPacket c = d;
    c.cc = flat;
    return c;
}
// A draw pipeline: Metal's PipeKey, plus (without VK_EXT_extended_dynamic_state) the depth/stencil state and the cull mode
// that Vulkan 1.1 bakes into a pipeline.
struct VkPipeKey {
    uint64_t slots; uint32_t opts; uint8_t blend, flat, depth_only, table_shadow, clip, samples, a2c, cut;
    uint8_t baked = 0;           // 1: the fields below are part of the pipeline (no extended dynamic state)
    uint8_t cull = 0, dtest = 0, dwrite = 0, dcompare = 0, stencil = 0, smode = 0;
    auto tie() const { return std::tie(slots, opts, blend, flat, depth_only, table_shadow, clip, samples, a2c, cut, baked, cull, dtest, dwrite,
                                       dcompare, stencil, smode); }
    bool operator<(const VkPipeKey &o) const { return tie() < o.tie(); }
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;         // all of it (colour; or depth only, for a depth image sampled)
    VkImageView att = VK_NULL_HANDLE;          // a depth/stencil image's attachment view (depth and stencil); else view
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = 0;
    uint32_t w = 0, h = 0, depth = 1, layers = 1, levels = 1;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};
using ImagePtr = std::shared_ptr<Image>;

struct Rec {
    PassDesc pass{};
    ViewConstants view{};
    LightU light{};
    VkBuffer vb = VK_NULL_HANDLE, nb = VK_NULL_HANDLE, pb = VK_NULL_HANDLE;
    VkDeviceSize voff = 0, noff = 0, poff = 0, nsize = 0, psize = 0, vsize = 0;
    uint32_t nv = 0;
    ImagePtr shadow, under, ao, env, env_prev, foot;
    std::vector<DrawPacket> draws;
    std::vector<ImagePtr> tex;                  // 2 per draw, as they were at record time
};
struct Target {
    ImagePtr color, depth, ms_color, ms_depth;
    bool memoryless = false;
    uint32_t w = 0, h = 0, layers = 1, samples = 1;
    std::vector<Rec> recs;
    size_t nrec = 0;
};
struct ShaderCache {
    std::mutex m;
    std::map<std::pair<int, std::string>, VkShaderModule> mods;
};
}

class VulkanDevice final : public IRenderDevice {
public:
    VulkanDevice() = default;
    ~VulkanDevice() override;
    bool init_headless(const VulkanDeviceOptions &opt, std::string *err);
    bool init_external(const VulkanExternalDevice &ext, const VulkanDeviceOptions &opt, std::string *err);
    VulkanDeviceInfo info() const;
    bool save_cache();

    // IRenderDevice
    void upload_texture(uint32_t id, uint32_t w, uint32_t h, const uint8_t *rgba) override;
    void set_edge_coverage(bool on) override { edge_ = on; }
    void set_texture_filter(TextureFilter f) override { if (f != filter_) { filter_ = f; } }
    void set_fine_textures(bool on) override { fine_ = on; }
    uint32_t anisotropy() const override { return filter_ == TextureFilter::smooth && aniso_ ? (fine_ ? 16u : 4u) : 0u; }
    MipCounts mip_counts() const override { return MipCounts{0, mips_blit_, 0}; }
    bool read_texture_level(uint32_t tex_id, uint32_t level, std::vector<uint8_t> &rgba, uint32_t &w, uint32_t &h) override;
    uint32_t create_target(uint32_t w, uint32_t h, uint32_t layers, uint32_t samples) override;
    void resize_target(uint32_t target, uint32_t w, uint32_t h, uint32_t layers, uint32_t samples) override;
    uint32_t supported_samples(uint32_t wanted, uint32_t layers) const override;
    uint32_t target_samples(uint32_t t) const override { return t == 0 || t > targets_.size() ? 1u : targets_[t - 1].samples; }
    void bind_target_texture(uint32_t tex_id, uint32_t target) override {
        if (!tex_id) return;
        if (target == 0 || target > targets_.size()) target_tex_.erase(tex_id); else target_tex_[tex_id] = target;
    }
    void release_target(uint32_t t) override;
    bool memoryless_msaa() const override { return lazily_; }
    void set_rate_map(uint32_t, void *) override {}
    void begin_frame() override;
    void encode_pass(uint32_t target, const PassDesc &pass, const ViewConstants &view, const fc_vertex *verts, uint32_t nv,
                     const DrawPacket *draws, uint32_t nd, const int8_t *normals, const float *prev_pos) override;
    void set_lighting(const LightingFrame *lf) override;
    void encode_underside_map(const UndersideDesc &u, const fc_vertex *verts, uint32_t nv, const DrawPacket *draws, uint32_t nd,
                              const DepthExtras &x) override;
    void encode_water_footprint(const float window[4], const float clip_from_level[16], const fc_vertex *verts, uint32_t nv,
                                const DrawPacket *draws, uint32_t nd) override;
    bool reuse_water_footprint() override { if (!recording_ || !foot_ || !foot_kept_) return false; foot_valid_ = true; return true; }
    void encode_shadow_map(const ShadowFit &fit, const fc_vertex *verts, uint32_t nv, const DrawPacket *draws, uint32_t nd,
                           const DepthExtras &x) override;
    bool bake_ao(uint32_t, const AoVolume &, const std::vector<ShadowFit> &, float, const fc_vertex *, uint32_t, const DrawPacket *,
                 uint32_t, const DepthExtras &) override {
        if (!ao_warned_) { std::fprintf(stderr, "AR_AO bake failed: not on the Vulkan backend yet\n"); ao_warned_ = true; }
        return false;
    }
    void encode_environment_reduce(void *, uint32_t) override { env_error_ = "no_probe"; }
    bool environment_stats(EnvStats *, uint32_t *) const override { return false; }
    const char *environment_error() const override { return env_error_.empty() ? nullptr : env_error_.c_str(); }
    void encode_composite(uint32_t dst, void *external_texture, uint32_t under, uint32_t over) override;
    void end_frame(void (*on_gpu_done)(void *), void *ctx, void *present_drawable) override;
    void wait_idle() override;
    double last_gpu_ms() const override { return -1.0; }
    uint32_t render_encoders_last_frame() const override { return passes_last_; }
    uint64_t late_passes() const override { return late_; }
    bool read_target(uint32_t target, std::vector<uint8_t> &rgba, uint32_t &w, uint32_t &h) override;
    bool compile_combiner(const fc_combiner_key &key, std::string *error, uint32_t samples) override;
    void encode_resolve(uint32_t src_target, const ExternalView &dst) override;
    void *command_buffer() override { return recording_ ? (void *)slot().cmd : nullptr; }
    bool read_target_depth(uint32_t target, std::vector<float> &depth, uint32_t &w, uint32_t &h) override;
    std::string pipeline_list() const override;
    size_t warm_pipelines(const std::string &list, uint32_t at_samples) override;
    size_t warm_pipelines_background(const std::string &list, const std::vector<DrawPacket> &draws, uint32_t samples) override;
    size_t pipelines_warming() override;
    bool wait_pipelines_warmed(double timeout_s) override;
    size_t draw_time_builds() const override { return draw_builds_; }
    void set_async_pipelines(bool on) override { async_ = on; }
    bool async_pipelines() const override { return async_; }
    uint64_t deferred_draws() const override { return deferred_; }
    bool take_deferred_miss(std::string *key) override {
        if (miss_log_.empty()) return false;
        if (key) *key = miss_log_.front();
        miss_log_.erase(miss_log_.begin());
        return true;
    }
    bool take_pipeline_error(std::string *why) override {
        std::lock_guard<std::mutex> g(pm_);
        if (refused_.empty()) return false;
        if (why) *why = refused_.front();
        refused_.erase(refused_.begin());
        return true;
    }
    size_t pipeline_count() const override { std::lock_guard<std::mutex> g(pm_); return pipes_.size(); }

    uint64_t validation_errors_ = 0, validation_warnings_ = 0;

private:
    // --- frames
    struct Slot {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE, up = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        bool submitted = false, up_open = false;
        std::vector<VkDescriptorPool> dpools;
        size_t dpool_next = 0;
        VkBuffer ring = VK_NULL_HANDLE; VkDeviceMemory ring_mem = VK_NULL_HANDLE; uint8_t *ring_ptr = nullptr;
        VkDeviceSize ring_size = 0, ring_used = 0;
        std::vector<std::function<void()>> garbage;
        std::vector<std::pair<void (*)(void *), void *>> done;
    };
    Slot &slot() { return slots_[slot_index_]; }
    void acquire_slot();            // the current slot is free for recording (its last submission completed)
    void retire(Slot &s);           // its submission completed: run the done callbacks, free its garbage
    VkCommandBuffer upload_cmd();   // the slot's upload command buffer, begun
    void submit_uploads_alone();

    // --- memory and images
    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid = 0) const;
    bool make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, VkBuffer *b, VkDeviceMemory *m);
    ImagePtr make_image(VkImageType type, VkFormat fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t layers, uint32_t levels,
                        VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageAspectFlags aspect, bool cube, bool transient);
    void defer_destroy(Image *img);   // the shared_ptr deleter: freed once the GPU is past every use
    void transition(VkCommandBuffer cb, Image &img, VkImageLayout to, bool always = false);
    VkDeviceSize stage(const void *data, size_t n, VkBuffer *buf);   // into the slot's ring; returns the offset

    // --- descriptors, samplers, shaders, pipelines
    VkDescriptorSet alloc_set(VkDescriptorSetLayout layout);
    VkSampler sampler(const fc_sampler &s, bool depth_pass = false);
    VkShaderModule shader(int stage, const std::string &src, std::string *why) const;
    VkPipeKey pipe_key(const DrawPacket &d, uint32_t samples, bool pass_depth) const;
    VkPipeline pipeline(const DrawPacket &d, uint32_t samples, bool pass_depth, std::string *err, bool at_draw);
    VkPipeline build_pipeline(const VkPipeKey &k, const DrawPacket &d, std::string *why, bool *refused) const;
    VkRenderPass target_pass(uint32_t samples, bool clear);
    VkRenderPass depth_pass();
    VkRenderPass color_pass(VkFormat fmt, bool load, VkFormat depth_fmt = VK_FORMAT_UNDEFINED);
    bool depth_pipelines();
    VkPipeline composite_pipeline(VkFormat fmt);
    VkPipeline resolve_pipeline(VkFormat cf, VkFormat df);
    void worker();
    void enqueue(const VkPipeKey &k, const DrawPacket &d);

    // --- passes
    void make_target(Target &t, uint32_t w, uint32_t h, uint32_t layers, uint32_t samples);
    void flush(Target &t);
    void encode_rec(VkCommandBuffer cb, Target &t, Rec &r);
    void flush_sampling(const ImagePtr &img);
    void encode_depth_map(Image &map, const float m[16], const fc_vertex *verts, uint32_t nv, const DrawPacket *draws, uint32_t nd,
                          float slope_bias, bool clamp_depth, const DepthExtras &x);
    ImagePtr texture(uint32_t tex_id);
    ImagePtr make_depth_map(uint32_t size);
    bool read_image(Image &img, VkImageAspectFlags aspect, std::vector<uint8_t> &out, uint32_t level = 0);
    VkFramebuffer framebuffer(VkRenderPass rp, const std::vector<VkImageView> &views, uint32_t w, uint32_t h);
    bool create_common(std::string *err);
    bool dying_ = false;

    // instance and device
    bool owns_ = false;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDevice dev_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t qfam_ = 0;
    uint32_t api_ = VK_API_VERSION_1_1;
    VkPhysicalDeviceProperties props_{};
    VkPhysicalDeviceMemoryProperties memprops_{};
    std::string driver_;
    bool eds_ = false, aniso_ = false, depth_clamp_ = false, a2one_ = false, lazily_ = false;
    bool depth_resolve_max_ = false, independent_none_ = false;
    VkSampleCountFlags sample_counts_ = VK_SAMPLE_COUNT_1_BIT;
    VkFormat ds_format_ = VK_FORMAT_UNDEFINED;
    PFN_vkCreateRenderPass2KHR create_rp2_ = nullptr;
    PFN_vkCmdSetCullModeEXT set_cull_ = nullptr;
    PFN_vkCmdSetFrontFaceEXT set_front_ = nullptr;
    PFN_vkCmdSetDepthTestEnableEXT set_dtest_ = nullptr;
    PFN_vkCmdSetDepthWriteEnableEXT set_dwrite_ = nullptr;
    PFN_vkCmdSetDepthCompareOpEXT set_dcompare_ = nullptr;
    PFN_vkCmdSetStencilTestEnableEXT set_stest_ = nullptr;
    PFN_vkCmdSetStencilOpEXT set_sop_ = nullptr;
    VkDeviceSize align_ = 256;
    std::string cache_path_;
    VkPipelineCache pcache_ = VK_NULL_HANDLE;

    Slot slots_[kSlots];
    int slot_index_ = 0;
    bool slot_acquired_ = false, recording_ = false;
    VkDescriptorSetLayout pass_layout_ = VK_NULL_HANDLE, draw_layout_ = VK_NULL_HANDLE, depth_layout_ = VK_NULL_HANDLE,
                          pair_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout main_pl_ = VK_NULL_HANDLE, depth_pl_ = VK_NULL_HANDLE, pair_pl_ = VK_NULL_HANDLE;
    mutable ShaderCache shaders_;
    VkSampler linear_clamp_ = VK_NULL_HANDLE, compare_ = VK_NULL_HANDLE, nearest_ = VK_NULL_HANDLE;
    std::map<uint32_t, VkSampler> samplers_;
    ImagePtr white_, under_ph_, shadow_ph_, ao_ph_, env_ph_;
    std::unordered_map<uint32_t, ImagePtr> textures_;
    std::map<uint32_t, uint32_t> target_tex_;
    std::vector<Target> targets_;
    std::map<std::pair<uint32_t, bool>, VkRenderPass> target_passes_;
    VkRenderPass depth_rp_ = VK_NULL_HANDLE;
    std::map<std::tuple<VkFormat, bool, VkFormat>, VkRenderPass> color_passes_;
    std::map<std::tuple<VkRenderPass, std::vector<VkImageView>, uint32_t, uint32_t>, VkFramebuffer> fbs_;
    bool depth_tried_ = false;
    VkPipeline depth_pipe_[2][2] = {};   // [cut][clamp]
    std::map<VkFormat, VkPipeline> composite_;
    std::map<std::pair<VkFormat, VkFormat>, VkPipeline> resolve_;

    // pipelines (the render thread and the warm-up workers)
    enum { kNone = 0, kQueued = 1, kBuilding = 2, kDone = 3 };
    struct PipeEntry { int state = kNone; VkPipeline p = VK_NULL_HANDLE; std::string why; DrawPacket d{}; };
    mutable std::mutex pm_;
    std::condition_variable pcv_;
    std::map<VkPipeKey, PipeEntry> pipes_;
    std::deque<VkPipeKey> queue_jobs_;
    std::vector<std::thread> workers_;
    bool stop_ = false;
    std::vector<std::string> refused_;
    mutable std::atomic<uint64_t> built_{0}, modules_{0};
    mutable std::atomic<uint64_t> compile_us_{0};
    size_t draw_builds_ = 0;
    bool async_ = false;
    uint64_t deferred_ = 0;
    std::set<VkPipeKey> missed_;
    std::vector<std::string> miss_log_;

    // frame state
    LightU light_{};
    ImagePtr shadow_, under_, foot_;
    bool shadow_valid_ = false, under_valid_ = false, foot_valid_ = false, foot_kept_ = false;
    ShadowFit shadow_fit_{};
    UndersideDesc under_desc_{};
    float foot_window_[4] = {0.f, 0.f, 1.f, 1.f};
    bool edge_ = false, fine_ = false, ao_warned_ = false;
    TextureFilter filter_ = TextureFilter::smooth;
    std::string env_error_;
    uint32_t passes_ = 0, passes_last_ = 0;
    uint64_t draws_ = 0, draws_last_ = 0, late_ = 0, mips_blit_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------- setup
namespace {
VKAPI_ATTR VkBool32 VKAPI_CALL on_debug(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT,
                                       const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    auto *d = static_cast<VulkanDevice *>(user);
    if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) d->validation_errors_++;
    else if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) d->validation_warnings_++;
    if (sev & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT))
        std::fprintf(stderr, "AR_VK_VALIDATION %s: %s\n", (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "error" : "warning",
                     data && data->pMessage ? data->pMessage : "?");
    return VK_FALSE;
}
bool has_ext(const std::vector<VkExtensionProperties> &v, const char *n) {
    for (const auto &e : v) if (!std::strcmp(e.extensionName, n)) return true;
    return false;
}
}

bool VulkanDevice::init_headless(const VulkanDeviceOptions &opt, std::string *err) {
    owns_ = true;
    cache_path_ = opt.pipeline_cache_path;
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> iexts(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, iexts.data());
    std::vector<const char *> layers, exts;
    bool debug = false;
    if (opt.validation) {
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> lp(n);
        vkEnumerateInstanceLayerProperties(&n, lp.data());
        bool have = false;
        for (const auto &l : lp) if (!std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation")) have = true;
        if (have) layers.push_back("VK_LAYER_KHRONOS_validation");
        else std::fprintf(stderr, "AR_VK validation layer not installed (apt install vulkan-validationlayers)\n");
        if (have && has_ext(iexts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) { exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME); debug = true; }
    }
    uint32_t loader_api = VK_API_VERSION_1_0;
    auto enum_ver = (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion");
    if (enum_ver) enum_ver(&loader_api);
    if (loader_api < VK_API_VERSION_1_1) { *err = "the Vulkan loader is older than 1.1"; return false; }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Diorama64"; app.pEngineName = "diorama64"; app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = (uint32_t)layers.size(); ici.ppEnabledLayerNames = layers.data();
    ici.enabledExtensionCount = (uint32_t)exts.size(); ici.ppEnabledExtensionNames = exts.data();
    if (vkCreateInstance(&ici, nullptr, &instance_) != VK_SUCCESS) { *err = "vkCreateInstance failed"; return false; }
    if (debug) {
        auto mk = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mi.pfnUserCallback = on_debug; mi.pUserData = this;
        if (mk) mk(instance_, &mi, nullptr, &messenger_);
    }
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(instance_, &n, pds.data());
    if (pds.empty()) { *err = "no Vulkan device"; return false; }
    phys_ = pds[0];
    for (VkPhysicalDevice p : pds) {
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(p, &pp);
        if (!opt.prefer_device.empty() && std::strstr(pp.deviceName, opt.prefer_device.c_str())) { phys_ = p; break; }
    }
    vkGetPhysicalDeviceProperties(phys_, &props_);
    api_ = std::min(props_.apiVersion, (uint32_t)VK_API_VERSION_1_2);
    if (api_ < VK_API_VERSION_1_1) { *err = "the Vulkan device is older than 1.1"; return false; }
    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &n, nullptr);
    std::vector<VkQueueFamilyProperties> qf(n);
    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &n, qf.data());
    qfam_ = UINT32_MAX;
    for (uint32_t i = 0; i < n; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfam_ = i; break; }
    if (qfam_ == UINT32_MAX) { *err = "no graphics queue"; return false; }
    vkEnumerateDeviceExtensionProperties(phys_, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> dexts(n);
    vkEnumerateDeviceExtensionProperties(phys_, nullptr, &n, dexts.data());
    std::vector<const char *> want;
    const bool core12 = api_ >= VK_API_VERSION_1_2;
    if (!core12) {
        for (const char *e : {VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME,
                              VK_KHR_MULTIVIEW_EXTENSION_NAME, VK_KHR_MAINTENANCE2_EXTENSION_NAME})
            if (has_ext(dexts, e)) want.push_back(e);
    }
    const bool eds = has_ext(dexts, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
    if (eds) want.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
    VkPhysicalDeviceFeatures have{};
    vkGetPhysicalDeviceFeatures(phys_, &have);
    VkPhysicalDeviceFeatures f{};
    f.shaderClipDistance = have.shaderClipDistance;
    f.samplerAnisotropy = have.samplerAnisotropy;
    f.depthClamp = have.depthClamp;
    f.alphaToOne = have.alphaToOne;
    f.depthBiasClamp = have.depthBiasClamp;
    f.imageCubeArray = have.imageCubeArray;
    if (!have.shaderClipDistance) { *err = "the device has no shaderClipDistance"; return false; }
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT edsf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
    edsf.extendedDynamicState = eds ? VK_TRUE : VK_FALSE;
    const float prio = 1.f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = qfam_; qi.queueCount = 1; qi.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = eds ? &edsf : nullptr;
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qi;
    dci.enabledExtensionCount = (uint32_t)want.size(); dci.ppEnabledExtensionNames = want.data();
    dci.pEnabledFeatures = &f;
    if (vkCreateDevice(phys_, &dci, nullptr, &dev_) != VK_SUCCESS) { *err = "vkCreateDevice failed"; return false; }
    vkGetDeviceQueue(dev_, qfam_, 0, &queue_);
    eds_ = eds;
    aniso_ = f.samplerAnisotropy; depth_clamp_ = f.depthClamp; a2one_ = f.alphaToOne;
    return create_common(err);
}

bool VulkanDevice::init_external(const VulkanExternalDevice &ext, const VulkanDeviceOptions &opt, std::string *err) {
    owns_ = false;
    cache_path_ = opt.pipeline_cache_path;
    instance_ = ext.instance; phys_ = ext.physical; dev_ = ext.device; qfam_ = ext.queue_family; queue_ = ext.queue;
    api_ = ext.api_version;
    vkGetPhysicalDeviceProperties(phys_, &props_);
    VkPhysicalDeviceFeatures have{};
    vkGetPhysicalDeviceFeatures(phys_, &have);
    aniso_ = have.samplerAnisotropy; depth_clamp_ = have.depthClamp; a2one_ = have.alphaToOne;   // the caller enabled these
    eds_ = ext.extended_dynamic_state;
    return create_common(err);
}

bool VulkanDevice::create_common(std::string *err) {
    vkGetPhysicalDeviceMemoryProperties(phys_, &memprops_);
    for (uint32_t i = 0; i < memprops_.memoryTypeCount; i++)
        if (memprops_.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) lazily_ = true;
    VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceDepthStencilResolveProperties dsr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES};
    dsr.pNext = &drv;
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &dsr;
    vkGetPhysicalDeviceProperties2(phys_, &p2);
    driver_ = drv.driverName;
    depth_resolve_max_ = (dsr.supportedDepthResolveModes & VK_RESOLVE_MODE_MAX_BIT) != 0;
    independent_none_ = dsr.independentResolveNone;
    const bool resolve_ok = dsr.supportedDepthResolveModes != 0;
    create_rp2_ = (PFN_vkCreateRenderPass2KHR)vkGetDeviceProcAddr(dev_, api_ >= VK_API_VERSION_1_2 ? "vkCreateRenderPass2" : "vkCreateRenderPass2KHR");
    if (!create_rp2_) { *err = "the device has no vkCreateRenderPass2 (Vulkan 1.2 or VK_KHR_create_renderpass2)"; return false; }
    if (eds_) {
        set_cull_ = (PFN_vkCmdSetCullModeEXT)vkGetDeviceProcAddr(dev_, "vkCmdSetCullModeEXT");
        set_front_ = (PFN_vkCmdSetFrontFaceEXT)vkGetDeviceProcAddr(dev_, "vkCmdSetFrontFaceEXT");
        set_dtest_ = (PFN_vkCmdSetDepthTestEnableEXT)vkGetDeviceProcAddr(dev_, "vkCmdSetDepthTestEnableEXT");
        set_dwrite_ = (PFN_vkCmdSetDepthWriteEnableEXT)vkGetDeviceProcAddr(dev_, "vkCmdSetDepthWriteEnableEXT");
        set_dcompare_ = (PFN_vkCmdSetDepthCompareOpEXT)vkGetDeviceProcAddr(dev_, "vkCmdSetDepthCompareOpEXT");
        set_stest_ = (PFN_vkCmdSetStencilTestEnableEXT)vkGetDeviceProcAddr(dev_, "vkCmdSetStencilTestEnableEXT");
        set_sop_ = (PFN_vkCmdSetStencilOpEXT)vkGetDeviceProcAddr(dev_, "vkCmdSetStencilOpEXT");
        eds_ = set_cull_ && set_front_ && set_dtest_ && set_dwrite_ && set_dcompare_ && set_stest_ && set_sop_;
    }
    for (VkFormat fmt : {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT}) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(phys_, fmt, &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) { ds_format_ = fmt; break; }
    }
    if (ds_format_ == VK_FORMAT_UNDEFINED) { *err = "no depth/stencil format"; return false; }
    sample_counts_ = props_.limits.framebufferColorSampleCounts & props_.limits.framebufferDepthSampleCounts &
                     props_.limits.framebufferStencilSampleCounts;
    if (!resolve_ok) sample_counts_ = VK_SAMPLE_COUNT_1_BIT;   // MSAA needs a depth resolve (the compositor's depth)
    align_ = std::max<VkDeviceSize>({256, props_.limits.minUniformBufferOffsetAlignment, props_.limits.minStorageBufferOffsetAlignment});

    // pipeline cache
    std::vector<char> blob;
    if (!cache_path_.empty()) {
        std::ifstream in(cache_path_, std::ios::binary);
        if (in) blob.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    pci.initialDataSize = blob.size(); pci.pInitialData = blob.empty() ? nullptr : blob.data();
    if (vkCreatePipelineCache(dev_, &pci, nullptr, &pcache_) != VK_SUCCESS) {
        pci.initialDataSize = 0; pci.pInitialData = nullptr;   // a stale or foreign blob: start empty
        vkCreatePipelineCache(dev_, &pci, nullptr, &pcache_);
    }

    // descriptor set layouts
    auto layout = [&](std::vector<VkDescriptorSetLayoutBinding> b) {
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = (uint32_t)b.size(); li.pBindings = b.data();
        VkDescriptorSetLayout l = VK_NULL_HANDLE;
        vkCreateDescriptorSetLayout(dev_, &li, nullptr, &l);
        return l;
    };
    const VkShaderStageFlags VF = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, FS = VK_SHADER_STAGE_FRAGMENT_BIT,
                             VS = VK_SHADER_STAGE_VERTEX_BIT;
    const VkDescriptorType UB = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           SB = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pass_layout_ = layout({{0, UB, 1, VF, nullptr}, {1, UB, 1, VF, nullptr}, {2, CIS, 1, FS, nullptr}, {3, CIS, 1, FS, nullptr},
                           {4, CIS, 1, FS, nullptr}, {5, CIS, 1, FS, nullptr}, {6, CIS, 1, FS, nullptr}, {7, CIS, 1, FS, nullptr},
                           {8, SB, 1, VS, nullptr}, {9, SB, 1, VS, nullptr}});
    draw_layout_ = layout({{0, UB, 1, VF, nullptr}, {1, CIS, 1, FS, nullptr}, {2, CIS, 1, FS, nullptr}});
    depth_layout_ = layout({{0, SB, 1, VS, nullptr}, {1, CIS, 1, FS, nullptr}});
    pair_layout_ = layout({{0, CIS, 1, FS, nullptr}, {1, CIS, 1, FS, nullptr}});
    auto playout = [&](std::vector<VkDescriptorSetLayout> sets, uint32_t push) {
        VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, push};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = (uint32_t)sets.size(); li.pSetLayouts = sets.data();
        li.pushConstantRangeCount = push ? 1 : 0; li.pPushConstantRanges = &pr;
        VkPipelineLayout l = VK_NULL_HANDLE;
        vkCreatePipelineLayout(dev_, &li, nullptr, &l);
        return l;
    };
    main_pl_ = playout({pass_layout_, draw_layout_}, 0);
    depth_pl_ = playout({depth_layout_}, sizeof(DepthPC));
    pair_pl_ = playout({pair_layout_}, 32);

    // frame slots
    for (Slot &s : slots_) {
        VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cp.queueFamilyIndex = qfam_;
        vkCreateCommandPool(dev_, &cp, nullptr, &s.pool);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = s.pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(dev_, &ai, &s.cmd);
        vkAllocateCommandBuffers(dev_, &ai, &s.up);
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(dev_, &fi, nullptr, &s.fence);
    }

    // samplers
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.f;
    vkCreateSampler(dev_, &si, nullptr, &linear_clamp_);
    si.compareEnable = VK_TRUE; si.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    vkCreateSampler(dev_, &si, nullptr, &compare_);
    si.compareEnable = VK_FALSE; si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    vkCreateSampler(dev_, &si, nullptr, &nearest_);

    // placeholders: white 1x1, depth 1x1 at 1 ("nothing"), AO 1, a black probe cube
    const VkImageUsageFlags SAMP = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    white_ = make_image(VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, 1, VK_SAMPLE_COUNT_1_BIT, SAMP, VK_IMAGE_ASPECT_COLOR_BIT, false, false);
    under_ph_ = make_image(VK_IMAGE_TYPE_2D, VK_FORMAT_D32_SFLOAT, 1, 1, 1, 1, 1, VK_SAMPLE_COUNT_1_BIT, SAMP, VK_IMAGE_ASPECT_DEPTH_BIT, false, false);
    shadow_ph_ = make_image(VK_IMAGE_TYPE_2D, VK_FORMAT_D32_SFLOAT, 1, 1, 1, 1, 1, VK_SAMPLE_COUNT_1_BIT, SAMP, VK_IMAGE_ASPECT_DEPTH_BIT, false, false);
    ao_ph_ = make_image(VK_IMAGE_TYPE_3D, VK_FORMAT_R8_UNORM, 1, 1, 1, 1, 1, VK_SAMPLE_COUNT_1_BIT, SAMP, VK_IMAGE_ASPECT_COLOR_BIT, false, false);
    env_ph_ = make_image(VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, 1, 1, 1, 6, 1, VK_SAMPLE_COUNT_1_BIT, SAMP, VK_IMAGE_ASPECT_COLOR_BIT, true, false);
    if (!white_ || !under_ph_ || !shadow_ph_ || !ao_ph_ || !env_ph_) { *err = "cannot create the placeholder textures"; return false; }
    acquire_slot();
    VkCommandBuffer cb = upload_cmd();
    for (Image *img : {white_.get(), ao_ph_.get(), env_ph_.get()}) {
        transition(cb, *img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue c{};
        if (img == white_.get() || img == ao_ph_.get()) { c.float32[0] = c.float32[1] = c.float32[2] = c.float32[3] = 1.f; }
        else { c.float32[3] = 1.f; }
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, img->layers};
        vkCmdClearColorImage(cb, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &r);
        transition(cb, *img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    for (Image *img : {under_ph_.get(), shadow_ph_.get()}) {
        transition(cb, *img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearDepthStencilValue c{1.f, 0};
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(cb, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &r);
        transition(cb, *img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    submit_uploads_alone();

    const unsigned nworkers = std::max(1u, std::min(4u, std::thread::hardware_concurrency() / 2));
    for (unsigned i = 0; i < nworkers; i++) workers_.emplace_back([this] { worker(); });
    std::fprintf(stderr, "AR_VK device=\"%s\" driver=\"%s\" api=%u.%u eds=%d msaa_max=%u depth_resolve_max=%d lazily=%d ds=%s\n",
                 props_.deviceName, driver_.c_str(), VK_API_VERSION_MAJOR(api_), VK_API_VERSION_MINOR(api_), eds_ ? 1 : 0,
                 supported_samples(4, 1), depth_resolve_max_ ? 1 : 0, lazily_ ? 1 : 0,
                 ds_format_ == VK_FORMAT_D32_SFLOAT_S8_UINT ? "D32S8" : "D24S8");
    return true;
}

VulkanDevice::~VulkanDevice() {
    {
        std::lock_guard<std::mutex> g(pm_);
        stop_ = true;
        queue_jobs_.clear();
    }
    pcv_.notify_all();
    for (std::thread &t : workers_) t.join();
    if (!dev_) return;
    wait_idle();
    save_cache();
    for (Slot &s : slots_) {
        retire(s);
        for (VkDescriptorPool p : s.dpools) vkDestroyDescriptorPool(dev_, p, nullptr);
        if (s.ring) { vkDestroyBuffer(dev_, s.ring, nullptr); vkFreeMemory(dev_, s.ring_mem, nullptr); }
        vkDestroyFence(dev_, s.fence, nullptr);
        vkDestroyCommandPool(dev_, s.pool, nullptr);
    }
    dying_ = true;   // from here on, dropping the last reference to an image destroys it at once
    textures_.clear();
    targets_.clear();
    for (ImagePtr *p : {&white_, &under_ph_, &shadow_ph_, &ao_ph_, &env_ph_, &shadow_, &under_, &foot_}) p->reset();
    for (Slot &s : slots_) retire(s);
    for (auto &kv : fbs_) vkDestroyFramebuffer(dev_, kv.second, nullptr);
    for (auto &kv : pipes_) if (kv.second.p) vkDestroyPipeline(dev_, kv.second.p, nullptr);
    for (auto &kv : composite_) if (kv.second) vkDestroyPipeline(dev_, kv.second, nullptr);
    for (auto &kv : resolve_) if (kv.second) vkDestroyPipeline(dev_, kv.second, nullptr);
    for (auto &a : depth_pipe_) for (VkPipeline p : a) if (p) vkDestroyPipeline(dev_, p, nullptr);
    for (auto &kv : shaders_.mods) vkDestroyShaderModule(dev_, kv.second, nullptr);
    for (auto &kv : target_passes_) vkDestroyRenderPass(dev_, kv.second, nullptr);
    for (auto &kv : color_passes_) vkDestroyRenderPass(dev_, kv.second, nullptr);
    if (depth_rp_) vkDestroyRenderPass(dev_, depth_rp_, nullptr);
    for (auto &kv : samplers_) vkDestroySampler(dev_, kv.second, nullptr);
    for (VkSampler s : {linear_clamp_, compare_, nearest_}) if (s) vkDestroySampler(dev_, s, nullptr);
    for (VkPipelineLayout l : {main_pl_, depth_pl_, pair_pl_}) if (l) vkDestroyPipelineLayout(dev_, l, nullptr);
    for (VkDescriptorSetLayout l : {pass_layout_, draw_layout_, depth_layout_, pair_layout_}) if (l) vkDestroyDescriptorSetLayout(dev_, l, nullptr);
    if (pcache_) vkDestroyPipelineCache(dev_, pcache_, nullptr);
    if (owns_) {
        vkDestroyDevice(dev_, nullptr);
        if (messenger_) {
            auto rm = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT");
            if (rm) rm(instance_, messenger_, nullptr);
        }
        vkDestroyInstance(instance_, nullptr);
    }
}

bool VulkanDevice::save_cache() {
    if (cache_path_.empty() || !pcache_) return false;
    size_t n = 0;
    if (vkGetPipelineCacheData(dev_, pcache_, &n, nullptr) != VK_SUCCESS || n == 0) return false;
    std::vector<char> blob(n);
    if (vkGetPipelineCacheData(dev_, pcache_, &n, blob.data()) != VK_SUCCESS) return false;
    const std::string tmp = cache_path_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(blob.data(), (std::streamsize)n);
        if (!out) return false;
    }
    return std::rename(tmp.c_str(), cache_path_.c_str()) == 0;
}

VulkanDeviceInfo VulkanDevice::info() const {
    VulkanDeviceInfo i;
    i.name = props_.deviceName; i.driver = driver_; i.api_version = api_;
    i.extended_dynamic_state = eds_; i.depth_resolve_max = depth_resolve_max_; i.lazily_allocated = lazily_;
    i.max_samples = supported_samples(4, 1);
    i.validation_errors = validation_errors_; i.validation_warnings = validation_warnings_;
    i.pipelines_built = built_.load(); i.shader_modules = modules_.load();
    i.draws_last_frame = draws_last_; i.render_passes_last_frame = passes_last_;
    i.compile_ms_total = compile_us_.load() / 1000.0;
    return i;
}

// ---------------------------------------------------------------------------------------------------------- memory
uint32_t VulkanDevice::memory_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) const {
    for (uint32_t i = 0; i < memprops_.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memprops_.memoryTypes[i].propertyFlags & want) == want && !(memprops_.memoryTypes[i].propertyFlags & avoid))
            return i;
    if (avoid) return memory_type(bits, want, 0);
    return UINT32_MAX;
}
bool VulkanDevice::make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, VkBuffer *b, VkDeviceMemory *m) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev_, &bi, nullptr, b) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev_, *b, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, props);
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev_, &ai, nullptr, m) != VK_SUCCESS) {
        vkDestroyBuffer(dev_, *b, nullptr); *b = VK_NULL_HANDLE; return false;
    }
    vkBindBufferMemory(dev_, *b, *m, 0);
    return true;
}
ImagePtr VulkanDevice::make_image(VkImageType type, VkFormat fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t layers, uint32_t levels,
                                  VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageAspectFlags aspect, bool cube, bool transient) {
    ImagePtr img(new Image(), [this](Image *i) { defer_destroy(i); });
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ii.imageType = type; ii.format = fmt; ii.extent = {w, h, d}; ii.mipLevels = levels; ii.arrayLayers = layers;
    ii.samples = samples; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage | (transient && lazily_ ? VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT : 0);
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev_, &ii, nullptr, &img->image) != VK_SUCCESS) return nullptr;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev_, img->image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = UINT32_MAX;
    if (transient && lazily_) ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev_, &ai, nullptr, &img->mem) != VK_SUCCESS) {
        vkDestroyImage(dev_, img->image, nullptr);
        return nullptr;
    }
    vkBindImageMemory(dev_, img->image, img->mem, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img->image;
    vi.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D
                  : layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    // A depth/stencil image is viewed (sampled, attached) through its depth aspect for sampling; attachments use the same view
    // only for depth-only formats, so a depth/stencil target gets a second, full view in make_target.
    vi.subresourceRange = {aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? (VkImageAspectFlags)VK_IMAGE_ASPECT_DEPTH_BIT : aspect, 0, levels, 0, layers};
    if (vkCreateImageView(dev_, &vi, nullptr, &img->view) != VK_SUCCESS) return nullptr;
    img->att = img->view;
    if ((aspect & VK_IMAGE_ASPECT_STENCIL_BIT) && levels == 1 && layers == 1) {
        vi.subresourceRange.aspectMask = aspect;
        if (vkCreateImageView(dev_, &vi, nullptr, &img->att) != VK_SUCCESS) img->att = img->view;
    }
    img->format = fmt; img->aspect = aspect; img->w = w; img->h = h; img->depth = d; img->layers = layers; img->levels = levels;
    return img;
}
void VulkanDevice::defer_destroy(Image *img) {
    // framebuffers naming its views go with it
    std::vector<VkFramebuffer> fbs;
    for (auto it = fbs_.begin(); it != fbs_.end();) {
        const auto &v = std::get<1>(it->first);
        if (std::find(v.begin(), v.end(), img->view) != v.end() || std::find(v.begin(), v.end(), img->att) != v.end()) {
            fbs.push_back(it->second);
            it = fbs_.erase(it);
        } else {
            ++it;
        }
    }
    VkDevice dev = dev_;
    auto destroy = [dev, img, fbs]() {
        for (VkFramebuffer f : fbs) vkDestroyFramebuffer(dev, f, nullptr);
        if (img->att && img->att != img->view) vkDestroyImageView(dev, img->att, nullptr);
        if (img->view) vkDestroyImageView(dev, img->view, nullptr);
        if (img->image) vkDestroyImage(dev, img->image, nullptr);
        if (img->mem) vkFreeMemory(dev, img->mem, nullptr);
        delete img;
    };
    if (dying_) destroy();
    else slot().garbage.push_back(destroy);   // the slot recording now: freed after its fence, past every earlier use
}
// Conservative barriers (all commands, all memory): the backend favours plain correctness; the tile-friendly passes keep
// their order within one render pass, where no barrier is needed.
void VulkanDevice::transition(VkCommandBuffer cb, Image &img, VkImageLayout to, bool always) {
    if (img.layout == to && !always) return;
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = img.layout; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange = {img.aspect, 0, img.levels, 0, img.layers};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    img.layout = to;
}

// ---------------------------------------------------------------------------------------------------------- frames
void VulkanDevice::retire(Slot &s) {
    if (s.submitted) {
        vkWaitForFences(dev_, 1, &s.fence, VK_TRUE, UINT64_MAX);
        vkResetFences(dev_, 1, &s.fence);
        s.submitted = false;
    }
    std::vector<std::pair<void (*)(void *), void *>> done;
    done.swap(s.done);
    for (auto &d : done) if (d.first) d.first(d.second);
    std::vector<std::function<void()>> garbage;
    garbage.swap(s.garbage);
    for (auto &g : garbage) g();
    garbage.clear();   // dropping held ImagePtrs may queue their destruction (into the slot recording next)
}
void VulkanDevice::acquire_slot() {
    if (slot_acquired_) return;
    slot_index_ = (slot_index_ + 1) % kSlots;
    Slot &s = slot();
    retire(s);
    for (VkDescriptorPool p : s.dpools) vkResetDescriptorPool(dev_, p, 0);
    s.dpool_next = 0;
    s.ring_used = 0;
    vkResetCommandPool(dev_, s.pool, 0);
    s.up_open = false;
    slot_acquired_ = true;
    // the other slot may have finished meanwhile: its callbacks run as soon as possible
    for (Slot &o : slots_)
        if (&o != &s && o.submitted && vkGetFenceStatus(dev_, o.fence) == VK_SUCCESS) retire(o);
}
VkCommandBuffer VulkanDevice::upload_cmd() {
    acquire_slot();
    Slot &s = slot();
    if (!s.up_open) {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(s.up, &bi);
        s.up_open = true;
    }
    return s.up;
}
void VulkanDevice::submit_uploads_alone() {
    if (!slot_acquired_ || recording_) return;
    Slot &s = slot();
    if (!s.up_open) return;
    vkEndCommandBuffer(s.up);
    s.up_open = false;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &s.up;
    vkQueueSubmit(queue_, 1, &si, s.fence);
    s.submitted = true;
    slot_acquired_ = false;
}
void VulkanDevice::begin_frame() {
    acquire_slot();
    Slot &s = slot();
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s.cmd, &bi);
    recording_ = true;
    under_valid_ = shadow_valid_ = foot_valid_ = false;
    passes_ = 0; draws_ = 0;
}
void VulkanDevice::end_frame(void (*done)(void *), void *ctx, void *) {
    if (!recording_) { if (done) done(ctx); return; }
    for (Target &t : targets_) flush(t);
    Slot &s = slot();
    vkEndCommandBuffer(s.cmd);
    recording_ = false;
    VkCommandBuffer cbs[2];
    uint32_t n = 0;
    if (s.up_open) { vkEndCommandBuffer(s.up); s.up_open = false; cbs[n++] = s.up; }
    cbs[n++] = s.cmd;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = n; si.pCommandBuffers = cbs;
    if (vkQueueSubmit(queue_, 1, &si, s.fence) != VK_SUCCESS) std::fprintf(stderr, "AR_VK error: vkQueueSubmit failed\n");
    s.submitted = true;
    s.done.emplace_back(done, ctx);
    slot_acquired_ = false;
    passes_last_ = passes_; draws_last_ = draws_;
}
void VulkanDevice::wait_idle() {
    if (recording_) return;   // the open frame's commands are not submitted yet
    submit_uploads_alone();
    for (Slot &s : slots_) retire(s);
}
VkDeviceSize VulkanDevice::stage(const void *data, size_t n, VkBuffer *buf) {
    Slot &s = slot();
    const VkDeviceSize need = (n + align_ - 1) / align_ * align_;
    if (!s.ring || s.ring_used + need > s.ring_size) {
        if (s.ring) {   // a frame still recorded may use the old ring: freed with the slot's garbage
            VkDevice dev = dev_;
            VkBuffer b = s.ring; VkDeviceMemory m = s.ring_mem;
            vkUnmapMemory(dev_, m);
            s.garbage.push_back([dev, b, m]() { vkDestroyBuffer(dev, b, nullptr); vkFreeMemory(dev, m, nullptr); });
        }
        const VkDeviceSize size = std::max<VkDeviceSize>({s.ring_size * 2, need * 2, 4u << 20});
        s.ring = VK_NULL_HANDLE; s.ring_mem = VK_NULL_HANDLE; s.ring_ptr = nullptr; s.ring_size = 0; s.ring_used = 0;
        if (!make_buffer(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &s.ring, &s.ring_mem))
            return *buf = VK_NULL_HANDLE, 0;
        void *p = nullptr;
        vkMapMemory(dev_, s.ring_mem, 0, size, 0, &p);
        s.ring_ptr = (uint8_t *)p; s.ring_size = size;
    }
    const VkDeviceSize off = s.ring_used;
    if (n) std::memcpy(s.ring_ptr + off, data, n);
    s.ring_used += need;
    *buf = s.ring;
    return off;
}
VkDescriptorSet VulkanDevice::alloc_set(VkDescriptorSetLayout layout) {
    Slot &s = slot();
    for (;;) {
        if (s.dpool_next == s.dpools.size()) {
            const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8192}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16384},
                                                  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2048}};
            VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pi.maxSets = 8192; pi.poolSizeCount = 3; pi.pPoolSizes = sizes;
            VkDescriptorPool p = VK_NULL_HANDLE;
            if (vkCreateDescriptorPool(dev_, &pi, nullptr, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
            s.dpools.push_back(p);
        }
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = s.dpools[s.dpool_next]; ai.descriptorSetCount = 1; ai.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(dev_, &ai, &set) == VK_SUCCESS) return set;
        s.dpool_next++;
    }
}

// ---------------------------------------------------------------------------------------------------------- textures
void VulkanDevice::upload_texture(uint32_t tex_id, uint32_t w, uint32_t h, const uint8_t *rgba) {
    if (!tex_id || !w || !h) return;
    uint32_t levels = 1;
    for (uint32_t m = std::max(w, h); m > 1; m >>= 1) levels++;
    ImagePtr img = make_image(VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, w, h, 1, 1, levels, VK_SAMPLE_COUNT_1_BIT,
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT, false, false);
    if (!img) { std::fprintf(stderr, "AR_VK error: no memory for texture %u (%ux%u)\n", tex_id, w, h); return; }
    VkCommandBuffer cb = upload_cmd();
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory smem = VK_NULL_HANDLE;
    const VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
    if (!make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging, &smem))
        return;
    void *p = nullptr;
    vkMapMemory(dev_, smem, 0, bytes, 0, &p);
    std::memcpy(p, rgba, (size_t)bytes);
    vkUnmapMemory(dev_, smem);
    VkDevice dev = dev_;
    slot().garbage.push_back([dev, staging, smem]() { vkDestroyBuffer(dev, staging, nullptr); vkFreeMemory(dev, smem, nullptr); });
    transition(cb, *img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cb, staging, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    // The mip chain by linear blits (Metal's generateMipmaps); level i - 1 goes to TRANSFER_SRC as it is read.
    for (uint32_t i = 1; i < levels; i++) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img->image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkImageBlit bl{};
        bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1};
        bl.srcOffsets[1] = {(int32_t)std::max(w >> (i - 1), 1u), (int32_t)std::max(h >> (i - 1), 1u), 1};
        bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
        bl.dstOffsets[1] = {(int32_t)std::max(w >> i, 1u), (int32_t)std::max(h >> i, 1u), 1};
        vkCmdBlitImage(cb, img->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
    }
    if (levels > 1) {   // all but the last level are TRANSFER_SRC now: one barrier per state to SHADER_READ
        VkImageMemoryBarrier b[2] = {{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}, {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}};
        b[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b[0].srcQueueFamilyIndex = b[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b[0].image = img->image; b[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels - 1, 0, 1};
        b[1] = b[0];
        b[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, levels - 1, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
        img->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        mips_blit_++;
    } else {
        transition(cb, *img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    textures_[tex_id] = img;   // recorded passes keep their own reference to the one it replaces
}
ImagePtr VulkanDevice::texture(uint32_t tex_id) {
    if (!tex_id) return white_;
    if (!target_tex_.empty()) {
        auto bt = target_tex_.find(tex_id);
        if (bt != target_tex_.end())
            return bt->second <= targets_.size() && targets_[bt->second - 1].layers == 1 && targets_[bt->second - 1].color
                   ? targets_[bt->second - 1].color : white_;
    }
    auto it = textures_.find(tex_id);
    return it == textures_.end() ? white_ : it->second;
}
VkSampler VulkanDevice::sampler(const fc_sampler &s, bool depth_pass) {
    const bool aniso16 = fine_ && !depth_pass;
    const bool smooth = filter_ == TextureFilter::smooth;
    const uint32_t key = s.linear | (uint32_t)s.wrap_s << 8 | (uint32_t)s.wrap_t << 16 | (aniso16 ? 1u << 24 : 0u) | (smooth ? 1u << 25 : 0u);
    VkSampler &st = samplers_[key];
    if (!st) {
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        const VkFilter own = s.linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        si.magFilter = own;
        if (smooth) {
            si.minFilter = VK_FILTER_LINEAR;
            si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            si.maxLod = VK_LOD_CLAMP_NONE;
            if (aniso_) { si.anisotropyEnable = VK_TRUE; si.maxAnisotropy = std::min(aniso16 ? 16.f : 4.f, props_.limits.maxSamplerAnisotropy); }
        } else {
            si.minFilter = own;                         // Metal's NotMipmapped: level 0 only
            si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            si.maxLod = 0.f;
        }
        si.addressModeU = address(s.wrap_s); si.addressModeV = address(s.wrap_t); si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        vkCreateSampler(dev_, &si, nullptr, &st);
    }
    return st;
}
bool VulkanDevice::read_image(Image &img, VkImageAspectFlags aspect, std::vector<uint8_t> &out, uint32_t level) {
    wait_idle();
    const uint32_t w = std::max(img.w >> level, 1u), h = std::max(img.h >> level, 1u);
    const VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
    VkBuffer buf; VkDeviceMemory mem;
    if (!make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &buf, &mem))
        return false;
    VkCommandBuffer cb = upload_cmd();
    const VkImageLayout was = img.layout;
    transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c{};
    c.imageSubresource = {aspect, level, 0, 1};
    c.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
    if (was != VK_IMAGE_LAYOUT_UNDEFINED) transition(cb, img, was);
    submit_uploads_alone();
    wait_idle();
    void *p = nullptr;
    vkMapMemory(dev_, mem, 0, bytes, 0, &p);
    out.assign((const uint8_t *)p, (const uint8_t *)p + bytes);
    vkUnmapMemory(dev_, mem);
    vkDestroyBuffer(dev_, buf, nullptr);
    vkFreeMemory(dev_, mem, nullptr);
    return true;
}
bool VulkanDevice::read_texture_level(uint32_t tex_id, uint32_t level, std::vector<uint8_t> &rgba, uint32_t &w, uint32_t &h) {
    auto it = textures_.find(tex_id);
    if (it == textures_.end() || level >= it->second->levels || recording_) return false;
    w = std::max(it->second->w >> level, 1u); h = std::max(it->second->h >> level, 1u);
    return read_image(*it->second, VK_IMAGE_ASPECT_COLOR_BIT, rgba, level);
}

// ---------------------------------------------------------------------------------------------------------- targets
uint32_t VulkanDevice::supported_samples(uint32_t wanted, uint32_t layers) const {
    if (layers > 1) return 1;
    for (uint32_t s : {4u, 2u}) if (s <= wanted && (sample_counts_ & s)) return s;
    return 1;
}
uint32_t VulkanDevice::create_target(uint32_t w, uint32_t h, uint32_t layers, uint32_t samples) {
    targets_.emplace_back();
    make_target(targets_.back(), w, h, layers, samples);
    return (uint32_t)targets_.size();
}
void VulkanDevice::resize_target(uint32_t t, uint32_t w, uint32_t h, uint32_t layers, uint32_t samples) {
    if (t == 0 || t > targets_.size()) return;
    Target &x = targets_[t - 1];
    if (x.w != std::max(w, 1u) || x.h != std::max(h, 1u) || x.layers != std::max(layers, 1u) || x.samples != supported_samples(samples, layers))
        make_target(x, w, h, layers, samples);
}
void VulkanDevice::release_target(uint32_t t) {
    if (t == 0 || t > targets_.size()) return;
    Target &x = targets_[t - 1];
    flush(x);
    for (ImagePtr *p : {&x.color, &x.depth, &x.ms_color, &x.ms_depth}) p->reset();
    x.w = x.h = 0; x.samples = 1; x.memoryless = false;
}
void VulkanDevice::make_target(Target &t, uint32_t w, uint32_t h, uint32_t layers, uint32_t samples) {
    flush(t);
    for (ImagePtr *p : {&t.color, &t.depth, &t.ms_color, &t.ms_depth}) p->reset();
    w = w ? w : 1; h = h ? h : 1;
    if (layers > 1) {
        static bool warned = false;
        if (!warned) { std::fprintf(stderr, "AR_VK layered targets are not supported yet: drawing one layer\n"); warned = true; }
    }
    layers = 1;
    t.w = w; t.h = h; t.layers = layers;
    t.samples = supported_samples(samples, layers);
    const VkImageUsageFlags cu = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageUsageFlags du = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const VkImageAspectFlags da = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    t.color = make_image(VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, w, h, 1, 1, 1, VK_SAMPLE_COUNT_1_BIT, cu, VK_IMAGE_ASPECT_COLOR_BIT, false, false);
    t.depth = make_image(VK_IMAGE_TYPE_2D, ds_format_, w, h, 1, 1, 1, VK_SAMPLE_COUNT_1_BIT, du, da, false, false);
    t.memoryless = false;
    if (t.samples > 1) {
        t.memoryless = lazily_;
        const VkSampleCountFlagBits sc = (VkSampleCountFlagBits)t.samples;
        t.ms_color = make_image(VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, w, h, 1, 1, 1, sc, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT, false, true);
        t.ms_depth = make_image(VK_IMAGE_TYPE_2D, ds_format_, w, h, 1, 1, 1, sc, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, da, false, true);
    }
}
VkFramebuffer VulkanDevice::framebuffer(VkRenderPass rp, const std::vector<VkImageView> &views, uint32_t w, uint32_t h) {
    auto key = std::make_tuple(rp, views, w, h);
    auto it = fbs_.find(key);
    if (it != fbs_.end()) return it->second;
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = rp; fi.attachmentCount = (uint32_t)views.size(); fi.pAttachments = views.data();
    fi.width = w; fi.height = h; fi.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    vkCreateFramebuffer(dev_, &fi, nullptr, &fb);
    return fbs_[key] = fb;
}
// A target's render pass: colour + depth/stencil (+ their single-sample resolves with MSAA). Attachments start and end in
// their attachment layouts (the caller transitions); a load pass loads all, a clear pass clears all.
VkRenderPass VulkanDevice::target_pass(uint32_t samples, bool clear) {
    VkRenderPass &rp = target_passes_[{samples, clear}];
    if (rp) return rp;
    const bool ms = samples > 1;
    const VkAttachmentLoadOp load = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    std::vector<VkAttachmentDescription2> att;
    auto add = [&](VkFormat f, uint32_t s, VkAttachmentLoadOp lo, VkAttachmentStoreOp so, VkAttachmentLoadOp slo, VkAttachmentStoreOp sso,
                   VkImageLayout lay) {
        VkAttachmentDescription2 a{VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2};
        a.format = f; a.samples = (VkSampleCountFlagBits)s;
        a.loadOp = lo; a.storeOp = so; a.stencilLoadOp = slo; a.stencilStoreOp = sso;
        a.initialLayout = lo == VK_ATTACHMENT_LOAD_OP_LOAD || slo == VK_ATTACHMENT_LOAD_OP_LOAD ? lay : VK_IMAGE_LAYOUT_UNDEFINED;
        a.finalLayout = lay;
        att.push_back(a);
    };
    const VkImageLayout CA = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, DA = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    const VkAttachmentStoreOp ST = VK_ATTACHMENT_STORE_OP_STORE, DC = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    const VkAttachmentLoadOp NL = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    if (!ms) {
        add(VK_FORMAT_R8G8B8A8_UNORM, 1, load, ST, NL, DC, CA);
        add(ds_format_, 1, load, ST, load, ST, DA);
    } else {
        // multisample attachments: stored only when not transient (Metal's StoreAndMultisampleResolve)
        add(VK_FORMAT_R8G8B8A8_UNORM, samples, load, lazily_ ? DC : ST, NL, DC, CA);
        add(ds_format_, samples, load, lazily_ ? DC : ST, load, lazily_ ? DC : ST, DA);
        add(VK_FORMAT_R8G8B8A8_UNORM, 1, NL, ST, NL, DC, CA);
        add(ds_format_, 1, NL, ST, NL, DC, DA);
    }
    VkAttachmentReference2 cref{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 0, CA, VK_IMAGE_ASPECT_COLOR_BIT};
    VkAttachmentReference2 dref{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 1, DA, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT};
    VkAttachmentReference2 rref{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 2, CA, VK_IMAGE_ASPECT_COLOR_BIT};
    VkAttachmentReference2 drref{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 3, DA, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT};
    VkSubpassDescriptionDepthStencilResolve dsr{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE};
    // reverse-Z: the nearest sample is the largest (Metal's MTLMultisampleDepthResolveFilterMax)
    const bool max = depth_resolve_max_ && independent_none_;
    dsr.depthResolveMode = max ? VK_RESOLVE_MODE_MAX_BIT : VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    dsr.stencilResolveMode = max ? VK_RESOLVE_MODE_NONE : VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    dsr.pDepthStencilResolveAttachment = &drref;
    VkSubpassDescription2 sp{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
    sp.pNext = ms ? &dsr : nullptr;
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1; sp.pColorAttachments = &cref;
    sp.pResolveAttachments = ms ? &rref : nullptr;
    sp.pDepthStencilAttachment = &dref;
    VkRenderPassCreateInfo2 ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
    ri.attachmentCount = (uint32_t)att.size(); ri.pAttachments = att.data();
    ri.subpassCount = 1; ri.pSubpasses = &sp;
    if (create_rp2_(dev_, &ri, nullptr, &rp) != VK_SUCCESS) std::fprintf(stderr, "AR_VK error: target render pass (samples %u)\n", samples);
    return rp;
}
VkRenderPass VulkanDevice::depth_pass() {
    if (depth_rp_) return depth_rp_;
    VkAttachmentDescription2 a{VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2};
    a.format = VK_FORMAT_D32_SFLOAT; a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; a.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference2 r{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                             VK_IMAGE_ASPECT_DEPTH_BIT};
    VkSubpassDescription2 sp{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.pDepthStencilAttachment = &r;
    VkRenderPassCreateInfo2 ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
    ri.attachmentCount = 1; ri.pAttachments = &a; ri.subpassCount = 1; ri.pSubpasses = &sp;
    create_rp2_(dev_, &ri, nullptr, &depth_rp_);
    return depth_rp_;
}
// One colour attachment (+ an optional depth/stencil one), loaded or not, in attachment layouts at both ends.
VkRenderPass VulkanDevice::color_pass(VkFormat fmt, bool load, VkFormat dfmt) {
    VkRenderPass &rp = color_passes_[{fmt, load, dfmt}];
    if (rp) return rp;
    VkAttachmentDescription2 a[2] = {{VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2}, {VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2}};
    a[0].format = fmt; a[0].samples = VK_SAMPLE_COUNT_1_BIT;
    a[0].loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE; a[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a[0].initialLayout = load ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    a[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    a[1] = a[0];
    a[1].format = dfmt;
    a[1].stencilLoadOp = a[1].loadOp; a[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    a[1].initialLayout = load ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    a[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference2 c{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT};
    VkAttachmentReference2 d{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                             VK_IMAGE_ASPECT_DEPTH_BIT};
    VkSubpassDescription2 sp{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1; sp.pColorAttachments = &c;
    sp.pDepthStencilAttachment = dfmt != VK_FORMAT_UNDEFINED ? &d : nullptr;
    VkRenderPassCreateInfo2 ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
    ri.attachmentCount = dfmt != VK_FORMAT_UNDEFINED ? 2 : 1; ri.pAttachments = a; ri.subpassCount = 1; ri.pSubpasses = &sp;
    create_rp2_(dev_, &ri, nullptr, &rp);
    return rp;
}

// ---------------------------------------------------------------------------------------------------------- shaders
VkShaderModule VulkanDevice::shader(int stage, const std::string &src, std::string *why) const {
    {
        std::lock_guard<std::mutex> g(shaders_.m);
        auto it = shaders_.mods.find({stage, src});
        if (it != shaders_.mods.end()) return it->second;
    }
    const auto t0 = std::chrono::steady_clock::now();
    thread_local shaderc_compiler_t compiler = shaderc_compiler_initialize();
    shaderc_compile_options_t opt = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(opt, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1);
    shaderc_compile_options_set_optimization_level(opt, shaderc_optimization_level_performance);
    shaderc_compilation_result_t r = shaderc_compile_into_spv(compiler, src.c_str(), src.size(),
                                                             stage == 0 ? shaderc_vertex_shader : shaderc_fragment_shader,
                                                             stage == 0 ? "vmain.vert" : "fmain.frag", "main", opt);
    shaderc_compile_options_release(opt);
    VkShaderModule mod = VK_NULL_HANDLE;
    if (shaderc_result_get_compilation_status(r) != shaderc_compilation_status_success) {
        if (why) *why = std::string("GLSL: ") + shaderc_result_get_error_message(r);
    } else {
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = shaderc_result_get_length(r);
        mi.pCode = (const uint32_t *)shaderc_result_get_bytes(r);
        if (vkCreateShaderModule(dev_, &mi, nullptr, &mod) != VK_SUCCESS && why) *why = "vkCreateShaderModule failed";
    }
    shaderc_result_release(r);
    compile_us_ += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    if (!mod) return VK_NULL_HANDLE;
    std::lock_guard<std::mutex> g(shaders_.m);
    auto ins = shaders_.mods.emplace(std::make_pair(stage, src), mod);
    if (!ins.second) { vkDestroyShaderModule(dev_, mod, nullptr); return ins.first->second; }   // another thread was first
    modules_++;
    return mod;
}

// ---------------------------------------------------------------------------------------------------------- pipelines
VkPipeKey VulkanDevice::pipe_key(const DrawPacket &draw, uint32_t samples, bool pass_depth) const {
    const PipeIdentity k = pipe_identity(pipeline_form(draw));
    VkPipeKey p{k.slots, k.opts, k.blend, k.silhouette, k.depth_only, k.table_shadow, k.clip, (uint8_t)samples,
                (uint8_t)(edge_ && samples > 1 && (k.opts & FC_CC_TEXTURE_EDGE) ? 1 : 0), k.cut};
    if (!eds_) {
        const DepthState ds = depth_state(draw, pass_depth);
        p.baked = 1;
        p.cull = draw.cull;
        p.dtest = ds.test; p.dwrite = ds.write; p.dcompare = (uint8_t)ds.compare; p.stencil = ds.stencil;
        p.smode = pass_depth ? stencil_mode(draw) : 0;
    }
    return p;
}
VkPipeline VulkanDevice::build_pipeline(const VkPipeKey &k, const DrawPacket &draw, std::string *why, bool *refused) const {
    const DrawPacket d = pipeline_form(draw);
    CombinerIR ir = decode_combiner(d.cc);
    ir.flat_color = d.silhouette != kSilhouetteNone;
    ir.flat_cutout = d.silhouette == kSilhouetteCutout;
    ir.depth_fill = d.depth_only;
    ir.table_shadow = d.table_shadow != 0;
    ir.clip = d.clip_on != 0;
    ir.alpha_coverage = k.a2c != 0;
    ir.layer_out = false;
    ir.cutaway = d.cutaway != 0;
    *refused = false;
    if (ir.unsupported) { *why = "unsupported combiner feature (light map)"; return VK_NULL_HANDLE; }
    VkShaderModule vs = shader(0, glsl_vertex(), why);
    VkShaderModule fs = vs ? shader(1, glsl_fragment(ir), why) : VK_NULL_HANDLE;
    if (!vs || !fs) { *refused = true; return VK_NULL_HANDLE; }
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkVertexInputBindingDescription vb{0, sizeof(fc_vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va[4] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32_SFLOAT, 12},
                                               {2, 0, VK_FORMAT_R32G32_SFLOAT, 20}, {3, 0, VK_FORMAT_R8G8B8A8_UNORM, 28}};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 4; vi.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = k.baked ? (k.cull == FC_CULL_BACK ? VK_CULL_MODE_BACK_BIT : k.cull == FC_CULL_FRONT ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE)
                          : VK_CULL_MODE_NONE;
    // The viewport is flipped (negative height) so NDC +y is up as in Metal; counter-clockwise stays the front face.
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.depthBiasEnable = VK_TRUE;   // the values are dynamic: 0 except decals
    rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = (VkSampleCountFlagBits)k.samples;
    if (k.a2c) { ms.alphaToCoverageEnable = VK_TRUE; ms.alphaToOneEnable = a2one_ ? VK_TRUE : VK_FALSE; }
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    if (k.baked) {
        ds.depthTestEnable = k.dtest; ds.depthWriteEnable = k.dwrite; ds.depthCompareOp = (VkCompareOp)k.dcompare;
        ds.stencilTestEnable = k.stencil;
        if (k.stencil) {
            DrawPacket probe{};
            // rebuild the stencil ops of the mode the key was made for
            if (k.smode == kStencilSilhouette) probe.silhouette = kSilhouetteFlat;
            else if (k.smode == kStencilFaint) probe.depth_only = kDepthOnlyFaint;
            else { probe.depth_write = 1; probe.focus = k.smode == kStencilMarkFocus; }
            ds.front = ds.back = depth_state(probe, true).st;
        }
    }
    VkPipelineColorBlendAttachmentState ca{};
    ca.colorWriteMask = d.depth_only ? 0 : 0xF;
    if (d.blend) {   // RGB SrcAlpha/1-SrcAlpha, alpha One/1-SrcAlpha: premultiplied coverage
        ca.blendEnable = VK_TRUE;
        ca.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; ca.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ca.colorBlendOp = VK_BLEND_OP_ADD;
        ca.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; ca.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ca.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &ca;
    std::vector<VkDynamicState> dyn = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
                                       VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK};
    if (!k.baked)
        for (VkDynamicState s : {VK_DYNAMIC_STATE_CULL_MODE_EXT, VK_DYNAMIC_STATE_FRONT_FACE_EXT, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT,
                                 VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT,
                                 VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE_EXT, VK_DYNAMIC_STATE_STENCIL_OP_EXT})
            dyn.push_back(s);
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = (uint32_t)dyn.size(); dy.pDynamicStates = dyn.data();
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 2; pi.pStages = st;
    pi.pVertexInputState = &vi; pi.pInputAssemblyState = &ia; pi.pViewportState = &vp; pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms; pi.pDepthStencilState = &ds; pi.pColorBlendState = &cb; pi.pDynamicState = &dy;
    pi.layout = main_pl_;
    pi.renderPass = const_cast<VulkanDevice *>(this)->target_pass(k.samples, false);
    const auto t0 = std::chrono::steady_clock::now();
    VkPipeline p = VK_NULL_HANDLE;
    const VkResult r = vkCreateGraphicsPipelines(dev_, pcache_, 1, &pi, nullptr, &p);
    compile_us_ += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    if (r != VK_SUCCESS) { *why = "vkCreateGraphicsPipelines failed (" + std::to_string((int)r) + ")"; *refused = true; return VK_NULL_HANDLE; }
    built_++;
    return p;
}
// at_draw: a draw needs it now. With async on, a draw never compiles nor waits: the pipeline is queued and the draw
// skipped (counted), a cutaway variant falling back to its plain pipeline (Metal's pipeline()).
VkPipeline VulkanDevice::pipeline(const DrawPacket &d, uint32_t samples, bool pass_depth, std::string *err, bool at_draw) {
    if (d.silhouette && decode_combiner(d.cc).unsupported) return VK_NULL_HANDLE;
    const VkPipeKey k = pipe_key(d, samples, pass_depth);
    const bool defer = at_draw && async_;
    std::unique_lock<std::mutex> g(pm_);
    PipeEntry &e = pipes_[k];
    if (e.state == kDone) { if (!e.p && err) *err = e.why; return e.p; }
    if (defer) {
        if (e.state == kNone) { e.state = kQueued; e.d = d; queue_jobs_.push_front(k); pcv_.notify_all(); draw_builds_++; }
        if (missed_.insert(k).second) {
            char b[96];
            std::snprintf(b, sizeof b, "%016llx/%x/b%u/f%u/d%u/t%u/c%u/s%u/k%u", (unsigned long long)k.slots, k.opts, k.blend, k.flat,
                          k.depth_only, k.table_shadow, k.clip, k.samples, (unsigned)k.cut);
            miss_log_.push_back(b);
        }
        deferred_++;
        g.unlock();
        if (d.cutaway) { DrawPacket plain = d; plain.cutaway = 0; return pipeline(plain, samples, pass_depth, err, at_draw); }
        return VK_NULL_HANDLE;
    }
    if (e.state == kBuilding) {
        pcv_.wait(g, [&] { return e.state == kDone; });
        if (!e.p && err) *err = e.why;
        return e.p;
    }
    e.state = kBuilding;   // none or queued: built here (a queued job finds it taken)
    g.unlock();
    std::string why;
    bool refused = false;
    VkPipeline p = build_pipeline(k, d, &why, &refused);
    g.lock();
    e.p = p; e.why = why; e.state = kDone;
    if (refused) { std::fprintf(stderr, "AR_VK combiner %016llx/%x: %s\n", (unsigned long long)k.slots, k.opts, why.c_str()); refused_.push_back(why); }
    pcv_.notify_all();
    if (at_draw) draw_builds_++;
    if (!p && err) *err = why;
    return p;
}
void VulkanDevice::worker() {
    std::unique_lock<std::mutex> g(pm_);
    for (;;) {
        pcv_.wait(g, [&] { return stop_ || !queue_jobs_.empty(); });
        if (stop_) return;
        const VkPipeKey k = queue_jobs_.front();
        queue_jobs_.pop_front();
        PipeEntry &e = pipes_[k];
        if (e.state != kQueued) continue;
        e.state = kBuilding;
        const DrawPacket d = e.d;
        g.unlock();
        std::string why;
        bool refused = false;
        VkPipeline p = build_pipeline(k, d, &why, &refused);
        g.lock();
        e.p = p; e.why = why; e.state = kDone;
        if (refused) refused_.push_back(why);
        pcv_.notify_all();
    }
}
void VulkanDevice::enqueue(const VkPipeKey &k, const DrawPacket &d) {
    PipeEntry &e = pipes_[k];
    if (e.state != kNone) return;
    e.state = kQueued; e.d = d;
    queue_jobs_.push_back(k);
}
bool VulkanDevice::compile_combiner(const fc_combiner_key &key, std::string *error, uint32_t samples) {
    DrawPacket d{};
    d.cc = normalize_combiner(key);
    d.blend = (d.cc.opts & FC_CC_ALPHA) != 0;
    d.depth_test = d.depth_write = 1;
    return pipeline(d, supported_samples(samples, 1), true, error, false) != VK_NULL_HANDLE;
}
std::string VulkanDevice::pipeline_list() const {
    std::set<std::string> lines;
    std::lock_guard<std::mutex> g(pm_);
    for (const auto &kv : pipes_) {
        if (kv.second.state != kDone || !kv.second.p) continue;
        const VkPipeKey &k = kv.first;
        char b[96];
        const int len = std::snprintf(b, sizeof b, "%016llx %x %u %u %u %u %u %u %u %u", (unsigned long long)k.slots, k.opts, kListFormat,
                                      (unsigned)k.blend, 1u, (unsigned)k.flat, (unsigned)k.depth_only, (unsigned)k.table_shadow,
                                      (unsigned)k.clip, (unsigned)k.samples);
        if (len > 0 && (size_t)len < sizeof b) std::snprintf(b + len, sizeof b - (size_t)len, k.cut ? " 1\n" : "\n");
        lines.insert(b);
    }
    std::string out;
    for (const std::string &l : lines) out += l;
    return out;
}
namespace {
struct ListLine { DrawPacket d; uint32_t samples; };
std::vector<ListLine> parse_pipeline_lines(const std::string &list) {
    std::vector<ListLine> out;
    size_t pos = 0, lines = 0;
    while (pos < list.size() && lines++ < kMaxWarmLines) {
        size_t e = list.find('\n', pos);
        if (e == std::string::npos) e = list.size();
        const size_t len = e - pos, at = pos;
        pos = e + 1;
        if (len > kMaxWarmLineBytes) continue;
        const std::string line = list.substr(at, len);
        unsigned long long slots = 0;
        unsigned opts = 0, fmt = 0, blend = 0, depth = 0, flat = 0, depth_only = 0, table_shadow = 0, clip = 0, samples = 1, cut = 0;
        char tail = 0;
        const int got = std::sscanf(line.c_str(), "%llx %x %u %u %u %u %u %u %u %u %u %c", &slots, &opts, &fmt, &blend, &depth, &flat,
                                    &depth_only, &table_shadow, &clip, &samples, &cut, &tail);
        if ((got != 8 && got != 9 && got != 10 && got != 11) || blend > 1 || depth != 1 || flat > kSilhouetteCutout ||
            depth_only > kDepthOnlyPrepass || table_shadow > 1 || clip > 1 || (samples != 1 && samples != 2 && samples != 4) || cut > 1 ||
            fmt != kListFormat)
            continue;
        DrawPacket d{};
        d.cc = fc_combiner_key{slots, opts, 0};
        d.blend = (uint8_t)blend; d.silhouette = (uint8_t)flat; d.depth_only = (uint8_t)depth_only; d.table_shadow = (uint8_t)table_shadow;
        d.clip_on = (uint8_t)clip; d.cutaway = (uint8_t)cut;
        d.depth_test = 1; d.depth_write = d.blend ? 0 : 1; d.cull = FC_CULL_BACK;
        out.push_back(ListLine{d, samples});
    }
    return out;
}
}
size_t VulkanDevice::warm_pipelines(const std::string &list, uint32_t at_samples) {
    std::vector<std::pair<VkPipeKey, DrawPacket>> jobs;
    {
        std::lock_guard<std::mutex> g(pm_);
        for (const ListLine &l : parse_pipeline_lines(list)) {
            const uint32_t s = supported_samples(at_samples ? at_samples : l.samples, 1);
            const VkPipeKey k = pipe_key(l.d, s, true);
            PipeEntry &e = pipes_[k];
            if (e.state == kNone || e.state == kQueued) { e.state = kBuilding; jobs.emplace_back(k, l.d); }
        }
    }
    std::atomic<size_t> next{0}, built{0};
    auto run = [&]() {
        for (size_t i; (i = next++) < jobs.size();) {
            std::string why;
            bool refused = false;
            VkPipeline p = build_pipeline(jobs[i].first, jobs[i].second, &why, &refused);
            std::lock_guard<std::mutex> g(pm_);
            PipeEntry &e = pipes_[jobs[i].first];
            e.p = p; e.why = why; e.state = kDone;
            if (refused) refused_.push_back(why);
            if (p) built++;
            pcv_.notify_all();
        }
    };
    std::vector<std::thread> pool;
    const unsigned n = std::max(1u, std::min<unsigned>(std::thread::hardware_concurrency(), (unsigned)jobs.size()));
    for (unsigned i = 1; i < n; i++) pool.emplace_back(run);
    run();
    for (std::thread &t : pool) t.join();
    return built;
}
size_t VulkanDevice::warm_pipelines_background(const std::string &list, const std::vector<DrawPacket> &draws, uint32_t samples) {
    std::vector<DrawPacket> want = draws;
    for (const ListLine &l : parse_pipeline_lines(list)) {
        want.push_back(l.d);
        for (const DrawPacket &z : pipeline_siblings(l.d)) want.push_back(z);
    }
    const uint32_t s = supported_samples(samples, 1);
    size_t queued = 0;
    std::lock_guard<std::mutex> g(pm_);
    for (const DrawPacket &d : want) {
        if (d.silhouette && decode_combiner(d.cc).unsupported) continue;
        const VkPipeKey k = pipe_key(d, s, true);
        if (pipes_.count(k) && pipes_[k].state != kNone) continue;
        enqueue(k, d);
        queued++;
    }
    pcv_.notify_all();
    return queued;
}
size_t VulkanDevice::pipelines_warming() {
    std::lock_guard<std::mutex> g(pm_);
    size_t n = 0;
    for (const auto &kv : pipes_) n += kv.second.state == kQueued || kv.second.state == kBuilding;
    return n;
}
bool VulkanDevice::wait_pipelines_warmed(double timeout_s) {
    std::unique_lock<std::mutex> g(pm_);
    return pcv_.wait_for(g, std::chrono::duration<double>(timeout_s), [&] {
        for (const auto &kv : pipes_) if (kv.second.state == kQueued || kv.second.state == kBuilding) return false;
        return true;
    });
}

// ---------------------------------------------------------------------------------------------------------- lighting
void VulkanDevice::set_lighting(const LightingFrame *lf) {
    light_ = LightU{};
    if (!lf || !lf->on) return;
    for (int k = 0; k < 3; k++) { light_.dir_on[k] = lf->dir[k]; light_.tint_amb[k] = lf->tint[k]; }
    light_.dir_on[3] = 1.f;
    light_.tint_amb[3] = lf->ambient;
    light_.contact[0] = lf->contact ? lf->contact_strength : 0.f;
    light_.contact[1] = 0.03f * lf->upm;
    light_.contact[2] = 0.015f * lf->upm;
    light_.contact[3] = lf->ground_y;
    uv_from_clip(under_desc_.clip_from_level, light_.under_uv);
    light_.under_range[0] = under_desc_.y0;
    light_.under_range[1] = under_desc_.span;
    light_.shadow[0] = shadow_valid_ && lf->shadows ? 1.f : 0.f;
    light_.shadow[1] = 1.f + 2.f * lf->softness;
    light_.shadow[2] = 1.f / (float)kShadowSize;
    light_.shadow[3] = 3.f * shadow_fit_.texel_units;
    uv_from_clip(shadow_fit_.clip_from_level, light_.shadow_uv);
    light_.cast[0] = lf->table_cast;
    light_.relight[0] = lf->relight ? 1.f : 0.f;
    light_.relight[1] = lf->relight_strength;
    // no AO volume and no probe cube on this backend yet: ao.x and toy.w stay 0
    light_.toy[0] = lf->toy ? 1.f : 0.f;
    light_.toy[1] = lf->gloss;
    light_.toy[2] = lf->rim;
    light_.env5[3] = 1.f;
    if (lf->liquid) {
        const uint32_t n = std::min<uint32_t>(lf->water_count, 8u);
        light_.liquid[0] = 1.f; light_.liquid[1] = lf->liquid_time_s; light_.liquid[2] = (float)n; light_.liquid[3] = 0.25f * lf->liquid_upm;
        light_.liquid2[0] = 6.2831853f / (0.04f * lf->liquid_upm); light_.liquid2[1] = 0.08f;
        light_.liquid2[2] = lf->water_trim && foot_valid_ && n > 0 ? 1.f : 0.f;
        light_.liquid2[3] = 0.01f * lf->liquid_upm;
        for (uint32_t i = 0; i < n; i++) {
            for (int k = 0; k < 4; k++) light_.water_box[i][k] = lf->water[i][k];
            light_.water_y[i / 4][i % 4] = lf->water[i][4];
        }
        if (light_.liquid2[2] > 0.5f) {
            light_.water_foot[0] = foot_window_[0]; light_.water_foot[1] = foot_window_[1];
            light_.water_foot[2] = 1.f / (foot_window_[2] - foot_window_[0]); light_.water_foot[3] = 1.f / (foot_window_[3] - foot_window_[1]);
        }
    }
    if (lf->cut && lf->cut_radius > 0.f) {
        for (int k = 0; k < 3; k++) light_.cut[k] = lf->cut_target[k];
        light_.cut[3] = lf->cut_radius;
        light_.cut2[0] = 0.f; light_.cut2[1] = lf->cut_keep; light_.cut2[2] = std::max(lf->cut_cell, 1e-4f);
    }
    if (lf->trans_kind) {
        light_.trans[0] = (float)lf->trans_kind; light_.trans[1] = lf->trans_cover; light_.trans[2] = lf->trans_radius; light_.trans[3] = lf->trans_soft;
        for (int k = 0; k < 3; k++) { light_.trans_rgb[k] = lf->trans_rgb[k]; light_.trans_c[k] = lf->trans_centre[k]; }
    }
    if (lf->fine_textures && filter_ == TextureFilter::smooth) { light_.tex[0] = 1.f; light_.tex[1] = 0.25f; }
}

// ---------------------------------------------------------------------------------------------------------- passes
void VulkanDevice::encode_pass(uint32_t target, const PassDesc &pass, const ViewConstants &view, const fc_vertex *verts, uint32_t nv,
                               const DrawPacket *draws, uint32_t nd, const int8_t *normals, const float *prev_pos) {
    if (!recording_ || target == 0 || target > targets_.size()) return;
    Target &t = targets_[target - 1];
    if (!t.color) return;
    if (pass.clear && t.nrec) flush(t);
    if (!target_tex_.empty())   // a draw sampling a target sees what it drew this frame
        for (uint32_t i = 0; i < nd; i++)
            for (uint32_t k : {draws[i].tex[0], draws[i].tex[1]}) {
                auto bt = k ? target_tex_.find(k) : target_tex_.end();
                if (bt != target_tex_.end() && bt->second != target && bt->second <= targets_.size()) flush(targets_[bt->second - 1]);
            }
    if (t.nrec == t.recs.size()) t.recs.emplace_back();
    Rec &r = t.recs[t.nrec++];
    r.pass = pass; r.view = view; r.light = light_; r.nv = nv;
    r.shadow = shadow_valid_ ? shadow_ : shadow_ph_;
    r.under = under_valid_ ? under_ : under_ph_;
    r.ao = ao_ph_;
    r.env = env_ph_; r.env_prev = env_ph_;
    r.foot = foot_valid_ ? foot_ : under_ph_;
    r.draws.clear();
    r.tex.clear();
    r.vb = r.nb = r.pb = VK_NULL_HANDLE; r.voff = r.noff = r.poff = 0;
    if (nv && nd) {
        r.vsize = (VkDeviceSize)nv * sizeof(fc_vertex);
        r.voff = stage(verts, (size_t)r.vsize, &r.vb);
        if (normals) { r.nsize = (VkDeviceSize)nv * 4; r.noff = stage(normals, (size_t)r.nsize, &r.nb); }
        if (prev_pos && view.interp_on) { r.psize = (VkDeviceSize)nv * 12; r.poff = stage(prev_pos, (size_t)r.psize, &r.pb); }
        r.draws.assign(draws, draws + nd);
        r.tex.resize((size_t)nd * 2);
        for (uint32_t i = 0; i < nd; i++) { r.tex[2 * i] = texture(draws[i].tex[0]); r.tex[2 * i + 1] = texture(draws[i].tex[1]); }
    }
}
void VulkanDevice::flush_sampling(const ImagePtr &img) {
    if (!img) return;
    for (Target &t : targets_)
        for (size_t i = 0; i < t.nrec; i++)
            if (t.recs[i].shadow == img || t.recs[i].under == img || t.recs[i].foot == img) { flush(t); break; }
}
void VulkanDevice::flush(Target &t) {
    if (!t.nrec) return;
    const size_t n = t.nrec;
    t.nrec = 0;
    const bool ms = t.samples > 1;
    if (!recording_) return;
    if (ms && t.memoryless && !t.recs[0].pass.clear) {   // transient MSAA attachments cannot be loaded (Metal's late passes)
        late_ += n;
        return;
    }
    VkCommandBuffer cb = slot().cmd;
    // everything the passes sample goes to SHADER_READ before the render pass (no barrier inside it)
    for (size_t i = 0; i < n; i++) {
        Rec &r = t.recs[i];
        for (ImagePtr *p : {&r.shadow, &r.under, &r.foot}) if (*p) transition(cb, **p, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        for (ImagePtr &x : r.tex)
            if (x && x != t.color && x->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) transition(cb, *x, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    const bool clear = t.recs[0].pass.clear;
    // always a barrier: an earlier pass of this frame may have written the same attachments
    transition(cb, *t.color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true);
    transition(cb, *t.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, true);
    std::vector<VkImageView> views;
    if (!ms) {
        views = {t.color->view, t.depth->att};
    } else {
        transition(cb, *t.ms_color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true);
        transition(cb, *t.ms_depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, true);
        views = {t.ms_color->view, t.ms_depth->att, t.color->view, t.depth->att};
    }
    VkRenderPass rp = target_pass(t.samples, clear);
    VkFramebuffer fb = framebuffer(rp, views, t.w, t.h);
    const PassDesc &p0 = t.recs[0].pass;
    VkClearValue cv[4] = {};
    for (int k = 0; k < 4; k++) cv[0].color.float32[k] = p0.clear_color[k];
    cv[1].depthStencil = {0.f, 0};
    cv[2] = cv[0]; cv[3] = cv[1];
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = rp; bi.framebuffer = fb;
    bi.renderArea = {{0, 0}, {t.w, t.h}};
    bi.clearValueCount = ms ? 4 : 2; bi.pClearValues = cv;
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    for (size_t i = 0; i < n; i++) encode_rec(cb, t, t.recs[i]);
    vkCmdEndRenderPass(cb);
    if (ms) { t.ms_color->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; t.ms_depth->layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; }
    passes_++;
    for (size_t i = 0; i < n; i++) {   // the command buffer holds what the pass used; the slot's garbage keeps it alive
        Rec &r = t.recs[i];
        std::vector<ImagePtr> keep;
        for (ImagePtr *p : {&r.shadow, &r.under, &r.ao, &r.env, &r.env_prev, &r.foot}) { keep.push_back(*p); p->reset(); }
        for (ImagePtr &x : r.tex) keep.push_back(x);
        r.tex.clear();
        slot().garbage.push_back([keep]() {});
    }
}
void VulkanDevice::encode_rec(VkCommandBuffer cb, Target &t, Rec &r) {
    Viewport v = r.pass.viewport;
    if (v.w <= 0 || v.h <= 0) v = Viewport{0, 0, float(t.w), float(t.h)};
    const uint32_t sx = (uint32_t)std::max(0.f, v.x), sy = (uint32_t)std::max(0.f, v.y);
    const uint32_t sw = std::min<uint32_t>((uint32_t)v.w, t.w > sx ? t.w - sx : 0), sh = std::min<uint32_t>((uint32_t)v.h, t.h > sy ? t.h - sy : 0);
    if (!r.vb || r.draws.empty() || !sw || !sh) return;
    // NDC +y up (Metal): a flipped viewport
    const VkViewport vp{v.x, v.y + v.h, v.w, -v.h, 0.f, 1.f};
    vkCmdSetViewport(cb, 0, 1, &vp);
    const VkRect2D sc{{(int32_t)sx, (int32_t)sy}, {sw, sh}};
    vkCmdSetScissor(cb, 0, 1, &sc);
    const ViewConstants &view = r.view;
    ViewU vu{};
    std::memcpy(vu.clip_from_model, view.clip_from_model, sizeof vu.clip_from_model);
    vu.eye_pull[0] = view.eye_model[0]; vu.eye_pull[1] = view.eye_model[1]; vu.eye_pull[2] = view.eye_model[2]; vu.eye_pull[3] = view.decal_pull;
    for (int k = 0; k < 3; k++) vu.eye_view[k] = view.eye_view[k];
    vu.layer = view.layer;
    for (int k = 0; k < 4; k++) { vu.wclip_lo[k] = view.wclip_lo[k]; vu.wclip_hi[k] = view.wclip_hi[k]; }
    vu.interp[0] = view.interp_alpha; vu.interp[1] = (view.interp_on && r.pb) ? 1.f : 0.f;
    VkBuffer ub = VK_NULL_HANDLE, lb = VK_NULL_HANDLE;
    const VkDeviceSize uoff = stage(&vu, sizeof vu, &ub);
    const VkDeviceSize loff = stage(&r.light, sizeof r.light, &lb);
    VkDescriptorSet set0 = alloc_set(pass_layout_);
    if (!set0) return;
    VkDescriptorBufferInfo bufs[4] = {{ub, uoff, sizeof(ViewU)}, {lb, loff, sizeof(LightU)},
                                      {r.nb ? r.nb : r.vb, r.nb ? r.noff : r.voff, r.nb ? r.nsize : r.vsize},
                                      {r.pb ? r.pb : r.vb, r.pb ? r.poff : r.voff, r.pb ? r.psize : r.vsize}};
    const VkImageLayout RO = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo imgs[6] = {{compare_, r.shadow->view, RO}, {linear_clamp_, r.ao->view, RO}, {linear_clamp_, r.env->view, RO},
                                     {linear_clamp_, r.under->view, RO}, {linear_clamp_, r.env_prev->view, RO}, {nearest_, r.foot->view, RO}};
    VkWriteDescriptorSet w[10];
    for (int i = 0; i < 10; i++) {
        w[i] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = set0; w[i].dstBinding = (uint32_t)i; w[i].descriptorCount = 1;
        if (i < 2) { w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[i].pBufferInfo = &bufs[i]; }
        else if (i < 8) { w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[i].pImageInfo = &imgs[i - 2]; }
        else { w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[i].pBufferInfo = &bufs[i - 6]; }
    }
    vkUpdateDescriptorSets(dev_, 10, w, 0, nullptr);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, main_pl_, 0, 1, &set0, 0, nullptr);
    const VkDeviceSize vbo = r.voff;
    vkCmdBindVertexBuffers(cb, 0, 1, &r.vb, &vbo);
    VkPipeline bound = VK_NULL_HANDLE;
    const uint32_t nd = (uint32_t)r.draws.size();
    for (uint32_t i = 0; i < nd; i++) {
        const DrawPacket &d = r.draws[i];
        if (d.vertex_count == 0 || d.first_vertex + d.vertex_count > r.nv) continue;
        VkPipeline ps = pipeline(d, t.samples, r.pass.depth, nullptr, true);
        if (!ps) continue;
        if (ps != bound) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, ps); bound = ps; }
        const DepthState ds = depth_state(d, r.pass.depth);
        if (eds_) {
            set_cull_(cb, d.cull == FC_CULL_BACK ? VK_CULL_MODE_BACK_BIT : d.cull == FC_CULL_FRONT ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE);
            set_front_(cb, VK_FRONT_FACE_COUNTER_CLOCKWISE);
            set_dtest_(cb, ds.test); set_dwrite_(cb, ds.write); set_dcompare_(cb, ds.compare);
            set_stest_(cb, ds.stencil);
            if (ds.stencil) set_sop_(cb, VK_STENCIL_FACE_FRONT_AND_BACK, ds.st.failOp, ds.st.passOp, ds.st.depthFailOp, ds.st.compareOp);
        }
        vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, ds.stencil ? ds.st.reference : 0u);
        vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, ds.stencil ? ds.st.compareMask : 0xFF);
        vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, ds.stencil ? ds.st.writeMask : 0);
        vkCmdSetDepthBias(cb, 0.f, 0.f, r.pass.depth && d.decal ? view.decal_slope : 0.f);
        DrawU du{};
        std::memcpy(du.prim, d.prim, sizeof du.prim); std::memcpy(du.env, d.env, sizeof du.env);
        du.lod = d.lod_frac; du.decal = d.decal ? 1.f : 0.f; du.noise_seed = view.noise_seed; du.pull = d.pull;
        du.fade = d.fade; du.talpha = d.trans_alpha; std::memcpy(du.clip, d.clip, sizeof du.clip); du.clip_on = d.clip_on ? 1.f : 0.f;
        du.lflags = d.light_flags | (r.nb ? 256u : 0u) | ((d.light_role & FC_LR_SELF_LIT) ? 512u : 0u) | ((d.light_role & FC_LR_MIRROR) ? 1024u : 0u) |
                    ((d.light_role & FC_LR_WATER) ? 2048u : 0u) | ((d.light_role & FC_LR_LAVA) ? 8192u : 0u) | (d.ink ? 4096u : 0u);
        VkBuffer db = VK_NULL_HANDLE;
        const VkDeviceSize doff = stage(&du, sizeof du, &db);
        VkDescriptorSet set1 = alloc_set(draw_layout_);
        if (!set1) continue;
        VkDescriptorBufferInfo dbi{db, doff, sizeof(DrawU)};
        VkDescriptorImageInfo ti[2];
        for (int k = 0; k < 2; k++) {
            const ImagePtr &x = r.tex[2 * i + k];
            ti[k] = VkDescriptorImageInfo{sampler(d.samp[k]), x ? x->view : white_->view, RO};
        }
        VkWriteDescriptorSet dw[3] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
                                      {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        for (int k = 0; k < 3; k++) { dw[k].dstSet = set1; dw[k].dstBinding = (uint32_t)k; dw[k].descriptorCount = 1; }
        dw[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; dw[0].pBufferInfo = &dbi;
        dw[1].descriptorType = dw[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        dw[1].pImageInfo = &ti[0]; dw[2].pImageInfo = &ti[1];
        vkUpdateDescriptorSets(dev_, 3, dw, 0, nullptr);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, main_pl_, 1, 1, &set1, 0, nullptr);
        vkCmdDraw(cb, d.vertex_count, 1, d.first_vertex, 0);
        draws_++;
    }
}

// ---------------------------------------------------------------------------------------------------------- fixed passes
bool VulkanDevice::depth_pipelines() {
    if (depth_tried_) return depth_pipe_[0][0] && depth_pipe_[1][0];
    depth_tried_ = true;
    std::string why;
    VkShaderModule vs = shader(0, glsl_depth_vertex(), &why);
    VkShaderModule fs = vs ? shader(1, glsl_depth_cut_fragment(), &why) : VK_NULL_HANDLE;
    if (!vs || !fs) { std::fprintf(stderr, "AR_VK depth shader: %s\n", why.c_str()); return false; }
    for (int cut = 0; cut < 2; cut++)
        for (int clamp = 0; clamp < 2; clamp++) {
            if (clamp && !depth_clamp_) { depth_pipe_[cut][1] = depth_pipe_[cut][0]; continue; }
            VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                     {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
            st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
            st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
            VkVertexInputBindingDescription vb{0, sizeof(fc_vertex), VK_VERTEX_INPUT_RATE_VERTEX};
            VkVertexInputAttributeDescription va[2] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32_SFLOAT, 12}};
            VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb;
            vi.vertexAttributeDescriptionCount = 2; vi.pVertexAttributeDescriptions = va;
            VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            vp.viewportCount = 1; vp.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rs.depthBiasEnable = VK_TRUE; rs.lineWidth = 1.f;
            rs.depthClampEnable = clamp ? VK_TRUE : VK_FALSE;
            VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE; ds.depthCompareOp = VK_COMPARE_OP_LESS;
            VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS};
            VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            dy.dynamicStateCount = 3; dy.pDynamicStates = dyn;
            VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            pi.stageCount = cut ? 2 : 1; pi.pStages = st;
            pi.pVertexInputState = &vi; pi.pInputAssemblyState = &ia; pi.pViewportState = &vp; pi.pRasterizationState = &rs;
            pi.pMultisampleState = &ms; pi.pDepthStencilState = &ds; pi.pColorBlendState = &cb; pi.pDynamicState = &dy;
            pi.layout = depth_pl_; pi.renderPass = depth_pass();
            if (vkCreateGraphicsPipelines(dev_, pcache_, 1, &pi, nullptr, &depth_pipe_[cut][clamp]) != VK_SUCCESS)
                std::fprintf(stderr, "AR_VK depth pipeline failed\n");
        }
    return depth_pipe_[0][0] && depth_pipe_[1][0];
}
ImagePtr VulkanDevice::make_depth_map(uint32_t size) {
    return make_image(VK_IMAGE_TYPE_2D, VK_FORMAT_D32_SFLOAT, size, size, 1, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                      VK_IMAGE_ASPECT_DEPTH_BIT, false, false);
}
void VulkanDevice::encode_depth_map(Image &map, const float m[16], const fc_vertex *verts, uint32_t nv, const DrawPacket *draws, uint32_t nd,
                                    float slope_bias, bool clamp_depth, const DepthExtras &x) {
    VkCommandBuffer cb = slot().cmd;
    VkBuffer vb = VK_NULL_HANDLE, pb = VK_NULL_HANDLE;
    VkDeviceSize voff = 0, poff = 0;
    const VkDeviceSize vsize = (VkDeviceSize)nv * sizeof(fc_vertex);
    if (nv) voff = stage(verts, (size_t)vsize, &vb);
    const bool interp = vb && x.prev_pos && x.interp_alpha < 1.f;
    if (interp) poff = stage(x.prev_pos, (size_t)nv * 12, &pb);
    transition(cb, map, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    VkFramebuffer fb = framebuffer(depth_pass(), {map.view}, map.w, map.h);
    VkClearValue cv{};
    cv.depthStencil = {1.f, 0};
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = depth_pass(); bi.framebuffer = fb; bi.renderArea = {{0, 0}, {map.w, map.h}};
    bi.clearValueCount = 1; bi.pClearValues = &cv;
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    if (vb && nd) {
        const VkViewport vp{0.f, (float)map.h, (float)map.w, -(float)map.h, 0.f, 1.f};
        vkCmdSetViewport(cb, 0, 1, &vp);
        const VkRect2D sc{{0, 0}, {map.w, map.h}};
        vkCmdSetScissor(cb, 0, 1, &sc);
        vkCmdSetDepthBias(cb, 0.f, 0.f, slope_bias);
        DepthPC pc{};
        std::memcpy(pc.m, m, sizeof pc.m);
        pc.interp[0] = x.interp_alpha; pc.interp[1] = pb ? 1.f : 0.f;
        std::memcpy(pc.wclip_lo, x.wclip_lo, sizeof pc.wclip_lo);
        std::memcpy(pc.wclip_hi, x.wclip_hi, sizeof pc.wclip_hi);
        vkCmdPushConstants(cb, depth_pl_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        const VkDeviceSize o = voff;
        vkCmdBindVertexBuffers(cb, 0, 1, &vb, &o);
        VkDescriptorBufferInfo pbi{pb ? pb : vb, pb ? poff : voff, pb ? (VkDeviceSize)nv * 12 : vsize};
        VkPipeline bound = VK_NULL_HANDLE;
        VkDescriptorSet plain = VK_NULL_HANDLE;
        for (uint32_t i = 0; i < nd; i++) {
            const DrawPacket &d = draws[i];
            if (d.vertex_count == 0 || d.first_vertex + d.vertex_count > nv) continue;
            const bool cut = (d.cc.opts & FC_CC_TEXTURE_EDGE) && d.tex[0];
            VkPipeline p = depth_pipe_[cut ? 1 : 0][clamp_depth ? 1 : 0];
            if (p != bound) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p); bound = p; }
            VkDescriptorSet set = VK_NULL_HANDLE;
            if (cut || !plain) {
                set = alloc_set(depth_layout_);
                if (!set) continue;
                const ImagePtr tex = cut ? texture(d.tex[0]) : white_;
                if (tex->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) continue;   // a target being drawn: skip its cutout
                VkDescriptorImageInfo ii{cut ? sampler(d.samp[0], true) : linear_clamp_, tex->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
                w[0].dstSet = w[1].dstSet = set; w[0].descriptorCount = w[1].descriptorCount = 1;
                w[0].dstBinding = 0; w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[0].pBufferInfo = &pbi;
                w[1].dstBinding = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo = &ii;
                vkUpdateDescriptorSets(dev_, 2, w, 0, nullptr);
                if (!cut) plain = set;
                if (cut) slot().garbage.push_back([tex]() {});
            } else {
                set = plain;
            }
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, depth_pl_, 0, 1, &set, 0, nullptr);
            vkCmdDraw(cb, d.vertex_count, 1, d.first_vertex, 0);
        }
    }
    vkCmdEndRenderPass(cb);
    map.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    transition(cb, map, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    passes_++;
}
void VulkanDevice::encode_shadow_map(const ShadowFit &fit, const fc_vertex *verts, uint32_t nv, const DrawPacket *draws, uint32_t nd,
                                     const DepthExtras &x) {
    if (!recording_ || !depth_pipelines()) return;
    if (!shadow_) shadow_ = make_depth_map(kShadowSize);
    if (!shadow_) return;
    flush_sampling(shadow_);
    encode_depth_map(*shadow_, fit.clip_from_level, verts, nv, draws, nd, 2.f, true, x);
    shadow_fit_ = fit;
    shadow_valid_ = true;
}
void VulkanDevice::encode_underside_map(const UndersideDesc &u, const fc_vertex *verts, uint32_t nv, const DrawPacket *draws, uint32_t nd,
                                        const DepthExtras &x) {
    if (!recording_ || !depth_pipelines()) return;
    if (!under_) under_ = make_depth_map(kUndersideSize);
    if (!under_) return;
    flush_sampling(under_);
    encode_depth_map(*under_, u.clip_from_level, verts, nv, draws, nd, 0.f, false, x);
    under_desc_ = u;
    under_valid_ = true;
}
void VulkanDevice::encode_water_footprint(const float window[4], const float clip_from_level[16], const fc_vertex *verts, uint32_t nv,
                                          const DrawPacket *draws, uint32_t nd) {
    if (!recording_ || !depth_pipelines() || !(window[2] > window[0]) || !(window[3] > window[1])) return;
    if (!foot_) foot_ = make_depth_map(kFootprintSize);
    if (!foot_) return;
    flush_sampling(foot_);
    encode_depth_map(*foot_, clip_from_level, verts, nv, draws, nd, 0.f, false, DepthExtras{});
    std::memcpy(foot_window_, window, sizeof foot_window_);
    foot_valid_ = true;
    foot_kept_ = true;
}

// ---------------------------------------------------------------------------------------------------------- composite and resolve
VkPipeline VulkanDevice::composite_pipeline(VkFormat fmt) {
    VkPipeline &p = composite_[fmt];
    if (p) return p;
    std::string why;
    VkShaderModule vs = shader(0, glsl_fullscreen_vertex(), &why), fs = vs ? shader(1, glsl_composite_fragment(), &why) : VK_NULL_HANDLE;
    if (!vs || !fs) { std::fprintf(stderr, "AR_VK composite shader: %s\n", why.c_str()); return VK_NULL_HANDLE; }
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ca{};
    ca.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &ca;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 2; pi.pStages = st; pi.pVertexInputState = &vi; pi.pInputAssemblyState = &ia; pi.pViewportState = &vp;
    pi.pRasterizationState = &rs; pi.pMultisampleState = &ms; pi.pColorBlendState = &cb; pi.pDynamicState = &dy;
    pi.layout = pair_pl_; pi.renderPass = color_pass(fmt, false);
    if (vkCreateGraphicsPipelines(dev_, pcache_, 1, &pi, nullptr, &p) != VK_SUCCESS) std::fprintf(stderr, "AR_VK composite pipeline failed\n");
    return p;
}
void VulkanDevice::encode_composite(uint32_t dst, void *external, uint32_t under, uint32_t over) {
    if (!recording_ || under == 0 || over == 0 || under > targets_.size() || over > targets_.size()) return;
    Image *out = nullptr;
    VulkanExternalImage *ext = nullptr;
    if (dst) { if (dst > targets_.size() || !targets_[dst - 1].color) return; out = targets_[dst - 1].color.get(); }
    else { ext = static_cast<VulkanExternalImage *>(external); if (!ext || !ext->view) return; }
    flush(targets_[under - 1]); flush(targets_[over - 1]);
    if (dst) flush(targets_[dst - 1]);
    VkCommandBuffer cb = slot().cmd;
    Image &u = *targets_[under - 1].color, &o = *targets_[over - 1].color;
    transition(cb, u, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transition(cb, o, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const VkFormat fmt = out ? out->format : ext->format;
    const uint32_t w = out ? out->w : ext->width, h = out ? out->h : ext->height;
    VkPipeline p = composite_pipeline(fmt);
    if (!p) return;
    Image ext_img;
    if (ext) { ext_img.image = ext->image; ext_img.aspect = VK_IMAGE_ASPECT_COLOR_BIT; ext_img.layout = ext->layout; }
    Image &target = out ? *out : ext_img;
    transition(cb, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderPass rp = color_pass(fmt, false);
    VkFramebuffer fb = framebuffer(rp, {out ? out->view : ext->view}, w, h);
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = rp; bi.framebuffer = fb; bi.renderArea = {{0, 0}, {w, h}};
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport vp{0.f, 0.f, (float)w, (float)h, 0.f, 1.f};
    const VkRect2D sc{{0, 0}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    VkDescriptorSet set = alloc_set(pair_layout_);
    VkDescriptorImageInfo ii[2] = {{nearest_, u.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {nearest_, o.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet wr[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    for (int k = 0; k < 2; k++) {
        wr[k].dstSet = set; wr[k].dstBinding = (uint32_t)k; wr[k].descriptorCount = 1;
        wr[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[k].pImageInfo = &ii[k];
    }
    vkUpdateDescriptorSets(dev_, 2, wr, 0, nullptr);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pair_pl_, 0, 1, &set, 0, nullptr);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    target.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    if (ext) transition(cb, target, ext->layout);
    passes_++;
}
VkPipeline VulkanDevice::resolve_pipeline(VkFormat cf, VkFormat df) {
    VkPipeline &p = resolve_[{cf, df}];
    if (p) return p;
    const bool depth = df != VK_FORMAT_UNDEFINED;
    // the source view's pixels 1:1 (src_origin + (frag - dst_origin), clamped to the view), optionally linearized, with
    // its depth when the destination has one (Metal's rfrag / rfrag_color)
    const std::string fsrc = std::string(R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1) uniform sampler2D srcd;
layout(push_constant, std430) uniform RU { vec2 dst_origin; uvec2 src_origin; uvec2 src_last; uint linearize; uint src_layer; } u;
layout(location = 0) out vec4 o_color;
void main() {
    const uvec2 sz = uvec2(textureSize(src, 0));
    const ivec2 p = ivec2(min(uvec2(gl_FragCoord.xy - u.dst_origin) + u.src_origin, min(u.src_last, sz - 1u)));
    vec4 c = texelFetch(src, p, 0);
    if (u.linearize != 0u && c.a > 0.0) {
        const vec3 g = clamp(c.rgb / c.a, 0.0, 1.0);
        c.rgb = mix(pow((g + 0.055) / 1.055, vec3(2.4)), g / 12.92, lessThanEqual(g, vec3(0.04045))) * c.a;
    }
    o_color = c;
)") + (depth ? "    gl_FragDepth = texelFetch(srcd, p, 0).r;\n" : "") + "}\n";
    std::string why;
    VkShaderModule vs = shader(0, glsl_fullscreen_vertex(), &why), fs = vs ? shader(1, fsrc, &why) : VK_NULL_HANDLE;
    if (!vs || !fs) { std::fprintf(stderr, "AR_VK resolve shader: %s\n", why.c_str()); return VK_NULL_HANDLE; }
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = depth; ds.depthWriteEnable = depth; ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    VkPipelineColorBlendAttachmentState ca{};
    ca.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &ca;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 2; pi.pStages = st; pi.pVertexInputState = &vi; pi.pInputAssemblyState = &ia; pi.pViewportState = &vp;
    pi.pRasterizationState = &rs; pi.pMultisampleState = &ms; pi.pDepthStencilState = &ds; pi.pColorBlendState = &cb; pi.pDynamicState = &dy;
    pi.layout = pair_pl_; pi.renderPass = color_pass(cf, true, df);
    if (vkCreateGraphicsPipelines(dev_, pcache_, 1, &pi, nullptr, &p) != VK_SUCCESS) std::fprintf(stderr, "AR_VK resolve pipeline failed\n");
    return p;
}
// The target's region into an external image (an OpenXR swapchain image): colour, and depth when dst.depth is given.
void VulkanDevice::encode_resolve(uint32_t src, const ExternalView &dst) {
    if (!recording_ || src == 0 || src > targets_.size() || !dst.color) return;
    Target &s = targets_[src - 1];
    flush(s);
    auto *ec = static_cast<VulkanExternalImage *>(dst.color);
    auto *ed = static_cast<VulkanExternalImage *>(dst.depth);
    VkPipeline p = resolve_pipeline(ec->format, ed ? ed->format : VK_FORMAT_UNDEFINED);
    if (!p) return;
    VkCommandBuffer cb = slot().cmd;
    transition(cb, *s.color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    // the target's depth through a depth-only view: its own view already is one
    transition(cb, *s.depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Image ci, di;
    ci.image = ec->image; ci.aspect = VK_IMAGE_ASPECT_COLOR_BIT; ci.layout = ec->layout;
    transition(cb, ci, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    std::vector<VkImageView> views = {ec->view};
    if (ed) {
        di.image = ed->image; di.layout = ed->layout;
        di.aspect = VK_IMAGE_ASPECT_DEPTH_BIT | (ed->format == VK_FORMAT_D32_SFLOAT_S8_UINT || ed->format == VK_FORMAT_D24_UNORM_S8_UINT
                                                 ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        transition(cb, di, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        views.push_back(ed->view);
    }
    VkRenderPass rp = color_pass(ec->format, true, ed ? ed->format : VK_FORMAT_UNDEFINED);
    VkFramebuffer fb = framebuffer(rp, views, ec->width, ec->height);
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = rp; bi.framebuffer = fb; bi.renderArea = {{0, 0}, {ec->width, ec->height}};
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport vp{dst.viewport.x, dst.viewport.y, dst.viewport.w, dst.viewport.h, 0.f, 1.f};
    vkCmdSetViewport(cb, 0, 1, &vp);
    const VkRect2D sc{{(int32_t)std::max(0.f, dst.viewport.x), (int32_t)std::max(0.f, dst.viewport.y)},
                      {(uint32_t)std::ceil(dst.viewport.w), (uint32_t)std::ceil(dst.viewport.h)}};
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    VkDescriptorSet set = alloc_set(pair_layout_);
    VkDescriptorImageInfo ii[2] = {{nearest_, s.color->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                   {nearest_, s.depth->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet wr[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    for (int k = 0; k < 2; k++) {
        wr[k].dstSet = set; wr[k].dstBinding = (uint32_t)k; wr[k].descriptorCount = 1;
        wr[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[k].pImageInfo = &ii[k];
    }
    vkUpdateDescriptorSets(dev_, 2, wr, 0, nullptr);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pair_pl_, 0, 1, &set, 0, nullptr);
    const uint32_t vw = std::max(1u, (uint32_t)std::ceil(dst.viewport.w)), vh = std::max(1u, (uint32_t)std::ceil(dst.viewport.h));
    struct { float dst_origin[2]; uint32_t src_origin[2]; uint32_t src_last[2]; uint32_t linearize; uint32_t src_layer; } u = {
        {dst.viewport.x, dst.viewport.y}, {dst.src_x, dst.src_y}, {dst.src_x + vw - 1, dst.src_y + vh - 1}, dst.linearize ? 1u : 0u, 0u};
    vkCmdPushConstants(cb, pair_pl_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof u, &u);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    ci.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    transition(cb, ci, ec->layout);
    if (ed) { di.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; transition(cb, di, ed->layout); }
    passes_++;
}

// ---------------------------------------------------------------------------------------------------------- read-back
bool VulkanDevice::read_target(uint32_t t, std::vector<uint8_t> &rgba, uint32_t &w, uint32_t &h) {
    if (t == 0 || t > targets_.size() || recording_) return false;
    Target &x = targets_[t - 1];
    if (!x.color) return false;
    w = x.w; h = x.h;
    if (x.color->layout == VK_IMAGE_LAYOUT_UNDEFINED) { rgba.assign((size_t)w * h * 4, 0); return true; }
    return read_image(*x.color, VK_IMAGE_ASPECT_COLOR_BIT, rgba);
}
bool VulkanDevice::read_target_depth(uint32_t t, std::vector<float> &depth, uint32_t &w, uint32_t &h) {
    if (t == 0 || t > targets_.size() || recording_) return false;
    Target &x = targets_[t - 1];
    if (!x.depth) return false;
    w = x.w; h = x.h;
    if (x.depth->layout == VK_IMAGE_LAYOUT_UNDEFINED) { depth.assign((size_t)w * h, 0.f); return true; }
    std::vector<uint8_t> raw;
    if (!read_image(*x.depth, VK_IMAGE_ASPECT_DEPTH_BIT, raw)) return false;
    depth.resize((size_t)w * h);
    if (x.depth->format == VK_FORMAT_D24_UNORM_S8_UINT) {
        for (size_t i = 0; i < depth.size(); i++) {
            uint32_t v;
            std::memcpy(&v, &raw[4 * i], 4);
            depth[i] = (float)(v & 0xFFFFFFu) / 16777215.f;
        }
    } else {
        std::memcpy(depth.data(), raw.data(), depth.size() * 4);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------- factories
std::unique_ptr<IRenderDevice> create_vulkan_device(const VulkanDeviceOptions &opt, std::string *error) {
    std::unique_ptr<VulkanDevice> d(new VulkanDevice());
    std::string err;
    if (!d->init_headless(opt, &err)) { if (error) *error = err; return nullptr; }
    return std::unique_ptr<IRenderDevice>(d.release());
}
std::unique_ptr<IRenderDevice> create_vulkan_device_on(const VulkanExternalDevice &ext, const VulkanDeviceOptions &opt, std::string *error) {
    std::unique_ptr<VulkanDevice> d(new VulkanDevice());
    std::string err;
    if (!d->init_external(ext, opt, &err)) { if (error) *error = err; return nullptr; }
    return std::unique_ptr<IRenderDevice>(d.release());
}
VulkanDeviceInfo vulkan_device_info(const IRenderDevice &dev) { return static_cast<const VulkanDevice &>(dev).info(); }
bool vulkan_device_save_pipeline_cache(IRenderDevice &dev) { return static_cast<VulkanDevice &>(dev).save_cache(); }
}

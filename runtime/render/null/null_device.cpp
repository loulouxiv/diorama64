#include "null_device.h"
#include <map>
#include <set>
#include <vector>
namespace ar {
class NullRenderDevice final : public IRenderDevice {
public:
    NullDeviceStats st;
    struct Target { uint32_t w = 1, h = 1, layers = 1, samples = 1; };
    std::map<uint32_t, Target> targets;
    std::set<uint32_t> textures;
    std::set<std::pair<uint64_t, uint32_t>> combiners;
    uint32_t next_target = 1;
    uint32_t frame_passes = 0, frame_draws = 0, frame_vertices = 0;

    void upload_texture(uint32_t id, uint32_t w, uint32_t h, const uint8_t *rgba) override {
        (void)rgba;
        st.texture_uploads++;
        st.texture_bytes += (uint64_t)w * h * 4;
        textures.insert(id);
        st.textures = (uint32_t)textures.size();
    }
    void set_edge_coverage(bool) override {}
    void set_texture_filter(TextureFilter) override {}
    uint32_t create_target(uint32_t w, uint32_t h, uint32_t layers, uint32_t samples) override {
        const uint32_t id = next_target++;
        targets[id] = Target{w ? w : 1, h ? h : 1, layers ? layers : 1, samples ? samples : 1};
        return id;
    }
    void resize_target(uint32_t t, uint32_t w, uint32_t h, uint32_t layers, uint32_t samples) override {
        targets[t] = Target{w ? w : 1, h ? h : 1, layers ? layers : 1, samples ? samples : 1};
    }
    uint32_t supported_samples(uint32_t wanted, uint32_t) const override { return wanted ? wanted : 1; }
    uint32_t target_samples(uint32_t t) const override { auto it = targets.find(t); return it == targets.end() ? 1 : it->second.samples; }
    void bind_target_texture(uint32_t, uint32_t) override {}
    void release_target(uint32_t t) override { targets.erase(t); }
    bool memoryless_msaa() const override { return false; }
    void set_rate_map(uint32_t, void *) override {}
    void begin_frame() override { frame_passes = frame_draws = frame_vertices = 0; }
    void encode_pass(uint32_t, const PassDesc &, const ViewConstants &, const fc_vertex *, uint32_t vert_count,
                     const DrawPacket *draws, uint32_t draw_count, const int8_t *, const float *) override {
        frame_passes++; st.passes++;
        frame_draws += draw_count; st.draws += draw_count;
        uint64_t v = 0;
        for (uint32_t i = 0; i < draw_count; i++) v += draws[i].vertex_count;
        (void)vert_count;
        frame_vertices += (uint32_t)v; st.vertices += v;
    }
    void set_lighting(const LightingFrame *) override {}
    void encode_underside_map(const UndersideDesc &, const fc_vertex *, uint32_t, const DrawPacket *, uint32_t, const DepthExtras &) override {
        st.underside_maps++;
    }
    void encode_shadow_map(const ShadowFit &, const fc_vertex *, uint32_t, const DrawPacket *, uint32_t, const DepthExtras &) override {
        st.shadow_maps++;
    }
    bool bake_ao(uint32_t, const AoVolume &, const std::vector<ShadowFit> &, float, const fc_vertex *, uint32_t, const DrawPacket *,
                 uint32_t, const DepthExtras &) override { return false; }
    void encode_environment_reduce(void *, uint32_t) override {}
    bool environment_stats(EnvStats *, uint32_t *) const override { return false; }
    void encode_composite(uint32_t, void *, uint32_t, uint32_t) override { st.composites++; }
    void end_frame(void (*on_gpu_done)(void *), void *ctx, void *) override {
        st.frames++;
        st.last_passes = frame_passes; st.last_draws = frame_draws; st.last_vertices = frame_vertices;
        if (on_gpu_done) on_gpu_done(ctx);   // nothing in flight: "GPU done" at once, on the calling thread
    }
    void wait_idle() override {}
    double last_gpu_ms() const override { return 0.0; }
    uint32_t render_encoders_last_frame() const override { return frame_passes; }
    uint64_t late_passes() const override { return 0; }
    bool read_target(uint32_t t, std::vector<uint8_t> &rgba, uint32_t &w, uint32_t &h) override {
        auto it = targets.find(t);
        if (it == targets.end()) return false;
        w = it->second.w; h = it->second.h;
        rgba.assign((size_t)w * h * 4, 0);
        return true;
    }
    bool compile_combiner(const fc_combiner_key &key, std::string *, uint32_t samples) override {
        combiners.insert({key.slots ^ ((uint64_t)key.opts << 32), samples});
        return true;
    }
    void encode_resolve(uint32_t, const ExternalView &) override { st.resolves++; }
    void *command_buffer() override { return nullptr; }
    bool read_target_depth(uint32_t t, std::vector<float> &depth, uint32_t &w, uint32_t &h) override {
        auto it = targets.find(t);
        if (it == targets.end()) return false;
        w = it->second.w; h = it->second.h;
        depth.assign((size_t)w * h, 0.f);   // reverse-Z: 0 is the far plane (nothing drawn)
        return true;
    }
    std::string pipeline_list() const override { return std::string(); }
    size_t warm_pipelines(const std::string &, uint32_t) override { return 0; }
    size_t warm_pipelines_background(const std::string &, const std::vector<DrawPacket> &, uint32_t) override { return 0; }
    size_t pipelines_warming() override { return 0; }
    size_t draw_time_builds() const override { return 0; }
    bool take_pipeline_error(std::string *) override { return false; }
    size_t pipeline_count() const override { return combiners.size(); }
};
std::unique_ptr<IRenderDevice> create_null_device() { return std::unique_ptr<IRenderDevice>(new NullRenderDevice()); }
NullDeviceStats null_device_stats(const IRenderDevice &dev) { return static_cast<const NullRenderDevice &>(dev).st; }
}

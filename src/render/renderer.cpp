#include "render/renderer.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstring>

#include <glm/gtc/matrix_inverse.hpp>

#include "core/log.hpp"
#include "core/timing.hpp"
#include "render/font.hpp"
#include "render/shader_program.hpp"

namespace ftv {

namespace {

struct GpuEdge {
  std::uint32_t src;
  std::uint32_t dst;
  std::uint32_t attr;  // bits 0-7 flags, 16-23 source hub, 24-31 target hub
  float weight;        // shading weight [0, 1] (see upload_static_data)
};
static_assert(sizeof(GpuEdge) == 16);

struct GpuHub {
  float anchor_radius[4];
  float geo[4];
  float east[4];
  float north[4];
};
static_assert(sizeof(GpuHub) == 64);

struct GpuLabelGlyph {
  std::uint32_t hub, code, column, place;  // place: label side | length << 8
};
static_assert(sizeof(GpuLabelGlyph) == 16);

struct GpuWhale {
  std::uint32_t nodes[4];  // src, dst, src hub, dst hub
  float timing[4];         // start (s), tier
};
static_assert(sizeof(GpuWhale) == 32);

#pragma pack(push, 1)
struct NodeStatic {
  std::uint16_t hub;
  std::uint16_t flags;
  float activity;  // log-normalised weighted degree [0, 1]
};
#pragma pack(pop)
static_assert(sizeof(NodeStatic) == 8);

}  // namespace

Renderer::Renderer(const Graph& graph, const HubTaxonomy& taxonomy, const WhaleSet& whales,
                   const RenderSettings& settings, const GlCaps& caps, std::string shaderDir)
    : graph_(graph),
      taxonomy_(taxonomy),
      whales_(whales),
      settings_(settings),
      caps_(caps),
      shader_dir_(std::move(shaderDir)),
      node_count_(graph.node_count()) {
  hub_frames_.reserve(taxonomy_.size());
  for (const HubDef& h : taxonomy_.hubs()) hub_frames_.push_back(hub_frame(h));
}

Renderer::~Renderer() = default;

bool Renderer::create_programs(std::string& error) {
  auto make = [&](gl::Program& out, std::initializer_list<ShaderStage> stages) {
    auto p = build_program(shader_dir_, stages, error);
    if (!p) return false;
    out = std::move(*p);
    return true;
  };
  const ShaderStage fsv{GL_VERTEX_SHADER, "fullscreen.vert"};
  if (!make(prog_globe_, {fsv, {GL_FRAGMENT_SHADER, "globe.frag"}})) return false;
  if (!make(prog_node_, {{GL_VERTEX_SHADER, "node.vert"}, {GL_FRAGMENT_SHADER, "node.frag"}})) return false;
  if (!make(prog_arc_, {{GL_VERTEX_SHADER, "arc.vert"}, {GL_FRAGMENT_SHADER, "arc.frag"}})) return false;
  if (!make(prog_whale_, {{GL_VERTEX_SHADER, "whale.vert"}, {GL_FRAGMENT_SHADER, "whale.frag"}})) return false;
  if (!make(prog_hub_, {{GL_VERTEX_SHADER, "hub.vert"}, {GL_FRAGMENT_SHADER, "hub.frag"}})) return false;
  if (!make(prog_pick_, {{GL_VERTEX_SHADER, "pick.vert"}, {GL_FRAGMENT_SHADER, "pick.frag"}})) return false;
  if (!make(prog_hud_, {{GL_VERTEX_SHADER, "hud.vert"}, {GL_FRAGMENT_SHADER, "hud.frag"}})) return false;
  if (!make(prog_down_, {fsv, {GL_FRAGMENT_SHADER, "bloom_down.frag"}})) return false;
  if (!make(prog_up_, {fsv, {GL_FRAGMENT_SHADER, "bloom_up.frag"}})) return false;
  if (!make(prog_composite_, {fsv, {GL_FRAGMENT_SHADER, "composite.frag"}})) return false;
  if (!make(prog_present_, {fsv, {GL_FRAGMENT_SHADER, "present.frag"}})) return false;
  loc_arc_inter_ = glGetUniformLocation(prog_arc_.id(), "u_inter");
  loc_arc_first_ = glGetUniformLocation(prog_arc_.id(), "u_first");
  loc_arc_segments_ = glGetUniformLocation(prog_arc_.id(), "u_segments");
  loc_whale_segments_ = glGetUniformLocation(prog_whale_.id(), "u_segments");
  loc_down_texel_ = glGetUniformLocation(prog_down_.id(), "u_texel");
  loc_up_texel_ = glGetUniformLocation(prog_up_.id(), "u_texel");
  loc_comp_bg_ = glGetUniformLocation(prog_composite_.id(), "u_background");
  loc_comp_exposure_ = glGetUniformLocation(prog_composite_.id(), "u_exposure");
  loc_comp_bloom_ = glGetUniformLocation(prog_composite_.id(), "u_bloom_strength");
  loc_comp_vignette_ = glGetUniformLocation(prog_composite_.id(), "u_vignette");
  loc_present_encode_ = glGetUniformLocation(prog_present_.id(), "u_encode");
  return true;
}

void Renderer::upload_static_data() {
  const std::size_t N = node_count_;

  // ---- per-node static attributes (immutable VBO, instanced) ---------------
  {
    float maxDeg = 0.0f;
    for (std::size_t i = 0; i < N; ++i) maxDeg = std::max(maxDeg, graph_.nodes.weighted_degree[i]);
    const float norm = maxDeg > 0.0f ? 1.0f / std::log1p(maxDeg) : 0.0f;
    std::vector<NodeStatic> st(std::max<std::size_t>(N, 1));
    for (std::size_t i = 0; i < N; ++i) {
      st[i] = NodeStatic{graph_.nodes.hub[i], graph_.nodes.flags[i],
                         std::log1p(std::max(graph_.nodes.weighted_degree[i], 0.0f)) * norm};
    }
    node_static_ = gl::create_buffer();
    glNamedBufferStorage(node_static_.id(), static_cast<GLsizeiptr>(st.size() * sizeof(NodeStatic)), st.data(), 0);
    vao_nodes_ = gl::create_vertex_array();
    const GLuint vao = vao_nodes_.id();
    glVertexArrayVertexBuffer(vao, 0, node_static_.id(), 0, sizeof(NodeStatic));
    glVertexArrayBindingDivisor(vao, 0, 1);  // one element per instance
    glVertexArrayAttribIFormat(vao, 0, 1, GL_UNSIGNED_SHORT, static_cast<GLuint>(offsetof(NodeStatic, hub)));
    glVertexArrayAttribIFormat(vao, 1, 1, GL_UNSIGNED_SHORT, static_cast<GLuint>(offsetof(NodeStatic, flags)));
    glVertexArrayAttribFormat(vao, 2, 1, GL_FLOAT, GL_FALSE, static_cast<GLuint>(offsetof(NodeStatic, activity)));
    for (GLuint a = 0; a < 3; ++a) {
      glEnableVertexArrayAttrib(vao, a);
      glVertexArrayAttribBinding(vao, a, 0);
    }
  }
  vao_empty_ = gl::create_vertex_array();

  // ---- edges: validated, intra first then inter, each by priority ----------
  {
    const EdgeList& e = graph_.edges;
    // Priority = weight relative to the largest edge of the same currency (no
    // FX conversion), computed once per edge so the sorts compare doubles.
    const std::vector<std::uint32_t>& cur = graph_.currencies;  // sorted, distinct
    std::vector<std::uint32_t> currencyRank(e.size());
    std::vector<double> maxPerCurrency(cur.size() + 1, 0.0);
    for (std::size_t i = 0; i < e.size(); ++i) {
      const auto it = std::lower_bound(cur.begin(), cur.end(), e.currency[i]);
      const auto r = static_cast<std::uint32_t>(
          (it != cur.end() && *it == e.currency[i]) ? static_cast<std::size_t>(it - cur.begin()) : cur.size());
      currencyRank[i] = r;
      maxPerCurrency[r] = std::max(maxPerCurrency[r], std::fabs(static_cast<double>(e.weight[i])));
    }
    std::vector<double> priority(e.size());
    for (std::size_t i = 0; i < e.size(); ++i) {
      const double m = maxPerCurrency[currencyRank[i]];
      priority[i] = m > 0 ? std::fabs(static_cast<double>(e.weight[i])) / m : 0.0;
    }
    std::vector<std::uint32_t> intra, inter;
    std::uint64_t invalid = 0;
    for (std::size_t i = 0; i < e.size(); ++i) {
      if (e.src[i] >= N || e.dst[i] >= N) {
        ++invalid;  // never upload an index that could read out of bounds
        continue;
      }
      if (e.flags[i] & kEdgeSelfLoop) continue;
      ((e.flags[i] & kEdgeInterHub) ? inter : intra).push_back(static_cast<std::uint32_t>(i));
    }
    if (invalid) FTV_LOG_WARN("renderer skipped %llu edges with invalid indices", static_cast<unsigned long long>(invalid));
    // Descending priority; ties by edge index, so the order is deterministic.
    auto byPrio = [&priority](std::uint32_t a, std::uint32_t b) {
      const double pa = priority[a], pb = priority[b];
      return pa != pb ? pa > pb : a < b;
    };
    std::sort(intra.begin(), intra.end(), byPrio);
    std::sort(inter.begin(), inter.end(), byPrio);
    // One arc per unordered node pair: A->B and B->A, and the same pair in
    // several currencies or channels, would draw the exact same curve and
    // stack additively. The heaviest edge of the pair (first in priority
    // order) stands for it.
    auto dedupe = [&e](std::vector<std::uint32_t>& list) {
      std::vector<std::uint64_t> seen;
      seen.reserve(list.size());
      for (std::uint32_t i : list) {
        const std::uint64_t a = std::min(e.src[i], e.dst[i]), b = std::max(e.src[i], e.dst[i]);
        seen.push_back(a << 32 | b);
      }
      std::vector<std::uint64_t> sorted = seen;
      std::sort(sorted.begin(), sorted.end());
      std::vector<std::uint8_t> taken(sorted.size(), 0);
      std::size_t out = 0;
      for (std::size_t k = 0; k < list.size(); ++k) {
        const auto pos = static_cast<std::size_t>(std::lower_bound(sorted.begin(), sorted.end(), seen[k]) - sorted.begin());
        if (taken[pos]) continue;  // equal keys share the first slot of their run
        taken[pos] = 1;
        list[out++] = list[k];
      }
      list.resize(out);
    };
    const std::size_t intraRaw = intra.size(), interRaw = inter.size();
    dedupe(intra);
    dedupe(inter);
    FTV_LOG_INFO("arcs: %zu -> %zu intra-hub, %zu -> %zu inter-hub after merging edges of the same node pair", intraRaw,
                 intra.size(), interRaw, inter.size());
    intra_count_ = static_cast<std::uint32_t>(intra.size());
    inter_count_ = static_cast<std::uint32_t>(inter.size());

    std::vector<GpuEdge> gpu;
    gpu.reserve(intra.size() + inter.size() + 1);
    // Shading weight: min-max normalised in log space within the currency,
    // squared, so the bulk of the edges sit at the bottom of the alpha range
    // and only the heaviest flows approach the top.
    std::vector<double> minLog(cur.size() + 1, 1e300), maxLog(cur.size() + 1, -1e300);
    for (std::size_t i = 0; i < e.size(); ++i) {
      const double l = std::log1p(std::fabs(static_cast<double>(e.weight[i])));
      minLog[currencyRank[i]] = std::min(minLog[currencyRank[i]], l);
      maxLog[currencyRank[i]] = std::max(maxLog[currencyRank[i]], l);
    }
    auto pack = [&](std::uint32_t i) {
      const std::uint32_t r = currencyRank[i];
      const double l = std::log1p(std::fabs(static_cast<double>(e.weight[i])));
      const double span = maxLog[r] - minLog[r];
      const double t = span > 0 ? (l - minLog[r]) / span : 0.0;
      const auto wn = static_cast<float>(t * t);
      const std::uint32_t hs = graph_.nodes.hub[e.src[i]] & 0xFFu;
      const std::uint32_t hd = graph_.nodes.hub[e.dst[i]] & 0xFFu;
      const std::uint32_t attr = (static_cast<std::uint32_t>(e.flags[i]) & 0xFFu) | (hs << 16) | (hd << 24);
      gpu.push_back(GpuEdge{e.src[i], e.dst[i], attr, wn});
    };
    for (auto i : intra) pack(i);
    for (auto i : inter) pack(i);
    if (gpu.empty()) gpu.push_back(GpuEdge{0, 0, 0, 0.0f});
    edges_ = gl::create_buffer();
    glNamedBufferStorage(edges_.id(), static_cast<GLsizeiptr>(gpu.size() * sizeof(GpuEdge)), gpu.data(), 0);

    // Edge budget (<= 2M drawn arcs per frame, heaviest first): inter-hub
    // arcs take up to max_inter_hub_edges, intra-hub chords up to
    // max_intra_hub_edges of what remains.
    const std::uint32_t budget = std::min(settings_.max_visible_edges, kMaxVisibleEdges);
    inter_lod_ = std::min({inter_count_, settings_.max_inter_hub_edges, budget});
    intra_lod_ = std::min({intra_count_, settings_.max_intra_hub_edges, budget - inter_lod_});
  }

  // ---- hubs and hub labels ---------------------------------------------------
  {
    std::vector<GpuHub> hubs;
    std::vector<GpuLabelGlyph> glyphs;
    for (std::size_t h = 0; h < taxonomy_.size(); ++h) {
      const HubDef& d = taxonomy_.hubs()[h];
      const HubFrame& f = hub_frames_[h];
      hubs.push_back(GpuHub{{d.anchor.x, d.anchor.y, d.anchor.z, d.radius},
                            {f.normal.x, f.normal.y, f.normal.z, f.footprint},
                            {f.east.x, f.east.y, f.east.z, 0.0f},
                            {f.north.x, f.north.y, f.north.z, 0.0f}});
      const std::string& name = d.display_name.empty() ? d.id : d.display_name;
      const std::size_t len = std::min<std::size_t>(name.size(), 32);
      const auto place = static_cast<std::uint32_t>(d.label_side | (len << 8));  // side | label length
      for (std::size_t c = 0; c < len; ++c) {
        const auto ch = static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(name[c])));
        if (ch <= 32 || ch >= 127) continue;  // spaces advance the column without a glyph
        glyphs.push_back(GpuLabelGlyph{static_cast<std::uint32_t>(h), ch, static_cast<std::uint32_t>(c), place});
      }
    }
    hubs_ = gl::create_buffer();
    glNamedBufferStorage(hubs_.id(), static_cast<GLsizeiptr>(hubs.size() * sizeof(GpuHub)), hubs.data(), 0);
    label_glyphs_ = static_cast<std::uint32_t>(glyphs.size());
    if (glyphs.empty()) glyphs.push_back(GpuLabelGlyph{});
    labels_ = gl::create_buffer();
    glNamedBufferStorage(labels_.id(), static_cast<GLsizeiptr>(glyphs.size() * sizeof(GpuLabelGlyph)), glyphs.data(), 0);

    const auto px = build_font_atlas();
    label_atlas_ = gl::create_texture(GL_TEXTURE_2D);
    glTextureStorage2D(label_atlas_.id(), 1, GL_R8, kGlyphCount * kCellW, kCellH);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTextureSubImage2D(label_atlas_.id(), 0, 0, 0, kGlyphCount * kCellW, kCellH, GL_RED, GL_UNSIGNED_BYTE, px.data());
    glTextureParameteri(label_atlas_.id(), GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(label_atlas_.id(), GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(label_atlas_.id(), GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(label_atlas_.id(), GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }

  // ---- whale events ------------------------------------------------------------
  {
    std::vector<GpuWhale> w;
    for (const WhaleEvent& ev : whales_.events) {
      if (ev.src >= N || ev.dst >= N) continue;
      w.push_back(GpuWhale{{ev.src, ev.dst, graph_.nodes.hub[ev.src], graph_.nodes.hub[ev.dst]},
                           {ev.start, static_cast<float>(ev.tier), 0.0f, 0.0f}});
    }
    whale_count_ = w.size();
    if (w.empty()) w.push_back(GpuWhale{});
    whales_buf_ = gl::create_buffer();
    glNamedBufferStorage(whales_buf_.id(), static_cast<GLsizeiptr>(w.size() * sizeof(GpuWhale)), w.data(), 0);
  }
}

bool Renderer::init(std::string& error) {
  Stopwatch initClock;
  if (!create_programs(error)) return false;
  const double programsMs = initClock.lap_ms();
  upload_static_data();
  const double staticMs = initClock.lap_ms();

  const std::size_t N = std::max<std::size_t>(node_count_, 1);
  pos_region_bytes_ = gl::align_up(static_cast<GLsizeiptr>(N * 3 * sizeof(float)), caps_.ssbo_alignment);
  ubo_region_bytes_ = gl::align_up(static_cast<GLsizeiptr>(sizeof(FrameUniforms)), caps_.ubo_alignment);
  const std::size_t words = (N + 31) / 32;
  hl_region_bytes_ = gl::align_up(static_cast<GLsizeiptr>(words * 4), caps_.ssbo_alignment);
  if (!positions_.create(pos_region_bytes_ * kSlots, GL_MAP_WRITE_BIT) ||
      !uniforms_.create(ubo_region_bytes_ * kSlots, GL_MAP_WRITE_BIT) ||
      !highlight_.create(hl_region_bytes_ * kSlots, GL_MAP_WRITE_BIT)) {
    error = "failed to create persistently mapped buffers";
    return false;
  }
  std::memset(positions_.data(), 0, static_cast<std::size_t>(positions_.size()));
  std::memset(highlight_.data(), 0, static_cast<std::size_t>(highlight_.size()));
  highlight_master_.assign(words, 0u);
  if (!picker_.create() || !hud_.create(caps_.ssbo_alignment)) {
    error = "failed to create picking / HUD buffers";
    return false;
  }
  timers_.create();

  // Is the default framebuffer's back buffer sRGB-encoded? (No window when
  // rendering offscreen: then nothing is presented and the answer is moot.)
  if (caps_.has_default_framebuffer) {
    GLint encoding = GL_LINEAR;
    glGetNamedFramebufferAttachmentParameteriv(0, GL_BACK_LEFT, GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &encoding);
    default_fb_srgb_ = encoding == GL_SRGB;
  }

  if (settings_.msaa_samples > caps_.max_samples) {
    FTV_LOG_WARN("MSAA: %d samples requested, the driver supports %d; using %d", settings_.msaa_samples,
                 caps_.max_samples, caps_.max_samples);
  }
  glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);  // reverse-Z needs [0,1] clip depth
  glDisable(GL_CULL_FACE);
  FTV_LOG_INFO("renderer: %zu nodes, %u intra + %u inter edges (budget %u / %u), %zu whale events, %u label glyphs; "
               "init %.0f ms (shaders %.0f, static data %.0f)",
               node_count_, intra_count_, inter_count_, intra_lod_, inter_lod_, whale_count_, label_glyphs_,
               programsMs + staticMs + initClock.lap_ms(), programsMs, staticMs);
  return true;
}

void Renderer::ensure_targets(int width, int height) {
  if (width == target_w_ && height == target_h_ && scene_fbo_) return;
  target_w_ = width;
  target_h_ = height;
  const int samples = std::clamp(settings_.msaa_samples, 1, static_cast<int>(caps_.max_samples));

  // Scene: colour + emissive MRT and depth, multisampled.
  scene_fbo_ = gl::create_framebuffer();
  const GLenum target = samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
  scene_color_ = gl::create_texture(target);
  scene_emissive_ = gl::create_texture(target);
  scene_depth_ = gl::create_texture(target);
  if (samples > 1) {
    glTextureStorage2DMultisample(scene_color_.id(), samples, GL_RGBA16F, width, height, GL_TRUE);
    glTextureStorage2DMultisample(scene_emissive_.id(), samples, GL_RGBA16F, width, height, GL_TRUE);
    glTextureStorage2DMultisample(scene_depth_.id(), samples, GL_DEPTH_COMPONENT32F, width, height, GL_TRUE);
  } else {
    glTextureStorage2D(scene_color_.id(), 1, GL_RGBA16F, width, height);
    glTextureStorage2D(scene_emissive_.id(), 1, GL_RGBA16F, width, height);
    glTextureStorage2D(scene_depth_.id(), 1, GL_DEPTH_COMPONENT32F, width, height);
  }
  glNamedFramebufferTexture(scene_fbo_.id(), GL_COLOR_ATTACHMENT0, scene_color_.id(), 0);
  glNamedFramebufferTexture(scene_fbo_.id(), GL_COLOR_ATTACHMENT1, scene_emissive_.id(), 0);
  glNamedFramebufferTexture(scene_fbo_.id(), GL_DEPTH_ATTACHMENT, scene_depth_.id(), 0);
  const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
  glNamedFramebufferDrawBuffers(scene_fbo_.id(), 2, bufs);

  auto make_target = [](gl::Framebuffer& fbo, gl::Texture& tex, GLenum fmt, int w, int h) {
    tex = gl::create_texture(GL_TEXTURE_2D);
    glTextureStorage2D(tex.id(), 1, fmt, w, h);
    glTextureParameteri(tex.id(), GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(tex.id(), GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(tex.id(), GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(tex.id(), GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    fbo = gl::create_framebuffer();
    glNamedFramebufferTexture(fbo.id(), GL_COLOR_ATTACHMENT0, tex.id(), 0);
    glNamedFramebufferDrawBuffer(fbo.id(), GL_COLOR_ATTACHMENT0);
  };
  make_target(resolve_color_fbo_, resolved_color_, GL_RGBA16F, width, height);
  make_target(resolve_emissive_fbo_, resolved_emissive_, GL_RGBA16F, width, height);
  int w = width, h = height;
  for (int l = 0; l < kBloomLevels; ++l) {
    w = std::max(1, w / 2);
    h = std::max(1, h / 2);
    level_w_[static_cast<std::size_t>(l)] = w;
    level_h_[static_cast<std::size_t>(l)] = h;
    make_target(down_fbo_[static_cast<std::size_t>(l)], down_tex_[static_cast<std::size_t>(l)], GL_RGBA16F, w, h);
    if (l < kBloomLevels - 1) {
      make_target(up_fbo_[static_cast<std::size_t>(l)], up_tex_[static_cast<std::size_t>(l)], GL_RGBA16F, w, h);
    }
  }
  make_target(output_fbo_, output_tex_, GL_SRGB8_ALPHA8, width, height);

  for (GLuint fbo : {scene_fbo_.id(), resolve_color_fbo_.id(), output_fbo_.id()}) {
    const GLenum status = glCheckNamedFramebufferStatus(fbo, GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) FTV_LOG_ERROR("framebuffer %u incomplete: 0x%x", fbo, status);
  }
}

void Renderer::begin_frame(TripleBuffer<PositionSnapshot>& snapshots) {
  slot_ = static_cast<unsigned>(frame_ % kSlots);
  // Synchronisation point: the GPU has finished frame (f - 3), so this slot's
  // UBO / highlight / HUD regions and its timer queries are free to reuse.
  Stopwatch sw;
  if (!fences_[slot_].wait(1'000'000'000ull)) FTV_LOG_WARN("frame fence wait timed out (GPU hang or reset?)");
  counters_.fence_wait_ms = static_cast<float>(sw.lap_ms());
  counters_.upload_ms = 0.0f;
  timers_.collect(slot_);

  // Layout snapshot upload, amortised over frames (kUploadBytesPerFrame).
  if (fill_region_ < 0 && snapshots.acquire_latest()) {
    // Fill a region not referenced by the two frames that may still be in
    // flight (3 regions, at most 2 busy => one is always free).
    fill_source_ = &snapshots.front();
    latest_ = fill_source_;
    const unsigned busyA = pos_region_of_slot_[(slot_ + 1) % kSlots];
    const unsigned busyB = pos_region_of_slot_[(slot_ + 2) % kSlots];
    unsigned r = 0;
    while (r == busyA || r == busyB) ++r;
    fill_region_ = static_cast<int>(r);
    fill_done_bytes_ = 0;
  }
  if (fill_region_ >= 0) {
    const std::size_t total = std::min(fill_source_->xyz.size(), node_count_ * 3) * sizeof(float);
    const std::size_t chunk = std::min(kUploadBytesPerFrame, total - fill_done_bytes_);
    std::memcpy(static_cast<char*>(positions_.data()) +
                    static_cast<std::size_t>(pos_region_bytes_) * static_cast<std::size_t>(fill_region_) +
                    fill_done_bytes_,
                reinterpret_cast<const char*>(fill_source_->xyz.data()) + fill_done_bytes_, chunk);
    fill_done_bytes_ += chunk;
    if (fill_done_bytes_ >= total) {
      pos_region_ = static_cast<unsigned>(fill_region_);
      fill_region_ = -1;
      counters_.snapshot_iteration = fill_source_->iteration;
      ++counters_.snapshots_uploaded;
    }
    counters_.upload_ms = static_cast<float>(sw.lap_ms());
  }
  pos_region_of_slot_[slot_] = pos_region_;
  hud_.begin(slot_);
}

void Renderer::prime_positions(const PositionSnapshot& snapshot) {
  // Only valid before the first frame of this renderer (no frame in flight).
  latest_ = &snapshot;
  const std::size_t bytes = std::min(snapshot.xyz.size(), node_count_ * 3) * sizeof(float);
  for (unsigned r = 0; r < kSlots; ++r) {
    std::memcpy(static_cast<char*>(positions_.data()) + static_cast<std::size_t>(pos_region_bytes_) * r,
                snapshot.xyz.data(), bytes);
  }
  counters_.snapshot_iteration = snapshot.iteration;
}

void Renderer::refresh_highlight_region(unsigned slot) noexcept {
  if (highlight_gen_of_slot_[slot] == highlight_gen_) return;
  std::memcpy(static_cast<char*>(highlight_.data()) + static_cast<std::size_t>(hl_region_bytes_) * slot,
              highlight_master_.data(), highlight_master_.size() * sizeof(std::uint32_t));
  highlight_gen_of_slot_[slot] = highlight_gen_;
}

void Renderer::set_selection(std::optional<std::uint32_t> node) {
  if (node && *node >= node_count_) node.reset();
  if (node == selected_) return;
  const auto& adj = graph_.adjacency;
  auto apply = [&](std::uint32_t n, bool set) {
    auto bit = [&](std::uint32_t i) {
      if (set) {
        highlight_master_[i >> 5] |= 1u << (i & 31u);
      } else {
        highlight_master_[i >> 5] &= ~(1u << (i & 31u));
      }
    };
    bit(n);
    for (std::uint32_t k = adj.offset[n]; k < adj.offset[n + 1]; ++k) bit(adj.neighbor[k]);
  };
  if (selected_) apply(*selected_, false);
  if (node) apply(*node, true);
  selected_ = node;
  ++highlight_gen_;
}

glm::vec3 Renderer::node_world_position(std::uint32_t node) const noexcept {
  if (node >= node_count_) return glm::vec3(0.0f);
  const std::uint16_t h = graph_.nodes.hub[node];
  const HubDef& hub = taxonomy_.hubs()[h];
  glm::vec3 p = hub.anchor;
  if (latest_ && static_cast<std::size_t>(node) * 3 + 2 < latest_->xyz.size()) {
    p = {latest_->xyz[node * 3u], latest_->xyz[node * 3u + 1], latest_->xyz[node * 3u + 2]};
  }
  return globe_position(p, hub, hub_frames_[h], settings_.globe);
}

void Renderer::fullscreen() noexcept {
  glDrawArrays(GL_TRIANGLES, 0, 3);
  ++counters_.post_draws;
}

void Renderer::draw_frame(const FrameParams& p) {
  const int w = p.width, h = p.height;
  ensure_targets(w, h);
  counters_.scene_draws = 0;
  counters_.post_draws = 0;
  const Theme& th = settings_.theme;
  const int segs = settings_.arc_segments;

  // Whale events in flight or flashing (HUD statistic; W <= a few hundred).
  {
    const float span = th.whale_flight_s + th.whale_flash_s;
    std::uint32_t active = 0;
    for (const WhaleEvent& ev : whales_.events) {
      const float age = p.time - ev.start;
      if (age > 0.0f && age < span) ++active;
    }
    counters_.active_whales = active;
  }

  // ---- per-frame uniforms (persistently mapped ring, no buffer updates) ----
  FrameUniforms u;
  u.view = p.view;
  u.proj = p.proj;
  u.view_proj = p.proj * p.view;
  u.inv_view_proj = glm::inverse(u.view_proj);
  u.camera = glm::vec4(p.camera_pos, p.time);
  u.viewport = glm::vec4(static_cast<float>(w), static_cast<float>(h), 1.0f / static_cast<float>(w),
                         1.0f / static_cast<float>(h));
  const GlobeParams& g = settings_.globe;
  u.globe = glm::vec4(g.radius, g.node_altitude, g.node_relief, g.arc_lift);
  u.ink = glm::vec4(th.ink[0], th.ink[1], th.ink[2], settings_.ui_scale);
  u.amber = glm::vec4(th.graticule[0], th.graticule[1], th.graticule[2], th.graticule_alpha);
  u.gold = glm::vec4(th.whale[0], th.whale[1], th.whale[2], th.graticule_spacing_deg * kDegToRad);
  u.alpha0 = glm::vec4(th.node_alpha_min, th.node_alpha_max, th.intra_arc_alpha_min, th.intra_arc_alpha_max);
  u.alpha1 = glm::vec4(th.inter_arc_alpha_min, th.inter_arc_alpha_max, th.hub_ring_alpha, th.label_alpha);
  u.shape0 = glm::vec4(th.node_min_px, th.node_max_px, th.hub_ring_px, th.hub_ring_radius_px);
  u.shape1 = glm::vec4(th.hub_dot_px, th.label_px, th.label_tracking_em, p.near_plane);
  u.whale0 = glm::vec4(th.whale_flight_s, th.whale_trail_s, th.whale_flash_s, th.whale_intensity_min);
  u.whale1 = glm::vec4(th.whale_intensity_max, th.whale_width_px[0], th.whale_width_px[1], th.whale_width_px[2]);
  u.counts = glm::uvec4(static_cast<std::uint32_t>(node_count_), selected_ ? *selected_ + 1u : 0u,
                        static_cast<std::uint32_t>(taxonomy_.size()), static_cast<std::uint32_t>(whale_count_));
  std::memcpy(static_cast<char*>(uniforms_.data()) + static_cast<std::size_t>(ubo_region_bytes_) * slot_, &u, sizeof(u));
  refresh_highlight_region(slot_);

  glBindBufferRange(GL_UNIFORM_BUFFER, 0, uniforms_.id(), ubo_region_bytes_ * slot_, sizeof(FrameUniforms));
  glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, positions_.id(), pos_region_bytes_ * pos_region_, pos_region_bytes_);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, edges_.id());
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, hubs_.id());
  glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 4, highlight_.id(), hl_region_bytes_ * slot_, hl_region_bytes_);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 9, labels_.id());
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 10, whales_buf_.id());

  // ---- scene target ------------------------------------------------------------
  glDisable(GL_FRAMEBUFFER_SRGB);
  glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo_.id());
  glViewport(0, 0, w, h);
  const GLfloat zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  const GLfloat depth0 = 0.0f;  // reverse-Z: 0 is the far plane
  glClearNamedFramebufferfv(scene_fbo_.id(), GL_COLOR, 0, zero);
  glClearNamedFramebufferfv(scene_fbo_.id(), GL_COLOR, 1, zero);
  glClearNamedFramebufferfv(scene_fbo_.id(), GL_DEPTH, 0, &depth0);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_GREATER);
  glEnable(GL_BLEND);
  glEnable(GL_MULTISAMPLE);
  glBindVertexArray(vao_empty_.id());
  // Only the whale pass may write the emissive attachment.
  glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

  // Pass 1 - globe depth + graticule (additive, premultiplied).
  timers_.begin(slot_, GpuPass::Globe);
  glDepthMask(GL_TRUE);
  glBlendFunc(GL_ONE, GL_ONE);
  glUseProgram(prog_globe_.id());
  glDrawArrays(GL_TRIANGLES, 0, 3);
  ++counters_.scene_draws;
  timers_.end();

  // Translucent passes test against the globe but do not write depth.
  glDepthMask(GL_FALSE);

  // Pass 2 - nodes (premultiplied "over": a dense cluster saturates at ink).
  timers_.begin(slot_, GpuPass::Nodes);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glUseProgram(prog_node_.id());
  glBindVertexArray(vao_nodes_.id());
  glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(node_count_));
  ++counters_.scene_draws;
  glBindVertexArray(vao_empty_.id());
  timers_.end();

  // Pass 3 - arcs (additive: density reads as brightness).
  timers_.begin(slot_, GpuPass::Arcs);
  glBlendFunc(GL_ONE, GL_ONE);
  glUseProgram(prog_arc_.id());
  const float altitude = glm::length(p.camera_pos) - g.radius;
  counters_.visible_intra = altitude <= settings_.intra_edge_max_altitude ? intra_lod_ : 0u;
  counters_.visible_inter = p.inter_detail ? inter_lod_ : 0u;
  glUniform1ui(loc_arc_inter_, 0u);
  glUniform1ui(loc_arc_first_, 0u);
  glDrawArraysInstanced(GL_LINES, 0, 2, static_cast<GLsizei>(counters_.visible_intra));
  glUniform1ui(loc_arc_inter_, 1u);
  glUniform1ui(loc_arc_first_, intra_count_);
  glUniform1ui(loc_arc_segments_, static_cast<GLuint>(segs));
  glDrawArraysInstanced(GL_LINE_STRIP, 0, segs + 1, static_cast<GLsizei>(counters_.visible_inter));
  counters_.scene_draws += 2;
  timers_.end();

  // Pass 4 - whales: emissive attachment only.
  timers_.begin(slot_, GpuPass::Whales);
  glColorMaski(0, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
  glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glUseProgram(prog_whale_.id());
  glUniform1i(loc_whale_segments_, segs);
  glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 2 * (segs + 1), static_cast<GLsizei>(2 * whale_count_));
  ++counters_.scene_draws;
  glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
  timers_.end();

  // Pass 5 - hub markers and labels (screen space, no depth test).
  timers_.begin(slot_, GpuPass::Hubs);
  glDisable(GL_DEPTH_TEST);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glUseProgram(prog_hub_.id());
  glBindTextureUnit(1, label_atlas_.id());
  glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(taxonomy_.size() + label_glyphs_));
  ++counters_.scene_draws;
  timers_.end();
  glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

  // Pass 6 - picking, only when a click is pending.
  if (picker_.pending_render() && node_count_ > 0) {
    timers_.begin(slot_, GpuPass::Pick);
    picker_.resize(w, h);
    picker_.begin_pass();
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glUseProgram(prog_pick_.id());
    glBindVertexArray(vao_nodes_.id());
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(node_count_));
    ++counters_.scene_draws;
    glBindVertexArray(vao_empty_.id());
    picker_.end_pass();
    glDepthMask(GL_FALSE);
    timers_.end();
  }

  // ---- post: resolve, bloom (emissive only), composite ---------------------------
  timers_.begin(slot_, GpuPass::Post);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glNamedFramebufferReadBuffer(scene_fbo_.id(), GL_COLOR_ATTACHMENT0);
  glBlitNamedFramebuffer(scene_fbo_.id(), resolve_color_fbo_.id(), 0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT,
                         GL_NEAREST);
  glNamedFramebufferReadBuffer(scene_fbo_.id(), GL_COLOR_ATTACHMENT1);
  glBlitNamedFramebuffer(scene_fbo_.id(), resolve_emissive_fbo_.id(), 0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT,
                         GL_NEAREST);

  glUseProgram(prog_down_.id());
  GLuint src = resolved_emissive_.id();
  int sw = w, sh = h;
  for (std::size_t l = 0; l < static_cast<std::size_t>(kBloomLevels); ++l) {
    glBindFramebuffer(GL_FRAMEBUFFER, down_fbo_[l].id());
    glViewport(0, 0, level_w_[l], level_h_[l]);
    glBindTextureUnit(0, src);
    glUniform2f(loc_down_texel_, 1.0f / static_cast<float>(sw), 1.0f / static_cast<float>(sh));
    fullscreen();
    src = down_tex_[l].id();
    sw = level_w_[l];
    sh = level_h_[l];
  }
  glUseProgram(prog_up_.id());
  for (int l = kBloomLevels - 2; l >= 0; --l) {
    const auto li = static_cast<std::size_t>(l);
    glBindFramebuffer(GL_FRAMEBUFFER, up_fbo_[li].id());
    glViewport(0, 0, level_w_[li], level_h_[li]);
    glBindTextureUnit(0, src);
    glBindTextureUnit(1, down_tex_[li].id());
    glUniform2f(loc_up_texel_, 1.0f / static_cast<float>(sw), 1.0f / static_cast<float>(sh));
    fullscreen();
    src = up_tex_[li].id();
    sw = level_w_[li];
    sh = level_h_[li];
  }

  glBindFramebuffer(GL_FRAMEBUFFER, output_fbo_.id());
  glViewport(0, 0, w, h);
  glEnable(GL_FRAMEBUFFER_SRGB);  // linear -> sRGB on write
  glUseProgram(prog_composite_.id());
  glBindTextureUnit(0, resolved_color_.id());
  glBindTextureUnit(1, resolved_emissive_.id());
  glBindTextureUnit(2, up_tex_[0].id());
  glUniform3f(loc_comp_bg_, th.background[0], th.background[1], th.background[2]);
  glUniform1f(loc_comp_exposure_, th.exposure);
  glUniform1f(loc_comp_bloom_, th.bloom_strength / static_cast<float>(kBloomLevels));
  glUniform1f(loc_comp_vignette_, th.vignette);
  fullscreen();
  timers_.end();

  // HUD on the output target.
  if (p.show_hud) {
    timers_.begin(slot_, GpuPass::Hud);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    hud_.draw(prog_hud_.id());
    ++counters_.post_draws;
    timers_.end();
  }

  // Present to the window.
  if (p.present) {
    glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    if (default_fb_srgb_) {
      glEnable(GL_FRAMEBUFFER_SRGB);
    } else {
      glDisable(GL_FRAMEBUFFER_SRGB);
    }
    glUseProgram(prog_present_.id());
    glUniform1i(loc_present_encode_, default_fb_srgb_ ? 0 : 1);
    glBindTextureUnit(0, output_tex_.id());
    fullscreen();
  }
  glDisable(GL_FRAMEBUFFER_SRGB);
  glDepthMask(GL_TRUE);
  counters_.draw_calls = counters_.scene_draws + counters_.post_draws;

  fences_[slot_].insert();
  ++frame_;
}

bool Renderer::read_output_rgba8(std::vector<std::uint8_t>& rgba, int& width, int& height) {
  if (!output_tex_) return false;
  width = target_w_;
  height = target_h_;
  const std::size_t row = static_cast<std::size_t>(width) * 4;
  std::vector<std::uint8_t> raw(row * static_cast<std::size_t>(height));
  glFinish();
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  // Texture image queries return the stored (sRGB-encoded) bytes unchanged.
  glGetTextureImage(output_tex_.id(), 0, GL_RGBA, GL_UNSIGNED_BYTE, static_cast<GLsizei>(raw.size()), raw.data());
  rgba.resize(raw.size());
  for (int y = 0; y < height; ++y) {
    std::memcpy(rgba.data() + static_cast<std::size_t>(y) * row,
                raw.data() + static_cast<std::size_t>(height - 1 - y) * row, row);
  }
  return true;
}

bool Renderer::read_emissive(std::vector<float>& rgba, int& width, int& height) {
  if (!resolved_emissive_) return false;
  width = target_w_;
  height = target_h_;
  const std::size_t row = static_cast<std::size_t>(width) * 4;
  std::vector<float> raw(row * static_cast<std::size_t>(height));
  glFinish();
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glGetTextureImage(resolved_emissive_.id(), 0, GL_RGBA, GL_FLOAT, static_cast<GLsizei>(raw.size() * sizeof(float)),
                    raw.data());
  rgba.resize(raw.size());
  for (int y = 0; y < height; ++y) {
    std::memcpy(rgba.data() + static_cast<std::size_t>(y) * row,
                raw.data() + static_cast<std::size_t>(height - 1 - y) * row, row * sizeof(float));
  }
  return true;
}

}  // namespace ftv

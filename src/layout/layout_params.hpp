#pragma once
// Force-model constants (config/hubs.json "layout" block). Defaults are the
// values documented and justified in DESIGN_NOTES.md.

#include "core/json.hpp"

namespace ftv {

struct LayoutParams {
  // Barnes-Hut
  float theta = 0.8f;             // opening angle
  unsigned leaf_size = 16;        // max bodies per octree leaf (interaction groups hold up to 64)
  float softening = 1.0f;         // Plummer softening length (world units)
  float cross_hub_repulsion = 0.05f;
  // Hub-population-normalised repulsion: every hub's total charge is chosen
  // so that, balanced against core gravity, it fills fill_fraction * radius.
  float fill_fraction = 0.8f;
  float core_gravity_ratio = 0.25f;  // k_core = ratio * k_anchor (harmonic pull to the anchor)
  // Hub anchoring (smoothstep window as fractions of the hub radius)
  float anchor_inner = 0.6f;
  float anchor_outer = 1.2f;
  // Springs
  float spring_k = 0.004f;          // max spring stiffness (scaled by normalised log10 volume)
  float spring_rest_base = 10.0f;   // L0 = base / ln(1 + tx_count), clamped
  float spring_rest_min = 1.5f;
  float spring_rest_max = 14.0f;
  float inter_hub_spring_scale = 0.1f;
  // Integration / annealing
  float damping = 0.85f;
  float dt = 1.0f;
  float initial_temperature = 30.0f;  // max displacement per step at t=0
  float cooling = 0.995f;             // T *= cooling each iteration
  float min_temperature = 0.02f;
  float convergence_epsilon = 0.03f;  // mean displacement threshold
  unsigned convergence_window = 30;   // consecutive iterations below epsilon

  static LayoutParams from_json(const JsonValue& j) {
    LayoutParams p;
    auto f = [&j](const char* k, float d) { return static_cast<float>(j.number_or(k, d)); };
    p.theta = f("theta", p.theta);
    p.leaf_size = static_cast<unsigned>(j.number_or("leaf_size", p.leaf_size));
    p.softening = f("softening", p.softening);
    p.cross_hub_repulsion = f("cross_hub_repulsion", p.cross_hub_repulsion);
    p.fill_fraction = f("fill_fraction", p.fill_fraction);
    p.core_gravity_ratio = f("core_gravity_ratio", p.core_gravity_ratio);
    p.anchor_inner = f("anchor_inner", p.anchor_inner);
    p.anchor_outer = f("anchor_outer", p.anchor_outer);
    p.spring_k = f("spring_k", p.spring_k);
    p.spring_rest_base = f("spring_rest_base", p.spring_rest_base);
    p.spring_rest_min = f("spring_rest_min", p.spring_rest_min);
    p.spring_rest_max = f("spring_rest_max", p.spring_rest_max);
    p.inter_hub_spring_scale = f("inter_hub_spring_scale", p.inter_hub_spring_scale);
    p.damping = f("damping", p.damping);
    p.dt = f("dt", p.dt);
    p.initial_temperature = f("initial_temperature", p.initial_temperature);
    p.cooling = f("cooling", p.cooling);
    p.min_temperature = f("min_temperature", p.min_temperature);
    p.convergence_epsilon = f("convergence_epsilon", p.convergence_epsilon);
    p.convergence_window = static_cast<unsigned>(j.number_or("convergence_window", p.convergence_window));
    if (p.leaf_size < 1) p.leaf_size = 1;
    if (p.leaf_size > 64) p.leaf_size = 64;
    return p;
  }
};

}  // namespace ftv

// runtime/render/vulkan/glsl_emitter — the Vulkan twin of render/metal/msl_emitter: GLSL 4.50 for the draw pipelines
// (one vertex shader for all, one fragment shader per CombinerIR) and the fixed passes. Same uniforms (std140 layouts
// byte-identical to the MSL structs), same maths, same order of operations, so both backends draw the same pixels.
//
// Descriptor layout (vk_device.cpp builds the matching set layouts):
//   set 0 (per pass):  0 ViewU, 1 LightU (uniform buffers), 2 shadow map (sampler2DShadow), 3 AO volume (sampler3D),
//                      4 probe cube, 5 underside map (sampler2D), 6 previous probe cube, 7 water footprint (sampler2D),
//                      8 normals (storage buffer, 4 x int8 per vertex), 9 previous positions (storage buffer, 3 floats)
//   set 1 (per draw):  0 DrawU (uniform buffer), 1 texture 0, 2 texture 1 (sampler2D)
#pragma once
#include <string>
#include "combiner.h"
namespace ar {
std::string glsl_vertex();                     // vmain: shared by every draw pipeline
std::string glsl_fragment(const CombinerIR &ir);   // fmain for one combiner and its variants
// The fixed passes. depth: dmain / dcut (push constants: clip_from_model, DepthU); composite: cvert / cfrag.
std::string glsl_depth_vertex();
std::string glsl_depth_cut_fragment();
std::string glsl_fullscreen_vertex();
std::string glsl_composite_fragment();
}

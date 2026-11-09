#pragma once
#include "Core/Export.h"

#include "Geometry/Vertex.h"

#include <cstdint>
#include <vector>

namespace engine {

// Unit cube geometry (edge length 1, centered on the origin).
//
// The cube uses 24 vertices — 4 per face — instead of 8 shared vertices so
// that every face carries its exact outward normal for diffuse shading.
// All faces share one material tint; triangles are wound counter-clockwise
// when viewed from outside the cube, and the graphics pipeline compensates
// for the projection matrix's Y-flip by declaring CLOCKWISE front faces
// with back-face culling enabled.
struct ENGINE_GEOMETRY_API Cube {
    static std::vector<Vertex> vertices();
    static std::vector<uint32_t> indices();
};

} // namespace engine

#pragma once

#include "Geometry/Vertex.h"

#include <cstdint>
#include <vector>

namespace engine {

// Unit cube geometry (edge length 1, centered on the origin).
//
// The cube uses 24 vertices — 4 per face — instead of 8 shared vertices so
// that every face can carry one flat, distinct color. All triangles are
// wound counter-clockwise when viewed from outside the cube (outward face
// normals follow the right-hand rule); the graphics pipeline compensates
// for the projection matrix's Y-flip by declaring CLOCKWISE front faces.
struct Cube {
    static std::vector<Vertex> vertices();
    static std::vector<uint32_t> indices();
};

} // namespace engine

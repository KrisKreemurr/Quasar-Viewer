/**
 * @file qlvkglsl.h
 * @brief Quasar: GL's own GLSL programs compiled for Vulkan, with what Vulkan needs to feed them.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Quasar Viewer Source Code
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The sources a GL program linked (each stage's own file and the shared objects attached to it, every one as GL
// compiled it: version line, defines and engine blocks spliced in) compiled for Vulkan with glslang's relaxed rules
// (loose uniforms gathered into one block, bindings and locations assigned), linked as GL links them, and reflected:
// where each loose uniform lies in that block, and the binding of every sampler, image and block. No GL and no Vulkan
// here: pure text in, SPIR-V and tables out, so it runs anywhere and is tested off the user's dumps.
//
// Clip space is GL's as it stands, y included: Vulkan's framebuffer row 0 is where NDC y is -1, as GL's is, so Vulkan
// writes each row where GL would have, into memory GL reads as GL laid it out, and gl_FragCoord reads in Vulkan what it
// reads in GL. (The winding turns over with Vulkan's y-down framebuffer: GL's counter-clockwise front is Vulkan's
// clockwise.) Depth is GL's clip space as it stands: the caller asks for zero-to-one (reverse-Z's glClipControl) or
// declines.
namespace ql_glsl
{
    enum Stage : uint32_t
    {
        STAGE_VERTEX = 0,
        STAGE_TESS_CONTROL,
        STAGE_TESS_EVALUATION,
        STAGE_FRAGMENT,
        STAGE_COMPUTE,
        STAGE_COUNT
    };

    struct Source
    {
        Stage       stage = STAGE_VERTEX;
        std::string name;  // for messages: the file it came from
        std::string text;
    };

    // A loose uniform's place in the default block: GL's type (GL_FLOAT_VEC3, GL_FLOAT_MAT3, GL_SAMPLER_2D... as the
    // GL enum), how many elements an array has (1 for none), each element's stride, and a matrix's column stride.
    struct Member
    {
        std::string name;  // GL's name for it, without "[0]"
        uint32_t    offset = 0;
        uint32_t    gl_type = 0;
        uint32_t    count = 1;
        uint32_t    array_stride = 0;
        uint32_t    matrix_stride = 0;
    };

    enum ResourceKind : uint32_t
    {
        RES_SAMPLER = 0,      // a combined image sampler (GL's sampler*)
        RES_STORAGE_IMAGE,    // GL's image*
        RES_UNIFORM_BLOCK,    // an engine block (Deferred, Environment...), or the default block itself
        RES_STORAGE_BLOCK,    // a buffer block
        RES_TEXEL_BUFFER,     // GL's samplerBuffer
    };

    struct Resource
    {
        std::string  name;
        ResourceKind kind = RES_SAMPLER;
        uint32_t     set = 0, binding = 0;
        uint32_t     count = 1;     // an array of samplers: its size
        uint32_t     gl_type = 0;   // a sampler's or image's GL type
        uint32_t     size = 0;      // a block's bytes
        uint32_t     stages = 0;    // bit per Stage reading it
        bool         default_block = false;
    };

    struct Attribute
    {
        std::string name;
        uint32_t    location = 0;
        uint32_t    gl_type = 0;
    };

    struct Program
    {
        std::vector<uint32_t>  spirv[STAGE_COUNT];  // empty for a stage the program has not
        uint32_t               stage_mask = 0;
        std::vector<Member>    members;              // the default block's
        uint32_t               default_size = 0;     // its bytes (0: no loose uniforms)
        int                    default_resource = -1;  // its entry in resources
        std::vector<Resource>  resources;
        std::vector<Attribute> inputs;               // the vertex stage's
        std::vector<Attribute> outputs;              // the fragment stage's
        uint32_t               local_size[3] = { 1, 1, 1 };  // a compute program's
        std::string            log;                  // what glslang said, on failure or warnings
    };

    // Once per process before the first compile, and once after the last (glslang's process state). Safe to call
    // more than once: counted.
    void initialize();
    void finalize();

    // false: the program did not compile or link for Vulkan (out.log says why); GL keeps drawing it.
    bool compile(const std::vector<Source>& sources, Program& out);

    // The member a GL uniform name ("foo", "foo[3]", "s.f") names, and the element: null where there is none.
    const Member* findMember(const Program& program, const std::string& gl_name, uint32_t& element);
}

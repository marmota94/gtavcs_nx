// The Switch port's OpenGL 3.3 core backend of upstream's rhi.h (rhi_gl.cpp): port-owned diagnostics that the
// upstream Device interface has no place for. Used by the conformance scenes (src/rhi_tests) and the core's reports.
#pragma once

#include "rhi/rhi.h"

#include <cstdint>
#include <string>

namespace rhi
{
    // What the context offers, read once at device creation.
    struct GlCaps
    {
        int major = 0, minor = 0;
        std::string vendor, renderer, version, glsl;
        bool clipControl = false;   // GL 4.5 / ARB_clip_control: D3D's 0..1 clip depth without a shader remap
        bool viewportArray = false; // GL 4.1 / ARB_viewport_array: fractional viewports (glViewportIndexedf)
        bool anisotropy = false;    // GL 4.6 / EXT_texture_filter_anisotropic
        bool khrDebug = false;      // GL 4.3 / KHR_debug: message callback, debug groups, object labels
        float maxAnisotropy = 1.0f;
        int maxDualSourceDrawBuffers = 0;
        int maxSamples = 0, maxColorTextureSamples = 0, maxDepthTextureSamples = 0;
        int maxTextureSize = 0, maxUniformBlockSize = 0, maxTextureUnits = 0;
        int defaultFramebufferEncoding = 0; // GL_LINEAR / GL_SRGB of the window's back buffer (0: unknown, offscreen)
    };

    // Live counts; every field is cumulative except the object counts and bytes, which are what exists now.
    struct GlStats
    {
        uint64_t textures = 0, textureBytes = 0; // estimated: width x height x 4 x samples, mip chains included
        uint64_t buffers = 0, bufferBytes = 0;
        uint64_t samplers = 0, pipelines = 0, programs = 0, framebuffers = 0;
        // render targets and depth buffers among the textures (M09-06), and the largest one alive (by area)
        uint64_t targets = 0, targetBytes = 0;
        uint32_t largestTargetWidth = 0, largestTargetHeight = 0;
        uint64_t pendingDeletes = 0;   // GL names waiting for the owner thread (deferred destruction)
        uint64_t glErrors = 0;         // glGetError results (polled at present and by glCheck)
        uint64_t debugMessages = 0;    // KHR_debug messages of severity medium or high
        uint64_t incompleteFramebuffers = 0;
        uint64_t ownerViolations = 0;  // a call from a thread other than the owner: refused
        uint64_t rejected = 0;         // calls refused by a bounds / alignment / state check (each logged)
        uint64_t hazards = 0;          // a draw that samples a texture bound as its own target (refused)
        uint64_t compileFailures = 0;  // shaders or programs that did not compile / link
        uint64_t drawCalls = 0, passes = 0, presents = 0;
        uint64_t offOwnerCreated = 0, offOwnerRefused = 0, fencesWaited = 0; // PS2X_GL_WORKER_CONTEXT textures
        // PS2X_GL_GPU_TIME=1 (M06-11): GL_TIME_ELAPSED from one present to the next, read back a few frames later
        uint64_t gpuFrames = 0, gpuNs = 0, gpuMaxNs = 0, gpuDropped = 0;
    };

    bool openGLCaps(Device &device, GlCaps &out);   // false: not the OpenGL backend
    bool openGLStats(Device &device, GlStats &out); // false: not the OpenGL backend
    // Every pending error of the context, counted into the stats and logged with `where`; returns how many there were.
    uint32_t openGLCheckErrors(Device &device, const char *where);
}

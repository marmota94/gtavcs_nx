// OpenGL 3.3 core backend of upstream's rhi.h, for the Nintendo Switch port (SDL2 + Mesa nouveau through EGL).
// The only port file that makes GL calls besides src/main.c's context set-up. Upstream's D3D11 backend is the model
// for every method's meaning; the conventions that differ in GL are resolved here and in the shader prelude
// (tools/glsl_shaders.py, src/render/glsl/README.md):
//
//   - Rows: every texture holds rows top-down, as in D3D / Vulkan (data row 0 = texture row 0 = PS2 row 0). Drawing
//     into a texture uses glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE) where the context has clip control (GL 4.5 /
//     ARB_clip_control; the Switch's Mesa has it): NDC +1 lands on row 0, gl_FragCoord.y counts rows from the top like
//     SV_Position and the rasteriser's tie rule for edges on pixel centres is D3D's top-left one. Without clip control
//     the vertex shader flips Y instead (rhi_flipY = -1): the same rows, but GL's lower-left tie rule (an edge exactly
//     on a pixel centre covers the row below instead of the row above). Viewports and scissors take D3D's top-left
//     rectangles unchanged. The window (the default framebuffer) is drawn with GL_LOWER_LEFT, unflipped, its
//     rectangles converted to GL's bottom-left origin.
//   - Depth: D3D's 0..1 clip depth through glClipControl(..., GL_ZERO_TO_ONE) where the context has it, else
//     remapped in the vertex shader (RHI_ZERO_TO_ONE 0). Depth targets are D32F; clears and shader depth are 0..1.
//   - Bindings: uniform blocks named <Name>_b<N> go to binding N (HLSL register bN), samplers <name>_t<N> to texture
//     unit N (tN / sN); vertex attributes at location = their index in PipelineDesc::attributes.
//   - Threading: one owner thread (the one that made the device, with the platform's context current); every other
//     thread is refused and counted. Objects may be released from any thread: their GL names wait in a queue the owner
//     drains (deferred destruction), and a device gone first leaves them to the context's own teardown.
//     Opt-in (PS2X_GL_WORKER_CONTEXT=1 and a platform loader context, M05-09's second prototype): createTexture (one
//     level with data, sampled only) and createTextureLevels from other threads, on a loader context sharing the
//     window context's objects (one thread at a time), each texture fenced; the owner waits for the fence on the GPU
//     (glWaitSync) the first time it uses the texture. freeThreadedCreation() reports it.
//   - Scissor always on (D3D11 ScissorEnable TRUE; beginPass resets it to the target), clears and copies ignore the
//     pipeline's masks and scissor as D3D11's do.
#include "rhi/gl/rhi_gl.h"

#include <glad/gl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rhi
{
    namespace
    {
        // Entry points and enums beyond GLAD's GL 3.3 core set, loaded only when the context reports them.
        constexpr GLenum kLowerLeft = 0x8CA1, kUpperLeft = 0x8CA2, kZeroToOne = 0x935F;
        constexpr GLenum kTextureMaxAnisotropy = 0x84FE, kMaxTextureMaxAnisotropy = 0x84FF;
        constexpr GLenum kDebugOutput = 0x92E0, kDebugOutputSynchronous = 0x8242, kDebugSourceApplication = 0x824A;
        constexpr GLenum kDebugSeverityHigh = 0x9146, kDebugSeverityMedium = 0x9147;
        constexpr GLenum kLabelBuffer = 0x82E0, kLabelProgram = 0x82E2, kLabelSampler = 0x82E6;
        using ClipControlFn = void (*)(GLenum, GLenum);
        using BufferStorageFn = void (*)(GLenum, GLsizeiptr, const void *, GLbitfield); // GL 4.4 / ARB_buffer_storage
        constexpr GLbitfield kMapPersistent = 0x0040, kMapCoherent = 0x0080;
        using ViewportIndexedfFn = void (*)(GLuint, GLfloat, GLfloat, GLfloat, GLfloat);
        using DebugProc = void (*)(GLenum, GLenum, GLuint, GLenum, GLsizei, const GLchar *, const void *);
        using DebugMessageCallbackFn = void (*)(DebugProc, const void *);
        using PushDebugGroupFn = void (*)(GLenum, GLuint, GLsizei, const GLchar *);
        using PopDebugGroupFn = void (*)();
        using ObjectLabelFn = void (*)(GLenum, GLuint, GLsizei, const GLchar *);

        bool envOn(const char *name)
        {
            const char *e = std::getenv(name);
            return e && *e && *e != '0';
        }

        uint32_t fullChain(uint32_t w, uint32_t h)
        {
            uint32_t levels = 1;
            for (uint32_t m = std::max(w, h); m > 1; m >>= 1)
                ++levels;
            return levels;
        }

        struct FormatInfo
        {
            GLenum internal, format, type;
            uint32_t bytes;
            bool depth;
        };

        FormatInfo formatInfo(Format f)
        {
            switch (f)
            {
            case Format::RGBA8: return {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, false};
            case Format::R32F: return {GL_R32F, GL_RED, GL_FLOAT, 4, false};
            case Format::D32F: return {GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, 4, true};
            default: return {0, 0, 0, 0, false};
            }
        }

        GLenum blendFactor(BlendFactor f)
        {
            switch (f)
            {
            case BlendFactor::Zero: return GL_ZERO;
            case BlendFactor::One: return GL_ONE;
            case BlendFactor::SrcAlpha: return GL_SRC_ALPHA;
            case BlendFactor::InvSrcAlpha: return GL_ONE_MINUS_SRC_ALPHA;
            case BlendFactor::DstAlpha: return GL_DST_ALPHA;
            case BlendFactor::InvDstAlpha: return GL_ONE_MINUS_DST_ALPHA;
            case BlendFactor::Src1Alpha: return GL_SRC1_ALPHA; // dual-source (GL 3.3 core, ARB_blend_func_extended)
            case BlendFactor::InvSrc1Alpha: return GL_ONE_MINUS_SRC1_ALPHA;
            case BlendFactor::Constant: return GL_CONSTANT_COLOR; // D3D11 BLEND_FACTOR: the constant's own channel
            case BlendFactor::InvConstant: return GL_ONE_MINUS_CONSTANT_COLOR;
            }
            return GL_ONE;
        }

        bool dualSource(BlendFactor f) { return f == BlendFactor::Src1Alpha || f == BlendFactor::InvSrc1Alpha; }

        GLenum blendOp(BlendOp op)
        {
            switch (op)
            {
            case BlendOp::Add: return GL_FUNC_ADD;
            case BlendOp::Subtract: return GL_FUNC_SUBTRACT;                 // src - dst
            case BlendOp::ReverseSubtract: return GL_FUNC_REVERSE_SUBTRACT;  // dst - src
            case BlendOp::Min: return GL_MIN;
            case BlendOp::Max: return GL_MAX;
            }
            return GL_FUNC_ADD;
        }

        GLenum compare(Compare c)
        {
            switch (c)
            {
            case Compare::Never: return GL_NEVER;
            case Compare::Less: return GL_LESS;
            case Compare::Equal: return GL_EQUAL;
            case Compare::LessEqual: return GL_LEQUAL;
            case Compare::Greater: return GL_GREATER;
            case Compare::NotEqual: return GL_NOTEQUAL;
            case Compare::GreaterEqual: return GL_GEQUAL;
            case Compare::Always: return GL_ALWAYS;
            }
            return GL_ALWAYS;
        }

        GLenum topology(Topology t)
        {
            switch (t)
            {
            case Topology::Triangles: return GL_TRIANGLES;
            case Topology::TriangleStrip: return GL_TRIANGLE_STRIP;
            case Topology::Lines: return GL_LINES;
            case Topology::Points: return GL_POINTS;
            }
            return GL_TRIANGLES;
        }

        struct VertexFormatInfo
        {
            GLint components;
            GLenum type;
            bool normalized, integer;
            uint32_t bytes;
        };

        VertexFormatInfo vertexFormat(VertexFormat f)
        {
            switch (f)
            {
            case VertexFormat::Float1: return {1, GL_FLOAT, false, false, 4};
            case VertexFormat::Float2: return {2, GL_FLOAT, false, false, 8};
            case VertexFormat::Float3: return {3, GL_FLOAT, false, false, 12};
            case VertexFormat::Float4: return {4, GL_FLOAT, false, false, 16};
            case VertexFormat::UByte4Norm: return {4, GL_UNSIGNED_BYTE, true, false, 4};
            case VertexFormat::UInt1: return {1, GL_UNSIGNED_INT, false, true, 4};
            }
            return {4, GL_FLOAT, false, false, 16};
        }

        const char *errorName(GLenum e)
        {
            switch (e)
            {
            case GL_INVALID_ENUM: return "GL_INVALID_ENUM";
            case GL_INVALID_VALUE: return "GL_INVALID_VALUE";
            case GL_INVALID_OPERATION: return "GL_INVALID_OPERATION";
            case GL_INVALID_FRAMEBUFFER_OPERATION: return "GL_INVALID_FRAMEBUFFER_OPERATION";
            case GL_OUT_OF_MEMORY: return "GL_OUT_OF_MEMORY";
            default: return "GL error";
            }
        }

        // State shared with the objects: outlives the device when a texture is released after it (the presenter's
        // pictures can be), so a late release only drops its names.
        struct Core
        {
            std::mutex mutex;
            bool alive = true;
            std::thread::id owner;
            std::vector<GLuint> textures, buffers, samplers, vaos; // names released, deleted by the owner (drain)
            std::vector<GLsync> fences;                            // worker-created textures released before first use
            std::atomic<uint64_t> textureCount{0}, textureBytes{0}, bufferCount{0}, bufferBytes{0}, samplerCount{0};
            std::atomic<uint64_t> pipelineCount{0};
            // render targets and depth buffers (M09-06: what widescreen's wider targets cost), and the live ones' sizes
            std::atomic<uint64_t> targetCount{0}, targetBytes{0};
            std::map<std::pair<uint32_t, uint32_t>, uint32_t> targetSizes; // width, height -> live targets (mutex)

            void release(std::vector<GLuint> Core::*list, GLuint name)
            {
                if (!name)
                    return;
                std::lock_guard<std::mutex> lock(mutex);
                if (alive)
                    (this->*list).push_back(name);
            }
        };

        class GlTexture final : public Texture
        {
        public:
            GlTexture(std::shared_ptr<Core> core, const TextureDesc &d) : m_core(std::move(core)) { m_desc = d; }
            ~GlTexture() override
            {
                m_core->textureCount.fetch_sub(1, std::memory_order_relaxed);
                m_core->textureBytes.fetch_sub(bytes, std::memory_order_relaxed);
                if (renderTarget)
                {
                    m_core->targetCount.fetch_sub(1, std::memory_order_relaxed);
                    m_core->targetBytes.fetch_sub(bytes, std::memory_order_relaxed);
                    std::lock_guard<std::mutex> lock(m_core->mutex);
                    auto it = m_core->targetSizes.find({m_desc.width, m_desc.height});
                    if (it != m_core->targetSizes.end() && --it->second == 0)
                        m_core->targetSizes.erase(it);
                }
                m_core->release(&Core::textures, name);
                if (fence)
                {
                    std::lock_guard<std::mutex> lock(m_core->mutex);
                    if (m_core->alive)
                        m_core->fences.push_back(fence);
                }
            }
            GLsync fence = nullptr; // made on a loader context: the owner waits for it at first use (ready)
            std::shared_ptr<Core> m_core;
            GLuint name = 0;
            GLenum target = GL_TEXTURE_2D;
            FormatInfo format{};
            uint32_t levels = 1;
            uint64_t bytes = 0;
            bool renderTarget = false; // UsageRenderTarget or UsageDepth: counted in Core::target*
        };

        class GlBuffer final : public Buffer
        {
        public:
            GlBuffer(std::shared_ptr<Core> core, const BufferDesc &d) : m_core(std::move(core)) { m_desc = d; }
            ~GlBuffer() override
            {
                m_core->bufferCount.fetch_sub(1, std::memory_order_relaxed);
                m_core->bufferBytes.fetch_sub(allocated, std::memory_order_relaxed);
                m_core->release(&Core::buffers, name);
            }
            std::shared_ptr<Core> m_core;
            GLuint name = 0;
            GLenum target = GL_ARRAY_BUFFER;
            size_t allocated = 0; // bytes the GL buffer holds (constant buffers: rounded up to 16 as in D3D11)
            // M10-03: a dynamic constant buffer's contents live in the device's uniform ring once updated (ring); its
            // own storage keeps the initial data until then
            bool ring = false, ringValid = false;
            size_t ringOffset = 0;
        };

        class GlSampler final : public Sampler
        {
        public:
            explicit GlSampler(std::shared_ptr<Core> core) : m_core(std::move(core)) {}
            ~GlSampler() override
            {
                m_core->samplerCount.fetch_sub(1, std::memory_order_relaxed);
                m_core->release(&Core::samplers, name);
            }
            std::shared_ptr<Core> m_core;
            GLuint name = 0;
        };

        struct Program
        {
            GLuint name = 0;
            GLint flipLocation = -1;
            float flip = 0.0f;                          // the rhi_flipY value set last (0: never)
            std::vector<std::pair<GLuint, GLint>> blocks; // binding, minimum size
            uint32_t samplerUnits = 0;                    // bit per texture unit a sampler uniform reads
            bool clipDistances = false;                   // the vertex shader writes gl_ClipDistance[0..1]
            bool dualOutput = false;                      // a second colour output at index 1 (o_blend, DUAL_SOURCE)
        };

        class GlPipeline final : public Pipeline
        {
        public:
            explicit GlPipeline(std::shared_ptr<Core> core) : m_core(std::move(core)) {}
            ~GlPipeline() override
            {
                m_core->pipelineCount.fetch_sub(1, std::memory_order_relaxed);
                m_core->release(&Core::vaos, vao);
            }
            std::shared_ptr<Core> m_core;
            uint64_t serial = 0; // never reused, unlike the address (the state cache compares this)
            std::shared_ptr<Program> program;
            GLuint vao = 0;
            std::vector<VertexAttribute> attributes;
            uint32_t stride = 0;
            GLenum mode = GL_TRIANGLES;
            BlendState blend;
            DepthState depth;
            bool depthClamp = false;
            std::string name;
            // what the VAO holds now (re-specified at a draw when the bound buffer, offset or buffer epoch changed)
            GLuint vaoBuffer = 0;
            uint32_t vaoOffset = 0;
            GLuint vaoIndexBuffer = 0;
            uint64_t vaoEpoch = 0;
        };

        // What is bound, by GL name and size: never a pointer to an object the caller may already have released.
        struct BoundTexture
        {
            GLuint name = 0;
            GLenum target = GL_TEXTURE_2D;
        };

        struct BoundBuffer
        {
            GLuint name = 0;
            size_t size = 0; // the caller's size (vertex / index bounds) or the allocation (constant buffers)
        };

        class GlDevice final : public Device
        {
        public:
            ~GlDevice() override
            {
                if (!onOwner())
                {
                    std::fprintf(stderr, "[rhi] OpenGL device destroyed off its owner thread: GL names left to the context\n");
                    std::lock_guard<std::mutex> lock(m_core->mutex);
                    m_core->alive = false;
                    return;
                }
                for (auto &f : m_fbos)
                    glDeleteFramebuffers(1, &f.second);
                m_fbos.clear();
                if (m_copyFbo[0])
                    glDeleteFramebuffers(2, m_copyFbo);
                if (m_emptyVao)
                    glDeleteVertexArrays(1, &m_emptyVao);
                for (auto &p : m_programs)
                    if (p.second)
                        glDeleteProgram(p.second->name);
                for (auto &s : m_shaders)
                    glDeleteShader(s.second);
                m_offscreenBack.reset();
                if (m_gpuTime)
                {
                    if (m_gpuActive)
                        glEndQuery(GL_TIME_ELAPSED);
                    glDeleteQueries(kGpuQueries, m_gpuQueries);
                    std::fprintf(stderr, "[rhi] GPU time: %llu frames, mean %.2f ms, max %.2f ms, %llu not timed\n",
                                 static_cast<unsigned long long>(m_stats.gpuFrames),
                                 m_stats.gpuFrames ? m_stats.gpuNs / 1e6 / static_cast<double>(m_stats.gpuFrames) : 0.0,
                                 m_stats.gpuMaxNs / 1e6, static_cast<unsigned long long>(m_stats.gpuDropped));
                }
                if (m_gpuLog)
                    std::fclose(m_gpuLog);
                if (m_passTime && !m_passQueries.empty())
                    glDeleteQueries(static_cast<GLsizei>(m_passQueries.size()), m_passQueries.data());
                for (GLsync &f : m_ubo.fences)
                    if (f)
                        glDeleteSync(f), f = nullptr;
                if (m_ubo.name)
                {
                    if (m_ubo.mapped)
                    {
                        glBindBuffer(GL_COPY_WRITE_BUFFER, m_ubo.name);
                        glUnmapBuffer(GL_COPY_WRITE_BUFFER);
                        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
                    }
                    glDeleteBuffers(1, &m_ubo.name);
                }
                drain();
                std::lock_guard<std::mutex> lock(m_core->mutex);
                m_core->alive = false;
            }

            bool create(const DeviceDesc &desc, std::string &error)
            {
                m_core = std::make_shared<Core>();
                m_core->owner = std::this_thread::get_id();
                m_window = desc.gl;
                if (!m_window.getProcAddress)
                {
                    error = "no GL context from the platform layer (DeviceDesc::gl)";
                    return false;
                }
                if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(m_window.getProcAddress)) || !GLAD_GL_VERSION_3_3)
                {
                    error = "the current GL context has no OpenGL 3.3 core entry points";
                    return false;
                }
                readCaps();
                m_offscreen = desc.offscreen || !m_window.swap;
                if (!m_caps.maxDualSourceDrawBuffers)
                {
                    // GL 3.3 core requires at least one; a context without it cannot run gs_draw's dual-source variants
                    error = "GL_MAX_DUAL_SOURCE_DRAW_BUFFERS is 0";
                    return false;
                }
                setUpDebug(desc.debug || envOn("PS2X_RHI_DEBUG"));
                // GPU time per presented frame (M06-11): GL 3.3 core has timer queries (ARB_timer_query)
                m_gpuTime = !m_offscreen && envOn("PS2X_GL_GPU_TIME");
                if (m_gpuTime)
                {
                    glGenQueries(kGpuQueries, m_gpuQueries);
                    if (const char *path = std::getenv("PS2X_GL_GPU_LOG"))
                    {
                        m_gpuLog = std::fopen(path, "w");
                        if (m_gpuLog)
                            std::fputs("frame,gpu_ms\n", m_gpuLog);
                        std::fprintf(stderr, "[rhi] GPU time per frame to %s%s\n", path, m_gpuLog ? "" : ": cannot open");
                    }
                    beginGpuFrame();
                }
                // M10-03: dynamic constant buffers through one uniform ring (PS2X_GL_UBO_RING=0: each update orphans the
                // buffer's own storage, as before). Driving, ~500 updates a frame cost the Switch's main thread 12-23 ms
                // with orphaning (route-prof-docked); each orphan is a new allocation in Mesa nouveau.
                {
                    const char *e = std::getenv("PS2X_GL_UBO_RING");
                    m_ubo.on = !(e && e[0] == '0');
                }
                if (m_ubo.on)
                {
                    GLint align = 256;
                    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &align);
                    m_ubo.align = static_cast<size_t>(std::max(align, 16));
                    glGenBuffers(1, &m_ubo.name);
                    glBindBuffer(GL_COPY_WRITE_BUFFER, m_ubo.name);
                    // mapped once, persistent and coherent where the context has buffer storage: an update is then a
                    // memcpy (an unsynchronized map / unmap per update cost as much as orphaning on nouveau,
                    // route-prof2-docked); else a map per update. PS2X_GL_UBO_PERSISTENT=0: the map per update.
                    const char *pe = std::getenv("PS2X_GL_UBO_PERSISTENT");
                    if (m_bufferStorage && !(pe && pe[0] == '0'))
                    {
                        m_bufferStorage(GL_COPY_WRITE_BUFFER, static_cast<GLsizeiptr>(kUboRingBytes), nullptr,
                                        GL_MAP_WRITE_BIT | kMapPersistent | kMapCoherent);
                        m_ubo.mapped = static_cast<uint8_t *>(glMapBufferRange(GL_COPY_WRITE_BUFFER, 0,
                                                                               static_cast<GLsizeiptr>(kUboRingBytes),
                                                                               GL_MAP_WRITE_BIT | kMapPersistent | kMapCoherent));
                        if (!m_ubo.mapped)
                        {
                            // storage is immutable now: a fresh buffer for the map-per-update path
                            glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
                            glDeleteBuffers(1, &m_ubo.name);
                            glGenBuffers(1, &m_ubo.name);
                            glBindBuffer(GL_COPY_WRITE_BUFFER, m_ubo.name);
                        }
                    }
                    if (!m_ubo.mapped)
                        glBufferData(GL_COPY_WRITE_BUFFER, static_cast<GLsizeiptr>(kUboRingBytes), nullptr, GL_STREAM_DRAW);
                    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
                    glCheckError("constant ring");
                    label(kLabelBuffer, m_ubo.name, "constant ring");
                    std::fprintf(stderr, "[rhi] constant buffers through a %u KiB ring (offset alignment %zu, %s)\n",
                                 static_cast<unsigned>(kUboRingBytes / 1024u), m_ubo.align,
                                 m_ubo.mapped ? "persistent map" : "map per update");
                }
                // GPU time per pass (M10-02): GL_TIMESTAMP queries around each pass, by pass name and target size
                m_passTime = !m_offscreen && envOn("PS2X_GL_PASS_TIME");
                if (m_passTime)
                {
                    m_passQueries.resize(2u * kPassRecords);
                    glGenQueries(static_cast<GLsizei>(m_passQueries.size()), m_passQueries.data());
                    m_passRecords.resize(kPassRecords);
                    std::fprintf(stderr, "[rhi] GPU time per pass (PS2X_GL_PASS_TIME): %u passes in flight at most\n", kPassRecords);
                }
                m_freeThreaded = envOn("PS2X_GL_WORKER_CONTEXT") && m_window.loaderAvailable && m_window.makeLoaderCurrent &&
                                 m_window.loaderAvailable();
                if (envOn("PS2X_GL_WORKER_CONTEXT") && !m_freeThreaded)
                    std::fprintf(stderr, "[rhi] PS2X_GL_WORKER_CONTEXT: no loader context from the platform; textures on the owner only\n");
                if (m_caps.clipControl && m_clipControl)
                {
                    m_clipControl(kLowerLeft, kZeroToOne);
                    m_clipOrigin = kLowerLeft;
                }
                glDisable(GL_FRAMEBUFFER_SRGB); // UNORM targets everywhere, as the D3D11 / Vulkan backends use
                glDisable(GL_PRIMITIVE_RESTART);
                glDisable(GL_CULL_FACE);
                glDisable(GL_STENCIL_TEST);
                glDisable(GL_DITHER);
                glEnable(GL_SCISSOR_TEST);
                glProvokingVertex(GL_FIRST_VERTEX_CONVENTION); // D3D's flat-shading vertex
                glPointSize(1.0f);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glGenVertexArrays(1, &m_emptyVao);
                glGenFramebuffers(2, m_copyFbo);
                GLint units = 0;
                glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &units);
                m_scratchUnit = static_cast<GLuint>(std::max(16, std::min(units, 48)) - 1);
                m_width = std::max(1u, desc.width);
                m_height = std::max(1u, desc.height);
                if (m_offscreen && !makeOffscreenBack())
                {
                    error = "no offscreen back buffer";
                    return false;
                }
                char name[512];
                std::snprintf(name, sizeof(name), "%s (%s; GLSL %s)%s%s", m_caps.renderer.c_str(), m_caps.version.c_str(),
                              m_caps.glsl.c_str(), m_caps.clipControl ? "" : ", depth remapped in shaders",
                              m_offscreen ? ", offscreen" : "");
                m_adapterName = name;
                std::fprintf(stderr,
                             "[rhi] OpenGL %d.%d: clip_control %d, viewport_array %d, anisotropy %d (max %.0f), KHR_debug %d, "
                             "dual-source draw buffers %d, samples %d/%d/%d, max texture %d, framebuffer encoding 0x%x\n",
                             m_caps.major, m_caps.minor, m_caps.clipControl, m_caps.viewportArray, m_caps.anisotropy,
                             m_caps.maxAnisotropy, m_caps.khrDebug, m_caps.maxDualSourceDrawBuffers, m_caps.maxSamples,
                             m_caps.maxColorTextureSamples, m_caps.maxDepthTextureSamples, m_caps.maxTextureSize,
                             m_caps.defaultFramebufferEncoding);
                if (m_freeThreaded)
                    std::fprintf(stderr, "[rhi] OpenGL free-threaded texture creation on a loader context (PS2X_GL_WORKER_CONTEXT)\n");
                return checkErrors("create") == 0;
            }

            Backend backend() const override { return Backend::OpenGL; }
            const std::string &adapterName() const override { return m_adapterName; }

            // ---- resources ----------------------------------------------------------------------------------------
            std::unique_ptr<Texture> createTexture(const TextureDesc &desc, const void *data, uint32_t rowPitch) override
            {
                CpuScope cpu(this, CpuCreate);
                if (!onOwner() && m_freeThreaded)
                {
                    if (!data || desc.samples > 1u || desc.mipLevels != 1u || (desc.usage & (UsageRenderTarget | UsageDepth)) ||
                        desc.format != Format::RGBA8)
                    {
                        ++m_offOwnerRefused;
                        return nullptr; // only what freeThreadedCreation promises (rhi.h)
                    }
                    const void *levels[1] = {data};
                    return createOffOwner(desc, levels, 1, rowPitch);
                }
                if (!owner("createTexture"))
                    return nullptr;
                const FormatInfo fi = formatInfo(desc.format);
                const uint32_t w = std::max(1u, desc.width), h = std::max(1u, desc.height);
                const bool msaa = desc.samples > 1u;
                if (!fi.internal || !checkSize(w, h, desc.name))
                    return nullptr;
                if (msaa)
                {
                    const int limit = fi.depth ? m_caps.maxDepthTextureSamples : m_caps.maxColorTextureSamples;
                    if (static_cast<int>(desc.samples) > limit || data || (desc.usage & UsageDynamic))
                    {
                        reject("createTexture: %u samples (limit %d) or initial data on a multisampled texture '%s'",
                               desc.samples, limit, desc.name ? desc.name : "");
                        return nullptr;
                    }
                }
                if (data && fi.depth)
                {
                    reject("createTexture: initial data for a depth texture '%s'", desc.name ? desc.name : "");
                    return nullptr;
                }
                const bool mips = desc.mipLevels != 1u && !msaa;
                TextureDesc sized = desc;
                sized.width = w;
                sized.height = h;
                auto t = std::make_unique<GlTexture>(m_core, sized);
                t->format = fi;
                t->levels = mips ? (desc.mipLevels ? std::min(desc.mipLevels, fullChain(w, h)) : fullChain(w, h)) : 1u;
                t->target = msaa ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
                glGenTextures(1, &t->name);
                bindScratch(t->target, t->name);
                if (msaa)
                    glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, static_cast<GLsizei>(desc.samples), fi.internal, w, h, GL_TRUE);
                else
                {
                    for (uint32_t level = 0; level < t->levels; ++level)
                        glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(level), static_cast<GLint>(fi.internal),
                                     std::max(1u, w >> level), std::max(1u, h >> level), 0, fi.format, fi.type, nullptr);
                    defaultParameters(t->levels);
                }
                t->bytes = accountTexture(w, h, fi.bytes, t->levels, msaa ? desc.samples : 1u);
                if (desc.usage & (UsageRenderTarget | UsageDepth))
                {
                    t->renderTarget = true;
                    m_core->targetCount.fetch_add(1, std::memory_order_relaxed);
                    m_core->targetBytes.fetch_add(t->bytes, std::memory_order_relaxed);
                    std::lock_guard<std::mutex> lock(m_core->mutex);
                    ++m_core->targetSizes[{w, h}];
                }
                label(GL_TEXTURE, t->name, desc.name);
                bindScratch(t->target, 0);
                if (glCheckError("createTexture"))
                    return nullptr; // its destructor releases the name
                if (data)
                {
                    upload(*t, 0, 0, 0, w, h, data, rowPitch);
                    if (mips)
                        mipmaps(*t);
                }
                return t;
            }

            void updateTexture(Texture &texture, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void *data,
                               uint32_t rowPitch) override
            {
                CpuScope cpu(this, CpuUpload);
                if (!owner("updateTexture"))
                    return;
                auto &t = static_cast<GlTexture &>(texture);
                ready(t);
                if (!inside(t, x, y, width, height) || !data || t.target != GL_TEXTURE_2D || t.format.depth)
                {
                    reject("updateTexture: %ux%u at %u,%u outside '%s' (%ux%u), no data, multisampled or depth", width, height,
                           x, y, t.desc().name ? t.desc().name : "", t.desc().width, t.desc().height);
                    return;
                }
                upload(t, 0, x, y, width, height, data, rowPitch);
            }

            bool readTexture(Texture &texture, uint32_t x, uint32_t y, uint32_t width, uint32_t height, void *out,
                             uint32_t rowPitch) override
            {
                if (!owner("readTexture"))
                    return false;
                auto &t = static_cast<GlTexture &>(texture);
                ready(t);
                if (!inside(t, x, y, width, height) || !out || t.target != GL_TEXTURE_2D)
                {
                    reject("readTexture: %ux%u at %u,%u outside '%s', no destination or multisampled (resolve first)", width,
                           height, x, y, t.desc().name ? t.desc().name : "");
                    return false;
                }
                const uint32_t bpp = t.format.bytes;
                const uint32_t pitch = rowPitch ? rowPitch : width * bpp;
                if (pitch < width * bpp)
                {
                    reject("readTexture: row pitch %u below a row's %u bytes", pitch, width * bpp);
                    return false;
                }
                attachCopy(GL_READ_FRAMEBUFFER, m_copyFbo[0], t);
                if (!complete(GL_READ_FRAMEBUFFER, "readTexture"))
                {
                    detachCopy(GL_READ_FRAMEBUFFER, m_copyFbo[0]);
                    restoreFramebuffer();
                    return false;
                }
                // rows come out in the texture's row order: row y first, i.e. top-down (see the header comment)
                if (pitch % bpp == 0)
                {
                    glPixelStorei(GL_PACK_ROW_LENGTH, static_cast<GLint>(pitch / bpp));
                    glReadPixels(static_cast<GLint>(x), static_cast<GLint>(y), static_cast<GLsizei>(width),
                                 static_cast<GLsizei>(height), t.format.format, t.format.type, out);
                    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
                }
                else
                {
                    std::vector<uint8_t> rows(static_cast<size_t>(width) * height * bpp);
                    glReadPixels(static_cast<GLint>(x), static_cast<GLint>(y), static_cast<GLsizei>(width),
                                 static_cast<GLsizei>(height), t.format.format, t.format.type, rows.data());
                    for (uint32_t row = 0; row < height; ++row)
                        std::memcpy(static_cast<uint8_t *>(out) + static_cast<size_t>(row) * pitch,
                                    rows.data() + static_cast<size_t>(row) * width * bpp, static_cast<size_t>(width) * bpp);
                }
                detachCopy(GL_READ_FRAMEBUFFER, m_copyFbo[0]);
                restoreFramebuffer();
                return glCheckError("readTexture") == 0;
            }

            void copyTexture(Texture &dst, uint32_t dstX, uint32_t dstY, Texture &src, uint32_t srcX, uint32_t srcY,
                             uint32_t width, uint32_t height) override
            {
                CpuScope cpu(this, CpuCopy);
                if (!owner("copyTexture"))
                    return;
                auto &d = static_cast<GlTexture &>(dst);
                auto &s = static_cast<GlTexture &>(src);
                ready(s);
                ready(d);
                if (!inside(s, srcX, srcY, width, height) || !inside(d, dstX, dstY, width, height) ||
                    d.format.internal != s.format.internal || d.desc().samples != s.desc().samples || &d == &s)
                {
                    reject("copyTexture: %ux%u from '%s' %u,%u to '%s' %u,%u: outside, formats / samples differ or the same "
                           "texture", width, height, s.desc().name ? s.desc().name : "", srcX, srcY,
                           d.desc().name ? d.desc().name : "", dstX, dstY);
                    return;
                }
                blit(s, srcX, srcY, d, dstX, dstY, width, height, "copyTexture");
            }

            void resolveTexture(Texture &dst, Texture &src) override
            {
                CpuScope cpu(this, CpuCopy);
                if (!owner("resolveTexture"))
                    return;
                auto &d = static_cast<GlTexture &>(dst);
                auto &s = static_cast<GlTexture &>(src);
                if (s.target != GL_TEXTURE_2D_MULTISAMPLE || d.target != GL_TEXTURE_2D || s.desc().width != d.desc().width ||
                    s.desc().height != d.desc().height || s.format.internal != d.format.internal)
                {
                    reject("resolveTexture: '%s' -> '%s' needs a multisampled source and a single-sampled target of the "
                           "same size and format", s.desc().name ? s.desc().name : "", d.desc().name ? d.desc().name : "");
                    return;
                }
                // a multisampled blit to one sample averages the samples (D3D11 ResolveSubresource)
                blit(s, 0, 0, d, 0, 0, s.desc().width, s.desc().height, "resolveTexture");
            }

            void generateMips(Texture &texture) override
            {
                if (!owner("generateMips"))
                    return;
                auto &t = static_cast<GlTexture &>(texture);
                if (t.levels > 1u)
                    mipmaps(t);
            }

            std::unique_ptr<Texture> createTextureLevels(const TextureDesc &desc, const void *const *levels, uint32_t levelCount) override
            {
                CpuScope cpu(this, CpuCreate);
                if (!onOwner() && m_freeThreaded)
                    return createOffOwner(desc, levels, levelCount, 0);
                if (!owner("createTextureLevels"))
                    return nullptr;
                const uint32_t w = std::max(1u, desc.width), h = std::max(1u, desc.height);
                const uint32_t count = std::min(std::max(1u, levelCount), std::min(16u, fullChain(w, h)));
                if (!levels || !checkSize(w, h, desc.name))
                    return nullptr;
                for (uint32_t i = 0; i < count; ++i)
                    if (!levels[i])
                    {
                        reject("createTextureLevels: level %u of '%s' has no data", i, desc.name ? desc.name : "");
                        return nullptr;
                    }
                TextureDesc sized = desc;
                sized.width = w;
                sized.height = h;
                sized.mipLevels = count;
                sized.format = Format::RGBA8;
                sized.usage = UsageSampled;
                auto t = std::make_unique<GlTexture>(m_core, sized);
                t->format = formatInfo(Format::RGBA8);
                t->levels = count;
                glGenTextures(1, &t->name);
                bindScratch(GL_TEXTURE_2D, t->name);
                for (uint32_t i = 0; i < count; ++i)
                    glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(i), GL_RGBA8, std::max(1u, w >> i), std::max(1u, h >> i), 0,
                                 GL_RGBA, GL_UNSIGNED_BYTE, levels[i]);
                defaultParameters(count);
                t->bytes = accountTexture(w, h, 4u, count, 1u);
                label(GL_TEXTURE, t->name, desc.name);
                bindScratch(GL_TEXTURE_2D, 0);
                if (glCheckError("createTextureLevels"))
                    return nullptr;
                return t;
            }

            // GL objects belong to the context current on the owner thread; other threads make textures only through the
            // opt-in loader context (PS2X_GL_WORKER_CONTEXT=1; M05-09 measured both: docs/milestones/M05).
            bool freeThreadedCreation() const override { return m_freeThreaded; }

            std::unique_ptr<Buffer> createBuffer(const BufferDesc &desc, const void *data) override
            {
                if (!owner("createBuffer"))
                    return nullptr;
                if (!desc.size)
                {
                    reject("createBuffer: size 0 ('%s')", desc.name ? desc.name : "");
                    return nullptr;
                }
                auto b = std::make_unique<GlBuffer>(m_core, desc);
                b->target = desc.usage == BufferUsage::Vertex ? GL_ARRAY_BUFFER
                            : desc.usage == BufferUsage::Index ? GL_ELEMENT_ARRAY_BUFFER
                                                               : GL_UNIFORM_BUFFER;
                b->allocated = desc.usage == BufferUsage::Constant ? (desc.size + 15u) & ~size_t(15) : desc.size;
                if (desc.usage == BufferUsage::Constant && b->allocated > static_cast<size_t>(m_caps.maxUniformBlockSize))
                {
                    reject("createBuffer: constant buffer '%s' of %zu bytes above GL_MAX_UNIFORM_BLOCK_SIZE %d",
                           desc.name ? desc.name : "", b->allocated, m_caps.maxUniformBlockSize);
                    return nullptr;
                }
                b->ring = m_ubo.on && desc.dynamic && desc.usage == BufferUsage::Constant && b->allocated <= kUboRingBytes / kUboSegments;
                glGenBuffers(1, &b->name);
                // element array buffers are VAO state: made through GL_COPY_WRITE_BUFFER so no VAO changes
                glBindBuffer(GL_COPY_WRITE_BUFFER, b->name);
                glBufferData(GL_COPY_WRITE_BUFFER, static_cast<GLsizeiptr>(b->allocated), nullptr,
                             desc.dynamic ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
                if (data)
                    glBufferSubData(GL_COPY_WRITE_BUFFER, 0, static_cast<GLsizeiptr>(desc.size), data);
                glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
                label(kLabelBuffer, b->name, desc.name);
                m_core->bufferCount.fetch_add(1, std::memory_order_relaxed);
                m_core->bufferBytes.fetch_add(b->allocated, std::memory_order_relaxed);
                if (glCheckError("createBuffer"))
                    return nullptr;
                return b;
            }

            void updateBuffer(Buffer &buffer, const void *data, size_t size) override
            {
                if (!owner("updateBuffer"))
                    return;
                auto &b = static_cast<GlBuffer &>(buffer);
                CpuScope cpu(this, b.ring ? CpuConst : CpuBuffer); // constants (the ring) apart from other buffers
                if (!b.ring && cpuTimeOn())
                    m_cpuBufferBytes += size;
                if (!data || !size)
                    return;
                if (size > b.desc().size)
                {
                    // D3D11 copies min(size, buffer size) for a dynamic buffer; the excess is a caller error either way
                    reject("updateBuffer: %zu bytes into '%s' of %zu: clamped", size, b.desc().name ? b.desc().name : "",
                           b.desc().size);
                    size = b.desc().size;
                }
                if (b.ring)
                {
                    ringWrite(b, data, size);
                    return;
                }
                glBindBuffer(GL_COPY_WRITE_BUFFER, b.name);
                if (b.desc().dynamic)
                {
                    // WRITE_DISCARD: new storage, the rest undefined; draws already submitted keep reading the old
                    glBufferData(GL_COPY_WRITE_BUFFER, static_cast<GLsizeiptr>(b.allocated), nullptr, GL_DYNAMIC_DRAW);
                }
                glBufferSubData(GL_COPY_WRITE_BUFFER, 0, static_cast<GLsizeiptr>(size), data);
                glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
            }

            std::unique_ptr<Sampler> createSampler(const SamplerDesc &desc) override
            {
                if (!owner("createSampler"))
                    return nullptr;
                auto s = std::make_unique<GlSampler>(m_core);
                glGenSamplers(1, &s->name);
                const bool linear = desc.filter == Filter::Linear;
                // D3D11 MIN_MAG_MIP_LINEAR / _POINT; a texture of one level ignores the mip filter
                glSamplerParameteri(s->name, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
                glSamplerParameteri(s->name, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
                glSamplerParameteri(s->name, GL_TEXTURE_WRAP_S, desc.wrapU == Wrap::Repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
                glSamplerParameteri(s->name, GL_TEXTURE_WRAP_T, desc.wrapV == Wrap::Repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
                glSamplerParameteri(s->name, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
                glSamplerParameterf(s->name, GL_TEXTURE_MIN_LOD, 0.0f); // D3D11 MinLOD 0 (zeroed desc), MaxLOD FLT_MAX
                glSamplerParameterf(s->name, GL_TEXTURE_MAX_LOD, 1000.0f);
                glSamplerParameteri(s->name, GL_TEXTURE_COMPARE_MODE, GL_NONE);
                if (linear && desc.anisotropy > 1u)
                {
                    if (m_caps.anisotropy)
                        glSamplerParameterf(s->name, kTextureMaxAnisotropy,
                                            std::min(static_cast<float>(std::min(desc.anisotropy, 16u)), m_caps.maxAnisotropy));
                    else
                        warnOnce(m_warnedAnisotropy, "anisotropic filtering asked for but the context has none: trilinear");
                }
                label(kLabelSampler, s->name, nullptr);
                m_core->samplerCount.fetch_add(1, std::memory_order_relaxed);
                if (glCheckError("createSampler"))
                    return nullptr;
                return s;
            }

            std::unique_ptr<Pipeline> createPipeline(const PipelineDesc &desc) override
            {
                if (!owner("createPipeline"))
                    return nullptr;
                std::shared_ptr<Program> program = programFor(desc);
                if (!program)
                    return nullptr;
                if (desc.attributes.empty() != (desc.stride == 0))
                {
                    reject("createPipeline '%s': attributes and stride must be given together", desc.name ? desc.name : "?");
                    return nullptr;
                }
                for (const VertexAttribute &a : desc.attributes)
                    if (a.offset + vertexFormat(a.format).bytes > desc.stride)
                    {
                        reject("createPipeline '%s': attribute at %u past the %u-byte vertex", desc.name ? desc.name : "?",
                               a.offset, desc.stride);
                        return nullptr;
                    }
                const bool dual = desc.blend.enable && (dualSource(desc.blend.srcColor) || dualSource(desc.blend.dstColor) ||
                                                         dualSource(desc.blend.srcAlpha) || dualSource(desc.blend.dstAlpha));
                if (dual && !program->dualOutput)
                {
                    // GL leaves Src1 factors undefined without the index-1 output; D3D11 reads zero (no visual guess here)
                    reject("createPipeline '%s': Src1 blend factors with a pixel shader without the second output",
                           desc.name ? desc.name : "?");
                    return nullptr;
                }
                auto p = std::make_unique<GlPipeline>(m_core);
                p->serial = ++m_pipelineSerial;
                p->program = std::move(program);
                p->attributes = desc.attributes;
                p->stride = desc.stride;
                p->mode = topology(desc.topology);
                p->blend = desc.blend;
                p->depth = desc.depth;
                p->depthClamp = !desc.depthClip;
                p->name = desc.name ? desc.name : "";
                if (!desc.attributes.empty())
                {
                    glGenVertexArrays(1, &p->vao);
                    glBindVertexArray(p->vao);
                    for (size_t i = 0; i < desc.attributes.size(); ++i)
                        glEnableVertexAttribArray(static_cast<GLuint>(i));
                    glBindVertexArray(m_boundVao);
                }
                m_core->pipelineCount.fetch_add(1, std::memory_order_relaxed);
                if (glCheckError("createPipeline"))
                    return nullptr;
                return p;
            }

            // ---- drawing ------------------------------------------------------------------------------------------
            void beginPass(const PassDesc &pass) override
            {
                CpuScope cpu(this, CpuPass);
                if (!owner("beginPass"))
                    return;
                if (m_inPass)
                    reject("beginPass '%s' inside another pass", pass.name ? pass.name : "");
                m_inPass = true;
                ++m_passes;
                GlTexture *color = pass.depthOnly ? nullptr : static_cast<GlTexture *>(pass.color);
                GlTexture *depth = static_cast<GlTexture *>(pass.depth);
                if (!pass.depthOnly && !color && m_offscreen)
                    color = m_offscreenBack.get();
                m_passColor = color ? color->name : 0u;
                m_passDepth = depth ? depth->name : 0u;
                uint32_t w = m_width, h = m_height;
                m_toWindow = !pass.depthOnly && !color;
                if (m_toWindow)
                {
                    refreshWindowSize();
                    w = m_width;
                    h = m_height;
                    if (depth)
                        reject("beginPass '%s': a depth texture with the window's back buffer (unsupported in GL)",
                               pass.name ? pass.name : "");
                    bindFramebuffer(0);
                    glDrawBuffer(GL_BACK);
                }
                else
                {
                    const GlTexture *sizeFrom = color ? color : depth;
                    if (!sizeFrom)
                    {
                        reject("beginPass '%s': no target", pass.name ? pass.name : "");
                        w = h = 1;
                    }
                    else
                    {
                        w = sizeFrom->desc().width;
                        h = sizeFrom->desc().height;
                    }
                    if (color && depth && (color->desc().width != depth->desc().width || color->desc().height != depth->desc().height ||
                                           color->desc().samples != depth->desc().samples))
                        reject("beginPass '%s': colour and depth targets differ in size or samples", pass.name ? pass.name : "");
                    bindFramebuffer(framebufferFor(color, depth));
                }
                m_passWidth = w;
                m_passHeight = h;
                // Rows top-down in a texture. With clip control the rasteriser itself works upper-left there
                // (glClipControl(GL_UPPER_LEFT)): NDC +1 lands on row 0 and the fill rule's tie-breaking follows
                // D3D's top-left rule. A Y flip in the shader gives the same rows but the GL lower-left fill rule:
                // an edge exactly on a pixel centre (every GS edge: gs_hw.cpp moves geometry by half a pixel) lost
                // the top row and gained the bottom one on nouveau (first M05 review). The flip stays only for
                // contexts without clip control.
                if (m_caps.clipControl && m_clipControl)
                {
                    const GLenum origin = m_toWindow ? kLowerLeft : kUpperLeft;
                    if (origin != m_clipOrigin)
                    {
                        m_clipControl(origin, kZeroToOne);
                        m_clipOrigin = origin;
                    }
                    m_flip = 1.0f;
                }
                else
                    m_flip = m_toWindow ? 1.0f : -1.0f;
                // a target may still be bound as a shader input from an earlier pass (D3D11 beginPass unbinds them)
                unbindTextures();
                if (pass.name)
                    pushMarker(pass.name);
                m_passMarker = pass.name != nullptr;
                // clears ignore the pipeline's write masks and the scissor (D3D11 ClearRenderTargetView)
                if ((pass.clearColor && (color || m_toWindow)) || (pass.clearDepth && depth))
                {
                    glDisable(GL_SCISSOR_TEST);
                    if (pass.clearColor && (color || m_toWindow))
                    {
                        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                        glClearBufferfv(GL_COLOR, 0, pass.clearRgba);
                    }
                    if (pass.clearDepth && depth)
                    {
                        glDepthMask(GL_TRUE);
                        glClearBufferfv(GL_DEPTH, 0, &pass.clearDepthValue);
                    }
                    glEnable(GL_SCISSOR_TEST);
                    m_pipelineStateValid = false;
                }
                setViewport(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
                setScissor(0, 0, static_cast<int>(w), static_cast<int>(h));
                if (m_passTime)
                    beginPassTime(pass.name, w, h, m_toWindow);
            }

            void endPass() override
            {
                if (!owner("endPass"))
                    return;
                if (m_passTime)
                    endPassTime();
                if (m_passMarker)
                    popMarker();
                m_passMarker = false;
                m_inPass = false;
            }

            void setViewport(float x, float y, float width, float height) override
            {
                if (!owner("setViewport"))
                    return;
                // the window counts rows from the bottom; textures keep D3D's top-left rectangles (rows top-down)
                const float gy = m_toWindow ? static_cast<float>(m_passHeight) - y - height : y;
                if (m_viewportIndexedf)
                    m_viewportIndexedf(0, x, gy, width, height);
                else
                {
                    const bool whole = x == static_cast<float>(static_cast<int>(x)) && gy == static_cast<float>(static_cast<int>(gy)) &&
                                       width == static_cast<float>(static_cast<int>(width)) &&
                                       height == static_cast<float>(static_cast<int>(height));
                    if (!whole)
                        warnOnce(m_warnedViewport, "a fractional viewport rounded (no GL 4.1 / ARB_viewport_array)");
                    glViewport(static_cast<GLint>(x), static_cast<GLint>(gy), static_cast<GLsizei>(width), static_cast<GLsizei>(height));
                }
                glDepthRange(0.0, 1.0);
            }

            void setScissor(int x, int y, int width, int height) override
            {
                if (!owner("setScissor"))
                    return;
                const int gy = m_toWindow ? static_cast<int>(m_passHeight) - y - height : y;
                glScissor(x, gy, std::max(0, width), std::max(0, height));
            }

            void setBlendConstant(const float rgba[4]) override
            {
                if (!owner("setBlendConstant"))
                    return;
                glBlendColor(rgba[0], rgba[1], rgba[2], rgba[3]);
            }

            void setPipeline(Pipeline &pipeline) override
            {
                if (!owner("setPipeline"))
                    return;
                auto &p = static_cast<GlPipeline &>(pipeline);
                if (m_pipeline == &p && m_pipelineSerialBound == p.serial && m_pipelineStateValid)
                    return;
                m_pipeline = &p;
                m_pipelineSerialBound = p.serial;
                m_pipelineStateValid = true;
                if (m_boundProgram != p.program->name)
                {
                    glUseProgram(p.program->name);
                    m_boundProgram = p.program->name;
                }
                bindVao(p.vao ? p.vao : m_emptyVao);
                const BlendState &b = p.blend;
                if (b.enable)
                {
                    glEnable(GL_BLEND);
                    glBlendFuncSeparate(blendFactor(b.srcColor), blendFactor(b.dstColor), blendFactor(b.srcAlpha),
                                        blendFactor(b.dstAlpha));
                    glBlendEquationSeparate(blendOp(b.colorOp), blendOp(b.alphaOp));
                }
                else
                    glDisable(GL_BLEND);
                glColorMask((b.writeMask & 1) != 0, (b.writeMask & 2) != 0, (b.writeMask & 4) != 0, (b.writeMask & 8) != 0);
                // D3D11 DepthEnable = test || write; the comparison ALWAYS when only writing
                if (p.depth.test || p.depth.write)
                {
                    glEnable(GL_DEPTH_TEST);
                    glDepthFunc(p.depth.test ? compare(p.depth.func) : GL_ALWAYS);
                }
                else
                    glDisable(GL_DEPTH_TEST);
                glDepthMask(p.depth.write ? GL_TRUE : GL_FALSE);
                if (p.depthClamp)
                    glEnable(GL_DEPTH_CLAMP);
                else
                    glDisable(GL_DEPTH_CLAMP);
                for (GLenum plane : {GL_CLIP_DISTANCE0, GL_CLIP_DISTANCE1})
                {
                    if (p.program->clipDistances)
                        glEnable(plane);
                    else
                        glDisable(plane);
                }
            }

            void setVertexBuffer(Buffer &buffer, uint32_t offset) override
            {
                if (!owner("setVertexBuffer"))
                    return;
                auto &b = static_cast<GlBuffer &>(buffer);
                if (b.target != GL_ARRAY_BUFFER || offset > b.desc().size)
                {
                    reject("setVertexBuffer: '%s' is not a vertex buffer or offset %u is past its end",
                           b.desc().name ? b.desc().name : "", offset);
                    m_vertexBuffer = {};
                    return;
                }
                m_vertexBuffer = {b.name, b.desc().size};
                m_vertexBufferName = b.desc().name ? b.desc().name : "";
                m_vertexOffset = offset;
            }

            void setIndexBuffer(Buffer &buffer, bool uint32Indices) override
            {
                if (!owner("setIndexBuffer"))
                    return;
                auto &b = static_cast<GlBuffer &>(buffer);
                if (b.target != GL_ELEMENT_ARRAY_BUFFER)
                {
                    reject("setIndexBuffer: '%s' is not an index buffer", b.desc().name ? b.desc().name : "");
                    m_indexBuffer = {};
                    return;
                }
                m_indexBuffer = {b.name, b.desc().size};
                m_index32 = uint32Indices;
            }

            void setTexture(uint32_t slot, Texture *texture, Sampler *sampler) override
            {
                if (!owner("setTexture"))
                    return;
                if (slot >= kUnits)
                {
                    reject("setTexture: slot %u", slot);
                    return;
                }
                auto *t = static_cast<GlTexture *>(texture);
                if (t && !(t->desc().usage & UsageSampled) && t->levels == 1u)
                {
                    reject("setTexture: '%s' was not made with UsageSampled", t->desc().name ? t->desc().name : "");
                    t = nullptr;
                }
                if (t)
                    ready(*t);
                glActiveTexture(GL_TEXTURE0 + slot);
                if (m_units[slot].name && (!t || m_units[slot].target != t->target))
                    glBindTexture(m_units[slot].target, 0);
                if (t)
                    glBindTexture(t->target, t->name);
                glBindSampler(slot, sampler && t ? static_cast<GlSampler *>(sampler)->name : 0u);
                m_units[slot] = t ? BoundTexture{t->name, t->target} : BoundTexture{};
                m_unitNames[slot] = t && t->desc().name ? t->desc().name : "";
            }

            void setConstants(uint32_t slot, Buffer &buffer) override
            {
                if (!owner("setConstants"))
                    return;
                auto &b = static_cast<GlBuffer &>(buffer);
                if (slot >= kBlocks || b.target != GL_UNIFORM_BUFFER)
                {
                    reject("setConstants: slot %u or '%s' is not a constant buffer", slot, b.desc().name ? b.desc().name : "");
                    return;
                }
                if (b.ring && b.ringValid)
                    glBindBufferRange(GL_UNIFORM_BUFFER, slot, m_ubo.name, static_cast<GLintptr>(b.ringOffset),
                                      static_cast<GLsizeiptr>(b.allocated));
                else
                    glBindBufferBase(GL_UNIFORM_BUFFER, slot, b.name);
                m_blocks[slot] = {b.name, b.allocated};
            }

            void draw(uint32_t vertexCount, uint32_t firstVertex) override
            {
                CpuScope cpu(this, CpuDraw);
                if (!owner("draw") || !vertexCount || !prepareDraw(static_cast<uint64_t>(firstVertex) + vertexCount))
                    return;
                glDrawArrays(m_pipeline->mode, static_cast<GLint>(firstVertex), static_cast<GLsizei>(vertexCount));
                ++m_draws;
            }

            void drawIndexed(uint32_t indexCount, uint32_t firstIndex, int32_t baseVertex) override
            {
                CpuScope cpu(this, CpuDraw);
                if (!owner("drawIndexed") || !indexCount)
                    return;
                const uint32_t isize = m_index32 ? 4u : 2u;
                if (!m_indexBuffer.name || (static_cast<uint64_t>(firstIndex) + indexCount) * isize > m_indexBuffer.size)
                {
                    reject("drawIndexed: %u indices from %u past the index buffer or none bound", indexCount, firstIndex);
                    return;
                }
                if (!prepareDraw(0))
                    return;
                glDrawElementsBaseVertex(m_pipeline->mode, static_cast<GLsizei>(indexCount), m_index32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT,
                                         reinterpret_cast<const void *>(static_cast<uintptr_t>(firstIndex) * isize), baseVertex);
                ++m_draws;
            }

            void pushMarker(const char *name) override
            {
                if (m_pushDebugGroup && onOwner())
                    m_pushDebugGroup(kDebugSourceApplication, 0, -1, name ? name : "?");
            }

            void popMarker() override
            {
                if (m_popDebugGroup && onOwner())
                    m_popDebugGroup();
            }

            // ---- swapchain ----------------------------------------------------------------------------------------
            void backbufferSize(uint32_t &width, uint32_t &height) const override
            {
                width = m_width;
                height = m_height;
            }

            void resize(uint32_t width, uint32_t height) override
            {
                if (!owner("resize") || !width || !height)
                    return;
                if (m_offscreen)
                {
                    if (width == m_width && height == m_height)
                        return;
                    m_width = width;
                    m_height = height;
                    makeOffscreenBack();
                    return;
                }
                // SDL owns the window's surface: the back buffer is the drawable it reports (it follows the dock), the
                // asked size only matters when there is no report
                m_width = width;
                m_height = height;
                refreshWindowSize();
            }

            void present(PresentMode mode) override
            {
                if (!owner("present"))
                    return;
                if (m_inPass)
                    reject("present inside a pass");
                ++m_presents;
                if (!m_offscreen)
                {
                    const int interval = mode == PresentMode::Vsync ? 1 : 0; // Tearing falls back to Immediate
                    if (interval != m_swapInterval && m_window.setSwapInterval)
                    {
                        // Through 0 first: on the Switch (Mesa 20.1 / switch-sdl2) a change from interval 2 straight to
                        // 1 is reported by SDL but not applied (30 presents a second remained); 2 -> 0 and 0 -> 1 are
                        // (M05 review 1, the present-interval probe in src/rhi_tests/switch_main.cpp)
                        const bool ok = (interval == 0 || m_window.setSwapInterval(0)) && m_window.setSwapInterval(interval);
                        if (!ok)
                            std::fprintf(stderr, "[rhi] swap interval %d refused\n", interval);
                        m_swapInterval = interval;
                    }
                    if (m_gpuTime)
                        endGpuFrame();
                    {
                        CpuScope cpu(this, CpuSwap);
                        m_window.swap();
                    }
                    m_pipelineStateValid = false; // the platform's swap may touch state
                    m_boundFramebuffer = ~0u;
                    if (m_gpuTime)
                        beginGpuFrame();
                }
                drain();
                checkErrors("frame");
                if (m_passTime)
                    collectPassTimes();
                reportStats();
            }

            // ---- Dear ImGui: drawn through the RHI by render::UiRenderer (as the Vulkan backend) ---------------------
            bool uiInit() override { return false; }
            void uiNewFrame() override {}
            void uiRender(void *) override {}
            void uiShutdown() override {}

            // ---- port diagnostics (rhi_gl.h) ----------------------------------------------------------------------
            const GlCaps &caps() const { return m_caps; }

            GlStats stats() const
            {
                GlStats s = m_stats;
                s.textures = m_core->textureCount.load();
                s.textureBytes = m_core->textureBytes.load();
                s.buffers = m_core->bufferCount.load();
                s.bufferBytes = m_core->bufferBytes.load();
                s.samplers = m_core->samplerCount.load();
                s.pipelines = m_core->pipelineCount.load();
                s.targets = m_core->targetCount.load();
                s.targetBytes = m_core->targetBytes.load();
                s.programs = m_programs.size();
                s.framebuffers = m_fbos.size();
                {
                    std::lock_guard<std::mutex> lock(m_core->mutex);
                    s.pendingDeletes = m_core->textures.size() + m_core->buffers.size() + m_core->samplers.size() + m_core->vaos.size();
                    for (const auto &[size, count] : m_core->targetSizes)
                        if (static_cast<uint64_t>(size.first) * size.second >
                            static_cast<uint64_t>(s.largestTargetWidth) * s.largestTargetHeight)
                        {
                            s.largestTargetWidth = size.first;
                            s.largestTargetHeight = size.second;
                        }
                }
                s.offOwnerCreated = m_offOwnerCreated.load();
                s.offOwnerRefused = m_offOwnerRefused.load();
                s.fencesWaited = m_fencesWaited;
                s.drawCalls = m_draws;
                s.passes = m_passes;
                s.presents = m_presents;
                return s;
            }

            uint32_t checkErrors(const char *where)
            {
                if (!onOwner())
                    return 0;
                uint32_t n = 0;
                for (GLenum e = glGetError(); e != GL_NO_ERROR && n < 64; e = glGetError(), ++n)
                {
                    ++m_stats.glErrors;
                    if (m_stats.glErrors <= 50u)
                        std::fprintf(stderr, "[rhi] %s after %s\n", errorName(e), where);
                }
                return n;
            }

        private:
            static constexpr uint32_t kUnits = 8;
            static constexpr uint32_t kBlocks = 8;

            bool onOwner() const { return std::this_thread::get_id() == m_core->owner; }

            bool owner(const char *what)
            {
                if (onOwner())
                    return true;
                const uint64_t n = ++m_ownerViolations;
                if (n <= 20u)
                    std::fprintf(stderr, "[rhi] OpenGL %s called off the owner thread: refused (%llu so far)\n", what,
                                 static_cast<unsigned long long>(n));
                return false;
            }

            __attribute__((format(printf, 2, 3))) void reject(const char *format, ...)
            {
                ++m_stats.rejected;
                if (m_stats.rejected <= 100u)
                {
                    va_list args;
                    va_start(args, format);
                    std::fprintf(stderr, "[rhi] refused: ");
                    std::vfprintf(stderr, format, args);
                    std::fputc('\n', stderr);
                    va_end(args);
                }
            }

            void warnOnce(bool &flag, const char *text)
            {
                if (!flag)
                    std::fprintf(stderr, "[rhi] %s\n", text);
                flag = true;
            }

            uint32_t glCheckError(const char *where) { return checkErrors(where); }

            void readCaps()
            {
                glGetIntegerv(GL_MAJOR_VERSION, &m_caps.major);
                glGetIntegerv(GL_MINOR_VERSION, &m_caps.minor);
                auto str = [](GLenum e) {
                    const GLubyte *s = glGetString(e);
                    return std::string(s ? reinterpret_cast<const char *>(s) : "?");
                };
                m_caps.vendor = str(GL_VENDOR);
                m_caps.renderer = str(GL_RENDERER);
                m_caps.version = str(GL_VERSION);
                m_caps.glsl = str(GL_SHADING_LANGUAGE_VERSION);
                GLint count = 0;
                glGetIntegerv(GL_NUM_EXTENSIONS, &count);
                std::vector<std::string> extensions;
                for (GLint i = 0; i < count; ++i)
                    if (const GLubyte *e = glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)))
                        extensions.emplace_back(reinterpret_cast<const char *>(e));
                auto has = [&](const char *name) { return std::find(extensions.begin(), extensions.end(), name) != extensions.end(); };
                const int version = m_caps.major * 10 + m_caps.minor;
                auto load = [&](const char *name) { return m_window.getProcAddress(name); };
                if ((version >= 45 || has("GL_ARB_clip_control")) && !envOn("PS2X_GL_NO_CLIP_CONTROL"))
                {
                    m_clipControl = reinterpret_cast<ClipControlFn>(load("glClipControl"));
                    m_caps.clipControl = m_clipControl != nullptr;
                }
                if (version >= 44 || has("GL_ARB_buffer_storage"))
                    m_bufferStorage = reinterpret_cast<BufferStorageFn>(load("glBufferStorage"));
                if ((version >= 41 || has("GL_ARB_viewport_array")) && !envOn("PS2X_GL_NO_VIEWPORT_ARRAY"))
                {
                    m_viewportIndexedf = reinterpret_cast<ViewportIndexedfFn>(load("glViewportIndexedf"));
                    m_caps.viewportArray = m_viewportIndexedf != nullptr;
                }
                m_caps.anisotropy = version >= 46 || has("GL_EXT_texture_filter_anisotropic") || has("GL_ARB_texture_filter_anisotropic");
                if (m_caps.anisotropy)
                    glGetFloatv(kMaxTextureMaxAnisotropy, &m_caps.maxAnisotropy);
                if (version >= 43 || has("GL_KHR_debug"))
                {
                    m_debugMessageCallback = reinterpret_cast<DebugMessageCallbackFn>(load("glDebugMessageCallback"));
                    m_pushDebugGroup = reinterpret_cast<PushDebugGroupFn>(load("glPushDebugGroup"));
                    m_popDebugGroup = reinterpret_cast<PopDebugGroupFn>(load("glPopDebugGroup"));
                    m_objectLabel = reinterpret_cast<ObjectLabelFn>(load("glObjectLabel"));
                    m_caps.khrDebug = m_debugMessageCallback && m_pushDebugGroup && m_popDebugGroup && m_objectLabel;
                    if (!m_caps.khrDebug)
                        m_pushDebugGroup = nullptr, m_popDebugGroup = nullptr, m_objectLabel = nullptr;
                }
                glGetIntegerv(GL_MAX_DUAL_SOURCE_DRAW_BUFFERS, &m_caps.maxDualSourceDrawBuffers);
                glGetIntegerv(GL_MAX_SAMPLES, &m_caps.maxSamples);
                glGetIntegerv(GL_MAX_COLOR_TEXTURE_SAMPLES, &m_caps.maxColorTextureSamples);
                glGetIntegerv(GL_MAX_DEPTH_TEXTURE_SAMPLES, &m_caps.maxDepthTextureSamples);
                glGetIntegerv(GL_MAX_TEXTURE_SIZE, &m_caps.maxTextureSize);
                glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &m_caps.maxUniformBlockSize);
                glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &m_caps.maxTextureUnits);
                GLint encoding = 0;
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_BACK_LEFT, GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &encoding);
                if (glGetError() == GL_NO_ERROR) // a surfaceless context has no back buffer
                    m_caps.defaultFramebufferEncoding = encoding;
            }

            static void debugMessage(GLenum, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar *message,
                                     const void *user)
            {
                auto *self = const_cast<GlDevice *>(static_cast<const GlDevice *>(user));
                if (severity != kDebugSeverityHigh && severity != kDebugSeverityMedium)
                    return;
                const uint64_t n = ++self->m_stats.debugMessages;
                if (n <= 50u)
                    std::fprintf(stderr, "[rhi] gl debug (type 0x%x id %u severity 0x%x): %.*s\n", type, id, severity,
                                 length < 0 ? static_cast<int>(std::strlen(message)) : static_cast<int>(length), message);
            }

            void setUpDebug(bool on)
            {
                if (!on || !m_debugMessageCallback)
                    return;
                glEnable(kDebugOutput);
                glEnable(kDebugOutputSynchronous);
                m_debugMessageCallback(debugMessage, this);
                std::fprintf(stderr, "[rhi] GL debug output on (KHR_debug, synchronous)\n");
            }

            void label(GLenum kind, GLuint name, const char *text)
            {
                if (m_objectLabel && text && *text)
                    m_objectLabel(kind, name, -1, text);
            }

            bool checkSize(uint32_t w, uint32_t h, const char *name)
            {
                if (static_cast<int>(w) <= m_caps.maxTextureSize && static_cast<int>(h) <= m_caps.maxTextureSize)
                    return true;
                reject("texture '%s' %ux%u above GL_MAX_TEXTURE_SIZE %d", name ? name : "", w, h, m_caps.maxTextureSize);
                return false;
            }

            static bool inside(const GlTexture &t, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
            {
                return w && h && static_cast<uint64_t>(x) + w <= t.desc().width && static_cast<uint64_t>(y) + h <= t.desc().height;
            }

            uint64_t accountTexture(uint32_t w, uint32_t h, uint32_t bpp, uint32_t levels, uint32_t samples)
            {
                uint64_t bytes = 0;
                for (uint32_t i = 0; i < levels; ++i)
                    bytes += static_cast<uint64_t>(std::max(1u, w >> i)) * std::max(1u, h >> i) * bpp;
                bytes *= samples;
                m_core->textureCount.fetch_add(1, std::memory_order_relaxed);
                m_core->textureBytes.fetch_add(bytes, std::memory_order_relaxed);
                return bytes;
            }

            void bindScratch(GLenum target, GLuint name)
            {
                glActiveTexture(GL_TEXTURE0 + m_scratchUnit);
                glBindTexture(target, name);
            }

            void defaultParameters(uint32_t levels)
            {
                // complete with every allocated level, whatever sampler reads it (texelFetch needs completeness too)
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(levels - 1u));
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, levels > 1u ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            }

            void upload(GlTexture &t, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void *data, uint32_t rowPitch)
            {
                const uint32_t bpp = t.format.bytes;
                const uint32_t pitch = rowPitch ? rowPitch : w * bpp;
                if (pitch < w * bpp)
                {
                    reject("texture upload: row pitch %u below a row's %u bytes", pitch, w * bpp);
                    return;
                }
                bindScratch(GL_TEXTURE_2D, t.name);
                if (pitch % bpp == 0)
                {
                    glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(pitch / bpp));
                    glTexSubImage2D(GL_TEXTURE_2D, static_cast<GLint>(level), static_cast<GLint>(x), static_cast<GLint>(y),
                                    static_cast<GLsizei>(w), static_cast<GLsizei>(h), t.format.format, t.format.type, data);
                    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                }
                else
                {
                    std::vector<uint8_t> rows(static_cast<size_t>(w) * h * bpp);
                    for (uint32_t row = 0; row < h; ++row)
                        std::memcpy(rows.data() + static_cast<size_t>(row) * w * bpp,
                                    static_cast<const uint8_t *>(data) + static_cast<size_t>(row) * pitch, static_cast<size_t>(w) * bpp);
                    glTexSubImage2D(GL_TEXTURE_2D, static_cast<GLint>(level), static_cast<GLint>(x), static_cast<GLint>(y),
                                    static_cast<GLsizei>(w), static_cast<GLsizei>(h), t.format.format, t.format.type, rows.data());
                }
                bindScratch(GL_TEXTURE_2D, 0);
            }

            // a texture made on the loader context: the GPU waits for its upload before the owner's next commands
            void ready(GlTexture &t)
            {
                if (!t.fence)
                    return;
                glWaitSync(t.fence, 0, GL_TIMEOUT_IGNORED);
                glDeleteSync(t.fence);
                t.fence = nullptr;
                ++m_fencesWaited;
            }

            // createTexture / createTextureLevels off the owner thread (freeThreadedCreation): on the loader context,
            // one creating thread at a time; nothing of the owner's state is touched (counts are atomics)
            std::unique_ptr<Texture> createOffOwner(const TextureDesc &desc, const void *const *levels, uint32_t levelCount, uint32_t rowPitch)
            {
                const uint32_t w = std::max(1u, desc.width), h = std::max(1u, desc.height);
                const uint32_t count = std::min(std::max(1u, levelCount), std::min(16u, fullChain(w, h)));
                if (!levels || static_cast<int>(w) > m_caps.maxTextureSize || static_cast<int>(h) > m_caps.maxTextureSize)
                {
                    ++m_offOwnerRefused;
                    return nullptr;
                }
                for (uint32_t i = 0; i < count; ++i)
                    if (!levels[i])
                    {
                        ++m_offOwnerRefused;
                        return nullptr;
                    }
                TextureDesc sized = desc;
                sized.width = w;
                sized.height = h;
                sized.mipLevels = count;
                sized.format = Format::RGBA8;
                sized.usage = UsageSampled;
                std::lock_guard<std::mutex> lock(m_loaderMutex);
                if (!m_window.makeLoaderCurrent(true))
                {
                    ++m_offOwnerRefused;
                    return nullptr;
                }
                auto t = std::make_unique<GlTexture>(m_core, sized);
                t->format = formatInfo(Format::RGBA8);
                t->levels = count;
                t->bytes = accountTexture(w, h, 4u, count, 1u); // counted from here: the destructor uncounts it
                glGenTextures(1, &t->name);
                glBindTexture(GL_TEXTURE_2D, t->name);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, rowPitch && count == 1u ? static_cast<GLint>(rowPitch / 4u) : 0);
                for (uint32_t i = 0; i < count; ++i)
                    glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(i), GL_RGBA8, std::max(1u, w >> i), std::max(1u, h >> i), 0,
                                 GL_RGBA, GL_UNSIGNED_BYTE, levels[i]);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                defaultParameters(count);
                glBindTexture(GL_TEXTURE_2D, 0);
                label(GL_TEXTURE, t->name, desc.name);
                t->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                glFlush(); // the fence must reach the GPU before another context waits on it
                const GLenum error = glGetError();
                m_window.makeLoaderCurrent(false);
                ++m_offOwnerCreated;
                if (error != GL_NO_ERROR)
                {
                    std::fprintf(stderr, "[rhi] %s creating a texture on the loader context\n", errorName(error));
                    ++m_offOwnerRefused;
                    return nullptr;
                }
                return t;
            }

            void mipmaps(GlTexture &t)
            {
                bindScratch(GL_TEXTURE_2D, t.name);
                glGenerateMipmap(GL_TEXTURE_2D);
                bindScratch(GL_TEXTURE_2D, 0);
            }

            // ---- framebuffers -------------------------------------------------------------------------------------
            void bindFramebuffer(GLuint fbo)
            {
                if (m_boundFramebuffer != fbo)
                {
                    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                    m_boundFramebuffer = fbo;
                }
            }

            void restoreFramebuffer()
            {
                glBindFramebuffer(GL_FRAMEBUFFER, m_boundFramebuffer == ~0u ? 0u : m_boundFramebuffer);
            }

            static void attach(GLenum target, const GlTexture *t)
            {
                const GLenum point = t->format.depth ? GL_DEPTH_ATTACHMENT : GL_COLOR_ATTACHMENT0;
                glFramebufferTexture2D(target, point, t->target, t->name, 0);
            }

            bool complete(GLenum target, const char *what)
            {
                const GLenum status = glCheckFramebufferStatus(target);
                if (status == GL_FRAMEBUFFER_COMPLETE)
                    return true;
                ++m_stats.incompleteFramebuffers;
                if (m_stats.incompleteFramebuffers <= 20u)
                    std::fprintf(stderr, "[rhi] incomplete framebuffer (0x%x) for %s\n", status, what);
                return false;
            }

            GLuint framebufferFor(GlTexture *color, GlTexture *depth)
            {
                const auto key = std::make_pair(color ? color->name : 0u, depth ? depth->name : 0u);
                auto it = m_fbos.find(key);
                if (it != m_fbos.end())
                    return it->second;
                GLuint fbo = 0;
                glGenFramebuffers(1, &fbo);
                glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                if (color)
                    attach(GL_FRAMEBUFFER, color);
                if (depth)
                    attach(GL_FRAMEBUFFER, depth);
                const GLenum buffer = GL_COLOR_ATTACHMENT0;
                if (color)
                    glDrawBuffers(1, &buffer);
                else
                {
                    glDrawBuffer(GL_NONE);
                    glReadBuffer(GL_NONE);
                }
                complete(GL_FRAMEBUFFER, "a pass");
                m_boundFramebuffer = fbo;
                m_fbos.emplace(key, fbo);
                return fbo;
            }

            void attachCopy(GLenum target, GLuint fbo, const GlTexture &t)
            {
                glBindFramebuffer(target, fbo);
                // one attachment at a time: the other point cleared so a colour copy never sees an old depth
                glFramebufferTexture2D(target, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
                glFramebufferTexture2D(target, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
                attach(target, &t);
                if (target == GL_READ_FRAMEBUFFER)
                    glReadBuffer(t.format.depth ? GL_NONE : GL_COLOR_ATTACHMENT0);
                else
                {
                    const GLenum buffer = t.format.depth ? GL_NONE : GL_COLOR_ATTACHMENT0;
                    glDrawBuffers(1, &buffer);
                }
            }

            void blit(GlTexture &s, uint32_t sx, uint32_t sy, GlTexture &d, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h,
                      const char *what)
            {
                attachCopy(GL_READ_FRAMEBUFFER, m_copyFbo[0], s);
                attachCopy(GL_DRAW_FRAMEBUFFER, m_copyFbo[1], d);
                if (complete(GL_READ_FRAMEBUFFER, what) && complete(GL_DRAW_FRAMEBUFFER, what))
                {
                    // a copy ignores the scissor and the write masks (D3D11 CopySubresourceRegion)
                    glDisable(GL_SCISSOR_TEST);
                    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                    glDepthMask(GL_TRUE);
                    const GLbitfield mask = s.format.depth ? GL_DEPTH_BUFFER_BIT : GL_COLOR_BUFFER_BIT;
                    glBlitFramebuffer(static_cast<GLint>(sx), static_cast<GLint>(sy), static_cast<GLint>(sx + w), static_cast<GLint>(sy + h),
                                      static_cast<GLint>(dx), static_cast<GLint>(dy), static_cast<GLint>(dx + w), static_cast<GLint>(dy + h),
                                      mask, GL_NEAREST);
                    glEnable(GL_SCISSOR_TEST);
                    m_pipelineStateValid = false;
                }
                detachCopy(GL_READ_FRAMEBUFFER, m_copyFbo[0]);
                detachCopy(GL_DRAW_FRAMEBUFFER, m_copyFbo[1]);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
                restoreFramebuffer();
                glCheckError(what);
            }

            // a copy / readback framebuffer keeps nothing attached after use: GL keeps a released texture's storage
            // while an unbound framebuffer still references it (first M05 review)
            static void detachCopy(GLenum target, GLuint fbo)
            {
                glBindFramebuffer(target, fbo);
                glFramebufferTexture2D(target, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
                glFramebufferTexture2D(target, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
            }

            bool makeOffscreenBack()
            {
                TextureDesc td;
                td.width = m_width;
                td.height = m_height;
                td.usage = UsageRenderTarget | UsageSampled;
                td.name = "offscreen back buffer";
                std::unique_ptr<Texture> t = createTexture(td, nullptr, 0);
                m_offscreenBack.reset(static_cast<GlTexture *>(t.release()));
                return m_offscreenBack != nullptr;
            }

            void refreshWindowSize()
            {
                if (m_offscreen || !m_window.drawableSize)
                    return;
                uint32_t w = 0, h = 0;
                m_window.drawableSize(&w, &h);
                if (w && h && (w != m_width || h != m_height))
                {
                    if (m_reportedSize)
                        std::fprintf(stderr, "[rhi] back buffer %ux%u -> %ux%u (the window's drawable)\n", m_width, m_height, w, h);
                    m_width = w;
                    m_height = h;
                }
                m_reportedSize = true;
            }

            // ---- programs -----------------------------------------------------------------------------------------
            static std::string text(const ShaderCode &code)
            {
                const char *s = static_cast<const char *>(code.data);
                if (!s)
                    return {};
                const void *end = std::memchr(s, 0, code.size);
                return std::string(s, end ? static_cast<const char *>(end) - s : code.size);
            }

            GLuint shader(const ShaderCode &code, GLenum stage)
            {
                auto it = m_shaders.find(code.data);
                if (it != m_shaders.end())
                    return it->second;
                const std::string body = text(code);
                if (body.empty())
                    return 0;
                const std::string head = std::string("#version 330 core\n#define RHI_ZERO_TO_ONE ") + (m_caps.clipControl ? "1" : "0") + "\n";
                const char *parts[2] = {head.c_str(), body.c_str()};
                const GLuint s = glCreateShader(stage);
                glShaderSource(s, 2, parts, nullptr);
                glCompileShader(s);
                GLint ok = 0;
                glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
                if (!ok)
                {
                    char log[2048] = {};
                    glGetShaderInfoLog(s, sizeof(log), nullptr, log);
                    std::fprintf(stderr, "[rhi] GLSL %s shader failed:\n%s\n", stage == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
                    ++m_stats.compileFailures;
                    glDeleteShader(s);
                    return 0;
                }
                m_shaders.emplace(code.data, s);
                return s;
            }

            std::shared_ptr<Program> programFor(const PipelineDesc &desc)
            {
                const auto key = std::make_pair(desc.vertex.data, desc.pixel.data);
                auto it = m_programs.find(key);
                if (it != m_programs.end())
                    return it->second;
                const GLuint vs = shader(desc.vertex, GL_VERTEX_SHADER);
                const GLuint fs = shader(desc.pixel, GL_FRAGMENT_SHADER);
                if (!vs || !fs)
                {
                    std::fprintf(stderr, "[rhi] shaders of pipeline '%s' rejected\n", desc.name ? desc.name : "?");
                    return nullptr;
                }
                auto p = std::make_shared<Program>();
                p->name = glCreateProgram();
                glAttachShader(p->name, vs);
                glAttachShader(p->name, fs);
                glLinkProgram(p->name);
                GLint ok = 0;
                glGetProgramiv(p->name, GL_LINK_STATUS, &ok);
                if (!ok)
                {
                    char log[2048] = {};
                    glGetProgramInfoLog(p->name, sizeof(log), nullptr, log);
                    std::fprintf(stderr, "[rhi] GLSL program '%s' failed to link:\n%s\n", desc.name ? desc.name : "?", log);
                    ++m_stats.compileFailures;
                    glDeleteProgram(p->name);
                    return nullptr;
                }
                label(kLabelProgram, p->name, desc.name);
                p->clipDistances = text(desc.vertex).find("gl_ClipDistance") != std::string::npos;
                p->flipLocation = glGetUniformLocation(p->name, "rhi_flipY");
                p->dualOutput = glGetFragDataIndex(p->name, "o_blend") == 1;
                // <Name>_b<N>: uniform block -> binding N; <name>_t<N>: sampler -> unit N (README.md)
                auto suffix = [](const std::string &name, char kind, GLuint &n) {
                    const size_t at = name.rfind(std::string("_") + kind);
                    if (at == std::string::npos || at + 2 >= name.size())
                        return false;
                    n = static_cast<GLuint>(std::strtoul(name.c_str() + at + 2, nullptr, 10));
                    return true;
                };
                GLint blocks = 0;
                glGetProgramiv(p->name, GL_ACTIVE_UNIFORM_BLOCKS, &blocks);
                for (GLint i = 0; i < blocks; ++i)
                {
                    char name[128] = {};
                    glGetActiveUniformBlockName(p->name, static_cast<GLuint>(i), sizeof(name), nullptr, name);
                    GLuint binding = 0;
                    if (!suffix(name, 'b', binding) || binding >= kBlocks)
                    {
                        std::fprintf(stderr, "[rhi] '%s': uniform block %s has no _b<N> binding\n", desc.name ? desc.name : "?", name);
                        ++m_stats.compileFailures;
                        glDeleteProgram(p->name);
                        return nullptr;
                    }
                    glUniformBlockBinding(p->name, static_cast<GLuint>(i), binding);
                    GLint size = 0;
                    glGetActiveUniformBlockiv(p->name, static_cast<GLuint>(i), GL_UNIFORM_BLOCK_DATA_SIZE, &size);
                    p->blocks.emplace_back(binding, size);
                }
                glUseProgram(p->name);
                GLint uniforms = 0;
                glGetProgramiv(p->name, GL_ACTIVE_UNIFORMS, &uniforms);
                for (GLint i = 0; i < uniforms; ++i)
                {
                    char name[128] = {};
                    GLint size = 0;
                    GLenum type = 0;
                    glGetActiveUniform(p->name, static_cast<GLuint>(i), sizeof(name), nullptr, &size, &type, name);
                    if (type != GL_SAMPLER_2D && type != GL_SAMPLER_2D_MULTISAMPLE)
                        continue;
                    GLuint unit = 0;
                    if (!suffix(name, 't', unit) || unit >= kUnits)
                    {
                        std::fprintf(stderr, "[rhi] '%s': sampler %s has no _t<N> unit\n", desc.name ? desc.name : "?", name);
                        ++m_stats.compileFailures;
                        glUseProgram(m_boundProgram);
                        glDeleteProgram(p->name);
                        return nullptr;
                    }
                    glUniform1i(glGetUniformLocation(p->name, name), static_cast<GLint>(unit));
                    p->samplerUnits |= 1u << unit;
                }
                glUseProgram(m_boundProgram);
                m_programs.emplace(key, p);
                return p;
            }

            // ---- draws --------------------------------------------------------------------------------------------
            void bindVao(GLuint vao)
            {
                if (m_boundVao != vao)
                {
                    glBindVertexArray(vao);
                    m_boundVao = vao;
                }
            }

            void unbindTextures()
            {
                for (uint32_t slot = 0; slot < kUnits; ++slot)
                    if (m_units[slot].name)
                    {
                        glActiveTexture(GL_TEXTURE0 + slot);
                        glBindTexture(m_units[slot].target, 0);
                        glBindSampler(slot, 0);
                        m_units[slot] = {};
                    }
            }

            bool prepareDraw(uint64_t vertexEnd)
            {
                if (!m_pipeline || !m_inPass)
                {
                    reject("draw without a pipeline or outside a pass");
                    return false;
                }
                if (!m_pipelineStateValid)
                    setPipeline(*m_pipeline);
                GlPipeline &p = *m_pipeline;
                Program &prog = *p.program;
                if (prog.flipLocation >= 0 && prog.flip != m_flip)
                {
                    glUniform1f(prog.flipLocation, m_flip);
                    prog.flip = m_flip;
                }
                for (const auto &[binding, size] : prog.blocks)
                    if (!m_blocks[binding].name || m_blocks[binding].size < static_cast<size_t>(size))
                    {
                        reject("draw '%s': constant buffer %u missing or below the shader's %d bytes", p.name.c_str(), binding, size);
                        return false;
                    }
                for (uint32_t unit = 0; unit < kUnits; ++unit)
                {
                    const BoundTexture &t = m_units[unit];
                    if (!t.name)
                        continue;
                    if (t.name == m_passColor || t.name == m_passDepth)
                    {
                        ++m_stats.hazards;
                        reject("draw '%s': texture '%s' is sampled while it is the pass's target", p.name.c_str(),
                               m_unitNames[unit].c_str());
                        return false;
                    }
                    if ((prog.samplerUnits & (1u << unit)) && t.target != GL_TEXTURE_2D)
                    {
                        reject("draw '%s': multisampled texture '%s' sampled (resolve first)", p.name.c_str(),
                               m_unitNames[unit].c_str());
                        return false;
                    }
                }
                if (p.vao)
                {
                    if (!m_vertexBuffer.name)
                    {
                        reject("draw '%s': no vertex buffer", p.name.c_str());
                        return false;
                    }
                    if (vertexEnd && m_vertexOffset + vertexEnd * p.stride > m_vertexBuffer.size)
                    {
                        reject("draw '%s': %llu vertices of %u bytes from offset %u past '%s' (%zu bytes)", p.name.c_str(),
                               static_cast<unsigned long long>(vertexEnd), p.stride, m_vertexOffset, m_vertexBufferName.c_str(),
                               m_vertexBuffer.size);
                        return false;
                    }
                    if (p.vaoBuffer != m_vertexBuffer.name || p.vaoOffset != m_vertexOffset || p.vaoEpoch != m_bufferEpoch)
                    {
                        bindVao(p.vao);
                        glBindBuffer(GL_ARRAY_BUFFER, m_vertexBuffer.name);
                        for (size_t i = 0; i < p.attributes.size(); ++i)
                        {
                            const VertexAttribute &a = p.attributes[i];
                            const VertexFormatInfo f = vertexFormat(a.format);
                            const void *at = reinterpret_cast<const void *>(static_cast<uintptr_t>(m_vertexOffset + a.offset));
                            if (f.integer)
                                glVertexAttribIPointer(static_cast<GLuint>(i), f.components, f.type, static_cast<GLsizei>(p.stride), at);
                            else
                                glVertexAttribPointer(static_cast<GLuint>(i), f.components, f.type, f.normalized ? GL_TRUE : GL_FALSE,
                                                      static_cast<GLsizei>(p.stride), at);
                        }
                        glBindBuffer(GL_ARRAY_BUFFER, 0);
                        p.vaoBuffer = m_vertexBuffer.name;
                        p.vaoOffset = m_vertexOffset;
                    }
                }
                const GLuint index = m_indexBuffer.name;
                const GLuint vao = p.vao ? p.vao : m_emptyVao;
                GLuint &held = p.vao ? p.vaoIndexBuffer : m_emptyVaoIndexBuffer;
                uint64_t &heldEpoch = p.vao ? p.vaoEpoch : m_emptyVaoEpoch;
                bindVao(vao);
                if (held != index || heldEpoch != m_bufferEpoch)
                {
                    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, index);
                    held = index;
                }
                heldEpoch = m_bufferEpoch;
                return true;
            }

            // ---- deferred destruction and reports -----------------------------------------------------------------
            void drain()
            {
                CpuScope cpu(this, CpuDelete);
                std::vector<GLuint> textures, buffers, samplers, vaos;
                std::vector<GLsync> fences;
                {
                    std::lock_guard<std::mutex> lock(m_core->mutex);
                    fences.swap(m_core->fences);
                    textures.swap(m_core->textures);
                    buffers.swap(m_core->buffers);
                    samplers.swap(m_core->samplers);
                    vaos.swap(m_core->vaos);
                }
                if (!vaos.empty())
                {
                    if (std::find(vaos.begin(), vaos.end(), m_boundVao) != vaos.end())
                        bindVao(m_emptyVao);
                    m_pipeline = nullptr; // a released pipeline's address may be reused
                    glDeleteVertexArrays(static_cast<GLsizei>(vaos.size()), vaos.data());
                }
                if (!textures.empty())
                {
                    // framebuffers made with a released texture go first: GL may reuse the name afterwards
                    for (auto it = m_fbos.begin(); it != m_fbos.end();)
                    {
                        const bool uses = std::find(textures.begin(), textures.end(), it->first.first) != textures.end() ||
                                          std::find(textures.begin(), textures.end(), it->first.second) != textures.end();
                        if (uses)
                        {
                            if (m_boundFramebuffer == it->second)
                            {
                                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                                m_boundFramebuffer = 0;
                            }
                            glDeleteFramebuffers(1, &it->second);
                            it = m_fbos.erase(it);
                        }
                        else
                            ++it;
                    }
                    for (uint32_t slot = 0; slot < kUnits; ++slot)
                        if (m_units[slot].name && std::find(textures.begin(), textures.end(), m_units[slot].name) != textures.end())
                            m_units[slot] = {};
                    for (GLuint *target : {&m_passColor, &m_passDepth})
                        if (std::find(textures.begin(), textures.end(), *target) != textures.end())
                            *target = 0;
                    glDeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
                }
                if (!buffers.empty())
                {
                    auto gone = [&](BoundBuffer &b) {
                        if (b.name && std::find(buffers.begin(), buffers.end(), b.name) != buffers.end())
                            b = {};
                    };
                    for (BoundBuffer &b : m_blocks)
                        gone(b);
                    gone(m_vertexBuffer);
                    gone(m_indexBuffer);
                    ++m_bufferEpoch; // names may come back: every VAO re-specifies its buffers
                    glDeleteBuffers(static_cast<GLsizei>(buffers.size()), buffers.data());
                }
                if (!samplers.empty())
                    glDeleteSamplers(static_cast<GLsizei>(samplers.size()), samplers.data());
                for (GLsync f : fences)
                    glDeleteSync(f);
            }

            // ---- GPU time per frame (PS2X_GL_GPU_TIME, M06-11) -----------------------------------------------------
            // One GL_TIME_ELAPSED query spans a present to the next, so it holds every command of the frame; results are
            // read without waiting when the GPU has them (a few frames later). A frame finding its slot still unread is
            // not timed (gpuDropped).
            void beginGpuFrame()
            {
                collectGpuFrames();
                if (m_gpuPending[m_gpuNext])
                {
                    ++m_stats.gpuDropped;
                    return;
                }
                glBeginQuery(GL_TIME_ELAPSED, m_gpuQueries[m_gpuNext]);
                m_gpuActive = true;
            }

            void endGpuFrame()
            {
                if (!m_gpuActive)
                    return;
                glEndQuery(GL_TIME_ELAPSED);
                m_gpuActive = false;
                m_gpuPending[m_gpuNext] = true;
                m_gpuFrame[m_gpuNext] = m_presents;
                m_gpuNext = (m_gpuNext + 1u) % kGpuQueries;
            }

            void collectGpuFrames()
            {
                for (uint32_t i = 0; i < kGpuQueries; ++i)
                {
                    if (!m_gpuPending[i])
                        continue;
                    GLuint available = 0;
                    glGetQueryObjectuiv(m_gpuQueries[i], GL_QUERY_RESULT_AVAILABLE, &available);
                    if (!available)
                        continue;
                    GLuint64 ns = 0;
                    glGetQueryObjectui64v(m_gpuQueries[i], GL_QUERY_RESULT, &ns);
                    m_gpuPending[i] = false;
                    ++m_stats.gpuFrames;
                    m_stats.gpuNs += ns;
                    m_stats.gpuMaxNs = std::max<uint64_t>(m_stats.gpuMaxNs, ns);
                    if (m_gpuLog)
                        std::fprintf(m_gpuLog, "%llu,%.3f\n", static_cast<unsigned long long>(m_gpuFrame[i]), ns / 1e6);
                }
            }

            // ---- the constant ring (M10-03) -----------------------------------------------------------------------------
            // One GL buffer of kUboRingBytes in kUboSegments segments. An update takes the next aligned span (the
            // buffer's whole allocation, so a binding covers the shader's block), written through an unsynchronized map:
            // draws already submitted keep reading their own spans (D3D11 WRITE_DISCARD). Entering a segment fences the
            // one left and waits for the segment's fence from the last lap (long done: a lap is many frames).
            void ringWrite(GlBuffer &b, const void *data, size_t size)
            {
                const size_t need = (b.allocated + m_ubo.align - 1u) & ~(m_ubo.align - 1u);
                const size_t segment = kUboRingBytes / kUboSegments;
                size_t at = m_ubo.head;
                if (at / segment != (at + need - 1u) / segment || at + need > kUboRingBytes)
                    at = ((at / segment) + 1u) * segment % kUboRingBytes; // to the next segment's start
                const uint32_t seg = static_cast<uint32_t>(at / segment);
                if (seg != m_ubo.segment)
                {
                    GLsync &left = m_ubo.fences[m_ubo.segment];
                    if (left)
                        glDeleteSync(left);
                    left = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                    GLsync &entering = m_ubo.fences[seg];
                    if (entering)
                    {
                        if (glClientWaitSync(entering, GL_SYNC_FLUSH_COMMANDS_BIT, 0) == GL_TIMEOUT_EXPIRED)
                        {
                            ++m_ubo.waits;
                            glClientWaitSync(entering, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
                        }
                        glDeleteSync(entering);
                        entering = nullptr;
                    }
                    m_ubo.segment = seg;
                }
                if (m_ubo.mapped)
                    std::memcpy(m_ubo.mapped + at, data, size); // coherent: visible to commands issued after this
                else
                {
                    glBindBuffer(GL_COPY_WRITE_BUFFER, m_ubo.name);
                    void *p = glMapBufferRange(GL_COPY_WRITE_BUFFER, static_cast<GLintptr>(at), static_cast<GLsizeiptr>(need),
                                               GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
                    if (p)
                    {
                        std::memcpy(p, data, size);
                        glUnmapBuffer(GL_COPY_WRITE_BUFFER);
                    }
                    else
                        glBufferSubData(GL_COPY_WRITE_BUFFER, static_cast<GLintptr>(at), static_cast<GLsizeiptr>(size), data);
                    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
                }
                m_ubo.head = at + need;
                b.ringOffset = at;
                b.ringValid = true;
                // D3D11: an update reaches the slots the buffer is bound to
                for (uint32_t slot = 0; slot < kBlocks; ++slot)
                    if (m_blocks[slot].name == b.name)
                        glBindBufferRange(GL_UNIFORM_BUFFER, slot, m_ubo.name, static_cast<GLintptr>(at),
                                          static_cast<GLsizeiptr>(b.allocated));
            }

            // ---- GPU time per pass (PS2X_GL_PASS_TIME, M10-02) ----------------------------------------------------------
            // A GL_TIMESTAMP query at the start and the end of each pass (they may overlap the frame's GL_TIME_ELAPSED
            // query); results read in order, without waiting, when the GPU has them. A pass finding no free record is
            // not timed (not timed count). Totals by "name WxH" ("name window" for the back buffer) go to [rhi-stats].
            void beginPassTime(const char *name, uint32_t w, uint32_t h, bool window)
            {
                m_passOpen = -1;
                if (m_passCount == kPassRecords)
                {
                    ++m_passDropped;
                    return;
                }
                const uint32_t slot = (m_passFirst + m_passCount) % kPassRecords;
                char key[96];
                if (window)
                    std::snprintf(key, sizeof(key), "%s window", name ? name : "(unnamed)");
                else
                    std::snprintf(key, sizeof(key), "%s %ux%u", name ? name : "(unnamed)", w, h);
                m_passRecords[slot].key = key;
                glQueryCounter(m_passQueries[2u * slot], GL_TIMESTAMP);
                m_passOpen = static_cast<int>(slot);
            }

            void endPassTime()
            {
                if (m_passOpen < 0)
                    return;
                glQueryCounter(m_passQueries[2u * static_cast<uint32_t>(m_passOpen) + 1u], GL_TIMESTAMP);
                ++m_passCount;
                m_passOpen = -1;
            }

            void collectPassTimes()
            {
                while (m_passCount)
                {
                    const uint32_t slot = m_passFirst;
                    GLuint available = 0;
                    glGetQueryObjectuiv(m_passQueries[2u * slot + 1u], GL_QUERY_RESULT_AVAILABLE, &available);
                    if (!available)
                        break;
                    GLuint64 t0 = 0, t1 = 0;
                    glGetQueryObjectui64v(m_passQueries[2u * slot], GL_QUERY_RESULT, &t0);
                    glGetQueryObjectui64v(m_passQueries[2u * slot + 1u], GL_QUERY_RESULT, &t1);
                    PassTotal &t = m_passTotals[m_passRecords[slot].key];
                    t.ns += t1 > t0 ? t1 - t0 : 0u;
                    ++t.count;
                    m_passFirst = (m_passFirst + 1u) % kPassRecords;
                    --m_passCount;
                }
            }

            void reportPassTimes(uint64_t presents)
            {
                std::vector<std::pair<uint64_t, std::string>> top;
                uint64_t all = 0;
                const double n = presents ? static_cast<double>(presents) : 1.0;
                for (const auto &e : m_passTotals)
                {
                    all += e.second.ns;
                    char item[160];
                    std::snprintf(item, sizeof(item), "%s %.2f ms x%.1f", e.first.c_str(), e.second.ns / 1e6 / n,
                                  e.second.count / n);
                    top.emplace_back(e.second.ns, item);
                }
                std::sort(top.begin(), top.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
                std::string line;
                for (size_t i = 0; i < top.size() && i < 10; ++i)
                    line += (i ? " | " : "") + top[i].second;
                std::fprintf(stderr, "[rhi-stats] passes per present (%llu presents, all passes %.2f ms, %llu not timed): %s\n",
                             static_cast<unsigned long long>(presents), all / 1e6 / n,
                             static_cast<unsigned long long>(m_passDropped), line.c_str());
                m_passTotals.clear();
            }

            void reportStats()
            {

                static const bool on = envOn("PS2X_RHI_STATS");
                if (!on)
                    return;
                const auto now = std::chrono::steady_clock::now();
                if (now - m_lastReport < std::chrono::seconds(5))
                    return;
                m_lastReport = now;
                const GlStats s = stats();
                if (m_gpuTime)
                {
                    const uint64_t frames = s.gpuFrames - m_gpuReported[0], ns = s.gpuNs - m_gpuReported[1];
                    std::fprintf(stderr, "[rhi-stats] gpu %llu frames timed, mean %.2f ms (run max %.2f ms, %llu not timed)\n",
                                 static_cast<unsigned long long>(frames), frames ? ns / 1e6 / static_cast<double>(frames) : 0.0,
                                 s.gpuMaxNs / 1e6, static_cast<unsigned long long>(s.gpuDropped));
                    m_gpuReported[0] = s.gpuFrames;
                    m_gpuReported[1] = s.gpuNs;
                }
                if (cpuTimeOn())
                {
                    static const char *const names[CpuKinds] = {"create", "upload", "buffer", "draw", "pass", "copy", "swap",
                                                                "delete", "constants"};
                    const double n = s.presents > m_cpuPresents ? static_cast<double>(s.presents - m_cpuPresents) : 1.0;
                    std::string line;
                    for (uint32_t k = 0; k < CpuKinds; ++k)
                    {
                        char item[64];
                        std::snprintf(item, sizeof(item), "%s%s %.2f ms x%.1f", k ? " | " : "", names[k], m_cpuNs[k] / 1e6 / n,
                                      m_cpuCalls[k] / n);
                        line += item;
                        m_cpuNs[k] = m_cpuCalls[k] = 0;
                    }
                    std::fprintf(stderr, "[rhi-stats] cpu per present (%.0f presents, owner thread): %s | buffer bytes %.0f KiB\n",
                                 n, line.c_str(), m_cpuBufferBytes / 1024.0 / n);
                    m_cpuBufferBytes = 0;
                    m_cpuPresents = s.presents;
                }
                if (m_passTime)
                {
                    reportPassTimes(s.presents - m_passPresentsReported);
                    m_passPresentsReported = s.presents;
                }
                std::fprintf(stderr,
                             "[rhi-stats] textures %llu (%.1f MiB) buffers %llu (%.1f MiB) samplers %llu pipelines %llu programs %llu "
                             "fbos %llu pending %llu | draws %llu passes %llu presents %llu | gl errors %llu debug %llu incomplete %llu "
                             "owner %llu rejected %llu hazards %llu\n",
                             static_cast<unsigned long long>(s.textures), s.textureBytes / 1048576.0,
                             static_cast<unsigned long long>(s.buffers), s.bufferBytes / 1048576.0,
                             static_cast<unsigned long long>(s.samplers), static_cast<unsigned long long>(s.pipelines),
                             static_cast<unsigned long long>(s.programs), static_cast<unsigned long long>(s.framebuffers),
                             static_cast<unsigned long long>(s.pendingDeletes), static_cast<unsigned long long>(s.drawCalls),
                             static_cast<unsigned long long>(s.passes), static_cast<unsigned long long>(s.presents),
                             static_cast<unsigned long long>(s.glErrors), static_cast<unsigned long long>(s.debugMessages),
                             static_cast<unsigned long long>(s.incompleteFramebuffers), static_cast<unsigned long long>(s.ownerViolations),
                             static_cast<unsigned long long>(s.rejected), static_cast<unsigned long long>(s.hazards));
                std::fprintf(stderr, "[rhi-stats] targets %llu (%.1f MiB) largest %ux%u\n", static_cast<unsigned long long>(s.targets),
                             s.targetBytes / 1048576.0, s.largestTargetWidth, s.largestTargetHeight);
            }

            friend bool rhi::openGLStats(Device &, GlStats &);

            std::shared_ptr<Core> m_core;
            GlWindow m_window;
            GlCaps m_caps;
            GlStats m_stats;
            std::atomic<uint64_t> m_ownerViolations{0};
            std::string m_adapterName;
            bool m_offscreen = false;
            std::unique_ptr<GlTexture> m_offscreenBack;
            uint32_t m_width = 1, m_height = 1;
            bool m_reportedSize = false;
            int m_swapInterval = -1;

            ClipControlFn m_clipControl = nullptr;
            BufferStorageFn m_bufferStorage = nullptr;
            ViewportIndexedfFn m_viewportIndexedf = nullptr;
            DebugMessageCallbackFn m_debugMessageCallback = nullptr;
            PushDebugGroupFn m_pushDebugGroup = nullptr;
            PopDebugGroupFn m_popDebugGroup = nullptr;
            ObjectLabelFn m_objectLabel = nullptr;

            std::map<std::pair<GLuint, GLuint>, GLuint> m_fbos; // (colour, depth) texture names -> framebuffer
            GLuint m_copyFbo[2] = {0, 0};                        // read / draw framebuffers of copies and readbacks
            GLuint m_boundFramebuffer = ~0u;
            GLuint m_emptyVao = 0, m_emptyVaoIndexBuffer = 0;
            GLuint m_boundVao = 0;
            GLuint m_boundProgram = 0;
            GLuint m_scratchUnit = 15;
            std::map<std::pair<const void *, const void *>, std::shared_ptr<Program>> m_programs;
            std::unordered_map<const void *, GLuint> m_shaders; // by source text
            uint64_t m_pipelineSerial = 0, m_pipelineSerialBound = 0;
            uint64_t m_bufferEpoch = 1, m_emptyVaoEpoch = 0;

            static constexpr uint32_t kGpuQueries = 8;
            bool m_gpuTime = false, m_gpuActive = false;
            GLuint m_gpuQueries[kGpuQueries] = {};
            bool m_gpuPending[kGpuQueries] = {};
            uint64_t m_gpuFrame[kGpuQueries] = {};
            uint32_t m_gpuNext = 0;
            uint64_t m_gpuReported[2] = {0, 0}; // frames, ns at the last [rhi-stats] line
            FILE *m_gpuLog = nullptr;             // PS2X_GL_GPU_LOG: frame,gpu_ms per timed frame

            // the constant ring (M10-03)
            static constexpr size_t kUboRingBytes = 8u << 20;
            static constexpr uint32_t kUboSegments = 8;
            struct UboRing
            {
                bool on = false;
                GLuint name = 0;
                size_t align = 256, head = 0;
                uint32_t segment = 0;
                uint8_t *mapped = nullptr; // the persistent map (buffer storage)
                GLsync fences[kUboSegments] = {};
                uint64_t waits = 0;
            } m_ubo;

            // PS2X_GL_CPU_TIME (M10-02): the owner thread's CPU time inside the backend by kind of call
            enum CpuKind : uint32_t { CpuCreate, CpuUpload, CpuBuffer, CpuDraw, CpuPass, CpuCopy, CpuSwap, CpuDelete, CpuConst,
                                     CpuKinds };
            uint64_t m_cpuBufferBytes = 0; // bytes through updateBuffer outside the constant ring
            uint64_t m_cpuNs[CpuKinds] = {}, m_cpuCalls[CpuKinds] = {};
            uint64_t m_cpuPresents = 0;
            static bool cpuTimeOn()
            {
                static const bool on = envOn("PS2X_GL_CPU_TIME");
                return on;
            }
            struct CpuScope
            {
                uint64_t *ns = nullptr, *calls = nullptr;
                std::chrono::steady_clock::time_point t0;
                CpuScope(GlDevice *d, CpuKind k)
                {
                    if (cpuTimeOn())
                    {
                        ns = &d->m_cpuNs[k];
                        calls = &d->m_cpuCalls[k];
                        t0 = std::chrono::steady_clock::now();
                    }
                }
                ~CpuScope()
                {
                    if (ns)
                    {
                        *ns += static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
                        ++*calls;
                    }
                }
            };

            // PS2X_GL_PASS_TIME (M10-02): a ring of begin / end timestamp pairs and totals by pass key
            static constexpr uint32_t kPassRecords = 2048;
            struct PassRecord
            {
                std::string key;
            };
            struct PassTotal
            {
                uint64_t ns = 0, count = 0;
            };
            bool m_passTime = false;
            std::vector<GLuint> m_passQueries;
            std::vector<PassRecord> m_passRecords;
            std::unordered_map<std::string, PassTotal> m_passTotals;
            uint32_t m_passFirst = 0, m_passCount = 0;
            int m_passOpen = -1;
            uint64_t m_passDropped = 0, m_passPresentsReported = 0;

            bool m_inPass = false, m_passMarker = false, m_toWindow = false;
            GLuint m_passColor = 0, m_passDepth = 0;
            uint32_t m_passWidth = 1, m_passHeight = 1;
            float m_flip = -1.0f;
            GLenum m_clipOrigin = 0; // the glClipControl origin set last (clip control only)
            GlPipeline *m_pipeline = nullptr;
            bool m_pipelineStateValid = false;
            BoundBuffer m_vertexBuffer;
            std::string m_vertexBufferName;
            uint32_t m_vertexOffset = 0;
            BoundBuffer m_indexBuffer;
            bool m_index32 = false;
            BoundTexture m_units[kUnits];
            std::string m_unitNames[kUnits];
            BoundBuffer m_blocks[kBlocks];

            bool m_warnedAnisotropy = false, m_warnedViewport = false;
            bool m_freeThreaded = false;
            std::mutex m_loaderMutex;
            std::atomic<uint64_t> m_offOwnerCreated{0}, m_offOwnerRefused{0};
            uint64_t m_fencesWaited = 0;
            uint64_t m_draws = 0, m_passes = 0, m_presents = 0;
            std::chrono::steady_clock::time_point m_lastReport = std::chrono::steady_clock::now();
        };
    }

    std::unique_ptr<Device> createOpenGLDevice(const DeviceDesc &desc, std::string &error)
    {
        auto device = std::make_unique<GlDevice>();
        if (!device->create(desc, error))
            return nullptr;
        return device;
    }

    bool openGLCaps(Device &device, GlCaps &out)
    {
        if (device.backend() != Backend::OpenGL)
            return false;
        out = static_cast<GlDevice &>(device).caps();
        return true;
    }

    bool openGLStats(Device &device, GlStats &out)
    {
        if (device.backend() != Backend::OpenGL)
            return false;
        auto &d = static_cast<GlDevice &>(device);
        out = d.stats();
        out.ownerViolations = d.m_ownerViolations.load();
        return true;
    }

    uint32_t openGLCheckErrors(Device &device, const char *where)
    {
        if (device.backend() != Backend::OpenGL)
            return 0;
        return static_cast<GlDevice &>(device).checkErrors(where);
    }
}

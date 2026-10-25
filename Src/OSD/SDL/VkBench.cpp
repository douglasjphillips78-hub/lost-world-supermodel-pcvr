#ifdef __ANDROID__

#include "VkBench.h"

#include <vulkan/vulkan.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>

#include <chrono>
#include <cstdarg>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "VkBench/vert.h"
#include "VkBench/frag.h"

// Feasibility test: does Vulkan submit a Model 3 sized scene faster than OpenGL ES on this headset?
//
// It draws the same synthetic frame both ways, offscreen, with no game and no headset session:
// two eyes (1024x1073), thousands of small meshes per eye, each with its own matrix and colour
// (like New3D, which sets uniforms and issues a draw per mesh), a state change every few draws,
// and about 8 MB of fresh vertex data every frame.
//
// OpenGL ES : glUniform* + glDrawArrays per mesh, glBufferSubData into a rotating buffer.
// Vulkan    : push constants + vkCmdDraw per mesh, a persistently mapped buffer, one thread.
// Vulkan 2T : the same, with each eye recorded on its own thread.
//
// "cpu" is the time the emulator thread spends issuing a frame (what the game loop pays for);
// "wall" is the time per frame when frames run back to back (what the player sees).

namespace
{
const char* kTag = "SupermodelVR";
const int kEyeW = 1024, kEyeH = 1073;
const int kVertsPerMesh = 30;
const int kMaxMeshes = 5700;
const int kSlices = 3;
const int kWarmup = 30, kFrames = 240;

struct Vertex
{
  float pos[3];
  float nrm[3];
  float uv[2];
  uint8_t col[4];
  uint8_t pad[12];  // 48 bytes, like New3D's FVertex
};
static_assert(sizeof(Vertex) == 48, "vertex size");

struct PerMesh
{
  float tx, ty, tz, s;
  float tint[4];
};

struct Result
{
  std::string name;
  int meshes = 0;
  double cpuMs = 0, wallMs = 0;
};

std::string g_report;

void Log(const char* fmt, ...)
{
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  __android_log_print(ANDROID_LOG_INFO, kTag, "[VKBENCH] %s", buf);
  g_report += buf;
  g_report += "\n";
}

double Now()
{
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint32_t g_rng = 12345;
float Rand01()
{
  g_rng = g_rng * 1664525u + 1013904223u;
  return (float)((g_rng >> 8) & 0xFFFF) / 65535.0f;
}

// The scene: identical for both APIs.
struct Scene
{
  std::vector<Vertex> verts;    // kMaxMeshes * kVertsPerMesh
  std::vector<PerMesh> meshes;  // per mesh
  std::vector<uint8_t> texture; // 2048x1024 RGBA

  Scene()
  {
    verts.resize((size_t)kMaxMeshes * kVertsPerMesh);
    meshes.resize(kMaxMeshes);
    for (auto& v : verts) {
      v.pos[0] = Rand01() * 2 - 1;
      v.pos[1] = Rand01() * 2 - 1;
      v.pos[2] = Rand01() * 2 - 1;
      v.nrm[0] = Rand01() * 2 - 1;
      v.nrm[1] = Rand01() * 2 - 1;
      v.nrm[2] = Rand01() * 2 - 1;
      v.uv[0] = Rand01();
      v.uv[1] = Rand01();
      for (int k = 0; k < 4; k++)
        v.col[k] = (uint8_t)(128 + Rand01() * 127);
      memset(v.pad, 0, sizeof(v.pad));
    }
    for (auto& m : meshes) {
      m.tx = Rand01() * 1.8f - 0.9f;
      m.ty = Rand01() * 1.8f - 0.9f;
      m.tz = 0.05f + Rand01() * 0.9f;
      m.s = 0.02f + Rand01() * 0.04f;
      m.tint[0] = Rand01();
      m.tint[1] = Rand01();
      m.tint[2] = Rand01();
      m.tint[3] = 1.0f;
    }
    texture.resize(2048 * 1024 * 4);
    for (auto& b : texture)
      b = (uint8_t)(Rand01() * 255);
  }
};

// Column-major matrix: scale s on x/y, 0.04 on z, then translate. zMul/zAdd map depth for each API.
void MakeMvp(float* m, const PerMesh& p, float eyeShift, bool glDepth)
{
  memset(m, 0, 16 * sizeof(float));
  m[0] = p.s;
  m[5] = p.s;
  m[10] = glDepth ? 0.08f : 0.04f;
  m[12] = p.tx + eyeShift;
  m[13] = p.ty;
  m[14] = glDepth ? (p.tz * 2.0f - 1.0f) : p.tz;
  m[15] = 1.0f;
}

bool BlendDraw(int i) { return (i & 3) == 0; }  // a state change every few draws

// ===== OpenGL ES ======================================================================================

const char* kGlVert = R"(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNrm;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aCol;
uniform mat4 uMvp;
uniform vec4 uTint;
uniform vec4 uParams;
out vec2 vUV;
out vec4 vCol;
void main()
{
    gl_Position = uMvp * vec4(aPos, 1.0);
    float shade = max(dot(normalize(aNrm), normalize(uParams.xyz)), 0.0) * 0.7 + 0.3;
    vUV = aUV;
    vCol = vec4(aCol.rgb * shade, aCol.a);
}
)";
const char* kGlFrag = R"(#version 300 es
precision highp float;
uniform sampler2D uTex;
uniform vec4 uTint;
in vec2 vUV;
in vec4 vCol;
out vec4 outCol;
void main()
{
    vec4 t = texture(uTex, vUV);
    vec3 c = mix(t.rgb * vCol.rgb, uTint.rgb, 0.2);
    outCol = vec4(c, uTint.a);
}
)";

GLuint Compile(GLenum type, const char* src)
{
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512];
    glGetShaderInfoLog(s, sizeof(log), nullptr, log);
    throw std::runtime_error(std::string("GL shader: ") + log);
  }
  return s;
}

void RunGL(const Scene& scene, std::vector<Result>& out)
{
  EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr))
    throw std::runtime_error("EGL init failed");
  const EGLint cfgAttr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
  EGLConfig cfg;
  EGLint ncfg = 0;
  if (!eglChooseConfig(dpy, cfgAttr, &cfg, 1, &ncfg) || ncfg < 1)
    throw std::runtime_error("no EGL config");
  const EGLint pbAttr[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
  EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pbAttr);
  const EGLint ctxAttr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttr);
  if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, surf, surf, ctx))
    throw std::runtime_error("EGL context failed");
  Log("OpenGL ES: %s | %s", (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION));

  GLuint prog = glCreateProgram();
  glAttachShader(prog, Compile(GL_VERTEX_SHADER, kGlVert));
  glAttachShader(prog, Compile(GL_FRAGMENT_SHADER, kGlFrag));
  glLinkProgram(prog);
  GLint linked = 0;
  glGetProgramiv(prog, GL_LINK_STATUS, &linked);
  if (!linked)
    throw std::runtime_error("GL link failed");
  glUseProgram(prog);
  const GLint uMvp = glGetUniformLocation(prog, "uMvp");
  const GLint uTint = glGetUniformLocation(prog, "uTint");
  const GLint uParams = glGetUniformLocation(prog, "uParams");
  glUniform1i(glGetUniformLocation(prog, "uTex"), 0);

  GLuint tex;
  glGenTextures(1, &tex);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2048, 1024, 0, GL_RGBA, GL_UNSIGNED_BYTE, scene.texture.data());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  GLuint fbo[2], col[2], dep[2];
  for (int e = 0; e < 2; e++) {
    glGenRenderbuffers(1, &col[e]);
    glBindRenderbuffer(GL_RENDERBUFFER, col[e]);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, kEyeW, kEyeH);
    glGenRenderbuffers(1, &dep[e]);
    glBindRenderbuffer(GL_RENDERBUFFER, dep[e]);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, kEyeW, kEyeH);
    glGenFramebuffers(1, &fbo[e]);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo[e]);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, col[e]);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, dep[e]);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
      throw std::runtime_error("GL framebuffer incomplete");
  }

  const size_t sliceBytes = scene.verts.size() * sizeof(Vertex);
  GLuint vbo, vao;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glGenBuffers(1, &vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, sliceBytes * kSlices, nullptr, GL_DYNAMIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, pos));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, nrm));
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, uv));
  glEnableVertexAttribArray(3);
  glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)offsetof(Vertex, col));

  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glViewport(0, 0, kEyeW, kEyeH);
  const float params[4] = {0.3f, 0.8f, 0.5f, 0.0f};
  glUniform4fv(uParams, 1, params);

  for (int meshes : {2850, 5700}) {
    double cpu = 0, t0 = 0;
    for (int f = 0; f < kWarmup + kFrames; f++) {
      if (f == kWarmup) {
        glFinish();
        t0 = Now();
        cpu = 0;
      }
      const double a = Now();
      const int slice = f % kSlices;
      glBindBuffer(GL_ARRAY_BUFFER, vbo);
      glBufferSubData(GL_ARRAY_BUFFER, sliceBytes * slice, sliceBytes, scene.verts.data());
      for (int e = 0; e < 2; e++) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[e]);
        glClearColor(0.1f, 0.1f, 0.2f, 1.0f);
        glClearDepthf(1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        bool blend = false;
        const float shift = e ? 0.03f : -0.03f;
        for (int i = 0; i < meshes; i++) {
          const bool wantBlend = BlendDraw(i);
          if (wantBlend != blend) {
            blend ? glDisable(GL_BLEND) : glEnable(GL_BLEND);
            blend = wantBlend;
          }
          float m[16];
          MakeMvp(m, scene.meshes[i], shift, true);
          glUniformMatrix4fv(uMvp, 1, GL_FALSE, m);
          glUniform4fv(uTint, 1, scene.meshes[i].tint);
          glDrawArrays(GL_TRIANGLES, (GLint)(slice * scene.verts.size() + (size_t)i * kVertsPerMesh), kVertsPerMesh);
        }
        if (blend)
          glDisable(GL_BLEND);
      }
      glFlush();
      cpu += Now() - a;
    }
    glFinish();
    const double wall = Now() - t0;
    Result r;
    r.name = "OpenGL ES";
    r.meshes = meshes;
    r.cpuMs = cpu / kFrames;
    r.wallMs = wall / kFrames;
    out.push_back(r);
    Log("%-10s %5d meshes/eye: cpu %.2f ms, wall %.2f ms per frame (%.0f fps)", r.name.c_str(), meshes, r.cpuMs, r.wallMs, 1000.0 / r.wallMs);
  }

  eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroyContext(dpy, ctx);
  eglDestroySurface(dpy, surf);
  eglTerminate(dpy);
}

// ===== Vulkan =========================================================================================

#define VKC(x)                                                                           \
  do {                                                                                   \
    VkResult _r = (x);                                                                   \
    if (_r != VK_SUCCESS)                                                                \
      throw std::runtime_error(std::string(#x) + " failed: " + std::to_string((int)_r)); \
  } while (0)

struct PushConstants
{
  float mvp[16];
  float tint[4];
  float params[4];
};
static_assert(sizeof(PushConstants) == 96, "push constants");

struct Vk
{
  VkInstance inst = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  VkDevice dev = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t family = 0;
  VkPhysicalDeviceMemoryProperties mem{};
};

uint32_t FindMemory(const Vk& v, uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer)
{
  for (int pass = 0; pass < 2; pass++) {
    const VkMemoryPropertyFlags need = pass == 0 ? (want | prefer) : want;
    for (uint32_t i = 0; i < v.mem.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (v.mem.memoryTypes[i].propertyFlags & need) == need)
        return i;
  }
  throw std::runtime_error("no suitable memory type");
}

struct Buf
{
  VkBuffer buf = VK_NULL_HANDLE;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  void* map = nullptr;
};

Buf MakeBuffer(const Vk& v, VkDeviceSize size, VkBufferUsageFlags usage)
{
  Buf b;
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = size;
  bi.usage = usage;
  bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VKC(vkCreateBuffer(v.dev, &bi, nullptr, &b.buf));
  VkMemoryRequirements rq;
  vkGetBufferMemoryRequirements(v.dev, b.buf, &rq);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = rq.size;
  ai.memoryTypeIndex = FindMemory(v, rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VKC(vkAllocateMemory(v.dev, &ai, nullptr, &b.mem));
  VKC(vkBindBufferMemory(v.dev, b.buf, b.mem, 0));
  VKC(vkMapMemory(v.dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.map));
  return b;
}

struct Target
{
  VkImage colorImg = VK_NULL_HANDLE, depthImg = VK_NULL_HANDLE;
  VkDeviceMemory colorMem = VK_NULL_HANDLE, depthMem = VK_NULL_HANDLE;
  VkImageView colorView = VK_NULL_HANDLE, depthView = VK_NULL_HANDLE;
  VkFramebuffer fb = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd[kSlices] = {};
};

void MakeImage(const Vk& v, VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkImage& img, VkDeviceMemory& mem, VkImageView& view)
{
  VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = fmt;
  ii.extent = {(uint32_t)kEyeW, (uint32_t)kEyeH, 1};
  ii.mipLevels = 1;
  ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = usage;
  ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VKC(vkCreateImage(v.dev, &ii, nullptr, &img));
  VkMemoryRequirements rq;
  vkGetImageMemoryRequirements(v.dev, img, &rq);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = rq.size;
  ai.memoryTypeIndex = FindMemory(v, rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
  VKC(vkAllocateMemory(v.dev, &ai, nullptr, &mem));
  VKC(vkBindImageMemory(v.dev, img, mem, 0));
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = img;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = fmt;
  vi.subresourceRange = {aspect, 0, 1, 0, 1};
  VKC(vkCreateImageView(v.dev, &vi, nullptr, &view));
}

VkShaderModule MakeShader(const Vk& v, const uint32_t* code, size_t bytes)
{
  VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  si.codeSize = bytes;
  si.pCode = code;
  VkShaderModule m;
  VKC(vkCreateShaderModule(v.dev, &si, nullptr, &m));
  return m;
}

void RunVulkan(const Scene& scene, std::vector<Result>& out)
{
  Vk v;
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_1;
  app.pApplicationName = "vkbench";
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  VKC(vkCreateInstance(&ici, nullptr, &v.inst));
  uint32_t n = 0;
  VKC(vkEnumeratePhysicalDevices(v.inst, &n, nullptr));
  if (!n)
    throw std::runtime_error("no Vulkan device");
  std::vector<VkPhysicalDevice> pds(n);
  VKC(vkEnumeratePhysicalDevices(v.inst, &n, pds.data()));
  v.phys = pds[0];
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(v.phys, &props);
  Log("Vulkan: %s | API %u.%u.%u | driver 0x%x", props.deviceName, VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
      VK_VERSION_PATCH(props.apiVersion), props.driverVersion);
  vkGetPhysicalDeviceMemoryProperties(v.phys, &v.mem);

  uint32_t nq = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(v.phys, &nq, nullptr);
  std::vector<VkQueueFamilyProperties> qf(nq);
  vkGetPhysicalDeviceQueueFamilyProperties(v.phys, &nq, qf.data());
  v.family = 0xFFFFFFFF;
  for (uint32_t i = 0; i < nq; i++)
    if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      v.family = i;
      break;
    }
  if (v.family == 0xFFFFFFFF)
    throw std::runtime_error("no graphics queue");
  const float prio = 1.0f;
  VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qi.queueFamilyIndex = v.family;
  qi.queueCount = 1;
  qi.pQueuePriorities = &prio;
  VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  di.queueCreateInfoCount = 1;
  di.pQueueCreateInfos = &qi;
  VKC(vkCreateDevice(v.phys, &di, nullptr, &v.dev));
  vkGetDeviceQueue(v.dev, v.family, 0, &v.queue);

  // Depth format: D32 is always available for attachments.
  const VkFormat depthFmt = VK_FORMAT_D32_SFLOAT;

  // Render pass
  VkAttachmentDescription att[2] = {};
  att[0].format = VK_FORMAT_R8G8B8A8_UNORM;
  att[0].samples = VK_SAMPLE_COUNT_1_BIT;
  att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  att[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  att[1] = att[0];
  att[1].format = depthFmt;
  att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &cref;
  sub.pDepthStencilAttachment = &dref;
  VkSubpassDependency dep{};
  dep.srcSubpass = VK_SUBPASS_EXTERNAL;
  dep.dstSubpass = 0;
  dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  dep.dstStageMask = dep.srcStageMask;
  dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  dep.dstAccessMask = dep.srcAccessMask;
  VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rpi.attachmentCount = 2;
  rpi.pAttachments = att;
  rpi.subpassCount = 1;
  rpi.pSubpasses = &sub;
  rpi.dependencyCount = 1;
  rpi.pDependencies = &dep;
  VkRenderPass rp;
  VKC(vkCreateRenderPass(v.dev, &rpi, nullptr, &rp));

  // Per-eye targets and command pools
  Target tg[2];
  for (auto& t : tg) {
    MakeImage(v, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT, t.colorImg, t.colorMem, t.colorView);
    MakeImage(v, depthFmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, t.depthImg, t.depthMem, t.depthView);
    VkImageView views[2] = {t.colorView, t.depthView};
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = rp;
    fi.attachmentCount = 2;
    fi.pAttachments = views;
    fi.width = kEyeW;
    fi.height = kEyeH;
    fi.layers = 1;
    VKC(vkCreateFramebuffer(v.dev, &fi, nullptr, &t.fb));
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = v.family;
    VKC(vkCreateCommandPool(v.dev, &pi, nullptr, &t.pool));
    VkCommandBufferAllocateInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ci.commandPool = t.pool;
    ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ci.commandBufferCount = kSlices;
    VKC(vkAllocateCommandBuffers(v.dev, &ci, t.cmd));
  }

  // Texture (staged upload)
  VkImage texImg;
  VkDeviceMemory texMem;
  VkImageView texView;
  {
    Buf stage = MakeBuffer(v, scene.texture.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    memcpy(stage.map, scene.texture.data(), scene.texture.size());
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {2048, 1024, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKC(vkCreateImage(v.dev, &ii, nullptr, &texImg));
    VkMemoryRequirements rq;
    vkGetImageMemoryRequirements(v.dev, texImg, &rq);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = rq.size;
    ai.memoryTypeIndex = FindMemory(v, rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    VKC(vkAllocateMemory(v.dev, &ai, nullptr, &texMem));
    VKC(vkBindImageMemory(v.dev, texImg, texMem, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = texImg;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VKC(vkCreateImageView(v.dev, &vi, nullptr, &texView));

    VkCommandBuffer cb = tg[0].cmd[0];
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkBeginCommandBuffer(cb, &bi));
    VkImageMemoryBarrier b1{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b1.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b1.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b1.srcQueueFamilyIndex = b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b1.image = texImg;
    b1.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b1);
    VkBufferImageCopy cp{};
    cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.imageExtent = {2048, 1024, 1};
    vkCmdCopyBufferToImage(cb, stage.buf, texImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    VkImageMemoryBarrier b2 = b1;
    b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b2);
    VKC(vkEndCommandBuffer(cb));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    VKC(vkQueueSubmit(v.queue, 1, &si, VK_NULL_HANDLE));
    VKC(vkQueueWaitIdle(v.queue));
  }

  VkSamplerCreateInfo smi{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  smi.magFilter = smi.minFilter = VK_FILTER_LINEAR;
  smi.addressModeU = smi.addressModeV = smi.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  VkSampler sampler;
  VKC(vkCreateSampler(v.dev, &smi, nullptr, &sampler));

  VkDescriptorSetLayoutBinding dlb{};
  dlb.binding = 0;
  dlb.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  dlb.descriptorCount = 1;
  dlb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dli.bindingCount = 1;
  dli.pBindings = &dlb;
  VkDescriptorSetLayout dsl;
  VKC(vkCreateDescriptorSetLayout(v.dev, &dli, nullptr, &dsl));
  VkDescriptorPoolSize dps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
  VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpi.maxSets = 1;
  dpi.poolSizeCount = 1;
  dpi.pPoolSizes = &dps;
  VkDescriptorPool dpool;
  VKC(vkCreateDescriptorPool(v.dev, &dpi, nullptr, &dpool));
  VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dai.descriptorPool = dpool;
  dai.descriptorSetCount = 1;
  dai.pSetLayouts = &dsl;
  VkDescriptorSet dset;
  VKC(vkAllocateDescriptorSets(v.dev, &dai, &dset));
  VkDescriptorImageInfo dii{sampler, texView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet wds{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  wds.dstSet = dset;
  wds.descriptorCount = 1;
  wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  wds.pImageInfo = &dii;
  vkUpdateDescriptorSets(v.dev, 1, &wds, 0, nullptr);

  VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants)};
  VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pli.setLayoutCount = 1;
  pli.pSetLayouts = &dsl;
  pli.pushConstantRangeCount = 1;
  pli.pPushConstantRanges = &pcr;
  VkPipelineLayout layout;
  VKC(vkCreatePipelineLayout(v.dev, &pli, nullptr, &layout));

  VkShaderModule vs = MakeShader(v, kVert, sizeof(kVert));
  VkShaderModule fs = MakeShader(v, kFrag, sizeof(kFrag));
  VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs;
  stages[0].pName = "main";
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fs;
  stages[1].pName = "main";
  VkVertexInputBindingDescription vb{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription va[4] = {
      {0, 0, VK_FORMAT_R32G32B32_SFLOAT, (uint32_t)offsetof(Vertex, pos)},
      {1, 0, VK_FORMAT_R32G32B32_SFLOAT, (uint32_t)offsetof(Vertex, nrm)},
      {2, 0, VK_FORMAT_R32G32_SFLOAT, (uint32_t)offsetof(Vertex, uv)},
      {3, 0, VK_FORMAT_R8G8B8A8_UNORM, (uint32_t)offsetof(Vertex, col)},
  };
  VkPipelineVertexInputStateCreateInfo vis{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vis.vertexBindingDescriptionCount = 1;
  vis.pVertexBindingDescriptions = &vb;
  vis.vertexAttributeDescriptionCount = 4;
  vis.pVertexAttributeDescriptions = va;
  VkPipelineInputAssemblyStateCreateInfo ias{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vps.viewportCount = 1;
  vps.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  dss.depthTestEnable = VK_TRUE;
  dss.depthWriteEnable = VK_TRUE;
  dss.depthCompareOp = VK_COMPARE_OP_LESS;
  VkPipelineColorBlendAttachmentState cba{};
  cba.colorWriteMask = 0xF;
  VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cbs.attachmentCount = 1;
  cbs.pAttachments = &cba;
  const VkDynamicState dyns[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dys{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dys.dynamicStateCount = 2;
  dys.pDynamicStates = dyns;
  VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gpi.stageCount = 2;
  gpi.pStages = stages;
  gpi.pVertexInputState = &vis;
  gpi.pInputAssemblyState = &ias;
  gpi.pViewportState = &vps;
  gpi.pRasterizationState = &rs;
  gpi.pMultisampleState = &ms;
  gpi.pDepthStencilState = &dss;
  gpi.pColorBlendState = &cbs;
  gpi.pDynamicState = &dys;
  gpi.layout = layout;
  gpi.renderPass = rp;
  VkPipeline pipes[2];  // [0] opaque, [1] alpha blended
  VKC(vkCreateGraphicsPipelines(v.dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipes[0]));
  cba.blendEnable = VK_TRUE;
  cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  cba.colorBlendOp = VK_BLEND_OP_ADD;
  cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  cba.alphaBlendOp = VK_BLEND_OP_ADD;
  VKC(vkCreateGraphicsPipelines(v.dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipes[1]));

  const VkDeviceSize sliceBytes = scene.verts.size() * sizeof(Vertex);
  Buf vbuf = MakeBuffer(v, sliceBytes * kSlices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

  VkFence fences[kSlices];
  for (auto& f : fences) {
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VKC(vkCreateFence(v.dev, &fi, nullptr, &f));
  }

  auto record = [&](int eye, int slice, int meshes) {
    VkCommandBuffer cb = tg[eye].cmd[slice];
    VKC(vkResetCommandBuffer(cb, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkBeginCommandBuffer(cb, &bi));
    VkClearValue cv[2];
    cv[0].color = {{0.1f, 0.1f, 0.2f, 1.0f}};
    cv[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = rp;
    rbi.framebuffer = tg[eye].fb;
    rbi.renderArea = {{0, 0}, {(uint32_t)kEyeW, (uint32_t)kEyeH}};
    rbi.clearValueCount = 2;
    rbi.pClearValues = cv;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, (float)kEyeW, (float)kEyeH, 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {(uint32_t)kEyeW, (uint32_t)kEyeH}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &dset, 0, nullptr);
    const VkDeviceSize off = sliceBytes * slice;
    vkCmdBindVertexBuffers(cb, 0, 1, &vbuf.buf, &off);
    int cur = -1;
    const float shift = eye ? 0.03f : -0.03f;
    PushConstants pc;
    pc.params[0] = 0.3f;
    pc.params[1] = 0.8f;
    pc.params[2] = 0.5f;
    pc.params[3] = 0.0f;
    for (int i = 0; i < meshes; i++) {
      const int want = BlendDraw(i) ? 1 : 0;
      if (want != cur) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes[want]);
        cur = want;
      }
      MakeMvp(pc.mvp, scene.meshes[i], shift, false);
      memcpy(pc.tint, scene.meshes[i].tint, sizeof(pc.tint));
      vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
      vkCmdDraw(cb, kVertsPerMesh, 1, (uint32_t)(i * kVertsPerMesh), 0);
    }
    vkCmdEndRenderPass(cb);
    VKC(vkEndCommandBuffer(cb));
  };

  for (int threads : {1, 2}) {
    for (int meshes : {2850, 5700}) {
      double cpu = 0, t0 = 0;
      for (int f = 0; f < kWarmup + kFrames; f++) {
        const int slice = f % kSlices;
        VKC(vkWaitForFences(v.dev, 1, &fences[slice], VK_TRUE, UINT64_MAX));  // the frame that used this slice is done
        if (f == kWarmup) {
          VKC(vkQueueWaitIdle(v.queue));
          t0 = Now();
          cpu = 0;
        }
        const double a = Now();
        VKC(vkResetFences(v.dev, 1, &fences[slice]));
        memcpy((char*)vbuf.map + sliceBytes * slice, scene.verts.data(), sliceBytes);
        if (threads == 1) {
          record(0, slice, meshes);
          record(1, slice, meshes);
        } else {
          std::thread t([&] { record(1, slice, meshes); });
          record(0, slice, meshes);
          t.join();
        }
        VkCommandBuffer cbs2[2] = {tg[0].cmd[slice], tg[1].cmd[slice]};
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 2;
        si.pCommandBuffers = cbs2;
        VKC(vkQueueSubmit(v.queue, 1, &si, fences[slice]));
        cpu += Now() - a;
      }
      VKC(vkQueueWaitIdle(v.queue));
      const double wall = Now() - t0;
      Result r;
      r.name = threads == 1 ? "Vulkan" : "Vulkan 2T";
      r.meshes = meshes;
      r.cpuMs = cpu / kFrames;
      r.wallMs = wall / kFrames;
      out.push_back(r);
      Log("%-10s %5d meshes/eye: cpu %.2f ms, wall %.2f ms per frame (%.0f fps)", r.name.c_str(), meshes, r.cpuMs, r.wallMs, 1000.0 / r.wallMs);
    }
  }

  vkDeviceWaitIdle(v.dev);
  vkDestroyDevice(v.dev, nullptr);
  vkDestroyInstance(v.inst, nullptr);
}
}  // namespace

namespace VkBench
{
std::string Run(const std::string& outPath)
{
  g_report.clear();
  Log("Vulkan vs OpenGL ES feasibility test: 2 eyes %dx%d, %d vertices per mesh, 8 MB of vertex data per frame", kEyeW, kEyeH, kVertsPerMesh);
  std::vector<Result> results;
  // Run on a worker thread: EGL and Vulkan then start from a clean thread, unrelated to SDL's.
  std::thread worker([&] {
    try {
      Scene scene;
      try {
        RunGL(scene, results);
      } catch (const std::exception& e) {
        Log("OpenGL ES test failed: %s", e.what());
      }
      try {
        RunVulkan(scene, results);
      } catch (const std::exception& e) {
        Log("Vulkan test failed: %s", e.what());
      }
    } catch (const std::exception& e) {
      Log("bench failed: %s", e.what());
    }
  });
  worker.join();

  Log("--- summary (ms per frame; lower is better) ---");
  for (int meshes : {2850, 5700}) {
    std::string line = std::to_string(meshes) + " meshes/eye:";
    for (const auto& r : results)
      if (r.meshes == meshes) {
        char b[96];
        snprintf(b, sizeof(b), "  %s cpu %.1f / wall %.1f;", r.name.c_str(), r.cpuMs, r.wallMs);
        line += b;
      }
    Log("%s", line.c_str());
  }
  if (FILE* f = fopen(outPath.c_str(), "w")) {
    fputs(g_report.c_str(), f);
    fclose(f);
  }
  return g_report;
}
}  // namespace VkBench

#endif  // __ANDROID__

// GL/glew.h replacement for the Android / Meta Quest build.
//
// Supermodel is written against desktop OpenGL 4.1 through GLEW. On Quest we get OpenGL ES 3.2.
// This header is found *before* Src/Pkgs/GL/glew.h (see Android/CMakeLists.txt), so every
// `#include <GL/glew.h>` in the emulator picks up ES 3.2 instead, with no source edits.
//
// It does three jobs:
//   1. pulls in the ES 3.2 headers and the few extension headers we need;
//   2. provides the handful of desktop-only entry points the emulator still calls, built from
//      ES equivalents (glGetTexImage, glGetBufferSubData, glDrawBuffer, glClearDepth, ...);
//   3. rewrites GLSL on its way to the driver: `#version 410 core` -> `#version 320 es` plus the
//      precision statements ES requires (see ConvertShader).
#ifndef ANDROID_GLCOMPAT_GLEW_H
#define ANDROID_GLCOMPAT_GLEW_H

#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3ext.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ---- GLEW stand-ins ----------------------------------------------------------------------------
#define GLEW_OK 0
inline GLenum glewInit() { return GLEW_OK; }
inline const GLubyte* glewGetErrorString(GLenum) { return reinterpret_cast<const GLubyte*>(""); }

namespace GLESCompat
{
inline bool HasExtension(const char* name)
{
  GLint n = 0;
  glGetIntegerv(GL_NUM_EXTENSIONS, &n);
  for (GLint i = 0; i < n; i++) {
    const char* e = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, (GLuint)i));
    if (e && std::strcmp(e, name) == 0)
      return true;
  }
  return false;
}
}  // namespace GLESCompat

#define GLEW_EXT_texture_filter_anisotropic (GLESCompat::HasExtension("GL_EXT_texture_filter_anisotropic"))

// ---- Names that differ between desktop GL and ES -------------------------------------------------
#ifndef GL_BGRA
#define GL_BGRA GL_BGRA_EXT
#endif
#define glClearDepth glClearDepthf
// ES writes to an sRGB attachment through the sRGB encoder unless GL_EXT_sRGB_write_control lets
// us switch that off. Supermodel writes already gamma-encoded colours, so it needs it off.
#define GL_FRAMEBUFFER_SRGB GL_FRAMEBUFFER_SRGB_EXT
// ES has no GL_FRONT read buffer; it only matters for the debug-analysis screenshots.
#define GL_CONTEXT_PROFILE_MASK 0x9126
#define GL_CONTEXT_CORE_PROFILE_BIT 0x00000001
#define GL_CONTEXT_COMPATIBILITY_PROFILE_BIT 0x00000002

// ---- Entry points ES does not have --------------------------------------------------------------
// ES draws to the back buffer of the bound default framebuffer automatically.
inline void glDrawBuffer(GLenum) {}

// ES 3.2 only has glReadnPixels-style readback, so read a texture level through a throw-away FBO.
inline void glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, void* pixels)
{
  GLint tex = 0, prevFbo = 0, w = 0, h = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevFbo);
  glGetTexLevelParameteriv(target, level, GL_TEXTURE_WIDTH, &w);
  glGetTexLevelParameteriv(target, level, GL_TEXTURE_HEIGHT, &h);
  GLuint fbo = 0;
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, (GLuint)tex, level);
  glReadPixels(0, 0, w, h, format, type, pixels);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevFbo);
  glDeleteFramebuffers(1, &fbo);
}

inline void glGetBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, void* data)
{
  void* p = glMapBufferRange(target, offset, size, GL_MAP_READ_BIT);
  if (p) {
    std::memcpy(data, p, (size_t)size);
    glUnmapBuffer(target);
  }
}

// ---- GLSL desktop -> ES ----------------------------------------------------------------------------
namespace GLESCompat
{
// Joins the shader strings, swaps the desktop `#version` line for `#version 320 es` and adds the
// default precisions a fragment shader must declare (usampler2D/isampler2D have no default in ES).
// Shaders with no #version line, and shaders already written for ES, pass through unchanged.
inline std::string ConvertShader(GLenum stage, GLsizei count, const GLchar* const* strings, const GLint* lengths)
{
  std::string src;
  for (GLsizei i = 0; i < count; i++) {
    if (lengths && lengths[i] >= 0)
      src.append(strings[i], (size_t)lengths[i]);
    else
      src.append(strings[i]);
  }

  const size_t v = src.find("#version");
  if (v == std::string::npos)
    return src;
  size_t eol = src.find('\n', v);
  if (eol == std::string::npos)
    eol = src.size();
  const std::string versionLine = src.substr(v, eol - v);
  if (versionLine.find(" es") != std::string::npos)
    return src;

  std::string out = "#version 320 es\n";
  if (stage == GL_FRAGMENT_SHADER) {
    out += "precision highp float;\nprecision highp int;\n"
           "precision highp sampler2D;\nprecision highp isampler2D;\nprecision highp usampler2D;\n";
  }
  out += src.substr(eol);  // keeps the newline, so line numbers shift by a constant in error logs
  return out;
}

inline void ShaderSource(GLuint shader, GLsizei count, const GLchar* const* strings, const GLint* lengths)
{
  GLint stage = 0;
  glGetShaderiv(shader, GL_SHADER_TYPE, &stage);
  const std::string s = ConvertShader((GLenum)stage, count, strings, lengths);
  const GLchar* p = s.c_str();
  glShaderSource(shader, 1, &p, nullptr);
}
}  // namespace GLESCompat

// Route every shader the emulator compiles through the converter. This must come after the
// declarations above, which call the real glShaderSource.
#define glShaderSource(shader, count, strings, lengths) GLESCompat::ShaderSource(shader, count, strings, lengths)

#endif  // ANDROID_GLCOMPAT_GLEW_H

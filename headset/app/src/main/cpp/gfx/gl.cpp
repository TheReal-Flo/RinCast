#include "gl.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "rc/platform.h"
#include "stb/stb_image.h"

#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif

namespace rv {

Program::~Program() {
    if (m_id) glDeleteProgram(m_id);
}

static GLuint Compile(GLenum type, const char* source, const char* name) {
    const GLuint s = glCreateShader(type);
    // `#extension` directives must precede any statement: hoist them above the
    // default precision declaration.
    std::string body(source), extensions;
    for (size_t pos = body.find("#extension"); pos != std::string::npos; pos = body.find("#extension")) {
        const size_t end = body.find('\n', pos);
        extensions += body.substr(pos, end - pos) + "\n";
        body.erase(pos, end == std::string::npos ? std::string::npos : end - pos + 1);
    }
    const std::string full = "#version 310 es\n" + extensions + "precision highp float;\n" + body;
    const char* sources[] = {full.c_str()};
    glShaderSource(s, 1, sources, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        RC_LOGE("Shader %s (%s) failed: %s", name, type == GL_VERTEX_SHADER ? "vs" : "fs", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

bool Program::Build(const char* vertex, const char* fragment, const char* name) {
    const GLuint vs = Compile(GL_VERTEX_SHADER, vertex, name);
    const GLuint fs = Compile(GL_FRAGMENT_SHADER, fragment, name);
    if (!vs || !fs) return false;
    m_id = glCreateProgram();
    glAttachShader(m_id, vs);
    glAttachShader(m_id, fs);
    glLinkProgram(m_id);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(m_id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(m_id, sizeof(log), nullptr, log);
        RC_LOGE("Program %s link failed: %s", name, log);
        return false;
    }
    return true;
}

static void SetSampling(GLenum target, bool mipmaps) {
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, mipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (mipmaps) glTexParameterf(target, GL_TEXTURE_MAX_ANISOTROPY_EXT, 8.f);
}

bool LoadImageTexture(const std::vector<uint8_t>& file, int channels, bool srgb, bool mipmaps, Texture& out) {
    int w = 0, h = 0, n = 0;
    stbi_uc* pixels = stbi_load_from_memory(file.data(), int(file.size()), &w, &h, &n, channels);
    if (!pixels) return false;
    UploadImageTexture(pixels, w, h, channels, srgb, mipmaps, out);
    stbi_image_free(pixels);
    return true;
}

std::vector<uint8_t> DecodeImage(const std::vector<uint8_t>& file, int channels, int& w, int& h) {
    int n = 0;
    stbi_uc* pixels = stbi_load_from_memory(file.data(), int(file.size()), &w, &h, &n, channels);
    if (!pixels) return {};
    std::vector<uint8_t> out(pixels, pixels + size_t(w) * h * channels);
    stbi_image_free(pixels);
    return out;
}

void UploadImageTexture(const uint8_t* pixels, int w, int h, int channels, bool srgb, bool mipmaps, Texture& out) {
    glGenTextures(1, &out.id);
    glBindTexture(GL_TEXTURE_2D, out.id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    const int levels = mipmaps ? int(std::log2(float(std::max(w, h)))) + 1 : 1;
    if (channels == 1) {
        glTexStorage2D(GL_TEXTURE_2D, levels, GL_R8, w, h);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE, pixels);
    } else {
        glTexStorage2D(GL_TEXTURE_2D, levels, srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, w, h);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    }
    if (mipmaps) glGenerateMipmap(GL_TEXTURE_2D);
    SetSampling(GL_TEXTURE_2D, mipmaps);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    out.width = w;
    out.height = h;
}

Texture CreateTexture(int width, int height, GLenum internalFormat, bool mipmaps) {
    Texture t;
    t.width = width;
    t.height = height;
    glGenTextures(1, &t.id);
    glBindTexture(GL_TEXTURE_2D, t.id);
    const int levels = mipmaps ? int(std::log2(float(std::max(width, height)))) + 1 : 1;
    glTexStorage2D(GL_TEXTURE_2D, levels, internalFormat, width, height);
    SetSampling(GL_TEXTURE_2D, mipmaps);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    return t;
}

bool RenderTarget::Create(int width, int height, GLenum colorFormat, bool withDepth, bool mipmaps) {
    Destroy();
    color = CreateTexture(width, height, colorFormat, mipmaps);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color.id, 0);
    if (withDepth) {
        glGenRenderbuffers(1, &depth);
        glBindRenderbuffer(GL_RENDERBUFFER, depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    }
    const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!ok) RC_LOGE("Render target %dx%d incomplete", width, height);
    return ok;
}

void RenderTarget::Destroy() {
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (depth) glDeleteRenderbuffers(1, &depth);
    color.Destroy();
    fbo = depth = 0;
}

void GlCheck(const char* where) {
    const GLenum err = glGetError();
    if (err != GL_NO_ERROR) RC_LOGW("GL error 0x%x at %s", err, where);
}

GLuint EmptyVao() {
    static GLuint vao = 0;
    if (!vao) glGenVertexArrays(1, &vao);
    return vao;
}

}  // namespace rv

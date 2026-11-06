// OpenGL ES 3.2 helpers.
#pragma once

#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>

#include <string>
#include <vector>

namespace rv {

class Program {
   public:
    Program() = default;
    ~Program();
    Program(const Program&) = delete;
    Program& operator=(const Program&) = delete;

    bool Build(const char* vertex, const char* fragment, const char* name);
    void Use() const { glUseProgram(m_id); }
    GLint Uniform(const char* name) const { return glGetUniformLocation(m_id, name); }
    GLuint Id() const { return m_id; }

   private:
    GLuint m_id = 0;
};

struct Texture {
    GLuint id = 0;
    GLenum target = GL_TEXTURE_2D;
    int width = 0;
    int height = 0;
    void Destroy() {
        if (id) glDeleteTextures(1, &id);
        id = 0;
    }
};

// Decodes PNG/JPEG (stb_image). channels: 1 = R8 (SDF atlases), 4 = RGBA8 (sRGB).
bool LoadImageTexture(const std::vector<uint8_t>& file, int channels, bool srgb, bool mipmaps, Texture& out);
Texture CreateTexture(int width, int height, GLenum internalFormat, bool mipmaps);

// Offscreen color (+ optional depth) target.
struct RenderTarget {
    GLuint fbo = 0;
    Texture color;
    GLuint depth = 0;
    bool Create(int width, int height, GLenum colorFormat, bool withDepth, bool mipmaps);
    void Destroy();
};

void GlCheck(const char* where);

// Shared fullscreen/quad vertex array (4 vertices, triangle strip, no buffers).
GLuint EmptyVao();

}  // namespace rv

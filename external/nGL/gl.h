#ifndef GL_H
#define GL_H

#ifndef __cplusplus
#error You need to use a C++ compiler to use nGL!
#endif

//nGL version 0.8
#include "fix.h"

#include "glconfig.h"

//These values are used to calculate offsets into the buffer.
//If you want something like FBOs, make them variables and set them accordingly.
//Watch out for different buffer sizes!
#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240

//GLFix is an integral part of all calculations.
//Changing resolution and width may be an improvement or even break everything.
typedef Fix<8, int32_t> GLFix;

/* Column vectors and matrices:
 * [ [0][0] [0][1] [0][2] [0][3] ]   [x]
 * [ [1][0] [1][1] [1][2] [1][3] ] * [y]
 * [ [2][0] [2][1] [2][2] [2][3] ]   [z]
 * [   0      0      0      1    ]   [1] (implicit)
 *
 * The 4th row (translation) is treated with integer precision only
 * for greater range during matrix multiplication.
 */

/* If TEXTURE_SUPPORT is enabled and a VERTEX has this as color, black pixels of the texture won't be drawn */
#define TEXTURE_TRANSPARENT 0xF000
/* Disables backface culling for this face */
#define TEXTURE_DRAW_BACKFACE 0x0F00

typedef uint16_t COLOR;

struct VECTOR3
{
    VECTOR3() : VECTOR3(0, 0, 0) {}
    VECTOR3(const GLFix x, const GLFix y, const GLFix z)
            : x(x), y(y), z(z) {}

    void print() const { printf("(%d %d %d)\n", x.toInteger<int>(), y.toInteger<int>(), z.toInteger<int>()); }

    GLFix x, y, z;

    VECTOR3& operator+=(const VECTOR3& other) {
        x += other.x;
        y += other.y;
        z += other.z;
        return *this;
    }

    VECTOR3& operator-=(const VECTOR3& other) {
        x -= other.x;
        y -= other.y;
        z -= other.z;
        return *this;
    }

    VECTOR3 operator+(const VECTOR3& other) const {
        return { x + other.x, y + other.y, z + other.z };
    }

    VECTOR3 operator-(const VECTOR3& other) const {
        return { x - other.x, y - other.y, z - other.z };
    }

    VECTOR3 operator*(GLFix scalar) const {
        return { x * scalar, y * scalar, z * scalar };
    }

    VECTOR3& operator*=(GLFix scalar) {
        x *= scalar;
        y *= scalar;
        z *= scalar;
        return *this;
    }

};

struct VERTEX
{
    VERTEX() : VERTEX(0, 0, 0, 0, 0, 0) {}
    VERTEX(const GLFix x, const GLFix y, const GLFix z, const GLFix u, const GLFix v, const COLOR c)
        : x(x), y(y), z(z), u(u), v(v), c(c) {}

    void print() const { printf("(%d %d %d) (0x%x) (%d %d)\n", x.toInteger<int>(), y.toInteger<int>(), z.toInteger<int>(), c, u.toInteger<int>(), v.toInteger<int>()); }

    GLFix x, y, z;
    GLFix u, v;
    COLOR c;
};

struct TEXTURE
{
    uint16_t width; uint16_t height;
    bool has_transparency; COLOR transparent_color;
    COLOR *bitmap;
};

class MATRIX {
public:
    MATRIX() {}
    GLFix data[3][4] = {};
};

#define GL_COLOR_BUFFER_BIT 1<<0
#define GL_DEPTH_BUFFER_BIT 1<<1

enum GLDrawMode
{
    GL_TRIANGLES,
    GL_QUADS,
    GL_QUAD_STRIP, //Not really tested
    GL_LINE_STRIP
};

//Range [0-1]
struct RGB
{
    RGB() : RGB(0,0,0) {}
    RGB(const GLFix r, const GLFix g, const GLFix b) : r(r), g(g), b(b) {}
    GLFix r, g, b;
};

#ifdef FPS_COUNTER
    extern volatile unsigned int fps;
#endif

/* Per-draw raster state (OptiCraft Nspire port). The defaults reproduce stock
 * nGL behaviour. modulate is an RGB565 colour multiplied into every textured
 * pixel (0xFFFF = off); fog_add is added per channel afterwards (0 = off);
 * blend averages the result with the framebuffer. Untextured triangles take
 * their colour from the vertex as before and only see blend/fog/depth. */
struct NGLRasterState
{
    bool depth_test = true;
    bool depth_write = true;
    bool color_write = true;
    bool blend = false;
    COLOR modulate = 0xFFFF;
    COLOR fog_add = 0;
};
extern NGLRasterState ngl_raster;

static inline COLOR ngl_shade(COLOR c, COLOR m)
{
    const unsigned r = (((c >> 11) & 31) * (((m >> 11) & 31) + 1)) >> 5;
    const unsigned g = (((c >> 5) & 63) * (((m >> 5) & 63) + 1)) >> 6;
    const unsigned b = ((c & 31) * ((m & 31) + 1)) >> 5;
    return static_cast<COLOR>((r << 11) | (g << 5) | b);
}

static inline COLOR ngl_add_sat(COLOR c, COLOR a)
{
    unsigned r = ((c >> 11) & 31) + ((a >> 11) & 31);
    unsigned g = ((c >> 5) & 63) + ((a >> 5) & 63);
    unsigned b = (c & 31) + (a & 31);
    if(r > 31) r = 31;
    if(g > 63) g = 63;
    if(b > 31) b = 31;
    return static_cast<COLOR>((r << 11) | (g << 5) | b);
}

template <typename Z>
static inline void ngl_put_pixel(COLOR *screen_px, uint16_t *z_px, COLOR c, const Z z, const bool textured)
{
    if(textured && ngl_raster.modulate != 0xFFFF)
        c = ngl_shade(c, ngl_raster.modulate);
    if(ngl_raster.fog_add)
        c = ngl_add_sat(c, ngl_raster.fog_add);
    if(ngl_raster.color_write)
    {
        if(ngl_raster.blend)
            c = static_cast<COLOR>(((c & 0xF7DE) >> 1) + ((*screen_px & 0xF7DE) >> 1));
        *screen_px = c;
    }
    if(ngl_raster.depth_write)
        *z_px = z;
}
extern MATRIX *transformation;

RGB rgbColor(const COLOR c);
COLOR colorRGB(const RGB rgb);
COLOR colorRGB(const GLFix r, const GLFix g, const GLFix b);

//Invoke once before using any other functions
void nglInit();
void nglUninit();
//The buffer to render to
void nglSetBuffer(COLOR *screenBuf);
void nglSetNearPlane(const GLFix near_plane);
GLFix nglGetNearPlane();
GLFix nglZBufferAt(const unsigned int x, const unsigned int y);
//Display the buffer
void nglDisplay();
void nglSetColor(const COLOR c);
void nglRotateX(const GLFix a);
void nglRotateY(const GLFix a);
void nglRotateZ(const GLFix a);
//To add nGL VERTEX instances directly without using old gl*3f calls
void nglAddVertices(const VERTEX *buffer, unsigned int length);
void nglAddVertex(const VERTEX &vertex);
void nglAddVertex(const VERTEX *vertex);
//Warning: The nglDraw*-Functions apply perspective projection!
//Returns whether the triangle is front-facing
bool nglDrawTriangle(const VERTEX *low, const VERTEX *middle, const VERTEX *high, bool backface_culling = true);
bool nglIsBackface(const VERTEX *v1, const VERTEX *v2, const VERTEX *v3);
void nglDrawTriangleZClipped(const VERTEX *low, const VERTEX *middle, const VERTEX *high);
void nglInterpolateVertexZ(const VERTEX *from, const VERTEX *to, VERTEX *res);
void nglDrawLine3D(const VERTEX *v1, const VERTEX *v2);

void nglPerspective(VERTEX *v);
void nglPerspective(VECTOR3 *v);
void nglMultMatVectRes(const MATRIX *mat1, const VERTEX *vect, VERTEX *res);
void nglMultMatVectRes(const MATRIX *mat1, const VECTOR3 *vect, VECTOR3 *res);
void nglMultMatMat(MATRIX *mat1, const MATRIX *mat2);
const TEXTURE *nglGetTexture();

void glLoadIdentity();
void glBegin(const GLDrawMode mode);
inline void glEnd() { }
void glClear(const int buffers);
//This has effectively only integer precision
void glTranslatef(const GLFix x, const GLFix y, const GLFix z);

void glBindTexture(const TEXTURE *tex);
void glTexCoord2f(const GLFix nu, const GLFix nv);
void glColor3f(const GLFix r, const GLFix g, const GLFix b);
void glVertex3f(const GLFix x, const GLFix y, const GLFix z);
void glScale3f(const GLFix x, const GLFix y, const GLFix z);
void glPushMatrix();
void glPopMatrix();

#endif

/* ============================================================
 *  present_gl_win32.c — OpenGL/WGL backend for the present seam.
 *
 *  A top-level Win32 window + a WGL OpenGL context that draws a
 *  framebuffer as one textured quad, scaled and letterboxed, vsync-
 *  paced. Uses ONLY core OpenGL 1.1 (glTexImage2D/glTexSubImage2D +
 *  immediate-mode quad) plus WGL, so there is nothing to install:
 *  opengl32 + gdi32 + user32 ship with Windows and mingw provides the
 *  headers. No SDL, no GLEW, no COM. Link: -lopengl32 -lgdi32 -luser32.
 *
 *  ---------------------------------------------------------------
 *  HONESTY: like audio_sink_waveout.c, this is glue written from the
 *  Win32/WGL API contract and compile-verified, but NOT run in the
 *  build sandbox (no display, can't run a Windows GUI binary). Whether
 *  a window actually appears and scales correctly is verified on real
 *  Windows by you. The call sequence is the standard one and each step
 *  is commented with what it expects.
 *  ---------------------------------------------------------------
 *
 *  Compatibility note: GL 1.1 wants power-of-two textures, so the
 *  texture is sized to the next power of two and the framebuffer is
 *  uploaded into its top-left corner; the quad's texcoords stop at
 *  fb/tex so only the live region is sampled.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "video/present.h"

#if defined(_WIN32) || defined(__CYGWIN__)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <string.h>

/* wglSwapIntervalEXT is an extension; declare its type and load it at
 * runtime (it is not in the import lib). */
typedef BOOL (WINAPI *PFN_wglSwapIntervalEXT)(int);

static const char *PRESENT_WND_CLASS = "MgPresentWnd";

static struct {
    bool            inited;
    bool            should_close;
    bool            fullscreen;
    PresentAspect   aspect;
    PresentFilter   filter;
    int             fb_w, fb_h;     /* logical framebuffer size  */
    int             tex_w, tex_h;   /* power-of-two texture size */
    HWND            hwnd;
    HDC             hdc;
    HGLRC           hglrc;
    GLuint          tex;
    bool            vsync;          /* swap interval successfully set?       */
    char            renderer[128];  /* GL_RENDERER string (HW vs software GL) */
    LONG_PTR        saved_style;    /* windowed style, for fullscreen toggle */
    WINDOWPLACEMENT saved_place;
} g;

static int next_pot(int v) {
    int p = 1;
    while (p < v) p <<= 1;
    return p;
}

static void apply_filter(void) {
    GLint f = (g.filter == PRESENT_FILTER_LINEAR) ? GL_LINEAR : GL_NEAREST;
    glBindTexture(GL_TEXTURE_2D, g.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:
    case WM_DESTROY:
        g.should_close = true;
        return 0;
    case WM_KEYDOWN:
        switch (wp) {
        case VK_ESCAPE: g.should_close = true; break;
        case VK_F11:    present_set_fullscreen(!g.fullscreen); break;
        case 'A': present_set_aspect(g.aspect == PRESENT_ASPECT_4_3
                                     ? PRESENT_ASPECT_SQUARE : PRESENT_ASPECT_4_3); break;
        case 'F': present_set_filter(g.filter == PRESENT_FILTER_NEAREST
                                     ? PRESENT_FILTER_LINEAR : PRESENT_FILTER_NEAREST); break;
        default: break;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

bool present_init(int fb_w, int fb_h, const char *title) {
    if (g.inited) return true;
    if (fb_w <= 0 || fb_h <= 0) return false;

    memset(&g, 0, sizeof g);
    g.fb_w  = fb_w;  g.fb_h  = fb_h;
    g.tex_w = next_pot(fb_w); g.tex_h = next_pot(fb_h);
    g.aspect = PRESENT_ASPECT_4_3;
    g.filter = PRESENT_FILTER_NEAREST;

    HINSTANCE inst = GetModuleHandleA(NULL);

    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = wndproc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.lpszClassName = PRESENT_WND_CLASS;
    if (!RegisterClassA(&wc)) return false;

    /* Default windowed size: 3x the framebuffer, as a visible starting
     * point; the user can resize or go fullscreen (F11). */
    RECT r = { 0, 0, fb_w * 3, fb_h * 3 };
    DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    AdjustWindowRect(&r, style, FALSE);
    g.hwnd = CreateWindowExA(0, PRESENT_WND_CLASS, title ? title : "present",
                             style, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top,
                             NULL, NULL, inst, NULL);
    if (!g.hwnd) return false;
    g.hdc = GetDC(g.hwnd);

    /* Pixel format: double-buffered RGBA, no depth (we just blit). */
    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize      = sizeof pfd;
    pfd.nVersion   = 1;
    pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.iLayerType = PFD_MAIN_PLANE;
    int pf = ChoosePixelFormat(g.hdc, &pfd);
    if (!pf || !SetPixelFormat(g.hdc, pf, &pfd)) return false;

    g.hglrc = wglCreateContext(g.hdc);
    if (!g.hglrc || !wglMakeCurrent(g.hdc, g.hglrc)) return false;

    /* vsync if the driver exposes it (load the fn ptr; memcpy avoids a
     * function-pointer cast warning under -Werror). */
    {
        PFN_wglSwapIntervalEXT swap_interval;
        PROC raw = wglGetProcAddress("wglSwapIntervalEXT");
        memcpy(&swap_interval, &raw, sizeof swap_interval);
        g.vsync = (swap_interval != NULL) && (swap_interval(1) != FALSE);
    }

    /* Capture the renderer string (HW GPU vs "GDI Generic" software GL). */
    {
        const GLubyte *rs = glGetString(GL_RENDERER);
        const char *s = rs ? (const char *)rs : "(unknown)";
        strncpy(g.renderer, s, sizeof g.renderer - 1u);
        g.renderer[sizeof g.renderer - 1u] = '\0';
    }

    glGenTextures(1, &g.tex);
    glBindTexture(GL_TEXTURE_2D, g.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g.tex_w, g.tex_h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    apply_filter();
    glEnable(GL_TEXTURE_2D);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    g.inited = true;
    return true;
}

void present_frame(const uint32_t *framebuffer) {
    if (!g.inited) return;

    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    if (g.should_close || !framebuffer) return;

    glBindTexture(GL_TEXTURE_2D, g.tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g.fb_w, g.fb_h,
                    GL_RGBA, GL_UNSIGNED_BYTE, framebuffer);

    RECT cr;
    GetClientRect(g.hwnd, &cr);
    int cw = cr.right - cr.left;
    int ch = cr.bottom - cr.top;
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;

    /* Letterboxed viewport, centered in the client area. */
    int vw, vh;
    if (g.aspect == PRESENT_ASPECT_SQUARE) {
        int sx = cw / g.fb_w;
        int sy = ch / g.fb_h;
        int s  = (sx < sy) ? sx : sy;
        if (s < 1) s = 1;
        vw = g.fb_w * s;
        vh = g.fb_h * s;
    } else { /* 4:3 NTSC display aspect */
        const double target = 4.0 / 3.0;
        if ((double)cw / (double)ch > target) {
            vh = ch;
            vw = (int)((double)ch * target + 0.5);
        } else {
            vw = cw;
            vh = (int)((double)cw / target + 0.5);
        }
    }
    int vx = (cw - vw) / 2;
    int vy = (ch - vh) / 2;

    glClear(GL_COLOR_BUFFER_BIT);    /* black letterbox bars */
    glViewport(vx, vy, vw, vh);

    /* Quad over NDC; texcoords stop at fb/tex (POT texture). Top of the
     * image (framebuffer row 0) maps to the top of the window. */
    float su = (float)g.fb_w / (float)g.tex_w;
    float sv = (float)g.fb_h / (float)g.tex_h;
    glBegin(GL_QUADS);
        glTexCoord2f(0.0f, sv);   glVertex2f(-1.0f, -1.0f);
        glTexCoord2f(su,   sv);   glVertex2f( 1.0f, -1.0f);
        glTexCoord2f(su,   0.0f); glVertex2f( 1.0f,  1.0f);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(-1.0f,  1.0f);
    glEnd();

    SwapBuffers(g.hdc);
}

bool present_should_close(void) {
    return g.inited ? g.should_close : true;
}

bool present_vsync_requested(void) {
    return g.inited ? g.vsync : false;
}

const char *present_gl_renderer(void) {
    return g.inited ? g.renderer : "";
}

void present_set_fullscreen(bool on) {
    if (!g.inited || on == g.fullscreen) return;
    if (on) {
        g.saved_style = GetWindowLongPtrA(g.hwnd, GWL_STYLE);
        g.saved_place.length = sizeof g.saved_place;
        GetWindowPlacement(g.hwnd, &g.saved_place);

        HMONITOR mon = MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        memset(&mi, 0, sizeof mi);
        mi.cbSize = sizeof mi;
        if (GetMonitorInfoA(mon, &mi)) {
            SetWindowLongPtrA(g.hwnd, GWL_STYLE, (LONG_PTR)(WS_POPUP | WS_VISIBLE));
            SetWindowPos(g.hwnd, HWND_TOP,
                         mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            g.fullscreen = true;
        }
    } else {
        SetWindowLongPtrA(g.hwnd, GWL_STYLE, g.saved_style);
        SetWindowPlacement(g.hwnd, &g.saved_place);
        SetWindowPos(g.hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g.fullscreen = false;
    }
}

void present_set_aspect(PresentAspect aspect) {
    if (g.inited) g.aspect = aspect;
}

void present_set_filter(PresentFilter filter) {
    if (!g.inited) return;
    g.filter = filter;
    apply_filter();
}

void present_shutdown(void) {
    if (!g.inited) return;
    if (g.tex)   glDeleteTextures(1, &g.tex);
    wglMakeCurrent(NULL, NULL);
    if (g.hglrc) wglDeleteContext(g.hglrc);
    if (g.hdc)   ReleaseDC(g.hwnd, g.hdc);
    if (g.hwnd)  DestroyWindow(g.hwnd);
    UnregisterClassA(PRESENT_WND_CLASS, GetModuleHandleA(NULL));
    memset(&g, 0, sizeof g);
}

#else /* no windowing on this platform */

#include <stddef.h>

bool present_init(int fb_w, int fb_h, const char *title) {
    (void)fb_w; (void)fb_h; (void)title;
    return false;
}
void present_frame(const uint32_t *framebuffer) { (void)framebuffer; }
bool present_should_close(void) { return true; }
bool present_vsync_requested(void) { return false; }
const char *present_gl_renderer(void) { return ""; }
void present_set_fullscreen(bool on) { (void)on; }
void present_set_aspect(PresentAspect aspect) { (void)aspect; }
void present_set_filter(PresentFilter filter) { (void)filter; }
void present_shutdown(void) { }

#endif

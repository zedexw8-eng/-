/* rr_int_gpu.cpp - gpu.h implementation for the in-process renderer.
 *
 * The external client draws through Direct2D on a composition swap chain.
 * Inside the game the same primitives are recorded into ImGui draw lists and
 * rendered by the title's own D3D11 device, so every ESP shape ends up in the
 * game's back buffer instead of a second window.
 */

#include <windows.h>
#include <d3d11.h>
#include <math.h>
#include <stdint.h>

#include "imgui.h"
#include "ui/gpu.h"
#include "internal/rr_weapon_icons.h"

static ImVec2 g_size(1920.0f, 1080.0f);
static ImU32 g_pen = IM_COL32(255, 255, 255, 255);
static float g_pen_width = 1.0f;
static float g_font_px = 12.0f;
/* ESP labels draw with their own font and size. The menu keeps the smooth UI
 * face, the labels keep the pixel face the external client uses. */
static ImFont* g_esp_font;
static float g_esp_px = 13.0f;

/* Weapon icon atlas state; the device arrives from rr_int_main once the
 * swap chain hands it one. */
static ID3D11Device* g_icon_device;
static ID3D11ShaderResourceView* g_icon_view;
static void icon_release(void);

static ImU32 to_im(RGBA c) {
    return IM_COL32((int)((c >> 16) & 0xFF), (int)((c >> 8) & 0xFF), (int)(c & 0xFF),
                    (int)((c >> 24) & 0xFF));
}

static ImDrawList* fg(void) {
    ImDrawList* list = ImGui::GetForegroundDrawList();
    return list ? list : ImGui::GetBackgroundDrawList();
}

int gpu_init(HWND hwnd, int w, int h) {
    (void)hwnd;
    if (w > 0 && h > 0) g_size = ImVec2((float)w, (float)h);
    return 1;
}
void gpu_shutdown(void) {
    icon_release();
    g_icon_device = NULL;
}
int gpu_ready(void) { return 1; }
void gpu_size(int w, int h) { if (w > 0 && h > 0) g_size = ImVec2((float)w, (float)h); }
void gpu_begin(void) {}
int gpu_present(void) { return 1; }
int gpu_frame_ready(void) { return 1; }

void gpu_pen(RGBA c, float width) {
    g_pen = to_im(c);
    g_pen_width = width > 0.0f ? width : 1.0f;
}
void gpu_font(float px) { if (px > 1.0f) g_font_px = px; }
float gpu_esp_distance(float metres) {
    (void)metres;
    return 1.0f;
}

void r_line(float x1, float y1, float x2, float y2) {
    fg()->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), g_pen, g_pen_width);
}
void r_line_c(float x1, float y1, float x2, float y2, RGBA c, float w) {
    fg()->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), to_im(c), w > 0.0f ? w : 1.0f);
}
void r_rect(float x, float y, float w, float h, RGBA c, float stroke) {
    fg()->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), to_im(c), 0.0f, 0,
                  stroke > 0.0f ? stroke : 1.0f);
}
void r_fill(float x, float y, float w, float h, RGBA c) {
    fg()->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), to_im(c));
}
void r_fill_meter(float x, float y, float w, float h, float fraction, RGBA top,
                  RGBA middle, RGBA bottom) {
    if (fraction < 0.0f) fraction = 0.0f;
    if (fraction > 1.0f) fraction = 1.0f;
    ImDrawList* list = fg();
    const float split = h * 0.5f;
    list->AddRectFilledMultiColor(ImVec2(x, y), ImVec2(x + w, y + split),
                                  to_im(bottom), to_im(bottom), to_im(middle), to_im(middle));
    list->AddRectFilledMultiColor(ImVec2(x, y + split), ImVec2(x + w, y + h),
                                  to_im(middle), to_im(middle), to_im(top), to_im(top));
    const float filled = h * fraction;
    list->AddRectFilled(ImVec2(x, y + h - filled), ImVec2(x + w, y + h),
                        IM_COL32(0, 0, 0, 90));
}

static void ellipse_points(float cx, float cy, float rx, float ry, ImVec2* out, int n) {
    for (int i = 0; i < n; i++) {
        const float a = (float)i * 6.28318530718f / (float)n;
        out[i] = ImVec2(cx + cosf(a) * rx, cy + sinf(a) * ry);
    }
}

void r_ellipse(float cx, float cy, float rx, float ry, RGBA c, float stroke) {
    ImVec2 pts[48];
    ellipse_points(cx, cy, rx, ry, pts, 48);
    fg()->AddPolyline(pts, 48, to_im(c), ImDrawFlags_Closed, stroke > 0.0f ? stroke : 1.0f);
}
void r_fill_ellipse(float cx, float cy, float rx, float ry, RGBA c) {
    ImVec2 pts[48];
    ellipse_points(cx, cy, rx, ry, pts, 48);
    fg()->AddConvexPolyFilled(pts, 48, to_im(c));
}
static ImU32 lerp_im(RGBA a, RGBA b, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const float inv = 1.0f - t;
    const int r = (int)(((a >> 16) & 0xFF) * inv + ((b >> 16) & 0xFF) * t + 0.5f);
    const int g = (int)(((a >> 8) & 0xFF) * inv + ((b >> 8) & 0xFF) * t + 0.5f);
    const int bl = (int)((a & 0xFF) * inv + (b & 0xFF) * t + 0.5f);
    const int al = (int)(((a >> 24) & 0xFF) * inv + ((b >> 24) & 0xFF) * t + 0.5f);
    return IM_COL32(r, g, bl, al);
}

/* Linear gradient across the ellipse's own silhouette: a triangle fan whose
 * rim vertices are colored by their projection on the angle axis, so the fill
 * keeps the circle shape instead of bleeding past it as a rectangle. */
void r_fill_ellipse_gradient(float cx, float cy, float rx, float ry, RGBA from,
                             RGBA to, float angle) {
    if (!(rx > 0.0f) || !(ry > 0.0f)) return;
    const int seg = 64;
    const float ax = cosf(angle), ay = sinf(angle);
    const float span = rx > ry ? rx : ry;
    ImDrawList* list = fg();
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    list->PrimReserve(seg * 3, seg + 1);
    /* Indices are relative to _VtxCurrentIdx, which PrimReserve resets when a
     * new vertex offset starts; capture it after the reserve. */
    const ImDrawIdx base = (ImDrawIdx)list->_VtxCurrentIdx;
    list->PrimWriteVtx(ImVec2(cx, cy), uv, lerp_im(from, to, 0.5f));
    for (int i = 0; i < seg; i++) {
        const float a = (float)i * 6.28318530718f / (float)seg;
        const float px = cosf(a) * rx, py = sinf(a) * ry;
        const float proj = (px * ax + py * ay) / span;
        list->PrimWriteVtx(ImVec2(cx + px, cy + py), uv,
                           lerp_im(from, to, 0.5f + 0.5f * proj));
    }
    for (int i = 0; i < seg; i++) {
        list->PrimWriteIdx(base);
        list->PrimWriteIdx((ImDrawIdx)(base + 1 + i));
        list->PrimWriteIdx((ImDrawIdx)(base + 1 + (i + 1) % seg));
    }
}
/* Shared hull fill body: colours == NULL means one flat tint for the mesh. */
static int tri_fill(const float* xy, const RGBA* colours, RGBA solid, int vertex_count,
                    const unsigned short* indices, int triangle_count) {
    if (!xy || !indices || vertex_count < 1 || vertex_count >= 65536 || triangle_count < 1) return 0;
    int valid = 0;
    for (int i = 0; i < triangle_count * 3; i += 3) {
        const unsigned a = indices[i], b = indices[i + 1], c = indices[i + 2];
        if (a < (unsigned)vertex_count && b < (unsigned)vertex_count && c < (unsigned)vertex_count &&
            isfinite(xy[2*a]) && isfinite(xy[2*a+1]) && isfinite(xy[2*b]) && isfinite(xy[2*b+1]) &&
            isfinite(xy[2*c]) && isfinite(xy[2*c+1])) ++valid;
    }
    if (!valid) return 0;
    ImDrawList* list = fg();
    /* The ESP draw list is 16-bit indexed; a mesh that would cross the limit
     * is refused rather than allowed to wrap the index buffer. */
    if (list->VtxBuffer.Size + vertex_count > 65000) return 0;
    const ImU32 col = to_im(solid);
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    list->PrimReserve(valid * 3, vertex_count);
    const unsigned base = list->_VtxCurrentIdx;
    for (int i = 0; i < vertex_count; ++i) {
        const ImVec2 p = isfinite(xy[2*i]) && isfinite(xy[2*i+1])
            ? ImVec2(xy[2*i], xy[2*i+1]) : ImVec2(0, 0);
        list->PrimWriteVtx(p, uv, colours ? to_im(colours[i]) : col);
    }
    for (int i = 0; i < triangle_count * 3; i += 3) {
        const unsigned a = indices[i], b = indices[i + 1], c = indices[i + 2];
        if (a >= (unsigned)vertex_count || b >= (unsigned)vertex_count || c >= (unsigned)vertex_count ||
            !isfinite(xy[2*a]) || !isfinite(xy[2*a+1]) || !isfinite(xy[2*b]) || !isfinite(xy[2*b+1]) ||
            !isfinite(xy[2*c]) || !isfinite(xy[2*c+1])) continue;
        list->PrimWriteIdx((ImDrawIdx)(base + a));
        list->PrimWriteIdx((ImDrawIdx)(base + b));
        list->PrimWriteIdx((ImDrawIdx)(base + c));
    }
    return 1;
}
int r_fill_triangles(const float* xy, int vertex_count, const unsigned short* indices,
                     int triangle_count, RGBA color) {
    return tri_fill(xy, NULL, color, vertex_count, indices, triangle_count);
}
int r_fill_triangles_shaded(const float* xy, const RGBA* colours, int vertex_count,
                            const unsigned short* indices, int triangle_count, RGBA color) {
    return tri_fill(xy, colours, color, vertex_count, indices, triangle_count);
}
void gpu_mesh_flush(void) {}
void r_polyline(const float* xy, int n, RGBA c, float w) {
    if (!xy || n < 2) return;
    ImVec2 pts[64];
    if (n > 64) n = 64;
    for (int i = 0; i < n; i++) pts[i] = ImVec2(xy[i * 2], xy[i * 2 + 1]);
    fg()->AddPolyline(pts, n, to_im(c), 0, w > 0.0f ? w : 1.0f);
}

static void text_outline(ImFont* font, float px, float x, float y, const char* utf8,
                         ImU32 col, int flags) {
    if (!font || !(px > 0.0f)) return;
    ImDrawList* list = fg();
    const ImVec2 size = font->CalcTextSizeA(px, FLT_MAX, 0.0f, utf8);
    if (flags & RT_CENTER) x -= size.x * 0.5f;
    else if (flags & RT_RIGHT) x -= size.x;
    if (flags & RT_BOTTOM) y -= px;
    /* The baked glyphs only land on whole pixels. A centered label sits on a
     * half pixel and every stroke is filtered across two rows, which is the
     * crawl the ESP text showed while the floored menu text stayed sharp. */
    x = floorf(x);
    y = floorf(y);
    const ImU32 halo = IM_COL32(0, 0, 0, 255);
    if (flags & RT_OUTLINE) {
        for (int ox = -1; ox <= 1; ox++)
            for (int oy = -1; oy <= 1; oy++)
                if (ox || oy)
                    list->AddText(font, px, ImVec2(x + ox, y + oy), halo, utf8);
    }
    list->AddText(font, px, ImVec2(x, y), col, utf8);
}

void r_text(float x, float y, const char* utf8, RGBA c, int flags) {
    if (!utf8 || !*utf8) return;
    text_outline(ImGui::GetFont(), g_font_px, x, y, utf8, to_im(c), flags);
}
void gpu_ui_font(float px) { gpu_font(px); }
void r_ui_text(float x, float y, const char* utf8, RGBA c, int flags) {
    r_text(x, y, utf8, c, flags);
}
float r_ui_text_width(const char* utf8) { return r_text_width(utf8); }
void gpu_set_esp_font(void* font, float px) {
    g_esp_font = (ImFont*)font;
    if (px > 1.0f) g_esp_px = px;
}
float r_esp_text(float x, float y, const char* utf8, RGBA c, int flags) {
    if (!utf8 || !*utf8) return 0;
    ImFont* font = g_esp_font ? g_esp_font : ImGui::GetFont();
    text_outline(font, g_esp_px, x, y, utf8, to_im(c), flags);
    /* One row of the pixel face; callers stack labels by this height. */
    return g_esp_px;
}
void r_text_w(float x, float y, const wchar_t* s, int len, RGBA c, int flags) {
    if (!s) return;
    char utf8[512];
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, len, utf8, (int)sizeof(utf8) - 1, NULL, NULL);
    if (n <= 0) return;
    utf8[n] = 0;
    r_text(x, y, utf8, c, flags);
}
float r_text_width(const char* utf8) {
    if (!utf8) return 0.0f;
    return ImGui::GetFont()->CalcTextSizeA(g_font_px, FLT_MAX, 0.0f, utf8).x;
}
float r_text_height(void) {
    return ImGui::GetFont()->CalcTextSizeA(g_font_px, FLT_MAX, 0.0f, "A").y;
}

/* ---- held-item icon atlas ----
 * One 224x210 texture; each icon is 52x26 (weapons) or 28x28 (items) inside
 * a 56x30 cell so the linear sampler has a transparent gutter to blend into.
 * ImGui multiplies the texture by the draw color, so white weapon art tints
 * with the ESP weapon color while item art is drawn with a white tint to
 * keep the game's own colors. */

static void icon_release(void) {
    if (g_icon_view) { g_icon_view->Release(); g_icon_view = NULL; }
}

void gpu_set_device(void* d3d11_device) {
    ID3D11Device* device = (ID3D11Device*)d3d11_device;
    if (device == g_icon_device) return;
    icon_release();
    g_icon_device = device;
}

static ID3D11ShaderResourceView* icon_texture(void) {
    if (g_icon_view || !g_icon_device) return g_icon_view;
    D3D11_TEXTURE2D_DESC desc;
    ZeroMemory(&desc, sizeof(desc));
    desc.Width = RR_WI_ATLAS_W;
    desc.Height = RR_WI_ATLAS_H;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sub;
    sub.pSysMem = rr_weapon_icon_rgba8;
    sub.SysMemPitch = RR_WI_ATLAS_W * 4;
    sub.SysMemSlicePitch = 0;
    ID3D11Texture2D* texture = NULL;
    const HRESULT created = g_icon_device->CreateTexture2D(&desc, &sub, &texture);
    if (FAILED(created) || !texture) return NULL;
    D3D11_SHADER_RESOURCE_VIEW_DESC view;
    ZeroMemory(&view, sizeof(view));
    view.Format = desc.Format;
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1;
    const HRESULT hr = g_icon_device->CreateShaderResourceView(texture, &view, &g_icon_view);
    texture->Release();
    if (FAILED(hr)) g_icon_view = NULL;
    return g_icon_view;
}

float r_weapon_icon(int icon, float cx, float top, RGBA tint) {
    if (icon < 0 || icon >= RR_WI_COUNT) return 0.0f;
    ID3D11ShaderResourceView* srv = icon_texture();
    if (!srv) return 0.0f;
    /* Draw the trimmed art rect, not the padded cell: a short weapon then sits
     * right under the label instead of on a shared cell baseline. */
    const float* box = rr_weapon_icon_rect[icon];
    const float w = box[2], h = box[3];
    const float x = floorf(cx - w * 0.5f);
    const float y = floorf(top);
    const float u0 = box[0] / (float)RR_WI_ATLAS_W;
    const float v0 = box[1] / (float)RR_WI_ATLAS_H;
    const float u1 = (box[0] + w) / (float)RR_WI_ATLAS_W;
    const float v1 = (box[1] + h) / (float)RR_WI_ATLAS_H;
    fg()->AddImage(ImTextureRef((ImTextureID)(intptr_t)srv), ImVec2(x, y),
                   ImVec2(x + w, y + h), ImVec2(u0, v0), ImVec2(u1, v1), to_im(tint));
    return h;
}

void gpu_chams_alloc(int w, int h) { (void)w; (void)h; }
void gpu_chams_upload(const uint32_t* px, int w, int h, int stride, int x0, int y0,
                      int x1, int y1) {
    (void)px; (void)w; (void)h; (void)stride; (void)x0; (void)y0; (void)x1; (void)y1;
}
void gpu_chams_draw(int outline_px, RGBA outline_col, float opacity) {
    (void)outline_px; (void)outline_col; (void)opacity;
}

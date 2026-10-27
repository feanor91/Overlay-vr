// Rendu D3D11 de l'editeur : cubes colores (aretes, laser, curseurs) et panneaux de texte (aide, etat).
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "../common/mathx.h"
#include "../common/zones.h"

namespace edr {

using ovm::Mat4;
using ovm::V3;

[[noreturn]] inline void failMsg(const char* what) { throw std::runtime_error(what); }

const char* const kShader = R"(
cbuffer CB : register(b0) { float4x4 viewProj; float4x4 world; float4 color; };
float4 VS(float3 p : POSITION) : SV_POSITION { return mul(viewProj, mul(world, float4(p, 1))); }
float4 PS() : SV_TARGET { return color; }

Texture2D tex : register(t0);
SamplerState smp : register(s0);
struct VO { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VO VSP(float3 p : POSITION, float2 uv : TEXCOORD0) {
    VO o;
    o.pos = mul(viewProj, mul(world, float4(p, 1)));
    o.uv = uv;
    return o;
}
float4 PSP(VO i) : SV_TARGET { return tex.Sample(smp, i.uv); }
)";

struct CB {
    Mat4 viewProj, world;
    float color[4];
};

struct Renderer {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11VertexShader *vs = nullptr, *vsp = nullptr;
    ID3D11PixelShader *ps = nullptr, *psp = nullptr;
    ID3D11InputLayout *layout = nullptr, *layoutP = nullptr;
    ID3D11Buffer *vb = nullptr, *vbQuad = nullptr, *cb = nullptr;
    ID3D11BlendState *blend = nullptr, *blendOff = nullptr;
    ID3D11RasterizerState* raster = nullptr;
    ID3D11DepthStencilState *depth = nullptr, *depthOff = nullptr;
    ID3D11SamplerState* sampler = nullptr;
    Mat4 viewProj;

    ID3DBlob* compile(const char* entry, const char* target) {
        ID3DBlob *blob = nullptr, *err = nullptr;
        if (FAILED(D3DCompile(kShader, std::strlen(kShader), nullptr, nullptr, nullptr, entry, target, 0, 0, &blob, &err))) {
            std::string msg = std::string("compilation du shader ") + entry;
            if (err) msg += std::string(" : ") + static_cast<const char*>(err->GetBufferPointer());
            failMsg(msg.c_str());
        }
        return blob;
    }

    void init(ID3D11Device* d, ID3D11DeviceContext* c) {
        dev = d;
        ctx = c;
        ID3DBlob* b = compile("VS", "vs_5_0");
        dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs);
        D3D11_INPUT_ELEMENT_DESC el = {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
        dev->CreateInputLayout(&el, 1, b->GetBufferPointer(), b->GetBufferSize(), &layout);
        b->Release();
        b = compile("PS", "ps_5_0");
        dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ps);
        b->Release();
        b = compile("VSP", "vs_5_0");
        dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vsp);
        D3D11_INPUT_ELEMENT_DESC elp[2] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}};
        dev->CreateInputLayout(elp, 2, b->GetBufferPointer(), b->GetBufferSize(), &layoutP);
        b->Release();
        b = compile("PSP", "ps_5_0");
        dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &psp);
        b->Release();

        // Cube unite centre en 0 : 6 faces x 2 triangles.
        const float c8[8][3] = {{-.5f, -.5f, -.5f}, {.5f, -.5f, -.5f}, {.5f, .5f, -.5f}, {-.5f, .5f, -.5f},
                                {-.5f, -.5f, .5f},  {.5f, -.5f, .5f},  {.5f, .5f, .5f},  {-.5f, .5f, .5f}};
        const int idx[36] = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 4, 7, 0, 7, 3,
                             1, 2, 6, 1, 6, 5, 3, 7, 6, 3, 6, 2, 0, 1, 5, 0, 5, 4};
        float verts[36][3];
        for (int i = 0; i < 36; i++) std::memcpy(verts[i], c8[idx[i]], sizeof verts[i]);
        D3D11_BUFFER_DESC bd = {sizeof verts, D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA sd = {verts, 0, 0};
        dev->CreateBuffer(&bd, &sd, &vb);

        // Quad unite (plan XY, normale +Z) avec coordonnees de texture.
        const float quad[6][5] = {{-.5f, -.5f, 0, 0, 1}, {-.5f, .5f, 0, 0, 0}, {.5f, .5f, 0, 1, 0},
                                  {-.5f, -.5f, 0, 0, 1}, {.5f, .5f, 0, 1, 0},  {.5f, -.5f, 0, 1, 1}};
        D3D11_BUFFER_DESC qd = {sizeof quad, D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA qs = {quad, 0, 0};
        dev->CreateBuffer(&qd, &qs, &vbQuad);

        D3D11_BUFFER_DESC cbd = {sizeof(CB), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        dev->CreateBuffer(&cbd, nullptr, &cb);

        D3D11_BLEND_DESC bl = {};
        bl.RenderTarget[0].BlendEnable = TRUE;
        bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        dev->CreateBlendState(&bl, &blend);
        bl.RenderTarget[0].BlendEnable = FALSE;
        dev->CreateBlendState(&bl, &blendOff);

        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        dev->CreateRasterizerState(&rd, &raster);

        D3D11_DEPTH_STENCIL_DESC dd = {};
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_LESS;
        dev->CreateDepthStencilState(&dd, &depth);
        dd.DepthEnable = FALSE;
        dev->CreateDepthStencilState(&dd, &depthOff);

        D3D11_SAMPLER_DESC sm = {};
        sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sm.MaxLOD = D3D11_FLOAT32_MAX;
        dev->CreateSamplerState(&sm, &sampler);
    }

    // Objets opaques (aretes, laser, curseurs). Rien n'est translucide : un melange avec le fond magenta
    // donnerait une couleur que le chroma key de Virtual Desktop ne reconnait plus.
    void begin() {
        UINT stride = 12, offset = 0;
        ctx->IASetInputLayout(layout);
        ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &cb);
        ctx->PSSetConstantBuffers(0, 1, &cb);  // le pixel shader lit `color` : sans ca tout est transparent
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->RSSetState(raster);
        ctx->OMSetBlendState(blendOff, nullptr, 0xffffffff);
        ctx->OMSetDepthStencilState(depth, 0);
    }

    void box(const Mat4& world, const float rgba[4]) {
        CB data;
        data.viewProj = viewProj;
        data.world = world;
        std::memcpy(data.color, rgba, sizeof data.color);
        ctx->UpdateSubresource(cb, 0, nullptr, &data, 0, 0);
        ctx->Draw(36, 0);
    }

    // Panneaux de texte : dessines en dernier, par-dessus tout.
    void beginPanels() {
        UINT stride = 20, offset = 0;
        ctx->IASetInputLayout(layoutP);
        ctx->IASetVertexBuffers(0, 1, &vbQuad, &stride, &offset);
        ctx->VSSetShader(vsp, nullptr, 0);
        ctx->PSSetShader(psp, nullptr, 0);
        ctx->PSSetSamplers(0, 1, &sampler);
        ctx->OMSetBlendState(blendOff, nullptr, 0xffffffff);
        ctx->OMSetDepthStencilState(depthOff, 0);
    }

    void panel(const Mat4& world, ID3D11ShaderResourceView* srv) {
        CB data;
        data.viewProj = viewProj;
        data.world = world;
        std::memset(data.color, 0, sizeof data.color);
        ctx->UpdateSubresource(cb, 0, nullptr, &data, 0, 0);
        ctx->PSSetShaderResources(0, 1, &srv);
        ctx->Draw(6, 0);
    }
};

// Panneau de texte : les lignes sont dessinees avec GDI (polices Windows, accents) dans une texture.
// Une ligne commençant par '#' est un titre (or), par '!' un avertissement (orange).
class TextPanel {
public:
    void init(ID3D11Device* dev, ID3D11DeviceContext* ctx, int w, int h) {
        ctx_ = ctx;
        w_ = w;
        h_ = h;
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        dev->CreateTexture2D(&td, nullptr, &tex_);
        dev->CreateShaderResourceView(tex_, nullptr, &srv_);
        dc_ = CreateCompatibleDC(nullptr);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;  // de haut en bas
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bmp_ = CreateDIBSection(dc_, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
        SelectObject(dc_, bmp_);
        font_ = CreateFontW(-21, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        fontBold_ = CreateFontW(-23, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        SetBkMode(dc_, TRANSPARENT);
    }

    ID3D11ShaderResourceView* srv() const { return srv_; }

    // Redessine seulement si le contenu a change.
    void setLines(const std::vector<std::wstring>& lines) {
        if (lines == last_) return;
        last_ = lines;
        auto* px = static_cast<uint32_t*>(bits_);
        for (int i = 0; i < w_ * h_; i++) px[i] = 0xFF14141C;  // fond sombre opaque
        RECT border = {0, 0, w_, h_};
        HBRUSH br = CreateSolidBrush(RGB(110, 110, 150));
        FrameRect(dc_, &border, br);
        RECT inner = {1, 1, w_ - 1, h_ - 1};
        FrameRect(dc_, &inner, br);
        DeleteObject(br);
        int y = 8;
        for (const std::wstring& raw : lines) {
            std::wstring s = raw;
            COLORREF col = RGB(235, 235, 235);
            HGDIOBJ f = font_;
            if (!s.empty() && s[0] == L'#') {
                col = RGB(255, 205, 70);
                f = fontBold_;
                s.erase(0, 1);
            } else if (!s.empty() && s[0] == L'!') {
                col = RGB(255, 140, 60);
                s.erase(0, 1);
            }
            SelectObject(dc_, f);
            SetTextColor(dc_, col);
            TextOutW(dc_, 14, y, s.c_str(), static_cast<int>(s.size()));
            y += 27;
        }
        for (int i = 0; i < w_ * h_; i++) px[i] |= 0xFF000000;
        ctx_->UpdateSubresource(tex_, 0, nullptr, bits_, w_ * 4, 0);
    }

private:
    ID3D11DeviceContext* ctx_ = nullptr;
    ID3D11Texture2D* tex_ = nullptr;
    ID3D11ShaderResourceView* srv_ = nullptr;
    HDC dc_ = nullptr;
    HBITMAP bmp_ = nullptr;
    HFONT font_ = nullptr, fontBold_ = nullptr;
    void* bits_ = nullptr;
    int w_ = 0, h_ = 0;
    std::vector<std::wstring> last_;
};

// Boite orientee : centre + yaw + dimensions.
inline Mat4 boxWorld(V3 center, float yaw, V3 size) {
    return ovm::mmul(ovm::mFromPose(center, ovm::qaxis('y', yaw)), ovm::mScale(size));
}

// Zone en fil de fer : aretes epaisses et opaques (visibles de loin, et le bureau reste visible au travers).
inline void drawZone(Renderer& r, const ovz::Zone& z, const float edgeRgba[4], float edge) {
    const bool plane = z.type == ovz::ZoneType::Plane;
    const V3 size = {z.size[0], plane ? 0.0f : z.size[1], z.size[2]};
    const V3 c = {z.pos[0], z.pos[1], z.pos[2]};
    Mat4 frame = ovm::mFromPose(c, ovm::qaxis('y', z.yaw));
    auto bar = [&](V3 pos, V3 dim) {
        r.box(ovm::mmul(frame, ovm::mmul(ovm::mTranslate(pos), ovm::mScale(dim))), edgeRgba);
    };
    const float sx[2] = {-0.5f, 0.5f};
    for (float s1 : sx) {
        if (plane) {  // 4 aretes horizontales
            bar({0, 0, s1 * size.z}, {size.x + edge, edge, edge});
            bar({s1 * size.x, 0, 0}, {edge, edge, size.z + edge});
            continue;
        }
        for (float s2 : sx) {  // 12 aretes du volume
            bar({0, s1 * size.y, s2 * size.z}, {size.x + edge, edge, edge});
            bar({s1 * size.x, 0, s2 * size.z}, {edge, size.y + edge, edge});
            bar({s1 * size.x, s2 * size.y, 0}, {edge, edge, size.z + edge});
        }
    }
}

}  // namespace edr

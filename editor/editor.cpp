// Editeur OverlayVR : application OpenXR (D3D11) a lancer dans le casque avant un jeu.
// Le fond est magenta (Virtual Desktop le remplace par le passthrough), on voit donc la piece
// et on dessine les zones avec les manettes. Tout est enregistre dans zones.json.
//
// Commandes (manettes Touch) :
//   Gachette droite   maintenue : dessine une zone en tirant de l'angle A a l'angle opposé B
//   A (droite)        bascule entre VOLUME (cyan, HOTAS/manettes) et PLAN (jaune, clavier/souris)
//   Grip droit        attrape la zone la plus proche ; stick droit : X tourne, Y agrandit/reduit
//   B (droite)        supprime la zone attrapee
//   X (gauche)        annule la derniere zone creee
//   Y (gauche)        active/desactive l'affichage conditionnel (regard >= 25 degres vers le bas)
//   Menu (gauche)     quitte
#define NOMINMAX
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <windows.h>
#include <unknwn.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "../common/mathx.h"
#include "../common/zones.h"

using ovm::Mat4;
using ovm::Q;
using ovm::V3;
using ovz::Zone;
using ovz::ZoneType;

namespace {

constexpr float kLookDegrees = 25.0f;
constexpr float kTriggerOn = 0.6f;
constexpr float kGrabMargin = 0.08f;
constexpr float kEdge = 0.01f;

[[noreturn]] void fail(const char* what, XrResult r = XR_SUCCESS) {
    char msg[256];
    std::snprintf(msg, sizeof msg, "%s (XrResult %d)", what, static_cast<int>(r));
    throw std::runtime_error(msg);
}
#define CHK(x)                                    \
    do {                                          \
        XrResult r_ = (x);                        \
        if (XR_FAILED(r_)) fail(#x, r_);          \
    } while (0)

// --- rendu D3D11 -----------------------------------------------------------------------------
const char* kShader = R"(
cbuffer CB : register(b0) { float4x4 viewProj; float4x4 world; float4 color; };
float4 VS(float3 p : POSITION) : SV_POSITION { return mul(viewProj, mul(world, float4(p, 1))); }
float4 PS() : SV_TARGET { return color; }
)";

struct CB {
    Mat4 viewProj, world;
    float color[4];
};

struct Renderer {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11InputLayout* layout = nullptr;
    ID3D11Buffer* vb = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11BlendState* blend = nullptr;
    ID3D11RasterizerState* raster = nullptr;
    ID3D11DepthStencilState* depth = nullptr;
    Mat4 viewProj;

    void init(ID3D11Device* d, ID3D11DeviceContext* c) {
        dev = d;
        ctx = c;
        ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
        if (FAILED(D3DCompile(kShader, std::strlen(kShader), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vsb, &err)))
            fail("compilation du vertex shader");
        if (FAILED(D3DCompile(kShader, std::strlen(kShader), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &psb, &err)))
            fail("compilation du pixel shader");
        dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
        dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
        D3D11_INPUT_ELEMENT_DESC el = {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
        dev->CreateInputLayout(&el, 1, vsb->GetBufferPointer(), vsb->GetBufferSize(), &layout);
        vsb->Release();
        psb->Release();

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
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        dev->CreateRasterizerState(&rd, &raster);
        D3D11_DEPTH_STENCIL_DESC dd = {};
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;  // translucide : pas d'ecriture de profondeur
        dd.DepthFunc = D3D11_COMPARISON_LESS;
        dev->CreateDepthStencilState(&dd, &depth);
    }

    void begin() {
        UINT stride = 12, offset = 0;
        ctx->IASetInputLayout(layout);
        ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &cb);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->RSSetState(raster);
        ctx->OMSetBlendState(blend, nullptr, 0xffffffff);
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
};

// Boite orientee : centre + yaw + dimensions.
Mat4 boxWorld(V3 center, float yaw, V3 size) {
    return ovm::mmul(ovm::mFromPose(center, ovm::qaxis('y', yaw)), ovm::mScale(size));
}

void drawZone(Renderer& r, const Zone& z, const float faceRgb[3], const float edgeRgba[4]) {
    const bool plane = z.type == ZoneType::Plane;
    const V3 size = {z.size[0], plane ? 0.004f : z.size[1], z.size[2]};
    const V3 c = {z.pos[0], z.pos[1], z.pos[2]};
    const float face[4] = {faceRgb[0], faceRgb[1], faceRgb[2], 0.30f};
    r.box(boxWorld(c, z.yaw, size), face);

    // Aretes : fines barres positionnees dans le repere de la zone.
    Mat4 frame = ovm::mFromPose(c, ovm::qaxis('y', z.yaw));
    auto bar = [&](V3 pos, V3 dim) {
        r.box(ovm::mmul(frame, ovm::mmul(ovm::mTranslate(pos), ovm::mScale(dim))), edgeRgba);
    };
    const float sx[2] = {-0.5f, 0.5f};
    for (float s1 : sx) {
        if (plane) {  // 4 aretes horizontales
            bar({0, 0, s1 * size.z}, {size.x, kEdge, kEdge});
            bar({s1 * size.x, 0, 0}, {kEdge, kEdge, size.z});
            continue;
        }
        for (float s2 : sx) {  // 12 aretes du volume
            bar({0, s1 * size.y, s2 * size.z}, {size.x, kEdge, kEdge});
            bar({s1 * size.x, 0, s2 * size.z}, {kEdge, size.y, kEdge});
            bar({s1 * size.x, s2 * size.y, 0}, {kEdge, kEdge, size.z});
        }
    }
}

bool insideZone(const Zone& z, V3 p, float margin) {
    V3 d = ovm::qrot(ovm::qaxis('y', -z.yaw), p - V3{z.pos[0], z.pos[1], z.pos[2]});
    const float hy = z.type == ZoneType::Plane ? 0.0f : z.size[1] / 2;
    return std::fabs(d.x) <= z.size[0] / 2 + margin && std::fabs(d.y) <= hy + margin &&
           std::fabs(d.z) <= z.size[2] / 2 + margin;
}

// --- application OpenXR ---------------------------------------------------------------------
struct Hand {
    XrPath path = 0;
    XrSpace space = XR_NULL_HANDLE;
    bool tracked = false;
    V3 pos;
    Q rot;
};

struct EyeSwapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    int32_t width = 0, height = 0;
    std::vector<XrSwapchainImageD3D11KHR> images;
    std::vector<ID3D11RenderTargetView*> rtvs;
    ID3D11Texture2D* depthTex = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
};

class Editor {
public:
    void run() {
        SetEnvironmentVariableA("OVERLAYVR_DISABLE", "1");  // la couche globale ne doit pas s'ajouter ici
        ovz::loadZones(ovz::zonesPath(), zones_);
        std::printf("OverlayVR editeur - %zu zone(s) chargee(s) depuis %s\n", zones_.size(),
                    ovz::zonesPath().string().c_str());
        initOpenXR();
        initGraphics();
        initSession();
        while (!quit_) {
            pollEvents();
            if (!running_) {
                Sleep(20);
                continue;
            }
            frame();
        }
        if (running_) xrEndSession(session_);
    }

private:
    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId system_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace stage_ = XR_NULL_HANDLE, view_ = XR_NULL_HANDLE;
    XrActionSet set_ = XR_NULL_HANDLE;
    XrAction trigger_{}, squeeze_{}, stick_{}, pose_{}, btnA_{}, btnB_{}, btnX_{}, btnY_{}, menu_{};
    Hand hands_[2];  // 0 = gauche, 1 = droite
    std::vector<EyeSwapchain> eyes_;
    std::vector<XrViewConfigurationView> viewConfigs_;
    ID3D11Device* dev_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    Renderer rend_;
    bool running_ = false, quit_ = false;
    DXGI_FORMAT colorFormat_ = DXGI_FORMAT_R8G8B8A8_UNORM;

    std::vector<Zone> zones_;
    ZoneType mode_ = ZoneType::Box;
    bool drawing_ = false;
    V3 anchor_;
    float drawYaw_ = 0;
    Zone preview_;
    int grabbed_ = -1;
    V3 grabOffset_;
    int hovered_ = -1;
    int lastCreated_ = -1;
    int counter_ = 1;
    bool prevTrigger_ = false, prevSqueeze_ = false, prevA_ = false, prevB_ = false, prevX_ = false, prevY_ = false;
    std::chrono::steady_clock::time_point lastFrame_ = std::chrono::steady_clock::now();
    Q headRot_;
    V3 headPos_;

    XrPath path(const char* s) {
        XrPath p;
        CHK(xrStringToPath(instance_, s, &p));
        return p;
    }

    void initOpenXR() {
        const char* ext[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
        XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
        std::strcpy(ci.applicationInfo.applicationName, "OverlayVR Editor");
        ci.applicationInfo.applicationVersion = 1;
        std::strcpy(ci.applicationInfo.engineName, "none");
        ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
        ci.enabledExtensionCount = 1;
        ci.enabledExtensionNames = ext;
        CHK(xrCreateInstance(&ci, &instance_));
        XrSystemGetInfo si = {XR_TYPE_SYSTEM_GET_INFO};
        si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        CHK(xrGetSystem(instance_, &si, &system_));
    }

    void initGraphics() {
        PFN_xrGetD3D11GraphicsRequirementsKHR getReq = nullptr;
        CHK(xrGetInstanceProcAddr(instance_, "xrGetD3D11GraphicsRequirementsKHR",
                                  reinterpret_cast<PFN_xrVoidFunction*>(&getReq)));
        XrGraphicsRequirementsD3D11KHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        CHK(getReq(instance_, system_, &req));

        IDXGIFactory1* factory = nullptr;
        CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 d;
            adapter->GetDesc1(&d);
            if (std::memcmp(&d.AdapterLuid, &req.adapterLuid, sizeof(LUID)) == 0) break;
            adapter->Release();
            adapter = nullptr;
        }
        if (!adapter) fail("carte graphique du casque introuvable");
        D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                     2, D3D11_SDK_VERSION, &dev_, nullptr, &ctx_)))
            fail("creation du device D3D11");
        adapter->Release();
        factory->Release();
        rend_.init(dev_, ctx_);
    }

    XrAction makeAction(const char* name, XrActionType type, bool bothHands) {
        XrActionCreateInfo ai = {XR_TYPE_ACTION_CREATE_INFO};
        std::strcpy(ai.actionName, name);
        std::strcpy(ai.localizedActionName, name);
        ai.actionType = type;
        XrPath subs[2] = {hands_[0].path, hands_[1].path};
        if (bothHands) {
            ai.countSubactionPaths = 2;
            ai.subactionPaths = subs;
        }
        XrAction a;
        CHK(xrCreateAction(set_, &ai, &a));
        return a;
    }

    void initSession() {
        XrGraphicsBindingD3D11KHR gb = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        gb.device = dev_;
        XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
        sci.next = &gb;
        sci.systemId = system_;
        CHK(xrCreateSession(instance_, &sci, &session_));

        XrReferenceSpaceCreateInfo rs = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        rs.poseInReferenceSpace.orientation.w = 1;
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
        if (XR_FAILED(xrCreateReferenceSpace(session_, &rs, &stage_))) {
            std::printf("ATTENTION : espace STAGE indisponible, repli sur LOCAL\n");
            rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
            CHK(xrCreateReferenceSpace(session_, &rs, &stage_));
        }
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        CHK(xrCreateReferenceSpace(session_, &rs, &view_));

        // Actions
        XrActionSetCreateInfo asi = {XR_TYPE_ACTION_SET_CREATE_INFO};
        std::strcpy(asi.actionSetName, "editor");
        std::strcpy(asi.localizedActionSetName, "Editeur de zones");
        CHK(xrCreateActionSet(instance_, &asi, &set_));
        hands_[0].path = path("/user/hand/left");
        hands_[1].path = path("/user/hand/right");
        trigger_ = makeAction("trigger", XR_ACTION_TYPE_FLOAT_INPUT, true);
        squeeze_ = makeAction("squeeze", XR_ACTION_TYPE_FLOAT_INPUT, true);
        stick_ = makeAction("stick", XR_ACTION_TYPE_VECTOR2F_INPUT, true);
        pose_ = makeAction("pose", XR_ACTION_TYPE_POSE_INPUT, true);
        btnA_ = makeAction("a", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
        btnB_ = makeAction("b", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
        btnX_ = makeAction("x", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
        btnY_ = makeAction("y", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
        menu_ = makeAction("menu", XR_ACTION_TYPE_BOOLEAN_INPUT, false);

        std::vector<XrActionSuggestedBinding> b;
        auto bind = [&](XrAction a, const char* p) { b.push_back({a, path(p)}); };
        for (const char* side : {"left", "right"}) {
            std::string s = std::string("/user/hand/") + side + "/input/";
            bind(trigger_, (s + "trigger/value").c_str());
            bind(squeeze_, (s + "squeeze/value").c_str());
            bind(stick_, (s + "thumbstick").c_str());
            bind(pose_, (s + "grip/pose").c_str());
        }
        bind(btnA_, "/user/hand/right/input/a/click");
        bind(btnB_, "/user/hand/right/input/b/click");
        bind(btnX_, "/user/hand/left/input/x/click");
        bind(btnY_, "/user/hand/left/input/y/click");
        bind(menu_, "/user/hand/left/input/menu/click");
        XrInteractionProfileSuggestedBinding sb = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
        sb.countSuggestedBindings = static_cast<uint32_t>(b.size());
        sb.suggestedBindings = b.data();
        CHK(xrSuggestInteractionProfileBindings(instance_, &sb));

        for (Hand& h : hands_) {
            XrActionSpaceCreateInfo asc = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
            asc.action = pose_;
            asc.subactionPath = h.path;
            asc.poseInActionSpace.orientation.w = 1;
            CHK(xrCreateActionSpace(session_, &asc, &h.space));
        }
        XrSessionActionSetsAttachInfo att = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        att.countActionSets = 1;
        att.actionSets = &set_;
        CHK(xrAttachSessionActionSets(session_, &att));

        // Swapchains (une par oeil)
        uint32_t n = 0;
        CHK(xrEnumerateViewConfigurationViews(instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr));
        viewConfigs_.assign(n, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
        CHK(xrEnumerateViewConfigurationViews(instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n,
                                              viewConfigs_.data()));
        uint32_t fn = 0;
        xrEnumerateSwapchainFormats(session_, 0, &fn, nullptr);
        std::vector<int64_t> formats(fn);
        xrEnumerateSwapchainFormats(session_, fn, &fn, formats.data());
        // UNORM (pas sRGB) : le magenta du fond doit sortir exactement a (255, 0, 255).
        int64_t chosen = formats.empty() ? 0 : formats[0];
        for (int64_t want : {static_cast<int64_t>(DXGI_FORMAT_R8G8B8A8_UNORM), static_cast<int64_t>(DXGI_FORMAT_B8G8R8A8_UNORM)})
            if (std::find(formats.begin(), formats.end(), want) != formats.end()) {
                chosen = want;
                break;
            }
        colorFormat_ = static_cast<DXGI_FORMAT>(chosen);

        eyes_.resize(n);
        for (uint32_t i = 0; i < n; i++) {
            EyeSwapchain& e = eyes_[i];
            e.width = static_cast<int32_t>(viewConfigs_[i].recommendedImageRectWidth);
            e.height = static_cast<int32_t>(viewConfigs_[i].recommendedImageRectHeight);
            XrSwapchainCreateInfo sc = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
            sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            sc.format = chosen;
            sc.sampleCount = 1;
            sc.width = e.width;
            sc.height = e.height;
            sc.faceCount = 1;
            sc.arraySize = 1;
            sc.mipCount = 1;
            CHK(xrCreateSwapchain(session_, &sc, &e.handle));
            uint32_t cnt = 0;
            CHK(xrEnumerateSwapchainImages(e.handle, 0, &cnt, nullptr));
            e.images.assign(cnt, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
            CHK(xrEnumerateSwapchainImages(e.handle, cnt, &cnt, reinterpret_cast<XrSwapchainImageBaseHeader*>(e.images.data())));
            for (auto& img : e.images) {
                D3D11_RENDER_TARGET_VIEW_DESC rd = {};
                rd.Format = colorFormat_;
                rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                ID3D11RenderTargetView* rtv = nullptr;
                dev_->CreateRenderTargetView(img.texture, &rd, &rtv);
                e.rtvs.push_back(rtv);
            }
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = e.width;
            td.Height = e.height;
            td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_D32_FLOAT;
            td.SampleDesc.Count = 1;
            td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
            dev_->CreateTexture2D(&td, nullptr, &e.depthTex);
            dev_->CreateDepthStencilView(e.depthTex, nullptr, &e.dsv);
        }
    }

    void pollEvents() {
        XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(instance_, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                auto* s = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
                if (s->state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    CHK(xrBeginSession(session_, &bi));
                    running_ = true;
                    std::printf("Session demarree. Mode : %s\n", mode_ == ZoneType::Box ? "VOLUME" : "PLAN");
                } else if (s->state == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(session_);
                    running_ = false;
                } else if (s->state == XR_SESSION_STATE_EXITING || s->state == XR_SESSION_STATE_LOSS_PENDING) {
                    quit_ = true;
                }
            } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                quit_ = true;
            }
            ev = {XR_TYPE_EVENT_DATA_BUFFER};
        }
    }

    // --- entrees -----------------------------------------------------------------------------
    float floatState(XrAction a, int hand) {
        XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        gi.subactionPath = hands_[hand].path;
        XrActionStateFloat s = {XR_TYPE_ACTION_STATE_FLOAT};
        return XR_SUCCEEDED(xrGetActionStateFloat(session_, &gi, &s)) && s.isActive ? s.currentState : 0.0f;
    }
    bool boolState(XrAction a) {
        XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        XrActionStateBoolean s = {XR_TYPE_ACTION_STATE_BOOLEAN};
        return XR_SUCCEEDED(xrGetActionStateBoolean(session_, &gi, &s)) && s.isActive && s.currentState;
    }
    void stickState(int hand, float& x, float& y) {
        XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = stick_;
        gi.subactionPath = hands_[hand].path;
        XrActionStateVector2f st = {XR_TYPE_ACTION_STATE_VECTOR2F};
        x = y = 0;
        if (XR_SUCCEEDED(xrGetActionStateVector2f(session_, &gi, &st)) && st.isActive) {
            x = st.currentState.x;
            y = st.currentState.y;
        }
    }

    void locateHands(XrTime time) {
        constexpr XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        for (Hand& h : hands_) {
            XrSpaceLocation l = {XR_TYPE_SPACE_LOCATION};
            h.tracked = XR_SUCCEEDED(xrLocateSpace(h.space, stage_, time, &l)) && (l.locationFlags & need) == need;
            if (h.tracked) {
                h.pos = {l.pose.position.x, l.pose.position.y, l.pose.position.z};
                h.rot = {l.pose.orientation.x, l.pose.orientation.y, l.pose.orientation.z, l.pose.orientation.w};
            }
        }
        XrSpaceLocation l = {XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(view_, stage_, time, &l)) && (l.locationFlags & need) == need) {
            headPos_ = {l.pose.position.x, l.pose.position.y, l.pose.position.z};
            headRot_ = {l.pose.orientation.x, l.pose.orientation.y, l.pose.orientation.z, l.pose.orientation.w};
        }
    }

    void save() { ovz::saveZones(ovz::zonesPath(), zones_); }

    int nearestZone(V3 p) {
        int best = -1;
        float bestD = 1e9f;
        for (size_t i = 0; i < zones_.size(); i++) {
            if (!insideZone(zones_[i], p, kGrabMargin)) continue;
            float d = ovm::length(p - V3{zones_[i].pos[0], zones_[i].pos[1], zones_[i].pos[2]});
            if (d < bestD) {
                bestD = d;
                best = static_cast<int>(i);
            }
        }
        return best;
    }

    void updateInteraction(float dt) {
        const Hand& rh = hands_[1];
        if (boolState(menu_)) quit_ = true;

        bool trig = floatState(trigger_, 1) > kTriggerOn;
        bool sq = floatState(squeeze_, 1) > kTriggerOn;
        bool a = boolState(btnA_), bb = boolState(btnB_), x = boolState(btnX_), y = boolState(btnY_);

        if (a && !prevA_) {
            mode_ = mode_ == ZoneType::Box ? ZoneType::Plane : ZoneType::Box;
            std::printf("Mode : %s\n", mode_ == ZoneType::Box ? "VOLUME (cyan)" : "PLAN (jaune)");
        }
        if (x && !prevX_ && lastCreated_ >= 0 && lastCreated_ < static_cast<int>(zones_.size())) {
            zones_.erase(zones_.begin() + lastCreated_);
            lastCreated_ = -1;
            grabbed_ = -1;
            save();
            std::printf("Derniere zone annulee\n");
        }

        if (rh.tracked) {
            // Dessin : de l'angle A (appui) a l'angle B (position courante), aligne sur le regard.
            if (trig && !prevTrigger_ && grabbed_ < 0) {
                drawing_ = true;
                anchor_ = rh.pos;
                V3 f = ovm::qrot(headRot_, V3{0, 0, -1});
                drawYaw_ = std::atan2(-f.x, -f.z);
            }
            if (drawing_) {
                V3 d = ovm::qrot(ovm::qaxis('y', -drawYaw_), rh.pos - anchor_);
                preview_ = Zone{};
                preview_.type = mode_;
                preview_.yaw = drawYaw_;
                preview_.size[0] = std::fabs(d.x);
                preview_.size[2] = std::fabs(d.z);
                preview_.size[1] = mode_ == ZoneType::Plane ? 0.0f : std::fabs(d.y);
                V3 localCenter = {d.x / 2, mode_ == ZoneType::Plane ? 0.0f : d.y / 2, d.z / 2};
                V3 c = anchor_ + ovm::qrot(ovm::qaxis('y', drawYaw_), localCenter);
                preview_.pos[0] = c.x;
                preview_.pos[1] = c.y;
                preview_.pos[2] = c.z;
                if (!trig) {
                    drawing_ = false;
                    const bool big = preview_.size[0] > 0.05f && preview_.size[2] > 0.05f &&
                                     (mode_ == ZoneType::Plane || preview_.size[1] > 0.03f);
                    if (big) {
                        preview_.name = (mode_ == ZoneType::Plane ? "plan" : "volume") + std::to_string(counter_++);
                        zones_.push_back(preview_);
                        lastCreated_ = static_cast<int>(zones_.size()) - 1;
                        save();
                        std::printf("Zone '%s' creee (%zu au total)\n", preview_.name.c_str(), zones_.size());
                    }
                }
            }

            // Saisie : deplacer / tourner / redimensionner / supprimer.
            if (!drawing_) {
                if (sq && !prevSqueeze_) {
                    grabbed_ = nearestZone(rh.pos);
                    if (grabbed_ >= 0) {
                        const Zone& z = zones_[grabbed_];
                        grabOffset_ = V3{z.pos[0], z.pos[1], z.pos[2]} - rh.pos;
                    }
                }
                if (grabbed_ >= 0 && grabbed_ < static_cast<int>(zones_.size())) {
                    if (sq) {
                        Zone& z = zones_[grabbed_];
                        V3 p = rh.pos + grabOffset_;
                        z.pos[0] = p.x;
                        z.pos[1] = p.y;
                        z.pos[2] = p.z;
                        float sx, sy;
                        stickState(1, sx, sy);
                        if (std::fabs(sx) > 0.15f) z.yaw += sx * dt * 1.5f;
                        if (std::fabs(sy) > 0.15f) {
                            float k = std::clamp(1.0f + sy * dt * 0.8f, 0.5f, 2.0f);
                            for (float& v : z.size) v = std::clamp(v * k, 0.03f, 4.0f);
                        }
                        if (bb && !prevB_) {
                            zones_.erase(zones_.begin() + grabbed_);
                            grabbed_ = -1;
                            lastCreated_ = -1;
                            save();
                            std::printf("Zone supprimee\n");
                        }
                    } else {
                        save();  // relache : on enregistre
                        grabbed_ = -1;
                    }
                }
                if (y && !prevY_) {
                    int target = grabbed_ >= 0 ? grabbed_ : nearestZone(rh.pos);
                    if (target >= 0) {
                        Zone& z = zones_[target];
                        z.look = z.look > 0 ? 0.0f : kLookDegrees;
                        save();
                        std::printf("Zone '%s' : %s\n", z.name.c_str(),
                                    z.look > 0 ? "visible seulement en regardant vers le bas" : "toujours visible");
                    }
                }
            }
            hovered_ = grabbed_ >= 0 ? grabbed_ : nearestZone(rh.pos);
        }
        prevTrigger_ = trig;
        prevSqueeze_ = sq;
        prevA_ = a;
        prevB_ = bb;
        prevX_ = x;
        prevY_ = y;
    }

    // --- rendu -------------------------------------------------------------------------------
    void drawScene() {
        const float cyan[3] = {0.0f, 0.8f, 1.0f}, yellow[3] = {1.0f, 0.9f, 0.0f};
        const float edgeCyan[4] = {0.3f, 1.0f, 1.0f, 1}, edgeYellow[4] = {1.0f, 1.0f, 0.4f, 1};
        const float hot[4] = {0.2f, 1.0f, 0.2f, 1}, white[4] = {1, 1, 1, 1};
        for (size_t i = 0; i < zones_.size(); i++) {
            const Zone& z = zones_[i];
            bool plane = z.type == ZoneType::Plane;
            const float* edge = static_cast<int>(i) == grabbed_ ? hot : static_cast<int>(i) == hovered_ ? white
                                                               : plane ? edgeYellow : edgeCyan;
            drawZone(rend_, z, plane ? yellow : cyan, edge);
            if (z.look > 0) {  // petit repere blanc au-dessus : affichage conditionnel actif
                const float mark[4] = {1, 1, 1, 0.9f};
                rend_.box(boxWorld({z.pos[0], z.pos[1] + (plane ? 0.05f : z.size[1] / 2 + 0.05f), z.pos[2]}, 0, {0.04f, 0.04f, 0.04f}), mark);
            }
        }
        if (drawing_) {
            const float pc[3] = {0.4f, 1.0f, 0.4f};
            const float pe[4] = {0.4f, 1.0f, 0.4f, 1};
            drawZone(rend_, preview_, pc, pe);
        }
        for (const Hand& h : hands_) {
            if (!h.tracked) continue;
            const float body[4] = {0.5f, 0.5f, 0.5f, 1};
            const float tip[4] = {mode_ == ZoneType::Box ? 0.0f : 1.0f, mode_ == ZoneType::Box ? 0.9f : 0.9f,
                                  mode_ == ZoneType::Box ? 1.0f : 0.0f, 1};
            Mat4 pose = ovm::mFromPose(h.pos, h.rot);
            rend_.box(ovm::mmul(pose, ovm::mmul(ovm::mTranslate({0, 0, 0.05f}), ovm::mScale({0.012f, 0.012f, 0.10f}))), body);
            rend_.box(ovm::mmul(pose, ovm::mScale({0.025f, 0.025f, 0.025f})), tip);
        }
    }

    void frame() {
        XrFrameState fs = {XR_TYPE_FRAME_STATE};
        CHK(xrWaitFrame(session_, nullptr, &fs));
        CHK(xrBeginFrame(session_, nullptr));

        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - lastFrame_).count();
        lastFrame_ = now;

        std::vector<XrCompositionLayerProjectionView> projViews;
        XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        const XrCompositionLayerBaseHeader* layers[1] = {};
        uint32_t layerCount = 0;

        if (fs.shouldRender) {
            XrActiveActionSet active = {set_, XR_NULL_PATH};
            XrActionsSyncInfo sync = {XR_TYPE_ACTIONS_SYNC_INFO};
            sync.countActiveActionSets = 1;
            sync.activeActionSets = &active;
            xrSyncActions(session_, &sync);
            locateHands(fs.predictedDisplayTime);
            updateInteraction(dt);

            XrViewLocateInfo vli = {XR_TYPE_VIEW_LOCATE_INFO};
            vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            vli.displayTime = fs.predictedDisplayTime;
            vli.space = stage_;
            XrViewState vs = {XR_TYPE_VIEW_STATE};
            std::vector<XrView> views(eyes_.size(), {XR_TYPE_VIEW});
            uint32_t vc = 0;
            CHK(xrLocateViews(session_, &vli, &vs, static_cast<uint32_t>(views.size()), &vc, views.data()));

            projViews.assign(vc, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
            for (uint32_t i = 0; i < vc; i++) {
                EyeSwapchain& e = eyes_[i];
                uint32_t idx = 0;
                CHK(xrAcquireSwapchainImage(e.handle, nullptr, &idx));
                XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wi.timeout = XR_INFINITE_DURATION;
                CHK(xrWaitSwapchainImage(e.handle, &wi));

                const XrPosef& pp = views[i].pose;
                const XrFovf& fov = views[i].fov;
                Mat4 view = ovm::mInverseRigid({pp.position.x, pp.position.y, pp.position.z},
                                               {pp.orientation.x, pp.orientation.y, pp.orientation.z, pp.orientation.w});
                rend_.viewProj = ovm::mmul(ovm::mProjection(fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown, 0.05f, 50.0f), view);

                D3D11_VIEWPORT vp = {0, 0, static_cast<float>(e.width), static_cast<float>(e.height), 0, 1};
                ctx_->RSSetViewports(1, &vp);
                ctx_->OMSetRenderTargets(1, &e.rtvs[idx], e.dsv);
                const float magenta[4] = {1, 0, 1, 1};  // fond = passthrough via le chroma key de VD
                ctx_->ClearRenderTargetView(e.rtvs[idx], magenta);
                ctx_->ClearDepthStencilView(e.dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
                rend_.begin();
                drawScene();

                CHK(xrReleaseSwapchainImage(e.handle, nullptr));
                projViews[i].pose = pp;
                projViews[i].fov = fov;
                projViews[i].subImage.swapchain = e.handle;
                projViews[i].subImage.imageRect = {{0, 0}, {e.width, e.height}};
            }
            layer.space = stage_;
            layer.viewCount = vc;
            layer.views = projViews.data();
            layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
            layerCount = 1;
        }

        XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
        ei.displayTime = fs.predictedDisplayTime;
        ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        ei.layerCount = layerCount;
        ei.layers = layers;
        CHK(xrEndFrame(session_, &ei));
    }
};

}  // namespace

int main() {
    try {
        Editor().run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Erreur : %s\n", e.what());
        std::fprintf(stderr, "Virtual Desktop est-il lance, avec VDXR comme runtime OpenXR actif ?\n");
        std::fflush(stderr);
        Sleep(8000);
        return 1;
    }
    return 0;
}

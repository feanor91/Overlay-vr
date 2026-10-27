// Couche API OpenXR implicite : ajoute des quads magenta (255,0,255) a chaque image pour les zones
// de %APPDATA%\OverlayVR\zones.json. Virtual Desktop remplace ce magenta par le passthrough.
// Compatible D3D11 et D3D12 (les quads ont besoin d'une swapchain que la couche remplit en magenta).
#define NOMINMAX
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <windows.h>
#include <unknwn.h>
#include <d3d11.h>
#include <d3d12.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "../common/mathx.h"
#include "../common/config.h"
#include "../common/keys.h"
#include "../common/zones.h"

// --- structures de l'interface loader <-> couche (openxr/loader_interfaces.h) ------------------
enum XrLoaderInterfaceStructs {
    XR_LOADER_INTERFACE_STRUCT_UNINTIALIZED = 0,
    XR_LOADER_INTERFACE_STRUCT_LOADER_INFO = 1,
    XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST = 2,
    XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST = 3,
    XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO = 4,
    XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO = 5,
};
#define XR_CURRENT_LOADER_API_LAYER_VERSION 1
#define XR_API_LAYER_MAX_SETTINGS_PATH_SIZE 512

struct XrApiLayerCreateInfo;
typedef XrResult(XRAPI_PTR* PFN_xrCreateApiLayerInstance)(const XrInstanceCreateInfo* info,
                                                          const XrApiLayerCreateInfo* layerInfo,
                                                          XrInstance* instance);
struct XrNegotiateLoaderInfo {
    XrLoaderInterfaceStructs structType;
    uint32_t structVersion;
    size_t structSize;
    uint32_t minInterfaceVersion;
    uint32_t maxInterfaceVersion;
    XrVersion minApiVersion;
    XrVersion maxApiVersion;
};
struct XrNegotiateApiLayerRequest {
    XrLoaderInterfaceStructs structType;
    uint32_t structVersion;
    size_t structSize;
    uint32_t layerInterfaceVersion;
    XrVersion layerApiVersion;
    PFN_xrGetInstanceProcAddr getInstanceProcAddr;
    PFN_xrCreateApiLayerInstance createApiLayerInstance;
};
struct XrApiLayerNextInfo {
    XrLoaderInterfaceStructs structType;
    uint32_t structVersion;
    size_t structSize;
    char layerName[XR_MAX_API_LAYER_NAME_SIZE];
    PFN_xrGetInstanceProcAddr nextGetInstanceProcAddr;
    PFN_xrCreateApiLayerInstance nextCreateApiLayerInstance;
    XrApiLayerNextInfo* next;
};
struct XrApiLayerCreateInfo {
    XrLoaderInterfaceStructs structType;
    uint32_t structVersion;
    size_t structSize;
    void* loaderInstance;
    char settings_file_location[XR_API_LAYER_MAX_SETTINGS_PATH_SIZE];
    XrApiLayerNextInfo* nextInfo;
};

namespace {

using ovm::Q;
using ovm::V3;

void logf(const char* fmt, ...) {
    auto path = ovz::dataDir() / "layer.log";
    FILE* f = std::fopen(path.string().c_str(), "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fputc('\n', f);
    std::fclose(f);
}

// Fonctions de la couche suivante (autre couche ou runtime).
struct Next {
    PFN_xrGetInstanceProcAddr getProcAddr = nullptr;
    PFN_xrDestroyInstance DestroyInstance = nullptr;
    PFN_xrCreateSession CreateSession = nullptr;
    PFN_xrDestroySession DestroySession = nullptr;
    PFN_xrEndFrame EndFrame = nullptr;
    PFN_xrCreateReferenceSpace CreateReferenceSpace = nullptr;
    PFN_xrDestroySpace DestroySpace = nullptr;
    PFN_xrLocateSpace LocateSpace = nullptr;
    PFN_xrEnumerateSwapchainFormats EnumerateSwapchainFormats = nullptr;
    PFN_xrCreateSwapchain CreateSwapchain = nullptr;
    PFN_xrDestroySwapchain DestroySwapchain = nullptr;
    PFN_xrEnumerateSwapchainImages EnumerateSwapchainImages = nullptr;
    PFN_xrAcquireSwapchainImage AcquireSwapchainImage = nullptr;
    PFN_xrWaitSwapchainImage WaitSwapchainImage = nullptr;
    PFN_xrReleaseSwapchainImage ReleaseSwapchainImage = nullptr;
} nx;

enum class Api { None, D3D11, D3D12 };
constexpr int32_t kSwapchainSize = 16;

struct State {
    bool disabled = false;
    XrInstance instance = XR_NULL_HANDLE;
    XrSession session = XR_NULL_HANDLE;
    Api api = Api::None;
    ID3D11Device* d3d11 = nullptr;
    ID3D12Device* d3d12 = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    XrSpace stage = XR_NULL_HANDLE;
    XrSpace view = XR_NULL_HANDLE;
    XrSwapchain swapchain = XR_NULL_HANDLE;
    int64_t format = 0;
    bool swapchainTried = false;
    bool swapchainReady = false;
    std::vector<ovz::Zone> zones;
    FILETIME zonesStamp = {};
    FILETIME configStamp = {};
    ULONGLONG lastCheck = 0;
    ovz::Config cfg;
    std::string exeName, appName;
    std::vector<ovz::Zone> shown;  // zones chargees + decalage global
    ULONGLONG lastTick = 0, lastKeyChange = 0;
    bool offsetDirty = false;
    int profileIdx = -1;      // profil du jeu courant (-1 : jeu sans profil)
    std::unordered_map<std::string, bool> gazeShown;  // hysteresis : zone actuellement affichee ou non
    bool active = true;  // reglages utilisateur : la couche dessine-t-elle dans ce jeu ?
    bool keyWasDown = false;
    ULONGLONG lastToggle = 0;
    // Bouton de joystick / HOTAS (DirectInput) : peripheriques ouverts et front montant du bouton choisi.
    IDirectInput8W* di = nullptr;
    HWND diWindow = nullptr;
    struct Joy {
        IDirectInputDevice8W* dev;
        std::string name;
        bool wasDown;
    };
    std::vector<Joy> joys;
    ULONGLONG lastEnum = 0;
} g;

// --- reglages et zones (rechargees a chaud) -----------------------------------------------------
// Decalage de recalage du jeu courant : celui de son profil, sinon le decalage commun.
float* contextOffset() { return g.profileIdx >= 0 ? g.cfg.profiles[g.profileIdx].offset : g.cfg.offset; }
float& contextYaw() { return g.profileIdx >= 0 ? g.cfg.profiles[g.profileIdx].offsetYawDeg : g.cfg.offsetYawDeg; }

// Zones a afficher dans ce jeu : on applique d'abord le recalage a toutes (meme pivot quel que soit le jeu), puis on
// garde celles sans jeu et celles du profil courant.
void rebuildShown() {
    std::vector<ovz::Zone> all = g.zones;
    ovz::applyOffset(all, contextOffset(), contextYaw());
    g.shown.clear();
    const std::string& mine = g.profileIdx >= 0 ? g.cfg.profiles[g.profileIdx].name : std::string();
    for (ovz::Zone& z : all)
        if (z.profile.empty() || z.profile == mine) g.shown.push_back(std::move(z));
}

void refreshConfig() {
    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (GetFileAttributesExW(ovz::configPath().c_str(), GetFileExInfoStandard, &attr) &&
        CompareFileTime(&attr.ftLastWriteTime, &g.configStamp) != 0) {
        g.configStamp = attr.ftLastWriteTime;
        ovz::Config c;
        if (ovz::loadConfig(c)) {
            g.cfg = c;
            g.profileIdx = ovz::currentProfile(g.cfg, g.exeName, g.appName);
            rebuildShown();
        }
    }
    bool now = ovz::appAllowed(g.cfg, g.exeName, g.appName);
    if (now != g.active) logf("%s (%s) : couche %s", g.exeName.c_str(), g.appName.c_str(), now ? "ACTIVE" : "inactive");
    g.active = now;
}

void refreshZones() {
    ULONGLONG now = GetTickCount64();
    if (now - g.lastCheck < 500) return;
    g.lastCheck = now;
    refreshConfig();
    if (!g.active) return;
    auto path = ovz::zonesPath();
    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr)) {
        g.zones.clear();
        g.shown.clear();
        g.zonesStamp = {};
        return;
    }
    if (CompareFileTime(&attr.ftLastWriteTime, &g.zonesStamp) == 0) return;
    g.zonesStamp = attr.ftLastWriteTime;
    if (ovz::loadZones(path, g.zones)) {
        rebuildShown();
        logf("%zu zone(s) chargee(s)", g.zones.size());
    }
}

// --- swapchain magenta ----------------------------------------------------------------------
bool clearD3D11(const XrSwapchainImageD3D11KHR& img) {
    ID3D11RenderTargetView* rtv = nullptr;
    D3D11_RENDER_TARGET_VIEW_DESC desc = {};
    desc.Format = static_cast<DXGI_FORMAT>(g.format);
    desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(g.d3d11->CreateRenderTargetView(img.texture, &desc, &rtv))) return false;
    ID3D11DeviceContext* ctx = nullptr;
    g.d3d11->GetImmediateContext(&ctx);
    const float magenta[4] = {1, 0, 1, 1};
    ctx->ClearRenderTargetView(rtv, magenta);
    ctx->Flush();
    ctx->Release();
    rtv->Release();
    return true;
}

bool clearD3D12(const XrSwapchainImageD3D12KHR& img) {
    ID3D12DescriptorHeap* heap = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    bool ok = false;
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 1;
    if (SUCCEEDED(g.d3d12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap))) &&
        SUCCEEDED(g.d3d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
        SUCCEEDED(g.d3d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
                                             IID_PPV_ARGS(&list))) &&
        SUCCEEDED(g.d3d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        D3D12_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = static_cast<DXGI_FORMAT>(g.format);
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
        g.d3d12->CreateRenderTargetView(img.texture, &rd, h);
        const float magenta[4] = {1, 0, 1, 1};
        list->ClearRenderTargetView(h, magenta, 0, nullptr);  // l'image est deja en RENDER_TARGET
        list->Close();
        ID3D12CommandList* lists[] = {list};
        g.queue->ExecuteCommandLists(1, lists);
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (ev && SUCCEEDED(g.queue->Signal(fence, 1)) && SUCCEEDED(fence->SetEventOnCompletion(1, ev))) {
            WaitForSingleObject(ev, 2000);
            ok = true;
        }
        if (ev) CloseHandle(ev);
    }
    if (fence) fence->Release();
    if (list) list->Release();
    if (alloc) alloc->Release();
    if (heap) heap->Release();
    return ok;
}

bool createMagentaSwapchain() {
    uint32_t n = 0;
    nx.EnumerateSwapchainFormats(g.session, 0, &n, nullptr);
    std::vector<int64_t> formats(n);
    nx.EnumerateSwapchainFormats(g.session, n, &n, formats.data());
    // UNORM d'abord : le magenta sort exactement a (255,0,255) ; le sRGB convient aussi pour 0 et 1.
    const int64_t preferred[] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM,
                                 DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
    g.format = 0;
    for (int64_t p : preferred)
        if (std::find(formats.begin(), formats.end(), p) != formats.end()) {
            g.format = p;
            break;
        }
    if (!g.format) return false;

    XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = g.format;
    ci.sampleCount = 1;
    ci.width = ci.height = kSwapchainSize;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    if (XR_FAILED(nx.CreateSwapchain(g.session, &ci, &g.swapchain))) return false;

    uint32_t count = 0;
    nx.EnumerateSwapchainImages(g.swapchain, 0, &count, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> img11;
    std::vector<XrSwapchainImageD3D12KHR> img12;
    if (g.api == Api::D3D11) {
        img11.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        nx.EnumerateSwapchainImages(g.swapchain, count, &count,
                                    reinterpret_cast<XrSwapchainImageBaseHeader*>(img11.data()));
    } else {
        img12.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        nx.EnumerateSwapchainImages(g.swapchain, count, &count,
                                    reinterpret_cast<XrSwapchainImageBaseHeader*>(img12.data()));
    }

    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(nx.AcquireSwapchainImage(g.swapchain, &ai, &index))) return false;
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(nx.WaitSwapchainImage(g.swapchain, &wi))) return false;
    bool ok = g.api == Api::D3D11 ? clearD3D11(img11[index]) : clearD3D12(img12[index]);
    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    nx.ReleaseSwapchainImage(g.swapchain, &ri);
    return ok;
}

bool ensureSwapchain() {
    if (g.swapchainReady) return true;
    if (g.swapchainTried) return false;
    g.swapchainTried = true;
    g.swapchainReady = createMagentaSwapchain();
    logf("swapchain magenta : %s", g.swapchainReady ? "ok" : "echec");
    return g.swapchainReady;
}

// --- generation des quads ---------------------------------------------------------------------
struct Face {
    Q rot;
    V3 off;
    float w, h;
};

XrPosef toXr(V3 p, Q q) {
    XrPosef x;
    x.position = {p.x, p.y, p.z};
    x.orientation = {q.x, q.y, q.z, q.w};
    return x;
}

void addZoneQuads(const ovz::Zone& z, bool haveHead, V3 head, std::vector<XrCompositionLayerQuad>& out) {
    const float hp = ovm::kPi / 2;
    const float grow = g.cfg.overlapCm / 100.0f;  // recouvrement : supprime les lignes aux raccords des faces
    const float w = z.size[0], h = z.size[1], d = z.size[2];
    std::vector<Face> faces;
    if (z.type == ovz::ZoneType::Plane) {
        faces.push_back({ovm::qaxis('x', -hp), {0, 0, 0}, w, d});  // face vers le haut
        faces.push_back({ovm::qaxis('x', hp), {0, 0, 0}, w, d});   // face vers le bas
    } else {
        faces.push_back({Q{}, {0, 0, d / 2}, w, h});
        faces.push_back({ovm::qaxis('y', 2 * hp), {0, 0, -d / 2}, w, h});
        faces.push_back({ovm::qaxis('y', hp), {w / 2, 0, 0}, d, h});
        faces.push_back({ovm::qaxis('y', -hp), {-w / 2, 0, 0}, d, h});
        faces.push_back({ovm::qaxis('x', -hp), {0, h / 2, 0}, w, d});
        faces.push_back({ovm::qaxis('x', hp), {0, -h / 2, 0}, w, d});
    }
    Q yaw = ovm::qaxis('y', z.yaw);
    V3 center = {z.pos[0], z.pos[1], z.pos[2]};
    for (const Face& f : faces) {
        Q q = ovm::qmul(yaw, f.rot);
        V3 pos = center + ovm::qrot(yaw, f.off);
        // On ne garde que les faces tournees vers la tete : 3 quads par volume au plus.
        if (haveHead && ovm::dot(ovm::qrot(q, V3{0, 0, 1}), head - pos) <= 0) continue;
        XrCompositionLayerQuad quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        quad.layerFlags = 0;
        quad.space = g.stage;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = g.swapchain;
        quad.subImage.imageRect = {{0, 0}, {kSwapchainSize, kSwapchainSize}};
        quad.subImage.imageArrayIndex = 0;
        quad.pose = toXr(pos, q);
        quad.size = {std::max(f.w + grow, 0.001f), std::max(f.h + grow, 0.001f)};
        out.push_back(quad);
    }
}

// --- fonctions interceptees -------------------------------------------------------------------
XrResult XRAPI_CALL ovCreateSession(XrInstance instance, const XrSessionCreateInfo* info, XrSession* session) {
    XrResult res = nx.CreateSession(instance, info, session);
    if (XR_FAILED(res) || g.disabled) return res;

    g.api = Api::None;
    for (auto p = static_cast<const XrBaseInStructure*>(info->next); p; p = p->next) {
        if (p->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
            g.d3d11 = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(p)->device;
            g.api = Api::D3D11;
        } else if (p->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
            auto b = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(p);
            g.d3d12 = b->device;
            g.queue = b->queue;
            g.api = Api::D3D12;
        }
    }
    if (g.api == Api::None) {
        logf("API graphique non prise en charge (ni D3D11 ni D3D12) : couche inactive");
        return res;
    }

    g.session = *session;
    g.swapchain = XR_NULL_HANDLE;
    g.swapchainTried = g.swapchainReady = false;

    XrReferenceSpaceCreateInfo rs = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace.orientation.w = 1;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    if (XR_FAILED(nx.CreateReferenceSpace(g.session, &rs, &g.stage))) {
        logf("espace STAGE indisponible, repli sur LOCAL (les zones peuvent etre decalees)");
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        nx.CreateReferenceSpace(g.session, &rs, &g.stage);
    }
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    nx.CreateReferenceSpace(g.session, &rs, &g.view);
    logf("session creee (%s)", g.api == Api::D3D11 ? "D3D11" : "D3D12");
    return res;
}

XrResult XRAPI_CALL ovDestroySession(XrSession session) {
    if (session == g.session) {
        if (g.swapchain) nx.DestroySwapchain(g.swapchain);
        if (g.stage) nx.DestroySpace(g.stage);
        if (g.view) nx.DestroySpace(g.view);
        g.swapchain = XR_NULL_HANDLE;
        g.stage = XR_NULL_HANDLE;
        g.view = XR_NULL_HANDLE;
        g.session = XR_NULL_HANDLE;
        g.api = Api::None;
    }
    return nx.DestroySession(session);
}

// Vrai si le regard (axe de la tete, centre de la vue) pointe vers la zone : un rayon part de la tete, et la zone, grossie
// d'une marge angulaire, doit le croiser. Etre simplement dans le champ de vision ne suffit pas.
// Marge supplementaire de 4 degres quand la zone est deja affichee, pour eviter le clignotement.
bool inGaze(const ovz::Zone& z, V3 head, V3 fwd) {
    const bool shown = g.gazeShown[z.name];
    const float margin = (g.cfg.gazeMargin + (shown ? 4.0f : 0.0f)) * ovm::kPi / 180.0f;
    const V3 c = {z.pos[0], z.pos[1], z.pos[2]};
    const Q inv = ovm::qaxis('y', -z.yaw);
    const V3 o = ovm::qrot(inv, head - c);  // origine et direction du rayon dans le repere de la zone
    const V3 d = ovm::qrot(inv, fwd);
    const float pad = std::max(0.02f, ovm::length(head - c) * std::tan(margin));  // marge angulaire, en metres, au niveau de la zone
    const float org[3] = {o.x, o.y, o.z}, dir[3] = {d.x, d.y, d.z};
    const float half[3] = {z.size[0] / 2 + pad, (z.type == ovz::ZoneType::Plane ? 0.0f : z.size[1] / 2) + pad, z.size[2] / 2 + pad};
    float t0 = 0, t1 = 1e9f;
    bool hit = true;
    for (int k = 0; k < 3 && hit; k++) {
        if (std::fabs(dir[k]) < 1e-6f) {
            if (std::fabs(org[k]) > half[k]) hit = false;
        } else {
            float ta = (-half[k] - org[k]) / dir[k], tb = (half[k] - org[k]) / dir[k];
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
            if (t0 > t1) hit = false;
        }
    }
    g.gazeShown[z.name] = hit;
    return hit;
}

// Recalage en jeu, au clavier : Ctrl+Alt + fleches (avancer/reculer/gauche/droite selon la direction de la tete),
// PageHaut/PageBas (hauteur), Inser/Suppr (rotation), Origine (remise a zero). Maj = pas plus grands.
// Le decalage est enregistre dans settings.json une fois les touches relachees.
void updateHotkeys(V3 fwd) {
    const ULONGLONG now = GetTickCount64();
    const float dt = g.lastTick ? std::min(0.1f, (now - g.lastTick) / 1000.0f) : 0.0f;
    g.lastTick = now;
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    bool changed = false;
    if (down(VK_CONTROL) && down(VK_MENU)) {
        const float move = (down(VK_SHIFT) ? 0.15f : 0.03f) * dt, turn = (down(VK_SHIFT) ? 15.0f : 3.0f) * dt;
        V3 f = {fwd.x, 0, fwd.z};
        const float n = ovm::length(f);
        f = n > 1e-3f ? f * (1.0f / n) : V3{0, 0, -1};
        const V3 right = {-f.z, 0, f.x};
        V3 d;
        if (down(VK_UP)) d = d + f * move;
        if (down(VK_DOWN)) d = d - f * move;
        if (down(VK_RIGHT)) d = d + right * move;
        if (down(VK_LEFT)) d = d - right * move;
        if (down(VK_PRIOR)) d.y += move;
        if (down(VK_NEXT)) d.y -= move;
        float dyaw = 0;
        if (down(VK_DELETE)) dyaw += turn;
        if (down(VK_INSERT)) dyaw -= turn;
        if (down(VK_HOME)) {
            float* o = contextOffset();
            o[0] = o[1] = o[2] = 0;
            contextYaw() = 0;
            changed = true;
        }
        if (d.x != 0 || d.y != 0 || d.z != 0 || dyaw != 0) {
            float* o = contextOffset();
            o[0] += d.x;
            o[1] += d.y;
            o[2] += d.z;
            contextYaw() += dyaw;
            changed = true;
        }
    }
    if (changed) {
        rebuildShown();
        g.offsetDirty = true;
        g.lastKeyChange = now;
    } else if (g.offsetDirty && now - g.lastKeyChange > 800) {  // touches relachees : on enregistre
        ovz::saveConfig(g.cfg);
        g.offsetDirty = false;
        logf("decalage enregistre : %.3f %.3f %.3f m, %.1f deg", contextOffset()[0], contextOffset()[1], contextOffset()[2], contextYaw());
    }
}

// --- bascule des zones (touche clavier / bouton manette) ---------------------------------------
// Inverse l'interrupteur general (le meme que le menu de la barre d'etat). Sans effet dans un jeu que la couche ignore.
void toggleZones(const char* source) {
    const ULONGLONG now = GetTickCount64();
    if (now - g.lastToggle < 300) return;
    ovz::Config probe = g.cfg;
    probe.enabled = true;
    if (!ovz::appAllowed(probe, g.exeName, g.appName)) return;
    g.lastToggle = now;
    ovz::Config c;
    if (ovz::loadConfig(c)) g.cfg = c;  // la barre d'etat a pu le changer entre-temps
    g.cfg.enabled = !g.cfg.enabled;
    ovz::saveConfig(g.cfg);
    g.active = ovz::appAllowed(g.cfg, g.exeName, g.appName);
    logf("zones %s (%s)", g.cfg.enabled ? "ACTIVEES" : "desactivees", source);
}

void pollToggleKey() {
    static std::string cached;
    static ovz::KeySpec spec;
    if (cached != g.cfg.toggleKey) {
        cached = g.cfg.toggleKey;
        spec = ovz::parseKey(cached);
    }
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    const bool pressed = spec.vk && down(spec.vk) && (!spec.ctrl || down(VK_CONTROL)) && (!spec.alt || down(VK_MENU)) &&
                         (!spec.shift || down(VK_SHIFT));
    if (pressed && !g.keyWasDown) toggleZones("clavier");
    g.keyWasDown = pressed;
}

BOOL CALLBACK enumJoystick(const DIDEVICEINSTANCEW* inst, void*) {
    IDirectInputDevice8W* dev = nullptr;
    if (FAILED(g.di->CreateDevice(inst->guidInstance, &dev, nullptr))) return DIENUM_CONTINUE;
    if (FAILED(dev->SetDataFormat(&c_dfDIJoystick2)) ||
        FAILED(dev->SetCooperativeLevel(g.diWindow, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE))) {
        dev->Release();
        return DIENUM_CONTINUE;
    }
    char name[260] = {};
    WideCharToMultiByte(CP_UTF8, 0, inst->tszProductName, -1, name, sizeof name - 1, nullptr, nullptr);
    g.joys.push_back({dev, name, false});
    logf("joystick detecte : %s", name);
    return DIENUM_CONTINUE;
}

void closeJoysticks() {
    for (auto& j : g.joys) {
        j.dev->Unacquire();
        j.dev->Release();
    }
    g.joys.clear();
}

// Bouton d'un joystick / HOTAS (numerotation de joy.cpl, a partir de 1). Lecture en arriere-plan, sans exclusivite :
// le jeu garde ses propres assignations. "toggle_joystick" : morceau du nom du peripherique (vide : n'importe lequel).
void pollToggleJoystick() {
    const int button = g.cfg.toggleJoystickButton;
    if (button < 1 || button > 128) return;
    const ULONGLONG now = GetTickCount64();
    if (!g.di) {
        if (g.lastEnum && now - g.lastEnum < 10000) return;
        if (FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                                      reinterpret_cast<void**>(&g.di), nullptr))) {
            g.lastEnum = now;
            return;
        }
        g.diWindow = CreateWindowExW(0, L"STATIC", L"OverlayVR", 0, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr);
        g.lastEnum = 0;
    }
    if (g.joys.empty()) {
        if (g.lastEnum && now - g.lastEnum < 10000) return;
        g.lastEnum = now;
        g.di->EnumDevices(DI8DEVCLASS_GAMECTRL, enumJoystick, nullptr, DIEDFL_ATTACHEDONLY);
        if (g.joys.empty()) return;
    }
    const std::string wanted = ovz::lowered(g.cfg.toggleJoystick);
    bool lost = false;
    for (auto& j : g.joys) {
        if (!wanted.empty() && ovz::lowered(j.name).find(wanted) == std::string::npos) continue;
        DIJOYSTATE2 st = {};
        j.dev->Acquire();
        j.dev->Poll();
        if (FAILED(j.dev->GetDeviceState(sizeof st, &st))) {
            lost = true;
            continue;
        }
        const bool down = (st.rgbButtons[button - 1] & 0x80) != 0;
        if (down && !j.wasDown) toggleZones("joystick");
        j.wasDown = down;
    }
    if (lost) {  // peripherique debranche : on relistera dans quelques secondes
        closeJoysticks();
        g.lastEnum = now;
    }
}

XrResult XRAPI_CALL ovEndFrame(XrSession session, const XrFrameEndInfo* info) {
    if (g.disabled || session != g.session || g.api == Api::None) return nx.EndFrame(session, info);

    pollToggleKey();
    pollToggleJoystick();
    refreshZones();
    if (!g.active || g.shown.empty() || !ensureSwapchain()) return nx.EndFrame(session, info);

    // Pose de la tete dans le repere STAGE, pour l'affichage conditionnel et le choix des faces.
    XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
    constexpr XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    bool haveHead = XR_SUCCEEDED(nx.LocateSpace(g.view, g.stage, info->displayTime, &loc)) &&
                    (loc.locationFlags & need) == need;
    V3 head = {loc.pose.position.x, loc.pose.position.y, loc.pose.position.z};
    float pitchDown = 0;
    V3 fwd = {0, 0, -1};
    if (haveHead) {
        Q q = {loc.pose.orientation.x, loc.pose.orientation.y, loc.pose.orientation.z, loc.pose.orientation.w};
        fwd = ovm::qrot(q, V3{0, 0, -1});
        float fy = fwd.y;
        pitchDown = std::asin(std::clamp(-fy, -1.0f, 1.0f)) * 180.0f / ovm::kPi;
    }

    if (g.cfg.hotkeys && haveHead) updateHotkeys(fwd);

    std::vector<XrCompositionLayerQuad> quads;
    quads.reserve(g.shown.size() * 3 + 8);
    for (const ovz::Zone& z : g.shown) {
        if (!z.enabled) continue;
        if (z.look > 0 && haveHead && pitchDown < z.look) continue;
        if (g.cfg.gazeOnly && haveHead && !inGaze(z, head, fwd)) continue;
        addZoneQuads(z, haveHead, head, quads);
    }
    if (quads.empty()) return nx.EndFrame(session, info);

    // Chaque face est superposee plusieurs fois : un pixel de bord couvert a moitie passe a 75 %, 87 %... de magenta,
    // et Virtual Desktop l'efface avec le reste au lieu de laisser un liseré rose. Plafond de couches : chaque couche pese sur le GPU.
    const size_t base = quads.size();
    const size_t passes = std::clamp<size_t>(24 / base, 1, static_cast<size_t>(g.cfg.edgePasses));
    quads.reserve(base * passes);
    for (size_t p = 1; p < passes; p++)
        for (size_t i = 0; i < base; i++) quads.push_back(quads[i]);

    std::vector<const XrCompositionLayerBaseHeader*> layers(info->layers, info->layers + info->layerCount);
    for (auto& q : quads) layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q));
    XrFrameEndInfo modified = *info;
    modified.layerCount = static_cast<uint32_t>(layers.size());
    modified.layers = layers.data();
    return nx.EndFrame(session, &modified);
}

XrResult XRAPI_CALL ovDestroyInstance(XrInstance instance) {
    XrResult r = nx.DestroyInstance(instance);
    g.instance = XR_NULL_HANDLE;
    return r;
}

XrResult XRAPI_CALL ovGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    if (!nx.getProcAddr) return XR_ERROR_HANDLE_INVALID;
    if (!g.disabled) {
        if (!std::strcmp(name, "xrCreateSession")) {
            *function = reinterpret_cast<PFN_xrVoidFunction>(ovCreateSession);
            return XR_SUCCESS;
        }
        if (!std::strcmp(name, "xrDestroySession")) {
            *function = reinterpret_cast<PFN_xrVoidFunction>(ovDestroySession);
            return XR_SUCCESS;
        }
        if (!std::strcmp(name, "xrEndFrame")) {
            *function = reinterpret_cast<PFN_xrVoidFunction>(ovEndFrame);
            return XR_SUCCESS;
        }
    }
    if (!std::strcmp(name, "xrDestroyInstance")) {
        *function = reinterpret_cast<PFN_xrVoidFunction>(ovDestroyInstance);
        return XR_SUCCESS;
    }
    return nx.getProcAddr(instance, name, function);
}

template <typename T>
void fetch(XrInstance instance, const char* name, T& fn) {
    nx.getProcAddr(instance, name, reinterpret_cast<PFN_xrVoidFunction*>(&fn));
}

XrResult XRAPI_CALL ovCreateApiLayerInstance(const XrInstanceCreateInfo* info, const XrApiLayerCreateInfo* layerInfo,
                                             XrInstance* instance) {
    if (!layerInfo || !layerInfo->nextInfo) return XR_ERROR_INITIALIZATION_FAILED;
    XrApiLayerCreateInfo next = *layerInfo;
    next.nextInfo = layerInfo->nextInfo->next;
    XrResult res = layerInfo->nextInfo->nextCreateApiLayerInstance(info, &next, instance);
    if (XR_FAILED(res)) return res;

    nx.getProcAddr = layerInfo->nextInfo->nextGetInstanceProcAddr;
    g.instance = *instance;
    // L'editeur positionne cette variable pour ne pas se dessiner deux fois.
    g.disabled = std::getenv("OVERLAYVR_DISABLE") != nullptr;

    fetch(*instance, "xrDestroyInstance", nx.DestroyInstance);
    fetch(*instance, "xrCreateSession", nx.CreateSession);
    fetch(*instance, "xrDestroySession", nx.DestroySession);
    fetch(*instance, "xrEndFrame", nx.EndFrame);
    fetch(*instance, "xrCreateReferenceSpace", nx.CreateReferenceSpace);
    fetch(*instance, "xrDestroySpace", nx.DestroySpace);
    fetch(*instance, "xrLocateSpace", nx.LocateSpace);
    fetch(*instance, "xrEnumerateSwapchainFormats", nx.EnumerateSwapchainFormats);
    fetch(*instance, "xrCreateSwapchain", nx.CreateSwapchain);
    fetch(*instance, "xrDestroySwapchain", nx.DestroySwapchain);
    fetch(*instance, "xrEnumerateSwapchainImages", nx.EnumerateSwapchainImages);
    fetch(*instance, "xrAcquireSwapchainImage", nx.AcquireSwapchainImage);
    fetch(*instance, "xrWaitSwapchainImage", nx.WaitSwapchainImage);
    fetch(*instance, "xrReleaseSwapchainImage", nx.ReleaseSwapchainImage);

    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    const char* slash = std::strrchr(path, '\\');
    g.exeName = slash ? slash + 1 : path;
    g.appName = info->applicationInfo.applicationName;
    ovz::ensureConfigFile();
    refreshConfig();
    logf("couche chargee dans %s (application OpenXR '%s') : %s%s, profil : %s", g.exeName.c_str(), g.appName.c_str(),
         g.active ? "ACTIVE" : "inactive (voir settings.json)", g.disabled ? " [OVERLAYVR_DISABLE]" : "",
         g.profileIdx >= 0 ? g.cfg.profiles[g.profileIdx].name.c_str() : "aucun (zones sans jeu seulement)");
    return XR_SUCCESS;
}

}  // namespace

extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo* loaderInfo, const char* /*layerName*/, XrNegotiateApiLayerRequest* request) {
    if (!loaderInfo || !request || loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST)
        return XR_ERROR_INITIALIZATION_FAILED;
    if (loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION)
        return XR_ERROR_INITIALIZATION_FAILED;
    request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    request->layerApiVersion = XR_CURRENT_API_VERSION;
    request->getInstanceProcAddr = ovGetInstanceProcAddr;
    request->createApiLayerInstance = ovCreateApiLayerInstance;
    return XR_SUCCESS;
}

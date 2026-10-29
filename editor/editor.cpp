// Editeur OverlayVR : application OpenXR (D3D11) a lancer dans le casque avant un jeu.
// Le fond est magenta (Virtual Desktop le remplace par le passthrough), on voit donc la piece
// et on dessine les zones avec les manettes. Tout est enregistre dans zones.json.
// Les commandes sont affichees dans le casque (panneau d'aide, masquable au clic du stick gauche).
//
// Principe : chaque manette projette un laser ; la zone visee est celle sur laquelle les boutons agissent.
//   Droite : gachette = tracer (1er bord, puis largeur, puis hauteur) | grip = deplacer/orienter | B = supprimer
//            A = volume/plan | clic stick = hauteur du bureau
//   Gauche : X = annuler (maintenu 2 s : tout effacer) | Y = affichage conditionnel | stick = objet actif
//            gachette = rattacher a l'objet | grip = deplacer l'objet entier | clic stick = aide | menu = quitter
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

#include "../common/config.h"
#include "../common/mathx.h"
#include "../common/zones.h"
#include "render.h"

using edr::boxWorld;
using edr::drawZone;
using edr::Renderer;
using edr::TextPanel;
using ovm::Mat4;
using ovm::Q;
using ovm::V3;
using ovz::Zone;
using ovz::ZoneType;

namespace {

constexpr float kLookDegrees = 25.0f;
constexpr float kTriggerOn = 0.6f;
constexpr float kPickMargin = 0.04f;  // tolerance de visee autour d'une zone (surtout pour les plans)

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Cap (rotation autour de Y) d'une pose : direction de visee -Z projetee sur le plan horizontal.
float headingOf(ovm::Q q) {
    ovm::V3 f = ovm::qrot(q, ovm::V3{0, 0, -1});
    return std::atan2(-f.x, -f.z);
}

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

// --- application OpenXR ---------------------------------------------------------------------
struct Hand {
    XrPath path = 0;
    XrSpace space = XR_NULL_HANDLE;
    bool tracked = false;
    V3 pos;
    Q rot;
    XrSpace aimSpace = XR_NULL_HANDLE;  // pose de visee (laser)
    bool aimTracked = false;
    V3 aimPos;
    Q aimRot;
};

struct EyeSwapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    int32_t width = 0, height = 0;
    std::vector<XrSwapchainImageD3D11KHR> images;
    std::vector<ID3D11RenderTargetView*> rtvs;
    ID3D11Texture2D* depthTex = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    // Profondeur transmise au runtime (XR_KHR_composition_layer_depth) : permet un recalage plus juste quand la tete se deplace.
    XrSwapchain depthHandle = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> depthImages;
    std::vector<ID3D11DepthStencilView*> depthDsvs;
};

class Editor {
public:
    void run() {
        SetEnvironmentVariableA("OVERLAYVR_DISABLE", "1");  // la couche globale ne doit pas s'ajouter ici
        ovz::loadZones(ovz::zonesPath(), zones_);
        loadSettings();
        ovz::ensureConfigFile();
        loadProfiles();
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
    XrAction trigger_{}, squeeze_{}, stick_{}, pose_{}, btnA_{}, btnB_{}, btnX_{}, btnY_{}, menu_{}, thumbClick_{}, aim_{}, thumbClickR_{};
    Hand hands_[2];  // 0 = gauche, 1 = droite
    std::vector<EyeSwapchain> eyes_;
    std::vector<XrViewConfigurationView> viewConfigs_;
    ID3D11Device* dev_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    Renderer rend_;
    bool running_ = false, quit_ = false;
    bool depthExt_ = false;      // extension de profondeur disponible
    // Cadence : le runtime impose 30 Hz a l'editeur ; on demande la plus haute (XR_FB_display_refresh_rate) si possible.
    bool refreshExt_ = false;
    float refreshWanted_ = 0;  // editor.json : "refresh_hz" (0 : la plus haute proposee)
    float scale_ = 1.0f;       // editor.json : "scale" (resolution de rendu relative a la recommandee)
    PFN_xrEnumerateDisplayRefreshRatesFB enumRates_ = nullptr;
    PFN_xrGetDisplayRefreshRateFB getRate_ = nullptr;
    PFN_xrRequestDisplayRefreshRateFB requestRate_ = nullptr;
    bool depthWanted_ = true;    // editor.json : "depth": false pour la couper
    DXGI_FORMAT depthFormat_ = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT colorFormat_ = DXGI_FORMAT_R8G8B8A8_UNORM;

    std::vector<Zone> zones_;
    std::vector<std::vector<Zone>> history_;  // instantanes pour « annuler »
    ZoneType mode_ = ZoneType::Box;

    // Tracé en 3 temps : 1er bord (A -> B, donne l'orientation), puis largeur du cote oppose, puis hauteur (volume).
    enum class Phase { Idle, Edge, Depth, Points, Top, Bottom };
    Phase phase_ = Phase::Idle;
    V3 anchor_, corner_;
    float drawYaw_ = 0, dz_ = 0.02f, heightAdj_ = 0, topY_ = 0, bottomRefY_ = 0;
    bool bottomActive_ = false;
    std::vector<V3> polyPts_;  // sommets de la forme libre en cours de trace (sur le plan de visee)
    Zone preview_;
    float surfaceY_ = 0.75f;  // hauteur du plan de visee (bureau), reglable au clic du stick droit
    V3 hit_;                  // point vise sur ce plan (main droite)
    bool hasHit_ = false;
    int counter_ = 1;
    // Nommage d'une zone qui vient d'etre tracee : on tape le nom au clavier, Entree valide, Echap garde le nom par defaut.
    int namingIdx_ = -1;
    std::string namingOrig_;
    std::wstring namingBuf_;
    bool namingTyped_ = false;
    ULONGLONG namingStart_ = 0;
    bool keyPrev_[256] = {};
    std::vector<std::string> profileNames_ = {std::string()};
    int curProfile_ = 0, prevFlickY_ = 0;

    // Lasers : une visee par main (0 = gauche, 1 = droite).
    V3 rayOrigin_[2], rayDir_[2];
    bool rayValid_[2] = {false, false};
    int hover_[2] = {-1, -1};
    float hoverT_[2] = {0, 0};

    struct Edge {
        bool now = false, was = false;
        void set(bool v) {
            was = now;
            now = v;
        }
        bool down() const { return now && !was; }
        bool up() const { return !now && was; }
    };
    struct Inputs {
        Edge trigR, gripR, a, b, thumbR, trigL, gripL, x, y, thumbL;
        float stickLy = 0;
    } in_;
    float xHold_ = 0;
    bool xCleared_ = false;

    // Saisie rigide : une zone (main droite) ou tout un objet (main gauche) suit la main, poignet compris.
    struct Saved {
        size_t index;
        V3 pos;
        float yaw;
        float size[3];
        std::vector<std::array<float, 2>> pts;
    };
    struct Grab {
        bool active = false;
        int hand = 1;
        V3 hand0;
        float heading0 = 0, extraYaw = 0, scale = 1, heightScale = 1;
        V3 nudge;  // reglage fin au stick droit pendant la saisie a la main gauche
        std::vector<Saved> items;
    } grab_;

    bool showHelp_ = true;
    V3 hudPos_;
    float hudYaw_ = 0;
    bool hudPlaced_ = false, headValid_ = false, surfaceDirty_ = false;
    TextPanel help_, status_, namePanel_;  // namePanel_ : invite de nom, devant toi, juste apres le trace d'une zone
    std::chrono::steady_clock::time_point lastFrame_ = std::chrono::steady_clock::now();
    Q headRot_;
    V3 headPos_;

    float lastDt_ = 0.016f;
    // Mesure de performance : images par seconde, temps de calcul de l'editeur (CPU) par image, pire image.
    float perfAcc_ = 0, perfWork_ = 0, perfWorst_ = 0, perfFps_ = 0, perfAvgMs_ = 0, perfWorstMs_ = 0;
    int perfFrames_ = 0, perfReports_ = 0;
    float perfWaitFrame_ = 0, perfWaitImage_ = 0, perfEnd_ = 0;  // temps cumules d'attente (s) : xrWaitFrame, xrWaitSwapchainImage, xrEndFrame
    float periodMs_ = 0;  // periode d'affichage annoncee par le runtime (xrWaitFrame) : 11 ms = 90 Hz, 33 ms = 30 Hz

    XrPath path(const char* s) {
        XrPath p;
        CHK(xrStringToPath(instance_, s, &p));
        return p;
    }

    void initOpenXR() {
        std::vector<const char*> ext = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
        uint32_t extCount = 0;
        xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
        std::vector<XrExtensionProperties> props(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
        xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, props.data());
        for (const auto& p : props)
            if (std::strcmp(p.extensionName, XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME) == 0) depthExt_ = true;
        for (const auto& p : props)
            if (std::strcmp(p.extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME) == 0) refreshExt_ = true;
        if (refreshExt_) ext.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
        if (depthExt_ && depthWanted_) ext.push_back(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
        else depthExt_ = false;
        XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
        std::strcpy(ci.applicationInfo.applicationName, "OverlayVR Editor");
        ci.applicationInfo.applicationVersion = 1;
        std::strcpy(ci.applicationInfo.engineName, "none");
        ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
        ci.enabledExtensionCount = static_cast<uint32_t>(ext.size());
        ci.enabledExtensionNames = ext.data();
        CHK(xrCreateInstance(&ci, &instance_));
        XrSystemGetInfo si = {XR_TYPE_SYSTEM_GET_INFO};
        si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        CHK(xrGetSystem(instance_, &si, &system_));
        XrInstanceProperties ip = {XR_TYPE_INSTANCE_PROPERTIES};
        if (XR_SUCCEEDED(xrGetInstanceProperties(instance_, &ip)))
            std::printf("Runtime OpenXR : %s (version %u.%u.%u)\n", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
                        XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));
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
        help_.init(dev_, ctx_, 820, 500);
        status_.init(dev_, ctx_, 820, 130);
        namePanel_.init(dev_, ctx_, 820, 140);
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

        if (refreshExt_) {
            xrGetInstanceProcAddr(instance_, "xrEnumerateDisplayRefreshRatesFB", reinterpret_cast<PFN_xrVoidFunction*>(&enumRates_));
            xrGetInstanceProcAddr(instance_, "xrGetDisplayRefreshRateFB", reinterpret_cast<PFN_xrVoidFunction*>(&getRate_));
            xrGetInstanceProcAddr(instance_, "xrRequestDisplayRefreshRateFB", reinterpret_cast<PFN_xrVoidFunction*>(&requestRate_));
        }

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
        thumbClick_ = makeAction("thumbclick", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
        thumbClickR_ = makeAction("thumbclickr", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
        aim_ = makeAction("aim", XR_ACTION_TYPE_POSE_INPUT, true);

        std::vector<XrActionSuggestedBinding> b;
        auto bind = [&](XrAction a, const char* p) { b.push_back({a, path(p)}); };
        for (const char* side : {"left", "right"}) {
            std::string s = std::string("/user/hand/") + side + "/input/";
            bind(trigger_, (s + "trigger/value").c_str());
            bind(squeeze_, (s + "squeeze/value").c_str());
            bind(stick_, (s + "thumbstick").c_str());
            bind(pose_, (s + "grip/pose").c_str());
            bind(aim_, (s + "aim/pose").c_str());
        }
        bind(btnA_, "/user/hand/right/input/a/click");
        bind(btnB_, "/user/hand/right/input/b/click");
        bind(btnX_, "/user/hand/left/input/x/click");
        bind(btnY_, "/user/hand/left/input/y/click");
        bind(menu_, "/user/hand/left/input/menu/click");
        bind(thumbClick_, "/user/hand/left/input/thumbstick/click");
        bind(thumbClickR_, "/user/hand/right/input/thumbstick/click");
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
            asc.action = aim_;
            CHK(xrCreateActionSpace(session_, &asc, &h.aimSpace));
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
        if (depthExt_) {
            for (DXGI_FORMAT want : {DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_D24_UNORM_S8_UINT})
                if (std::find(formats.begin(), formats.end(), static_cast<int64_t>(want)) != formats.end()) {
                    depthFormat_ = want;
                    break;
                }
            if (depthFormat_ == DXGI_FORMAT_UNKNOWN) depthExt_ = false;
        }

        eyes_.resize(n);
        for (uint32_t i = 0; i < n; i++) {
            EyeSwapchain& e = eyes_[i];
            e.width = std::max(256, static_cast<int32_t>(viewConfigs_[i].recommendedImageRectWidth * scale_) & ~1);
            e.height = std::max(256, static_cast<int32_t>(viewConfigs_[i].recommendedImageRectHeight * scale_) & ~1);
            if (i == 0) std::printf("Resolution de rendu par oeil : %d x %d (recommandee %u x %u, echelle %.2f)\n", e.width, e.height,
                                    viewConfigs_[i].recommendedImageRectWidth, viewConfigs_[i].recommendedImageRectHeight, scale_);
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

            if (depthExt_) {  // swapchain de profondeur ; en cas d'echec on garde le tampon prive (sans profondeur transmise)
                XrSwapchainCreateInfo ds = sc;
                ds.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
                ds.format = static_cast<int64_t>(depthFormat_);
                uint32_t dcnt = 0;
                if (XR_SUCCEEDED(xrCreateSwapchain(session_, &ds, &e.depthHandle)) &&
                    XR_SUCCEEDED(xrEnumerateSwapchainImages(e.depthHandle, 0, &dcnt, nullptr)) && dcnt > 0) {
                    e.depthImages.assign(dcnt, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
                    if (XR_SUCCEEDED(xrEnumerateSwapchainImages(e.depthHandle, dcnt, &dcnt,
                                                                reinterpret_cast<XrSwapchainImageBaseHeader*>(e.depthImages.data())))) {
                        for (auto& img : e.depthImages) {
                            D3D11_DEPTH_STENCIL_VIEW_DESC dv = {};
                            dv.Format = depthFormat_;
                            dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
                            ID3D11DepthStencilView* view = nullptr;
                            dev_->CreateDepthStencilView(img.texture, &dv, &view);
                            e.depthDsvs.push_back(view);
                        }
                    }
                }
                if (e.depthDsvs.size() != e.depthImages.size() || e.depthDsvs.empty() ||
                    std::find(e.depthDsvs.begin(), e.depthDsvs.end(), nullptr) != e.depthDsvs.end()) {
                    if (e.depthHandle) xrDestroySwapchain(e.depthHandle);
                    e.depthHandle = XR_NULL_HANDLE;
                    e.depthImages.clear();
                    e.depthDsvs.clear();
                }
            }
        }
        bool anyDepth = false;
        for (const EyeSwapchain& e : eyes_) anyDepth = anyDepth || e.depthHandle != XR_NULL_HANDLE;
        std::printf(anyDepth ? "Profondeur transmise au runtime (essai : recalage plus juste quand la tete bouge)\n"
                             : "Profondeur non transmise (extension ou format indisponible, ou desactivee dans editor.json)\n");
    }

    // Demande au runtime la cadence la plus haute (ou editor.json "refresh_hz") : il impose 30 Hz a l'editeur par defaut.
    void tuneRefreshRate() {
        if (!refreshExt_ || !enumRates_ || !requestRate_) {
            std::printf("Cadence : extension XR_FB_display_refresh_rate indisponible, le runtime choisit seul\n");
            return;
        }
        uint32_t n = 0;
        enumRates_(session_, 0, &n, nullptr);
        std::vector<float> rates(n);
        if (n) enumRates_(session_, n, &n, rates.data());
        float cur = 0;
        if (getRate_) getRate_(session_, &cur);
        std::printf("Cadences proposees par le runtime :");
        for (float r : rates) std::printf(" %.0f", r);
        std::printf(" Hz ; actuelle : %.0f Hz\n", cur);
        if (rates.empty()) return;
        const float want = refreshWanted_ > 0 ? refreshWanted_ : *std::max_element(rates.begin(), rates.end());
        if (want != cur) {
            const XrResult r = requestRate_(session_, want);
            std::printf("Demande de %.0f Hz : %s (code %d)\n", want, XR_SUCCEEDED(r) ? "acceptee" : "refusee", static_cast<int>(r));
        }
    }

    void pollEvents() {
        XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(instance_, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                auto* s = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
                static const char* const names[] = {"inconnu", "IDLE", "READY", "SYNCHRONIZED", "VISIBLE", "FOCUSED", "STOPPING", "LOSS_PENDING", "EXITING"};
                std::printf("Etat de la session : %s\n", s->state < 9 ? names[s->state] : "?");
                if (s->state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    CHK(xrBeginSession(session_, &bi));
                    running_ = true;
                    std::printf("Session demarree. Mode : VOLUME\n");
                    tuneRefreshRate();
                } else if (s->state == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(session_);
                    running_ = false;
                } else if (s->state == XR_SESSION_STATE_EXITING || s->state == XR_SESSION_STATE_LOSS_PENDING) {
                    quit_ = true;
                }
            } else if (ev.type == XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB) {
                auto* r = reinterpret_cast<XrEventDataDisplayRefreshRateChangedFB*>(&ev);
                std::printf("Cadence d'affichage : %.0f Hz -> %.0f Hz\n", r->fromDisplayRefreshRate, r->toDisplayRefreshRate);
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
            XrSpaceLocation al = {XR_TYPE_SPACE_LOCATION};
            h.aimTracked = XR_SUCCEEDED(xrLocateSpace(h.aimSpace, stage_, time, &al)) && (al.locationFlags & need) == need;
            if (h.aimTracked) {
                h.aimPos = {al.pose.position.x, al.pose.position.y, al.pose.position.z};
                h.aimRot = {al.pose.orientation.x, al.pose.orientation.y, al.pose.orientation.z, al.pose.orientation.w};
            }
        }
        XrSpaceLocation l = {XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(view_, stage_, time, &l)) && (l.locationFlags & need) == need) {
            headPos_ = {l.pose.position.x, l.pose.position.y, l.pose.position.z};
            headRot_ = {l.pose.orientation.x, l.pose.orientation.y, l.pose.orientation.z, l.pose.orientation.w};
            headValid_ = true;
        }
    }

    // Jeux : le jeu actif (stick gauche ▲▼) decide a quel jeu appartiennent les nouvelles zones ; index 0 = tous les jeux.
    void loadProfiles() {
        ovz::Config c;
        ovz::loadConfig(c);
        profileNames_.assign(1, std::string());
        for (const ovz::Profile& p : c.profiles) profileNames_.push_back(p.name);
    }
    const std::string& activeProfile() const { return profileNames_[curProfile_]; }
    // Zone d'un autre jeu que le jeu actif : affichee en gris, non selectionnable.
    bool otherProfile(const Zone& z) const { return curProfile_ > 0 && !z.profile.empty() && z.profile != activeProfile(); }

    // --- donnees : enregistrement, historique, reglages ---------------------------------------------
    void save() { ovz::saveZones(ovz::zonesPath(), zones_); }

    void pushHistory() {
        history_.push_back(zones_);
        if (history_.size() > 50) history_.erase(history_.begin());
    }

    void undo() {
        if (history_.empty()) {
            std::printf("Rien a annuler\n");
            return;
        }
        zones_ = history_.back();
        history_.pop_back();
        grab_.active = false;
        phase_ = Phase::Idle;
        save();
        std::printf("Annule : %zu zone(s)\n", zones_.size());
    }

    void loadSettings() {
        std::ifstream f(ovz::dataDir() / "editor.json");
        if (!f) return;
        try {
            const auto j = nlohmann::json::parse(f);
            surfaceY_ = j.value("surface_y", surfaceY_);
            depthWanted_ = j.value("depth", depthWanted_);
            refreshWanted_ = j.value("refresh_hz", refreshWanted_);
            scale_ = std::clamp(j.value("scale", scale_), 0.25f, 1.5f);
        } catch (...) {
        }
    }
    void saveSettings() {
        std::error_code ec;
        std::filesystem::create_directories(ovz::dataDir(), ec);
        std::ofstream(ovz::dataDir() / "editor.json") << nlohmann::json{{"surface_y", surfaceY_}, {"depth", depthWanted_}, {"refresh_hz", refreshWanted_}, {"scale", scale_}}.dump(2);
    }

    // --- visee ---------------------------------------------------------------------------------------
    // Zone touchee par un rayon (boite orientee, methode des dalles). Renvoie l'indice ou -1.
    int pickZone(V3 o, V3 d, float& tBest) const {
        int best = -1;
        tBest = 1e9f;
        for (size_t i = 0; i < zones_.size(); i++) {
            const Zone& z = zones_[i];
            if (otherProfile(z)) continue;
            Q inv = ovm::qaxis('y', -z.yaw);
            V3 lo = ovm::qrot(inv, o - V3{z.pos[0], z.pos[1], z.pos[2]});
            V3 ld = ovm::qrot(inv, d);
            const float org[3] = {lo.x, lo.y, lo.z}, dir[3] = {ld.x, ld.y, ld.z};
            const float half[3] = {z.size[0] / 2 + kPickMargin, (z.type == ZoneType::Plane ? 0.0f : z.size[1] / 2) + kPickMargin,
                                   z.size[2] / 2 + kPickMargin};
            float t0 = 0, t1 = 1e9f;
            bool ok = true;
            for (int k = 0; k < 3 && ok; k++) {
                if (std::fabs(dir[k]) < 1e-6f) {
                    if (std::fabs(org[k]) > half[k]) ok = false;
                } else {
                    float ta = (-half[k] - org[k]) / dir[k], tb = (half[k] - org[k]) / dir[k];
                    if (ta > tb) std::swap(ta, tb);
                    t0 = std::max(t0, ta);
                    t1 = std::min(t1, tb);
                    if (t0 > t1) ok = false;
                }
            }
            if (ok && t0 < tBest) {
                tBest = t0;
                best = static_cast<int>(i);
            }
        }
        return best;
    }

    bool isGrabbed(size_t i) const {
        if (!grab_.active) return false;
        for (const Saved& s : grab_.items)
            if (s.index == i) return true;
        return false;
    }

    void updateRays() {
        for (int i = 0; i < 2; i++) {
            const Hand& h = hands_[i];
            rayValid_[i] = h.aimTracked;
            hover_[i] = -1;
            if (!h.aimTracked) continue;
            rayOrigin_[i] = h.aimPos;
            rayDir_[i] = ovm::qrot(h.aimRot, V3{0, 0, -1});
            if (phase_ == Phase::Idle && !grab_.active) hover_[i] = pickZone(rayOrigin_[i], rayDir_[i], hoverT_[i]);
        }
        // Plan de visee (main droite) : intersection du laser avec le plan horizontal y = surfaceY_.
        hasHit_ = false;
        if (!rayValid_[1] || rayDir_[1].y > -0.02f) return;
        float t = (surfaceY_ - rayOrigin_[1].y) / rayDir_[1].y;
        if (t < 0.05f || t > 8.0f) return;
        hit_ = rayOrigin_[1] + rayDir_[1] * t;
        hasHit_ = true;
    }

    // --- actions ---------------------------------------------------------------------------------------
    void readInputs() {
        in_.trigR.set(floatState(trigger_, 1) > kTriggerOn);
        in_.gripR.set(floatState(squeeze_, 1) > kTriggerOn);
        in_.a.set(boolState(btnA_));
        in_.b.set(boolState(btnB_));
        in_.thumbR.set(boolState(thumbClickR_));
        in_.trigL.set(floatState(trigger_, 0) > kTriggerOn);
        in_.gripL.set(floatState(squeeze_, 0) > kTriggerOn);
        in_.x.set(boolState(btnX_));
        in_.y.set(boolState(btnY_));
        in_.thumbL.set(boolState(thumbClick_));
        float lx;
        stickState(0, lx, in_.stickLy);
        if (boolState(menu_)) quit_ = true;
    }

    // Saisie : la seule zone visee ; zoneIndex < 0 : toutes les zones du jeu actif (recalage d'ensemble).
    void startGrab(int hand, int zoneIndex) {
        pushHistory();
        grab_ = Grab{};
        grab_.active = true;
        grab_.hand = hand;
        grab_.hand0 = hands_[hand].pos;
        grab_.heading0 = headingOf(hands_[hand].aimRot);
        for (size_t i = 0; i < zones_.size(); i++) {
            // Main gauche : tout l'objet. Main droite : la seule zone visee.
            const bool take = zoneIndex < 0 ? !otherProfile(zones_[i]) : static_cast<int>(i) == zoneIndex;
            if (!take) continue;
            const Zone& z = zones_[i];
            grab_.items.push_back({i, {z.pos[0], z.pos[1], z.pos[2]}, z.yaw, {z.size[0], z.size[1], z.size[2]}, z.pts});
        }
        std::printf("Saisie : %zu zone(s)\n", grab_.items.size());
    }

    void updateGrab(float dt) {
        if (!grab_.active) return;
        const Hand& h = hands_[grab_.hand];
        const bool held = grab_.hand == 1 ? in_.gripR.now : in_.gripL.now;
        if (!held) {
            grab_.active = false;
            save();
            return;
        }
        if (!h.tracked || !h.aimTracked) return;
        if (grab_.hand == 1) {
            if (in_.b.down() && grab_.items.size() == 1) {  // B pendant la saisie : supprime la zone tenue
                zones_.erase(zones_.begin() + grab_.items[0].index);
                grab_.active = false;
                save();
                std::printf("Zone supprimee\n");
                return;
            }
            float sx, sy;
            stickState(1, sx, sy);
            if (std::fabs(sx) > 0.15f) grab_.extraYaw += sx * dt * 1.5f;
            if (std::fabs(sy) > 0.15f) grab_.scale = std::clamp(grab_.scale * (1.0f + sy * dt * 0.8f), 0.3f, 3.0f);
            float lx, ly;
            stickState(0, lx, ly);  // stick gauche haut/bas : hauteur seule (volumes)
            if (std::fabs(ly) > 0.15f) grab_.heightScale = std::clamp(grab_.heightScale * (1.0f + ly * dt * 0.8f), 0.2f, 5.0f);
        }
        if (grab_.hand == 0) {  // stick droit : fin reglage de l'orientation (◄►) et avance/recul (▲▼)
            float sx, sy;
            stickState(1, sx, sy);
            if (std::fabs(sx) > 0.15f) grab_.extraYaw += sx * dt * 0.5f;
            if (std::fabs(sy) > 0.15f) {
                V3 f = ovm::qrot(headRot_, V3{0, 0, -1});
                f.y = 0;
                const float n = ovm::length(f);
                if (n > 1e-3f) grab_.nudge = grab_.nudge + f * (sy * dt * 0.15f / n);
            }
        }
        const float dyaw = headingOf(h.aimRot) - grab_.heading0 + grab_.extraYaw;
        const Q rot = ovm::qaxis('y', dyaw);
        V3 centroid;
        for (const Saved& s : grab_.items) centroid = centroid + s.pos * (1.0f / static_cast<float>(grab_.items.size()));
        const V3 delta = h.pos - grab_.hand0 + grab_.nudge;
        for (const Saved& s : grab_.items) {
            if (s.index >= zones_.size()) continue;
            Zone& z = zones_[s.index];
            // Rotation autour du centre du groupe (et non de la main, parfois loin de la zone visee).
            const V3 np = centroid + ovm::qrot(rot, s.pos - centroid) + delta;
            z.pos[0] = np.x;
            z.pos[1] = np.y;
            z.pos[2] = np.z;
            z.yaw = s.yaw + dyaw;
            if (grab_.hand == 1)
                for (int k = 0; k < 3; k++)
                    z.size[k] = z.type == ZoneType::Plane && k == 1 ? 0.0f : std::clamp(s.size[k] * grab_.scale * (k == 1 ? grab_.heightScale : 1.0f), 0.03f, 6.0f);
            if (grab_.hand == 1 && z.type == ZoneType::Poly) {  // les sommets suivent la boite englobante
                const float kx = z.size[0] / std::max(s.size[0], 1e-4f), kz = z.size[2] / std::max(s.size[2], 1e-4f);
                for (size_t i = 0; i < z.pts.size() && i < s.pts.size(); i++) z.pts[i] = {s.pts[i][0] * kx, s.pts[i][1] * kz};
            }
        }
    }

    // Zone en cours de trace : 1er bord (anchor_ -> corner_), profondeur signee dz, et pour un volume les hauteurs
    // absolues du haut et du bas (y0, y1, dans n'importe quel ordre). Un plan reste a la hauteur du plan de visee.
    void buildPreview(float dz, float y0, float y1) {
        const bool plane = mode_ == ZoneType::Plane;
        V3 e = corner_ - anchor_;
        e.y = 0;
        const float w = ovm::length(e);
        if (w > 1e-4f) drawYaw_ = std::atan2(-e.z, e.x);
        const V3 xd = {std::cos(drawYaw_), 0, -std::sin(drawYaw_)}, zd = {std::sin(drawYaw_), 0, std::cos(drawYaw_)};
        V3 c = anchor_ + xd * (w / 2) + zd * (dz / 2);
        float lo = std::min(y0, y1), hi = std::max(y0, y1);
        if (hi - lo < 0.03f) hi = lo + 0.03f;
        c.y = plane ? anchor_.y : (lo + hi) / 2;
        preview_ = Zone{};
        preview_.type = mode_;
        preview_.yaw = drawYaw_;
        preview_.size[0] = w;
        preview_.size[1] = plane ? 0.0f : hi - lo;
        preview_.size[2] = std::fabs(dz);
        preview_.pos[0] = c.x;
        preview_.pos[1] = c.y;
        preview_.pos[2] = c.z;
    }

    // Forme libre : les sommets (monde) deviennent des points relatifs au centre de leur boite englobante.
    void buildPolyPreview(float y0, float y1) {
        float minx = 1e9f, maxx = -1e9f, minz = 1e9f, maxz = -1e9f;
        for (const V3& p : polyPts_) {
            minx = std::min(minx, p.x);
            maxx = std::max(maxx, p.x);
            minz = std::min(minz, p.z);
            maxz = std::max(maxz, p.z);
        }
        const float cx = (minx + maxx) / 2, cz = (minz + maxz) / 2;
        float lo = std::min(y0, y1), hi = std::max(y0, y1);
        if (hi - lo < 0.03f) hi = lo + 0.03f;
        preview_ = Zone{};
        preview_.type = ZoneType::Poly;
        preview_.yaw = 0;
        preview_.pos[0] = cx;
        preview_.pos[1] = (lo + hi) / 2;
        preview_.pos[2] = cz;
        preview_.size[0] = maxx - minx;
        preview_.size[1] = hi - lo;
        preview_.size[2] = maxz - minz;
        for (const V3& p : polyPts_) preview_.pts.push_back({p.x - cx, p.z - cz});
    }

    // Apercu avec les hauteurs du haut et du bas : forme libre ou rectangle selon le mode.
    void previewHeights(float y0, float y1) {
        if (mode_ == ZoneType::Poly) buildPolyPreview(y0, y1);
        else buildPreview(dz_, y0, y1);
    }

    void cancelDrawing() {
        phase_ = Phase::Idle;
        polyPts_.clear();
    }

    void commitPreview() {
        pushHistory();
        preview_.name = (mode_ == ZoneType::Plane ? "plan" : mode_ == ZoneType::Poly ? "forme" : "volume") + std::to_string(counter_++);
        preview_.profile = activeProfile();
        zones_.push_back(preview_);
        save();
        std::printf("Zone '%s' creee : %.2f x %.2f x %.2f m (%zu au total)\n", preview_.name.c_str(), preview_.size[0],
                    preview_.size[1], preview_.size[2], zones_.size());
        cancelDrawing();
        startNaming();
    }

    void startNaming() {
        namingIdx_ = static_cast<int>(zones_.size()) - 1;
        namingOrig_ = zones_.back().name;
        namingBuf_.clear();
        namingTyped_ = false;
        namingStart_ = GetTickCount64();
        MessageBeep(MB_ICONASTERISK);  // signal sonore : la zone est creee, le nom peut etre saisi
        for (int vk = 0; vk < 256; vk++) keyPrev_[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
    }

    std::string uniqueName(const std::string& wanted, int except) const {
        auto taken = [&](const std::string& n) {
            for (int i = 0; i < static_cast<int>(zones_.size()); i++)
                if (i != except && zones_[i].name == n) return true;
            return false;
        };
        if (!taken(wanted)) return wanted;
        for (int k = 2;; k++) {
            const std::string c = wanted + " " + std::to_string(k);
            if (!taken(c)) return c;
        }
    }

    // Lit le clavier du PC (le casque est pose sur la tete mais le clavier reste a portee) pendant 20 s apres le trace.
    void pollNaming() {
        if (namingIdx_ < 0) return;
        if (namingIdx_ >= static_cast<int>(zones_.size()) || zones_[namingIdx_].name != namingOrig_) {  // annule / supprime entre-temps
            namingIdx_ = -1;
            return;
        }
        if (!namingTyped_ && GetTickCount64() - namingStart_ > 20000) {
            namingIdx_ = -1;
            return;
        }
        auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        BYTE state[256] = {};
        if (down(VK_SHIFT)) state[VK_SHIFT] = 0x80;
        if (down(VK_CONTROL)) state[VK_CONTROL] = 0x80;
        if (down(VK_MENU)) state[VK_MENU] = 0x80;
        if (GetKeyState(VK_CAPITAL) & 1) state[VK_CAPITAL] = 1;
        const HKL layout = GetKeyboardLayout(GetWindowThreadProcessId(GetForegroundWindow(), nullptr));
        for (int vk = 0x08; vk <= 0xFE; vk++) {
            const bool now = down(vk);
            const bool pressed = now && !keyPrev_[vk];
            keyPrev_[vk] = now;
            if (!pressed || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || (vk >= 0xA0 && vk <= 0xA5)) continue;
            if (vk == VK_ESCAPE) {
                namingIdx_ = -1;
                return;
            }
            if (vk == VK_RETURN) {
                size_t a = namingBuf_.find_first_not_of(L" "), b = namingBuf_.find_last_not_of(L" ");
                if (a != std::wstring::npos) {
                    const std::wstring w = namingBuf_.substr(a, b - a + 1);
                    char name[512] = {};
                    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, name, sizeof name - 1, nullptr, nullptr);
                    zones_[namingIdx_].name = uniqueName(name, namingIdx_);
                    save();
                    std::printf("Zone '%s' renommee '%s'\n", namingOrig_.c_str(), zones_[namingIdx_].name.c_str());
                }
                namingIdx_ = -1;
                return;
            }
            if (vk == VK_BACK) {
                if (!namingBuf_.empty()) namingBuf_.pop_back();
                continue;
            }
            wchar_t out[8] = {};
            const int n = ToUnicodeEx(vk, MapVirtualKeyW(vk, MAPVK_VK_TO_VSC), state, out, 8, 0x4, layout);
            if (n == 1 && out[0] >= 32 && namingBuf_.size() < 40) {
                namingBuf_ += out[0];
                namingTyped_ = true;
            }
        }
    }

    void updateDrawing() {
        const Hand& rh = hands_[1];
        switch (phase_) {
            case Phase::Idle:
                if (in_.trigR.down() && hasHit_ && !grab_.active) {
                    if (mode_ == ZoneType::Poly) {  // forme libre : un clic par sommet
                        phase_ = Phase::Points;
                        polyPts_.assign(1, hit_);
                    } else {
                        phase_ = Phase::Edge;
                        anchor_ = corner_ = hit_;
                    }
                }
                break;
            case Phase::Points: {  // gachette = ajouter un sommet ; clic pres du 1er (ou A) = fermer ; B = retirer le dernier
                if (in_.b.down()) {
                    polyPts_.pop_back();
                    if (polyPts_.empty()) cancelDrawing();
                    break;
                }
                auto dist2 = [](V3 a, V3 b) { return std::hypot(a.x - b.x, a.z - b.z); };
                const bool nearFirst = hasHit_ && polyPts_.size() >= 3 && dist2(hit_, polyPts_[0]) < 0.08f;
                if ((in_.a.down() && polyPts_.size() >= 3) || (in_.trigR.down() && nearFirst)) {
                    phase_ = Phase::Top;
                    heightAdj_ = 0;
                } else if (in_.trigR.down() && hasHit_ && dist2(hit_, polyPts_.back()) > 0.03f) {
                    polyPts_.push_back(hit_);
                }
                break;
            }
            case Phase::Edge:  // gachette maintenue : on tire le 1er bord
                if (hasHit_) corner_ = hit_;
                buildPreview(0.02f, surfaceY_, surfaceY_ + 0.02f);
                if (in_.b.down()) {
                    cancelDrawing();
                } else if (!in_.trigR.now) {
                    if (preview_.size[0] < 0.05f) {
                        cancelDrawing();
                        std::printf("Bord trop court, annule\n");
                    } else {
                        phase_ = Phase::Depth;
                        dz_ = 0.02f;
                    }
                }
                break;
            case Phase::Depth:  // on vise le cote oppose ; gachette pour valider
                if (hasHit_) {
                    const V3 zd = {std::sin(drawYaw_), 0, std::cos(drawYaw_)};
                    dz_ = ovm::dot(hit_ - anchor_, zd);
                }
                buildPreview(dz_, surfaceY_, surfaceY_ + 0.02f);
                if (in_.b.down()) {
                    cancelDrawing();
                } else if (in_.trigR.down() && std::fabs(dz_) >= 0.05f) {
                    if (mode_ == ZoneType::Plane) {
                        commitPreview();
                    } else {
                        phase_ = Phase::Top;
                        heightAdj_ = 0;
                    }
                }
                break;
            case Phase::Top: {  // la manette est au SOMMET de l'objet : sa hauteur reelle fixe le haut de la boite
                float sx, sy;
                stickState(1, sx, sy);
                if (std::fabs(sy) > 0.15f) heightAdj_ += sy * 0.3f * lastDt_;
                const float top = rh.pos.y + heightAdj_;
                previewHeights(surfaceY_, top);
                if (in_.b.down()) {
                    cancelDrawing();
                } else if (in_.trigR.down()) {
                    topY_ = top;
                    phase_ = Phase::Bottom;
                    heightAdj_ = 0;
                    bottomRefY_ = rh.pos.y;
                    bottomActive_ = false;
                }
                break;
            }
            case Phase::Bottom: {  // la manette est au BAS de l'objet ; sans bouger, le bas reste au plan de visee
                float sx, sy;
                stickState(1, sx, sy);
                if (std::fabs(sy) > 0.15f) heightAdj_ += sy * 0.3f * lastDt_;
                if (!bottomActive_ && std::fabs(rh.pos.y - bottomRefY_) > 0.04f) bottomActive_ = true;
                const float bottom = (bottomActive_ ? rh.pos.y : surfaceY_) + heightAdj_;
                previewHeights(bottom, topY_);
                if (in_.b.down()) cancelDrawing();
                else if (in_.trigR.down()) commitPreview();
                break;
            }
        }
    }

    void updateInteraction(float dt) {
        lastDt_ = dt;
        readInputs();
        updateRays();
        const Hand& rh = hands_[1];
        const Hand& lh = hands_[0];

        // Main gauche : aide, objet actif, annuler / tout effacer.
        if (!hudPlaced_ && headValid_) {
            placeHud();
            hudPlaced_ = true;
        }
        if (in_.thumbL.down()) {
            showHelp_ = !showHelp_;
            if (showHelp_) placeHud();  // reapparait devant toi
        }
        if (phase_ == Phase::Idle && !grab_.active) {  // stick droit haut/bas : monte/descend le plan de visee
            float sx, sy;
            stickState(1, sx, sy);
            if (std::fabs(sy) > 0.5f) {
                surfaceY_ = std::clamp(surfaceY_ + sy * 0.6f * dt, 0.0f, 2.5f);
                surfaceDirty_ = true;
            } else if (surfaceDirty_) {
                saveSettings();
                surfaceDirty_ = false;
            }
        }
        const int flickY = in_.stickLy > 0.7f ? 1 : in_.stickLy < -0.7f ? -1 : 0;
        if (flickY != 0 && flickY != prevFlickY_ && phase_ == Phase::Idle && !grab_.active && profileNames_.size() > 1) {
            const int n = static_cast<int>(profileNames_.size());
            curProfile_ = (curProfile_ + flickY + n) % n;
            hover_[0] = hover_[1] = -1;
        }
        prevFlickY_ = flickY;
        if (in_.x.now) {
            xHold_ += dt;
            if (!xCleared_ && xHold_ >= 2.0f) {
                pushHistory();
                zones_.clear();
                grab_.active = false;
                save();
                xCleared_ = true;
                std::printf("Toutes les zones effacees (X pour annuler)\n");
            }
        }
        if (in_.x.up()) {
            if (!xCleared_ && xHold_ < 1.0f) undo();
            xHold_ = 0;
            xCleared_ = false;
        }

        if (phase_ == Phase::Idle && !grab_.active) {
            if (in_.a.down()) {
                mode_ = mode_ == ZoneType::Box ? ZoneType::Plane : mode_ == ZoneType::Plane ? ZoneType::Poly : ZoneType::Box;
                std::printf("Mode : %s\n", mode_ == ZoneType::Box ? "VOLUME" : mode_ == ZoneType::Plane ? "PLAN" : "FORME LIBRE");
            }
            if (in_.b.down() && hover_[1] >= 0) {  // supprimer la zone visee
                pushHistory();
                std::printf("Zone '%s' supprimee\n", zones_[hover_[1]].name.c_str());
                zones_.erase(zones_.begin() + hover_[1]);
                hover_[1] = -1;
                save();
            }
            if (in_.gripR.down() && hover_[1] >= 0 && rh.tracked && rh.aimTracked) startGrab(1, hover_[1]);
            if (in_.gripL.down() && lh.tracked && lh.aimTracked && (hover_[0] >= 0 || !zones_.empty())) startGrab(0, hover_[0]);  // dans le vide : toutes
            if (in_.y.down() && hover_[0] >= 0) {  // affichage conditionnel de la zone visee
                pushHistory();
                Zone& z = zones_[hover_[0]];
                z.look = z.look > 0 ? 0.0f : kLookDegrees;
                save();
            }
        }
        if (in_.trigL.up() && phase_ == Phase::Idle && !grab_.active && hover_[0] >= 0) {  // rattacher la zone visee au jeu actif
            pushHistory();
            zones_[hover_[0]].profile = activeProfile();
            save();
        }
        if (in_.thumbR.down() && rh.tracked) {  // manette posee sur le bureau + clic = hauteur du plan de visee
            surfaceY_ = rh.pos.y;
            saveSettings();
            std::printf("Hauteur du plan de visee : %.2f m\n", surfaceY_);
        }
        updateGrab(dt);
        updateDrawing();
        pollNaming();
        updateHud();
    }

    // Les panneaux restent fixes dans la piece, devant toi au moment ou ils sont (re)affiches.
    void placeHud() {
        V3 f = ovm::qrot(headRot_, V3{0, 0, -1});
        f.y = 0;
        const float n = ovm::length(f);
        f = n > 1e-3f ? f * (1.0f / n) : V3{0, 0, -1};
        hudPos_ = headPos_ + f * 1.1f;
        hudPos_.y = headPos_.y - 0.12f;
        hudYaw_ = headingOf(headRot_);
    }

    // --- aide et etat affiches dans le casque ------------------------------------------------------------
    std::wstring zoneLabel(int index) const {
        if (index < 0 || index >= static_cast<int>(zones_.size())) return L"—";
        const Zone& z = zones_[index];
        std::wstring s = widen(z.name);
        s += z.profile.empty() ? L"  {tous les jeux}" : L"  {" + widen(z.profile) + L"}";
        if (z.look > 0) s += L"  (visible en regardant vers le bas)";
        return s;
    }

    void updateHud() {
        const bool plane = mode_ == ZoneType::Plane;
        std::wstring hint;
        switch (phase_) {
            case Phase::Idle:
                hint = grab_.active ? L"Saisie : déplace la main, tourne le poignet pour orienter"
                                    : L"Prêt : vise le bureau et maintiens la gâchette droite pour tracer";
                break;
            case Phase::Edge: hint = std::wstring(L"1/") + (plane ? L"2" : L"4") + L" Tire le 1er bord jusqu'à l'autre coin, puis relâche (B annule)"; break;
            case Phase::Depth: hint = std::wstring(L"2/") + (plane ? L"2" : L"4") + L" Vise le côté opposé, gâchette pour valider (B annule)"; break;
            case Phase::Points:
                hint = L"Forme libre : gâchette à chaque sommet ; clic sur le 1er point (ou A) ferme ; B retire le dernier";
                break;
            case Phase::Top: hint = std::wstring(mode_ == ZoneType::Poly ? L"2/3" : L"3/4") + L" Place la manette au SOMMET de l'objet, gâchette pour valider (B annule)"; break;
            case Phase::Bottom: hint = std::wstring(mode_ == ZoneType::Poly ? L"3/3" : L"4/4") + L" Descends la manette au BAS de l'objet (ou valide : bas = plan de visée), gâchette"; break;
        }
        if (xHold_ > 0.6f && in_.x.now && !xCleared_) hint = L"!Maintiens X… tout va être effacé";
        if (namingIdx_ >= 0) {
            const std::wstring keep = L"Échap : garder « " + widen(namingOrig_) + L" »";
            hint = namingTyped_ ? L"Nom : " + namingBuf_ + L"_     (Entrée valide, " + keep + L")"
                                : L"Zone créée : tape son nom au clavier puis Entrée (" + keep + L")";
        }
        if (namingIdx_ >= 0) {  // invite bien visible : le panneau d'etat est trop haut pour etre lu en regardant le bureau
            namePanel_.setLines({L"#NOM DE LA ZONE",
                                 L"Tape son nom sur le clavier du PC, puis Entrée   (Échap : garder « " + widen(namingOrig_) + L" »)",
                                 namingTyped_ ? L"> " + namingBuf_ + L"_" : L"> (nom par défaut : " + widen(namingOrig_) + L")"});
        }
        char buf[160];
        std::snprintf(buf, sizeof buf, "%d zone(s)  |  bureau à %.2f m  |  %.0f img/s (runtime %.0f Hz), calcul %.1f ms", static_cast<int>(zones_.size()),
                      surfaceY_, perfFps_, periodMs_ > 0 ? 1000.0f / periodMs_ : 0.0f, perfAvgMs_);
        status_.setLines({std::wstring(L"#Mode : ") + (plane ? L"PLAN" : mode_ == ZoneType::Poly ? L"FORME LIBRE" : L"VOLUME") + L"   |   Jeu actif : " +
                              (curProfile_ == 0 ? std::wstring(L"tous les jeux") : widen(activeProfile())),
                          widen(buf),
                          hint,
                          L"Zone visée : " + zoneLabel(hover_[1] >= 0 ? hover_[1] : hover_[0])});
        if (!showHelp_) return;
        help_.setLines({
            L"#MAIN DROITE",
            L"Gâchette : tracer (1er bord, largeur, puis haut et bas)",
            L"Grip tenu sur une zone visée : la déplacer ; tourne le poignet",
            L"     pour l'orienter ; stick ◄► tourne, ▲▼ taille ; stick gauche ▲▼ hauteur",
            L"B sur une zone visée : la supprimer",
            L"A : volume / plan / forme libre        Stick ▲▼ : monter/descendre le plan de visée",
            L"Forme libre : gâchette à chaque sommet, A ou clic sur le 1er point ferme, B retire",
            L"Clic stick : plan de visée = hauteur de la manette (posée sur le bureau)",
            L"#MAIN GAUCHE",
            L"X : annuler (maintenu 2 s : tout effacer)",
            L"Y sur une zone visée : visible seulement en regardant en bas",
            L"Stick ▲▼ : jeu actif (les nouvelles zones lui appartiennent)",
            L"Gâchette (relâchée) sur une zone visée : la rattacher au jeu actif",
            L"Grip tenu sur une zone visée : la déplacer",
            L"Grip tenu dans le vide : déplacer TOUTES les zones (recalage) ;",
            L"     stick droit pendant ce geste : ◄► tourne fin, ▲▼ avance/recule",
            L"Clic stick : masquer / replacer l'aide devant toi         Menu : quitter",
        });
    }

    // --- rendu -------------------------------------------------------------------------------------------
    void drawScene() {
        const float orange[4] = {1.0f, 0.55f, 0.1f, 1};
        const float cyan[4] = {0.1f, 0.85f, 1, 1}, yellow[4] = {1, 0.9f, 0.0f, 1}, white[4] = {1, 1, 1, 1}, green[4] = {0.2f, 1, 0.2f, 1};
        for (size_t i = 0; i < zones_.size(); i++) {
            const Zone& z = zones_[i];
            const bool plane = z.type == ZoneType::Plane;
            const bool hovered = static_cast<int>(i) == hover_[0] || static_cast<int>(i) == hover_[1];
            if (otherProfile(z)) {
                const float grey[4] = {0.38f, 0.38f, 0.42f, 1};
                drawZone(rend_, z, grey, 0.003f);
                continue;
            }
            const float* col = isGrabbed(i) ? green : hovered ? white : plane ? yellow : z.type == ZoneType::Poly ? orange : cyan;
            drawZone(rend_, z, col, hovered || isGrabbed(i) ? 0.008f : 0.004f);  // traits fins
            if (z.look > 0) {  // repere blanc au-dessus : affichage conditionnel actif
                rend_.box(boxWorld({z.pos[0], z.pos[1] + (plane ? 0.06f : z.size[1] / 2 + 0.06f), z.pos[2]}, 0, {0.05f, 0.05f, 0.05f}), white);
            }
        }
        if (phase_ == Phase::Points) {  // contour en cours : segments, sommets et trait vers le point vise
            for (size_t i = 0; i + 1 < polyPts_.size(); i++) drawSegment(rend_, polyPts_[i], polyPts_[i + 1], green, 0.006f);
            if (hasHit_) drawSegment(rend_, polyPts_.back(), hit_, white, 0.004f);
            const bool closable = hasHit_ && polyPts_.size() >= 3 && std::hypot(hit_.x - polyPts_[0].x, hit_.z - polyPts_[0].z) < 0.08f;
            for (size_t i = 0; i < polyPts_.size(); i++)
                rend_.box(boxWorld(polyPts_[i], 0, i == 0 ? V3{0.03f, 0.03f, 0.03f} : V3{0.015f, 0.015f, 0.015f}), i == 0 && closable ? white : green);
        } else if (phase_ != Phase::Idle) {
            drawZone(rend_, preview_, green, 0.008f);
        }

        // Lasers et curseurs.
        const float* rightCol = mode_ == ZoneType::Box ? cyan : mode_ == ZoneType::Plane ? yellow : orange;
        for (int i = 0; i < 2; i++) {
            if (!rayValid_[i]) continue;
            float len = 2.5f;
            if (hover_[i] >= 0) len = hoverT_[i];
            else if (i == 1 && hasHit_) len = ovm::length(hit_ - rayOrigin_[1]);
            const float leftCol[4] = {0.85f, 0.85f, 0.85f, 1};
            const float* col = i == 1 ? rightCol : leftCol;
            rend_.box(ovm::mmul(ovm::mFromPose(rayOrigin_[i] + rayDir_[i] * (len / 2), hands_[i].aimRot),
                                ovm::mScale({0.004f, 0.004f, len})), col);
            rend_.box(boxWorld(rayOrigin_[i] + rayDir_[i] * len, 0, {0.02f, 0.02f, 0.02f}), white);
        }
        if (hasHit_ && hover_[1] < 0) {  // croix sur le plan de visee
            rend_.box(boxWorld(hit_, 0, {0.10f, 0.004f, 0.004f}), white);
            rend_.box(boxWorld(hit_, 0, {0.004f, 0.004f, 0.10f}), white);
        }

        // Panneaux de texte fixes dans la piece (ils ne suivent pas la tete), l'etat au-dessus de l'aide.
        rend_.beginPanels();
        const Mat4 hud = ovm::mFromPose(hudPos_, ovm::qaxis('y', hudYaw_));
        const Mat4 head = ovm::mFromPose(headPos_, headRot_);
        const float w = 0.65f;
        rend_.panel(ovm::mmul(hud, ovm::mmul(ovm::mTranslate({0, showHelp_ ? 0.26f : 0.0f, 0}), ovm::mScale({w, w * 130 / 820, 1}))), status_.srv());
        if (showHelp_) rend_.panel(ovm::mmul(hud, ovm::mScale({w, w * 500 / 820, 1})), help_.srv());
        if (namingIdx_ >= 0)  // invite de nom : collee a la tete, devant et un peu en dessous du regard
            rend_.panel(ovm::mmul(head, ovm::mmul(ovm::mTranslate({0, -0.12f, -0.75f}), ovm::mScale({0.5f, 0.5f * 140 / 820, 1}))), namePanel_.srv());
    }

    void frame() {
        XrFrameState fs = {XR_TYPE_FRAME_STATE};
        const auto tWait0 = std::chrono::steady_clock::now();
        CHK(xrWaitFrame(session_, nullptr, &fs));
        perfWaitFrame_ += std::chrono::duration<float>(std::chrono::steady_clock::now() - tWait0).count();
        CHK(xrBeginFrame(session_, nullptr));
        periodMs_ = static_cast<float>(fs.predictedDisplayPeriod) / 1.0e6f;

        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - lastFrame_).count();
        lastFrame_ = now;
        if (dt < 1.0f) {
            perfAcc_ += dt;
            perfFrames_++;
        }

        std::vector<XrCompositionLayerProjectionView> projViews;
        std::vector<XrCompositionLayerDepthInfoKHR> depthInfos;
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
            depthInfos.assign(vc, {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR});
            for (uint32_t i = 0; i < vc; i++) {
                EyeSwapchain& e = eyes_[i];
                uint32_t idx = 0;
                CHK(xrAcquireSwapchainImage(e.handle, nullptr, &idx));
                XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wi.timeout = XR_INFINITE_DURATION;
                const auto tImg0 = std::chrono::steady_clock::now();
                CHK(xrWaitSwapchainImage(e.handle, &wi));
                perfWaitImage_ += std::chrono::duration<float>(std::chrono::steady_clock::now() - tImg0).count();
                // Profondeur : image du swapchain de profondeur si disponible, sinon tampon prive.
                bool haveDepth = false;
                uint32_t didx = 0;
                if (e.depthHandle != XR_NULL_HANDLE && XR_SUCCEEDED(xrAcquireSwapchainImage(e.depthHandle, nullptr, &didx))) {
                    if (XR_SUCCEEDED(xrWaitSwapchainImage(e.depthHandle, &wi)) && didx < e.depthDsvs.size()) haveDepth = true;
                    else xrReleaseSwapchainImage(e.depthHandle, nullptr);
                }
                ID3D11DepthStencilView* dsv = haveDepth ? e.depthDsvs[didx] : e.dsv;

                const XrPosef& pp = views[i].pose;
                const XrFovf& fov = views[i].fov;
                Mat4 view = ovm::mInverseRigid({pp.position.x, pp.position.y, pp.position.z},
                                               {pp.orientation.x, pp.orientation.y, pp.orientation.z, pp.orientation.w});
                rend_.viewProj = ovm::mmul(ovm::mProjection(fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown, 0.05f, 50.0f), view);

                D3D11_VIEWPORT vp = {0, 0, static_cast<float>(e.width), static_cast<float>(e.height), 0, 1};
                ctx_->RSSetViewports(1, &vp);
                ctx_->OMSetRenderTargets(1, &e.rtvs[idx], dsv);
                const float magenta[4] = {1, 0, 1, 1};  // fond = passthrough via le chroma key de VD
                ctx_->ClearRenderTargetView(e.rtvs[idx], magenta);
                ctx_->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
                rend_.begin();
                drawScene();
                ctx_->Flush();  // D3D11 : sans cela les commandes peuvent rester dans le pilote et le runtime attend le GPU

                CHK(xrReleaseSwapchainImage(e.handle, nullptr));
                projViews[i].pose = pp;
                projViews[i].fov = fov;
                projViews[i].subImage.swapchain = e.handle;
                projViews[i].subImage.imageRect = {{0, 0}, {e.width, e.height}};
                if (haveDepth) {
                    xrReleaseSwapchainImage(e.depthHandle, nullptr);
                    depthInfos[i].subImage.swapchain = e.depthHandle;
                    depthInfos[i].subImage.imageRect = {{0, 0}, {e.width, e.height}};
                    depthInfos[i].subImage.imageArrayIndex = 0;
                    depthInfos[i].minDepth = 0.0f;  // meme plage et memes plans que la matrice de projection (0,05 m - 50 m)
                    depthInfos[i].maxDepth = 1.0f;
                    depthInfos[i].nearZ = 0.05f;
                    depthInfos[i].farZ = 50.0f;
                    projViews[i].next = &depthInfos[i];
                }
            }
            layer.space = stage_;
            layer.viewCount = vc;
            layer.views = projViews.data();
            layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
            layerCount = 1;
        }

        const float work = std::chrono::duration<float>(std::chrono::steady_clock::now() - now).count();  // calcul + rendu de l'image
        perfWork_ += work;
        perfWorst_ = std::max(perfWorst_, work);
        if (perfAcc_ >= 1.0f && perfFrames_ > 0) {
            perfFps_ = perfFrames_ / perfAcc_;
            perfAvgMs_ = perfWork_ / perfFrames_ * 1000.0f;
            perfWorstMs_ = perfWorst_ * 1000.0f;
            if (++perfReports_ % 5 == 0)
                std::printf("Performances : %.0f images/s, calcul %.1f ms par image (pire %.0f ms), rythme du runtime : %.1f ms (%.0f Hz)\n"
                            "   attentes par image : xrWaitFrame %.1f ms, xrWaitSwapchainImage %.1f ms, xrEndFrame %.1f ms\n",
                            perfFps_, perfAvgMs_, perfWorstMs_, periodMs_, periodMs_ > 0 ? 1000.0f / periodMs_ : 0.0f,
                            perfWaitFrame_ / perfFrames_ * 1000.0f, perfWaitImage_ / perfFrames_ * 1000.0f, perfEnd_ / perfFrames_ * 1000.0f);
            perfWaitFrame_ = perfWaitImage_ = perfEnd_ = 0;
            perfAcc_ = perfWork_ = perfWorst_ = 0;
            perfFrames_ = 0;
        }

        XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
        ei.displayTime = fs.predictedDisplayTime;
        ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        ei.layerCount = layerCount;
        ei.layers = layers;
        const auto tEnd0 = std::chrono::steady_clock::now();
        CHK(xrEndFrame(session_, &ei));
        perfEnd_ += std::chrono::duration<float>(std::chrono::steady_clock::now() - tEnd0).count();
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

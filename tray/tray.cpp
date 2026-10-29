// OverlayVR : application de la barre d'etat (a cote de l'horloge).
//   Clic gauche  : active / desactive les zones, tout de suite (meme jeu lance), sans droits administrateur.
//   Clic droit   : menu - lancer le gestionnaire de zones, charger / decharger la couche OpenXR (administrateur),
//                  reglages, dossier des donnees, demarrage avec Windows.
// Icone en forme d'oeil : iris vert = zones actives, gris et barre = zones coupees, rouge et barre = couche OpenXR desactivee.
#define NOMINMAX
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <string>

#include "../common/config.h"
#include "../common/joypoll.h"
#include "../common/keys.h"

namespace {

constexpr UINT kTrayMsg = WM_APP + 1;
constexpr UINT_PTR kTimer = 1, kPollTimer = 2;
enum : UINT { ID_ZONES = 1, ID_LAYER, ID_EDITOR, ID_SETTINGS, ID_FOLDER, ID_AUTOSTART, ID_QUIT, ID_RAWJSON };
enum IconState { Green = 0, Grey = 1, Red = 2 };

const wchar_t* const kLayerKey = L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit";
const wchar_t* const kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

ovz::Config g_cfg;            // reglages relus chaque seconde (touche et bouton du gestionnaire de zones)
ULONGLONG g_cfgLoaded = 0;
ovz::JoyButton g_joy;
bool g_editorKeyDown = false;
HWND g_hwnd = nullptr;
UINT g_taskbarCreated = 0;
HICON g_icons[3] = {};
std::wstring g_exeDir, g_manifest, g_exePath;
IconState g_shown = Green;
bool g_haveIcon = false;

// --- etat ----------------------------------------------------------------------------------------------
bool zonesEnabled() {
    ovz::Config c;
    ovz::loadConfig(c);
    return c.enabled;
}

void setZonesEnabled(bool on) {
    ovz::Config c;
    ovz::loadConfig(c);
    c.enabled = on;
    ovz::saveConfig(c);
}

// -1 : couche non installee, 0 : activee, 1 : desactivee.
int layerState() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kLayerKey, 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return -1;
    DWORD v = 0, size = sizeof v, type = 0;
    const LONG r = RegQueryValueExW(k, g_manifest.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(&v), &size);
    RegCloseKey(k);
    return r == ERROR_SUCCESS && type == REG_DWORD ? static_cast<int>(v) : -1;
}

// Ecrit dans HKLM : demande l'elevation (fenetre UAC) via reg.exe.
void setLayerDisabled(bool disabled) {
    const std::wstring args = L"add \"HKLM\\" + std::wstring(kLayerKey) + L"\" /v \"" + g_manifest +
                              L"\" /t REG_DWORD /d " + (disabled ? L"1" : L"0") + L" /f";
    SHELLEXECUTEINFOW sei = {sizeof sei};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = L"reg.exe";
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (ShellExecuteExW(&sei) && sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 15000);
        CloseHandle(sei.hProcess);
    }
}

bool autostartEnabled() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    const bool present = RegQueryValueExW(k, L"OverlayVR", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return present;
}

void setAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_WRITE, &k) != ERROR_SUCCESS) return;
    if (on) {
        const std::wstring cmd = L"\"" + g_exePath + L"\"";
        RegSetValueExW(k, L"OverlayVR", 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                       static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"OverlayVR");
    }
    RegCloseKey(k);
}

IconState currentState() {
    if (layerState() == 1) return Red;
    return zonesEnabled() ? Green : Grey;
}

// --- icone ---------------------------------------------------------------------------------------------
// Couleur d'un point de l'icone en forme d'oeil (u, v dans [-1, 1], v vers le haut). Renvoie faux hors de l'icone.
bool eyePixel(float u, float v, uint8_t ir, uint8_t ig, uint8_t ib, bool slashed, float rgb[3]) {
    auto set = [&](float r, float g, float b) {
        rgb[0] = r;
        rgb[1] = g;
        rgb[2] = b;
    };
    bool hit = false;
    const float lens = std::fabs(u) <= 1.0f ? 0.58f * (1.0f - u * u) : -1.0f;  // demi-hauteur de la paupiere en u
    const float inside = lens - std::fabs(v);                                  // > 0 : dans l'oeil
    if (inside >= 0) {
        hit = true;
        if (inside < 0.10f) set(25, 25, 35);                                   // contour
        else if (u * u + v * v < 0.17f * 0.17f) set(15, 15, 20);               // pupille
        else if ((u + 0.12f) * (u + 0.12f) + (v - 0.14f) * (v - 0.14f) < 0.07f * 0.07f) set(255, 255, 255);  // reflet
        else if (u * u + v * v < 0.40f * 0.40f) set(ir, ig, ib);               // iris, couleur = etat
        else set(246, 246, 246);                                               // blanc de l'oeil
    }
    if (slashed) {  // oeil barre : zones ou couche coupees
        const float dist = std::fabs(0.8f * u + v) / std::sqrt(0.8f * 0.8f + 1.0f);  // distance a la droite v = -0.8 u
        if (std::fabs(u) < 0.95f && dist < 0.15f) {
            hit = true;
            if (dist < 0.075f) set(205, 35, 35);
            else set(255, 255, 255);  // liseré blanc autour du trait
        }
    }
    return hit;
}

// Icone en forme d'oeil : l'iris prend la couleur de l'etat, un trait barre l'oeil quand c'est coupe.
HICON makeIcon(uint8_t r, uint8_t g, uint8_t b, bool slashed) {
    constexpr int S = 32, SS = 4;  // 4x4 sous-echantillons par pixel : bords lisses
    HDC dc = GetDC(nullptr);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = S;
    bi.bmiHeader.biHeight = -S;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    uint32_t* px = nullptr;
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&px), nullptr, 0);
    HBITMAP mask = CreateBitmap(S, S, 1, 1, nullptr);
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            float acc[3] = {0, 0, 0}, cover = 0;
            for (int sy = 0; sy < SS; sy++)
                for (int sx = 0; sx < SS; sx++) {
                    const float u = ((x + (sx + 0.5f) / SS) - S / 2.0f) / 15.5f;
                    const float v = -((y + (sy + 0.5f) / SS) - S / 2.0f) / 15.5f;
                    float c3[3];
                    if (eyePixel(u, v, r, g, b, slashed, c3)) {
                        acc[0] += c3[0];
                        acc[1] += c3[1];
                        acc[2] += c3[2];
                        cover += 1;
                    }
                }
            const float n = static_cast<float>(SS * SS);
            const uint32_t A = static_cast<uint32_t>(cover / n * 255);
            // alpha premultiplie : les couleurs accumulees sont deja ponderees par la couverture
            px[y * S + x] = (A << 24) | (static_cast<uint32_t>(acc[0] / n) << 16) | (static_cast<uint32_t>(acc[1] / n) << 8) |
                            static_cast<uint32_t>(acc[2] / n);
        }
    ICONINFO ii = {TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    ReleaseDC(nullptr, dc);
    return icon;
}

const wchar_t* tooltipFor(IconState s) {
    switch (s) {
        case Green: return L"OverlayVR : zones activées";
        case Grey: return L"OverlayVR : zones désactivées";
        default: return L"OverlayVR : couche OpenXR désactivée";
    }
}

void updateIcon(bool add) {
    NOTIFYICONDATAW nid = {sizeof nid};
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = kTrayMsg;
    g_shown = currentState();
    nid.hIcon = g_icons[g_shown];
    wcsncpy(nid.szTip, tooltipFor(g_shown), ARRAYSIZE(nid.szTip) - 1);
    Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &nid);
    g_haveIcon = true;
}

// --- actions -------------------------------------------------------------------------------------------
bool editorRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe = {sizeof pe};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        found = _wcsicmp(pe.szExeFile, L"OverlayVR_Editor.exe") == 0;
    CloseHandle(snap);
    return found;
}

void launchEditor() {
    if (editorRunning()) return;  // deja lance : un seul gestionnaire a la fois
    const std::wstring exe = g_exeDir + L"\\OverlayVR_Editor.exe";
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, g_exeDir.c_str(), SW_SHOW)) <= 32)
        MessageBoxW(nullptr, L"OverlayVR_Editor.exe est introuvable à côté de cette application.", L"OverlayVR", MB_ICONWARNING);
}

// Touche clavier et bouton de HOTAS qui lancent le gestionnaire de zones : lus ici (processus normal, sans droits
// administrateur) car la couche, elle, n'existe que dans un jeu.
void pollEditorTriggers() {
    const ULONGLONG now = GetTickCount64();
    if (now - g_cfgLoaded > 1000) {
        g_cfgLoaded = now;
        ovz::Config c;
        if (ovz::loadConfig(c)) g_cfg = c;
    }
    bool trigger = false;
    const ovz::KeySpec k = ovz::parseKey(g_cfg.editorKey);
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    const bool pressed = k.vk && down(k.vk) && (!k.ctrl || down(VK_CONTROL)) && (!k.alt || down(VK_MENU)) && (!k.shift || down(VK_SHIFT));
    if (pressed && !g_editorKeyDown) trigger = true;
    g_editorKeyDown = pressed;
    if (g_joy.pressed(g_hwnd, g_cfg.editorJoystickButton, g_cfg.editorJoystick)) trigger = true;
    if (trigger && !editorRunning()) {
        MessageBeep(MB_OK);
        launchEditor();
    }
}

std::wstring shortcutLabel() {
    const ovz::KeySpec k = ovz::parseKey(g_cfg.editorKey);
    std::string t = k.vk ? ovz::formatKey(k) : "";
    if (g_cfg.editorJoystickButton > 0) t += (t.empty() ? "" : " / ") + std::string("bouton ") + std::to_string(g_cfg.editorJoystickButton);
    if (t.empty()) return L"";
    return L"\t" + std::wstring(t.begin(), t.end());  // ASCII : touches et numero de bouton
}

void showMenu() {
    ovz::ensureConfigFile();
    const int layer = layerState();
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (zonesEnabled() ? MF_CHECKED : 0), ID_ZONES, L"Zones activées\t(clic gauche sur l'icône)");
    AppendMenuW(m, MF_STRING | (layer == 0 ? MF_CHECKED : 0) | (layer < 0 ? MF_GRAYED : 0), ID_LAYER,
                L"Couche OpenXR chargée par les jeux (administrateur)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EDITOR, (std::wstring(L"Lancer le gestionnaire de zones (dans le casque)") + shortcutLabel()).c_str());
    AppendMenuW(m, MF_STRING, ID_SETTINGS, L"Configuration… (bouton de bascule, jeux, regard)");
    AppendMenuW(m, MF_STRING, ID_RAWJSON, L"Modifier settings.json (avancé)");
    AppendMenuW(m, MF_STRING, ID_FOLDER, L"Ouvrir le dossier des données");
    AppendMenuW(m, MF_STRING | (autostartEnabled() ? MF_CHECKED : 0), ID_AUTOSTART, L"Lancer au démarrage de Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_QUIT, L"Quitter");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);  // sinon le menu ne se ferme pas au clic ailleurs
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated) {  // l'explorateur a redemarre : on remet l'icone
        updateIcon(true);
        return 0;
    }
    switch (msg) {
        case kTrayMsg:
            if (lp == WM_LBUTTONUP) {
                setZonesEnabled(!zonesEnabled());
                updateIcon(false);
            } else if (lp == WM_RBUTTONUP) {
                showMenu();
            }
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case ID_ZONES:
                    setZonesEnabled(!zonesEnabled());
                    break;
                case ID_LAYER:
                    setLayerDisabled(layerState() == 0);
                    break;
                case ID_EDITOR:
                    launchEditor();
                    break;
                case ID_SETTINGS: {
                    const std::wstring exe = g_exeDir + L"\\OverlayVR_Config.exe";
                    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, g_exeDir.c_str(), SW_SHOW)) > 32) break;
                    [[fallthrough]];  // pas de fenetre de configuration a cote de l'application : on ouvre le fichier
                }
                case ID_RAWJSON: {
                    ovz::ensureConfigFile();
                    const std::wstring arg = L"\"" + ovz::configPath().wstring() + L"\"";
                    ShellExecuteW(nullptr, L"open", L"notepad.exe", arg.c_str(), nullptr, SW_SHOW);
                    break;
                }
                case ID_FOLDER: {
                    std::error_code ec;
                    std::filesystem::create_directories(ovz::dataDir(), ec);
                    ShellExecuteW(nullptr, L"open", ovz::dataDir().wstring().c_str(), nullptr, nullptr, SW_SHOW);
                    break;
                }
                case ID_AUTOSTART:
                    setAutostart(!autostartEnabled());
                    break;
                case ID_QUIT:
                    DestroyWindow(hwnd);
                    return 0;
            }
            updateIcon(false);
            return 0;
        case WM_TIMER:
            if (wp == kPollTimer) {  // touche / bouton du gestionnaire de zones
                pollEditorTriggers();
            } else if (g_haveIcon && currentState() != g_shown) {  // reglages modifies ailleurs : on met la pastille a jour
                updateIcon(false);
            }
            return 0;
        case WM_DESTROY: {
            NOTIFYICONDATAW nid = {sizeof nid};
            nid.hWnd = hwnd;
            nid.uID = 1;
            Shell_NotifyIconW(NIM_DELETE, &nid);
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    HANDLE once = CreateMutexW(nullptr, TRUE, L"Local\\OverlayVR_Tray");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;  // deja lance : l'icone est la

    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    g_exePath = path;
    g_exeDir = g_exePath.substr(0, g_exePath.find_last_of(L'\\'));
    g_manifest = g_exeDir + L"\\XR_APILAYER_OVERLAYVR_zones.json";

    g_icons[Green] = makeIcon(60, 200, 90, false);
    g_icons[Grey] = makeIcon(135, 135, 135, true);
    g_icons[Red] = makeIcon(215, 55, 55, true);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc = {};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"OverlayVRTray";
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"OverlayVR", 0, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    ovz::ensureConfigFile();
    updateIcon(true);
    SetTimer(g_hwnd, kTimer, 1500, nullptr);
    SetTimer(g_hwnd, kPollTimer, 40, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CloseHandle(once);
    return 0;
}

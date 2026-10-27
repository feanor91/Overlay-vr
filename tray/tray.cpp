// OverlayVR : application de la barre d'etat (a cote de l'horloge).
//   Clic gauche  : active / desactive les zones, tout de suite (meme jeu lance), sans droits administrateur.
//   Clic droit   : menu - lancer le gestionnaire de zones, charger / decharger la couche OpenXR (administrateur),
//                  reglages, dossier des donnees, demarrage avec Windows.
// Pastille : verte = zones actives, grise = zones coupees, rouge = couche OpenXR desactivee.
#define NOMINMAX
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <string>

#include "../common/config.h"

namespace {

constexpr UINT kTrayMsg = WM_APP + 1;
constexpr UINT_PTR kTimer = 1;
enum : UINT { ID_ZONES = 1, ID_LAYER, ID_EDITOR, ID_SETTINGS, ID_FOLDER, ID_AUTOSTART, ID_QUIT, ID_RAWJSON };
enum IconState { Green = 0, Grey = 1, Red = 2 };

const wchar_t* const kLayerKey = L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit";
const wchar_t* const kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

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
HICON makeIcon(uint8_t r, uint8_t g, uint8_t b) {
    constexpr int S = 32;
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
            const float dx = x + 0.5f - S / 2.0f, dy = y + 0.5f - S / 2.0f;
            const float d = std::sqrt(dx * dx + dy * dy);
            uint32_t argb = 0;
            if (d <= 14.5f) {
                // anneau blanc, disque colore, bord adouci
                const bool ring = d > 11.5f;
                const uint8_t cr = ring ? 255 : r, cg = ring ? 255 : g, cb = ring ? 255 : b;
                const float a = std::min(1.0f, 14.5f - d + 0.5f);
                const uint8_t A = static_cast<uint8_t>(a * 255);
                argb = (static_cast<uint32_t>(A) << 24) | (static_cast<uint32_t>(cr * a) << 16) |
                       (static_cast<uint32_t>(cg * a) << 8) | static_cast<uint32_t>(cb * a);  // alpha premultiplie
            }
            px[y * S + x] = argb;
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
void launchEditor() {
    const std::wstring exe = g_exeDir + L"\\OverlayVR_Editor.exe";
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, g_exeDir.c_str(), SW_SHOW)) <= 32)
        MessageBoxW(nullptr, L"OverlayVR_Editor.exe est introuvable à côté de cette application.", L"OverlayVR", MB_ICONWARNING);
}

void showMenu() {
    ovz::ensureConfigFile();
    const int layer = layerState();
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (zonesEnabled() ? MF_CHECKED : 0), ID_ZONES, L"Zones activées\t(clic gauche sur l'icône)");
    AppendMenuW(m, MF_STRING | (layer == 0 ? MF_CHECKED : 0) | (layer < 0 ? MF_GRAYED : 0), ID_LAYER,
                L"Couche OpenXR chargée par les jeux (administrateur)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EDITOR, L"Lancer le gestionnaire de zones (dans le casque)");
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
        case WM_TIMER:  // reglages modifies ailleurs (fichier, autre outil) : on met la pastille a jour
            if (g_haveIcon && currentState() != g_shown) updateIcon(false);
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

    g_icons[Green] = makeIcon(60, 200, 90);
    g_icons[Grey] = makeIcon(135, 135, 135);
    g_icons[Red] = makeIcon(215, 55, 55);
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

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CloseHandle(once);
    return 0;
}

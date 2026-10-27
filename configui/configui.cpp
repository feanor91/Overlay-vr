// OverlayVR : fenetre de configuration (settings.json).
//   - interrupteur general, touche clavier de bascule des zones (on appuie sur la touche pour la choisir) ;
//   - detection des joysticks / HOTAS branches et du bouton de bascule (on appuie sur le bouton pour le choisir) ;
//   - affichage (regard, bords), jeux concernes.
// La couche relit settings.json a chaud : les changements sont pris en compte sans relancer le jeu.
#define NOMINMAX
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <unknwn.h>
#include <commctrl.h>
#include <dinput.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../common/config.h"
#include "../common/keys.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

enum : int {
    ID_ENABLED = 100, ID_KEYTXT, ID_KEYSET, ID_KEYNONE, ID_JOYDEV, ID_JOYSCAN, ID_BTNTXT, ID_BTNDETECT, ID_BTNNONE,
    ID_LIVE, ID_HINT, ID_GAZE, ID_MARGIN, ID_PASSES, ID_OVERLAP, ID_HOTKEYS, ID_MODE, ID_APPS, ID_SAVE, ID_CLOSE, ID_SAVED
};
constexpr UINT_PTR kTimer = 1;

struct Dev {
    IDirectInputDevice8W* dev;
    std::string name;
    bool down[128];
};

HINSTANCE g_inst = nullptr;
HWND g_wnd = nullptr;
HFONT g_font = nullptr;
int g_dpi = 96;
IDirectInput8W* g_di = nullptr;
std::vector<Dev> g_devs;
std::vector<std::string> g_comboNames;  // nom de joystick de chaque ligne de la liste ("" : tous)
std::string g_keyText = "space";
std::string g_joyName;
int g_joyButton = 0;
bool g_captureKey = false, g_detect = false;
bool g_keyPrev[256] = {};
std::wstring g_liveShown;

int S(int v) { return MulDiv(v, g_dpi, 96); }

std::wstring W(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

std::string U(const std::wstring& w) {
    if (w.empty()) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

HWND ctl(int id) { return GetDlgItem(g_wnd, id); }

std::wstring getText(int id) {
    wchar_t buf[2048] = {};
    GetWindowTextW(ctl(id), buf, 2047);
    return buf;
}

void setText(int id, const std::wstring& t) { SetWindowTextW(ctl(id), t.c_str()); }

bool checked(int id) { return SendMessageW(ctl(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
void setChecked(int id, bool on) { SendMessageW(ctl(id), BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }

HWND make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_wnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_inst, nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    return c;
}

void label(const wchar_t* text, int x, int y, int w) { make(L"STATIC", text, SS_LEFT, x, y + 3, w, 18, 0); }
void group(const wchar_t* text, int x, int y, int w, int h) { make(L"BUTTON", text, BS_GROUPBOX, x, y, w, h, 0); }
void button(const wchar_t* text, int x, int y, int w, int id) { make(L"BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, x, y, w, 24, id); }
void edit(int x, int y, int w, int id, bool readOnly = false) {
    make(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP | (readOnly ? ES_READONLY : 0), x, y, w, 22, id, WS_EX_CLIENTEDGE);
}

// --- joysticks (DirectInput) ------------------------------------------------------------------------
BOOL CALLBACK enumJoystick(const DIDEVICEINSTANCEW* inst, void*) {
    IDirectInputDevice8W* dev = nullptr;
    if (FAILED(g_di->CreateDevice(inst->guidInstance, &dev, nullptr))) return DIENUM_CONTINUE;
    if (FAILED(dev->SetDataFormat(&c_dfDIJoystick2)) ||
        FAILED(dev->SetCooperativeLevel(g_wnd, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE))) {
        dev->Release();
        return DIENUM_CONTINUE;
    }
    Dev d = {dev, U(inst->tszProductName), {}};
    g_devs.push_back(d);
    return DIENUM_CONTINUE;
}

void closeDevices() {
    for (Dev& d : g_devs) {
        d.dev->Unacquire();
        d.dev->Release();
    }
    g_devs.clear();
}

void fillCombo() {
    HWND cb = ctl(ID_JOYDEV);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    g_comboNames.clear();
    SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Tous les joysticks"));
    g_comboNames.push_back("");
    int sel = 0;
    for (const Dev& d : g_devs) {
        SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(W(d.name).c_str()));
        g_comboNames.push_back(d.name);
        if (!g_joyName.empty() && d.name == g_joyName) sel = static_cast<int>(g_comboNames.size()) - 1;
    }
    if (!g_joyName.empty() && !sel) {  // enregistre mais pas branche pour l'instant
        SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(W(g_joyName + "  (non branché)").c_str()));
        g_comboNames.push_back(g_joyName);
        sel = static_cast<int>(g_comboNames.size()) - 1;
    }
    SendMessageW(cb, CB_SETCURSEL, sel, 0);
}

void scanDevices() {
    closeDevices();
    if (g_di) g_di->EnumDevices(DI8DEVCLASS_GAMECTRL, enumJoystick, nullptr, DIEDFL_ATTACHEDONLY);
    fillCombo();
}

void showHint(const std::wstring& t) { setText(ID_HINT, t); }

void refreshTexts() {
    const ovz::KeySpec k = ovz::parseKey(g_keyText);
    setText(ID_KEYTXT, k.vk ? W(ovz::formatKey(k)) : L"(aucune)");
    setText(ID_BTNTXT, g_joyButton > 0 ? L"Bouton " + std::to_wstring(g_joyButton) : L"(aucun)");
}

// --- chargement / enregistrement -----------------------------------------------------------------------
void loadIntoUi() {
    ovz::Config c;
    ovz::loadConfig(c);
    setChecked(ID_ENABLED, c.enabled);
    g_keyText = c.toggleKey;
    g_joyName = c.toggleJoystick;
    g_joyButton = c.toggleJoystickButton;
    setChecked(ID_GAZE, c.gazeOnly);
    wchar_t buf[32];
    swprintf_s(buf, L"%g", c.gazeMargin);
    setText(ID_MARGIN, buf);
    setText(ID_PASSES, std::to_wstring(c.edgePasses));
    swprintf_s(buf, L"%g", c.overlapCm);
    setText(ID_OVERLAP, buf);
    setChecked(ID_HOTKEYS, c.hotkeys);
    SendMessageW(ctl(ID_MODE), CB_SETCURSEL, c.mode == "all" ? 2 : c.mode == "blocklist" ? 1 : 0, 0);
    std::wstring apps;
    for (const std::string& a : c.apps) apps += W(a) + L"\r\n";
    setText(ID_APPS, apps);
    refreshTexts();
    fillCombo();
}

bool save() {
    ovz::Config c;
    ovz::loadConfig(c);  // conserve profils et decalages
    c.enabled = checked(ID_ENABLED);
    c.toggleKey = ovz::parseKey(g_keyText).vk ? g_keyText : "none";
    c.toggleJoystick = g_joyName;
    c.toggleJoystickButton = g_joyButton;
    c.gazeOnly = checked(ID_GAZE);
    c.gazeMargin = std::clamp(static_cast<float>(_wtof(getText(ID_MARGIN).c_str())), 0.0f, 90.0f);
    c.edgePasses = std::clamp(_wtoi(getText(ID_PASSES).c_str()), 1, 6);
    c.overlapCm = std::clamp(static_cast<float>(_wtof(getText(ID_OVERLAP).c_str())), 0.0f, 20.0f);
    c.hotkeys = checked(ID_HOTKEYS);
    const int mode = static_cast<int>(SendMessageW(ctl(ID_MODE), CB_GETCURSEL, 0, 0));
    c.mode = mode == 2 ? "all" : mode == 1 ? "blocklist" : "allowlist";
    c.apps.clear();
    std::wstring all = getText(ID_APPS), line;
    for (size_t i = 0; i <= all.size(); i++) {
        if (i == all.size() || all[i] == L'\n' || all[i] == L'\r') {
            const size_t a = line.find_first_not_of(L" \t"), b = line.find_last_not_of(L" \t");
            if (a != std::wstring::npos) c.apps.push_back(U(line.substr(a, b - a + 1)));
            line.clear();
        } else {
            line += all[i];
        }
    }
    return ovz::saveConfig(c);
}

// --- sondage : touches et boutons de joystick ----------------------------------------------------------
bool isModifierOrMouse(int vk) {
    return vk < 0x08 || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || (vk >= 0xA0 && vk <= 0xA5) || vk == VK_LWIN ||
           vk == VK_RWIN;
}

void stopKeyCapture() {
    g_captureKey = false;
    setText(ID_KEYSET, L"Définir…");
}

void pollKeys() {
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    for (int vk = 0x08; vk <= 0xFE; vk++) {
        const bool now = down(vk);
        if (now && !g_keyPrev[vk] && g_captureKey && !isModifierOrMouse(vk)) {
            if (vk == VK_ESCAPE) {
                stopKeyCapture();
                showHint(L"Choix de la touche annulé.");
            } else {
                ovz::KeySpec k;
                k.vk = vk;
                k.ctrl = down(VK_CONTROL);
                k.alt = down(VK_MENU);
                k.shift = down(VK_SHIFT);
                g_keyText = ovz::formatKey(k);
                stopKeyCapture();
                refreshTexts();
                showHint(L"Touche choisie : " + W(g_keyText) + L". Pense à enregistrer.");
            }
        }
        g_keyPrev[vk] = now;
    }
}

void pollJoysticks() {
    std::wstring live;
    for (Dev& d : g_devs) {
        DIJOYSTATE2 st = {};
        d.dev->Acquire();
        d.dev->Poll();
        if (FAILED(d.dev->GetDeviceState(sizeof st, &st))) {
            std::fill(std::begin(d.down), std::end(d.down), false);
            continue;
        }
        std::wstring pressed;
        for (int i = 0; i < 128; i++) {
            const bool now = (st.rgbButtons[i] & 0x80) != 0;
            if (now) pressed += (pressed.empty() ? L"" : L", ") + std::to_wstring(i + 1);
            if (now && !d.down[i] && g_detect) {
                g_detect = false;
                g_joyName = d.name;
                g_joyButton = i + 1;
                setText(ID_BTNDETECT, L"Détecter…");
                fillCombo();
                refreshTexts();
                showHint(L"Détecté : bouton " + std::to_wstring(i + 1) + L" de « " + W(d.name) + L" ». Pense à enregistrer.");
            }
            d.down[i] = now;
        }
        if (!pressed.empty()) live += W(d.name) + L" : " + pressed + L"\r\n";
    }
    if (live.empty()) live = g_devs.empty() ? L"Aucun joystick détecté (branche-le puis clique « Rescanner »)."
                                            : L"Test : appuie sur un bouton de ton HOTAS, il s'affiche ici.";
    if (live != g_liveShown) {
        g_liveShown = live;
        setText(ID_LIVE, live);
    }
}

// --- fenetre -----------------------------------------------------------------------------------------------
void build() {
    make(L"BUTTON", L"Zones activées (interrupteur général, comme l'icône de la barre d'état)", BS_AUTOCHECKBOX | WS_TABSTOP, 12, 10,
         570, 22, ID_ENABLED);

    group(L"Bascule des zones en jeu", 10, 40, 580, 235);
    label(L"Touche clavier", 24, 64, 120);
    edit(150, 64, 150, ID_KEYTXT, true);
    button(L"Définir…", 310, 63, 90, ID_KEYSET);
    button(L"Aucune", 406, 63, 80, ID_KEYNONE);
    label(L"Joystick / HOTAS", 24, 98, 120);
    make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 150, 98, 336, 200, ID_JOYDEV);
    button(L"Rescanner", 492, 97, 86, ID_JOYSCAN);
    label(L"Bouton", 24, 132, 120);
    edit(150, 132, 150, ID_BTNTXT, true);
    button(L"Détecter…", 310, 131, 90, ID_BTNDETECT);
    button(L"Aucun", 406, 131, 80, ID_BTNNONE);
    make(L"STATIC", L"", SS_LEFT | WS_BORDER, 24, 166, 552, 52, ID_LIVE);
    make(L"STATIC", L"Choisir le bouton : clique « Détecter… » puis appuie sur le bouton de ton HOTAS (il est lu sans gêner MSFS). "
         L"La touche : clique « Définir… » puis appuie sur la touche (Échap annule).",
         SS_LEFT, 24, 224, 552, 44, ID_HINT);

    group(L"Affichage", 10, 282, 580, 104);
    make(L"BUTTON", L"Afficher une zone seulement quand je la regarde", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 302, 400, 22, ID_GAZE);
    label(L"Marge du regard (°)", 24, 332, 160);
    edit(190, 332, 60, ID_MARGIN);
    label(L"Passes de bord (1 à 6)", 270, 332, 160);
    edit(436, 332, 50, ID_PASSES);
    label(L"Recouvrement (cm)", 24, 358, 160);
    edit(190, 358, 60, ID_OVERLAP);
    make(L"BUTTON", L"Recalage Ctrl+Alt+flèches", BS_AUTOCHECKBOX | WS_TABSTOP, 270, 358, 310, 22, ID_HOTKEYS);

    group(L"Jeux concernés", 10, 392, 580, 150);
    label(L"Mode", 24, 416, 120);
    HWND mode = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 150, 416, 336, 120, ID_MODE);
    SendMessageW(mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Seulement les jeux listés"));
    SendMessageW(mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Tous les jeux sauf ceux listés"));
    SendMessageW(mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Tous les jeux"));
    label(L"Applications (un nom d'exe par ligne, voir layer.log)", 24, 446, 400);
    make(L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_TABSTOP, 24, 470, 552, 62, ID_APPS,
         WS_EX_CLIENTEDGE);

    make(L"STATIC", L"", SS_LEFT, 12, 556, 380, 20, ID_SAVED);
    button(L"Enregistrer", 400, 552, 90, ID_SAVE);
    button(L"Fermer", 496, 552, 90, ID_CLOSE);
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case ID_KEYSET:
                    g_captureKey = !g_captureKey;
                    if (g_captureKey) {
                        g_detect = false;
                        setText(ID_BTNDETECT, L"Détecter…");
                        setText(ID_KEYSET, L"Annuler");
                        showHint(L"Appuie sur la touche à utiliser (avec Ctrl / Alt / Maj si tu veux), Échap pour annuler…");
                        SetFocus(g_wnd);  // sinon la barre d'espace « clique » encore le bouton
                    } else {
                        stopKeyCapture();
                    }
                    break;
                case ID_KEYNONE:
                    stopKeyCapture();
                    g_keyText = "none";
                    refreshTexts();
                    break;
                case ID_JOYSCAN:
                    scanDevices();
                    showHint(g_devs.empty() ? L"Aucun joystick détecté." : L"Joysticks détectés : " + std::to_wstring(g_devs.size()) + L".");
                    break;
                case ID_JOYDEV:
                    if (HIWORD(wp) == CBN_SELCHANGE) {
                        const int i = static_cast<int>(SendMessageW(ctl(ID_JOYDEV), CB_GETCURSEL, 0, 0));
                        if (i >= 0 && i < static_cast<int>(g_comboNames.size())) g_joyName = g_comboNames[i];
                    }
                    break;
                case ID_BTNDETECT:
                    g_detect = !g_detect;
                    if (g_detect) {
                        stopKeyCapture();
                        setText(ID_BTNDETECT, L"Annuler");
                        for (Dev& d : g_devs) std::fill(std::begin(d.down), std::end(d.down), true);  // ignore ce qui est deja enfonce
                        showHint(g_devs.empty() ? L"Aucun joystick détecté : branche-le puis clique « Rescanner »."
                                                : L"Appuie maintenant sur le bouton voulu de ton joystick / HOTAS…");
                    } else {
                        setText(ID_BTNDETECT, L"Détecter…");
                    }
                    break;
                case ID_BTNNONE:
                    g_detect = false;
                    setText(ID_BTNDETECT, L"Détecter…");
                    g_joyButton = 0;
                    refreshTexts();
                    break;
                case ID_SAVE:
                    setText(ID_SAVED, save() ? L"Enregistré : pris en compte tout de suite, sans relancer le jeu."
                                              : L"Échec de l'enregistrement de settings.json.");
                    break;
                case ID_CLOSE:
                    DestroyWindow(hwnd);
                    break;
            }
            return 0;
        case WM_TIMER:
            pollKeys();
            pollJoysticks();
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kTimer);
            closeDevices();
            if (g_di) g_di->Release();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    HANDLE once = CreateMutexW(nullptr, TRUE, L"Local\\OverlayVR_Config");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND w = FindWindowW(L"OverlayVRConfig", nullptr)) SetForegroundWindow(w);
        return 0;
    }
    SetProcessDPIAware();
    g_inst = inst;
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    g_font = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    WNDCLASSW wc = {};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"OverlayVRConfig";
    RegisterClassW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = {0, 0, S(600), S(588)};
    AdjustWindowRect(&r, style, FALSE);
    const int w = r.right - r.left, h = r.bottom - r.top;
    g_wnd = CreateWindowExW(0, wc.lpszClassName, L"OverlayVR : configuration", style, (GetSystemMetrics(SM_CXSCREEN) - w) / 2,
                            (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h, nullptr, nullptr, inst, nullptr);
    DirectInput8Create(inst, DIRECTINPUT_VERSION, IID_IDirectInput8W, reinterpret_cast<void**>(&g_di), nullptr);
    build();
    ovz::ensureConfigFile();
    loadIntoUi();
    scanDevices();
    showHint(L"Choisir le bouton : clique « Détecter… » puis appuie sur le bouton de ton HOTAS (il est lu sans gêner MSFS). "
             L"La touche : clique « Définir… » puis appuie sur la touche (Échap annule).");
    for (int vk = 0; vk < 256; vk++) g_keyPrev[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
    SetTimer(g_wnd, kTimer, 40, nullptr);
    ShowWindow(g_wnd, show);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(g_wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    CloseHandle(once);
    return 0;
}

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
    ID_LIVE, ID_HINT, ID_GAZE, ID_MARGIN, ID_PASSES, ID_OVERLAP, ID_HOTKEYS, ID_MODE, ID_APPS, ID_SAVE, ID_CLOSE, ID_SAVED,
    ID_TAB, ID_ZLIST, ID_ASSIGN, ID_ASSIGNBTN, ID_ZRELOAD, ID_PLIST, ID_PNAME, ID_PEXE, ID_PNEW, ID_PADD, ID_PDEL,
    ID_ZNAME, ID_ZRENAME
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
int g_dy = 0;                                  // decalage vertical du contenu des onglets (sous la barre d'onglets)
std::vector<HWND>* g_cur = nullptr;            // onglet en cours de construction
std::vector<HWND> g_tabs[2];                   // controles de chaque onglet
std::vector<ovz::Zone> g_zones;                // zones affichees dans l'onglet « Zones et jeux »
std::vector<ovz::Profile> g_profiles;          // jeux (profils) de settings.json

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
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y + g_dy), S(w), S(h), g_wnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_inst, nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    if (g_cur) g_cur->push_back(c);
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

// --- zones et jeux (profils) -----------------------------------------------------------------------------
std::wstring trimmed(const std::wstring& s) {
    const size_t a = s.find_first_not_of(L" \t\r\n"), b = s.find_last_not_of(L" \t\r\n");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

std::wstring joinApps(const std::vector<std::string>& v) {
    std::wstring s;
    for (const std::string& a : v) s += (s.empty() ? L"" : L", ") + W(a);
    return s;
}

std::vector<std::string> splitApps(const std::wstring& text) {
    std::vector<std::string> out;
    std::wstring cur;
    for (size_t i = 0; i <= text.size(); i++) {
        if (i == text.size() || text[i] == L',' || text[i] == L';' || text[i] == L'\n') {
            const std::wstring t = trimmed(cur);
            if (!t.empty()) out.push_back(U(t));
            cur.clear();
        } else {
            cur += text[i];
        }
    }
    return out;
}

void say(const std::wstring& t) { setText(ID_SAVED, t); }

void refreshProfiles() {
    ovz::Config c;
    ovz::loadConfig(c);
    g_profiles = c.profiles;
    SendMessageW(ctl(ID_PLIST), LB_RESETCONTENT, 0, 0);
    SendMessageW(ctl(ID_ASSIGN), CB_RESETCONTENT, 0, 0);
    SendMessageW(ctl(ID_ASSIGN), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Tous les jeux"));
    for (const ovz::Profile& p : g_profiles) {
        const std::wstring line = W(p.name) + L"   —   " + joinApps(p.apps);
        SendMessageW(ctl(ID_PLIST), LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
        SendMessageW(ctl(ID_ASSIGN), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(W(p.name).c_str()));
    }
    SendMessageW(ctl(ID_ASSIGN), CB_SETCURSEL, g_profiles.empty() ? 0 : 1, 0);
}

void refreshZones() {
    g_zones.clear();
    ovz::loadZones(ovz::zonesPath(), g_zones);
    HWND lv = ctl(ID_ZLIST);
    ListView_DeleteAllItems(lv);
    for (size_t i = 0; i < g_zones.size(); i++) {
        const ovz::Zone& z = g_zones[i];
        std::wstring name = W(z.name), type = z.type == ovz::ZoneType::Plane ? L"plan" : L"volume",
                     game = z.profile.empty() ? L"Tous les jeux" : W(z.profile);
        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = &name[0];
        ListView_InsertItem(lv, &item);
        ListView_SetItemText(lv, static_cast<int>(i), 1, &type[0]);
        ListView_SetItemText(lv, static_cast<int>(i), 2, &game[0]);
    }
}

// Affecte les zones selectionnees au jeu choisi. On relit zones.json juste avant d'ecrire, par les noms.
void assignZones() {
    const int sel = static_cast<int>(SendMessageW(ctl(ID_ASSIGN), CB_GETCURSEL, 0, 0));
    const std::string target = sel > 0 && sel - 1 < static_cast<int>(g_profiles.size()) ? g_profiles[sel - 1].name : "";
    std::vector<std::string> names;
    HWND lv = ctl(ID_ZLIST);
    for (int i = ListView_GetNextItem(lv, -1, LVNI_SELECTED); i >= 0; i = ListView_GetNextItem(lv, i, LVNI_SELECTED))
        if (i < static_cast<int>(g_zones.size())) names.push_back(g_zones[i].name);
    if (names.empty()) {
        say(L"Sélectionne d'abord une ou plusieurs zones dans la liste (Ctrl / Maj pour en prendre plusieurs).");
        return;
    }
    std::vector<ovz::Zone> fresh;
    if (!ovz::loadZones(ovz::zonesPath(), fresh)) {
        say(L"Impossible de lire zones.json.");
        return;
    }
    int changed = 0;
    for (ovz::Zone& z : fresh)
        if (std::find(names.begin(), names.end(), z.name) != names.end()) {
            z.profile = target;
            changed++;
        }
    if (!ovz::saveZones(ovz::zonesPath(), fresh)) {
        say(L"Échec de l'enregistrement de zones.json.");
        return;
    }
    refreshZones();
    say(std::to_wstring(changed) + L" zone(s) affectée(s) à « " + (target.empty() ? L"Tous les jeux" : W(target)) +
        L" » : pris en compte tout de suite.");
}

// Renomme / retire un profil dans zones.json (les zones qui l'utilisent suivent).
void retargetZones(const std::string& from, const std::string& to) {
    std::vector<ovz::Zone> fresh;
    if (!ovz::loadZones(ovz::zonesPath(), fresh)) return;
    bool changed = false;
    for (ovz::Zone& z : fresh)
        if (z.profile == from) {
            z.profile = to;
            changed = true;
        }
    if (changed) ovz::saveZones(ovz::zonesPath(), fresh);
}

// Une seule zone selectionnee : son nom passe dans le champ de renommage.
void zoneSelectionChanged() {
    HWND lv = ctl(ID_ZLIST);
    if (ListView_GetSelectedCount(lv) != 1) return;
    const int i = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
    if (i >= 0 && i < static_cast<int>(g_zones.size())) setText(ID_ZNAME, W(g_zones[i].name));
}

void renameZone() {
    HWND lv = ctl(ID_ZLIST);
    if (ListView_GetSelectedCount(lv) != 1) {
        say(L"Sélectionne une seule zone dans la liste pour la renommer.");
        return;
    }
    const int sel = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
    const std::string name = U(trimmed(getText(ID_ZNAME)));
    if (name.empty() || sel < 0 || sel >= static_cast<int>(g_zones.size())) {
        say(L"Donne un nom à la zone.");
        return;
    }
    const std::string old = g_zones[sel].name;
    std::vector<ovz::Zone> fresh;
    if (!ovz::loadZones(ovz::zonesPath(), fresh)) {
        say(L"Impossible de lire zones.json.");
        return;
    }
    int target = sel < static_cast<int>(fresh.size()) && fresh[sel].name == old ? sel : -1;
    for (int i = 0; i < static_cast<int>(fresh.size()); i++) {
        if (target < 0 && fresh[i].name == old) target = i;
        if (i != target && fresh[i].name == name && name != old) {
            say(L"Une autre zone porte déjà ce nom.");
            return;
        }
    }
    if (target < 0) {
        say(L"Zone introuvable : clique « Recharger ».");
        return;
    }
    fresh[target].name = name;
    if (!ovz::saveZones(ovz::zonesPath(), fresh)) {
        say(L"Échec de l'enregistrement de zones.json.");
        return;
    }
    refreshZones();
    ListView_SetItemState(lv, target, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    say(L"Zone « " + W(old) + L" » renommée en « " + W(name) + L" ».");
}

void profileSelected() {
    const int i = static_cast<int>(SendMessageW(ctl(ID_PLIST), LB_GETCURSEL, 0, 0));
    if (i < 0 || i >= static_cast<int>(g_profiles.size())) return;
    setText(ID_PNAME, W(g_profiles[i].name));
    setText(ID_PEXE, joinApps(g_profiles[i].apps));
}

void saveProfile() {
    const std::string name = U(trimmed(getText(ID_PNAME)));
    const std::vector<std::string> apps = splitApps(getText(ID_PEXE));
    if (name.empty()) {
        say(L"Donne un nom au jeu (ex. MSFS).");
        return;
    }
    ovz::Config c;
    ovz::loadConfig(c);
    int sel = static_cast<int>(SendMessageW(ctl(ID_PLIST), LB_GETCURSEL, 0, 0));
    if (sel >= static_cast<int>(c.profiles.size())) sel = -1;
    for (int i = 0; i < static_cast<int>(c.profiles.size()); i++)
        if (i != sel && c.profiles[i].name == name) {
            say(L"Un jeu porte déjà ce nom.");
            return;
        }
    if (sel >= 0) {
        const std::string old = c.profiles[sel].name;
        c.profiles[sel].name = name;
        c.profiles[sel].apps = apps;
        if (old != name) retargetZones(old, name);
    } else {
        ovz::Profile p;
        p.name = name;
        p.apps = apps;
        c.profiles.push_back(p);
        sel = static_cast<int>(c.profiles.size()) - 1;
    }
    if (!ovz::saveConfig(c)) {
        say(L"Échec de l'enregistrement de settings.json.");
        return;
    }
    refreshProfiles();
    refreshZones();
    SendMessageW(ctl(ID_PLIST), LB_SETCURSEL, sel, 0);
    say(L"Jeu « " + W(name) + L" » enregistré.");
}

void deleteProfile() {
    ovz::Config c;
    ovz::loadConfig(c);
    const int sel = static_cast<int>(SendMessageW(ctl(ID_PLIST), LB_GETCURSEL, 0, 0));
    if (sel < 0 || sel >= static_cast<int>(c.profiles.size())) {
        say(L"Sélectionne d'abord un jeu dans la liste.");
        return;
    }
    const std::string name = c.profiles[sel].name;
    if (MessageBoxW(g_wnd, (L"Supprimer le jeu « " + W(name) + L" » ?\nSes zones deviendront visibles dans tous les jeux.").c_str(),
                    L"OverlayVR", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    c.profiles.erase(c.profiles.begin() + sel);
    retargetZones(name, "");
    ovz::saveConfig(c);
    setText(ID_PNAME, L"");
    setText(ID_PEXE, L"");
    refreshProfiles();
    refreshZones();
    say(L"Jeu « " + W(name) + L" » supprimé.");
}

void showTab(int tab) {
    for (int t = 0; t < 2; t++)
        for (HWND h : g_tabs[t]) ShowWindow(h, t == tab ? SW_SHOW : SW_HIDE);
    if (tab == 1) {
        refreshProfiles();
        refreshZones();
    }
}

// --- fenetre -----------------------------------------------------------------------------------------------
void build() {
    HWND tab = make(L"SysTabControl32", L"", WS_TABSTOP, 6, 4, 588, 28, ID_TAB);
    TCITEMW ti = {};
    ti.mask = TCIF_TEXT;
    ti.pszText = const_cast<wchar_t*>(L"Général");
    TabCtrl_InsertItem(tab, 0, &ti);
    ti.pszText = const_cast<wchar_t*>(L"Zones et jeux");
    TabCtrl_InsertItem(tab, 1, &ti);
    g_dy = 34;
    g_cur = &g_tabs[0];

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

    // --- onglet « Zones et jeux » ---
    g_cur = &g_tabs[1];
    group(L"Zones tracées dans le casque", 10, 10, 580, 270);
    make(L"SysListView32", L"", LVS_REPORT | LVS_SHOWSELALWAYS | WS_TABSTOP, 24, 34, 552, 140, ID_ZLIST, WS_EX_CLIENTEDGE);
    HWND lv = ctl(ID_ZLIST);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT);
    const wchar_t* cols[] = {L"Zone", L"Type", L"Jeu"};
    const int widths[] = {200, 90, 240};
    for (int i = 0; i < 3; i++) {
        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = const_cast<wchar_t*>(cols[i]);
        col.cx = S(widths[i]);
        ListView_InsertColumn(lv, i, &col);
    }
    label(L"Nom de la zone", 24, 182, 110);
    edit(140, 182, 260, ID_ZNAME);
    button(L"Renommer", 410, 181, 166, ID_ZRENAME);
    label(L"Affecter la sélection à :", 24, 214, 170);
    make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 200, 214, 200, 200, ID_ASSIGN);
    button(L"Affecter", 410, 213, 80, ID_ASSIGNBTN);
    button(L"Recharger", 496, 213, 80, ID_ZRELOAD);
    make(L"STATIC", L"Une zone « Tous les jeux » apparaît dans chaque jeu autorisé ; une zone affectée à un jeu n'apparaît que dans "
         L"celui-ci. Ferme l'éditeur du casque avant d'affecter.", SS_LEFT, 24, 242, 552, 34, 0);

    group(L"Jeux", 10, 288, 580, 236);
    make(L"LISTBOX", L"", LBS_NOTIFY | WS_VSCROLL | WS_TABSTOP, 24, 312, 552, 90, ID_PLIST, WS_EX_CLIENTEDGE);
    label(L"Nom du jeu", 24, 412, 160);
    edit(190, 412, 386, ID_PNAME);
    label(L"Exécutables (séparés par des virgules)", 24, 440, 250);
    edit(280, 440, 296, ID_PEXE);
    button(L"Nouveau", 24, 472, 90, ID_PNEW);
    button(L"Ajouter / modifier", 120, 472, 130, ID_PADD);
    button(L"Supprimer", 256, 472, 90, ID_PDEL);
    make(L"STATIC", L"Nom exact de l'exe : ligne « couche chargée dans … » de layer.log. Un jeu qui a un profil est autorisé "
         L"automatiquement.", SS_LEFT, 24, 500, 552, 20, 0);

    g_cur = nullptr;
    g_dy = 0;
    make(L"STATIC", L"", SS_LEFT, 12, 592, 380, 20, ID_SAVED);
    button(L"Enregistrer", 400, 588, 90, ID_SAVE);
    button(L"Fermer", 496, 588, 90, ID_CLOSE);
    showTab(0);
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
                case ID_ZRENAME:
                    renameZone();
                    break;
                case ID_ASSIGNBTN:
                    assignZones();
                    break;
                case ID_ZRELOAD:
                    refreshProfiles();
                    refreshZones();
                    break;
                case ID_PLIST:
                    if (HIWORD(wp) == LBN_SELCHANGE) profileSelected();
                    break;
                case ID_PNEW:
                    SendMessageW(ctl(ID_PLIST), LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
                    setText(ID_PNAME, L"");
                    setText(ID_PEXE, L"");
                    break;
                case ID_PADD:
                    saveProfile();
                    break;
                case ID_PDEL:
                    deleteProfile();
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
        case WM_NOTIFY:
            if (reinterpret_cast<NMHDR*>(lp)->idFrom == ID_ZLIST && reinterpret_cast<NMHDR*>(lp)->code == LVN_ITEMCHANGED) {
                const NMLISTVIEW* nm = reinterpret_cast<NMLISTVIEW*>(lp);
                if ((nm->uChanged & LVIF_STATE) && ((nm->uNewState ^ nm->uOldState) & LVIS_SELECTED)) zoneSelectionChanged();
            }
            if (reinterpret_cast<NMHDR*>(lp)->idFrom == ID_TAB && reinterpret_cast<NMHDR*>(lp)->code == TCN_SELCHANGE)
                showTab(TabCtrl_GetCurSel(ctl(ID_TAB)));
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
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_STANDARD_CLASSES | ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES};
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
    RECT r = {0, 0, S(600), S(624)};
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

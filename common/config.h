// Reglages utilisateur de la couche : interrupteur general et liste des jeux concernes.
// Fichier : %APPDATA%\OverlayVR\settings.json (relu a chaud par la couche, sans redemarrer le jeu).
#pragma once

#include "zones.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace ovz {

// Un jeu (ou une famille d'exe) : ses zones propres, et son propre recalage (le repere varie d'une session a l'autre).
struct Profile {
    std::string name;
    std::vector<std::string> apps;  // noms d'exe (voir layer.log) ou noms d'application OpenXR
    float offset[3] = {0, 0, 0};
    float offsetYawDeg = 0;
};

struct Config {
    bool enabled = true;                 // interrupteur general
    std::string mode = "allowlist";      // "all" | "allowlist" (seulement apps) | "blocklist" (tous sauf apps)
    std::vector<std::string> apps;       // jeux autorises en plus des profils (mode allowlist) ou exclus (blocklist)
    std::vector<Profile> profiles = {{"MSFS", {"FlightSimulator2024.exe", "FlightSimulator.exe"}, {0, 0, 0}, 0},
                                     {"Assetto Corsa", {"acs.exe", "AssettoCorsa.exe"}, {0, 0, 0}, 0}};
    bool gazeOnly = true;      // une zone n'apparait que si on la regarde (direction de la tete)
    float gazeMargin = 8.0f;   // degres : le centre du regard doit viser la zone a cette marge pres (hysteresis +4 une fois affichee)
    float offset[3] = {0, 0, 0};  // decalage des zones pour les jeux SANS profil (metres), regle en jeu au clavier
    float offsetYawDeg = 0;       // rotation correspondante (degres) autour du centre des zones
    bool hotkeys = true;          // Ctrl+Alt+fleches... pour recaler les zones en jeu
    std::string toggleKey = "space";      // touche clavier qui active / desactive les zones ("none" : aucune), ex. "ctrl+f9"
    std::string toggleJoystick;           // morceau du nom du joystick / HOTAS ("" : n'importe lequel)
    std::string editorKey = "ctrl+alt+e";  // touche qui lance le gestionnaire de zones (application de la barre d'etat) ; "none" : aucune
    std::string editorJoystick;           // morceau du nom du joystick / HOTAS pour lancer le gestionnaire ("" : n'importe lequel)
    int editorJoystickButton = 0;         // numero du bouton (comme dans joy.cpl, a partir de 1) ; 0 : aucun
    int toggleJoystickButton = 0;         // numero du bouton (comme dans joy.cpl, a partir de 1) ; 0 : aucun
    int edgePasses = 1;        // superpositions de chaque face (1 a 6). Plus de passes = bords plus propres mais plus de couches a composer (FPS)
    float overlapCm = 3.0f;    // les faces d'un volume se recouvrent de ce qu'il faut pour cacher les raccords
};

inline std::filesystem::path configPath() { return dataDir() / "settings.json"; }

inline bool loadConfig(Config& out) {
    std::ifstream f(configPath());
    if (!f) return false;
    try {
        auto j = nlohmann::json::parse(f);
        Config c;
        c.enabled = j.value("enabled", true);
        c.mode = j.value("mode", "allowlist");
        if (j.contains("apps")) c.apps = j.at("apps").get<std::vector<std::string>>();
        c.gazeOnly = j.value("gaze_only", c.gazeOnly);
        c.gazeMargin = j.value("gaze_margin", c.gazeMargin);
        c.overlapCm = j.value("edge_overlap_cm", c.overlapCm);
        if (j.contains("offset") && j.at("offset").size() == 3)
            for (int i = 0; i < 3; i++) c.offset[i] = j.at("offset").at(i).get<float>();
        c.offsetYawDeg = j.value("offset_yaw_deg", c.offsetYawDeg);
        c.hotkeys = j.value("hotkeys", c.hotkeys);
        c.toggleKey = j.value("toggle_key", c.toggleKey);
        c.toggleJoystick = j.value("toggle_joystick", c.toggleJoystick);
        c.toggleJoystickButton = j.value("toggle_joystick_button", c.toggleJoystickButton);
        c.editorKey = j.value("editor_key", c.editorKey);
        c.editorJoystick = j.value("editor_joystick", c.editorJoystick);
        c.editorJoystickButton = j.value("editor_joystick_button", c.editorJoystickButton);
        c.edgePasses = std::clamp(j.value("edge_passes", c.edgePasses), 1, 6);
        if (j.contains("profiles")) {
            c.profiles.clear();
            for (auto& e : j.at("profiles")) {
                Profile p;
                p.name = e.value("name", "jeu");
                if (e.contains("apps")) p.apps = e.at("apps").get<std::vector<std::string>>();
                if (e.contains("offset") && e.at("offset").size() == 3)
                    for (int i = 0; i < 3; i++) p.offset[i] = e.at("offset").at(i).get<float>();
                p.offsetYawDeg = e.value("offset_yaw_deg", 0.0f);
                c.profiles.push_back(p);
            }
        } else if (!c.profiles.empty()) {
            // Ancien fichier (sans profils) : le recalage global fait en jeu passe au premier profil (MSFS).
            for (int i = 0; i < 3; i++) {
                c.profiles[0].offset[i] = c.offset[i];
                c.offset[i] = 0;
            }
            c.profiles[0].offsetYawDeg = c.offsetYawDeg;
            c.offsetYawDeg = 0;
        }
        out = std::move(c);
        return true;
    } catch (...) {
        return false;
    }
}

inline bool saveConfig(const Config& c) {
    std::error_code ec;
    std::filesystem::create_directories(dataDir(), ec);
    std::ofstream f(configPath());
    if (!f) return false;
    nlohmann::json profiles = nlohmann::json::array();
    for (const Profile& p : c.profiles)
        profiles.push_back({{"name", p.name},
                            {"apps", p.apps},
                            {"offset", {p.offset[0], p.offset[1], p.offset[2]}},
                            {"offset_yaw_deg", p.offsetYawDeg}});
    f << nlohmann::json{{"_aide", "mode : all (tous les jeux), allowlist (seulement les jeux de 'apps'), blocklist (tous sauf 'apps'). "
                                  "'apps' : nom de l'exe (voir layer.log) ou nom de l'application OpenXR. "
                                  "profiles : un profil par jeu (nom + exe) ; une zone peut etre attachee a un profil, elle n'apparait alors que dans ce jeu. "
                                  "gaze_only : les zones n'apparaissent que quand on les regarde (gaze_margin = tolerance en degres : le centre du regard doit viser la zone). "
                                  "edge_overlap_cm : recouvrement des faces, cache les lignes aux raccords. "
                                  "edge_passes : superpositions de chaque face (1 a 6), efface les liserés roses du contour. "
                                  "hotkeys / offset : recalage des zones en jeu (Ctrl+Alt+fleches, voir le README). "
                                  "toggle_key : touche qui active / desactive les zones en jeu (space, f9, ctrl+f9, none...). "
                                  "toggle_joystick_button : numero du bouton du joystick / HOTAS (numerotation de joy.cpl, a partir de 1 ; 0 : aucun) ; "
                                  "toggle_joystick : morceau du nom du peripherique (vide : n'importe lequel, voir layer.log). "
                                  "editor_key / editor_joystick_button / editor_joystick : touche ou bouton de HOTAS qui lance le gestionnaire de zones "
                                  "(application de la barre d'etat lancee, sans droits administrateur)."},
                        {"enabled", c.enabled},
                        {"mode", c.mode},
                        {"apps", c.apps},
                        {"profiles", profiles},
                        {"gaze_only", c.gazeOnly},
                        {"gaze_margin", c.gazeMargin},
                        {"edge_overlap_cm", c.overlapCm},
                        {"edge_passes", c.edgePasses},
                        {"hotkeys", c.hotkeys},
                        {"toggle_key", c.toggleKey},
                        {"toggle_joystick", c.toggleJoystick},
                        {"toggle_joystick_button", c.toggleJoystickButton},
                        {"editor_key", c.editorKey},
                        {"editor_joystick", c.editorJoystick},
                        {"editor_joystick_button", c.editorJoystickButton},
                        {"offset", {c.offset[0], c.offset[1], c.offset[2]}},
                        {"offset_yaw_deg", c.offsetYawDeg}}
             .dump(2);
    return true;
}

inline void ensureConfigFile() {
    if (!std::filesystem::exists(configPath())) saveConfig(Config{});
}

inline std::string lowered(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return s;
}

inline std::string stripExe(std::string s) {
    s = lowered(s);
    if (s.size() > 4 && s.compare(s.size() - 4, 4, ".exe") == 0) s.resize(s.size() - 4);
    return s;
}

inline bool matchesApp(const std::vector<std::string>& list, const std::string& exe, const std::string& appName) {
    for (const std::string& a : list)
        if (stripExe(a) == stripExe(exe) || stripExe(a) == stripExe(appName)) return true;
    return false;
}

// Indice du profil dont la liste d'applications contient ce jeu, ou -1.
inline int currentProfile(const Config& c, const std::string& exe, const std::string& appName) {
    for (size_t i = 0; i < c.profiles.size(); i++)
        if (matchesApp(c.profiles[i].apps, exe, appName)) return static_cast<int>(i);
    return -1;
}

// Vrai si la couche doit dessiner dans ce jeu : un jeu qui a un profil est autorise (sauf liste d'exclusion).
inline bool appAllowed(const Config& c, const std::string& exe, const std::string& appName) {
    if (!c.enabled) return false;
    if (c.mode == "all") return true;
    const bool listed = matchesApp(c.apps, exe, appName);
    if (c.mode == "blocklist") return !listed;
    return listed || currentProfile(c, exe, appName) >= 0;
}

// Decalage global : translation + rotation (degres) autour du centre moyen des zones. Meme calcul dans l'editeur et la couche.
inline void applyOffset(std::vector<Zone>& zones, const float offset[3], float yawDeg) {
    if (zones.empty()) return;
    float cx = 0, cz = 0;
    for (const Zone& z : zones) {
        cx += z.pos[0];
        cz += z.pos[2];
    }
    cx /= static_cast<float>(zones.size());
    cz /= static_cast<float>(zones.size());
    const float a = yawDeg * 3.14159265f / 180.0f, c = std::cos(a), sn = std::sin(a);
    for (Zone& z : zones) {
        const float dx = z.pos[0] - cx, dz = z.pos[2] - cz;
        z.pos[0] = cx + dx * c + dz * sn + offset[0];
        z.pos[1] += offset[1];
        z.pos[2] = cz - dx * sn + dz * c + offset[2];
        z.yaw += a;
    }
}

}  // namespace ovz

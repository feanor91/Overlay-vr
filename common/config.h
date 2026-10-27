// Reglages utilisateur de la couche : interrupteur general et liste des jeux concernes.
// Fichier : %APPDATA%\OverlayVR\settings.json (relu a chaud par la couche, sans redemarrer le jeu).
#pragma once

#include "zones.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace ovz {

struct Config {
    bool enabled = true;                 // interrupteur general
    std::string mode = "allowlist";      // "all" | "allowlist" (seulement apps) | "blocklist" (tous sauf apps)
    std::vector<std::string> apps = {"FlightSimulator2024.exe", "FlightSimulator.exe"};
    bool gazeOnly = true;      // une zone n'apparait que si on la regarde (direction de la tete)
    float gazeMargin = 8.0f;   // degres : le centre du regard doit viser la zone a cette marge pres (hysteresis +4 une fois affichee)
    float offset[3] = {0, 0, 0};  // decalage global des zones (metres, repere de la piece), regle en jeu au clavier
    float offsetYawDeg = 0;       // rotation globale (degres) autour du centre des zones
    bool hotkeys = true;          // Ctrl+Alt+fleches... pour recaler les zones en jeu
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
        c.edgePasses = std::clamp(j.value("edge_passes", c.edgePasses), 1, 6);
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
    f << nlohmann::json{{"_aide", "mode : all (tous les jeux), allowlist (seulement les jeux de 'apps'), blocklist (tous sauf 'apps'). "
                                  "'apps' : nom de l'exe (voir layer.log) ou nom de l'application OpenXR. "
                                  "gaze_only : les zones n'apparaissent que quand on les regarde (gaze_margin = tolerance en degres : le centre du regard doit viser la zone). "
                                  "edge_overlap_cm : recouvrement des faces, cache les lignes aux raccords. "
                                  "edge_passes : superpositions de chaque face (1 a 6), efface les liserés roses du contour. "
                                  "hotkeys / offset : recalage des zones en jeu (Ctrl+Alt+fleches, voir le README)."},
                        {"enabled", c.enabled},
                        {"mode", c.mode},
                        {"apps", c.apps},
                        {"gaze_only", c.gazeOnly},
                        {"gaze_margin", c.gazeMargin},
                        {"edge_overlap_cm", c.overlapCm},
                        {"edge_passes", c.edgePasses},
                        {"hotkeys", c.hotkeys},
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

// Vrai si la couche doit dessiner dans ce jeu.
inline bool appAllowed(const Config& c, const std::string& exe, const std::string& appName) {
    if (!c.enabled) return false;
    if (c.mode == "all") return true;
    auto strip = [](std::string s) {
        s = lowered(s);
        if (s.size() > 4 && s.compare(s.size() - 4, 4, ".exe") == 0) s.resize(s.size() - 4);
        return s;
    };
    bool listed = false;
    for (const std::string& a : c.apps)
        if (strip(a) == strip(exe) || strip(a) == strip(appName)) listed = true;
    return c.mode == "blocklist" ? !listed : listed;
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

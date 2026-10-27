// Reglages utilisateur de la couche : interrupteur general et liste des jeux concernes.
// Fichier : %APPDATA%\OverlayVR\settings.json (relu a chaud par la couche, sans redemarrer le jeu).
#pragma once

#include "zones.h"

#include <algorithm>
#include <cctype>

namespace ovz {

struct Config {
    bool enabled = true;                 // interrupteur general
    std::string mode = "allowlist";      // "all" | "allowlist" (seulement apps) | "blocklist" (tous sauf apps)
    std::vector<std::string> apps = {"FlightSimulator2024.exe", "FlightSimulator.exe"};
    bool gazeOnly = true;      // une zone n'apparait que si on la regarde (direction de la tete)
    float gazeAngle = 25.0f;   // degres entre le centre du regard et le bord de la zone
    float overlapCm = 2.0f;    // les faces d'un volume se recouvrent de ce qu'il faut pour cacher les raccords
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
        c.gazeAngle = j.value("gaze_angle", c.gazeAngle);
        c.overlapCm = j.value("edge_overlap_cm", c.overlapCm);
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
                                  "gaze_only : les zones n'apparaissent que quand on les regarde (gaze_angle = degres autour du regard). "
                                  "edge_overlap_cm : recouvrement des faces, cache les lignes aux raccords."},
                        {"enabled", c.enabled},
                        {"mode", c.mode},
                        {"apps", c.apps},
                        {"gaze_only", c.gazeOnly},
                        {"gaze_angle", c.gazeAngle},
                        {"edge_overlap_cm", c.overlapCm}}
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

}  // namespace ovz

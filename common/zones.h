// Modele de zones partage par l'editeur et la couche OpenXR.
// Coordonnees en metres dans l'espace STAGE d'OpenXR (origine au sol, repere de la piece).
#pragma once

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace ovz {

enum class ZoneType { Plane, Box };

struct Zone {
    std::string name;
    ZoneType type = ZoneType::Box;
    float pos[3] = {0, 0, 0};          // centre
    float yaw = 0;                     // rotation autour de Y, radians
    float size[3] = {0.3f, 0.3f, 0.3f};  // largeur X, hauteur Y, profondeur Z (hauteur ignoree pour un plan)
    float look = 0;                    // > 0 : visible seulement si le regard descend d'au moins look degres
    bool enabled = true;
    std::string object;                // objet physique auquel la zone appartient ("" = fixe dans la piece)
    std::string profile;               // jeu auquel la zone appartient ("" = tous les jeux)
};

inline std::filesystem::path dataDir() {
    const char* appdata = std::getenv("APPDATA");
    std::filesystem::path base = appdata ? appdata : ".";
    return base / "OverlayVR";
}

inline std::filesystem::path zonesPath() { return dataDir() / "zones.json"; }

inline bool loadZones(const std::filesystem::path& path, std::vector<Zone>& out) {
    std::ifstream f(path);
    if (!f) return false;
    try {
        auto j = nlohmann::json::parse(f);
        std::vector<Zone> zones;
        for (auto& e : j.at("zones")) {
            Zone z;
            z.name = e.value("name", "zone");
            z.type = e.value("type", "box") == "plane" ? ZoneType::Plane : ZoneType::Box;
            auto pos = e.at("pos");
            auto size = e.at("size");
            for (int i = 0; i < 3; i++) {
                z.pos[i] = pos.at(i).get<float>();
                z.size[i] = size.at(i).get<float>();
            }
            z.yaw = e.value("yaw", 0.0f);
            z.look = e.value("look", 0.0f);
            z.enabled = e.value("enabled", true);
            z.object = e.value("object", "");
            z.profile = e.value("profile", "");
            zones.push_back(z);
        }
        out = std::move(zones);
        return true;
    } catch (...) {
        return false;  // fichier en cours d'ecriture ou invalide : on garde l'ancien contenu
    }
}

inline bool saveZones(const std::filesystem::path& path, const std::vector<Zone>& zones) {
    nlohmann::json arr = nlohmann::json::array();
    for (auto& z : zones) {
        arr.push_back({{"name", z.name},
                       {"type", z.type == ZoneType::Plane ? "plane" : "box"},
                       {"pos", {z.pos[0], z.pos[1], z.pos[2]}},
                       {"size", {z.size[0], z.size[1], z.size[2]}},
                       {"yaw", z.yaw},
                       {"look", z.look},
                       {"enabled", z.enabled},
                       {"object", z.object},
                       {"profile", z.profile}});
    }
    nlohmann::json root = {{"version", 1}, {"space", "stage"}, {"zones", arr}};
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp);
        if (!f) return false;
        f << root.dump(2);
    }
    std::filesystem::rename(tmp, path, ec);  // remplace atomiquement l'ancien fichier
    return !ec;
}

}  // namespace ovz

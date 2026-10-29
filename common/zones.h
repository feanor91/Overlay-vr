// Modele de zones partage par l'editeur et la couche OpenXR.
// Coordonnees en metres dans l'espace STAGE d'OpenXR (origine au sol, repere de la piece).
#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace ovz {

// Plane : rectangle plat. Box : boite. Poly : forme libre = polygone (sommets `pts`) extrude sur la hauteur size[1].
enum class ZoneType { Plane, Box, Poly };

struct Zone {
    std::string name;
    ZoneType type = ZoneType::Box;
    float pos[3] = {0, 0, 0};          // centre
    float yaw = 0;                     // rotation autour de Y, radians
    float size[3] = {0.3f, 0.3f, 0.3f};  // largeur X, hauteur Y, profondeur Z (hauteur ignoree pour un plan)
    float look = 0;                    // > 0 : visible seulement si le regard descend d'au moins look degres
    bool enabled = true;
    std::string profile;               // jeu auquel la zone appartient ("" = tous les jeux)
    // Poly : sommets (x, z) en metres, relatifs au centre `pos`, dans le repere local (avant la rotation `yaw`). size[0] et
    // size[2] sont la boite englobante de ces sommets (centree sur pos) ; size[1] est la hauteur.
    std::vector<std::array<float, 2>> pts;
};

// Ancre : deux reperes physiques fixes (ex. deux coins du bureau) releves avec les manettes quand les zones etaient bien
// alignees. On garde leur milieu et le cap du segment qui les relie, dans le repere des zones. A chaque session le repere
// STAGE du runtime peut avoir bouge (recentrage Meta...) : en reposant les manettes sur les memes reperes on obtient la
// meme paire de points dans le nouveau repere, et on deplace toutes les zones d'un bloc (rotation + translation sur le plan
// horizontal, la hauteur ne change pas).
struct Anchor {
    bool valid = false;
    float pos[3] = {0, 0, 0};
    float yaw = 0;
};

// Anchor a partir de deux points (x, z) ; faux s'ils sont trop proches (< 30 cm) pour donner un cap fiable.
inline bool anchorFromPoints(const float l[3], const float r[3], Anchor& out) {
    const float dx = r[0] - l[0], dz = r[2] - l[2];
    if (dx * dx + dz * dz < 0.09f) return false;
    out.valid = true;
    out.pos[0] = (l[0] + r[0]) / 2;
    out.pos[1] = (l[1] + r[1]) / 2;
    out.pos[2] = (l[2] + r[2]) / 2;
    out.yaw = std::atan2(-dx, -dz);
    return true;
}

// Deplace les zones comme le fait passer l'ancre `from` a la pose de tete (toPos, toYaw).
inline void rigidMove(std::vector<Zone>& zones, const Anchor& from, const float toPos[3], float toYaw) {
    const float a = toYaw - from.yaw, c = std::cos(a), s = std::sin(a);
    for (Zone& z : zones) {
        const float dx = z.pos[0] - from.pos[0], dz = z.pos[2] - from.pos[2];
        z.pos[0] = toPos[0] + dx * c + dz * s;
        z.pos[2] = toPos[2] - dx * s + dz * c;
        z.yaw += a;
    }
}

inline std::filesystem::path dataDir() {
    const char* appdata = std::getenv("APPDATA");
    std::filesystem::path base = appdata ? appdata : ".";
    return base / "OverlayVR";
}

inline std::filesystem::path zonesPath() { return dataDir() / "zones.json"; }

inline bool loadZones(const std::filesystem::path& path, std::vector<Zone>& out, Anchor* anchor = nullptr) {
    std::ifstream f(path);
    if (!f) return false;
    try {
        auto j = nlohmann::json::parse(f);
        std::vector<Zone> zones;
        for (auto& e : j.at("zones")) {
            Zone z;
            z.name = e.value("name", "zone");
            const std::string type = e.value("type", "box");
            z.type = type == "plane" ? ZoneType::Plane : type == "poly" ? ZoneType::Poly : ZoneType::Box;
            auto pos = e.at("pos");
            auto size = e.at("size");
            for (int i = 0; i < 3; i++) {
                z.pos[i] = pos.at(i).get<float>();
                z.size[i] = size.at(i).get<float>();
            }
            z.yaw = e.value("yaw", 0.0f);
            z.look = e.value("look", 0.0f);
            z.enabled = e.value("enabled", true);
            z.profile = e.value("profile", "");
            if (z.type == ZoneType::Poly) {
                for (auto& p : e.at("points")) z.pts.push_back({p.at(0).get<float>(), p.at(1).get<float>()});
                if (z.pts.size() < 3) continue;  // forme invalide : ignoree
            }
            zones.push_back(z);
        }
        out = std::move(zones);
        if (anchor) {
            *anchor = Anchor();
            if (j.contains("anchor")) {
                auto& a = j["anchor"];
                for (int i = 0; i < 3; i++) anchor->pos[i] = a.at("pos").at(i).get<float>();
                anchor->yaw = a.value("yaw", 0.0f);
                anchor->valid = true;
            }
        }
        return true;
    } catch (...) {
        return false;  // fichier en cours d'ecriture ou invalide : on garde l'ancien contenu
    }
}

inline bool saveZones(const std::filesystem::path& path, const std::vector<Zone>& zones, const Anchor* anchor = nullptr) {
    nlohmann::json arr = nlohmann::json::array();
    for (auto& z : zones) {
        nlohmann::json e = {{"name", z.name},
                            {"type", z.type == ZoneType::Plane ? "plane" : z.type == ZoneType::Poly ? "poly" : "box"},
                            {"pos", {z.pos[0], z.pos[1], z.pos[2]}},
                            {"size", {z.size[0], z.size[1], z.size[2]}},
                            {"yaw", z.yaw},
                            {"look", z.look},
                            {"enabled", z.enabled},
                            {"profile", z.profile}};
        if (z.type == ZoneType::Poly) {
            nlohmann::json pts = nlohmann::json::array();
            for (auto& p : z.pts) pts.push_back({p[0], p[1]});
            e["points"] = pts;
        }
        arr.push_back(e);
    }
    nlohmann::json root = {{"version", 1}, {"space", "stage"}, {"zones", arr}};
    Anchor kept;  // sans ancre fournie (fenetre de configuration...), on garde celle deja presente dans le fichier
    if (!anchor) {
        std::vector<Zone> ignored;
        loadZones(path, ignored, &kept);
        anchor = &kept;
    }
    if (anchor && anchor->valid) root["anchor"] = {{"pos", {anchor->pos[0], anchor->pos[1], anchor->pos[2]}}, {"yaw", anchor->yaw}};
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

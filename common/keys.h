// Touche de bascule des zones : lecture du texte des reglages ("space", "ctrl+f9"...) et ecriture inverse
// (utilisee par l'application de configuration quand on appuie sur une touche pour la choisir).
#pragma once

#include <windows.h>

#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace ovz {

struct KeySpec {
    int vk = 0;  // 0 : aucune touche
    bool ctrl = false, alt = false, shift = false;
};

namespace keys_detail {
struct Named {
    const char* name;
    int vk;
};
inline const Named* named(size_t& count) {
    static const Named list[] = {{"space", VK_SPACE}, {"enter", VK_RETURN}, {"tab", VK_TAB},       {"backspace", VK_BACK},
                                 {"insert", VK_INSERT}, {"delete", VK_DELETE}, {"home", VK_HOME},   {"end", VK_END},
                                 {"pageup", VK_PRIOR},  {"pagedown", VK_NEXT}, {"up", VK_UP},       {"down", VK_DOWN},
                                 {"left", VK_LEFT},     {"right", VK_RIGHT}};
    count = sizeof list / sizeof list[0];
    return list;
}
inline bool isDigit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }
}  // namespace keys_detail

// "space", "f9", "ctrl+f9", "ctrl+alt+x", "numpad5", "vk186" (code Windows brut), "none"...
inline KeySpec parseKey(const std::string& text) {
    using namespace keys_detail;
    KeySpec k;
    std::vector<std::string> parts;
    std::string cur;
    for (char ch : text) {
        if (ch == '+') {
            parts.push_back(cur);
            cur.clear();
        } else if (ch != ' ') {
            cur += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
    }
    parts.push_back(cur);
    for (size_t i = 0; i + 1 < parts.size(); i++) {
        if (parts[i] == "ctrl") k.ctrl = true;
        else if (parts[i] == "alt") k.alt = true;
        else if (parts[i] == "shift") k.shift = true;
    }
    const std::string& n = parts.back();
    size_t count = 0;
    const Named* list = named(count);
    for (size_t i = 0; i < count; i++)
        if (n == list[i].name) k.vk = list[i].vk;
    if (!k.vk && n.size() == 1 && std::isalnum(static_cast<unsigned char>(n[0]))) k.vk = std::toupper(static_cast<unsigned char>(n[0]));
    if (!k.vk && n.size() >= 2 && n.size() <= 3 && n[0] == 'f' && isDigit(n[1])) {
        const int num = std::atoi(n.c_str() + 1);
        if (num >= 1 && num <= 24) k.vk = VK_F1 + num - 1;
    }
    if (!k.vk && n.size() == 7 && n.compare(0, 6, "numpad") == 0 && isDigit(n[6])) k.vk = VK_NUMPAD0 + (n[6] - '0');
    if (!k.vk && n.size() >= 3 && n.compare(0, 2, "vk") == 0 && isDigit(n[2])) {
        const int num = std::atoi(n.c_str() + 2);
        if (num > 0 && num < 255) k.vk = num;
    }
    return k;
}

// Inverse de parseKey ; "none" si aucune touche.
inline std::string formatKey(const KeySpec& k) {
    using namespace keys_detail;
    if (!k.vk) return "none";
    std::string s;
    if (k.ctrl) s += "ctrl+";
    if (k.alt) s += "alt+";
    if (k.shift) s += "shift+";
    size_t count = 0;
    const Named* list = named(count);
    for (size_t i = 0; i < count; i++)
        if (list[i].vk == k.vk) return s + list[i].name;
    if ((k.vk >= 'A' && k.vk <= 'Z') || (k.vk >= '0' && k.vk <= '9')) return s + static_cast<char>(std::tolower(k.vk));
    if (k.vk >= VK_F1 && k.vk <= VK_F24) return s + "f" + std::to_string(k.vk - VK_F1 + 1);
    if (k.vk >= VK_NUMPAD0 && k.vk <= VK_NUMPAD9) return s + "numpad" + std::to_string(k.vk - VK_NUMPAD0);
    return s + "vk" + std::to_string(k.vk);
}

}  // namespace ovz

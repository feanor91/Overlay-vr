// Bouton de joystick / HOTAS lu en arriere-plan (DirectInput, sans exclusivite : les jeux gardent leurs assignations).
// Meme principe que la couche, mais utilisable depuis n'importe quelle application (barre d'etat...).
#pragma once

#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
#include <windows.h>
#include <dinput.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace ovz {

class JoyButton {
public:
    JoyButton() = default;
    JoyButton(const JoyButton&) = delete;
    JoyButton& operator=(const JoyButton&) = delete;
    ~JoyButton() { close(); }

    // Vrai quand le bouton (numerotation de joy.cpl, de 1 a 128) vient d'etre enfonce sur un joystick dont le nom
    // contient `nameFilter` (sans tenir compte des majuscules ; vide : n'importe lequel). A appeler souvent (30 ms).
    bool pressed(HWND window, int button, const std::string& nameFilter) {
        if (button < 1 || button > 128) return false;
        const ULONGLONG now = GetTickCount64();
        if (!di_) {
            if (lastEnum_ && now - lastEnum_ < 10000) return false;
            if (FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                                          reinterpret_cast<void**>(&di_), nullptr))) {
                lastEnum_ = now;
                return false;
            }
            window_ = window;
            lastEnum_ = 0;
        }
        if (joys_.empty()) {  // un HOTAS branche plus tard est repris dans les 10 secondes
            if (lastEnum_ && now - lastEnum_ < 10000) return false;
            lastEnum_ = now;
            di_->EnumDevices(DI8DEVCLASS_GAMECTRL, &JoyButton::enumCallback, this, DIEDFL_ATTACHEDONLY);
            if (joys_.empty()) return false;
        }
        std::string wanted = nameFilter;
        std::transform(wanted.begin(), wanted.end(), wanted.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        bool edge = false, lost = false;
        for (Joy& j : joys_) {
            if (!wanted.empty()) {
                std::string n = j.name;
                std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (n.find(wanted) == std::string::npos) continue;
            }
            DIJOYSTATE2 st = {};
            j.dev->Acquire();
            j.dev->Poll();
            if (FAILED(j.dev->GetDeviceState(sizeof st, &st))) {
                lost = true;
                continue;
            }
            const bool down = (st.rgbButtons[button - 1] & 0x80) != 0;
            if (down && !j.wasDown) edge = true;
            j.wasDown = down;
        }
        if (lost) {  // peripherique debranche : on relistera dans quelques secondes
            close();
            lastEnum_ = now;
        }
        return edge;
    }

private:
    struct Joy {
        IDirectInputDevice8W* dev;
        std::string name;
        bool wasDown;
    };

    static BOOL CALLBACK enumCallback(const DIDEVICEINSTANCEW* inst, void* self) {
        auto* me = static_cast<JoyButton*>(self);
        IDirectInputDevice8W* dev = nullptr;
        if (FAILED(me->di_->CreateDevice(inst->guidInstance, &dev, nullptr))) return DIENUM_CONTINUE;
        if (FAILED(dev->SetDataFormat(&c_dfDIJoystick2)) ||
            FAILED(dev->SetCooperativeLevel(me->window_, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE))) {
            dev->Release();
            return DIENUM_CONTINUE;
        }
        char name[260] = {};
        WideCharToMultiByte(CP_UTF8, 0, inst->tszProductName, -1, name, sizeof name - 1, nullptr, nullptr);
        me->joys_.push_back({dev, name, false});
        return DIENUM_CONTINUE;
    }

    void close() {
        for (Joy& j : joys_) {
            j.dev->Unacquire();
            j.dev->Release();
        }
        joys_.clear();
    }

    IDirectInput8W* di_ = nullptr;
    HWND window_ = nullptr;
    std::vector<Joy> joys_;
    ULONGLONG lastEnum_ = 0;
};

}  // namespace ovz

#!/usr/bin/env python3
"""Overlay SteamVR de zones magenta pour le passthrough par chroma key de Virtual Desktop.

Les zones sont des rectangles plats. En mode normal elles sont magenta opaque
(255, 0, 255) et Virtual Desktop les remplace par le passthrough. En mode
edition elles sont semi-transparentes et se deplacent avec le grip.

Commandes (saisies dans le terminal):
  add NOM L P        cree une zone L x P metres a la position de la main droite
  del NOM            supprime la zone
  look NOM DEG       n'affiche la zone que si le regard est >= DEG degres vers le bas
                     (DEG = 0 : toujours affichee)
  edit               bascule edition (semi-transparent) / passthrough (opaque)
  save               ecrit zones.json
  list               liste les zones
  quit               quitte
"""
import ctypes
import json
import math
import os
import sys
import threading
import time

import openvr

ZONES_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "zones.json")
GRAB_RADIUS = 0.30
EDIT_ALPHA = 0.5
FRAME_DT = 1 / 60
UNIVERSE = openvr.TrackingUniverseStanding


# --- maths -----------------------------------------------------------------

def mat_pos(m):
    return (m[0][3], m[1][3], m[2][3])


def mat_yaw(m):
    """Cap (rotation autour de Y) de la direction avant (-Z) du device."""
    fx, fz = -m[0][2], -m[2][2]
    return math.atan2(-fx, -fz)


def head_pitch_down(m):
    """Angle en degres du regard sous l'horizon (positif = vers le bas)."""
    fy = -m[1][2]
    return math.degrees(math.asin(max(-1.0, min(1.0, -fy))))


def dist(a, b):
    return math.dist(a, b)


def flat_transform(pos, yaw):
    """Transform d'un quad a plat, face vers le haut, oriente selon yaw."""
    c, s = math.cos(yaw), math.sin(yaw)
    # Ry(yaw) * Rx(-90deg) : la normale +Z du quad devient +Y (monde).
    rows = (
        (c, s, 0.0, pos[0]),
        (0.0, 0.0, 1.0, pos[1]),
        (s, -c, 0.0, pos[2]),
    )
    hm = openvr.HmdMatrix34_t()
    for i in range(3):
        for j in range(4):
            hm[i][j] = rows[i][j]
    return hm


# --- zones -----------------------------------------------------------------

class Zone:
    def __init__(self, name, width, depth, pos, yaw=0.0, look=0.0):
        self.name = name
        self.width = width
        self.depth = depth
        self.pos = list(pos)
        self.yaw = yaw
        self.look = look
        self.handle = None
        self.visible = None

    def to_json(self):
        return {"name": self.name, "width": self.width, "depth": self.depth,
                "pos": self.pos, "yaw": self.yaw, "look": self.look}


class App:
    def __init__(self):
        openvr.init(openvr.VRApplication_Overlay)
        self.system = openvr.VRSystem()
        self.overlay = openvr.VROverlay()
        self.zones = {}
        self.edit = False
        self.commands = []
        self.lock = threading.Lock()
        self.running = True
        self.grabbed = None  # (zone, offset)
        self.grip_prev = False
        # texture blanche 4x4, teintee en magenta via setOverlayColor
        self.tex = (ctypes.c_ubyte * (4 * 4 * 4))(*([255] * 64))
        self.load()

    # persistance
    def load(self):
        if not os.path.exists(ZONES_FILE):
            return
        with open(ZONES_FILE, encoding="utf-8") as f:
            for d in json.load(f):
                self.make_zone(Zone(d["name"], d["width"], d["depth"], d["pos"],
                                    d.get("yaw", 0.0), d.get("look", 0.0)))
        print(f"{len(self.zones)} zone(s) chargee(s)")

    def save(self):
        with open(ZONES_FILE, "w", encoding="utf-8") as f:
            json.dump([z.to_json() for z in self.zones.values()], f, indent=2)
        print(f"sauvegarde dans {ZONES_FILE}")

    # overlays
    def make_zone(self, z):
        if z.name in self.zones:
            self.destroy_zone(z.name)
        z.handle = self.overlay.createOverlay(f"vrzones.{z.name}", f"zone {z.name}")
        self.overlay.setOverlayRaw(z.handle, self.tex, 4, 4, 4)
        self.overlay.setOverlayColor(z.handle, 1.0, 0.0, 1.0)
        self.overlay.setOverlayWidthInMeters(z.handle, z.width)
        self.zones[z.name] = z
        self.apply_transform(z)

    def destroy_zone(self, name):
        z = self.zones.pop(name, None)
        if z:
            self.overlay.destroyOverlay(z.handle)
            if self.grabbed and self.grabbed[0] is z:
                self.grabbed = None

    def apply_transform(self, z):
        # le quad est carre de largeur `width` ; la profondeur est un ratio de hauteur
        self.overlay.setOverlayTexelAspect(z.handle, z.width / z.depth if z.depth else 1.0)
        self.overlay.setOverlayTransformAbsolute(z.handle, UNIVERSE, flat_transform(z.pos, z.yaw))

    # entrees
    def poses(self):
        return self.system.getDeviceToAbsoluteTrackingPose(
            UNIVERSE, 0, openvr.k_unMaxTrackedDeviceCount)

    def right_hand(self, poses):
        idx = self.system.getTrackedDeviceIndexForControllerRole(
            openvr.TrackedControllerRole_RightHand)
        if idx == openvr.k_unTrackedDeviceIndexInvalid or not poses[idx].bPoseIsValid:
            return None, None
        return idx, poses[idx].mDeviceToAbsoluteTracking

    def grip_pressed(self, idx):
        ok, state = self.system.getControllerState(idx)
        return bool(ok and state.ulButtonPressed & (1 << openvr.k_EButton_Grip))

    # commandes
    def stdin_loop(self):
        for line in sys.stdin:
            with self.lock:
                self.commands.append(line.strip())

    def handle(self, line, poses):
        parts = line.split()
        if not parts:
            return
        cmd, args = parts[0].lower(), parts[1:]
        try:
            if cmd == "add":
                name, w, d = args[0], float(args[1]), float(args[2])
                _, hand = self.right_hand(poses)
                if hand is None:
                    print("manette droite introuvable")
                    return
                self.make_zone(Zone(name, w, d, mat_pos(hand), mat_yaw(hand)))
                print(f"zone '{name}' creee")
            elif cmd == "del":
                self.destroy_zone(args[0])
            elif cmd == "look":
                self.zones[args[0]].look = float(args[1])
            elif cmd == "edit":
                self.edit = not self.edit
                print("mode edition" if self.edit else "mode passthrough")
            elif cmd == "save":
                self.save()
            elif cmd == "list":
                for z in self.zones.values():
                    print(f"{z.name}: {z.width}x{z.depth} m, pos={z.pos}, look={z.look}")
            elif cmd == "quit":
                self.running = False
            else:
                print(__doc__)
        except (IndexError, ValueError, KeyError) as e:
            print(f"commande invalide ({e!r})")

    # boucle
    def update_grab(self, poses):
        idx, hand = self.right_hand(poses)
        if hand is None or not self.edit:
            self.grabbed = None
            self.grip_prev = False
            return
        pressed = self.grip_pressed(idx)
        hp = mat_pos(hand)
        if pressed and not self.grip_prev:
            near = [(dist(hp, z.pos), z) for z in self.zones.values()]
            near = [t for t in near if t[0] <= GRAB_RADIUS]
            if near:
                z = min(near, key=lambda t: t[0])[1]
                self.grabbed = (z, [z.pos[i] - hp[i] for i in range(3)])
        elif not pressed:
            self.grabbed = None
        if self.grabbed:
            z, off = self.grabbed
            z.pos = [hp[i] + off[i] for i in range(3)]
            self.apply_transform(z)
        self.grip_prev = pressed

    def update_visibility(self, poses):
        hmd = poses[openvr.k_unTrackedDeviceIndex_Hmd]
        pitch = head_pitch_down(hmd.mDeviceToAbsoluteTracking) if hmd.bPoseIsValid else 0.0
        for z in self.zones.values():
            show = self.edit or z.look <= 0 or pitch >= z.look
            if show != z.visible:
                (self.overlay.showOverlay if show else self.overlay.hideOverlay)(z.handle)
                z.visible = show
            self.overlay.setOverlayAlpha(z.handle, EDIT_ALPHA if self.edit else 1.0)

    def run(self):
        threading.Thread(target=self.stdin_loop, daemon=True).start()
        print(__doc__)
        try:
            while self.running:
                poses = self.poses()
                with self.lock:
                    cmds, self.commands = self.commands, []
                for line in cmds:
                    self.handle(line, poses)
                self.update_grab(poses)
                self.update_visibility(poses)
                time.sleep(FRAME_DT)
        except KeyboardInterrupt:
            pass
        finally:
            for name in list(self.zones):
                self.destroy_zone(name)
            openvr.shutdown()


if __name__ == "__main__":
    App().run()

// Editeur de zones integre a la couche : tourne dans le jeu (pas de session VR supplementaire, le jeu ne se ferme pas).
// Inclus dans layer.cpp (namespace anonyme), apres l'etat global `g`, le panneau texte et les envois de pixels.
//
// Entrees : la couche ajoute son propre jeu d'actions (manettes Touch) a ceux du jeu (xrSuggestInteractionProfileBindings,
// xrAttachSessionActionSets, xrSyncActions interceptes). Pendant l'edition, les actions du jeu ne sont plus synchronisees :
// les gachettes ne declenchent rien dans le cockpit.
// Rendu : traits colores (couches quad aplaties vers la tete) tires d'une petite swapchain de couleurs unies.
// Les zones editees sont celles de g.zones (coordonnees brutes) ; le recalage du profil est integre aux zones a l'entree.

struct Edge {
    bool now = false, was = false;
    void set(bool v) {
        was = now;
        now = v;
    }
    bool down() const { return now && !was; }
    bool up() const { return !now && was; }
};

struct InputState {
    XrActionSet set = XR_NULL_HANDLE;
    XrAction trigger = XR_NULL_HANDLE, squeeze = XR_NULL_HANDLE, stick = XR_NULL_HANDLE, thumb = XR_NULL_HANDLE;
    XrAction a = XR_NULL_HANDLE, b = XR_NULL_HANDLE, x = XR_NULL_HANDLE, y = XR_NULL_HANDLE, aim = XR_NULL_HANDLE;
    XrPath hand[2] = {XR_NULL_PATH, XR_NULL_PATH};
    XrPath touch = XR_NULL_PATH;
    std::vector<XrActionSuggestedBinding> bindings;
    bool inited = false, failed = false, touchSuggested = false, attached = false;
    XrSpace aimSpace[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
} inp;

enum class Ph { Idle, Edge, Depth, Top, Bottom };

struct Edit {
    bool on = false;
    bool box = true;  // volume (sinon plan)
    Ph ph = Ph::Idle;
    float surfaceY = 0.75f;
    bool surfaceDirty = false;
    bool tracked[2] = {false, false};
    V3 handPos[2], rayO[2], rayD[2];
    Q handRot[2];
    Edge trig[2], grip[2], a, b, x, y, thumbR;
    float stickRy = 0;
    bool hasHit = false;
    V3 hit;
    V3 anchor, corner;
    float yaw = 0, dz = 0.02f, topY = 0, heightAdj = 0, bottomRefY = 0;
    bool bottomActive = false;
    ovz::Zone preview;
    int hover = -1;
    int counter = 1;
    struct Grab {
        bool active = false;
        size_t idx = 0;
        V3 hand0, pos0;
        float heading0 = 0, yaw0 = 0;
    } grab;
    std::vector<std::vector<ovz::Zone>> history;
    XrSwapchain sw = XR_NULL_HANDLE;
    bool swTried = false, swReady = false;
    ULONGLONG lastTick = 0, lastHeartbeat = 0;
    std::string message;
    ULONGLONG messageUntil = 0;
} ed;

bool editOn() { return ed.on; }

constexpr int kSwatch = 16, kSwatchCount = 8;
enum Col { kCyan = 0, kYellow, kWhite, kOrange, kGreen, kRed };

float headingOfQ(Q q) {
    const V3 f = ovm::qrot(q, V3{0, 0, -1});
    return std::atan2(-f.x, -f.z);
}

V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

Q quatFromBasis(V3 x, V3 y, V3 z) {  // colonnes x, y, z de la matrice de rotation
    const float m00 = x.x, m10 = x.y, m20 = x.z, m01 = y.x, m11 = y.y, m21 = y.z, m02 = z.x, m12 = z.y, m22 = z.z;
    Q q;
    const float tr = m00 + m11 + m22;
    if (tr > 0) {
        const float s = std::sqrt(tr + 1.0f) * 2;
        q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2;
        q = {0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2;
        q = {(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s};
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2;
        q = {(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s};
    }
    return q;
}

// --- entrees ---------------------------------------------------------------------------------------
bool inputInit() {
    if (inp.inited) return true;
    if (inp.failed || !nx.CreateActionSet || !nx.CreateAction || !nx.StringToPath || !nx.SuggestBindings) return false;
    inp.failed = true;
    XrActionSetCreateInfo si = {XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(si.actionSetName, "overlayvr_edit");
    std::strcpy(si.localizedActionSetName, "OverlayVR editeur");
    if (XR_FAILED(nx.CreateActionSet(g.instance, &si, &inp.set))) return false;
    nx.StringToPath(g.instance, "/user/hand/left", &inp.hand[0]);
    nx.StringToPath(g.instance, "/user/hand/right", &inp.hand[1]);
    nx.StringToPath(g.instance, "/interaction_profiles/oculus/touch_controller", &inp.touch);
    auto mk = [&](const char* name, XrActionType type, bool sub) {
        XrActionCreateInfo ci = {XR_TYPE_ACTION_CREATE_INFO};
        ci.actionType = type;
        std::strcpy(ci.actionName, name);
        std::strcpy(ci.localizedActionName, name);
        if (sub) {
            ci.countSubactionPaths = 2;
            ci.subactionPaths = inp.hand;
        }
        XrAction act = XR_NULL_HANDLE;
        nx.CreateAction(inp.set, &ci, &act);
        return act;
    };
    inp.trigger = mk("ovz_trigger", XR_ACTION_TYPE_FLOAT_INPUT, true);
    inp.squeeze = mk("ovz_squeeze", XR_ACTION_TYPE_FLOAT_INPUT, true);
    inp.stick = mk("ovz_stick", XR_ACTION_TYPE_VECTOR2F_INPUT, true);
    inp.thumb = mk("ovz_thumb", XR_ACTION_TYPE_BOOLEAN_INPUT, true);
    inp.a = mk("ovz_a", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    inp.b = mk("ovz_b", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    inp.x = mk("ovz_x", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    inp.y = mk("ovz_y", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    inp.aim = mk("ovz_aim", XR_ACTION_TYPE_POSE_INPUT, true);
    if (!inp.trigger || !inp.squeeze || !inp.stick || !inp.thumb || !inp.a || !inp.b || !inp.x || !inp.y || !inp.aim) return false;
    auto bind = [&](XrAction act, const std::string& path) {
        XrPath p = XR_NULL_PATH;
        if (XR_SUCCEEDED(nx.StringToPath(g.instance, path.c_str(), &p))) inp.bindings.push_back({act, p});
    };
    const char* side[2] = {"left", "right"};
    for (int h = 0; h < 2; h++) {
        const std::string s = std::string("/user/hand/") + side[h] + "/input/";
        bind(inp.trigger, s + "trigger/value");
        bind(inp.squeeze, s + "squeeze/value");
        bind(inp.stick, s + "thumbstick");
        bind(inp.thumb, s + "thumbstick/click");
        bind(inp.aim, s + "aim/pose");
    }
    bind(inp.a, "/user/hand/right/input/a/click");
    bind(inp.b, "/user/hand/right/input/b/click");
    bind(inp.x, "/user/hand/left/input/x/click");
    bind(inp.y, "/user/hand/left/input/y/click");
    inp.failed = false;
    inp.inited = true;
    return true;
}

bool inputEnabled() { return g.active && !g.disabled && g.cfg.inGameEditor; }

XrResult XRAPI_CALL ovSuggestBindings(XrInstance instance, const XrInteractionProfileSuggestedBinding* sb) {
    if (inputEnabled() && inputInit() && sb->interactionProfile == inp.touch) {
        std::vector<XrActionSuggestedBinding> all(sb->suggestedBindings, sb->suggestedBindings + sb->countSuggestedBindings);
        all.insert(all.end(), inp.bindings.begin(), inp.bindings.end());
        XrInteractionProfileSuggestedBinding m = *sb;
        m.countSuggestedBindings = static_cast<uint32_t>(all.size());
        m.suggestedBindings = all.data();
        inp.touchSuggested = true;
        return nx.SuggestBindings(instance, &m);
    }
    return nx.SuggestBindings(instance, sb);
}

XrResult XRAPI_CALL ovAttachActionSets(XrSession session, const XrSessionActionSetsAttachInfo* ai) {
    if (session != g.session || !inputEnabled() || !inputInit()) return nx.AttachActionSets(session, ai);
    if (!inp.touchSuggested) {  // le jeu n'a pas propose les manettes Touch : on le fait avant que les actions soient verrouillees
        XrInteractionProfileSuggestedBinding sb = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile = inp.touch;
        sb.countSuggestedBindings = static_cast<uint32_t>(inp.bindings.size());
        sb.suggestedBindings = inp.bindings.data();
        nx.SuggestBindings(g.instance, &sb);
        inp.touchSuggested = true;
    }
    std::vector<XrActionSet> sets(ai->actionSets, ai->actionSets + ai->countActionSets);
    sets.push_back(inp.set);
    XrSessionActionSetsAttachInfo m = *ai;
    m.countActionSets = static_cast<uint32_t>(sets.size());
    m.actionSets = sets.data();
    const XrResult r = nx.AttachActionSets(session, &m);
    if (XR_SUCCEEDED(r)) {
        inp.attached = true;
        for (int h = 0; h < 2; h++) {
            XrActionSpaceCreateInfo ci = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
            ci.action = inp.aim;
            ci.subactionPath = inp.hand[h];
            ci.poseInActionSpace.orientation.w = 1;
            nx.CreateActionSpace(session, &ci, &inp.aimSpace[h]);
        }
        logf("editeur en jeu : entrees des manettes ajoutees");
    } else {
        logf("editeur en jeu : attachement refuse (%d), pas d'edition possible", static_cast<int>(r));
    }
    return r;
}

XrResult XRAPI_CALL ovSyncActionSets(XrSession session, const XrActionsSyncInfo* si) {
    if (session != g.session || !inp.attached || !ed.on) return nx.SyncActions(session, si);
    XrActiveActionSet mine = {inp.set, XR_NULL_PATH};  // edition : seules nos actions sont actives, le jeu ne recoit rien
    XrActionsSyncInfo m = *si;
    m.countActiveActionSets = 1;
    m.activeActionSets = &mine;
    return nx.SyncActions(session, &m);
}

// --- lecture des manettes ----------------------------------------------------------------------------
float fval(XrAction act, int h) {
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = act;
    gi.subactionPath = inp.hand[h];
    XrActionStateFloat s = {XR_TYPE_ACTION_STATE_FLOAT};
    return XR_SUCCEEDED(nx.GetFloat(g.session, &gi, &s)) && s.isActive ? s.currentState : 0.0f;
}
bool bval(XrAction act, int h = -1) {
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = act;
    if (h >= 0) gi.subactionPath = inp.hand[h];
    XrActionStateBoolean s = {XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(nx.GetBool(g.session, &gi, &s)) && s.isActive && s.currentState;
}
float stickY(int h) {
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = inp.stick;
    gi.subactionPath = inp.hand[h];
    XrActionStateVector2f s = {XR_TYPE_ACTION_STATE_VECTOR2F};
    return XR_SUCCEEDED(nx.GetVec2(g.session, &gi, &s)) && s.isActive ? s.currentState.y : 0.0f;
}

// --- sauvegarde ---------------------------------------------------------------------------------------
void editSave() {
    ovz::saveZones(ovz::zonesPath(), g.zones);
    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (GetFileAttributesExW(ovz::zonesPath().c_str(), GetFileExInfoStandard, &attr)) g.zonesStamp = attr.ftLastWriteTime;
    rebuildShown();
}

std::filesystem::path editorJsonPath() { return ovz::dataDir() / "editor.json"; }

void saveSurface() {
    nlohmann::json j = nlohmann::json::object();
    {
        std::ifstream f(editorJsonPath());
        if (f) {
            try {
                j = nlohmann::json::parse(f);
            } catch (...) {
            }
        }
    }
    j["surface_y"] = ed.surfaceY;
    std::ofstream(editorJsonPath()) << j.dump(2);
}

std::string currentProfileName() { return g.profileIdx >= 0 ? g.cfg.profiles[g.profileIdx].name : std::string(); }
bool otherProfileZone(const ovz::Zone& z) { return !z.profile.empty() && z.profile != currentProfileName(); }

void editPushHistory() {
    ed.history.push_back(g.zones);
    if (ed.history.size() > 50) ed.history.erase(ed.history.begin());
}

void editNote(const std::string& text) {
    ed.message = text;
    ed.messageUntil = GetTickCount64() + 4000;
}

// --- entree / sortie du mode edition ----------------------------------------------------------------
void editEnter() {
    logf("editeur en jeu : demande d'ouverture (manettes ajoutees : %s)", inp.attached ? "oui" : "non");
    if (!inp.attached) {
        logf("editeur en jeu : impossible, les manettes n'ont pas pu etre ajoutees (voir plus haut)");
        return;
    }
    // Le recalage du profil est integre aux zones visibles dans ce jeu ; ensuite on edite en coordonnees brutes.
    float* o = contextOffset();
    if (o[0] != 0 || o[1] != 0 || o[2] != 0 || contextYaw() != 0) {
        std::vector<ovz::Zone> all = g.zones;
        ovz::applyOffset(all, o, contextYaw());
        const std::string mine = currentProfileName();
        for (size_t i = 0; i < all.size(); i++)
            if (all[i].profile.empty() || all[i].profile == mine) g.zones[i] = all[i];
        o[0] = o[1] = o[2] = 0;
        contextYaw() = 0;
        ovz::saveConfig(g.cfg);
        logf("editeur en jeu : recalage integre aux zones");
    }
    {
        std::ifstream f(editorJsonPath());
        if (f) {
            try {
                ed.surfaceY = nlohmann::json::parse(f).value("surface_y", ed.surfaceY);
            } catch (...) {
            }
        }
    }
    ed.history.clear();
    ed.ph = Ph::Idle;
    ed.grab = Edit::Grab();
    editSave();
    ed.on = true;
    ed.lastTick = 0;
    logf("editeur en jeu : entree (%zu zones)", g.zones.size());
    for (int h = 0; h < 2; h++) {
        ed.trig[h] = ed.grip[h] = Edge();
        ed.tracked[h] = false;
    }
    ed.a = ed.b = ed.x = ed.y = ed.thumbR = Edge();
    MessageBeep(MB_ICONASTERISK);
    logf("editeur en jeu : ACTIF");
}

void editExit() {
    ed.on = false;
    ed.ph = Ph::Idle;
    ed.grab = Edit::Grab();
    editSave();
    MessageBeep(MB_ICONEXCLAMATION);
    logf("editeur en jeu : ferme");
}

// --- geometrie de l'edition --------------------------------------------------------------------------
int pickZone(V3 o, V3 d, float& tBest) {
    int best = -1;
    tBest = 1e9f;
    for (size_t i = 0; i < g.zones.size(); i++) {
        const ovz::Zone& z = g.zones[i];
        if (otherProfileZone(z)) continue;
        const Q inv = ovm::qaxis('y', -z.yaw);
        const V3 lo = ovm::qrot(inv, o - V3{z.pos[0], z.pos[1], z.pos[2]});
        const V3 ld = ovm::qrot(inv, d);
        const float org[3] = {lo.x, lo.y, lo.z}, dir[3] = {ld.x, ld.y, ld.z};
        const float half[3] = {z.size[0] / 2 + 0.02f, (z.type == ovz::ZoneType::Plane ? 0.0f : z.size[1] / 2) + 0.02f, z.size[2] / 2 + 0.02f};
        float t0 = 0, t1 = 1e9f;
        bool ok = true;
        for (int k = 0; k < 3 && ok; k++) {
            if (std::fabs(dir[k]) < 1e-6f) {
                if (std::fabs(org[k]) > half[k]) ok = false;
            } else {
                float ta = (-half[k] - org[k]) / dir[k], tb = (half[k] - org[k]) / dir[k];
                if (ta > tb) std::swap(ta, tb);
                t0 = std::max(t0, ta);
                t1 = std::min(t1, tb);
                if (t0 > t1) ok = false;
            }
        }
        if (ok && t0 < tBest) {
            tBest = t0;
            best = static_cast<int>(i);
        }
    }
    return best;
}

void buildPreview(float dz, float y0, float y1) {
    V3 e = ed.corner - ed.anchor;
    e.y = 0;
    const float w = ovm::length(e);
    if (w > 1e-4f) ed.yaw = std::atan2(-e.z, e.x);
    const V3 xd = {std::cos(ed.yaw), 0, -std::sin(ed.yaw)}, zd = {std::sin(ed.yaw), 0, std::cos(ed.yaw)};
    V3 c = ed.anchor + xd * (w / 2) + zd * (dz / 2);
    float lo = std::min(y0, y1), hi = std::max(y0, y1);
    if (hi - lo < 0.03f) hi = lo + 0.03f;
    c.y = ed.box ? (lo + hi) / 2 : ed.anchor.y;
    ed.preview = ovz::Zone();
    ed.preview.type = ed.box ? ovz::ZoneType::Box : ovz::ZoneType::Plane;
    ed.preview.yaw = ed.yaw;
    ed.preview.size[0] = w;
    ed.preview.size[1] = ed.box ? hi - lo : 0.0f;
    ed.preview.size[2] = std::fabs(dz);
    ed.preview.pos[0] = c.x;
    ed.preview.pos[1] = c.y;
    ed.preview.pos[2] = c.z;
}

void commitPreview() {
    editPushHistory();
    ed.preview.name = std::string(ed.box ? "volume" : "plan") + std::to_string(ed.counter++);
    ed.preview.profile = currentProfileName();
    g.zones.push_back(ed.preview);
    editSave();
    editNote("Zone '" + ed.preview.name + "' creee (renomme-la dans la fenetre de configuration)");
    ed.ph = Ph::Idle;
    MessageBeep(MB_ICONASTERISK);
}

// --- dessin -------------------------------------------------------------------------------------------
bool ensureEditSwapchain() {
    if (ed.swReady) return true;
    if (ed.swTried) return false;
    ed.swTried = true;
    const int W = kSwatch * kSwatchCount;
    if (!createColorSwapchain(ed.sw, W, kSwatch)) return false;
    const unsigned char rgb[kSwatchCount][3] = {{0, 220, 255}, {255, 230, 0}, {255, 255, 255}, {255, 140, 26},
                                                {50, 255, 50},  {255, 60, 60}, {255, 255, 255}, {255, 255, 255}};
    const bool rgba = g.format == DXGI_FORMAT_R8G8B8A8_UNORM || g.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    std::vector<uint8_t> px(static_cast<size_t>(W) * kSwatch * 4);
    for (int y = 0; y < kSwatch; y++)
        for (int x = 0; x < W; x++) {
            uint8_t* p = &px[(static_cast<size_t>(y) * W + x) * 4];
            const unsigned char* c = rgb[x / kSwatch];
            p[0] = rgba ? c[0] : c[2];
            p[1] = c[1];
            p[2] = rgba ? c[2] : c[0];
            p[3] = 255;
        }
    const ULONGLONG t = GetTickCount64();
    ed.swReady = uploadPixels(ed.sw, W, kSwatch, px);
    logf("editeur en jeu : envoi des couleurs en %llu ms", GetTickCount64() - t);
    logf("editeur en jeu : couleurs %s", ed.swReady ? "ok" : "echec");
    return ed.swReady;
}

void addSegment(std::vector<XrCompositionLayerQuad>& out, V3 head, V3 a, V3 b, float thick, Col col) {
    V3 d = b - a;
    const float len = ovm::length(d);
    if (len < 1e-3f) return;
    const V3 x = d * (1.0f / len), mid = (a + b) * 0.5f;
    V3 z = head - mid;
    z = z - x * ovm::dot(z, x);  // le quad contient le trait et regarde la tete
    const float zl = ovm::length(z);
    if (zl < 1e-3f) return;
    z = z * (1.0f / zl);
    const V3 y = cross(z, x);
    XrCompositionLayerQuad q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    q.space = g.stage;
    q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = ed.sw;
    q.subImage.imageRect = {{col * kSwatch + 2, 2}, {kSwatch - 4, kSwatch - 4}};
    q.pose = toXr(mid, quatFromBasis(x, y, z));
    q.size = {len, thick};
    out.push_back(q);
}

// `full` : 12 aretes ; sinon le seul contour du haut (4 couches de moins par zone : les couches de composition coutent des fps).
void addZoneWire(std::vector<XrCompositionLayerQuad>& out, V3 head, const ovz::Zone& z, float thick, Col col, size_t cap, bool full = true) {
    const Q yaw = ovm::qaxis('y', z.yaw);
    const V3 c = {z.pos[0], z.pos[1], z.pos[2]};
    auto P = [&](float x, float y, float zz) { return c + ovm::qrot(yaw, V3{x, y, zz}); };
    const float hx = z.size[0] / 2, hz = z.size[2] / 2;
    auto ring = [&](float y) {
        std::vector<V3> v;
        if (z.type == ovz::ZoneType::Poly) {
            for (auto& p : z.pts) v.push_back(P(p[0], y, p[1]));
        } else {
            v = {P(-hx, y, -hz), P(hx, y, -hz), P(hx, y, hz), P(-hx, y, hz)};
        }
        return v;
    };
    const bool plane = z.type == ovz::ZoneType::Plane;
    const float hy = plane ? 0.0f : z.size[1] / 2;
    const std::vector<V3> top = ring(hy), bot = ring(-hy);
    if (z.type == ovz::ZoneType::Poly && top.size() > 10) return;
    for (size_t i = 0; i < top.size() && out.size() < cap; i++) addSegment(out, head, top[i], top[(i + 1) % top.size()], thick, col);
    if (plane || !full) return;
    for (size_t i = 0; i < bot.size() && out.size() < cap; i++) addSegment(out, head, bot[i], bot[(i + 1) % bot.size()], thick, col);
    for (size_t i = 0; i < top.size() && out.size() < cap; i++) addSegment(out, head, top[i], bot[i], thick, col);
}

// --- une image d'edition ---------------------------------------------------------------------------
// Lit les manettes, fait avancer l'edition et ajoute les traits colores a `out`. `head`/`headRot` : pose de la tete (STAGE).
void editFrame(XrTime displayTime, V3 head, std::vector<XrCompositionLayerQuad>& out) {
    static int logged = 0;
    if (logged < 3) logf("editeur en jeu : image %d", ++logged);
    if (!ensureEditSwapchain()) return;
    const ULONGLONG nowMs = GetTickCount64();
    const float dt = ed.lastTick ? std::min(0.1f, (nowMs - ed.lastTick) / 1000.0f) : 0.0f;
    ed.lastTick = nowMs;

    // poses et boutons
    for (int h = 0; h < 2; h++) {
        XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
        constexpr XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        ed.tracked[h] = inp.aimSpace[h] && XR_SUCCEEDED(nx.LocateSpace(inp.aimSpace[h], g.stage, displayTime, &loc)) &&
                        (loc.locationFlags & need) == need;
        if (ed.tracked[h]) {
            ed.handPos[h] = {loc.pose.position.x, loc.pose.position.y, loc.pose.position.z};
            ed.handRot[h] = {loc.pose.orientation.x, loc.pose.orientation.y, loc.pose.orientation.z, loc.pose.orientation.w};
            ed.rayO[h] = ed.handPos[h];
            ed.rayD[h] = ovm::qrot(ed.handRot[h], V3{0, 0, -1});
        }
        ed.trig[h].set(fval(inp.trigger, h) > 0.6f);
        ed.grip[h].set(fval(inp.squeeze, h) > 0.6f);
    }
    ed.a.set(bval(inp.a));
    ed.b.set(bval(inp.b));
    ed.x.set(bval(inp.x));
    ed.y.set(bval(inp.y));
    ed.thumbR.set(bval(inp.thumb, 1));
    ed.stickRy = stickY(1);

    // point vise sur le plan du bureau (main droite)
    ed.hasHit = false;
    if (ed.tracked[1] && ed.rayD[1].y < -0.02f) {
        const float t = (ed.surfaceY - ed.rayO[1].y) / ed.rayD[1].y;
        if (t > 0.05f && t < 8.0f) {
            ed.hit = ed.rayO[1] + ed.rayD[1] * t;
            ed.hasHit = true;
        }
    }
    float hoverT = 0;
    ed.hover = (ed.ph == Ph::Idle && !ed.grab.active && ed.tracked[1]) ? pickZone(ed.rayO[1], ed.rayD[1], hoverT) : -1;

    // commandes globales
    if (ed.y.down()) {
        editExit();
        return;
    }
    if (ed.x.down() && !ed.history.empty()) {
        g.zones = ed.history.back();
        ed.history.pop_back();
        ed.grab = Edit::Grab();
        ed.ph = Ph::Idle;
        editSave();
        editNote("Annule");
    }
    if (ed.ph == Ph::Idle && !ed.grab.active) {
        if (std::fabs(ed.stickRy) > 0.5f) {
            ed.surfaceY = std::clamp(ed.surfaceY + ed.stickRy * 0.6f * dt, 0.0f, 2.5f);
            ed.surfaceDirty = true;
        } else if (ed.surfaceDirty) {
            saveSurface();
            ed.surfaceDirty = false;
        }
        if (ed.thumbR.down() && ed.tracked[1]) {
            ed.surfaceY = ed.handPos[1].y;
            saveSurface();
        }
        if (ed.a.down()) ed.box = !ed.box;
        if (ed.b.down() && ed.hover >= 0) {
            editPushHistory();
            g.zones.erase(g.zones.begin() + ed.hover);
            ed.hover = -1;
            editSave();
            editNote("Zone supprimee");
        }
        if (ed.grip[1].down() && ed.hover >= 0 && ed.tracked[1]) {
            editPushHistory();
            ed.grab.active = true;
            ed.grab.idx = static_cast<size_t>(ed.hover);
            ed.grab.hand0 = ed.handPos[1];
            ed.grab.heading0 = headingOfQ(ed.handRot[1]);
            const ovz::Zone& z = g.zones[ed.grab.idx];
            ed.grab.pos0 = {z.pos[0], z.pos[1], z.pos[2]};
            ed.grab.yaw0 = z.yaw;
        }
    }
    if (ed.grab.active) {
        if (!ed.grip[1].now || ed.grab.idx >= g.zones.size()) {
            ed.grab.active = false;
            editSave();
        } else if (ed.tracked[1]) {
            ovz::Zone& z = g.zones[ed.grab.idx];
            const V3 np = ed.grab.pos0 + (ed.handPos[1] - ed.grab.hand0);
            z.pos[0] = np.x;
            z.pos[1] = np.y;
            z.pos[2] = np.z;
            z.yaw = ed.grab.yaw0 + headingOfQ(ed.handRot[1]) - ed.grab.heading0;
        }
    }

    // trace d'une zone (main droite)
    bool previewOn = false;
    switch (ed.ph) {
        case Ph::Idle:
            if (ed.trig[1].down() && ed.hasHit && !ed.grab.active) {
                ed.ph = Ph::Edge;
                ed.anchor = ed.corner = ed.hit;
            }
            break;
        case Ph::Edge:
            if (ed.hasHit) ed.corner = ed.hit;
            buildPreview(0.02f, ed.surfaceY, ed.surfaceY + 0.02f);
            previewOn = true;
            if (ed.b.down()) {
                ed.ph = Ph::Idle;
            } else if (!ed.trig[1].now) {
                if (ed.preview.size[0] < 0.05f) {
                    ed.ph = Ph::Idle;
                    editNote("Bord trop court, annule");
                } else {
                    ed.ph = Ph::Depth;
                    ed.dz = 0.02f;
                }
            }
            break;
        case Ph::Depth:
            if (ed.hasHit) ed.dz = ovm::dot(ed.hit - ed.anchor, V3{std::sin(ed.yaw), 0, std::cos(ed.yaw)});
            buildPreview(ed.dz, ed.surfaceY, ed.surfaceY + 0.02f);
            previewOn = true;
            if (ed.b.down()) {
                ed.ph = Ph::Idle;
            } else if (ed.trig[1].down() && std::fabs(ed.dz) >= 0.05f) {
                if (!ed.box) {
                    commitPreview();
                    previewOn = false;
                } else {
                    ed.ph = Ph::Top;
                    ed.heightAdj = 0;
                }
            }
            break;
        case Ph::Top: {
            if (std::fabs(ed.stickRy) > 0.15f) ed.heightAdj += ed.stickRy * 0.3f * dt;
            const float top = ed.handPos[1].y + ed.heightAdj;
            buildPreview(ed.dz, ed.surfaceY, top);
            previewOn = true;
            if (ed.b.down()) {
                ed.ph = Ph::Idle;
            } else if (ed.trig[1].down()) {
                ed.topY = top;
                ed.ph = Ph::Bottom;
                ed.heightAdj = 0;
                ed.bottomRefY = ed.handPos[1].y;
                ed.bottomActive = false;
            }
            break;
        }
        case Ph::Bottom: {
            if (std::fabs(ed.stickRy) > 0.15f) ed.heightAdj += ed.stickRy * 0.3f * dt;
            if (!ed.bottomActive && std::fabs(ed.handPos[1].y - ed.bottomRefY) > 0.04f) ed.bottomActive = true;
            const float bottom = ed.bottomActive ? ed.handPos[1].y + ed.heightAdj : ed.surfaceY + ed.heightAdj;
            buildPreview(ed.dz, bottom, ed.topY);
            previewOn = true;
            if (ed.b.down()) {
                ed.ph = Ph::Idle;
            } else if (ed.trig[1].down()) {
                commitPreview();
                previewOn = false;
            }
            break;
        }
    }

    // --- visuels ---
    static ULONGLONG lastDiag = 0;
    if (nowMs - lastDiag > 5000) {
        lastDiag = nowMs;
        logf("editeur en jeu : manettes suivies G%d D%d, point vise %d, phase %d, %zu zone(s), tete (%.2f %.2f %.2f)", ed.tracked[0], ed.tracked[1],
             ed.hasHit, static_cast<int>(ed.ph), g.zones.size(), head.x, head.y, head.z);
    }
    size_t cap = out.size() + 34;  // plafond de traits : au-dela, les fps chutent et la tete semble figee
    for (int h = 0; h < 2; h++) {
        if (!ed.tracked[h]) continue;
        const float reach = h == 1 && ed.hasHit ? std::min(ovm::length(ed.hit - ed.rayO[1]), 3.0f) : 1.5f;
        addSegment(out, head, ed.rayO[h], ed.rayO[h] + ed.rayD[h] * reach, 0.004f, h == 1 ? kWhite : kCyan);
    }
    if (ed.hasHit) {
        const V3 p = ed.hit;
        addSegment(out, head, p + V3{-0.03f, 0, 0}, p + V3{0.03f, 0, 0}, 0.006f, kYellow);
        addSegment(out, head, p + V3{0, 0, -0.03f}, p + V3{0, 0, 0.03f}, 0.006f, kYellow);
    }
    if (previewOn) addZoneWire(out, head, ed.preview, 0.008f, kOrange, cap);
    // zone visee d'abord (priorite sur le plafond de couches), puis les autres
    if (ed.hover >= 0) addZoneWire(out, head, g.zones[ed.hover], 0.008f, kYellow, cap);
    if (ed.grab.active && ed.grab.idx < g.zones.size()) addZoneWire(out, head, g.zones[ed.grab.idx], 0.008f, kGreen, cap);
    for (size_t i = 0; i < g.zones.size(); i++) {
        if (static_cast<int>(i) == ed.hover || (ed.grab.active && i == ed.grab.idx)) continue;
        addZoneWire(out, head, g.zones[i], 0.004f, otherProfileZone(g.zones[i]) ? kWhite : kCyan, cap, false);
    }
}

// --- panneau d'aide du mode edition -------------------------------------------------------------------
std::vector<std::wstring> editHudLines() {
    const wchar_t* hint = L"";
    switch (ed.ph) {
        case Ph::Idle: hint = L"Prêt : vise le bureau, maintiens la gâchette droite et tire le 1er bord"; break;
        case Ph::Edge: hint = L"1 Tire le 1er bord jusqu'à l'autre coin, relâche (B annule)"; break;
        case Ph::Depth: hint = L"2 Vise le côté opposé, gâchette pour valider (B annule)"; break;
        case Ph::Top: hint = L"3 Manette au SOMMET de l'objet, gâchette (B annule)"; break;
        case Ph::Bottom: hint = L"4 Manette au BAS de l'objet (ou ne bouge pas : bas = bureau), gâchette"; break;
    }
    wchar_t buf[200];
    std::swprintf(buf, 200, L"%zu zone(s)  |  bureau à %.2f m  |  jeu : %ls", g.zones.size(), ed.surfaceY,
                  g.profileIdx >= 0 ? widenUtf8(g.cfg.profiles[g.profileIdx].name).c_str() : L"tous");
    std::wstring l1 = std::wstring(L"ÉDITEUR DE ZONES EN JEU  -  mode : ") + (ed.box ? L"VOLUME" : L"PLAN") + L"  (A change)";
    return {l1, buf, GetTickCount64() < ed.messageUntil ? widenUtf8(ed.message) : std::wstring(hint),
            L"Droite : gâchette tracer, grip déplacer la zone visée, B supprimer, stick ▲▼ hauteur du bureau",
            L"Gauche : X annuler, Y QUITTER l'éditeur (ou touche/bouton de lancement)"};
}

// --- cycle de vie ---------------------------------------------------------------------------------
void ingameReset() {
    for (int h = 0; h < 2; h++) {
        if (inp.aimSpace[h]) nx.DestroySpace(inp.aimSpace[h]);
        inp.aimSpace[h] = XR_NULL_HANDLE;
    }
    if (ed.sw) nx.DestroySwapchain(ed.sw);
    ed.sw = XR_NULL_HANDLE;
    ed.swTried = ed.swReady = false;
    ed.on = false;
    ed.ph = Ph::Idle;
    inp.attached = false;
}

// Appelee toutes les 500 ms : bascule demandee par la barre d'etat (fichier edit.toggle) et battement de coeur
// (layer_alive) qui indique a la barre d'etat qu'un jeu est actif : elle n'ouvre alors pas l'editeur separe.
void ingamePoll() {
    if (!g.cfg.inGameEditor) return;
    const auto flag = ovz::dataDir() / "edit.toggle";
    std::error_code ec;
    if (std::filesystem::exists(flag, ec)) {
        std::filesystem::remove(flag, ec);
        if (ed.on) editExit();
        else editEnter();
    }
    const ULONGLONG now = GetTickCount64();
    if (g.session && now - ed.lastHeartbeat > 1500) {
        ed.lastHeartbeat = now;
        std::ofstream(ovz::dataDir() / "layer_alive") << now;
    }
}

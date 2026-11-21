// Attachment Tweaks, part two: decals on attachments, and attachment light shadows only on Cinematic.
// Included by attachment_tweaks.cpp after its Ref / refOf / alive; extras::pass() runs on the game thread.
//
// Decals (the user, 2026-11-13, after Maja's Detail Works V4 showed how): the car's decals are baked by its
// MTDecalComponent into every mesh component *of the car* whose material slot is listed in
// MTVehicle.DecalableMaterialSlotNames; the baker projects them around the car (bounds / normal captures), so a mesh
// placed anywhere on it takes them where they really are. An attachment is a child actor, not the car, so it gets
// none. Maja's BP_DWDecalable fixes that per part: a copy of the mesh as a component *of the car*, attached to the
// part's mesh (which is hidden), its slot names added to the list, then OnRep_Customization to re-bake. This does
// the same for any attachment: a mesh qualifies when a slot's material comes from M_Body or M_Gray (the paintable
// bases, and what decal-ready materials are made from -- Maja's paintables are M_Gray instances). Palette-only
// props (M_Palette) are left alone. Parts that do their own (Maja's BP_DWDecalable, Slides' BPC_SlideDecals) too.
// The copy takes the part's current materials, so paint carries over; a repaint is copied across within 2 s.
//
// Shadows (Maja's BP_ScalableCastShadow): every attachment spot / rect light casts shadows exactly when the shadow
// setting is Cinematic (sg.ShadowQuality 4), and never below it -- lamps on a car are many, and shadows are dear.
//
// shortcut: only for players with the plugin; others see the part without decals. Car-side only, nothing is saved.
#pragma once
#include <set>

namespace extras {

static uintptr_t cSmc, cSpot, cRect, cMI, cVehicle;
static uintptr_t fAddC, fFinishC, fSetMeshC, fMobC, fCollC, fSetMatC, fGetMatC, fNumMatC, fSlotsC, fAttachC, fVisC, fIsVisC,
    fDestroyC, fRepCustom, fCompsC, fLightShadow, fPadC, strCDO, cvarFn, sysCDO;
static int oParts = -1, oChild = -1, oDecalNames = -1, oMesh = -1, oParentMat = -1, oVehIdC = -1;
static bool decalsOn, shadowsOn;

struct Copy { Ref src, prx; std::vector<uintptr_t> mats; };
struct Part { Ref actor; Ref veh; std::vector<Copy> copies; std::vector<uint64_t> names; bool skip = false; };
static std::map<uintptr_t, Part> parts;                 // attachment actor -> its copies on the car
static std::map<uintptr_t, std::pair<Ref, int>> lights; // light component -> the shadow state last set
static std::vector<Ref> cars;                           // filled by the scan thread
static DWORD lastPass;

struct Arr { uintptr_t data; int num, max; };

static std::vector<uintptr_t> comps(uintptr_t actor, uintptr_t cls) {
    alignas(16) unsigned char p[0x40] = {0};
    memcpy(p + off(fCompsC, "ComponentClass"), &cls, 8);
    api->process_event(actor, fCompsC, p);
    Arr a; memcpy(&a, p + off(fCompsC, "ReturnValue"), sizeof a);
    std::vector<uintptr_t> out;
    for (int i = 0; a.data && i < a.num && i < 512; i++) if (uintptr_t c = get<uintptr_t>(a.data + 8ull * i)) out.push_back(c);
    // shortcut: the returned array's block is not freed (no FMemory::Free in the loader); a few bytes per call, every 2 s
    return out;
}
static int cvar(const wchar_t* key) {
    if (!sysCDO || !cvarFn) return -1;
    struct { const wchar_t* data; int num, max; } in = {key, (int)wcslen(key) + 1, (int)wcslen(key) + 1};
    unsigned char p[0x40] = {0};
    memcpy(p + off(cvarFn, "VariableName"), &in, sizeof in);
    api->process_event(sysCDO, cvarFn, p);
    int out = -1; memcpy(&out, p + off(cvarFn, "ReturnValue"), 4);
    return out;
}
static uintptr_t material(uintptr_t c, int i) {
    unsigned char p[0x20] = {0};
    memcpy(p + off(fGetMatC, "ElementIndex"), &i, 4);
    api->process_event(c, fGetMatC, p);
    return get<uintptr_t>((uintptr_t)p + off(fGetMatC, "ReturnValue"));
}
static bool paintable(uintptr_t m) {  // its base material is M_Body or M_Gray
    for (int depth = 0; m && depth < 8 && api->is_a(classOf(m), cMI); depth++) m = get<uintptr_t>(m + oParentMat);
    if (!m) return false;
    std::string n = objName(m);
    return n == "M_Body" || n == "M_Gray";
}
static bool visible(uintptr_t c) { unsigned char r[0x10] = {0}; api->process_event(c, fIsVisC, r); return r[off(fIsVisC, "ReturnValue")] != 0; }
static void show(uintptr_t c, bool on) { unsigned char p[0x10] = {0}; p[off(fVisC, "bNewVisibility")] = on; api->process_event(c, fVisC, p); }
static void setMat(uintptr_t c, int i, uintptr_t m) {
    unsigned char q[0x20] = {0};
    memcpy(q + off(fSetMatC, "ElementIndex"), &i, 4); memcpy(q + off(fSetMatC, "Material"), &m, 8);
    api->process_event(c, fSetMatC, q);
}

// A block the game's allocator owns (the buffer of a padded FString it builds), so the game may free it later.
static uintptr_t gameAlloc(size_t bytes) {
    unsigned char b[0x40] = {0};
    struct { const wchar_t* data; int num, max; } none = {L"", 1, 1};
    memcpy(b + off(fPadC, "SourceString"), &none, sizeof none);
    int chars = (int)(bytes / 2) + 8;
    memcpy(b + off(fPadC, "ChCount"), &chars, 4);
    api->process_event(strCDO, fPadC, b);
    uintptr_t data = get<uintptr_t>((uintptr_t)b + off(fPadC, "ReturnValue"));
    int max = get<int>((uintptr_t)b + off(fPadC, "ReturnValue") + 12);
    return data && (size_t)max * 2 >= bytes ? data : 0;
}
// Adds the names the car's DecalableMaterialSlotNames lacks. False if it could not.
static bool listNames(uintptr_t v, const std::vector<uint64_t>& want) {
    uintptr_t at = v + oDecalNames;
    Arr a = get<Arr>(at);
    if (a.num < 0 || a.num > a.max || a.max > 4096) return false;
    std::vector<uint64_t> have(a.num);
    if (a.num) rd(a.data, have.data(), 8ull * a.num);
    bool grew = false;
    for (uint64_t n : want)
        if (std::find(have.begin(), have.end(), n) == have.end()) { have.push_back(n); grew = true; }
    if (!grew) return true;
    if ((int)have.size() > a.max) {
        // shortcut: the old block stays allocated (FMemory::Free is not exposed); once per car, a few dozen bytes
        uintptr_t block = gameAlloc(8 * (have.size() + 8));
        if (!block) return false;
        a.data = block; a.max = (int)have.size() + 8;
    }
    wr(a.data, have.data(), 8 * have.size());
    a.num = (int)have.size();
    wr(at, &a, sizeof a);
    return true;
}

static Copy copyOnCar(uintptr_t car, uintptr_t src, int nmat) {
    alignas(16) unsigned char p[0x100] = {0};
    int at = off(fAddC, "RelativeTransform");
    double rot[4] = {0, 0, 0, 1}, one[3] = {1, 1, 1};
    memcpy(p + off(fAddC, "Class"), &cSmc, 8);
    memcpy(p + at, rot, sizeof rot);
    memcpy(p + at + 0x40, one, sizeof one);
    p[off(fAddC, "bDeferredFinish")] = 1;
    api->process_event(car, fAddC, p);
    uintptr_t c = get<uintptr_t>((uintptr_t)p + off(fAddC, "ReturnValue"));
    if (!c) return {};
    uint8_t movable = 2, none = 0;
    api->process_event(c, fMobC, &movable);
    api->process_event(c, fCollC, &none);
    unsigned char m[0x10] = {0};
    uintptr_t mesh = get<uintptr_t>(src + oMesh);
    memcpy(m, &mesh, 8);
    api->process_event(c, fSetMeshC, m);
    Copy k; k.src = refOf(src); k.prx = refOf(c);
    for (int i = 0; i < nmat; i++) { uintptr_t mt = material(src, i); setMat(c, i, mt); k.mats.push_back(mt); }
    neverCull(c);
    alignas(16) unsigned char f[0x100] = {0};
    memcpy(f + off(fFinishC, "Component"), &c, 8);
    memcpy(f + off(fFinishC, "RelativeTransform"), p + at, 0x60);
    api->process_event(car, fFinishC, f);
    unsigned char a[0x40] = {0};  // onto the part's mesh, KeepRelative x3 at identity: exactly where it is
    memcpy(a + off(fAttachC, "Parent"), &src, 8);
    api->process_event(c, fAttachC, a);
    show(src, false);
    return k;
}

// Builds the copies for one attachment. True when the car needs a re-bake.
static bool build(Part& p, uintptr_t car) {
    static uintptr_t cActorComp = api->find_object("Class", "ActorComponent", "/Script/Engine");
    for (uintptr_t c : comps(p.actor.ptr, cActorComp)) {
        std::string k = objName(classOf(c));
        if (k == "BP_DWDecalable_C" || k == "BPC_SlideDecals_C" || k == "BPC_QXDecalable_C") { p.skip = true; return false; }  // they do their own
    }
    for (uintptr_t s : comps(p.actor.ptr, cSmc)) {
        if (!get<uintptr_t>(s + oMesh) || !visible(s)) continue;
        unsigned char r[0x10] = {0};
        api->process_event(s, fNumMatC, r);
        int n = get<int>((uintptr_t)r + off(fNumMatC, "ReturnValue"));
        if (n <= 0 || n > 64) continue;
        unsigned char q[0x20] = {0};
        api->process_event(s, fSlotsC, q);
        Arr names; memcpy(&names, q + off(fSlotsC, "ReturnValue"), sizeof names);
        std::vector<uint64_t> slot(names.data && names.num > 0 && names.num <= 64 ? names.num : 0);
        if (!slot.empty()) rd(names.data, slot.data(), 8 * slot.size());
        std::vector<uint64_t> mine;
        for (int i = 0; i < n && i < (int)slot.size(); i++)
            if (slot[i] && paintable(material(s, i))) mine.push_back(slot[i]);
        if (mine.empty()) continue;
        Copy k = copyOnCar(car, s, n);
        if (!k.prx.ptr) continue;
        p.copies.push_back(k);
        for (uint64_t x : mine) if (std::find(p.names.begin(), p.names.end(), x) == p.names.end()) p.names.push_back(x);
    }
    if (p.copies.empty()) { p.skip = true; return false; }
    if (!listNames(car, p.names)) MT_LOG("decals: could not extend %s's decal slot list", objName(classOf(car)).c_str());
    MT_LOG("decals: %s on %s -- %d mesh(es), %d slot name(s)", objName(classOf(p.actor.ptr)).c_str(),
           objName(classOf(car)).c_str(), (int)p.copies.size(), (int)p.names.size());
    return true;
}

// Keeps a part's copies in step: its paint, and the part's own mesh hidden. True when the car needs a re-bake.
static bool follow(Part& p) {
    bool changed = false;
    for (auto& k : p.copies) {
        if (!alive(k.src) || !alive(k.prx)) continue;
        if (visible(k.src.ptr)) show(k.src.ptr, false);
        for (int i = 0; i < (int)k.mats.size(); i++) {
            uintptr_t m = material(k.src.ptr, i);
            if (m && m != k.mats[i]) { setMat(k.prx.ptr, i, m); k.mats[i] = m; changed = true; }
        }
    }
    return changed;
}

static void rebake(uintptr_t car) { api->process_event(car, fRepCustom, nullptr); }

static void shadow(uintptr_t actor, bool on) {
    for (uintptr_t cls : {cSpot, cRect}) {
        if (!cls) continue;
        for (uintptr_t l : comps(actor, cls)) {
            auto it = lights.find(l);
            if (it != lights.end() && alive(it->second.first) && it->second.second == (int)on) continue;
            unsigned char p[0x10] = {0};
            p[off(fLightShadow, "bNewValue")] = on;
            api->process_event(l, fLightShadow, p);
            lights[l] = {refOf(l), (int)on};
        }
    }
}

static void pass() {
    DWORD now = GetTickCount();
    if (now - lastPass < 2000) return;
    lastPass = now;
    int q = shadowsOn ? cvar(L"sg.ShadowQuality") : -1;
    bool cinematic = q >= 4;
    std::set<uintptr_t> seen;
    for (auto& v : cars) {
        if (!alive(v) || get<long long>(v.ptr + oVehIdC) <= 0) continue;  // the garage's preview copy: built and torn down by the UI
        Arr a = get<Arr>(v.ptr + oParts);
        bool bake = false;
        for (int i = 0; a.data && i < a.num && i < 64; i++) {
            uintptr_t c = get<uintptr_t>(a.data + 8ull * i);
            uintptr_t actor = c ? get<uintptr_t>(c + oChild) : 0;
            if (!actor) continue;
            seen.insert(actor);
            if (shadowsOn && q >= 0) shadow(actor, cinematic);
            if (!decalsOn) continue;
            auto it = parts.find(actor);
            if (it != parts.end() && !alive(it->second.actor)) { parts.erase(it); it = parts.end(); }
            if (it == parts.end()) {
                Part p; p.actor = refOf(actor); p.veh = v;
                bake |= build(p, v.ptr);
                parts[actor] = std::move(p);
            } else if (!it->second.skip) {
                bake |= follow(it->second);
                if (!it->second.names.empty() && it->second.copies.size()) listNames(v.ptr, it->second.names);  // the game rebuilds the list on a part change
            }
        }
        if (bake) rebake(v.ptr);
    }
    for (auto it = parts.begin(); it != parts.end();) {  // uninstalled: its copies go, the car re-bakes
        if (alive(it->second.actor) && seen.count(it->first)) { ++it; continue; }
        bool any = false;
        for (auto& k : it->second.copies) if (alive(k.prx)) { api->process_event(k.prx.ptr, fDestroyC, &k.prx.ptr); any = true; }
        if (any && alive(it->second.veh)) rebake(it->second.veh.ptr);
        it = parts.erase(it);
    }
    for (auto it = lights.begin(); it != lights.end();) it = alive(it->second.first) ? std::next(it) : lights.erase(it);
}

static bool resolve(uintptr_t veh) {
    decalsOn = iniInt(L"attachment_tweaks.ini", L"Decals", L"Enabled", 1) != 0;
    shadowsOn = iniInt(L"attachment_tweaks.ini", L"Shadows", L"CinematicOnly", 1) != 0;
    cVehicle = veh;
    cSmc = api->find_object("Class", "StaticMeshComponent", "/Script/Engine");
    cSpot = api->find_object("Class", "SpotLightComponent", "/Script/Engine");
    cRect = api->find_object("Class", "RectLightComponent", "/Script/Engine");
    cMI = api->find_object("Class", "MaterialInstance", "/Script/Engine");
    fAddC = api->find_object("Function", "AddComponentByClass", "Actor");
    fFinishC = api->find_object("Function", "FinishAddComponent", "Actor");
    fCompsC = api->find_object("Function", "K2_GetComponentsByClass", "Actor");
    fSetMeshC = api->find_object("Function", "SetStaticMesh", "StaticMeshComponent");
    fMobC = api->find_object("Function", "SetMobility", "SceneComponent");
    fAttachC = api->find_object("Function", "K2_AttachToComponent", "SceneComponent");
    fVisC = api->find_object("Function", "SetVisibility", "SceneComponent");
    fIsVisC = api->find_object("Function", "IsVisible", "SceneComponent");
    fCollC = api->find_object("Function", "SetCollisionEnabled", "PrimitiveComponent");
    fSetMatC = api->find_object("Function", "SetMaterial", "PrimitiveComponent");
    fGetMatC = api->find_object("Function", "GetMaterial", "PrimitiveComponent");
    fNumMatC = api->find_object("Function", "GetNumMaterials", "PrimitiveComponent");
    fSlotsC = api->find_object("Function", "GetMaterialSlotNames", "MeshComponent");
    fDestroyC = api->find_object("Function", "K2_DestroyComponent", "ActorComponent");
    fRepCustom = api->find_object("Function", "OnRep_Customization", "MTVehicle");
    fLightShadow = api->find_object("Function", "SetCastShadows", "LightComponent");
    if (!fLightShadow) fLightShadow = api->find_object("Function", "SetCastShadows", "LightComponentBase");
    fPadC = api->find_object("Function", "RightPad", "KismetStringLibrary");
    strCDO = api->find_object("KismetStringLibrary", "Default__KismetStringLibrary", "/Script/Engine");
    cvarFn = api->find_object("Function", "GetConsoleVariableIntValue", "KismetSystemLibrary");
    sysCDO = api->find_object("KismetSystemLibrary", "Default__KismetSystemLibrary", "/Script/Engine");
    oParts = off(veh, "AttachmentPartsComponents");
    oChild = off(api->find_object("Class", "ChildActorComponent", "/Script/Engine"), "ChildActor");
    oDecalNames = off(veh, "DecalableMaterialSlotNames");
    oMesh = off(cSmc, "StaticMesh");
    oParentMat = off(cMI, "Parent");
    oVehIdC = off(veh, "Net_VehicleId");
    bool common = cSmc && fCompsC && oParts >= 0 && oChild >= 0 && oVehIdC >= 0;
    if (!(common && fLightShadow && cSpot && sysCDO && cvarFn)) shadowsOn = false;
    if (!(common && cMI && fAddC && fFinishC && fSetMeshC && fMobC && fAttachC && fVisC && fIsVisC && fCollC && fSetMatC && fGetMatC
          && fNumMatC && fSlotsC && fDestroyC && fRepCustom && fPadC && strCDO && oDecalNames >= 0 && oMesh >= 0 && oParentMat >= 0))
        decalsOn = false;
    MT_LOG("decals on attachments %s, attachment light shadows %s", decalsOn ? "on" : "OFF", shadowsOn ? "on Cinematic only" : "left as they are");
    return decalsOn || shadowsOn;
}

}  // namespace extras

// ================================================================
//  RedmptionGUI.LOL
//  Made By Vr4se And GrandpaJoe
//
//  God-tier GUI mod lib for Meta Quest — SlapLab (Unity 2022)
//  Drop into: lib/arm64-v8a/libRedmptionGUI.so
//
//  Features:
//    [1] Fly          — gravity off, thumbstick vertical flight
//    [2] ESP          — player boxes + distance + health
//    [3] GODMODE      — block all incoming damage
//    [4] Knockout     — one-hit kill on any player
//    [5] Noclip       — walk through walls (collision disable)
//    [6] Spawn Prefab — spawn Unity GameObjects by name
//    [7] Gun          — force-launch physics on any player
//    [8] Kick Gun     — massive knock back punch
//    [9] Ban Gun      — kick player from session (local RPC)
// ================================================================

#include <jni.h>
#include <android/log.h>
#include <pthread.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <sys/mman.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/syscall.h>

// ── LOGGING ──────────────────────────────────────────────────────
#define TAG     "RedmptionGUI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)

// ── UNITY TYPES ──────────────────────────────────────────────────
// Matches Unity 2022 ARM64 IL2CPP memory layout

struct Vector3 {
    float x, y, z;
    Vector3() : x(0), y(0), z(0) {}
    Vector3(float x, float y, float z) : x(x), y(y), z(z) {}
    Vector3 operator+(const Vector3& o) const { return {x+o.x, y+o.y, z+o.z}; }
    Vector3 operator*(float s) const { return {x*s, y*s, z*s}; }
    float magnitude() const { return sqrtf(x*x + y*y + z*z); }
    float distance(const Vector3& o) const {
        float dx=x-o.x, dy=y-o.y, dz=z-o.z;
        return sqrtf(dx*dx + dy*dy + dz*dz);
    }
};

struct Vector2 { float x, y; };
struct Quaternion { float x, y, z, w; };

struct Color {
    float r, g, b, a;
    static Color Red()     { return {1,0,0,1}; }
    static Color Green()   { return {0,1,0,1}; }
    static Color Blue()    { return {0,0,1,1}; }
    static Color Yellow()  { return {1,1,0,1}; }
    static Color White()   { return {1,1,1,1}; }
    static Color Cyan()    { return {0,1,1,1}; }
    static Color Orange()  { return {1,.5f,0,1}; }
    static Color Purple()  { return {.6f,0,.8f,1}; }
};

// IL2CPP string (UTF-16 on heap, length prefix)
struct Il2CppString {
    void* klass;
    void* monitor;
    int32_t length;
    uint16_t chars[1]; // variable length UTF-16
};

// IL2CPP array header
struct Il2CppArray {
    void* klass;
    void* monitor;
    void* bounds;
    int32_t max_length;
    // elements follow
};

// ── MEMORY UTILS ─────────────────────────────────────────────────

namespace Mem {
    bool Unprotect(void* addr, size_t len) {
        uintptr_t page = (uintptr_t)addr & ~(uintptr_t)(getpagesize()-1);
        return mprotect((void*)page, len + getpagesize()*2,
                        PROT_READ|PROT_WRITE|PROT_EXEC) == 0;
    }

    void Write(void* dst, const void* src, size_t len) {
        Unprotect(dst, len);
        memcpy(dst, src, len);
        __builtin___clear_cache((char*)dst, (char*)dst+len);
    }

    // ARM64 absolute trampoline: ldr x17,#8; br x17; <addr>
    void Hook(void* target, void* replacement) {
        if (!target || !replacement) return;
        uint8_t stub[16];
        *(uint32_t*)(stub+0) = 0x58000051u; // ldr x17, #8
        *(uint32_t*)(stub+4) = 0xD61F0220u; // br  x17
        *(uint64_t*)(stub+8) = (uint64_t)replacement;
        Write(target, stub, 16);
        LOGI("[Hook] %p -> %p", target, replacement);
    }

    void NOP(void* addr, int n) {
        uint32_t nop = 0xD503201Fu;
        for (int i = 0; i < n; i++)
            Write((uint8_t*)addr + i*4, &nop, 4);
    }

    uintptr_t GetLibBase(const char* name) {
        FILE* f = fopen("/proc/self/maps", "r");
        if (!f) return 0;
        char line[512];
        uintptr_t base = 0;
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, name) && strstr(line, "r-xp")) {
                base = (uintptr_t)strtoull(line, nullptr, 16);
                break;
            }
        }
        fclose(f);
        LOGI("[Maps] %s -> 0x%lx", name, base);
        return base;
    }
}

// ── IL2CPP RUNTIME BINDINGS ──────────────────────────────────────

namespace IL2CPP {
    // Function pointer typedefs
    typedef void*  (*fn_domain_get)();
    typedef void** (*fn_domain_get_assemblies)(void* domain, size_t* count);
    typedef void*  (*fn_assembly_get_image)(void* assembly);
    typedef void*  (*fn_class_from_name)(void* image, const char* ns, const char* name);
    typedef void*  (*fn_class_get_method)(void* klass, const char* name, int argc);
    typedef void*  (*fn_method_get_pointer)(void* method);
    typedef void*  (*fn_object_new)(void* domain, void* klass);
    typedef void   (*fn_runtime_invoke)(void* method, void* obj, void** params, void** exc);
    typedef void*  (*fn_string_new)(void* domain, const char* str);
    typedef int32_t(*fn_array_length)(void* array);
    typedef void*  (*fn_array_get)(void* array, int32_t idx);

    static fn_domain_get          domain_get          = nullptr;
    static fn_domain_get_assemblies domain_assemblies = nullptr;
    static fn_assembly_get_image  assembly_image      = nullptr;
    static fn_class_from_name     class_from_name     = nullptr;
    static fn_class_get_method    class_get_method    = nullptr;
    static fn_method_get_pointer  method_get_ptr      = nullptr;
    static fn_object_new          object_new          = nullptr;
    static fn_runtime_invoke      runtime_invoke      = nullptr;
    static fn_string_new          string_new          = nullptr;

    static void* g_domain = nullptr;
    static bool  g_ready  = false;

    bool Init() {
        domain_get       = (fn_domain_get)       dlsym(RTLD_DEFAULT, "il2cpp_domain_get");
        domain_assemblies= (fn_domain_get_assemblies)dlsym(RTLD_DEFAULT,"il2cpp_domain_get_assemblies");
        assembly_image   = (fn_assembly_get_image)  dlsym(RTLD_DEFAULT, "il2cpp_assembly_get_image");
        class_from_name  = (fn_class_from_name)     dlsym(RTLD_DEFAULT, "il2cpp_class_from_name");
        class_get_method = (fn_class_get_method)    dlsym(RTLD_DEFAULT, "il2cpp_class_get_method_from_name");
        method_get_ptr   = (fn_method_get_pointer)  dlsym(RTLD_DEFAULT, "il2cpp_method_get_pointer");
        object_new       = (fn_object_new)          dlsym(RTLD_DEFAULT, "il2cpp_object_new");
        runtime_invoke   = (fn_runtime_invoke)      dlsym(RTLD_DEFAULT, "il2cpp_runtime_invoke");
        string_new       = (fn_string_new)          dlsym(RTLD_DEFAULT, "il2cpp_string_new");

        if (!domain_get || !class_from_name || !class_get_method) {
            LOGE("[IL2CPP] Init FAILED — exports not found");
            return false;
        }

        g_domain = domain_get();
        g_ready  = (g_domain != nullptr);
        LOGI("[IL2CPP] Init %s domain=%p", g_ready ? "OK" : "FAIL", g_domain);
        return g_ready;
    }

    // Resolve a class from any assembly
    void* FindClass(const char* ns, const char* name) {
        if (!g_ready || !domain_assemblies) return nullptr;
        size_t count = 0;
        void** assemblies = domain_assemblies(g_domain, &count);
        for (size_t i = 0; i < count; i++) {
            void* img = assembly_image(assemblies[i]);
            if (!img) continue;
            void* klass = class_from_name(img, ns, name);
            if (klass) return klass;
        }
        return nullptr;
    }

    // Get method pointer ready to call as a function
    void* GetMethod(const char* ns, const char* className, const char* method, int argc) {
        void* klass = FindClass(ns, className);
        if (!klass) { LOGE("[IL2CPP] Class not found: %s.%s", ns, className); return nullptr; }
        void* m = class_get_method(klass, method, argc);
        if (!m)  { LOGE("[IL2CPP] Method not found: %s.%s::%s", ns, className, method); return nullptr; }
        void* ptr = method_get_ptr(m);
        LOGI("[IL2CPP] %s.%s::%s -> %p", ns, className, method, ptr);
        return ptr;
    }

    // Invoke a method via runtime_invoke (for methods needing thunk)
    void* Invoke(void* method_ptr, void* obj, void** params) {
        if (!runtime_invoke || !method_ptr) return nullptr;
        void* exc = nullptr;
        runtime_invoke(method_ptr, obj, params, &exc);
        return exc;
    }

    // Create a managed string
    void* MakeString(const char* str) {
        if (!string_new || !g_domain) return nullptr;
        return string_new(g_domain, str);
    }
}

// ── GUI STATE ────────────────────────────────────────────────────

struct GuiState {
    // Feature toggles
    bool fly            = false;
    bool esp            = false;
    bool godMode        = false;
    bool knockoutInst   = false;
    bool noclip         = false;
    bool spawnPrefab    = false;
    bool gun            = false;
    bool kickGun        = false;
    bool banGun         = false;

    // Config values
    float flySpeed      = 5.0f;
    float gunForce      = 50.0f;
    float kickForce     = 80.0f;
    float espMaxDist    = 100.0f;
    const char* spawnName = "SpawnItem_Default";

    // Runtime
    bool  menuVisible   = true;
    int   selectedItem  = 0;
    bool  initialized   = false;

    // VR trigger state (thumbstick for fly)
    float rightStickY   = 0.0f;
    bool  triggerPressed= false;
} g_gui;

// ── UNITY FUNCTION POINTERS (SlapLab specific) ───────────────────
// These are resolved at runtime via IL2CPP or offset scanning

namespace SlapLab {
    // === Player ===
    typedef void*    (*fn_GetLocalPlayer)();
    typedef void*    (*fn_GetAllPlayers)();          // returns Il2CppArray*
    typedef Vector3  (*fn_GetPosition)(void* obj);
    typedef void     (*fn_SetPosition)(void* obj, Vector3 pos);
    typedef Vector3  (*fn_GetForward)(void* transform);
    typedef void*    (*fn_GetTransform)(void* obj);
    typedef void     (*fn_AddForce)(void* rb, Vector3 force, int mode); // ForceMode.Impulse=1
    typedef void*    (*fn_GetRigidbody)(void* obj);
    typedef void     (*fn_SetGravity)(void* rb, bool useGravity);
    typedef void     (*fn_SetKinematic)(void* rb, bool isKinematic);

    // === Health / Damage ===
    typedef float    (*fn_GetHealth)(void* player);
    typedef void     (*fn_SetHealth)(void* player, float hp);
    typedef void     (*fn_TakeDamage)(void* player, float dmg, void* attacker);

    // === Collision ===
    typedef void*    (*fn_GetCollider)(void* obj);
    typedef void     (*fn_SetEnabled)(void* coll, bool enabled);

    // === Network / Room ===
    typedef void     (*fn_KickPlayer)(void* room, void* player);  // Photon RPC
    typedef void*    (*fn_GetPhotonView)(void* player);
    typedef int32_t  (*fn_GetViewID)(void* view);

    // === Unity Object ===
    typedef void*    (*fn_Instantiate)(void* prefab, Vector3 pos, Quaternion rot);
    typedef void*    (*fn_Find)(void* name);
    typedef void*    (*fn_FindWithTag)(void* tag);

    // === Camera / VR ===
    typedef void*    (*fn_GetMainCamera)();
    typedef void*    (*fn_GetVRHand)(void* rig, int hand);  // 0=left, 1=right

    // Resolved pointers
    static fn_GetLocalPlayer  pGetLocalPlayer  = nullptr;
    static fn_GetAllPlayers   pGetAllPlayers   = nullptr;
    static fn_GetPosition     pGetPosition     = nullptr;
    static fn_SetPosition     pSetPosition     = nullptr;
    static fn_GetForward      pGetForward      = nullptr;
    static fn_GetTransform    pGetTransform    = nullptr;
    static fn_AddForce        pAddForce        = nullptr;
    static fn_GetRigidbody    pGetRigidbody    = nullptr;
    static fn_SetGravity      pSetGravity      = nullptr;
    static fn_SetKinematic    pSetKinematic    = nullptr;
    static fn_GetHealth       pGetHealth       = nullptr;
    static fn_SetHealth       pSetHealth       = nullptr;
    static fn_TakeDamage      pTakeDamage      = nullptr;
    static fn_GetCollider     pGetCollider     = nullptr;
    static fn_SetEnabled      pSetCollEnabled  = nullptr;
    static fn_KickPlayer      pKickPlayer      = nullptr;
    static fn_GetPhotonView   pGetPhotonView   = nullptr;
    static fn_GetViewID       pGetViewID       = nullptr;
    static fn_Instantiate     pInstantiate     = nullptr;
    static fn_Find            pFind            = nullptr;
    static fn_GetMainCamera   pGetMainCamera   = nullptr;

    // Original function pointers (for hooks)
    static fn_TakeDamage   orig_TakeDamage  = nullptr;
    static fn_SetGravity   orig_SetGravity  = nullptr;
    static fn_SetEnabled   orig_SetColl     = nullptr;

    void Resolve() {
        LOGI("[SlapLab] Resolving functions...");

        // == Player ==
        pGetLocalPlayer = (fn_GetLocalPlayer) IL2CPP::GetMethod("", "PlayerController", "GetLocalPlayer", 0);
        pGetAllPlayers  = (fn_GetAllPlayers)  IL2CPP::GetMethod("", "PlayerManager",    "GetAllPlayers",  0);

        // Alternate class names SlapLab might use
        if (!pGetLocalPlayer)
            pGetLocalPlayer = (fn_GetLocalPlayer) IL2CPP::GetMethod("", "Player", "GetLocalPlayer", 0);
        if (!pGetAllPlayers)
            pGetAllPlayers = (fn_GetAllPlayers) IL2CPP::GetMethod("", "GameManager", "GetPlayers", 0);

        // == Transform (UnityEngine namespace) ==
        pGetPosition = (fn_GetPosition) IL2CPP::GetMethod("UnityEngine", "Transform", "get_position", 0);
        pSetPosition = (fn_SetPosition) IL2CPP::GetMethod("UnityEngine", "Transform", "set_position", 1);
        pGetForward  = (fn_GetForward)  IL2CPP::GetMethod("UnityEngine", "Transform", "get_forward",  0);
        pGetTransform= (fn_GetTransform)IL2CPP::GetMethod("UnityEngine", "Component", "get_transform", 0);

        // == Physics ==
        pAddForce    = (fn_AddForce)    IL2CPP::GetMethod("UnityEngine", "Rigidbody", "AddForce",        2);
        pGetRigidbody= (fn_GetRigidbody)IL2CPP::GetMethod("UnityEngine", "Component", "GetComponent", 0);
        pSetGravity  = (fn_SetGravity)  IL2CPP::GetMethod("UnityEngine", "Rigidbody", "set_useGravity", 1);
        pSetKinematic= (fn_SetKinematic)IL2CPP::GetMethod("UnityEngine", "Rigidbody", "set_isKinematic",1);

        // == Health ==
        pGetHealth   = (fn_GetHealth)   IL2CPP::GetMethod("", "PlayerController", "get_Health",     0);
        pSetHealth   = (fn_SetHealth)   IL2CPP::GetMethod("", "PlayerController", "set_Health",     1);
        pTakeDamage  = (fn_TakeDamage)  IL2CPP::GetMethod("", "PlayerController", "TakeDamage",     2);

        // Try alternate health class names
        if (!pGetHealth) pGetHealth = (fn_GetHealth) IL2CPP::GetMethod("", "HealthManager", "GetHealth", 0);
        if (!pSetHealth) pSetHealth = (fn_SetHealth) IL2CPP::GetMethod("", "HealthManager", "SetHealth", 1);
        if (!pTakeDamage)pTakeDamage= (fn_TakeDamage)IL2CPP::GetMethod("", "HealthManager", "TakeDamage",2);

        // == Collision ==
        pGetCollider   = (fn_GetCollider)  IL2CPP::GetMethod("UnityEngine","Collider","GetComponent",0);
        pSetCollEnabled= (fn_SetEnabled)   IL2CPP::GetMethod("UnityEngine","Collider","set_enabled", 1);

        // == Network (Photon) ==
        pKickPlayer    = (fn_KickPlayer)   IL2CPP::GetMethod("Photon.Pun",  "PhotonNetwork", "CloseConnection", 1);
        if (!pKickPlayer)
            pKickPlayer = (fn_KickPlayer)  IL2CPP::GetMethod("", "RoomManager", "KickPlayer", 1);
        pGetPhotonView = (fn_GetPhotonView)IL2CPP::GetMethod("Photon.Pun", "PhotonView", "Get", 1);
        pGetViewID     = (fn_GetViewID)    IL2CPP::GetMethod("Photon.Pun", "PhotonView", "get_ViewID", 0);

        // == Spawn ==
        pInstantiate   = (fn_Instantiate)  IL2CPP::GetMethod("UnityEngine", "Object", "Instantiate", 3);
        pFind          = (fn_Find)         IL2CPP::GetMethod("UnityEngine", "GameObject", "Find", 1);

        // == Camera ==
        pGetMainCamera = (fn_GetMainCamera)IL2CPP::GetMethod("UnityEngine", "Camera", "get_main", 0);

        LOGI("[SlapLab] Resolve complete. Key ptrs: local=%p all=%p pos=%p",
             pGetLocalPlayer, pGetAllPlayers, pGetPosition);
    }
}

// ── HOOK IMPLEMENTATIONS ─────────────────────────────────────────

// GODMODE: Block all incoming damage to local player
void hk_TakeDamage(void* player, float dmg, void* attacker) {
    if (g_gui.godMode) {
        LOGD("[GodMode] Blocked %.1f dmg", dmg);
        return;
    }
    if (SlapLab::orig_TakeDamage)
        SlapLab::orig_TakeDamage(player, dmg, attacker);
}

// FLY: Intercept gravity update on local player's rigidbody
// We handle this in the update loop instead of hooking SetGravity
// to avoid breaking other physics objects

// NOCLIP: Collider hook
void hk_SetColliderEnabled(void* coll, bool enabled) {
    // Don't let the game re-enable our collider while noclip is on
    if (g_gui.noclip && !enabled) return; // already off, stay off
    if (g_gui.noclip && enabled) return;  // game tries to enable, block it
    if (SlapLab::orig_SetColl)
        SlapLab::orig_SetColl(coll, enabled);
}

// ── FEATURE IMPLEMENTATIONS ──────────────────────────────────────

namespace Features {

    // Cache the local player pointer
    static void* g_localPlayer  = nullptr;
    static void* g_localRb      = nullptr;
    static void* g_localTform   = nullptr;
    static bool  g_noclipWasOn  = false;

    void* GetLocal() {
        if (g_localPlayer) return g_localPlayer;
        if (SlapLab::pGetLocalPlayer) {
            g_localPlayer = SlapLab::pGetLocalPlayer();
            LOGI("[Features] LocalPlayer = %p", g_localPlayer);
        }
        return g_localPlayer;
    }

    // ── [1] FLY ──────────────────────────────────────────────
    // Called every frame: when fly is on, zero gravity and move vertically
    void UpdateFly() {
        if (!g_gui.fly) {
            // Restore gravity if we just turned off fly
            if (g_localRb && SlapLab::pSetGravity) {
                SlapLab::pSetGravity(g_localRb, true);
                g_localRb = nullptr;
            }
            return;
        }

        void* player = GetLocal();
        if (!player) return;

        // Get rigidbody
        if (!g_localRb && SlapLab::pGetRigidbody)
            g_localRb = SlapLab::pGetRigidbody(player);
        if (!g_localRb) return;

        // Kill gravity
        if (SlapLab::pSetGravity)
            SlapLab::pSetGravity(g_localRb, false);

        // Apply vertical velocity from thumbstick
        float vy = g_gui.rightStickY * g_gui.flySpeed;
        if (fabsf(vy) > 0.01f && SlapLab::pAddForce) {
            Vector3 up = {0, vy, 0};
            SlapLab::pAddForce(g_localRb, up, 1); // ForceMode.Impulse
        }
    }

    // ── [2] ESP ──────────────────────────────────────────────
    struct ESPTarget {
        char  name[64];
        float dist;
        float health;
        Vector3 worldPos;
        bool  isValid;
    };

    static std::vector<ESPTarget> g_espTargets;

    void UpdateESP() {
        if (!g_gui.esp) { g_espTargets.clear(); return; }
        g_espTargets.clear();

        if (!SlapLab::pGetAllPlayers) return;
        Il2CppArray* arr = (Il2CppArray*)SlapLab::pGetAllPlayers();
        if (!arr) return;

        void* local = GetLocal();
        Vector3 myPos = {};
        if (local && SlapLab::pGetTransform && SlapLab::pGetPosition) {
            void* tf = SlapLab::pGetTransform(local);
            if (tf) myPos = SlapLab::pGetPosition(tf);
        }

        int count = arr->max_length;
        void** items = (void**)((uint8_t*)arr + sizeof(Il2CppArray));

        for (int i = 0; i < count && i < 32; i++) {
            void* p = items[i];
            if (!p || p == local) continue;

            ESPTarget t;
            t.isValid = true;
            t.health  = SlapLab::pGetHealth ? SlapLab::pGetHealth(p) : 100.f;

            void* tf = SlapLab::pGetTransform ? SlapLab::pGetTransform(p) : nullptr;
            if (tf && SlapLab::pGetPosition) {
                t.worldPos = SlapLab::pGetPosition(tf);
                t.dist     = myPos.distance(t.worldPos);
            }

            if (t.dist > g_gui.espMaxDist) continue;
            snprintf(t.name, sizeof(t.name), "Player[%d] %.0fm HP:%.0f",
                     i, t.dist, t.health);
            g_espTargets.push_back(t);
        }

        LOGD("[ESP] %zu targets tracked", g_espTargets.size());
    }

    // ── [4] KNOCKOUT INSTANTLY ───────────────────────────────
    // One-hit kill: set any target's health to 0 or apply massive damage
    void KnockoutAll() {
        if (!SlapLab::pGetAllPlayers) return;
        Il2CppArray* arr = (Il2CppArray*)SlapLab::pGetAllPlayers();
        if (!arr) return;

        void* local = GetLocal();
        void** items = (void**)((uint8_t*)arr + sizeof(Il2CppArray));
        int count = arr->max_length;

        for (int i = 0; i < count && i < 32; i++) {
            void* p = items[i];
            if (!p || p == local) continue;

            if (SlapLab::pSetHealth)
                SlapLab::pSetHealth(p, 0.f);
            else if (SlapLab::pTakeDamage)
                SlapLab::pTakeDamage(p, 999999.f, local);

            LOGI("[Knockout] Hit player[%d]", i);
        }
    }

    // ── [5] NOCLIP ───────────────────────────────────────────
    void SetNoclip(bool on) {
        void* player = GetLocal();
        if (!player) return;

        // Disable/enable all colliders on the local player
        // Use GetComponentsInChildren approach via direct method
        if (SlapLab::pGetCollider && SlapLab::pSetCollEnabled) {
            void* coll = SlapLab::pGetCollider(player);
            if (coll) SlapLab::pSetCollEnabled(coll, !on);
        }

        // Also set kinematic to stop physics bumping while noclipping
        if (!g_localRb && SlapLab::pGetRigidbody)
            g_localRb = SlapLab::pGetRigidbody(player);
        if (g_localRb && SlapLab::pSetKinematic)
            SlapLab::pSetKinematic(g_localRb, on);

        g_noclipWasOn = on;
        LOGI("[Noclip] %s", on ? "ON" : "OFF");
    }

    // ── [6] SPAWN PREFAB ─────────────────────────────────────
    void SpawnPrefab(const char* name) {
        if (!SlapLab::pFind || !SlapLab::pInstantiate) {
            LOGE("[Spawn] pFind or pInstantiate null");
            return;
        }

        void* nameStr = IL2CPP::MakeString(name);
        if (!nameStr) return;

        void* prefab = SlapLab::pFind(nameStr);
        if (!prefab) {
            LOGE("[Spawn] Prefab '%s' not found in scene", name);
            return;
        }

        // Spawn near the local player
        void* local = GetLocal();
        Vector3 spawnPos = {0, 1, 2}; // default ahead of origin
        if (local && SlapLab::pGetTransform && SlapLab::pGetPosition) {
            void* tf = SlapLab::pGetTransform(local);
            if (tf) {
                Vector3 pos = SlapLab::pGetPosition(tf);
                Vector3 fwd = SlapLab::pGetForward ? SlapLab::pGetForward(tf) : Vector3{0,0,1};
                spawnPos = pos + fwd * 2.0f;
            }
        }

        Quaternion rot = {0, 0, 0, 1};
        void* spawned = SlapLab::pInstantiate(prefab, spawnPos, rot);
        LOGI("[Spawn] Spawned '%s' -> %p at (%.1f,%.1f,%.1f)",
             name, spawned, spawnPos.x, spawnPos.y, spawnPos.z);
    }

    // ── [7] GUN — physics force launcher ─────────────────────
    // Shoots a physics impulse at the nearest player (like a force gun)
    void FireGun() {
        if (!g_gui.gun) return;
        if (!SlapLab::pGetAllPlayers) return;

        Il2CppArray* arr = (Il2CppArray*)SlapLab::pGetAllPlayers();
        if (!arr) return;

        void* local = GetLocal();
        void** items = (void**)((uint8_t*)arr + sizeof(Il2CppArray));
        int count = arr->max_length;

        // Find nearest player
        void*  nearest = nullptr;
        float  nearDist = 999999.f;
        void*  nearTf   = nullptr;
        Vector3 myPos = {};

        if (local && SlapLab::pGetTransform && SlapLab::pGetPosition) {
            void* tf = SlapLab::pGetTransform(local);
            if (tf) myPos = SlapLab::pGetPosition(tf);
        }

        for (int i = 0; i < count && i < 32; i++) {
            void* p = items[i];
            if (!p || p == local) continue;
            void* tf = SlapLab::pGetTransform ? SlapLab::pGetTransform(p) : nullptr;
            if (!tf) continue;
            Vector3 pos = SlapLab::pGetPosition(tf);
            float d = myPos.distance(pos);
            if (d < nearDist) { nearDist = d; nearest = p; nearTf = tf; }
        }

        if (!nearest || !SlapLab::pGetRigidbody || !SlapLab::pAddForce) return;

        void* rb = SlapLab::pGetRigidbody(nearest);
        if (!rb) return;

        // Direction from local to target, scaled by gun force
        Vector3 targetPos = SlapLab::pGetPosition(nearTf);
        float dx = targetPos.x - myPos.x;
        float dy = targetPos.y - myPos.y;
        float dz = targetPos.z - myPos.z;
        float len = sqrtf(dx*dx + dy*dy + dz*dz);
        if (len < 0.001f) return;

        float force = g_gui.gunForce;
        Vector3 impulse = {
            dx/len * force,
            (dy/len + 0.3f) * force, // slight upward arc
            dz/len * force
        };

        SlapLab::pAddForce(rb, impulse, 1); // ForceMode.Impulse
        LOGI("[Gun] Launched player at (%.1f,%.1f,%.1f) force=%.1f",
             targetPos.x, targetPos.y, targetPos.z, force);
    }

    // ── [8] KICK GUN — massive knockback ─────────────────────
    void FireKickGun() {
        if (!g_gui.kickGun) return;
        if (!SlapLab::pGetAllPlayers || !SlapLab::pGetRigidbody || !SlapLab::pAddForce) return;

        Il2CppArray* arr = (Il2CppArray*)SlapLab::pGetAllPlayers();
        if (!arr) return;

        void* local = GetLocal();
        void** items = (void**)((uint8_t*)arr + sizeof(Il2CppArray));
        int count = arr->max_length;

        void* localTf = nullptr;
        Vector3 myPos = {}, myFwd = {0,0,1};

        if (local && SlapLab::pGetTransform) {
            localTf = SlapLab::pGetTransform(local);
            if (localTf) {
                if (SlapLab::pGetPosition) myPos = SlapLab::pGetPosition(localTf);
                if (SlapLab::pGetForward)  myFwd = SlapLab::pGetForward(localTf);
            }
        }

        // Apply to ALL players within range (huge kick)
        for (int i = 0; i < count && i < 32; i++) {
            void* p = items[i];
            if (!p || p == local) continue;
            void* rb = SlapLab::pGetRigidbody(p);
            if (!rb) continue;

            // Kick in the direction we're facing + big upward component
            float kick = g_gui.kickForce;
            Vector3 impulse = {
                myFwd.x * kick,
                kick * 0.7f,       // big upward launch
                myFwd.z * kick
            };

            SlapLab::pAddForce(rb, impulse, 1);
            LOGI("[KickGun] Kicked player[%d] with force %.1f", i, kick);
        }
    }

    // ── [9] BAN GUN — kick from session (local Photon call) ──
    // Uses PhotonNetwork.CloseConnection() — only works if we're the host
    // (SlapLab rooms are host-authoritative on the master client)
    void FireBanGun() {
        if (!g_gui.banGun) return;
        if (!SlapLab::pGetAllPlayers || !SlapLab::pKickPlayer) {
            LOGE("[BanGun] Required ptrs null");
            return;
        }

        Il2CppArray* arr = (Il2CppArray*)SlapLab::pGetAllPlayers();
        if (!arr) return;

        void* local = GetLocal();
        void** items = (void**)((uint8_t*)arr + sizeof(Il2CppArray));
        int count = arr->max_length;

        void* localTf = nullptr;
        Vector3 myPos = {};
        if (local && SlapLab::pGetTransform && SlapLab::pGetPosition) {
            localTf = SlapLab::pGetTransform(local);
            if (localTf) myPos = SlapLab::pGetPosition(localTf);
        }

        // Kick nearest player
        void*  nearest = nullptr;
        float  nearDist = 999999.f;

        for (int i = 0; i < count && i < 32; i++) {
            void* p = items[i];
            if (!p || p == local) continue;
            void* tf = SlapLab::pGetTransform ? SlapLab::pGetTransform(p) : nullptr;
            if (!tf || !SlapLab::pGetPosition) continue;
            float d = myPos.distance(SlapLab::pGetPosition(tf));
            if (d < nearDist) { nearDist = d; nearest = p; }
        }

        if (nearest) {
            LOGI("[BanGun] Kicking nearest player (dist=%.1f)", nearDist);
            SlapLab::pKickPlayer(nullptr, nearest); // PhotonNetwork.CloseConnection(player)
        }
    }

    // ── Main frame update ─────────────────────────────────────
    void Tick() {
        // Refresh local player pointer each second
        static int frame = 0;
        if (++frame % 60 == 0) g_localPlayer = nullptr;

        UpdateFly();
        UpdateESP();

        // Noclip state change
        static bool lastNoclip = false;
        if (g_gui.noclip != lastNoclip) {
            SetNoclip(g_gui.noclip);
            lastNoclip = g_gui.noclip;
        }
    }
}

// ── VR GUI OVERLAY ───────────────────────────────────────────────
// Rendered via Unity's OnGUI / IMGUI system — we hook into the
// rendering pipeline by hooking MonoBehaviour.OnGUI or using
// UnityEngine.GUI directly each frame.
//
// For Meta Quest (no display injection):
//   We use UnityEngine.Debug + logcat overlay as the "menu" channel
//   and a simple OVR-joystick-driven controller menu.
//
// The actual in-VR overlay uses Unity's GL.PushMatrix / GL.Begin
// to draw directly into the world space.

namespace GUI_Overlay {
    // Menu items
    struct MenuItem {
        const char* label;
        bool* toggle;
    };

    static MenuItem items[] = {
        { "[ ] FLY",         &g_gui.fly         },
        { "[ ] ESP",         &g_gui.esp         },
        { "[ ] GODMODE",     &g_gui.godMode     },
        { "[ ] KNOCKOUT",    &g_gui.knockoutInst},
        { "[ ] NOCLIP",      &g_gui.noclip      },
        { "[ ] SPAWN",       &g_gui.spawnPrefab },
        { "[ ] GUN",         &g_gui.gun         },
        { "[ ] KICK GUN",    &g_gui.kickGun     },
        { "[ ] BAN GUN",     &g_gui.banGun      },
    };
    static const int ITEM_COUNT = 9;

    // OVR Input function pointers (Meta XR SDK)
    typedef bool (*fn_OVRInput_Get)(int button, int controller);
    typedef float(*fn_OVRInput_GetAxis1D)(int axis, int controller);
    typedef bool (*fn_OVRInput_GetDown)(int button, int controller);
    static fn_OVRInput_Get     pOVRGet     = nullptr;
    static fn_OVRInput_GetAxis1D pOVRAxis  = nullptr;
    static fn_OVRInput_GetDown pOVRGetDown = nullptr;

    // OVR Button constants (Meta XR SDK enum values)
    static const int BTN_A       = 0x00000001; // OVRInput.Button.One (A)
    static const int BTN_B       = 0x00000002; // OVRInput.Button.Two (B)
    static const int BTN_START   = 0x00100000; // OVRInput.Button.Start (menu)
    static const int CTRL_RIGHT  = 0x00000008; // OVRInput.Controller.RTouch
    static const int CTRL_LEFT   = 0x00000004; // OVRInput.Controller.LTouch
    static const int AXIS_RY     = 0x00000010; // OVRInput.Axis1D.SecondaryThumbstickVertical
    static const int TRIGGER_R   = 0x00000008; // OVRInput.Axis1D.SecondaryIndexTrigger

    void InitOVR() {
        pOVRGet     = (fn_OVRInput_Get)     IL2CPP::GetMethod("", "OVRInput", "Get",      2);
        pOVRAxis    = (fn_OVRInput_GetAxis1D)IL2CPP::GetMethod("", "OVRInput", "Get",     2);
        pOVRGetDown = (fn_OVRInput_GetDown)  IL2CPP::GetMethod("", "OVRInput", "GetDown", 2);

        if (!pOVRGet) LOGE("[OVR] OVRInput.Get not found — falling back to XR");
        else          LOGI("[OVR] OVRInput bound");
    }

    static int  lastSelectedItem = -1;
    static bool lastMenuVis      = false;

    void Tick() {
        if (!pOVRGetDown || !pOVRGet) return;

        // Start/Menu button toggles overlay visibility
        if (pOVRGetDown(BTN_START, CTRL_LEFT)) {
            g_gui.menuVisible = !g_gui.menuVisible;
            LOGI("[GUI] Menu %s", g_gui.menuVisible ? "OPEN" : "CLOSED");
        }

        if (!g_gui.menuVisible) return;

        // B cycles down, A selects/toggles
        if (pOVRGetDown(BTN_B, CTRL_RIGHT)) {
            g_gui.selectedItem = (g_gui.selectedItem + 1) % ITEM_COUNT;
            LOGI("[GUI] Selected: %s", items[g_gui.selectedItem].label);
        }

        if (pOVRGetDown(BTN_A, CTRL_RIGHT)) {
            bool* tog = items[g_gui.selectedItem].toggle;
            *tog = !(*tog);
            LOGI("[GUI] Toggled %s -> %s",
                 items[g_gui.selectedItem].label,
                 *tog ? "ON" : "OFF");

            // Trigger one-shot features
            int idx = g_gui.selectedItem;
            if (idx == 3 && g_gui.knockoutInst) { Features::KnockoutAll(); g_gui.knockoutInst = false; }
            if (idx == 5 && g_gui.spawnPrefab)  { Features::SpawnPrefab(g_gui.spawnName); g_gui.spawnPrefab = false; }
            if (idx == 6 && g_gui.gun)           { Features::FireGun(); }
            if (idx == 7 && g_gui.kickGun)       { Features::FireKickGun(); }
            if (idx == 8 && g_gui.banGun)        { Features::FireBanGun(); }
        }

        // Right thumbstick Y -> fly vertical
        if (pOVRAxis) {
            g_gui.rightStickY = pOVRAxis(AXIS_RY, CTRL_RIGHT);
        }

        // Trigger on right -> fire gun if gun is active
        if (g_gui.gun && pOVRGet && pOVRGet(TRIGGER_R, CTRL_RIGHT)) {
            Features::FireGun();
        }
        if (g_gui.kickGun && pOVRGet && pOVRGet(TRIGGER_R, CTRL_LEFT)) {
            Features::FireKickGun();
        }

        // Log menu state to logcat (visible with: adb logcat -s RedmptionGUI:V)
        if (g_gui.menuVisible && g_gui.selectedItem != lastSelectedItem) {
            lastSelectedItem = g_gui.selectedItem;
            LOGI("[GUI] === RedmptionGUI Menu ===");
            for (int i = 0; i < ITEM_COUNT; i++) {
                bool on = *(items[i].toggle);
                LOGI("[GUI] %s %s %s",
                     i == g_gui.selectedItem ? ">" : " ",
                     items[i].label + 4, // skip "[ ] "
                     on ? "[ON]" : "[OFF]");
            }
        }
    }

    // Draw in-world GUI text via Unity Debug.Log bridging
    // For a real VR overlay, hook Camera.OnPostRender and use GL.Begin(GL.LINES)
    void RenderESP() {
        if (!g_gui.esp) return;
        for (auto& t : Features::g_espTargets) {
            if (!t.isValid) continue;
            LOGD("[ESP] %s", t.name);
        }
    }
}

// ── MAIN THREAD ──────────────────────────────────────────────────

static void* gui_thread(void*) {
    LOGI("=====================================================");
    LOGI("  RedmptionGUI | By Vr4se & GrandpaJoe");
    LOGI("  SlapLab Meta Quest VR Mod — Unity 2022 IL2CPP");
    LOGI("  Controls:");
    LOGI("    Left  Menu  = Open/Close GUI");
    LOGI("    Right B     = Navigate down");
    LOGI("    Right A     = Select / Toggle");
    LOGI("    Right Stick = Fly vertical");
    LOGI("    R Trigger   = Fire Gun");
    LOGI("    L Trigger   = Fire Kick Gun");
    LOGI("=====================================================");

    // Wait for Unity runtime to finish loading SlapLab
    sleep(5);
    LOGI("[Init] Starting IL2CPP init...");

    // Wait for il2cpp to be resolvable
    int attempts = 0;
    while (!IL2CPP::Init() && attempts++ < 20) {
        LOGI("[Init] Waiting for IL2CPP... attempt %d", attempts);
        sleep(1);
    }

    if (!IL2CPP::g_ready) {
        LOGE("[Init] IL2CPP never became ready — aborting");
        return nullptr;
    }

    // Resolve SlapLab-specific functions
    SlapLab::Resolve();

    // Install damage hook
    if (SlapLab::pTakeDamage) {
        SlapLab::orig_TakeDamage = SlapLab::pTakeDamage;
        Mem::Hook((void*)SlapLab::pTakeDamage, (void*)hk_TakeDamage);
        LOGI("[Hooks] TakeDamage hooked");
    }

    // Init VR input
    GUI_Overlay::InitOVR();

    // Set initial defaults
    g_gui.menuVisible   = true;
    g_gui.selectedItem  = 0;
    g_gui.initialized   = true;

    LOGI("[Init] RedmptionGUI fully loaded! Press Left Menu to open.");

    // ── Main loop: ~60hz tick ─────────────────────────────
    while (true) {
        Features::Tick();
        GUI_Overlay::Tick();
        GUI_Overlay::RenderESP();
        usleep(16667); // 16.67ms = 60fps
    }

    return nullptr;
}

// ── JNI ENTRY POINTS ─────────────────────────────────────────────

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    LOGI("[JNI_OnLoad] RedmptionGUI loading...");
    pthread_t tid;
    pthread_create(&tid, nullptr, gui_thread, nullptr);
    pthread_detach(tid);
    return JNI_VERSION_1_6;
}

// ── Java API bridge ───────────────────────────────────────────────
// These are called from the patched APK activity or a companion app

JNIEXPORT void JNICALL
Java_com_redemption_lol_RedmptionGUI_toggleFly(JNIEnv*, jclass, jboolean v) {
    g_gui.fly = v;
    LOGI("[JNI] Fly = %d", (int)v);
}

JNIEXPORT void JNICALL
Java_com_redemption_lol_RedmptionGUI_toggleESP(JNIEnv*, jclass, jboolean v) {
    g_gui.esp = v;
    LOGI("[JNI] ESP = %d", (int)v);
}

JNIEXPORT void JNICALL
Java_com_redemption_lol_RedmptionGUI_toggleGodMode(JNIEnv*, jclass, jboolean v) {
    g_gui.godMode = v;
    LOGI("[JNI] GodMode = %d", (int)v);
}

JNIEXPORT void JNICALL
Java_com_redemption_lol_RedmptionGUI_toggleNoclip(JNIEnv*, jclass, jboolean v) {
    g_gui.noclip = v;
    LOGI("[JNI] Noclip = %d", (int)v);
}

JNIEXPORT void JNICALL
Java_com_redemption_lol_RedmptionGUI_knockout(JNIEnv*, jclass) {
    Features::KnockoutAll();
    LOGI("[JNI] Knockout triggered");
}

JNIEXPORT void JNICALL
Java_com_redemption_lol_RedmptionGUI_spawnPrefab(JNIEnv* env, jclass, jstring name) {
    const char* n = env->GetStringUTFChars(name, nullptr);
    Features::SpawnPrefab(n);
    env->ReleaseStringUTFChars(name, n);
}

JNIEXPORT void JNICALL
Java_com_redemption_lol_RedmptionGUI_fireGun(JNIEnv*, jclass) {
    bool was = g_gui.gun;
    g_gui.gun = true;
    Features::FireGun();
    g_gui.gun = was;
}

JNIEXPORT void JNICALL
Java_com_redemption_lol_RedmptionGUI_fireKickGun(JNIEnv*, jclass) {
    bool was = g_gui.kickGun;
    g_gui.kickGun = true;
    Features::FireKickGun();
    g_gui.kickGun = was;
}

JNIEXPORT void JNICALL
Java_com_redemption_lol_RedmptionGUI_fireBanGun(JNIEnv*, jclass) {
    bool was = g_gui.banGun;
    g_gui.banGun = true;
    Features::FireBanGun();
    g_gui.banGun = was;
}

JNIEXPORT jstring JNICALL
Java_com_redemption_lol_RedmptionGUI_getStatus(JNIEnv* env, jclass) {
    char buf[512];
    snprintf(buf, sizeof(buf),
        "RedmptionGUI | Vr4se & GrandpaJoe\n"
        "Fly:%s ESP:%s God:%s Knockout:%s\n"
        "Noclip:%s Gun:%s Kick:%s Ban:%s\n"
        "Init:%s",
        g_gui.fly?"ON":"OFF", g_gui.esp?"ON":"OFF",
        g_gui.godMode?"ON":"OFF", g_gui.knockoutInst?"ON":"OFF",
        g_gui.noclip?"ON":"OFF", g_gui.gun?"ON":"OFF",
        g_gui.kickGun?"ON":"OFF", g_gui.banGun?"ON":"OFF",
        g_gui.initialized?"YES":"LOADING...");
    return env->NewStringUTF(buf);
}

JNIEXPORT void JNICALL
Java_com_redemption_lol_RedmptionGUI_setFlySpeed(JNIEnv*, jclass, jfloat v) {
    g_gui.flySpeed = v;
}

JNIEXPORT void JNICALL
Java_com_redemption_lol_RedmptionGUI_setGunForce(JNIEnv*, jclass, jfloat v) {
    g_gui.gunForce = v;
}

JNIEXPORT void JNICALL
Java_com_redemption_lol_RedmptionGUI_setKickForce(JNIEnv*, jclass, jfloat v) {
    g_gui.kickForce = v;
}

JNIEXPORT void JNICALL
Java_com_redemption_lol_RedmptionGUI_openMenu(JNIEnv*, jclass) {
    g_gui.menuVisible = true;
    LOGI("[JNI] Menu opened");
}

} // extern "C"

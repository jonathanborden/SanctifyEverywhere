#include "mod.h"
#include "tick_hook.h"
#include <cstdio>
#include <atomic>
#include <cstring>
#include <cmath>

// ============================================================
// Globals
// ============================================================

static std::atomic<bool> gRunning{false};
static HANDLE gModThread = nullptr;
FILE* gLogFile = nullptr;

// Types needed early
struct FName { int32_t ComparisonIndex; int32_t Number; };
struct FString { wchar_t* Data; int32_t Count; int32_t Max; };
typedef void (__fastcall *FNameToString_fn)(const FName* name, FString& outStr);
typedef void (__fastcall *ProcessEvent_fn)(void*, void*, void*);

// UE state - found once, reused
void* gObjArrayBase = nullptr;
int32_t gNumElements = 0, gNumChunks = 0;
int gItemSize = 24;
uintptr_t gTextStart = 0, gTextSize = 0;
uintptr_t gDataStart = 0, gDataSize = 0;
uintptr_t gExeBase = 0, gExeEnd = 0;
static uintptr_t gProcessEventAddr = 0;
static FNameToString_fn gFNameToString = nullptr;

// Cached UFunction/object pointers (found once per session)
static void* gBeginSpawnFunc = nullptr;
static void* gFinishSpawnFunc = nullptr;
static void* gGetLocFunc = nullptr;
static void* gSetLocFunc = nullptr;
static void* gConstructionFunc = nullptr;
static void* gBeginPlayFunc = nullptr;
static void* gGameplayStaticsCDO = nullptr;

// ============================================================
// Constants (Deadzone Rogue v1.4 - stable across ASLR)
// ============================================================

static const int64_t FNAME_TOSTRING_OFFSET = -0x0A27FB50;
static const int PROCESS_EVENT_VTABLE_SLOT = 0x4C;
static const int BEGIN_SPAWN_RETVAL_OFFSET = 0x88;
static const int FINISH_SPAWN_RETVAL_OFFSET = 120;

// Brute-forced property offsets (constant per game version)
static const int GAMESTATE_SANCTIFY_BOOL_OFFSET = 0x592;
static const int PLAYER_SANCTIFY_BOOL_OFFSET = 0x13A;

// ============================================================
// Forward declarations & types
// ============================================================

void Log(const char* fmt, ...);
bool IsSafeToRead(void* addr, size_t size);
bool SafeRead64(uintptr_t addr, uintptr_t* out);
bool SafeRead32(uintptr_t addr, int32_t* out);
void* GetUObject(int32_t index);
bool GetObjectName(void* obj, wchar_t* buf, int bufSize);
void* GetObjClass(void* obj);
void* GetObjOuter(void* obj);
bool IsCDO(void* obj);
static bool GetFNameStr(const FName* n, wchar_t* buf, int sz);

// ============================================================
// Spawn request (background thread -> game thread)
// ALL pointers found FRESH per F9 press — not cached across level loads
// ============================================================

static struct {
    void* bpAnvilClass;
    void* gameWorld;
    void* spawnFunc;
    void* finishSpawnFunc;
    void* gameplayStaticsCDO;
    void* setLocFunc;
    void* constructionFunc;
    void* beginPlayFunc;
    void* rerunConstructionFunc;
    void* setSanctifiedAnvilFunc;
    void* gameModeInstance;
    void* enableSAFunc;
    void* safeRoomMgr;
    void* gameStateInst;
    void* playerSanctifyComp;
    uintptr_t processEventAddr;
    double spawnX[32], spawnY[32], spawnZ[32];
    int spawnCount;
    volatile bool resultReady;
    volatile bool success;
} gSpawnReq = {};

// ============================================================
// Currency HUD experiment (F6) — game thread callback
// ============================================================

static struct {
    void* visorCurrenciesWidget;    // WBP_Game_Currencies_C inside UI_Game_Visor_C
    void* setVisibilityFunc;
    void* onMechanicsUpdatedFunc;   // OnAvailableRunProgressionMechanicsUpdated
    void* addCurrencyWidgetFunc;
    void* setCurrencyIdFunc;
    void* bindCurrencyFunc;
    void* playerStateInst;
    void* getMatchCurrencyFunc;
    uintptr_t processEventAddr;
    volatile bool done;
    volatile bool success;
} gCurrReq = {};

static void GameThreadCurrencyExperiment() {
    ProcessEvent_fn pe = (ProcessEvent_fn)gCurrReq.processEventAddr;
    void* w = gCurrReq.visorCurrenciesWidget;

    // Step 1: SetVisibility(Visible=0) on the Currencies container
    if (gCurrReq.setVisibilityFunc && w) {
        __declspec(align(16)) uint8_t vp[16] = {};
        // ESlateVisibility: 0=Visible, 1=Collapsed, 2=Hidden, 3=HitTestInvisible, 4=SelfHitTestInvisible
        vp[0] = 0; // Visible
        __try {
            pe(w, gCurrReq.setVisibilityFunc, vp);
            Log("[GT-Curr] SetVisibility(Visible) OK\n");
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            Log("[GT-Curr] SetVisibility CRASHED\n");
        }
    }

    // Step 2: Call OnAvailableRunProgressionMechanicsUpdated with zeroed params
    if (gCurrReq.onMechanicsUpdatedFunc && w) {
        __declspec(align(16)) uint8_t mp[16] = {};
        __try {
            pe(w, gCurrReq.onMechanicsUpdatedFunc, mp);
            Log("[GT-Curr] OnAvailableRunProgressionMechanicsUpdated(zeroed) OK\n");
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            Log("[GT-Curr] OnAvailableRunProgressionMechanicsUpdated CRASHED\n");
        }
    }

    // Step 3: Try OnAvailableRunProgressionMechanicsUpdated with flags=0xFF
    if (gCurrReq.onMechanicsUpdatedFunc && w) {
        __declspec(align(16)) uint8_t mp[16] = {};
        mp[0] = 0xFF; // All flags set
        __try {
            pe(w, gCurrReq.onMechanicsUpdatedFunc, mp);
            Log("[GT-Curr] OnAvailableRunProgressionMechanicsUpdated(0xFF) OK\n");
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            Log("[GT-Curr] OnAvailableRunProgressionMechanicsUpdated(0xFF) CRASHED\n");
        }
    }

    // Step 4: Read currency after experiment
    if (gCurrReq.getMatchCurrencyFunc && gCurrReq.playerStateInst) {
        __declspec(align(16)) uint8_t cp[16] = {};
        __try {
            pe(gCurrReq.playerStateInst, gCurrReq.getMatchCurrencyFunc, cp);
            Log("[GT-Curr] GetMatchCurrency = %d\n", *(int32_t*)cp);
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Step 5: Dump child widgets of the CurrencyGroup to see if entries were added
    // Find CurrencyGroup (HorizontalBox) by scanning objects whose outer is the same WidgetTree
    // as the Currencies widget
    void* currOuter = nullptr;
    if (w) {
        uintptr_t outerPtr = 0;
        SafeRead64((uintptr_t)w + 0x20, &outerPtr);
        currOuter = (void*)outerPtr;
    }
    if (currOuter) {
        wchar_t nb[256], cb[256];
        int32_t ne = 0;
        SafeRead32((uintptr_t)gObjArrayBase + 0x14, &ne);
        Log("[GT-Curr] Scanning for children of same WidgetTree (outer=0x%llX)...\n", (uintptr_t)currOuter);
        for (int i = 0; i < ne; i++) {
            void* o = GetUObject(i); if (!o) continue;
            void* oo = GetObjOuter(o);
            if (oo != currOuter) continue;
            if (!GetObjectName(o, nb, 256)) continue;
            void* c = GetObjClass(o);
            cb[0] = 0; if (c) GetObjectName(c, cb, 256);
            if (wcsstr(cb, L"Currency") || wcsstr(nb, L"Currency") || wcsstr(cb, L"HorizontalBox"))
                Log("[GT-Curr]   child: \"%ls\" class=\"%ls\" (0x%llX)\n", nb, cb, (uintptr_t)o);
        }
    }

    Log("[GT-Curr] *** EXPERIMENT COMPLETE ***\n");
    gCurrReq.success = true;
    gCurrReq.done = true;
}

// ============================================================
// Direct AddCurrencyWidget (F11 harvest / F12 call)
// ============================================================

// Harvested SanctityToken FPrimaryAssetId (16B: type FName[8] + name FName[8]).
// Persists for the process lifetime — FName indices are process-global, so an ID
// harvested in zone 4 stays valid for an F12 call in a later zone-1-3 run (no restart).
static uint8_t gSanctityCurrencyId[16] = {};
static bool gSanctityCurrencyIdValid = false;

static struct {
    void* container;             // live WBP_Game_Currencies_C
    void* addCurrencyWidgetFunc; // AddCurrencyWidget on WBP_Game_Currencies_C
    uintptr_t processEventAddr;
    uint8_t currencyId[16];
    volatile bool done;
    volatile bool success;
} gAddReq = {};

static void GameThreadAddCurrency() {
    ProcessEvent_fn pe = (ProcessEvent_fn)gAddReq.processEventAddr;
    __declspec(align(16)) uint8_t p[256] = {};
    memcpy(p, gAddReq.currencyId, 16); // FPrimaryAssetId at param offset 0
    __try {
        pe(gAddReq.container, gAddReq.addCurrencyWidgetFunc, p);
        Log("[GT-Add] AddCurrencyWidget OK (retptr@+16=0x%llX, @+24=0x%llX)\n",
            *(uintptr_t*)(p + 16), *(uintptr_t*)(p + 24));
        gAddReq.success = true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("[GT-Add] AddCurrencyWidget CRASHED\n");
    }
    gAddReq.done = true;
}

// Find an FName by locating any loaded UObject whose name resolves to `target`.
// Any FName resolving to a given string shares the same ComparisonIndex, so this
// yields the exact FName needed to rebuild an FPrimaryAssetId — no harvest required.
static bool FindFNameByObjectName(const wchar_t* target, FName* out) {
    wchar_t nb[256];
    for (int i = 0; i < gNumElements; i++) {
        void* o = GetUObject(i); if (!o || !IsSafeToRead(o, 0x20)) continue;
        FName n;
        if (!SafeRead32((uintptr_t)o + 0x18, &n.ComparisonIndex)) continue;
        if (!SafeRead32((uintptr_t)o + 0x1C, &n.Number)) continue;
        if (!GetFNameStr(&n, nb, 256)) continue;
        if (wcscmp(nb, target) == 0) { *out = n; return true; }
    }
    return false;
}

// Rebuild the SanctityToken FPrimaryAssetId = {type FName, name FName} from names.
static bool BuildSanctityCurrencyId(uint8_t out[16]) {
    FName typeName, assetName;
    if (!FindFNameByObjectName(L"ValItemAsset", &typeName)) return false;
    if (!FindFNameByObjectName(L"Item_Currency_SanctityToken", &assetName)) return false;
    memset(out, 0, 16);
    memcpy(out + 0, &typeName, 8);
    memcpy(out + 8, &assetName, 8);
    return true;
}

static void SpawnOneAnvil(ProcessEvent_fn pe, double sx, double sy, double sz) {

    // BeginDeferredActorSpawnFromClass
    __declspec(align(16)) uint8_t p[256] = {};
    *(void**)(p + 0) = gSpawnReq.gameWorld;
    *(void**)(p + 8) = gSpawnReq.bpAnvilClass;
    *(double*)(p + 16) = 0; *(double*)(p + 24) = 0; *(double*)(p + 32) = 0; *(double*)(p + 40) = 1.0; // Quat identity
    *(double*)(p + 48) = sx;
    *(double*)(p + 56) = sy;
    *(double*)(p + 64) = sz;
    *(double*)(p + 72) = 0;
    *(double*)(p + 80) = 1.0; *(double*)(p + 88) = 1.0; *(double*)(p + 96) = 1.0; *(double*)(p + 104) = 0; // Scale
    *(uint8_t*)(p + 112) = 1; // AlwaysSpawn

    __try { pe(gSpawnReq.gameplayStaticsCDO, gSpawnReq.spawnFunc, p); }
    __except(EXCEPTION_EXECUTE_HANDLER) { Log("[GT] BeginSpawn CRASHED\n"); gSpawnReq.resultReady = true; return; }

    void* actor = *(void**)(p + BEGIN_SPAWN_RETVAL_OFFSET);
    if (!actor) { Log("[GT] BeginSpawn returned null\n"); gSpawnReq.resultReady = true; return; }
    Log("[GT] Actor: 0x%llX\n", (uintptr_t)actor);

    // FinishSpawningActor
    Log("[GT] Calling FinishSpawn...\n");
    __declspec(align(16)) uint8_t fp[256] = {};
    *(void**)(fp + 0) = actor;
    *(double*)(fp + 0x10) = 0; *(double*)(fp + 0x18) = 0; *(double*)(fp + 0x20) = 0; *(double*)(fp + 0x28) = 1.0;
    *(double*)(fp + 0x30) = sx; *(double*)(fp + 0x38) = sy; *(double*)(fp + 0x40) = sz;
    *(double*)(fp + 0x48) = 0;
    *(double*)(fp + 0x50) = 1.0; *(double*)(fp + 0x58) = 1.0; *(double*)(fp + 0x60) = 1.0; *(double*)(fp + 0x68) = 0;

    __try {
        pe(gSpawnReq.gameplayStaticsCDO, gSpawnReq.finishSpawnFunc, fp);
        void* result = *(void**)(fp + FINISH_SPAWN_RETVAL_OFFSET);
        Log("[GT] FinishSpawn returned: 0x%llX\n", (uintptr_t)result);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("[GT] FinishSpawn CRASHED!\n");
    }

    // Move actor to correct position (FTransform is ignored during spawn)
    if (gSpawnReq.setLocFunc) {
        __declspec(align(16)) uint8_t slp[512] = {};
        *(double*)(slp + 0) = sx;
        *(double*)(slp + 8) = sy;
        *(double*)(slp + 16) = sz;
        // bSweep=false at 24, bTeleport=true near end
        int32_t setRv = 0;
        SafeRead32((uintptr_t)gSpawnReq.setLocFunc + 0xB8, &setRv);
        if (setRv > 4) *(uint8_t*)(slp + setRv - 1) = 1; // bTeleport=true
        __try { pe(actor, gSpawnReq.setLocFunc, slp); Log("[GT] SetActorLocation OK\n"); }
        __except(EXCEPTION_EXECUTE_HANDLER) { Log("[GT] SetActorLocation CRASHED\n"); }
    }

    // Trigger Blueprint init
    if (gSpawnReq.constructionFunc) {
        __declspec(align(16)) uint8_t ip[64] = {};
        __try { pe(actor, gSpawnReq.constructionFunc, ip); Log("[GT] UserConstructionScript OK\n"); }
        __except(EXCEPTION_EXECUTE_HANDLER) { Log("[GT] UserConstructionScript CRASHED\n"); }
    }
    if (gSpawnReq.beginPlayFunc) {
        __declspec(align(16)) uint8_t ip[64] = {};
        __try { pe(actor, gSpawnReq.beginPlayFunc, ip); Log("[GT] ReceiveBeginPlay OK\n"); }
        __except(EXCEPTION_EXECUTE_HANDLER) { Log("[GT] ReceiveBeginPlay CRASHED\n"); }
    }

    // RerunConstructionScripts — forces component creation and registration
    if (gSpawnReq.rerunConstructionFunc) {
        __declspec(align(16)) uint8_t ip[64] = {};
        __try { pe(actor, gSpawnReq.rerunConstructionFunc, ip); Log("[GT] RerunConstructionScripts OK\n"); }
        __except(EXCEPTION_EXECUTE_HANDLER) { Log("[GT] RerunConstructionScripts CRASHED\n"); }
    }

    // Register anvil with game's sanctify system
    if (gSpawnReq.setSanctifiedAnvilFunc && gSpawnReq.gameModeInstance && actor) {
        __declspec(align(16)) uint8_t ssa[64] = {};
        *(void**)(ssa + 0) = actor;
        __try {
            pe(gSpawnReq.gameModeInstance, gSpawnReq.setSanctifiedAnvilFunc, ssa);
            Log("[GT] SetSanctifiedAnvil OK\n");
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            Log("[GT] SetSanctifiedAnvil CRASHED\n");
        }
    }

    // Enable sanctification via the safe room encounter manager
    if (gSpawnReq.enableSAFunc && gSpawnReq.safeRoomMgr && actor) {
        __declspec(align(16)) uint8_t esp[256] = {};
        // EnableSanctifiedAnvil PS=144 — likely takes an actor reference
        *(void**)(esp + 0) = actor;
        __try {
            pe(gSpawnReq.safeRoomMgr, gSpawnReq.enableSAFunc, esp);
            Log("[GT] EnableSanctifiedAnvil OK!\n");
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            Log("[GT] EnableSanctifiedAnvil CRASHED\n");
        }
    }

    Log("[GT] Anvil at (%.0f, %.0f, %.0f) done\n", sx, sy, sz);
}

static void GameThreadSpawn() {
    Log("[GT] Spawning %d anvils\n", gSpawnReq.spawnCount);
    ProcessEvent_fn pe = (ProcessEvent_fn)gSpawnReq.processEventAddr;

    for (int i = 0; i < gSpawnReq.spawnCount; i++) {
        SpawnOneAnvil(pe, gSpawnReq.spawnX[i], gSpawnReq.spawnY[i], gSpawnReq.spawnZ[i]);
    }

    // Set GameState sanctify bool using cached offset (instant — no scanning)
    if (gSpawnReq.gameStateInst && IsSafeToRead(gSpawnReq.gameStateInst, GAMESTATE_SANCTIFY_BOOL_OFFSET + 1)) {
        ((uint8_t*)gSpawnReq.gameStateInst)[GAMESTATE_SANCTIFY_BOOL_OFFSET] = 1;
        Log("[GT] GameState+0x%X set TRUE\n", GAMESTATE_SANCTIFY_BOOL_OFFSET);
    }

    // Set player sanctify component bool using cached offset (instant)
    if (gSpawnReq.playerSanctifyComp && IsSafeToRead(gSpawnReq.playerSanctifyComp, PLAYER_SANCTIFY_BOOL_OFFSET + 1)) {
        ((uint8_t*)gSpawnReq.playerSanctifyComp)[PLAYER_SANCTIFY_BOOL_OFFSET] = 1;
        Log("[GT] PlayerSanctifyComp+0x%X set TRUE\n", PLAYER_SANCTIFY_BOOL_OFFSET);
    }

    Log("[GT] *** %d ANVILS SPAWNED! ***\n", gSpawnReq.spawnCount);
    gSpawnReq.success = true;
    gSpawnReq.resultReady = true;
}

// ============================================================
// Core functions
// ============================================================

void Log(const char* fmt, ...) {
    if (!gLogFile) return;
    va_list a; va_start(a, fmt); vfprintf(gLogFile, fmt, a); va_end(a);
    fflush(gLogFile);
}

bool IsSafeToRead(void* addr, size_t size) {
    if (!addr) return false;
    MEMORY_BASIC_INFORMATION m;
    if (!VirtualQuery(addr, &m, sizeof(m))) return false;
    if (m.State != MEM_COMMIT || (m.Protect & (PAGE_NOACCESS|PAGE_GUARD)) || !m.Protect) return false;
    return true;
}

bool SafeRead64(uintptr_t a, uintptr_t* o) {
    if (!IsSafeToRead((void*)a, 8)) return false;
    __try { *o = *(uintptr_t*)a; return true; } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SafeRead32(uintptr_t a, int32_t* o) {
    if (!IsSafeToRead((void*)a, 4)) return false;
    __try { *o = *(int32_t*)a; return true; } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool GetFNameStr(const FName* n, wchar_t* buf, int sz) {
    buf[0] = 0;
    if (!gFNameToString || !n) return false;
    FString r = {0,0,0};
    __try { gFNameToString(n, r); } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (r.Data && r.Count > 0 && IsSafeToRead(r.Data, 2)) {
        __try { wcsncpy_s(buf, sz, r.Data, min(r.Count, sz-1)); return true; }
        __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    return false;
}

void* GetUObject(int32_t i) {
    if (!gObjArrayBase || i < 0 || i >= gNumElements) return nullptr;
    int32_t ci = i / 65536, wi = i % 65536;
    if (ci >= gNumChunks) return nullptr;
    uintptr_t cb = 0, cp = 0, op = 0;
    __try {
        if (!SafeRead64((uintptr_t)gObjArrayBase, &cb) || !cb) return nullptr;
        if (!SafeRead64(cb + ci * 8, &cp) || !cp) return nullptr;
        if (!SafeRead64(cp + (uintptr_t)wi * gItemSize, &op)) return nullptr;
        return (void*)op;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

bool GetObjectName(void* obj, wchar_t* buf, int sz) {
    if (!obj || !IsSafeToRead(obj, 0x20)) { buf[0]=0; return false; }
    FName n; SafeRead32((uintptr_t)obj+0x18, &n.ComparisonIndex); SafeRead32((uintptr_t)obj+0x1C, &n.Number);
    return GetFNameStr(&n, buf, sz);
}

void* GetObjClass(void* o) {
    if (!o || !IsSafeToRead(o, 0x18)) return nullptr;
    uintptr_t c = 0; SafeRead64((uintptr_t)o + 0x10, &c);
    return (c && IsSafeToRead((void*)c, 0x20)) ? (void*)c : nullptr;
}

void* GetObjOuter(void* o) {
    if (!o || !IsSafeToRead(o, 0x28)) return nullptr;
    uintptr_t u = 0; SafeRead64((uintptr_t)o + 0x20, &u);
    return (u && IsSafeToRead((void*)u, 0x20)) ? (void*)u : nullptr;
}

bool IsCDO(void* o) {
    wchar_t n[256]; return GetObjectName(o, n, 256) && wcsncmp(n, L"Default__", 9) == 0;
}

// Case-insensitive substring (needle must be lowercase ASCII).
static bool ContainsCI(const wchar_t* hay, const wchar_t* needleLower) {
    if (!hay) return false;
    wchar_t low[256]; int i = 0;
    for (; hay[i] && i < 255; i++) { wchar_t c = hay[i]; if (c>=L'A'&&c<=L'Z') c+=32; low[i]=c; }
    low[i] = 0;
    return wcsstr(low, needleLower) != nullptr;
}

// ============================================================
// Pattern scan (only needed once per session for GUObjectArray)
// ============================================================

static uintptr_t ScanPattern(uintptr_t start, size_t size, const uint8_t* pat, const char* mask) {
    size_t pl = strlen(mask); if (size < pl) return 0;
    const uint8_t* m = (const uint8_t*)start;
    for (size_t i = 0; i <= size - pl; i++) {
        bool f = true;
        for (size_t j = 0; j < pl; j++) if (mask[j]=='x' && m[i+j]!=pat[j]) { f=false; break; }
        if (f) return start + i;
    }
    return 0;
}

static bool ValidateObjArray(uintptr_t a, int32_t ne, int32_t nc, int is) {
    uintptr_t cp = 0; if (!SafeRead64(a, &cp) || !cp) return false;
    if (!IsSafeToRead((void*)cp, nc*8)) return false;
    uintptr_t c0 = 0; if (!SafeRead64(cp, &c0) || !c0) return false;
    if (!IsSafeToRead((void*)c0, (size_t)is*16)) return false;
    int v = 0;
    for (int i = 1; i < min(30, ne); i++) {
        uintptr_t op = 0; if (!SafeRead64(c0 + (uintptr_t)i*is, &op)) continue;
        if (!op || !IsSafeToRead((void*)op, 0x28)) continue;
        uintptr_t vt = 0; if (!SafeRead64(op, &vt) || vt < gExeBase || vt >= gExeEnd) continue;
        uintptr_t cl = 0; if (!SafeRead64(op+0x10, &cl) || !cl || !IsSafeToRead((void*)cl, 0x20)) continue;
        uintptr_t cv = 0; if (!SafeRead64(cl, &cv) || cv < gExeBase || cv >= gExeEnd) continue;
        v++;
    }
    return v >= 5;
}

static bool InitUE() {
    gExeBase = (uintptr_t)GetModuleHandleA(nullptr);
    PIMAGE_DOS_HEADER d = (PIMAGE_DOS_HEADER)gExeBase;
    PIMAGE_NT_HEADERS n = (PIMAGE_NT_HEADERS)(gExeBase + d->e_lfanew);
    gExeEnd = gExeBase + n->OptionalHeader.SizeOfImage;
    PIMAGE_SECTION_HEADER s = IMAGE_FIRST_SECTION(n);
    for (int i = 0; i < n->FileHeader.NumberOfSections; i++) {
        if (!strncmp((char*)s[i].Name, ".text", 5)) { gTextStart = gExeBase + s[i].VirtualAddress; gTextSize = s[i].Misc.VirtualSize; }
        if (!strncmp((char*)s[i].Name, ".data", 5)) { gDataStart = gExeBase + s[i].VirtualAddress; gDataSize = s[i].Misc.VirtualSize; }
    }

    // Find GUObjectArray
    const uint8_t lp[] = {0x48, 0x8D, 0x0D};
    uintptr_t sf = gTextStart; int sc = 0;
    while (sc < 10000) {
        size_t rem = gTextSize - (sf - gTextStart); if (rem < 16) break;
        uintptr_t r = ScanPattern(sf, rem, lp, "xxx"); if (!r) break; sf = r + 1; sc++;
        int32_t disp = *(int32_t*)(r + 3); uintptr_t tgt = r + 7 + disp;
        if (tgt < gDataStart || tgt >= gDataStart + gDataSize) continue;
        for (int off = 0; off <= 0x48; off += 8) {
            int32_t me=0, ne=0, mc=0, nc=0;
            if (!SafeRead32(tgt+off+0x10,&me)||!SafeRead32(tgt+off+0x14,&ne)||!SafeRead32(tgt+off+0x18,&mc)||!SafeRead32(tgt+off+0x1C,&nc)) continue;
            if (ne<10000||ne>2000000||nc<1||nc>500||me<ne||mc<nc||me>10000000||ne==32759) continue;
            if (ValidateObjArray(tgt+off, ne, nc, 24)) {
                gObjArrayBase = (void*)(tgt+off); gNumElements = ne; gNumChunks = nc;
                Log("GUObjectArray: 0x%llX (%d objects)\n", tgt+off, ne);
                goto found_guoa;
            }
        }
    }
    Log("GUObjectArray NOT FOUND\n"); return false;
found_guoa:

    // Compute FNameToString
    uintptr_t guoaStruct = (uintptr_t)gObjArrayBase - 0x10;
    uintptr_t fna = guoaStruct + FNAME_TOSTRING_OFFSET;
    if (fna < gTextStart || fna >= gTextStart + gTextSize) { Log("FNameToString out of range\n"); return false; }
    FName tn = {0,0}; FString tr = {0,0,0};
    FNameToString_fn fc = (FNameToString_fn)fna;
    __try { fc(&tn, tr); } __except(EXCEPTION_EXECUTE_HANDLER) { Log("FNameToString crashed\n"); return false; }
    if (!tr.Data || tr.Count < 4) { Log("FNameToString didn't return 'None'\n"); return false; }
    gFNameToString = fc;
    Log("FNameToString: 0x%llX OK\n", fna);

    return true;
}

// Find UFunction by name (returns first match with PropsSize in range)
static void* FindUFunc(const wchar_t* name, int minPS, int maxPS) {
    wchar_t nb[256];
    for (int i = 0; i < gNumElements; i++) {
        void* o = GetUObject(i); if (!o) continue;
        if (!GetObjectName(o, nb, 256)) continue;
        if (wcscmp(nb, name) != 0) continue;
        int32_t ps = 0; SafeRead32((uintptr_t)o + 0x58, &ps);
        if (ps >= minPS && ps <= maxPS) return o;
    }
    return nullptr;
}

// Find class by name
static void* FindClass(const wchar_t* name) {
    wchar_t nb[256], cb[256];
    for (int i = 0; i < gNumElements; i++) {
        void* o = GetUObject(i); if (!o) continue;
        if (!GetObjectName(o, nb, 256) || wcscmp(nb, name) != 0) continue;
        void* c = GetObjClass(o); if (!c) continue;
        if (GetObjectName(c, cb, 256) && (wcscmp(cb, L"Class")==0 || wcscmp(cb, L"BlueprintGeneratedClass")==0))
            return o;
    }
    return nullptr;
}

// ============================================================
// Reflection dumpers — decode UStruct param chains & UEnum entries
// UE5 offsets:
//   UStruct: SuperStruct 0x40, Children(UField*) 0x48, ChildProperties(FField*) 0x50, PropertiesSize 0x58
//   FField:  ClassPrivate(FFieldClass*) 0x08, Next(FField*) 0x20, NamePrivate(FName) 0x28
//   FProperty: ArrayDim 0x38, ElementSize 0x3C, PropertyFlags(u64) 0x40, Offset_Internal 0x4C
//   typed inner ptr (Struct/Enum/PropertyClass) 0x78
//   FFieldClass: FName Name 0x00
//   UEnum: Names TArray { data 0x40, count 0x48 }; entry = FName(8)+i64(8)=16B
// ============================================================

static bool GetFFieldName(void* field, wchar_t* buf, int sz) {
    buf[0] = 0;
    if (!field || !IsSafeToRead(field, 0x30)) return false;
    FName n;
    if (!SafeRead32((uintptr_t)field + 0x28, &n.ComparisonIndex)) return false;
    if (!SafeRead32((uintptr_t)field + 0x2C, &n.Number)) return false;
    return GetFNameStr(&n, buf, sz);
}

static bool GetFFieldClassName(void* field, wchar_t* buf, int sz) {
    buf[0] = 0;
    if (!field || !IsSafeToRead(field, 0x10)) return false;
    uintptr_t fc = 0;
    if (!SafeRead64((uintptr_t)field + 0x08, &fc) || !fc || !IsSafeToRead((void*)fc, 0x08)) return false;
    FName n;
    if (!SafeRead32(fc + 0x00, &n.ComparisonIndex)) return false;
    if (!SafeRead32(fc + 0x04, &n.Number)) return false;
    return GetFNameStr(&n, buf, sz);
}

// Walk a UStruct/UFunction's ChildProperties (FField*) chain and log each property.
static void DumpStructProperties(void* ustruct, const wchar_t* label) {
    if (!ustruct || !IsSafeToRead(ustruct, 0x60)) { Log("  [%ls] unreadable\n", label); return; }
    int32_t propsSize = 0; SafeRead32((uintptr_t)ustruct + 0x58, &propsSize);
    Log("  [%ls] PropertiesSize=%d\n", label, propsSize);
    uintptr_t field = 0; SafeRead64((uintptr_t)ustruct + 0x50, &field);
    int guard = 0;
    while (field && IsSafeToRead((void*)field, 0x50) && guard++ < 64) {
        wchar_t pname[256], ptype[256];
        GetFFieldName((void*)field, pname, 256);
        GetFFieldClassName((void*)field, ptype, 256);
        int32_t arrDim = 0, elemSize = 0, off = 0;
        uintptr_t flags = 0;
        SafeRead32(field + 0x38, &arrDim);
        SafeRead32(field + 0x3C, &elemSize);
        SafeRead64(field + 0x40, &flags);
        SafeRead32(field + 0x4C, &off);
        // flags: Parm=0x80, OutParm=0x100, ReturnParm=0x400, ConstParm=0x200
        char dir[32] = "";
        if (flags & 0x400) strcpy_s(dir, "RET");
        else if (flags & 0x100) strcpy_s(dir, "OUT");
        else if (flags & 0x80)  strcpy_s(dir, "IN ");
        else strcpy_s(dir, "loc");
        // inner typed pointer (Struct/Enum/PropertyClass) at 0x78 for relevant property classes
        wchar_t inner[256] = L"";
        if (wcsstr(ptype, L"Struct") || wcsstr(ptype, L"Enum") || wcsstr(ptype, L"Byte") ||
            wcsstr(ptype, L"Object") || wcsstr(ptype, L"Class") || wcsstr(ptype, L"Interface")) {
            uintptr_t ip = 0;
            if (SafeRead64(field + 0x78, &ip) && ip && IsSafeToRead((void*)ip, 0x20))
                GetObjectName((void*)ip, inner, 256);
        }
        Log("    [%s] off=0x%-3X size=%-3d dim=%d  %-18ls %ls%s%ls\n",
            dir, off, elemSize, arrDim, ptype, pname,
            inner[0] ? L"  -> " : L"", inner);
        SafeRead64(field + 0x20, &field); // Next
    }
}

// Dump a UEnum's Names TArray (name=value pairs).
static void DumpEnum(void* uenum) {
    if (!uenum || !IsSafeToRead(uenum, 0x50)) { Log("  enum unreadable\n"); return; }
    uintptr_t data = 0; int32_t count = 0;
    SafeRead64((uintptr_t)uenum + 0x40, &data);
    SafeRead32((uintptr_t)uenum + 0x48, &count);
    if (!data || count <= 0 || count > 4096 || !IsSafeToRead((void*)data, (size_t)count * 16)) {
        Log("  enum Names array invalid (data=0x%llX count=%d)\n", data, count);
        return;
    }
    Log("  enum has %d entries:\n", count);
    for (int i = 0; i < count; i++) {
        FName n; uintptr_t val = 0;
        SafeRead32(data + (uintptr_t)i*16 + 0, &n.ComparisonIndex);
        SafeRead32(data + (uintptr_t)i*16 + 4, &n.Number);
        SafeRead64(data + (uintptr_t)i*16 + 8, &val);
        wchar_t en[256]; GetFNameStr(&n, en, 256);
        Log("    [%lld] %ls\n", (long long)val, en);
    }
}

// Minimal startup cache — just enough to gate F9. F9 does its own full search.
static bool CacheObjects() {
    SafeRead32((uintptr_t)gObjArrayBase + 0x14, &gNumElements);
    Log("Startup scan (%d objects)...\n", gNumElements);

    // Single pass: find BeginSpawn + CDO + GetLoc (minimum needed for F9 gate)
    wchar_t nb[256];
    for (int i = 0; i < gNumElements; i++) {
        void* o = GetUObject(i); if (!o) continue;
        if (!GetObjectName(o, nb, 256)) continue;
        if (!gBeginSpawnFunc && wcscmp(nb, L"BeginDeferredActorSpawnFromClass") == 0) {
            int32_t ps = 0; SafeRead32((uintptr_t)o + 0x58, &ps);
            if (ps >= 100 && ps <= 200) gBeginSpawnFunc = o;
        }
        if (!gGetLocFunc && wcscmp(nb, L"K2_GetActorLocation") == 0) {
            int32_t ps = 0; SafeRead32((uintptr_t)o + 0x58, &ps);
            if (ps >= 20 && ps <= 50) gGetLocFunc = o;
        }
        if (!gGameplayStaticsCDO && wcscmp(nb, L"Default__GameplayStatics") == 0)
            gGameplayStaticsCDO = o;
    }

    // Find ProcessEvent
    if (gGameplayStaticsCDO) {
        uintptr_t vt = 0; SafeRead64((uintptr_t)gGameplayStaticsCDO, &vt);
        if (vt) {
            uintptr_t fa = 0; SafeRead64(vt + PROCESS_EVENT_VTABLE_SLOT * 8, &fa);
            if (fa >= gTextStart && fa < gTextStart + gTextSize) {
                uint8_t* b = (uint8_t*)fa;
                if (IsSafeToRead((void*)fa, 4) && b[0]==0x40 && b[1]==0x55 && b[2]==0x56 && b[3]==0x57)
                    gProcessEventAddr = fa;
            }
        }
    }

    // Don't install tick hook here - do it in F9 when we know the level window is ready

    Log("Cached: BeginSpawn=%s FinishSpawn=%s GetLoc=%s SetLoc=%s Constr=%s BeginPlay=%s CDO=%s PE=%s Tick=%s\n",
        gBeginSpawnFunc?"OK":"NO", gFinishSpawnFunc?"OK":"NO", gGetLocFunc?"OK":"NO",
        gSetLocFunc?"OK":"NO", gConstructionFunc?"OK":"NO", gBeginPlayFunc?"OK":"NO",
        gGameplayStaticsCDO?"OK":"NO", gProcessEventAddr?"OK":"NO", "OK");

    return gBeginSpawnFunc && gFinishSpawnFunc && gGameplayStaticsCDO && gProcessEventAddr;
}

// Get actor location via ProcessEvent on any object
static bool GetActorLoc(void* actor, double* x, double* y, double* z) {
    if (!gGetLocFunc || !gProcessEventAddr || !actor) return false;
    __declspec(align(16)) uint8_t lp[128] = {};
    __try {
        ((ProcessEvent_fn)gProcessEventAddr)(actor, gGetLocFunc, lp);
        *x = *(double*)(lp); *y = *(double*)(lp+8); *z = *(double*)(lp+16);
        return (*x != 0 || *y != 0 || *z != 0);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Get player PAWN location (not controller!)
static bool GetPlayerLoc(double* x, double* y, double* z) {
    if (!gGetLocFunc || !gProcessEventAddr) return false;
    wchar_t cb[256];
    // Try pawn classes first (the actual character in the world)
    const wchar_t* pawnClasses[] = {L"CharPlayer_Dungeon", L"CharPlayer", L"ValChar", nullptr};
    for (int pc = 0; pawnClasses[pc]; pc++) {
        for (int i = 0; i < gNumElements; i++) {
            void* o = GetUObject(i); if (!o || IsCDO(o)) continue;
            void* c = GetObjClass(o); if (!c) continue;
            if (!GetObjectName(c, cb, 256) || !wcsstr(cb, pawnClasses[pc])) continue;
            if (GetActorLoc(o, x, y, z)) return true;
        }
    }
    return false;
}

// ============================================================
// Key handlers
// ============================================================

static bool IsKeyJustPressed(int vk) {
    static bool p[256] = {};
    bool c = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool w = p[vk]; p[vk] = c;
    return c && !w;
}

// ============================================================
// Reusable actions — shared by hotkeys (F9/F12) and the auto-on-load path
// ============================================================

static const int CURRENCY_ID_OFFSET = 0x55C; // FPrimaryAssetId in WBP_Game_Currency_C body

// True when the current level already provides sanctify natively (zone 4):
// the SafeRoom EncounterManager exists, or GameState bSanctifyingAvailable is set.
static bool IsZone4Native() {
    wchar_t cb[256];
    for (int i = 0; i < gNumElements; i++) {
        void* o = GetUObject(i); if (!o || IsCDO(o)) continue;
        void* c = GetObjClass(o); if (!c || !GetObjectName(c, cb, 256)) continue;
        if (wcsstr(cb, L"EncounterManager") && wcsstr(cb, L"SafeRoom")) return true;
        if (wcsstr(cb, L"GameState") && wcsstr(cb, L"RogueLite")) {
            int32_t v = 0;
            if (SafeRead32((uintptr_t)o + GAMESTATE_SANCTIFY_BOOL_OFFSET, &v) && (v & 0xFF)) return true;
        }
    }
    return false;
}

// Find the active gameplay World instance (a fresh one is created each level load).
static void* FindCurrentWorld() {
    wchar_t cb[256], nb[256];
    for (int i = 0; i < gNumElements; i++) {
        void* o = GetUObject(i); if (!o || IsCDO(o)) continue;
        void* c = GetObjClass(o); if (!c || !GetObjectName(c, cb, 256)) continue;
        if (wcscmp(cb, L"World") != 0) continue;
        if (!GetObjectName(o, nb, 256)) continue;
        if (wcsstr(nb, L"RogueLite") || wcsstr(nb, L"Mission")) return o;
    }
    return nullptr;
}

// True if a SanctityToken currency widget already exists (native zone 4, or already added).
static bool SanctityWidgetPresent() {
    wchar_t cb[256], ns[256];
    for (int i = 0; i < gNumElements; i++) {
        void* o = GetUObject(i); if (!o || IsCDO(o)) continue;
        void* c = GetObjClass(o); if (!c || !GetObjectName(c, cb, 256)) continue;
        if (wcscmp(cb, L"WBP_Game_Currency_C") != 0) continue;
        uintptr_t a = (uintptr_t)o + CURRENCY_ID_OFFSET + 8; // name FName
        if (!IsSafeToRead((void*)a, 8)) continue;
        FName n; SafeRead32(a + 0, &n.ComparisonIndex); SafeRead32(a + 4, &n.Number);
        if (GetFNameStr(&n, ns, 256) && wcsstr(ns, L"SanctityToken")) return true;
    }
    return false;
}

// Spawn sanctified anvils at every fabricator. Returns 0 = not ready (retry later),
// 1 = attempted (do not retry — avoids duplicate anvils).
// requireFabricators: when true (auto path) don't fall back to player position; wait
// until fabricators have loaded.
static int DoSpawnAnvils(bool requireFabricators) {
    Log("=== SPAWN ANVILS (requireFab=%d) ===\n", requireFabricators ? 1 : 0);
    SafeRead32((uintptr_t)gObjArrayBase + 0x14, &gNumElements);
    SafeRead32((uintptr_t)gObjArrayBase + 0x1C, &gNumChunks);

    void* anvilClass = nullptr;
    void* world = nullptr;
    void* freshBS = nullptr, *freshFS = nullptr, *freshSL = nullptr;
    void* freshCS = nullptr, *freshBP = nullptr, *freshCDO = nullptr;
    void* ssaFunc = nullptr, *esaFunc = nullptr;
    void* gmInst = nullptr, *srmInst = nullptr, *gsInst = nullptr, *pscInst = nullptr;
    void* fabInstances[32] = {}; int fabCount = 0;
    wchar_t nb[256], cb[256], ob[256];

    for (int i = 0; i < gNumElements; i++) {
        void* o = GetUObject(i); if (!o) continue;
        if (!GetObjectName(o, nb, 256)) continue;

        int32_t ps = 0;
        if (wcscmp(nb, L"BeginDeferredActorSpawnFromClass") == 0) {
            SafeRead32((uintptr_t)o + 0x58, &ps);
            if (ps >= 100 && ps <= 200 && !freshBS) freshBS = o;
        }
        else if (wcscmp(nb, L"FinishSpawningActor") == 0) {
            SafeRead32((uintptr_t)o + 0x58, &ps);
            if (ps >= 100 && ps <= 200 && !freshFS) freshFS = o;
        }
        else if (wcscmp(nb, L"K2_SetActorLocation") == 0) {
            SafeRead32((uintptr_t)o + 0x58, &ps);
            if (ps >= 200 && !freshSL) freshSL = o;
        }
        else if (wcscmp(nb, L"UserConstructionScript") == 0) {
            SafeRead32((uintptr_t)o + 0x58, &ps);
            void* ou = GetObjOuter(o);
            if (ou) { GetObjectName(ou, ob, 256); if (wcsstr(ob, L"SanctifiedAnvil")) freshCS = o; }
            if (ps <= 16 && !freshCS) freshCS = o;
        }
        else if (wcscmp(nb, L"ReceiveBeginPlay") == 0) {
            SafeRead32((uintptr_t)o + 0x58, &ps);
            void* ou = GetObjOuter(o);
            if (ou) { GetObjectName(ou, ob, 256); if (wcsstr(ob, L"SanctifiedAnvil")) freshBP = o; }
            if (ps <= 16 && !freshBP) freshBP = o;
        }
        else if (wcscmp(nb, L"Default__GameplayStatics") == 0) { if (!freshCDO) freshCDO = o; }
        else if (wcscmp(nb, L"SetSanctifiedAnvil") == 0) {
            SafeRead32((uintptr_t)o + 0x58, &ps);
            if (ps >= 4 && ps <= 16 && !ssaFunc) ssaFunc = o;
        }
        else if (wcscmp(nb, L"EnableSanctifiedAnvil") == 0) {
            SafeRead32((uintptr_t)o + 0x58, &ps);
            if (ps >= 100 && !esaFunc) esaFunc = o;
        }

        if (wcsncmp(nb, L"Default__", 9) == 0) continue;

        void* c = GetObjClass(o); if (!c) continue;
        if (!GetObjectName(c, cb, 256)) continue;

        if (!anvilClass && (wcscmp(cb, L"BlueprintGeneratedClass") == 0 || wcscmp(cb, L"Class") == 0)
            && wcscmp(nb, L"BP_SanctifiedAnvil_C") == 0) anvilClass = o;

        if (!world && wcscmp(cb, L"World") == 0 && (wcsstr(nb, L"RogueLite") || wcsstr(nb, L"Mission")))
            world = o;

        if (wcscmp(cb, L"BP_Fabricator_C") == 0 && fabCount < 32)
            fabInstances[fabCount++] = o;

        if (!gmInst && wcsstr(cb, L"GameMode") && (wcsstr(cb, L"Dungeon") || wcsstr(cb, L"RogueLite")))
            gmInst = o;
        if (!srmInst && wcsstr(cb, L"EncounterManager") && wcsstr(cb, L"SafeRoom"))
            srmInst = o;
        if (!gsInst && wcsstr(cb, L"GameState") && wcsstr(cb, L"RogueLite"))
            gsInst = o;
        if (!pscInst && wcsstr(cb, L"ValPlayerSanctifyComponent") && !wcsstr(nb, L"GEN_VARIABLE"))
            pscInst = o;
    }

    uintptr_t freshPE = 0;
    if (freshCDO) {
        uintptr_t vt = 0; SafeRead64((uintptr_t)freshCDO, &vt);
        if (vt) { uintptr_t fa = 0; SafeRead64(vt + PROCESS_EVENT_VTABLE_SLOT * 8, &fa);
            if (fa >= gTextStart && fa < gTextStart + gTextSize) freshPE = fa; }
    }

    Log("Anvil=%s World=%s BS=%s FS=%s PE=%s Fabs=%d\n",
        anvilClass?"OK":"NO", world?"OK":"NO", freshBS?"OK":"NO", freshFS?"OK":"NO",
        freshPE?"OK":"NO", fabCount);

    if (!anvilClass || !world || !freshBS || !freshFS || !freshCDO || !freshPE) {
        Log("Missing critical objects — not ready\n"); return 0;
    }

    int sc = 0;
    for (int fi = 0; fi < fabCount && sc < 32; fi++) {
        double fx, fy, fz;
        if (GetActorLoc(fabInstances[fi], &fx, &fy, &fz)) {
            gSpawnReq.spawnX[sc] = fx - 576.8;
            gSpawnReq.spawnY[sc] = fy + 35.5;
            gSpawnReq.spawnZ[sc] = fz - 123.1;
            sc++;
        }
    }
    if (sc == 0) {
        if (requireFabricators) { Log("No fabricators loaded yet — not ready\n"); return 0; }
        double px, py, pz;
        if (GetPlayerLoc(&px, &py, &pz)) {
            gSpawnReq.spawnX[0] = px + 300; gSpawnReq.spawnY[0] = py; gSpawnReq.spawnZ[0] = pz; sc = 1;
        }
    }
    gSpawnReq.spawnCount = sc;

    gSpawnReq.bpAnvilClass = anvilClass;
    gSpawnReq.gameWorld = world;
    gSpawnReq.spawnFunc = freshBS;
    gSpawnReq.finishSpawnFunc = freshFS;
    gSpawnReq.gameplayStaticsCDO = freshCDO;
    gSpawnReq.processEventAddr = freshPE;
    gSpawnReq.setLocFunc = freshSL;
    gSpawnReq.constructionFunc = freshCS;
    gSpawnReq.beginPlayFunc = freshBP;
    gSpawnReq.rerunConstructionFunc = nullptr;
    gSpawnReq.setSanctifiedAnvilFunc = ssaFunc;
    gSpawnReq.gameModeInstance = gmInst;
    gSpawnReq.enableSAFunc = esaFunc;
    gSpawnReq.safeRoomMgr = srmInst;
    gSpawnReq.gameStateInst = gsInst;
    gSpawnReq.playerSanctifyComp = pscInst;

    Log("Spawning %d anvils...\n", sc);
    gSpawnReq.resultReady = false;
    gSpawnReq.success = false;

    TickHook::Uninstall();
    if (!TickHook::Install(0)) { Log("FAILED to install window timer!\n"); return 1; }

    TickHook::QueueOnGameThread(GameThreadSpawn);
    for (int w = 0; w < 100 && !gSpawnReq.resultReady; w++) Sleep(50);
    if (gSpawnReq.success) Log("*** ANVIL VISIBLE! ***\n");
    else Log("Spawn may have failed\n");
    return 1;
}

// Add the SanctityToken vial widget to the live HUD. Returns 0 = not ready (retry),
// 1 = added, 2 = already present (skip).
static int DoAddVials() {
    Log("=== ADD VIALS ===\n");
    SafeRead32((uintptr_t)gObjArrayBase + 0x14, &gNumElements);
    SafeRead32((uintptr_t)gObjArrayBase + 0x1C, &gNumChunks);

    if (SanctityWidgetPresent()) { Log("SanctityToken widget already present — skip\n"); return 2; }

    uint8_t currencyId[16] = {};
    bool haveId = BuildSanctityCurrencyId(currencyId);
    if (!haveId && gSanctityCurrencyIdValid) { memcpy(currencyId, gSanctityCurrencyId, 16); haveId = true; }
    if (!haveId) { Log("could not resolve SanctityToken CurrencyId\n"); return 0; }
    if (!gProcessEventAddr) { Log("ProcessEvent not available\n"); return 0; }

    void* addCurrFunc = nullptr;
    void* container = nullptr;
    wchar_t nb[256], cb[256], ob[256];
    for (int i = 0; i < gNumElements; i++) {
        void* o = GetUObject(i); if (!o) continue;
        if (!GetObjectName(o, nb, 256)) continue;
        if (!addCurrFunc && wcscmp(nb, L"AddCurrencyWidget") == 0) {
            void* ou = GetObjOuter(o);
            if (ou && GetObjectName(ou, ob, 256) && wcsstr(ob, L"WBP_Game_Currencies_C"))
                addCurrFunc = o;
        }
        if (IsCDO(o)) continue;
        void* c = GetObjClass(o); if (!c) continue;
        if (!GetObjectName(c, cb, 256)) continue;
        if (!container && wcscmp(cb, L"WBP_Game_Currencies_C") == 0 && wcscmp(nb, L"Currencies") == 0) {
            void* ou = GetObjOuter(o);
            void* ouou = ou ? GetObjOuter(ou) : nullptr;
            if (ouou && GetObjectName(ouou, ob, 256) && wcsstr(ob, L"UI_Game_Visor_C")) container = o;
        }
    }

    if (!addCurrFunc || !container) { Log("HUD container/func not ready (addFn=%s cont=%s)\n",
        addCurrFunc?"OK":"NO", container?"OK":"NO"); return 0; }

    gAddReq.container = container;
    gAddReq.addCurrencyWidgetFunc = addCurrFunc;
    gAddReq.processEventAddr = gProcessEventAddr;
    memcpy(gAddReq.currencyId, currencyId, 16);
    gAddReq.done = false; gAddReq.success = false;

    TickHook::Uninstall();
    if (!TickHook::Install(0)) { Log("tick hook install failed\n"); return 0; }
    TickHook::QueueOnGameThread(GameThreadAddCurrency);
    for (int w = 0; w < 100 && !gAddReq.done; w++) Sleep(50);
    Log(gAddReq.success ? "vial added OK\n" : "vial add failed\n");
    return gAddReq.success ? 1 : 0;
}

// ============================================================
// Main thread
// ============================================================

static DWORD WINAPI ModThreadProc(LPVOID) {
    Sleep(5000);
    char lp[MAX_PATH];
    if (GetEnvironmentVariableA("USERPROFILE", lp, MAX_PATH)) strcat_s(lp, "\\Desktop\\SanctifyMod.log");
    gLogFile = fopen(lp, "w");
    if (!gLogFile) return 0;

    Log("=== SanctifyEverywhere v0.9 ===\n");
    Log("Background thread ID: %lu\n", GetCurrentThreadId());

    HWND gw = nullptr;
    for (int i = 0; i < 120 && gRunning; i++) { Sleep(1000); gw = FindWindowA("UnrealWindow", nullptr); if (gw) break; }
    if (!gw) { Log("No window\n"); return 0; }

    Log("Window found, waiting for level load...\n");
    Sleep(10000);

    if (!InitUE()) { Log("InitUE FAILED\n"); goto idle; }
    if (!CacheObjects()) { Log("CacheObjects FAILED\n"); goto idle; }

    Log("\n*** READY! Press F9 to spawn anvil, F10 to capture position ***\n\n");

idle:
    while (gRunning) {
        Sleep(100);

        // ---- AUTO: spawn anvils + add vials on each new level load (zones 1-3) ----
        // Runs regardless of window focus, throttled to ~1s. Detects a new gameplay
        // World, waits ~3s for it to settle, skips zone 4 (native), and otherwise
        // applies anvils + vials once per world. Manual F9/F12 remain as fallback.
        if (gObjArrayBase) {
            static void* autoWorld = nullptr;
            static bool  autoAnvilsDone = false;
            static bool  autoVialsDone = false;
            static bool  autoSkipZone4 = false;
            static int   autoAttempts = 0;
            static DWORD autoFirstSeen = 0;
            static DWORD autoNextScan = 0;

            DWORD now = GetTickCount();
            if ((int)(now - autoNextScan) >= 0) {
                autoNextScan = now + 1000; // throttle full scans to ~1s

                SafeRead32((uintptr_t)gObjArrayBase + 0x14, &gNumElements);
                SafeRead32((uintptr_t)gObjArrayBase + 0x1C, &gNumChunks);

                void* w = FindCurrentWorld();
                if (w != autoWorld) {
                    autoWorld = w;
                    autoAnvilsDone = autoVialsDone = autoSkipZone4 = false;
                    autoAttempts = 0;
                    autoFirstSeen = now;
                    if (w) Log("[Auto] New world 0x%llX — will apply after settle\n", (uintptr_t)w);
                    else   Log("[Auto] No gameplay world (menu/loading)\n");
                }

                if (autoWorld && !autoSkipZone4 && !(autoAnvilsDone && autoVialsDone)
                    && (int)(now - autoFirstSeen) >= 3000) {

                    if (IsZone4Native()) {
                        autoSkipZone4 = true;
                        Log("[Auto] Zone 4 (native sanctify) — skipping auto spawn/vials\n");
                    } else {
                        autoAttempts++;
                        if (!autoAnvilsDone) {
                            int r = DoSpawnAnvils(true); // wait for fabricators, no player fallback
                            if (r == 1) { autoAnvilsDone = true; Log("[Auto] Anvils done\n"); }
                        }
                        if (!autoVialsDone) {
                            int r = DoAddVials();
                            if (r != 0) { autoVialsDone = true; Log("[Auto] Vials done (r=%d)\n", r); }
                        }
                        if (autoAttempts >= 20 && !(autoAnvilsDone && autoVialsDone)) {
                            Log("[Auto] Gave up after %d attempts (anvils=%d vials=%d)\n",
                                autoAttempts, autoAnvilsDone?1:0, autoVialsDone?1:0);
                            autoAnvilsDone = autoVialsDone = true;
                        }
                    }
                }
            }
        }

        HWND fg = GetForegroundWindow();
        if (!fg) continue;
        char wc[64] = {}; GetClassNameA(fg, wc, 64);
        if (strcmp(wc, "UnrealWindow") != 0) continue;

        if (IsKeyJustPressed(VK_F9) && gProcessEventAddr && gBeginSpawnFunc) {
            Log("=== F9: SPAWN (manual) ===\n");
            DoSpawnAnvils(false); // manual: fall back to player position if no fabricators
        }

        if (IsKeyJustPressed(VK_F10) && gProcessEventAddr && gGetLocFunc) {
            SafeRead32((uintptr_t)gObjArrayBase + 0x14, &gNumElements);
            SafeRead32((uintptr_t)gObjArrayBase + 0x1C, &gNumChunks);
            double x, y, z;
            if (GetPlayerLoc(&x, &y, &z))
                Log("F10 PAWN POSITION: (%.2f, %.2f, %.2f)\n", x, y, z);
            else
                Log("F10: Could not get pawn position\n");
        }

        if (IsKeyJustPressed(VK_F7)) {
            SafeRead32((uintptr_t)gObjArrayBase + 0x14, &gNumElements);
            SafeRead32((uintptr_t)gObjArrayBase + 0x1C, &gNumChunks);
            Log("\n=== F7: CURRENCY HUD SCAN (%d objects) ===\n", gNumElements);

            void* getMatchCurrencyFunc = nullptr;
            void* setVisibilityFunc = nullptr;
            void* playerStateInst = nullptr;
            void* hudInst = nullptr;
            void* inventoryComp = nullptr;
            void* addCurrencyWidgetFunc = nullptr;
            void* setCurrencyIdFunc = nullptr;
            void* bindCurrencyFunc = nullptr;

            // Collect ALL Currencies widgets to find the HUD's one
            void* currWidgets[64] = {};
            void* currWidgetOuters[64] = {};
            int currWidgetCount = 0;

            wchar_t nb[256], cb[256], ob[256];
            for (int i = 0; i < gNumElements; i++) {
                void* o = GetUObject(i); if (!o) continue;
                if (!GetObjectName(o, nb, 256)) continue;

                // Functions
                if (!getMatchCurrencyFunc && wcscmp(nb, L"GetMatchCurrency") == 0) {
                    void* ou = GetObjOuter(o);
                    if (ou && GetObjectName(ou, ob, 256) && wcsstr(ob, L"ValPlayerState"))
                        getMatchCurrencyFunc = o;
                }
                if (!setVisibilityFunc && wcscmp(nb, L"SetVisibility") == 0) {
                    int32_t ps = 0; SafeRead32((uintptr_t)o + 0x58, &ps);
                    if (ps >= 1 && ps <= 8) {
                        setVisibilityFunc = o;
                        Log("SetVisibility: PS=%d\n", ps);
                    }
                }
                if (!addCurrencyWidgetFunc && wcscmp(nb, L"AddCurrencyWidget") == 0) {
                    void* ou = GetObjOuter(o);
                    if (ou && GetObjectName(ou, ob, 256) && wcsstr(ob, L"WBP_Game_Currencies_C"))
                        addCurrencyWidgetFunc = o;
                }
                if (!setCurrencyIdFunc && wcscmp(nb, L"SetCurrencyId") == 0) {
                    void* ou = GetObjOuter(o);
                    if (ou && GetObjectName(ou, ob, 256) && wcsstr(ob, L"WBP_Game_Currency_C"))
                        setCurrencyIdFunc = o;
                }
                if (!bindCurrencyFunc && wcscmp(nb, L"BindCurrency") == 0) {
                    void* ou = GetObjOuter(o);
                    if (ou && GetObjectName(ou, ob, 256) && wcsstr(ob, L"WBP_Game_Currency_C"))
                        bindCurrencyFunc = o;
                }

                // Log ALL functions on HUD-related classes
                void* fOuter = GetObjOuter(o);
                if (fOuter && GetObjectName(fOuter, ob, 256)) {
                    void* fClass = GetObjClass(o);
                    wchar_t fcb[256] = {};
                    if (fClass) GetObjectName(fClass, fcb, 256);
                    if ((wcscmp(fcb, L"Function") == 0 || wcscmp(fcb, L"DelegateFunction") == 0) &&
                        (wcsstr(ob, L"ValHUDStateTracker") || wcsstr(ob, L"UI_HUD_Game_RH") ||
                         wcsstr(ob, L"RHGameHUD") || wcsstr(ob, L"WBP_Game_Currencies"))) {
                        int32_t ps = 0; SafeRead32((uintptr_t)o + 0x58, &ps);
                        Log("  Func: \"%ls\" outer=\"%ls\" PS=%d\n", nb, ob, ps);
                    }
                }

                if (wcsncmp(nb, L"Default__", 9) == 0) continue;
                void* c = GetObjClass(o); if (!c) continue;
                if (!GetObjectName(c, cb, 256)) continue;

                // PlayerState
                if (!playerStateInst && wcsstr(cb, L"ValPlayerState") && !wcsstr(cb, L"BlueprintGenerated")) {
                    playerStateInst = o;
                    Log("PlayerState: 0x%llX class=\"%ls\"\n", (uintptr_t)o, cb);
                }

                // HUD instance
                if (wcsstr(cb, L"UI_HUD_Game_RH_C") && !wcsstr(cb, L"BlueprintGenerated")) {
                    void* ou = GetObjOuter(o);
                    if (ou && GetObjectName(ou, ob, 256) && wcsstr(ob, L"PersistentLevel")) {
                        hudInst = o;
                        Log("HUD: 0x%llX name=\"%ls\"\n", (uintptr_t)o, nb);
                    }
                }

                // ALL Currencies widgets
                if (wcscmp(cb, L"WBP_Game_Currencies_C") == 0 && wcscmp(nb, L"Currencies") == 0) {
                    if (currWidgetCount < 64) {
                        void* ou = GetObjOuter(o);
                        currWidgets[currWidgetCount] = o;
                        currWidgetOuters[currWidgetCount] = ou;
                        currWidgetCount++;
                    }
                }

                // InventoryComp on actual player controller
                if (wcscmp(cb, L"ValInventoryComponent") == 0 && wcscmp(nb, L"InventoryComponent") == 0) {
                    void* ou = GetObjOuter(o);
                    if (ou && GetObjectName(ou, ob, 256) && wcsstr(ob, L"BP_ValPlayerController") && !wcsstr(ob, L"Default__")) {
                        inventoryComp = o;
                        Log("InventoryComp: 0x%llX\n", (uintptr_t)o);
                    }
                }
            }

            // Find which Currencies widget is inside the HUD by tracing outer chain
            void* hudCurrenciesWidget = nullptr;
            if (hudInst) {
                for (int w = 0; w < currWidgetCount; w++) {
                    // Trace: Currencies -> WidgetTree -> UserWidget (== HUD?)
                    void* wt = currWidgetOuters[w]; // WidgetTree
                    if (!wt) continue;
                    void* wtOuter = GetObjOuter(wt); // The UserWidget that owns this WidgetTree
                    if (!wtOuter) continue;

                    // Check if the UserWidget IS the HUD or its outer chain reaches the HUD
                    if (wtOuter == hudInst) {
                        hudCurrenciesWidget = currWidgets[w];
                        Log("HUD Currencies widget: 0x%llX (WidgetTree->HUD direct)\n", (uintptr_t)hudCurrenciesWidget);
                        break;
                    }
                    // Also check one more level (WidgetTree -> subwidget -> HUD)
                    void* wtOuterOuter = GetObjOuter(wtOuter);
                    if (wtOuterOuter == hudInst) {
                        hudCurrenciesWidget = currWidgets[w];
                        Log("HUD Currencies widget: 0x%llX (WidgetTree->sub->HUD)\n", (uintptr_t)hudCurrenciesWidget);
                        break;
                    }
                }

                // If not found via direct chain, try matching by name suffix
                if (!hudCurrenciesWidget) {
                    wchar_t hudName[256];
                    GetObjectName(hudInst, hudName, 256);
                    // Extract the numeric suffix from the HUD name
                    for (int w = 0; w < currWidgetCount; w++) {
                        void* wt = currWidgetOuters[w];
                        if (!wt) continue;
                        wchar_t wtName[256];
                        GetObjectName(wt, wtName, 256);
                        // WidgetTree without a number suffix = BP template, not instance
                        // We want the one that's part of the active HUD
                        Log("  CurrWidget[%d]: outer=\"%ls\" outerOuter=", w, wtName);
                        void* wtOuter = GetObjOuter(wt);
                        if (wtOuter) {
                            wchar_t wtob[256]; GetObjectName(wtOuter, wtob, 256);
                            Log("\"%ls\"", wtob);
                            // Check if outerOuter name contains HUD identifier
                            if (wcsstr(wtob, L"UI_HUD_Game_RH_C") || wcsstr(wtob, L"ValHUDStateTracker")) {
                                hudCurrenciesWidget = currWidgets[w];
                                Log(" <-- MATCH!");
                            }
                        }
                        Log("\n");
                    }
                }
            }

            Log("\n--- RESULTS ---\n");
            Log("GetMatchCurrency: %s\n", getMatchCurrencyFunc?"OK":"NO");
            Log("SetVisibility: %s\n", setVisibilityFunc?"OK":"NO");
            Log("AddCurrencyWidget: %s (PS=73)\n", addCurrencyWidgetFunc?"OK":"NO");
            Log("SetCurrencyId: %s (PS=50)\n", setCurrencyIdFunc?"OK":"NO");
            Log("BindCurrency: %s (PS=33)\n", bindCurrencyFunc?"OK":"NO");
            Log("PlayerState: %s\n", playerStateInst?"OK":"NO");
            Log("HUD: %s\n", hudInst?"OK":"NO");
            Log("HUD Currencies: %s (0x%llX)\n", hudCurrenciesWidget?"OK":"NO", (uintptr_t)hudCurrenciesWidget);
            Log("InventoryComp: %s\n", inventoryComp?"OK":"NO");
            Log("Total Currencies widgets: %d\n", currWidgetCount);

            // Read currency value
            if (getMatchCurrencyFunc && playerStateInst && gProcessEventAddr) {
                __declspec(align(16)) uint8_t p[16] = {};
                __try {
                    ((ProcessEvent_fn)gProcessEventAddr)(playerStateInst, getMatchCurrencyFunc, p);
                    Log("*** GetMatchCurrency = %d ***\n", *(int32_t*)p);
                } __except(EXCEPTION_EXECUTE_HANDLER) { Log("GetMatchCurrency CRASHED\n"); }
            }

            // Dump bytes on the HUD's Currencies widget + its parent for visibility comparison
            if (hudCurrenciesWidget && IsSafeToRead(hudCurrenciesWidget, 0x200)) {
                uint8_t* base = (uint8_t*)hudCurrenciesWidget;
                Log("\nHUD Currencies widget bytes:\n");
                for (int row = 0x28; row < 0x178; row += 0x20) {
                    Log("  +0x%03X: ", row);
                    for (int b = 0; b < 0x20 && (row+b) < 0x178; b++)
                        Log("%02X ", base[row + b]);
                    Log("\n");
                }

                // Also trace up to the parent widget and dump its bytes
                // UPanelSlot is at some offset in UWidget, then the parent panel via slot
                // The parent UWidget outer might be the WidgetTree, check via outer
                void* parentPanel = nullptr;
                // In UE5 UWidget, the Slot pointer is typically early in the struct
                // Try reading potential parent pointers
                for (int off = 0x28; off <= 0x60; off += 8) {
                    uintptr_t ptr = 0;
                    if (!SafeRead64((uintptr_t)hudCurrenciesWidget + off, &ptr) || !ptr) continue;
                    if (!IsSafeToRead((void*)ptr, 0x28)) continue;
                    void* pc = GetObjClass((void*)ptr);
                    if (!pc) continue;
                    wchar_t pcName[256];
                    if (GetObjectName(pc, pcName, 256)) {
                        Log("  Ptr at +0x%X -> 0x%llX class=\"%ls\"", off, ptr, pcName);
                        // Check if this looks like a parent widget
                        if (wcsstr(pcName, L"CanvasPanel") || wcsstr(pcName, L"VerticalBox") ||
                            wcsstr(pcName, L"HorizontalBox") || wcsstr(pcName, L"Overlay") ||
                            wcsstr(pcName, L"Widget") || wcsstr(pcName, L"Border") ||
                            wcsstr(pcName, L"SizeBox")) {
                            parentPanel = (void*)ptr;
                            Log(" <-- parent candidate");
                        }
                        Log("\n");
                    }
                }

                if (parentPanel && IsSafeToRead(parentPanel, 0x178)) {
                    uint8_t* pbase = (uint8_t*)parentPanel;
                    wchar_t pName[256], pcName[256];
                    GetObjectName(parentPanel, pName, 256);
                    void* pc = GetObjClass(parentPanel);
                    if (pc) GetObjectName(pc, pcName, 256);
                    Log("\nParent widget: \"%ls\" class=\"%ls\" (0x%llX)\n", pName, pcName, (uintptr_t)parentPanel);
                    for (int row = 0x28; row < 0x178; row += 0x20) {
                        Log("  +0x%03X: ", row);
                        for (int b = 0; b < 0x20 && (row+b) < 0x178; b++)
                            Log("%02X ", pbase[row + b]);
                        Log("\n");
                    }
                }
            }

            Log("=== F7 DONE ===\n\n");
        }

        if (IsKeyJustPressed(VK_F6)) {
            SafeRead32((uintptr_t)gObjArrayBase + 0x14, &gNumElements);
            SafeRead32((uintptr_t)gObjArrayBase + 0x1C, &gNumChunks);
            Log("\n=== F6: CURRENCY HUD EXPERIMENT ===\n");

            // Single pass to find everything we need
            void* setVisFunc = nullptr;
            void* mechUpdatedFunc = nullptr;
            void* addCurrFunc = nullptr;
            void* setCurrIdFunc = nullptr;
            void* bindCurrFunc = nullptr;
            void* getMCFunc = nullptr;
            void* psInst = nullptr;
            void* visorCurrWidget = nullptr;

            wchar_t nb[256], cb[256], ob[256];
            for (int i = 0; i < gNumElements; i++) {
                void* o = GetUObject(i); if (!o) continue;
                if (!GetObjectName(o, nb, 256)) continue;

                // Functions
                if (!setVisFunc && wcscmp(nb, L"SetVisibility") == 0) {
                    int32_t ps = 0; SafeRead32((uintptr_t)o + 0x58, &ps);
                    if (ps >= 1 && ps <= 8) setVisFunc = o;
                }
                if (!getMCFunc && wcscmp(nb, L"GetMatchCurrency") == 0) {
                    void* ou = GetObjOuter(o);
                    if (ou && GetObjectName(ou, ob, 256) && wcsstr(ob, L"ValPlayerState"))
                        getMCFunc = o;
                }
                if (!mechUpdatedFunc && wcscmp(nb, L"OnAvailableRunProgressionMechanicsUpdated") == 0) {
                    void* ou = GetObjOuter(o);
                    if (ou && GetObjectName(ou, ob, 256) && wcsstr(ob, L"WBP_Game_Currencies_C"))
                        mechUpdatedFunc = o;
                }
                if (!addCurrFunc && wcscmp(nb, L"AddCurrencyWidget") == 0) {
                    void* ou = GetObjOuter(o);
                    if (ou && GetObjectName(ou, ob, 256) && wcsstr(ob, L"WBP_Game_Currencies_C"))
                        addCurrFunc = o;
                }
                if (!setCurrIdFunc && wcscmp(nb, L"SetCurrencyId") == 0) {
                    void* ou = GetObjOuter(o);
                    if (ou && GetObjectName(ou, ob, 256) && wcsstr(ob, L"WBP_Game_Currency_C"))
                        setCurrIdFunc = o;
                }
                if (!bindCurrFunc && wcscmp(nb, L"BindCurrency") == 0) {
                    void* ou = GetObjOuter(o);
                    if (ou && GetObjectName(ou, ob, 256) && wcsstr(ob, L"WBP_Game_Currency_C"))
                        bindCurrFunc = o;
                }

                if (wcsncmp(nb, L"Default__", 9) == 0) continue;
                void* c = GetObjClass(o); if (!c) continue;
                if (!GetObjectName(c, cb, 256)) continue;

                // PlayerState
                if (!psInst && wcsstr(cb, L"ValPlayerState") && !wcsstr(cb, L"BlueprintGenerated"))
                    psInst = o;

                // Currencies widget inside UI_Game_Visor_C
                if (!visorCurrWidget && wcscmp(cb, L"WBP_Game_Currencies_C") == 0 && wcscmp(nb, L"Currencies") == 0) {
                    void* ou = GetObjOuter(o); // WidgetTree
                    if (ou) {
                        void* ouou = GetObjOuter(ou); // The UserWidget
                        if (ouou && GetObjectName(ouou, ob, 256) && wcsstr(ob, L"UI_Game_Visor_C")) {
                            visorCurrWidget = o;
                            Log("Found Visor Currencies widget: 0x%llX (outer->%ls)\n", (uintptr_t)o, ob);
                        }
                    }
                }
            }

            Log("SetVis=%s MechUpd=%s AddCurr=%s SetId=%s Bind=%s GetMC=%s PS=%s Visor=%s\n",
                setVisFunc?"OK":"NO", mechUpdatedFunc?"OK":"NO", addCurrFunc?"OK":"NO",
                setCurrIdFunc?"OK":"NO", bindCurrFunc?"OK":"NO", getMCFunc?"OK":"NO",
                psInst?"OK":"NO", visorCurrWidget?"OK":"NO");

            if (!visorCurrWidget || !gProcessEventAddr) {
                Log("Missing critical objects for experiment\n");
            } else {
                gCurrReq.visorCurrenciesWidget = visorCurrWidget;
                gCurrReq.setVisibilityFunc = setVisFunc;
                gCurrReq.onMechanicsUpdatedFunc = mechUpdatedFunc;
                gCurrReq.addCurrencyWidgetFunc = addCurrFunc;
                gCurrReq.setCurrencyIdFunc = setCurrIdFunc;
                gCurrReq.bindCurrencyFunc = bindCurrFunc;
                gCurrReq.playerStateInst = psInst;
                gCurrReq.getMatchCurrencyFunc = getMCFunc;
                gCurrReq.processEventAddr = gProcessEventAddr;
                gCurrReq.done = false;
                gCurrReq.success = false;

                TickHook::Uninstall();
                if (TickHook::Install(0)) {
                    TickHook::QueueOnGameThread(GameThreadCurrencyExperiment);
                    for (int w = 0; w < 100 && !gCurrReq.done; w++) Sleep(50);
                    if (gCurrReq.success)
                        Log("=== F6 EXPERIMENT SUCCEEDED ===\n\n");
                    else
                        Log("=== F6 EXPERIMENT FAILED ===\n\n");
                } else {
                    Log("Failed to install tick hook\n");
                }
            }
        }

        if (IsKeyJustPressed(VK_F8)) {
            SafeRead32((uintptr_t)gObjArrayBase + 0x14, &gNumElements);
            SafeRead32((uintptr_t)gObjArrayBase + 0x1C, &gNumChunks);
            Log("\n=== F8: CURRENCY DATA MODEL DUMP (%d objects) ===\n", gNumElements);

            void* addCurrFunc = nullptr;   // AddCurrencyWidget on WBP_Game_Currencies_C
            void* setCurrIdFunc = nullptr; // SetCurrencyId on WBP_Game_Currency_C
            void* bindCurrFunc = nullptr;  // BindCurrency on WBP_Game_Currency_C
            void* getMCFunc = nullptr;     // GetMatchCurrency on ValPlayerState

            wchar_t nb[256], ob[256], cb[256];
            for (int i = 0; i < gNumElements; i++) {
                void* o = GetUObject(i); if (!o) continue;
                if (!GetObjectName(o, nb, 256)) continue;

                if (!addCurrFunc && wcscmp(nb, L"AddCurrencyWidget") == 0) {
                    void* ou = GetObjOuter(o);
                    if (ou && GetObjectName(ou, ob, 256) && wcsstr(ob, L"WBP_Game_Currencies_C"))
                        addCurrFunc = o;
                }
                if (!setCurrIdFunc && wcscmp(nb, L"SetCurrencyId") == 0) {
                    void* ou = GetObjOuter(o);
                    if (ou && GetObjectName(ou, ob, 256) && wcsstr(ob, L"WBP_Game_Currency_C"))
                        setCurrIdFunc = o;
                }
                if (!bindCurrFunc && wcscmp(nb, L"BindCurrency") == 0) {
                    void* ou = GetObjOuter(o);
                    if (ou && GetObjectName(ou, ob, 256) && wcsstr(ob, L"WBP_Game_Currency_C"))
                        bindCurrFunc = o;
                }
                if (!getMCFunc && wcscmp(nb, L"GetMatchCurrency") == 0) {
                    void* ou = GetObjOuter(o);
                    if (ou && GetObjectName(ou, ob, 256) && wcsstr(ob, L"ValPlayerState"))
                        getMCFunc = o;
                }
            }

            Log("\n--- FUNCTION PARAMETERS ---\n");
            if (getMCFunc)   { Log("GetMatchCurrency (0x%llX):\n", (uintptr_t)getMCFunc);     DumpStructProperties(getMCFunc, L"GetMatchCurrency"); }   else Log("GetMatchCurrency NOT FOUND\n");
            if (addCurrFunc) { Log("AddCurrencyWidget (0x%llX):\n", (uintptr_t)addCurrFunc);   DumpStructProperties(addCurrFunc, L"AddCurrencyWidget"); } else Log("AddCurrencyWidget NOT FOUND\n");
            if (setCurrIdFunc){ Log("SetCurrencyId (0x%llX):\n", (uintptr_t)setCurrIdFunc);    DumpStructProperties(setCurrIdFunc, L"SetCurrencyId"); }  else Log("SetCurrencyId NOT FOUND\n");
            if (bindCurrFunc){ Log("BindCurrency (0x%llX):\n", (uintptr_t)bindCurrFunc);       DumpStructProperties(bindCurrFunc, L"BindCurrency"); }   else Log("BindCurrency NOT FOUND\n");

            // Dump every Enum/ScriptStruct whose name mentions Currency.
            Log("\n--- CURRENCY ENUMS / STRUCTS ---\n");
            for (int i = 0; i < gNumElements; i++) {
                void* o = GetUObject(i); if (!o) continue;
                if (!GetObjectName(o, nb, 256)) continue;
                if (!wcsstr(nb, L"Currency")) continue;
                void* c = GetObjClass(o); if (!c) continue;
                if (!GetObjectName(c, cb, 256)) continue;
                if (wcscmp(cb, L"Enum") == 0) {
                    Log("ENUM \"%ls\" (0x%llX):\n", nb, (uintptr_t)o);
                    DumpEnum(o);
                } else if (wcscmp(cb, L"ScriptStruct") == 0) {
                    Log("STRUCT \"%ls\" (0x%llX):\n", nb, (uintptr_t)o);
                    DumpStructProperties(o, nb);
                }
            }

            Log("=== F8 DONE ===\n\n");
        }

        if (IsKeyJustPressed(VK_F5)) {
            SafeRead32((uintptr_t)gObjArrayBase + 0x14, &gNumElements);
            SafeRead32((uintptr_t)gObjArrayBase + 0x1C, &gNumChunks);
            Log("\n=== F5: VIAL/NANITE DISCOVERY (%d objects) ===\n", gNumElements);

            const wchar_t* kw[] = { L"vial", L"nanite", L"scrap", L"wallet", L"resource" };
            const int nkw = 5;
            wchar_t nb[256], cb[256], ob[256];
            int hits = 0, currWidgetInsts = 0;
            for (int i = 0; i < gNumElements; i++) {
                void* o = GetUObject(i); if (!o) continue;
                if (!GetObjectName(o, nb, 256)) continue;
                void* c = GetObjClass(o);
                cb[0] = 0; if (c) GetObjectName(c, cb, 256);

                bool match = false;
                for (int k = 0; k < nkw; k++) if (ContainsCI(nb, kw[k]) || ContainsCI(cb, kw[k])) { match = true; break; }
                if (match && hits < 400) {
                    void* ou = GetObjOuter(o); ob[0]=0; if (ou) GetObjectName(ou, ob, 256);
                    Log("  obj=\"%ls\" class=\"%ls\" outer=\"%ls\" (0x%llX)\n", nb, cb, ob, (uintptr_t)o);
                    hits++;
                    if (wcscmp(cb, L"Enum") == 0) DumpEnum(o);
                }

                // Live individual-currency widget instances (one shows the vial count in zone 4)
                if (wcscmp(cb, L"WBP_Game_Currency_C") == 0 && wcsncmp(nb, L"Default__", 9) != 0) {
                    if (currWidgetInsts < 32 && IsSafeToRead(o, 0x300)) {
                        uint8_t* base = (uint8_t*)o;
                        Log("  CURRENCY WIDGET \"%ls\" (0x%llX):\n", nb, (uintptr_t)o);
                        for (int off = 0x100; off < 0x300; off += 4) {
                            int32_t v = *(int32_t*)(base + off);
                            if (v > 0 && v < 1000000)
                                Log("    +0x%03X = %d\n", off, v);
                        }
                        currWidgetInsts++;
                    }
                }
            }
            Log("Discovery hits=%d, currency-widget instances=%d\n", hits, currWidgetInsts);
            Log("=== F5 DONE ===\n\n");
        }

        // Trace each currency widget to the model object it's bound to (shared source).
        if (IsKeyJustPressed(VK_F3)) {
            SafeRead32((uintptr_t)gObjArrayBase + 0x14, &gNumElements);
            SafeRead32((uintptr_t)gObjArrayBase + 0x1C, &gNumChunks);
            Log("\n=== F3: CURRENCY WIDGET -> MODEL TRACE (%d objects) ===\n", gNumElements);

            const wchar_t* tk[] = { L"currency", L"wallet", L"data", L"item", L"model",
                                    L"match", L"run", L"store", L"inventory", L"player",
                                    L"nanite", L"vial" };
            const int ntk = 12;
            wchar_t nb[256], cb[256], tnb[256], tcb[256];
            int widgets = 0;
            for (int i = 0; i < gNumElements; i++) {
                void* o = GetUObject(i); if (!o) continue;
                if (!GetObjectName(o, nb, 256) || wcsncmp(nb, L"Default__", 9) == 0) continue;
                void* c = GetObjClass(o); if (!c || !GetObjectName(c, cb, 256)) continue;
                if (wcscmp(cb, L"WBP_Game_Currency_C") != 0) continue;
                if (widgets++ >= 16) break;

                int32_t v570 = 0, v574 = 0;
                SafeRead32((uintptr_t)o + 0x570, &v570);
                SafeRead32((uintptr_t)o + 0x574, &v574);
                Log("WIDGET \"%ls\" (0x%llX) value@0x570=%d from@0x574=%d\n",
                    nb, (uintptr_t)o, v570, v574);

                for (int off = 0x28; off < 0x800; off += 8) {
                    uintptr_t p = 0;
                    if (!SafeRead64((uintptr_t)o + off, &p) || p < 0x10000) continue;
                    void* tc = GetObjClass((void*)p); if (!tc) continue;
                    if (!GetObjectName(tc, tcb, 256)) continue;
                    if (!GetObjectName((void*)p, tnb, 256)) continue;
                    bool rel = false;
                    for (int k = 0; k < ntk; k++) if (ContainsCI(tcb, tk[k]) || ContainsCI(tnb, tk[k])) { rel = true; break; }
                    if (!rel) continue;
                    Log("    +0x%03X -> 0x%llX \"%ls\" class=\"%ls\"\n", off, p, tnb, tcb);
                }
            }
            Log("Traced %d currency widgets\n=== F3 DONE ===\n\n", widgets);
        }

        // CE-style changed-value scan: 1st press snapshots, spend vials, 2nd press diffs.
        if (IsKeyJustPressed(VK_F4)) {
            SafeRead32((uintptr_t)gObjArrayBase + 0x14, &gNumElements);
            SafeRead32((uintptr_t)gObjArrayBase + 0x1C, &gNumChunks);

            static const int SNAP_MAX = 2500000;
            static struct { void* obj; int32_t off; int32_t val; } *snap = nullptr;
            static int snapN = 0;
            static bool armed = false;
            if (!snap) snap = (decltype(snap))VirtualAlloc(nullptr, sizeof(*snap)*SNAP_MAX, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
            if (!snap) { Log("F4: alloc failed\n"); return 0; }

            // Broadened: gameplay singletons most likely to HOLD a match currency.
            const wchar_t* ck[] = { L"playerstate", L"gamestate", L"inventory", L"wallet",
                                    L"currency", L"nanite", L"vial", L"progression",
                                    L"subsystem", L"manager", L"tracker", L"match",
                                    L"run", L"mode", L"attribute", L"ability" };
            const int nck = 16;
            wchar_t nb[256], cb[256];

            if (!armed) {
                snapN = 0;
                Log("\n=== F4: SNAPSHOT ===\n");
                for (int i = 0; i < gNumElements && snapN < SNAP_MAX; i++) {
                    void* o = GetUObject(i); if (!o) continue;
                    if (!GetObjectName(o, nb, 256) || wcsncmp(nb, L"Default__", 9)==0) continue;
                    void* c = GetObjClass(o); if (!c || !GetObjectName(c, cb, 256)) continue;
                    if (wcscmp(cb, L"WBP_Game_Currency_C") == 0) continue; // skip display mirrors
                    bool cur = false; for (int k=0;k<nck;k++) if (ContainsCI(cb, ck[k])) { cur=true; break; }
                    if (!cur) continue;
                    // One VirtualQuery to bound the read, then read ints directly (fast).
                    MEMORY_BASIC_INFORMATION mbi;
                    if (!VirtualQuery(o, &mbi, sizeof(mbi))) continue;
                    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS|PAGE_GUARD)) || !mbi.Protect) continue;
                    uintptr_t maxOff = ((uintptr_t)mbi.BaseAddress + mbi.RegionSize) - (uintptr_t)o;
                    if (maxOff > 0x1200) maxOff = 0x1200;
                    __try {
                        for (uintptr_t off = 0x10; off + 4 <= maxOff && snapN < SNAP_MAX; off += 4) {
                            int32_t v = *(int32_t*)((uint8_t*)o + off);
                            float f = *(float*)((uint8_t*)o + off);
                            bool keepInt = (v > 0 && v <= 10000000);
                            bool keepFloat = (f >= 1.0f && f <= 10000000.0f && f == floorf(f));
                            if (!keepInt && !keepFloat) continue;
                            snap[snapN].obj=o; snap[snapN].off=(int32_t)off; snap[snapN].val=v; snapN++;
                        }
                    } __except(EXCEPTION_EXECUTE_HANDLER) {}
                }
                armed = true;
                Log("Snapshot stored %d int fields. Spend some vials, then press F4 again.\n", snapN);
                Log("=== F4 SNAPSHOT DONE ===\n\n");
            } else {
                Log("\n=== F4: DIFF ===\n");
                int changes = 0;
                for (int i = 0; i < snapN; i++) {
                    if (!IsSafeToRead((uint8_t*)snap[i].obj + snap[i].off, 4)) continue;
                    int32_t now = *(int32_t*)((uint8_t*)snap[i].obj + snap[i].off);
                    if (now == snap[i].val) continue;
                    if (changes < 600) {
                        GetObjectName(snap[i].obj, nb, 256);
                        void* c = GetObjClass(snap[i].obj); cb[0]=0; if (c) GetObjectName(c, cb, 256);
                        float fOld = *(float*)&snap[i].val, fNow = *(float*)&now;
                        Log("  CHANGED \"%ls\" class=\"%ls\" +0x%03X: int %d -> %d | float %.2f -> %.2f  (0x%llX)\n",
                            nb, cb, snap[i].off, snap[i].val, now, fOld, fNow, (uintptr_t)snap[i].obj);
                    }
                    changes++;
                }
                armed = false;
                Log("Total changed fields: %d (re-press F4 to take a fresh snapshot)\n", changes);
                Log("=== F4 DIFF DONE ===\n\n");
            }
        }

        // F1: pointer-owner reverse-resolver. For a CE address that F2 reports as heap
        // (not inside any object body), find which UObject holds a POINTER into the heap
        // block containing it. Reports owner object + field offset, plus the element offset
        // (delta) of the target within the pointed-to buffer. Same .CT source as F2.
        // Use after F2 flags an address as heap, to recover a stable obj->ptr->+off path.
        if (IsKeyJustPressed(VK_F1)) {
            SafeRead32((uintptr_t)gObjArrayBase + 0x14, &gNumElements);
            SafeRead32((uintptr_t)gObjArrayBase + 0x1C, &gNumChunks);
            Log("\n=== F1: POINTER-OWNER SCAN (%d objects) ===\n", gNumElements);

            const char* ctPath = "C:\\Users\\jonathan\\Desktop\\Projects\\Deadzone\\DeadzoneSteam-Win64-Shipping.CT";
            uintptr_t targets[64]; int nT = 0;
            FILE* cf = nullptr; fopen_s(&cf, ctPath, "rb");
            if (!cf) {
                Log("F1: cannot open %hs\n", ctPath);
            } else {
                static char fbuf[65536];
                size_t rd = fread(fbuf, 1, sizeof(fbuf)-1, cf); fclose(cf); fbuf[rd] = 0;
                const char* p = fbuf;
                while (nT < 64) {
                    const char* a = strstr(p, "<Address>"); if (!a) break; a += 9;
                    uintptr_t v = 0; bool any = false;
                    while (*a) {
                        char ch = *a; int d;
                        if (ch>='0'&&ch<='9') d = ch-'0';
                        else if (ch>='a'&&ch<='f') d = ch-'a'+10;
                        else if (ch>='A'&&ch<='F') d = ch-'A'+10;
                        else break;
                        v = v*16 + d; any = true; a++;
                    }
                    if (any) targets[nT++] = v;
                    p = a;
                }
                Log("F1: parsed %d target addresses from .CT\n", nT);
            }

            if (nT > 0) {
                const uintptr_t WINDOW = 0x40000; // max bytes between a buffer-base ptr and the target
                struct { void* obj; int32_t off; uintptr_t delta; int hits; } B[64] = {};
                for (int t = 0; t < nT; t++) B[t].delta = (uintptr_t)-1;

                for (int i = 0; i < gNumElements; i++) {
                    void* o = GetUObject(i); if (!o) continue;
                    MEMORY_BASIC_INFORMATION mbi;
                    if (!VirtualQuery(o, &mbi, sizeof(mbi))) continue;
                    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS|PAGE_GUARD)) || !mbi.Protect) continue;
                    uintptr_t maxOff = ((uintptr_t)mbi.BaseAddress + mbi.RegionSize) - (uintptr_t)o;
                    if (maxOff > 0x1400) maxOff = 0x1400;
                    __try {
                        for (uintptr_t off = 0x10; off + 8 <= maxOff; off += 8) {
                            uintptr_t P = *(uintptr_t*)((uint8_t*)o + off);
                            if (!P) continue;
                            for (int t = 0; t < nT; t++) {
                                if (P <= targets[t] && (targets[t] - P) <= WINDOW) {
                                    B[t].hits++;
                                    if ((targets[t] - P) < B[t].delta) { B[t].delta = targets[t] - P; B[t].obj = o; B[t].off = (int32_t)off; }
                                }
                            }
                        }
                    } __except(EXCEPTION_EXECUTE_HANDLER) {}
                }

                wchar_t nb[256], cb[256];
                for (int t = 0; t < nT; t++) {
                    int32_t iv = 0; bool okv = SafeRead32(targets[t], &iv);
                    Log("\n[%d] target 0x%llX  value: int %d\n", t, (unsigned long long)targets[t], okv ? iv : -1);
                    if (B[t].obj) {
                        GetObjectName(B[t].obj, nb, 256);
                        void* c = GetObjClass(B[t].obj); cb[0]=0; if (c) GetObjectName(c, cb, 256);
                        Log("    owner ptr in \"%ls\" class=\"%ls\" base=0x%llX +0x%X => target = *(base+0x%X) + 0x%llX  (%d candidate ptrs)\n",
                            nb, cb, (unsigned long long)(uintptr_t)B[t].obj, B[t].off, B[t].off, (unsigned long long)B[t].delta, B[t].hits);
                    } else {
                        Log("    no UObject holds a pointer within 0x%llX below this address.\n", (unsigned long long)WINDOW);
                    }
                }
            }
            Log("\n=== F1 DONE ===\n\n");
        }

        // F2: address-resolver. Parses <Address>HEX</Address> entries from the saved
        // Cheat Engine .CT, then walks GUObjectArray to find which UObject body contains
        // each absolute address (and at what offset). CE addresses change every launch,
        // so we read them from the file at press-time rather than hardcoding.
        // Workflow: scan in CE -> Save table (Ctrl+S) -> press F2.
        if (IsKeyJustPressed(VK_F2)) {
            SafeRead32((uintptr_t)gObjArrayBase + 0x14, &gNumElements);
            SafeRead32((uintptr_t)gObjArrayBase + 0x1C, &gNumChunks);
            Log("\n=== F2: ADDRESS RESOLVER (%d objects) ===\n", gNumElements);

            const char* ctPath = "C:\\Users\\jonathan\\Desktop\\Projects\\Deadzone\\DeadzoneSteam-Win64-Shipping.CT";
            uintptr_t targets[64]; int nT = 0;
            FILE* cf = nullptr; fopen_s(&cf, ctPath, "rb");
            if (!cf) {
                Log("F2: cannot open %hs\n", ctPath);
            } else {
                static char fbuf[65536];
                size_t rd = fread(fbuf, 1, sizeof(fbuf)-1, cf); fclose(cf); fbuf[rd] = 0;
                const char* p = fbuf;
                while (nT < 64) {
                    const char* a = strstr(p, "<Address>");
                    if (!a) break;
                    a += 9;
                    uintptr_t v = 0; bool any = false;
                    while (*a) {
                        char ch = *a; int d;
                        if (ch>='0'&&ch<='9') d = ch-'0';
                        else if (ch>='a'&&ch<='f') d = ch-'a'+10;
                        else if (ch>='A'&&ch<='F') d = ch-'A'+10;
                        else break;
                        v = v*16 + d; any = true; a++;
                    }
                    if (any) targets[nT++] = v;
                    p = a;
                }
                Log("F2: parsed %d target addresses from .CT\n", nT);
            }

            if (nT > 0) {
                struct { void* hitObj; int32_t hitOff; void* nearObj; uintptr_t nearGap; int32_t nearSize; } R[64] = {};
                for (int t = 0; t < nT; t++) R[t].nearGap = (uintptr_t)-1;

                for (int i = 0; i < gNumElements; i++) {
                    void* o = GetUObject(i); if (!o) continue;
                    void* c = GetObjClass(o); if (!c) continue;
                    int32_t sz = 0; SafeRead32((uintptr_t)c + 0x58, &sz);
                    if (sz <= 0 || sz > 0x200000) continue;
                    uintptr_t base = (uintptr_t)o, end = base + (uintptr_t)sz;
                    for (int t = 0; t < nT; t++) {
                        uintptr_t a = targets[t];
                        if (a >= base && a < end) { R[t].hitObj = o; R[t].hitOff = (int32_t)(a - base); }
                        else if (a >= base && (a - base) < R[t].nearGap) { R[t].nearGap = a - base; R[t].nearObj = o; R[t].nearSize = sz; }
                    }
                }

                wchar_t nb[256], cb[256];
                for (int t = 0; t < nT; t++) {
                    int32_t iv = 0; bool okv = SafeRead32(targets[t], &iv); float fv = *(float*)&iv;
                    if (okv) Log("\n[%d] target 0x%llX  value: int %d / float %.3f\n", t, (unsigned long long)targets[t], iv, fv);
                    else     Log("\n[%d] target 0x%llX  (value unreadable)\n", t, (unsigned long long)targets[t]);
                    if (R[t].hitObj) {
                        GetObjectName(R[t].hitObj, nb, 256);
                        void* c = GetObjClass(R[t].hitObj); cb[0]=0; if (c) GetObjectName(c, cb, 256);
                        Log("    INSIDE object \"%ls\" class=\"%ls\" base=0x%llX +0x%X\n",
                            nb, cb, (unsigned long long)(uintptr_t)R[t].hitObj, R[t].hitOff);
                    } else if (R[t].nearObj) {
                        GetObjectName(R[t].nearObj, nb, 256);
                        void* c = GetObjClass(R[t].nearObj); cb[0]=0; if (c) GetObjectName(c, cb, 256);
                        Log("    NOT in any object body (heap/TArray/TMap likely).\n");
                        Log("    nearest object below: \"%ls\" class=\"%ls\" base=0x%llX size=0x%X => target is base+0x%llX (past end)\n",
                            nb, cb, (unsigned long long)(uintptr_t)R[t].nearObj, R[t].nearSize, (unsigned long long)R[t].nearGap);
                    } else {
                        Log("    NOT in any object body and no object below it.\n");
                    }
                }
            }
            Log("\n=== F2 DONE ===\n\n");
        }

        // F11: harvest the SanctityToken FPrimaryAssetId from a live currency widget.
        // Press in ZONE 4 (where the SanctityToken widget exists). Also dumps the
        // WBP_Game_Currency_C CurrencyId property offsets + AddCurrencyWidget params.
        if (IsKeyJustPressed(VK_F11)) {
            SafeRead32((uintptr_t)gObjArrayBase + 0x14, &gNumElements);
            SafeRead32((uintptr_t)gObjArrayBase + 0x1C, &gNumChunks);
            Log("\n=== F11: CURRENCY ID HARVEST (%d objects) ===\n", gNumElements);

            void* currencyClass = nullptr;   // WBP_Game_Currency_C (per-currency) class
            void* addCurrFunc = nullptr;     // AddCurrencyWidget on WBP_Game_Currencies_C
            wchar_t nb[256], cb[256], ob[256];

            for (int i = 0; i < gNumElements; i++) {
                void* o = GetUObject(i); if (!o) continue;
                if (!GetObjectName(o, nb, 256)) continue;
                if (!currencyClass && wcscmp(nb, L"WBP_Game_Currency_C") == 0) {
                    void* c = GetObjClass(o);
                    // UMG widget classes are WidgetBlueprintGeneratedClass — match by substring.
                    if (c && GetObjectName(c, cb, 256) &&
                        (wcsstr(cb, L"BlueprintGeneratedClass") || wcscmp(cb, L"Class")==0))
                        currencyClass = o;
                }
                if (!addCurrFunc && wcscmp(nb, L"AddCurrencyWidget") == 0) {
                    void* ou = GetObjOuter(o);
                    if (ou && GetObjectName(ou, ob, 256) && wcsstr(ob, L"WBP_Game_Currencies_C"))
                        addCurrFunc = o;
                }
            }

            // (A) Walk class properties; collect candidate FPrimaryAssetId offsets (16B structs).
            int candOff[16] = {}; wchar_t candName[16][64] = {}; int candCount = 0;
            if (currencyClass) {
                Log("WBP_Game_Currency_C class 0x%llX — currency properties:\n", (uintptr_t)currencyClass);
                uintptr_t field = 0; SafeRead64((uintptr_t)currencyClass + 0x50, &field);
                int guard = 0;
                while (field && IsSafeToRead((void*)field, 0x50) && guard++ < 256) {
                    wchar_t pname[256], ptype[256];
                    GetFFieldName((void*)field, pname, 256);
                    GetFFieldClassName((void*)field, ptype, 256);
                    int32_t off = 0, elemSize = 0;
                    SafeRead32(field + 0x4C, &off);
                    SafeRead32(field + 0x3C, &elemSize);
                    if ((wcsstr(pname, L"Currency") || wcsstr(pname, L"AssetId")) &&
                        wcsstr(ptype, L"Struct") && elemSize >= 12 && elemSize <= 32) {
                        Log("  prop \"%ls\" type=%ls off=0x%X size=%d\n", pname, ptype, off, elemSize);
                        if (candCount < 16) {
                            candOff[candCount] = off;
                            wcsncpy_s(candName[candCount], pname, 63);
                            candCount++;
                        }
                    }
                    SafeRead64(field + 0x20, &field);
                }
            } else Log("WBP_Game_Currency_C class NOT FOUND\n");

            // (B) AddCurrencyWidget param layout.
            if (addCurrFunc) { Log("AddCurrencyWidget params:\n"); DumpStructProperties(addCurrFunc, L"AddCurrencyWidget"); }
            else Log("AddCurrencyWidget func NOT FOUND\n");

            // (C) Brute-force scan each live currency widget body for an FName that
            // resolves to a currency name. FPrimaryAssetId = {type FName @X-8}{name FName @X}.
            int instCount = 0;
            for (int i = 0; i < gNumElements; i++) {
                void* o = GetUObject(i); if (!o || IsCDO(o)) continue;
                void* c = GetObjClass(o);
                if (!c || !GetObjectName(c, cb, 256) || wcscmp(cb, L"WBP_Game_Currency_C") != 0) continue;
                instCount++;
                GetObjectName(o, nb, 256);

                int32_t instSize = 0; SafeRead32((uintptr_t)c + 0x58, &instSize);
                if (instSize < 0x40 || instSize > 0x4000) instSize = 0x800;
                uintptr_t base = (uintptr_t)o;
                bool printedHdr = false;
                for (int off = 0x10; off + 8 <= instSize; off += 4) {
                    FName fn;
                    if (!SafeRead32(base + off + 0, &fn.ComparisonIndex)) continue;
                    if (!SafeRead32(base + off + 4, &fn.Number)) continue;
                    if (fn.ComparisonIndex <= 0 || fn.ComparisonIndex > 8000000) continue;
                    wchar_t s[256] = L"";
                    if (!GetFNameStr(&fn, s, 256) || !s[0]) continue;
                    if (!(ContainsCI(s, L"sanctity") || ContainsCI(s, L"scrap") ||
                          ContainsCI(s, L"currency") || ContainsCI(s, L"token") ||
                          ContainsCI(s, L"tech"))) continue;

                    // Treat this FName as the PrimaryAssetName; type FName is 8 bytes before.
                    uintptr_t idAddr = base + off - 8;
                    wchar_t ts[256] = L"";
                    if (off >= 8 + 0x10 && IsSafeToRead((void*)idAddr, 8)) {
                        FName tfn; SafeRead32(idAddr + 0, &tfn.ComparisonIndex); SafeRead32(idAddr + 4, &tfn.Number);
                        GetFNameStr(&tfn, ts, 256);
                    }
                    if (!printedHdr) { Log("instance[%d] \"%ls\" 0x%llX size=0x%X\n", instCount, nb, base, instSize); printedHdr = true; }
                    Log("    @0x%X name=\"%ls\"  (type@-8=\"%ls\")\n", off, s, ts);

                    if (ContainsCI(s, L"sanctity") && !gSanctityCurrencyIdValid && IsSafeToRead((void*)idAddr, 16)) {
                        memcpy(gSanctityCurrencyId, (void*)idAddr, 16);
                        gSanctityCurrencyIdValid = true;
                        Log("    *** CACHED SanctityToken CurrencyId @0x%X: type=\"%ls\" name=\"%ls\" "
                            "raw=%08X %08X %08X %08X ***\n", off - 8, ts, s,
                            *(uint32_t*)(idAddr+0), *(uint32_t*)(idAddr+4),
                            *(uint32_t*)(idAddr+8), *(uint32_t*)(idAddr+12));
                    }
                }
            }
            Log("Live WBP_Game_Currency_C instances: %d  | cached=%s\n",
                instCount, gSanctityCurrencyIdValid ? "YES" : "NO");
            Log("=== F11 DONE ===\n\n");
        }

        // F12: in the CURRENT zone, call AddCurrencyWidget(SanctityToken) on the live
        // WBP_Game_Currencies_C container so the game draws + auto-binds the vial widget.
        // Self-sufficient: rebuilds the CurrencyId by name — no zone-4 visit / F11 needed.
        if (IsKeyJustPressed(VK_F12)) {
            Log("\n=== F12: ADD CURRENCY WIDGET (manual) ===\n");
            DoAddVials();
        }
    }
    return 0;
}

void Mod::Start() { gRunning = true; gModThread = CreateThread(nullptr, 0, ModThreadProc, nullptr, 0, nullptr); }
void Mod::Stop() {
    gRunning = false;
    TickHook::Uninstall();
    if (gModThread) { WaitForSingleObject(gModThread, 5000); CloseHandle(gModThread); }
    if (gLogFile) { fclose(gLogFile); gLogFile = nullptr; }
}

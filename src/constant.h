// Event-driven locks. Included by payload.cpp after feature loading.
// Native addresses and field offsets are resolved from live IL2CPP metadata.
static std::atomic<unsigned> g_constantMask{0};
static std::atomic<bool> g_bpTutorialPaused{false};
static std::atomic<unsigned> g_pendingNorendeAchievements{0};
static std::atomic<unsigned> g_hookCalls{0};
static bool g_hooksReady = false, g_hooksInitialized = false;
static bool g_achievementHookReady = false;
static std::atomic<bool> g_achievementUpdateSeen{false};
// The game treats this as a bonus in its steal probability calculation. A
// large bounded bonus clears the different base rates used by steal actions.
static int g_bpTarget = 3, g_stealTarget = 10000;
static bool g_uniqueItemHookReady = false;
static void* g_battleClass;
static void* g_isTeamPlayerInfo;
static void* g_btlFunctionClass; static void* g_btlSequenceCtrlClass;
static void* g_btlGetInstanceInfo; static void* g_btlGetSequenceInfo; static void* g_isBDTutorialInfo;
static void* g_achievementUpdateInfo; static void* g_gameDataGetterInfo;
struct HookCall { HookCall() { ++g_hookCalls; } ~HookCall() { --g_hookCalls; } };
using IntGetter = int(*)(void*, void*);
using BoolGetter = bool(*)(void*, void*);
using IntSetter = void(*)(void*, int, void*);
using MaxGetter = int(*)(void*, bool, void*);
using BpSetter = int(*)(void*, int, void*);
using UpdateFn = void(*)(void*, float, void*);
using AchievementUpdateFn = void(*)(void*, void*);
using DamageFn = void(*)(void*, void*, int, bool, bool, bool, void*);
using CostFn = void(*)(void*, void*, void*);
using CostCheck = bool(*)(void*, void*, void*);
using PartyAddItemFn = void(*)(void*, int, int, void*);
static IntGetter origHP, origMP, origBP, origSteal;
static BoolGetter isTeamPlayer;
static IntSetter origSetHP, origSetMP;
static MaxGetter maxHP, maxMP;
static BpSetter origSetBP;
static UpdateFn origUpdate;
static AchievementUpdateFn origAchievementUpdate;
static DamageFn origDamage;
static CostFn origCost;
static CostCheck origCheckCost;
static PartyAddItemFn origPartyAddItem;
static void* maxHPInfo; static void* maxMPInfo; static void* bpInfo; static void* setBpInfo;
static void* damageCloneInfo; static void* costCopyInfo; static void* costClass;
static int parameterOffset, commandCtrlOffset, commandBpOffset, hpOffset, mpOffset, hpMaxOffset, mpMaxOffset;
static int damageOffset, mpDamageOffset, bpDamageOffset, costHpOffset, costMpOffset, costBpOffset;
static void HookAchievementUpdate(void* obj, void* method) {
    HookCall call;
    origAchievementUpdate(obj, method);
    if (!g_achievementUpdateSeen.exchange(true)) Log("Norende achievement main-thread callback active");
    unsigned pending = g_pendingNorendeAchievements.exchange(0);
    if (!pending) return;
    Log("Processing queued Norende achievements (mask=%u)", pending);
    void* exception = nullptr;
    void* gameData = il.runtime_invoke(g_gameDataGetterInfo, nullptr, nullptr, &exception);
    void* flags = nullptr;
    if (!exception && gameData) {
        void* getter = FindMethodTyped(il.object_get_class(gameData), "get_m_NewAchievementFlag", {});
        if (getter) flags = il.runtime_invoke(getter, gameData, nullptr, &exception);
    }
    void* setter = (!exception && flags) ? FindMethodTyped(il.object_get_class(flags), "SetAchieve", {T_I4, T_BOOL}) : nullptr;
    if (exception || !setter) {
        Log("Norende achievement update failed: game achievement data unavailable");
        return;
    }
    const int ids[] = {34, 35}; // A Familiar View, Welcome to Norende
    uint8_t noDelay = 0;
    for (unsigned bit = 1, i = 0; i < 2; ++i, bit <<= 1) {
        if (!(pending & bit)) continue;
        int id = ids[i]; void* args[] = {&id, &noDelay}; exception = nullptr;
        il.runtime_invoke(setter, flags, args, &exception);
        if (exception) Log("Norende achievement %d could not be registered", id);
        else Log("Norende achievement %d registered", id);
    }
}
static unsigned EffectiveConstantMask() {
    unsigned mask = g_constantMask.load();
    if (g_bpTutorialPaused.load()) mask &= ~4u;
    return mask;
}
// Temporarily suppress only the BP bit while the game's Brave/Default tutorial is active.
static int UpdateBpTutorialGuard(DWORD now) {
    static DWORD lastCheck = 0;
    if (now - lastCheck < 100) return 0;
    lastCheck = now;
    if (!(g_constantMask.load() & 4u)) {
        g_bpTutorialPaused = false;
        return 0;
    }
    if (!g_btlGetInstanceInfo || !g_btlGetSequenceInfo || !g_isBDTutorialInfo) return 0;
    void* exception = nullptr;
    void* battle = il.runtime_invoke(g_btlGetInstanceInfo, nullptr, nullptr, &exception);
    if (exception) return 0;
    bool inTutorial = false;
    if (battle) {
        void* sequence = il.runtime_invoke(g_btlGetSequenceInfo, battle, nullptr, &exception);
        if (exception) return 0;
        if (sequence) {
            void* boxed = il.runtime_invoke(g_isBDTutorialInfo, sequence, nullptr, &exception);
            if (exception || !boxed) return 0;
            void* value = il.object_unbox(boxed);
            if (!value) return 0;
            inTutorial = *(uint8_t*)value != 0;
        }
    }
    bool wasPaused = g_bpTutorialPaused.exchange(inTutorial);
    if (wasPaused == inTutorial) return 0;
    Log("Infinite BP %s during Brave/Default tutorial", inTutorial ? "paused" : "resumed");
    return inTutorial ? 1 : -1;
}
static int& NativeInt(void* obj, int offset) { return *(int*)((char*)obj + offset); }
static bool PlayerBattleObject(void* obj) {
    return obj && isTeamPlayer && il.class_is_assignable_from(g_battleClass, il.object_get_class(obj)) && isTeamPlayer(obj, g_isTeamPlayerInfo);
}
static void LockBattleResources(void* obj, unsigned mask) {
    if (!(mask & 7) || !PlayerBattleObject(obj)) return;
    void* parameter = *(void**)((char*)obj + parameterOffset);
    if (parameter) {
        if ((mask & 1) && NativeInt(parameter, hpMaxOffset) > 0) NativeInt(parameter, hpOffset) = NativeInt(parameter, hpMaxOffset);
    if ((mask & 2) && NativeInt(parameter, mpMaxOffset) >= 0) NativeInt(parameter, mpOffset) = NativeInt(parameter, mpMaxOffset);
    }
    if (mask & 4) {
        // BP is stored by BtlHelper.CommandCtrl rather than in BtlChara itself.
        // Write the game's backing value directly so Brave's rapid queued-cost
        // path cannot leave the player at negative BP between frames.
        void* command = *(void**)((char*)obj + commandCtrlOffset);
        if (command) NativeInt(command, commandBpOffset) = g_bpTarget;
        if (origBP(obj, bpInfo) != g_bpTarget) origSetBP(obj, g_bpTarget, setBpInfo);
    }
}
static int HookHP(void* obj, void* method) { HookCall call; return (EffectiveConstantMask() & 1) ? maxHP(obj, false, maxHPInfo) : origHP(obj, method); }
static int HookMP(void* obj, void* method) { HookCall call; return (EffectiveConstantMask() & 2) ? maxMP(obj, false, maxMPInfo) : origMP(obj, method); }
static void HookSetHP(void* obj, int value, void* method) { HookCall call; origSetHP(obj, (EffectiveConstantMask() & 1) ? maxHP(obj, false, maxHPInfo) : value, method); }
static void HookSetMP(void* obj, int value, void* method) { HookCall call; origSetMP(obj, (EffectiveConstantMask() & 2) ? maxMP(obj, false, maxMPInfo) : value, method); }
static int HookBP(void* obj, void* method) { HookCall call; return (EffectiveConstantMask() & 4) && PlayerBattleObject(obj) ? g_bpTarget : origBP(obj, method); }
static int HookSetBP(void* obj, int value, void* method) { HookCall call; return origSetBP(obj, (EffectiveConstantMask() & 4) && PlayerBattleObject(obj) ? g_bpTarget : value, method); }
static int HookSteal(void* obj, void* method) { HookCall call; return (EffectiveConstantMask() & 8) && PlayerBattleObject(obj) ? g_stealTarget : origSteal(obj, method); }
static void HookUpdate(void* obj, float delta, void* method) {
    HookCall call; unsigned mask = EffectiveConstantMask();
    LockBattleResources(obj, mask); origUpdate(obj, delta, method); LockBattleResources(obj, mask);
}
static void HookDamage(void* obj, void* damage, int hits, bool notDead, bool noAnim, bool delay, void* method) {
    HookCall call; unsigned mask = EffectiveConstantMask();
    if ((mask & 7) && damage && PlayerBattleObject(obj)) {
        void* exc = nullptr;
        void* copy = il.runtime_invoke(damageCloneInfo, damage, nullptr, &exc);
        if (!exc && copy) {
            // Damage objects can be shared by several targets; protect only a copy.
            if (mask & 1) { NativeInt(copy, damageOffset) = 0; notDead = true; }
            if (mask & 2) NativeInt(copy, mpDamageOffset) = 0;
            if (mask & 4) NativeInt(copy, bpDamageOffset) = 0;
            damage = copy;
        }
    }
    LockBattleResources(obj, mask);
    origDamage(obj, damage, hits, notDead, noAnim, delay, method);
    LockBattleResources(obj, mask);
}
static void* ProtectedCost(void* obj, void* cost, unsigned mask) {
    if (!(mask & 7) || !cost || !PlayerBattleObject(obj)) return cost;
    if (!((mask & 1) && NativeInt(cost, costHpOffset)) &&
        !((mask & 2) && NativeInt(cost, costMpOffset)) &&
        !((mask & 4) && NativeInt(cost, costBpOffset))) return cost;
    void* copy = il.object_new(costClass);
    if (!copy) return cost;
    void* exc = nullptr; void* args[] = {cost};
    il.runtime_invoke(costCopyInfo, copy, args, &exc);
    if (exc) return cost;
    if (mask & 1) NativeInt(copy, costHpOffset) = 0;
    if (mask & 2) NativeInt(copy, costMpOffset) = 0;
    if (mask & 4) NativeInt(copy, costBpOffset) = 0;
    return copy;
}
static void HookCost(void* obj, void* cost, void* method) {
    HookCall call; unsigned mask = EffectiveConstantMask();
    origCost(obj, ProtectedCost(obj, cost, mask), method); LockBattleResources(obj, mask);
}
static bool HookCheckCost(void* obj, void* cost, void* method) {
    HookCall call; unsigned mask = EffectiveConstantMask();
    LockBattleResources(obj, mask);
    return origCheckCost(obj, ProtectedCost(obj, cost, mask), method);
}
static void HookPartyAddItem(void* party, int id, int quantity, void* method) {
    HookCall call;
    const int type = ItemTypeById(id);
    if (IsProtectedUniqueItem(id, type)) {
        bool ownershipCheckAvailable = false;
        if (PartyAlreadyHasUniqueItem(party, id, ownershipCheckAvailable) && ownershipCheckAvailable) {
            Log("duplicate unique item blocked: id=%d type=%d", id, type);
            return;
        }
    }
    origPartyAddItem(party, id, quantity, method);
}
static bool InitConstantHooks() {
    g_battleClass = FindClassN(g_domain, "", "BtlChara");
    g_isTeamPlayerInfo = g_battleClass ? FindMethodTyped(g_battleClass, "IsTeamPlayer", {}) : nullptr;
    isTeamPlayer = g_isTeamPlayerInfo ? (BoolGetter)*(void**)g_isTeamPlayerInfo : nullptr;
    g_btlFunctionClass = FindClassN(g_domain, "", "BtlFunction");
    g_btlSequenceCtrlClass = FindClassN(g_domain, "", "BtlSequenceCtrl");
    g_btlGetInstanceInfo = g_btlFunctionClass ? FindMethodTyped(g_btlFunctionClass, "GetInstance", {}) : nullptr;
    g_btlGetSequenceInfo = g_btlFunctionClass ? FindMethodTyped(g_btlFunctionClass, "GetBtlSequenceCtrl", {}) : nullptr;
    g_isBDTutorialInfo = g_btlSequenceCtrlClass ? FindMethodTyped(g_btlSequenceCtrlClass, "IsBDTutorial", {}) : nullptr;
    if (!g_btlGetInstanceInfo || !g_btlGetSequenceInfo || !g_isBDTutorialInfo)
        Log("constant lock: Brave/Default tutorial detector methods unavailable");
    void* achievementClass = FindClassN(g_domain, "", "ApplicationSystem");
    void* hikariClass = FindClassN(g_domain, "", "Hikari");
    g_achievementUpdateInfo = achievementClass ? FindMethodTyped(achievementClass, "Update", {}) : nullptr;
    g_gameDataGetterInfo = hikariClass ? FindMethodTyped(hikariClass, "get_GameData", {}) : nullptr;
    void* character = FindClassN(g_domain, "", "CharacterState");
    void* parameter = FindClassN(g_domain, "", "BtlCharaParameter");
    void* damage = FindClassN(g_domain, "", "BtlDamageData");
    costClass = FindClassN(g_domain, "", "BtlAbilityCost");
    bool valid = true;
    auto fieldOffset = [&](void* klass, const char* name) {
        void* field = klass ? il.class_get_field_from_name(klass, name) : nullptr;
        int offset = field ? il.field_get_offset(field) : -1;
        if (offset < 16) { valid = false; Log("constant lock: field missing %s", name); }
        return offset;
    };
    if (!isTeamPlayer) { valid = false; Log("constant lock: BtlChara.IsTeamPlayer unavailable"); }
    parameterOffset = fieldOffset(g_battleClass, "m_parameter");
    commandCtrlOffset = fieldOffset(g_battleClass, "m_commandCtrl");
    void* commandCtrlClass = FindClassN(g_domain, "", "BtlHelper.CommandCtrl");
    commandBpOffset = fieldOffset(commandCtrlClass, "m_bp");
    hpOffset = fieldOffset(parameter, "hp"); mpOffset = fieldOffset(parameter, "mp");
    hpMaxOffset = fieldOffset(parameter, "hpMax"); mpMaxOffset = fieldOffset(parameter, "mpMax");
    damageOffset = fieldOffset(damage, "damage"); mpDamageOffset = fieldOffset(damage, "mpDamage"); bpDamageOffset = fieldOffset(damage, "bpDamage");
    costHpOffset = fieldOffset(costClass, "hp"); costMpOffset = fieldOffset(costClass, "mp"); costBpOffset = fieldOffset(costClass, "bp");
    maxHPInfo = FindMethodTyped(character, "GetMHP", {T_BOOL}); maxMPInfo = FindMethodTyped(character, "GetMMP", {T_BOOL});
    bpInfo = FindMethodTyped(g_battleClass, "GetBP", {});
    setBpInfo = FindMethodTyped(g_battleClass, "SetBP", {T_I4});
    damageCloneInfo = FindMethodTyped(damage, "Clone", {}); costCopyInfo = FindMethodTyped(costClass, "Copy", {T_CLASS});
    if (!valid || !maxHPInfo || !maxMPInfo || !bpInfo || !setBpInfo || !damageCloneInfo || !costCopyInfo) return false;
    // IL2CPP MethodInfo begins with its native entry point; no game RVA is stored.
    maxHP = (MaxGetter)*(void**)maxHPInfo; maxMP = (MaxGetter)*(void**)maxMPInfo;
    if (MH_Initialize() != MH_OK) return false;
    g_hooksInitialized = true;
    auto hook = [&](void* klass, const char* name, std::vector<int> types, void* detour, void** original) {
        void* info = FindMethodTyped(klass, name, types);
        MH_STATUS status = info ? MH_CreateHook(*(void**)info, detour, original) : MH_ERROR_NOT_EXECUTABLE;
        if (status != MH_OK) { Log("constant hook %s: %s", name, MH_StatusToString(status)); valid = false; }
    };
    hook(character, "GetHP", {}, (void*)HookHP, (void**)&origHP);
    hook(character, "GetMP", {}, (void*)HookMP, (void**)&origMP);
    hook(character, "SetHP", {T_I4}, (void*)HookSetHP, (void**)&origSetHP);
    hook(character, "SetMP", {T_I4}, (void*)HookSetMP, (void**)&origSetMP);
    hook(g_battleClass, "GetBP", {}, (void*)HookBP, (void**)&origBP);
    hook(g_battleClass, "SetBP", {T_I4}, (void*)HookSetBP, (void**)&origSetBP);
    hook(g_battleClass, "GetAddStealRate", {}, (void*)HookSteal, (void**)&origSteal);
    hook(g_battleClass, "Update", {T_R4}, (void*)HookUpdate, (void**)&origUpdate);
    hook(g_battleClass, "StartDamage", {T_CLASS,T_I4,T_BOOL,T_BOOL,T_BOOL}, (void*)HookDamage, (void**)&origDamage);
    hook(g_battleClass, "UseCost", {T_CLASS}, (void*)HookCost, (void**)&origCost);
    hook(g_battleClass, "IsUseAbilityCost", {T_CLASS}, (void*)HookCheckCost, (void**)&origCheckCost);
    // PartyState.AddItem is the shared inventory insertion path used by the
    // trainer and game rewards. Intercepting it also catches natural boss,
    // chest, and shop acquisitions after an item has been spawned.
    void* partyClass = FindClassN(g_domain, "", "PartyState");
    void* addItemInfo = partyClass ? FindMethodTyped(partyClass, "AddItem", {T_I4, T_I4}) : nullptr;
    const bool hasOwnershipLookup = partyClass &&
        (FindMethodTyped(partyClass, "GetItemCount", {T_I4}) || FindMethodTyped(partyClass, "GetItem", {T_I4}));
    if (addItemInfo && hasOwnershipLookup) {
        MH_STATUS uniqueStatus = MH_CreateHook(*(void**)addItemInfo, (void*)HookPartyAddItem, (void**)&origPartyAddItem);
        g_uniqueItemHookReady = uniqueStatus == MH_OK;
        Log("unique inventory acquisition guard: %s", g_uniqueItemHookReady ? "installed" : MH_StatusToString(uniqueStatus));
    } else Log("unique inventory acquisition guard unavailable: PartyState.AddItem or ownership lookup missing");
    if (g_achievementUpdateInfo && g_gameDataGetterInfo) {
        MH_STATUS achievementStatus = MH_CreateHook(*(void**)g_achievementUpdateInfo, (void*)HookAchievementUpdate, (void**)&origAchievementUpdate);
        g_achievementHookReady = achievementStatus == MH_OK;
        Log("Norende achievement callback hook: %s", g_achievementHookReady ? "installed" : MH_StatusToString(achievementStatus));
        if (!g_achievementHookReady) Log("Norende achievement update hook unavailable: %s", MH_StatusToString(achievementStatus));
    } else Log("Norende achievement update hook unavailable: game methods not found");
    if (!valid || MH_EnableHook(MH_ALL_HOOKS) != MH_OK) { MH_Uninitialize(); g_hooksInitialized = false; return false; }
    Log("constant battle hooks installed");
    return true;
}
static unsigned ConstantBit(const Feature& f) {
    if (f.constant == L"hp") return 1;
    if (f.constant == L"mp") return 2;
    if (f.constant == L"bp") return 4;
    if (f.constant == L"steal") return 8;
    return 0;
}
static bool IsToggle(const Feature& f) { return f.repeatMs > 0 || !f.constant.empty(); }
static bool QueueNorendeLevelAchievements(int level) {
    if (level < 1) return true;
    if (!g_achievementHookReady) return false;
    unsigned mask = 1u;
    if (level >= 11) mask |= 2u;
    g_pendingNorendeAchievements.fetch_or(mask);
    Log("Queued Norende achievement mask %u", mask);
    return true;
}
static void ApplyConstantState(const Feature& f) {
    unsigned bit = ConstantBit(f);
    if (f.active) g_constantMask.fetch_or(bit); else g_constantMask.fetch_and(~bit);
}
static void ConfigureConstantFeatures() {
    g_hooksReady = InitConstantHooks();
    for (auto& f : g_features) {
        if (f.constant.empty()) continue;
        if (!g_hooksReady || !ConstantBit(f)) { f.ok = false; f.active = false; f.status = L"constant hooks could not be installed; see payload.log"; continue; }
        if (f.constant == L"bp" && (!g_btlGetInstanceInfo || !g_btlGetSequenceInfo || !g_isBDTutorialInfo)) {
            f.ok = false; f.active = false; f.status = L"Brave/Default tutorial detector unavailable; see payload.log"; continue;
        }
        if (!f.actions.empty() && !f.actions[0].args.empty()) {
            if (f.constant == L"bp") g_bpTarget = std::clamp((int)f.actions[0].args[0].num, -4, 3);
            if (f.constant == L"steal") g_stealTarget = (int)f.actions[0].args[0].num;
        }
        ApplyConstantState(f);
    }
}
static bool StopConstantHooks() {
    g_constantMask = 0;
    g_bpTutorialPaused = false;
    g_pendingNorendeAchievements = 0;
    if (!g_hooksInitialized) return true;
    if (MH_DisableHook(MH_ALL_HOOKS) != MH_OK) { Log("Could not disable hooks; retaining DLL for safety"); return false; }
    // Let callbacks already executing on game threads return before freeing code.
    Sleep(100);
    while (g_hookCalls.load()) Sleep(1);
    MH_Uninitialize(); g_hooksInitialized = false;
    return true;
}

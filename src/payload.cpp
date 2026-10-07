// bdffhd_payload.dll - runs inside the game. No hard-coded offsets: it resolves game classes/methods *by name* through
// the IL2CPP API that GameAssembly.dll exports, driven by features.ini. A feature is enabled only if everything it needs
// resolves in the live game, so patches that rename things disable that feature instead of crashing the game.
#include "common.h"
#include <commctrl.h>
#include <cstdarg>
#include <cstdint>
#include <algorithm>
#include <cwctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <bcrypt.h>
#include <atomic>
#include <MinHook.h>

static HMODULE g_self; static std::wstring g_dir; static FILE* g_log; static volatile bool g_unload; static void* g_domain;

static void Log(const char* fmt, ...) {
    if (!g_log) return;
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a); fputc('\n', g_log); fflush(g_log);
}

// ---------------- IL2CPP API (resolved from GameAssembly.dll exports) ----------------
static struct {
    void* (*domain_get)();
    void* (*thread_attach)(void*);
    void (*thread_detach)(void*);
    void** (*domain_get_assemblies)(void*, size_t*);
    void* (*assembly_get_image)(void*);
    void* (*class_from_name)(void*, const char*, const char*);
    void* (*runtime_invoke)(void*, void*, void**, void**);
    void* (*object_new)(void*);
    void* (*class_get_type)(void*);
    void* (*type_get_object)(void*);
    void* (*method_get_object)(void*, void*);
    void* (*string_new)(const char*);
    void* (*object_get_class)(void*);
    void* (*object_unbox)(void*);
    void* (*class_get_field_from_name)(void*, const char*);
    void  (*field_set_value)(void*, void*, void*);
    void  (*field_set_value_object)(void*, void*, void*);
    void  (*field_get_value)(void*, void*, void*);
    void* (*class_get_methods)(void*, void**);
    const char* (*method_get_name)(void*);
    uint32_t (*method_get_param_count)(void*);
    void* (*method_get_param)(void*, uint32_t);
    int   (*type_get_type)(void*);
    void* (*class_get_parent)(void*);
    bool (*class_is_assignable_from)(void*, void*);
    int32_t (*field_get_offset)(void*);
    const wchar_t* (*string_chars)(void*);
    int   (*string_length)(void*);
    void  (*field_static_get_value)(void*, void*);
    uintptr_t (*array_length)(void*);
} il;

static bool LoadApi(HMODULE ga) {
    bool ok = true;
#define LOAD(n) il.n = (decltype(il.n))GetProcAddress(ga, "il2cpp_" #n); if (!il.n) { Log("missing export il2cpp_" #n); ok = false; }
    LOAD(domain_get) LOAD(thread_attach) LOAD(thread_detach) LOAD(domain_get_assemblies) LOAD(assembly_get_image)
    LOAD(class_from_name) LOAD(runtime_invoke) LOAD(object_new) LOAD(class_get_type) LOAD(type_get_object) LOAD(method_get_object) LOAD(string_new) LOAD(object_get_class) LOAD(object_unbox)
    LOAD(class_get_field_from_name) LOAD(field_set_value) LOAD(field_set_value_object) LOAD(field_get_value) LOAD(class_get_methods) LOAD(method_get_name)
    LOAD(method_get_param_count) LOAD(method_get_param) LOAD(type_get_type) LOAD(class_get_parent) LOAD(string_chars) LOAD(string_length) LOAD(field_static_get_value) LOAD(array_length)
    LOAD(class_is_assignable_from) LOAD(field_get_offset)
#undef LOAD
    return ok;
}

static void* FindClass(void* domain, const std::string& ns, const std::string& name) {
    static std::map<std::string, void*> cache;
    const std::string key = std::to_string((uintptr_t)domain) + ":" + ns + ":" + name;
    auto cached = cache.find(key);
    if (cached != cache.end()) return cached->second;
    size_t n = 0; void** asms = il.domain_get_assemblies(domain, &n);
    for (size_t i = 0; asms && i < n; ++i) {
        void* img = il.assembly_get_image(asms[i]);
        if (void* c = il.class_from_name(img, ns.c_str(), name.c_str())) { cache[key] = c; return c; }
    }
    return nullptr;
}
// Nested classes are stored as "Outer/Inner"; accept the dump.cs style "Outer.Inner" too.
static void* FindClassN(void* domain, const std::string& ns, std::string name) {
    if (void* c = FindClass(domain, ns, name)) return c;
    for (auto& ch : name) if (ch == '.') ch = '/';
    return FindClass(domain, ns, name);
}

// ---------------- Features (from features.ini) ----------------
// A feature = root (static 0-arg method returning an object)  ->  chain of instance steps  ->  optional foreach
//             ->  final action (call a method, or set a field) on each resulting object.
enum { T_BOOL = 2, T_U1 = 5, T_I4 = 8, T_I8 = 10, T_R4 = 12, T_STRING = 14, T_CLASS = 18 };   // Il2CppTypeEnum values

struct Slot { char kind = 'i'; int32_t i = 0; int64_t l = 0; uint8_t b = 0; float f = 0; void* s = nullptr; };
struct Arg  { char kind = 'i'; int input = -1; int64_t num = 0; double fl = 0; std::string str; };
struct Step { std::string name; std::vector<int> inputs; };

static int  TypeOfKind(char k) { switch (k) { case 'l': return T_I8; case 'b': return T_BOOL; case 'y': return T_U1; case 'f': return T_R4; case 's': return T_STRING; case 'n': return T_CLASS; default: return T_I4; } }
static char KindOfType(const std::wstring& t) { if (t == L"long") return 'l'; if (t == L"bool") return 'b'; if (t == L"byte") return 'y'; if (t == L"float") return 'f'; if (t == L"string") return 's'; return 'i'; }
static void* PtrOf(Slot& s) { switch (s.kind) { case 'l': return &s.l; case 'b': case 'y': return &s.b; case 'f': return &s.f; case 's': return s.s; case 'n': return nullptr; default: return &s.i; } }

// Find a method by exact name + parameter types, searching the class and its parents (so overloads like
// GetFsFriend(int) vs GetFsFriend(SaveDataId) are told apart).
static void* FindMethodTyped(void* klass, const std::string& name, const std::vector<int>& types) {
    static std::map<std::string, void*> cache;
    std::string key = std::to_string((uintptr_t)klass) + ":" + name;
    for (int type : types) key += ":" + std::to_string(type);
    auto cached = cache.find(key);
    if (cached != cache.end()) return cached->second;

    for (void* k = klass; k; k = il.class_get_parent(k)) {
        void* iter = nullptr;
        while (void* m = il.class_get_methods(k, &iter)) {
            if (name != il.method_get_name(m)) continue;
            if (il.method_get_param_count(m) != types.size()) continue;
            bool match = true;
            for (size_t i = 0; i < types.size() && match; ++i)
                match = il.type_get_type(il.method_get_param(m, (uint32_t)i)) == types[i];
            if (match) { cache.emplace(std::move(key), m); return m; }
        }
    }
    cache.emplace(std::move(key), nullptr);
    return nullptr;
}

struct Action { std::string method, field, staticArr, reportInt; bool restore = false; std::vector<Arg> args; };
struct Level { std::string count, item; };
static std::map<std::string, std::vector<float>> g_origArr;   // original rate tables, so they can be restored
struct Feature {
    std::wstring id, category, label, notes, status;
    std::vector<std::wstring> argTypes, argLabels, argDefaults, argValues;
    std::vector<int> oneBasedLimits;   // argValues = what is currently typed for THIS entry
    std::string rootNs, rootClass, rootMethod; void* rootM = nullptr;
    std::vector<Step> chain;
    std::vector<Level> levels;              // nested foreach: party members -> jobs, etc.
    bool noRoot = false, addComNpcFriend = false, repairFriends = false, friendMax = false, friendMaxAll = false, setJobLevel = false, setOneJobLevel = false, cureAllStatus = false, setColonyLevels = false, unlockColony = false, persistUnavailable = false, waitingForState = false, usedSinceLaunch = false;
    void* fKlass = nullptr;   // noRoot = only static actions on fClass
    std::string fNs, fClass, filter; void* filterM = nullptr;
    std::vector<Action> actions;            // run in order on each target object
    int repeatMs = 0; bool active = false; DWORD lastTick = 0;   // repeat_ms > 0 makes the entry a toggle
    std::wstring constant;
    bool ok = false;
};
static std::vector<Feature> g_features;
static bool IsKnownItemId(int id);
static int ItemTypeById(int id);
static bool IsProtectedUniqueItem(int id, int itemType);
static bool PartyAlreadyHasUniqueItem(void* party, int id, bool& checkAvailable);

static Arg ParseArg(const std::wstring& t) {          // "#1" = UI input 1;  "i:999" "l:0" "b:1" "f:1.5" "s:text" "y:5" = literal
    Arg a;
    if (!t.empty() && t[0] == L'#') { a.input = _wtoi(t.c_str() + 1); return a; }
    if (t.size() > 2 && t[1] == L':') { a.kind = (char)t[0]; std::wstring v = t.substr(2); a.num = _wtoi64(v.c_str()); a.fl = _wtof(v.c_str()); a.str = Narrow(v); }
    return a;
}
static Step ParseStep(const std::wstring& t) {         // "GetParty"  or  "GetFsFriend(#0)"
    Step s; size_t p = t.find(L'(');
    s.name = Narrow(p == std::wstring::npos ? t : t.substr(0, p));
    if (p != std::wstring::npos) {
        size_t q = t.find(L')', p);
        for (auto& x : Split(t.substr(p + 1, q == std::wstring::npos ? std::wstring::npos : q - p - 1), L','))
            if (!x.empty() && x[0] == L'#') s.inputs.push_back(_wtoi(x.c_str() + 1));
    }
    return s;
}

static void LoadFeatures(void* domain) {
    for (auto& s : LoadIni(g_dir + L"\\features.ini")) {
        // These sections store trainer configuration, not callable game features.
        if (s.name == L"Overlay" || s.name == L"ItemDefaults" || s.name == L"Hotkeys") continue;
        Feature f; f.id = s.name; f.category = Get(s, L"category", L"Misc"); f.label = Get(s, L"label", s.name); f.notes = Get(s, L"notes");
        f.argTypes = Split(Get(s, L"arg_types"), L','); f.argLabels = Split(Get(s, L"arg_labels"), L','); f.argDefaults = Split(Get(s, L"arg_defaults"), L',');
        for (auto& limit : Split(Get(s, L"one_based_limits"), L',')) f.oneBasedLimits.push_back(_wtoi(limit.c_str()));
        f.argValues.resize(f.argTypes.size());
        for (size_t i = 0; i < f.argValues.size(); ++i) f.argValues[i] = i < f.argDefaults.size() ? f.argDefaults[i] : L"";
        f.rootNs = Narrow(Get(s, L"root_ns")); f.rootClass = Narrow(Get(s, L"root_class")); f.rootMethod = Narrow(Get(s, L"root_method"));
        for (auto& st : Split(Get(s, L"chain"), L'>')) if (!st.empty()) f.chain.push_back(ParseStep(st));
        for (const wchar_t* sfx : {L"", L"2"}) {
            Level lv; lv.count = Narrow(Get(s, std::wstring(L"foreach") + sfx + L"_count")); lv.item = Narrow(Get(s, std::wstring(L"foreach") + sfx + L"_item"));
            if (!lv.count.empty() && !lv.item.empty()) f.levels.push_back(lv);
        }
        f.fNs = Narrow(Get(s, L"ns")); f.fClass = Narrow(Get(s, L"class"));
        f.filter = Narrow(Get(s,L"foreach_filter"));
        f.repeatMs = _wtoi(Get(s, L"repeat_ms", L"0").c_str());
        f.constant = Get(s, L"constant");
        f.active = Get(s, L"enabled_by_default", L"0") == L"1";
        f.persistUnavailable = Get(s, L"persist_unavailable") == L"1";
        for (int n = 1; n <= 16; ++n) {                           // method/set_field/static_array + final_args, then ...2, ...3 ... up to 16 actions
            const std::wstring sfx = n == 1 ? std::wstring() : std::to_wstring(n);
            Action a; a.method = Narrow(Get(s, std::wstring(L"method") + sfx)); a.field = Narrow(Get(s, std::wstring(L"set_field") + sfx));
            a.staticArr = Narrow(Get(s, std::wstring(L"static_array") + sfx));
            a.reportInt = Narrow(Get(s, std::wstring(L"report_int") + sfx));
            std::string rst = Narrow(Get(s, std::wstring(L"static_array_restore") + sfx));
            if (!rst.empty()) { a.staticArr = rst; a.restore = true; }
            if (a.method.empty() && a.field.empty() && a.staticArr.empty() && a.reportInt.empty()) continue;
            for (auto& t : Split(Get(s, std::wstring(L"final_args") + sfx), L',')) if (!t.empty()) a.args.push_back(ParseArg(t));
            f.actions.push_back(a);
        }
        f.addComNpcFriend = Get(s, L"operation") == L"add_com_npc_friend";
        f.repairFriends = Get(s, L"operation") == L"repair_computer_friends";
        f.friendMax = Get(s, L"operation") == L"friend_max";
        f.friendMaxAll = Get(s, L"operation") == L"friend_max_all";
        f.setJobLevel = Get(s, L"operation") == L"set_job_level";
        f.setOneJobLevel = Get(s, L"operation") == L"set_one_job_level";
        f.setJobLevel = f.setJobLevel || f.setOneJobLevel;
        f.cureAllStatus = Get(s, L"operation") == L"cure_all_status";
        f.setColonyLevels = Get(s, L"operation") == L"set_colony_levels";
        f.unlockColony = Get(s, L"operation") == L"unlock_colony_all";
        f.noRoot = f.rootClass.empty();

        if (f.addComNpcFriend || f.repairFriends || f.friendMax || f.friendMaxAll) {
            void* gameDataClass = FindClassN(domain, "", "GameData");
            f.fKlass = FindClassN(domain, f.fNs, f.fClass);
            if (f.rootClass != "Hikari" || !(f.rootM = FindMethodTyped(FindClassN(domain, "", "Hikari"), f.rootMethod, {})))
                f.status = L"game data accessor not found";
            else if (!gameDataClass || !FindMethodTyped(gameDataClass, "GetFsFriendCOUNT", {}) || !FindMethodTyped(gameDataClass, "SetFsFriend", {T_CLASS}))
                f.status = L"GameData friend-list methods not found";
            else if (!f.fKlass || !il.object_new || !FindMethodTyped(f.fKlass, ".ctor", {}) ||
                     !FindMethodTyped(f.fKlass, "SetComNPC", {T_I4}) || !FindMethodTyped(f.fKlass, "SetFriend", {T_BOOL}) ||
                     !FindMethodTyped(f.fKlass, "SetFriendship", {T_U1}) || !FindMethodTyped(f.fKlass, "VerifyData", {}) ||
                     !FindMethodTyped(f.fKlass, "IsInvalid", {}))
                f.status = L"FriendState creation methods not found";
            else { f.ok = true; f.status.clear(); }
        }
        else if (f.setJobLevel) {
            void* root = FindClassN(domain, f.rootNs, f.rootClass);
            void* characterState = FindClassN(domain, "", "CharacterState");
            f.fKlass = FindClassN(domain, f.fNs, f.fClass);
            if (!root || !(f.rootM = FindMethodTyped(root, f.rootMethod, {}))) f.status = L"game data accessor not found";
            else if (!characterState || !FindMethodTyped(characterState, "GetJOB_COUNT", {}) || !FindMethodTyped(characterState, "GetJOB", {T_I4})) f.status = L"party job accessors not found";
            else if (!FindMethodTyped(FindClassN(domain, "", "JobState"), "SetLevel", {T_I4,T_I4,T_CLASS}) || !FindMethodTyped(FindClassN(domain, "", "JobParam"), "GetParam", {T_I4,T_I4})) f.status = L"job level methods not found";
            else { f.ok = true; f.status.clear(); }
        }
        else if (f.cureAllStatus) {
            void* root = FindClassN(domain, f.rootNs, f.rootClass);
            void* characterState = FindClassN(domain, "", "CharacterState");
            void* battleCharacter = FindClassN(domain, "", "BtlChara");
            void* status = FindClassN(domain, "", "BtlStatusManager");
            void* manager = FindClassN(domain, "", "BtlCharaManager");
            if (!root || !(f.rootM = FindMethodTyped(root, f.rootMethod, {}))) f.status = L"saved party accessor not found";
            else if (!characterState || !FindMethodTyped(characterState, "SetDEBUFF_POI", {T_BOOL}) ||
                     !FindMethodTyped(characterState, "SetDEBUFF_BLD", {T_BOOL}) || !FindMethodTyped(characterState, "SetDEBUFF_SLC", {T_BOOL}) ||
                     !FindMethodTyped(FindClassN(domain, "", "GameData"), "GetParty", {}))
                f.status = L"saved party status methods not found";
            else if (!battleCharacter || !manager || !FindMethodTyped(manager, "GetBtlChara", {T_I4}) ||
                     !FindMethodTyped(battleCharacter, "GetStatusManager", {}) || !status || !FindMethodTyped(status, "ResetStatus", {}))
                f.status = L"battle status methods not found";
            else { f.ok = true; f.status.clear(); }
        }
        else if (f.setColonyLevels || f.unlockColony) {
            void* root = FindClassN(domain, f.rootNs, f.rootClass);
            void* gameData = FindClassN(domain, "", "GameData");
            void* colony = FindClassN(domain, "", "ColonyData");
            if (!root || !(f.rootM = FindMethodTyped(root, f.rootMethod, {}))) f.status = L"game data accessor not found";
            else if (!gameData || !FindMethodTyped(gameData, "GetColonyData", {})) f.status = L"Norende data accessor not found";
            else if (!colony || !il.class_get_field_from_name(colony, "fence") || !il.class_get_field_from_name(colony, "plant")) f.status = L"Norende building data fields not found";
            else { f.ok = true; f.status.clear(); }
        }
        else if (f.rootClass == "REPLACE_ME" || f.fClass.empty() || f.actions.empty()) f.status = L"not configured in features.ini";
        else {
            bool rootOk = true;
            if (!f.noRoot) {
                void* rk = FindClassN(domain, f.rootNs, f.rootClass);
                if (!rk) { rootOk = false; f.status = L"root class not found: " + Widen(f.rootClass); }
                else if (!(f.rootM = FindMethodTyped(rk, f.rootMethod, {}))) { rootOk = false; f.status = L"root method not found: " + Widen(f.rootMethod); }
            }
            if (rootOk) {
                void* fk = FindClassN(domain, f.fNs, f.fClass);
                if (!fk) f.status = L"class not found: " + Widen(f.fClass);
                else {
                    f.fKlass = fk; f.ok = true;
                    if(!f.filter.empty() && !(f.filterM=FindMethodTyped(fk,f.filter,{}))){f.ok=false;f.status=L"filter method not found: "+Widen(f.filter);}
                    for (auto& a : f.actions) {
                        if(!f.ok) break;
                        if (!a.staticArr.empty()) {
                            if (!il.class_get_field_from_name(fk, a.staticArr.c_str())) { f.ok = false; f.status = L"static field not found: " + Widen(a.staticArr); break; }
                        } else if (f.noRoot) { f.ok = false; f.status = L"entries without a root may only use static_array"; break; }
                        else if (!a.reportInt.empty()) {
                            std::vector<int> types;
                            for (auto& x : a.args) types.push_back(TypeOfKind(x.input >= 0 && x.input < (int)f.argTypes.size() ? KindOfType(f.argTypes[x.input]) : x.kind));
                            if (!FindMethodTyped(fk, a.reportInt, types)) { f.ok = false; f.status = L"method not found: " + Widen(a.reportInt); break; }
                        } else if (!a.field.empty()) {
                            if (!il.class_get_field_from_name(fk, a.field.c_str())) { f.ok = false; f.status = L"field not found: " + Widen(a.field); break; }
                        } else {
                            std::vector<int> types;
                            for (auto& x : a.args) types.push_back(TypeOfKind(x.input >= 0 && x.input < (int)f.argTypes.size() ? KindOfType(f.argTypes[x.input]) : x.kind));
                            if (!FindMethodTyped(fk, a.method, types)) { f.ok = false; f.status = L"method not found (name/arg types): " + Widen(a.method); break; }
                        }
                    }
                }
            }
        }
        Log("feature %ls: %s %ls", f.id.c_str(), f.ok ? "OK" : "unavailable", f.status.c_str());
        g_features.push_back(f);
    }
}

#include "constant.h"

// Use the default name delegate, keeping computer template identity and icon intact.
static bool UseSavedFriendName(void* info) {
    if (!info) return false;
    void* klass = il.object_get_class(info);
    void* nameField = il.class_get_field_from_name(klass, "_NAME");
    void* getterField = il.class_get_field_from_name(klass, "GetName");
    void* defaultName = FindMethodTyped(klass, "<.ctor>b__31_0", {});
    void* get = FindMethodTyped(klass, "get_NAME", {});
    if (!nameField || !getterField || !defaultName || !get) return false;
    void* saved = nullptr; il.field_get_value(info, nameField, &saved);
    if (!saved || il.string_length(saved) == 0) return true;
    void* exc = nullptr;
    void* shown = il.runtime_invoke(get, info, nullptr, &exc);
    if (!exc && shown && il.string_length(shown) == il.string_length(saved) &&
        wmemcmp(il.string_chars(shown), il.string_chars(saved), il.string_length(saved)) == 0) return true;
    // Bind the existing saved-name method directly to this friend. Constructing a
    // temporary FriendInfo also initializes Unity resources and must not run here.
    void* currentGetter = nullptr; il.field_get_value(info, getterField, &currentGetter);
    if (!currentGetter) return false;
    void* delegateType = il.type_get_object(il.class_get_type(il.object_get_class(currentGetter)));
    void* reflectedMethod = il.method_get_object(defaultName, klass);
    if (!delegateType || !reflectedMethod) return false;
    void* create = FindMethodTyped(il.object_get_class(reflectedMethod), "CreateDelegate", {T_CLASS, 28});
    if (!create) return false;
    void* args[] = {delegateType, info}; exc = nullptr;
    void* getter = il.runtime_invoke(create, reflectedMethod, args, &exc);
    if (exc || !getter) return false;
    // Reference fields receive the managed object itself, never the address of a local pointer.
    il.field_set_value_object(info, getterField, getter);
    void* installed = nullptr;
    il.field_get_value(info, getterField, &installed);
    if (installed != getter) return false;
    exc = nullptr;
    shown = il.runtime_invoke(get, info, nullptr, &exc);
    return !exc && shown && il.string_length(shown) == il.string_length(saved) &&
           wmemcmp(il.string_chars(shown), il.string_chars(saved), il.string_length(saved)) == 0;
}

static bool IsFriendsMenuOpen() {
    void* klass = FindClassN(g_domain, "UIRoot", "FriendMenu");
    void* field = klass ? il.class_get_field_from_name(klass, "instance") : nullptr;
    void* menu = nullptr;
    if (field) il.field_static_get_value(field, &menu);
    if (!menu) return false;
    // The component survives after closing the menu. Check the actual page's
    // visibility, including its parents, rather than the component's existence.
    // The game's close coroutine clears this cursor as soon as closing starts.
    void* cursorField = il.class_get_field_from_name(klass, "_Cursor");
    void* cursor = nullptr;
    if (cursorField) il.field_get_value(menu, cursorField, &cursor);
    if (!cursor) return false;
    void* pageField = il.class_get_field_from_name(klass, "_Friend");
    void* page = nullptr;
    if (pageField) il.field_get_value(menu, pageField, &page);
    if (!page) return false;
    void* active = FindMethodTyped(il.object_get_class(page), "get_activeInHierarchy", {});
    void* exc = nullptr;
    void* value = active ? il.runtime_invoke(active, page, nullptr, &exc) : nullptr;
    return !exc && value && *(uint8_t*)il.object_unbox(value) != 0;
}

static bool InitializeComputerFriend(void* state, int templateIndex) {
    void* method = FindMethodTyped(il.object_get_class(state), "SetComNPC", {T_I4});
    if (!method) return false;
    void* exc = nullptr; void* args[] = {&templateIndex};
    il.runtime_invoke(method, state, args, &exc);
    return exc == nullptr;
}

static bool GiveRandomFriendName(void* state) {
    static const char* names[] = {"Aster", "Lyra", "Rowan", "Selene", "Cedric", "Mira", "Elara", "Lucian",
        "Terra", "Celes", "Cecil", "Rydia", "Vivi", "Zidane", "Estelle", "Lloyd", "Crono", "Marle",
        "Shion", "Leon", "Iris", "Galen", "Seren", "Arden"};
    ULONG random = 0;
    if (BCryptGenRandom(nullptr, (PUCHAR)&random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        random = GetTickCount();
    void* method = FindMethodTyped(il.object_get_class(state), "GetFriendInfo", {});
    void* exc = nullptr; void* info = method ? il.runtime_invoke(method, state, nullptr, &exc) : nullptr;
    if (exc || !info) return false;
    void* setter = FindMethodTyped(il.object_get_class(info), "set_NAME", {T_STRING});
    if (!setter) return false;
    void* name = il.string_new(names[random % (sizeof(names) / sizeof(names[0]))]);
    void* args[] = {name}; il.runtime_invoke(setter, info, args, &exc);
    return !exc && UseSavedFriendName(info);
}

static bool RebuildComputerFriend(void* state, void* info, int slot) {
    void* klass = il.object_get_class(state);
    int templateIndex = slot % 32;
    void* ctor = FindMethodTyped(klass, ".ctor", {});
    void* replacement = ctor ? il.object_new(klass) : nullptr;
    if (!replacement) return false;
    void* exc = nullptr;
    il.runtime_invoke(ctor, replacement, nullptr, &exc);
    if (exc || !InitializeComputerFriend(replacement, templateIndex) || !GiveRandomFriendName(replacement)) return false;
    // Preserve editable values; build a complete replacement before touching the
    // existing record. Keep the same list entry so no friends are removed.
    for (auto pair : {std::make_pair("GetFriendship", "SetFriendship"),
                      std::make_pair("GetPopulation", "SetPopulation")}) {
        int type = strcmp(pair.first, "GetFriendship") == 0 ? T_U1 : T_I4;
        void* getter = FindMethodTyped(klass, pair.first, {});
        void* setter = FindMethodTyped(klass, pair.second, {type});
        if (!getter || !setter) return false;
        void* value = il.runtime_invoke(getter, state, nullptr, &exc);
        if (exc || !value) return false;
        void* args[] = {il.object_unbox(value)};
        il.runtime_invoke(setter, replacement, args, &exc);
        if (exc) return false;
    }
    void* verify = FindMethodTyped(klass, "VerifyData", {});
    void* invalid = FindMethodTyped(klass, "IsInvalid", {});
    if (!verify || !invalid) return false;
    il.runtime_invoke(verify, replacement, nullptr, &exc);
    if (exc) return false;
    void* result = il.runtime_invoke(invalid, replacement, nullptr, &exc);
    if (exc || !result || *(uint8_t*)il.object_unbox(result)) return false;
    void* fields[3]{}; void* values[3]{};
    const char* names[] = {"m_MetaData", "m_CoreData", "m_passenger"};
    for (int i = 0; i < 3; ++i) {
        fields[i] = il.class_get_field_from_name(klass, names[i]);
        if (!fields[i]) return false;
        il.field_get_value(replacement, fields[i], &values[i]);
    }
    for (int i = 0; i < 3; ++i) il.field_set_value_object(state, fields[i], values[i]);
    Log("Rebuilt and renamed computer friend in slot %d", slot + 1);
    return true;
}

static std::wstring MaintainComputerFriends(bool force = false) {
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    if (!force && now - last < 1000) return L"";
    last = now;
    const std::wstring settings = g_dir + L"\\trainer-local.ini";
    bool queued = GetPrivateProfileIntW(L"Friends", L"RepairAndRename", 0, settings.c_str()) != 0;
    if (IsFriendsMenuOpen()) return force ? L"Close the in-game Friends page, then run the repair." : L"";
    void* root = FindMethodTyped(FindClassN(g_domain, "", "Hikari"), "get_GameData", {});
    if (!root) return L"Game data accessor is unavailable.";
    void* exc = nullptr; void* data = il.runtime_invoke(root, nullptr, nullptr, &exc);
    if (exc || !data) return force ? L"Load your save first." : L"";
    void* klass = il.object_get_class(data);
    void* countMethod = FindMethodTyped(klass, "GetFsFriendCOUNT", {});
    void* friendMethod = FindMethodTyped(klass, "GetFsFriend", {T_I4});
    if (!countMethod || !friendMethod) return L"Friend-list methods are unavailable.";
    void* boxed = il.runtime_invoke(countMethod, data, nullptr, &exc);
    if (exc || !boxed) return L"Could not read the friend list.";
    int count = *(int32_t*)il.object_unbox(boxed);
    if (count <= 0 || count > 20) return force ? L"No friends are available. Load your save first." : L"";
    int repaired = 0, failed = 0, computers = 0;
    for (int slot = 0; slot < count; ++slot) {
        void* args[] = {&slot}; exc = nullptr;
        void* state = il.runtime_invoke(friendMethod, data, args, &exc);
        if (exc || !state) { ++failed; continue; }
        void* stateClass = il.object_get_class(state);
        void* typeMethod = FindMethodTyped(stateClass, "GetFriendType", {});
        void* infoMethod = FindMethodTyped(stateClass, "GetFriendInfo", {});
        if (!typeMethod || !infoMethod) { ++failed; continue; }
        boxed = il.runtime_invoke(typeMethod, state, nullptr, &exc);
        if (exc || !boxed) { ++failed; continue; }
        if (*(uint8_t*)il.object_unbox(boxed) != 15) continue;
        ++computers;
        void* info = il.runtime_invoke(infoMethod, state, nullptr, &exc);
        if (exc || !info) { ++failed; continue; }
        void* infoClass = il.object_get_class(info);
        void* idField = il.class_get_field_from_name(infoClass, "SAVEDATA_ID");
        void* id = nullptr; if (idField) il.field_get_value(info, idField, &id);
        void* identity = id ? FindMethodTyped(il.object_get_class(id), "GetTransferableid", {}) : nullptr;
        void* value = identity ? il.runtime_invoke(identity, id, nullptr, &exc) : nullptr;
        bool incomplete = !exc && (!value || *(uint64_t*)il.object_unbox(value) == 0);
        if (force || queued || incomplete) {
            if (RebuildComputerFriend(state, info, slot)) ++repaired;
            else ++failed;
        } else if (!exc) UseSavedFriendName(info);
    }
    if (!force && !queued) return L"";
    if (!computers) return force ? L"There are no computer friends to repair." : L"";
    // Consume the one-shot request after this batch, including partial failures;
    // retrying every second would repeatedly change already repaired names.
    if (queued) WritePrivateProfileStringW(L"Friends", L"RepairAndRename", L"0", settings.c_str());
    std::wstring message = L"Repaired and renamed " + std::to_wstring(repaired) + L" computer friends. Save in-game to keep the changes.";
    if (failed) message += L" " + std::to_wstring(failed) + L" failed; use Repair & rename to retry.";
    Log("Friend repair: %d repaired, %d failures", repaired, failed);
    return message;
}

static std::wstring Execute(Feature& f, const std::vector<std::wstring>& text) {
    if (!f.ok) return L"This option isn't available right now.";
    std::vector<Slot> in(f.argTypes.size());
    for (size_t i = 0; i < in.size(); ++i) {
        Slot& s = in[i]; s.kind = KindOfType(f.argTypes[i]); const std::wstring x = i < text.size() ? text[i] : L"";
        if (i < f.oneBasedLimits.size() && f.oneBasedLimits[i] > 0) {
            wchar_t* end = nullptr;
            long value = wcstol(x.c_str(), &end, 10);
            while (end && iswspace(*end)) ++end;
            int limit = f.oneBasedLimits[i];
            if (x.empty() || end == x.c_str() || !end || *end || value < 1 || value > limit)
                return (i < f.argLabels.size() ? f.argLabels[i] : L"Slot") + L": enter a number from 1 to " + std::to_wstring(limit) + L".";
            s.i = (int32_t)value - 1;
            continue;
        }
        switch (s.kind) {
        case 'l': s.l = _wtoi64(x.c_str()); break;
        case 'b': s.b = (x == L"1" || x == L"true" || x == L"on"); break;
        case 'y': s.b = (uint8_t)_wtoi(x.c_str()); break;
        case 'f': s.f = (float)_wtof(x.c_str()); break;
        case 's': s.s = il.string_new(Narrow(x).c_str()); break;
        default:  s.i = (int32_t)_wtoi64(x.c_str());
        }
    }
    // Reject invalid inventory IDs and quantities before calling the game's
    // AddItem method. That method can accept malformed table IDs, but the bad
    // inventory entry may only crash later when the equipment screen reads it.
    bool isUniqueItem = false;
    bool uniqueItemQuantityReduced = false;
    if (f.id == L"add_item" || f.id == L"add_item_alt") {
        if (in.size() < 2) return L"The item feature is missing its ID or quantity input.";
        int id = in[0].i, quantity = in[1].i;
        if (id <= 0) return L"Choose a real item from Search; item ID 0 is not valid.";
        if (quantity < 1 || quantity > 99) return L"Item quantity must be from 1 to 99.";
        if (!IsKnownItemId(id)) return L"That ID is not an item in the loaded game table. Choose a listed Search result; nothing was added.";
        const int itemType = ItemTypeById(id);
        isUniqueItem = IsProtectedUniqueItem(id, itemType);
        if (isUniqueItem && quantity != 1) {
            in[1].i = 1;
            uniqueItemQuantityReduced = true;
        }
        Log("item spawn quantity=%d", quantity);
    }
    if (f.setColonyLevels || f.unlockColony) {
        int level = f.unlockColony ? 11 : (in.empty() ? 0 : in[0].i);
        if (level < 0 || level > 11) return L"Norende building level must be from 0 to 11.";
        void* exc = nullptr; void* data = il.runtime_invoke(f.rootM, nullptr, nullptr, &exc);
        if (exc || !data) return L"Load your save first.";
        for (const auto& step : f.chain) {
            std::vector<int> types; std::vector<void*> args;
            for (int idx : step.inputs) { types.push_back(TypeOfKind(in[idx].kind)); args.push_back(PtrOf(in[idx])); }
            void* method = FindMethodTyped(il.object_get_class(data), step.name, types);
            if (!method) return L"Norende data path is unavailable.";
            data = il.runtime_invoke(method, data, args.empty() ? nullptr : args.data(), &exc);
            if (exc || !data) return L"Norende data is unavailable. Load a save first.";
        }
        void* dataClass = il.object_get_class(data);
        int changed = 0;
        for (const char* fieldName : {"fence", "plant"}) {
            void* field = il.class_get_field_from_name(dataClass, fieldName); void* array = nullptr;
            if (field) il.field_get_value(data, field, &array);
            if (!array) return L"Norende shop and building records are unavailable.";
            for (uintptr_t i = 0; i < il.array_length(array); ++i) {
                void* building = *(void**)((char*)array + 0x20 + i * sizeof(void*));
                if (!building) continue;
                void* levelField = il.class_get_field_from_name(il.object_get_class(building), "level");
                if (!levelField) return L"A Norende building level field is unavailable.";
                il.field_set_value(building, levelField, &level);
                ++changed;
            }
        }
        if (f.unlockColony) {
            void* population = il.class_get_field_from_name(dataClass, "population"); int maxPopulation = 999;
            if (!population) return L"Norende population field is unavailable.";
            il.field_set_value(data, population, &maxPopulation);
        }
        if (!changed) return L"No Norende shops or buildings were found.";
        std::wstring result = f.unlockColony ? L"Unlocked and set all Norende shops/buildings to level 11; population set to 999. " : L"Set all Norende shops/buildings to level " + std::to_wstring(level) + L". ";
        if (level >= 1) {
            if (QueueNorendeLevelAchievements(level)) result += L" Matching Norende achievement(s) queued. ";
            else result += L" Achievement notification could not be queued. ";
        }
        return result + L"Reopen Norende to refresh, then save in-game.";
    }
    if (f.repairFriends) return MaintainComputerFriends(true);
    if (f.cureAllStatus) {
        void* exception = nullptr;
        void* gameData = il.runtime_invoke(f.rootM, nullptr, nullptr, &exception);
        if (exception || !gameData) return L"Load a save first so the trainer can reach your party.";
        void* partyGetter = FindMethodTyped(il.object_get_class(gameData), "GetParty", {});
        void* party = partyGetter ? il.runtime_invoke(partyGetter, gameData, nullptr, &exception) : nullptr;
        if (exception || !party) return L"Your saved party is unavailable.";
        void* partyClass = il.object_get_class(party);
        void* countMethod = FindMethodTyped(partyClass, "GetMemberCount", {});
        void* memberMethod = FindMethodTyped(partyClass, "GetMember", {T_I4});
        if (!countMethod || !memberMethod) return L"Your party member list is unavailable.";
        void* countBox = il.runtime_invoke(countMethod, party, nullptr, &exception);
        if (exception || !countBox) return L"Could not read your party members.";
        const int count = std::clamp(*(int*)il.object_unbox(countBox), 0, 4);
        int savedCured = 0, battleCured = 0;
        for (int index = 0; index < count; ++index) {
            void* indexArgs[] = {&index}; exception = nullptr;
            void* character = il.runtime_invoke(memberMethod, party, indexArgs, &exception);
            if (exception || !character) continue;
            void* characterClass = il.object_get_class(character);
            bool changed = false;
            for (const char* methodName : {"SetDEBUFF_POI", "SetDEBUFF_BLD", "SetDEBUFF_SLC"}) {
                void* setter = FindMethodTyped(characterClass, methodName, {T_BOOL});
                if (!setter) continue;
                uint8_t clear = 0; void* args[] = {&clear}; exception = nullptr;
                il.runtime_invoke(setter, character, args, &exception);
                if (!exception) changed = true;
            }
            if (changed) ++savedCured;
        }

        // During battle, also clear temporary effects held only by battle instances.
        void* battleClass = FindClassN(g_domain, "", "BtlFunction");
        void* battleGetter = battleClass ? FindMethodTyped(battleClass, "GetInstance", {}) : nullptr;
        exception = nullptr;
        void* battle = battleGetter ? il.runtime_invoke(battleGetter, nullptr, nullptr, &exception) : nullptr;
        if (!exception && battle) {
            void* managerGetter = FindMethodTyped(il.object_get_class(battle), "GetBtlCharaManager", {});
            void* manager = managerGetter ? il.runtime_invoke(managerGetter, battle, nullptr, &exception) : nullptr;
            void* getCharacter = manager ? FindMethodTyped(il.object_get_class(manager), "GetBtlChara", {T_I4}) : nullptr;
            if (getCharacter) for (int index = 0; index < 16; ++index) {
                void* indexArgs[] = {&index}; exception = nullptr;
                void* character = il.runtime_invoke(getCharacter, manager, indexArgs, &exception);
                if (exception || !character) continue;
                void* teamMethod = FindMethodTyped(il.object_get_class(character), "IsTeamPlayer", {});
                void* teamBox = teamMethod ? il.runtime_invoke(teamMethod, character, nullptr, &exception) : nullptr;
                if (exception || !teamBox || !*(uint8_t*)il.object_unbox(teamBox)) continue;
                void* statusGetter = FindMethodTyped(il.object_get_class(character), "GetStatusManager", {});
                void* statusManager = statusGetter ? il.runtime_invoke(statusGetter, character, nullptr, &exception) : nullptr;
                void* reset = statusManager ? FindMethodTyped(il.object_get_class(statusManager), "ResetStatus", {}) : nullptr;
                if (!reset) continue;
                exception = nullptr; il.runtime_invoke(reset, statusManager, nullptr, &exception);
                if (!exception) ++battleCured;
            }
        }
        return (savedCured || battleCured)
            ? L"Cleared saved poison, blindness, and silence for " + std::to_wstring(savedCured) + L" party member(s), plus active battle effects for " + std::to_wstring(battleCured) + L"."
            : L"No party status effects were found to clear.";
    }
    if (f.setJobLevel) {
        const size_t expectedInputs = f.setOneJobLevel ? 3u : (f.argTypes.size() == 2 ? 2u : 1u);
        if (in.size() != expectedInputs) return L"Job level inputs are misconfigured.";
        int level = f.setOneJobLevel ? in[2].i : (f.argTypes.size() == 2 ? in[1].i : in[0].i);
        if (level < 1 || level > 14) return L"Job level must be from 1 to 14.";
        void* exc = nullptr; void* root = il.runtime_invoke(f.rootM, nullptr, nullptr, &exc);
        if (exc || !root) return L"Load your save first.";
        void* party = root;
        if (!f.chain.empty()) {
            for (const auto& step : f.chain) {
                std::vector<int> types; std::vector<void*> args;
                for (int idx : step.inputs) { types.push_back(TypeOfKind(in[idx].kind)); args.push_back(PtrOf(in[idx])); }
                void* method = FindMethodTyped(il.object_get_class(party), step.name, types);
                if (!method) return L"Job-level path is unavailable.";
                party = il.runtime_invoke(method, party, args.empty()?nullptr:args.data(), &exc);
                if (exc || !party) return L"Selected character slot is empty.";
            }
        }
        std::vector<void*> members;
        bool individual = std::any_of(f.chain.begin(), f.chain.end(), [](const Step& step) { return step.name == "GetMember"; });
        if (individual) members.push_back(party);
        else {
            void* countM=FindMethodTyped(il.object_get_class(party),"GetMemberCount",{});
            void* getM=FindMethodTyped(il.object_get_class(party),"GetMember",{T_I4});
            if(!countM||!getM)return L"Party member list is unavailable.";
            void* count=il.runtime_invoke(countM,party,nullptr,&exc);if(exc||!count)return L"Could not read party members.";
            int n=std::clamp(*(int*)il.object_unbox(count),0,4);
            for(int i=0;i<n;++i){void* a[]={&i};void* m=il.runtime_invoke(getM,party,a,&exc);if(!exc&&m)members.push_back(m);}
        }
        void* paramClass=FindClassN(g_domain,"","JobParam");
        void* getParam=FindMethodTyped(paramClass,"GetParam",{T_I4,T_I4});
        int changed=0;
        if (f.setOneJobLevel) {
            if (members.size() != 1) return L"Choose one party member first.";
            void* member = members.front();
            void* countM = FindMethodTyped(il.object_get_class(member), "GetJOB_COUNT", {});
            void* getJob = FindMethodTyped(il.object_get_class(member), "GetJOB", {T_I4});
            if (!countM || !getJob) return L"The selected character's job list is unavailable.";
            void* countBox = il.runtime_invoke(countM, member, nullptr, &exc);
            if (exc || !countBox) return L"Could not read the selected character's jobs.";
            int count = std::clamp(*(int*)il.object_unbox(countBox), 0, 64);
            const int jobSlot = in[1].i;
            if (jobSlot < 0 || jobSlot >= count) return L"That job slot is not available for this character.";
            void* args[] = {(void*)&jobSlot};
            void* job = il.runtime_invoke(getJob, member, args, &exc);
            if (exc || !job) return L"Could not find the selected job.";
            void* idM = FindMethodTyped(il.object_get_class(job), "GetJobId", {});
            void* idBox = idM ? il.runtime_invoke(idM, job, nullptr, &exc) : nullptr;
            if (exc || !idBox) return L"The selected job ID is unavailable.";
            int id = *(int*)il.object_unbox(idBox); void* paramArgs[] = {&id, &level};
            void* param = getParam ? il.runtime_invoke(getParam, nullptr, paramArgs, &exc) : nullptr;
            if (exc || !param) return L"Job-level data is unavailable.";
            void* field = il.class_get_field_from_name(il.object_get_class(param), "TARGET_EXP"); int exp = 0;
            if (!field) return L"Job-level experience data is unavailable.";
            il.field_get_value(param, field, &exp);
            void* set = FindMethodTyped(il.object_get_class(job), "SetLevel", {T_I4,T_I4,T_CLASS});
            int tableId = 0; void* setArgs[] = {&exp, &tableId, nullptr};
            if (!set) return L"The selected job cannot be updated.";
            il.runtime_invoke(set, job, setArgs, &exc);
            if (exc) return L"The game rejected the selected job-level change.";
            return L"Set job slot " + std::to_wstring(jobSlot + 1) + L" on party member " + std::to_wstring(in[0].i + 1) + L" to job level " + std::to_wstring(level) + L".";
        }
        for(void* member:members){
            void* jobCountM=FindMethodTyped(il.object_get_class(member),"GetJOB_COUNT",{});
            void* getJob=FindMethodTyped(il.object_get_class(member),"GetJOB",{T_I4});
            if(!jobCountM||!getJob)continue;
            void* count=il.runtime_invoke(jobCountM,member,nullptr,&exc);if(exc||!count)continue;
            int n=std::clamp(*(int*)il.object_unbox(count),0,64);
            for(int i=0;i<n;++i){
                void* a[]={&i};void* job=il.runtime_invoke(getJob,member,a,&exc);if(exc||!job)continue;
                void* idM=FindMethodTyped(il.object_get_class(job),"GetJobId",{});
                void* idBox=idM?il.runtime_invoke(idM,job,nullptr,&exc):nullptr;if(exc||!idBox)continue;
                int id=*(int*)il.object_unbox(idBox);void* pa[]={&id,&level};
                void* param=getParam?il.runtime_invoke(getParam,nullptr,pa,&exc):nullptr;if(exc||!param)continue;
                void* field=il.class_get_field_from_name(il.object_get_class(param),"TARGET_EXP");int exp=0;if(field)il.field_get_value(param,field,&exp);
                void* set=FindMethodTyped(il.object_get_class(job),"SetLevel",{T_I4,T_I4,T_CLASS});
                int tableId=0;void* sa[]={&exp,&tableId,nullptr};if(set){il.runtime_invoke(set,job,sa,&exc);if(!exc)++changed;}
            }
        }
        return L"Set job level to " + std::to_wstring(level) + L" on " + std::to_wstring(changed) + L" job slot(s).";
    }
    if (f.friendMax || f.friendMaxAll) {
        if (f.friendMax && (in.size() != 1 || in[0].i < 0 || in[0].i >= 20)) return L"Friend slot must be from 1 to 20.";
        void* exc = nullptr;
        void* data = il.runtime_invoke(f.rootM, nullptr, nullptr, &exc);
        if (exc || !data) return L"Load your save first.";
        void* dataClass = il.object_get_class(data);
        void* countM = FindMethodTyped(dataClass, "GetFsFriendCOUNT", {});
        void* getM = FindMethodTyped(dataClass, "GetFsFriend", {T_I4});
        if (!countM || !getM) return L"Friend list access is unavailable.";
        void* boxed = il.runtime_invoke(countM, data, nullptr, &exc);
        if (exc || !boxed) return L"Could not read the friend slots.";
        int count = std::clamp(*(int*)il.object_unbox(boxed), 0, 20);
        int first = f.friendMax ? in[0].i : 0;
        int last = f.friendMax ? first + 1 : count;
        int changed = 0, failed = 0;
        for (int slot = first; slot < last; ++slot) {
            void* a[] = {&slot}; exc = nullptr;
            void* state = il.runtime_invoke(getM, data, a, &exc);
            if (exc || !state) { if (f.friendMax) ++failed; continue; }
            void* stateClass = il.object_get_class(state);
            auto call = [&](const char* name, const std::vector<int>& types, void** args) {
                void* method = FindMethodTyped(stateClass, name, types);
                if (!method) return false;
                void* error = nullptr; il.runtime_invoke(method, state, args, &error); return error == nullptr;
            };
            uint8_t friendship = 255; int population = 999;
            void* sf[] = {&friendship}; void* sp[] = {&population};
            bool ok = call("SetFriendship", {T_U1}, sf) && call("SetPopulation", {T_I4}, sp);
            void* summonerMethod = FindMethodTyped(stateClass, "GetFriendSummoner", {});
            void* summoner = nullptr; if (ok && summonerMethod) summoner = il.runtime_invoke(summonerMethod, state, nullptr, &exc);
            void* character = nullptr;
            void* characterField = summoner ? il.class_get_field_from_name(il.object_get_class(summoner), "Character") : nullptr;
            if (characterField) il.field_get_value(summoner, characterField, &character);
            if (!character) ok = false;
            if (character) {
                void* ck = il.object_get_class(character);
                for (auto item : {std::pair<const char*, int>{"LV", 99}, {"JOB_LV", 14}, {"ABI_JOB_LV", 14}, {"EXP", 99999999}, {"JOB_EXP", 9999999}}) {
                    void* field = il.class_get_field_from_name(ck, item.first);
                    if (!field) { ok = false; continue; }
                    il.field_set_value(character, field, &item.second);
                }
            }
            // The friend card's displayed power is a separate cached value from
            // the summon stat snapshot below. Keep it in sync with a maxed friend.
            if (ok) {
                void* infoMethod = FindMethodTyped(stateClass, "GetFriendInfo", {});
                void* info = infoMethod ? il.runtime_invoke(infoMethod, state, nullptr, &exc) : nullptr;
                if (exc || !info) ok = false;
                else {
                    void* infoClass = il.object_get_class(info);
                    for (const char* name : {"POWER", "MAXPOWER"}) {
                        void* field = il.class_get_field_from_name(infoClass, name);
                        int value = 99999;
                        if (!field) { ok = false; continue; }
                        il.field_set_value(info, field, &value);
                    }
                }
            }
            void* abiMethod = FindMethodTyped(stateClass, "GetAbiLinkState", {});
            void* abi = ok && abiMethod ? il.runtime_invoke(abiMethod, state, nullptr, &exc) : nullptr;
            if (abi) {
                void* arrayField = il.class_get_field_from_name(il.object_get_class(abi), "Character");
                void* array = nullptr; if (arrayField) il.field_get_value(abi, arrayField, &array);
                for (uintptr_t n = 0; array && n < il.array_length(array); ++n) {
                    void* ac = *(void**)((char*)array + 0x20 + n * sizeof(void*)); if (!ac) continue;
                    void* ack = il.object_get_class(ac);
                    void* expField = il.class_get_field_from_name(ack, "Exp"); int exp = 99999999;
                    if (expField) il.field_set_value(ac, expField, &exp);
                    void* jobsField = il.class_get_field_from_name(ack, "JobExp"); void* jobs = nullptr;
                    if (jobsField) il.field_get_value(ac, jobsField, &jobs);
                    for (uintptr_t j = 0; jobs && j < il.array_length(jobs); ++j) ((int*)((char*)jobs + 0x20))[j] = 9999999;
                }
            } else ok = false;
            // Rebuild the cached friend combat-stat snapshot after changing level and job levels.
            // Friend summon data is a serialized snapshot; changing Character alone leaves its
            // attack, defense, accuracy, and magic values stale (often at the generated default).
            if (ok) {
                void* summonerClass = il.object_get_class(summoner);
                void* parameterField = il.class_get_field_from_name(summonerClass, "Paramater");
                void* parameter = nullptr;
                if (parameterField) il.field_get_value(summoner, parameterField, &parameter);
                void* helperClass = FindClassN(g_domain, "", "BtlHelper");
                void* buildCharacter = helperClass ? FindMethodTyped(helperClass, "CreateCharacterState", {T_CLASS, T_CLASS}) : nullptr;
                void* rebuilt = nullptr;
                if (parameter && buildCharacter) {
                    void* buildArgs[] = {state, nullptr};
                    exc = nullptr;
                    rebuilt = il.runtime_invoke(buildCharacter, nullptr, buildArgs, &exc);
                }
                if (!rebuilt || exc) ok = false;
                else {
                    struct StatCall { const char* method; const char* field; int argc; };
                    const StatCall calls[] = {
                        {"GetMHP", "HP", 1}, {"GetATK", "ATK", 2}, {"GetDEF", "DEF", 1},
                        {"GetMATK", "MATK", 1}, {"GetMDEF", "MDEF", 1}, {"GetACC", "HIT", 2},
                        {"GetDOD", "AVD", 1}, {"GetSTR", "STR", 1}, {"GetVIT", "VIT", 1},
                        {"GetINT", "INT", 1}, {"GetMND", "MND", 1}, {"GetAGI", "AGI", 1},
                        {"GetDEX", "DEX", 1}
                    };
                    int arm = 0; uint8_t noManjuu = 0;
                    for (const auto& stat : calls) {
                        std::vector<int> types = stat.argc == 2 ? std::vector<int>{T_I4, T_BOOL} : std::vector<int>{T_BOOL};
                        void* method = FindMethodTyped(il.object_get_class(rebuilt), stat.method, types);
                        void* field = il.class_get_field_from_name(il.object_get_class(parameter), stat.field);
                        void* args[] = {stat.argc == 2 ? static_cast<void*>(&arm) : static_cast<void*>(&noManjuu), &noManjuu};
                        exc = nullptr;
                        void* value = method ? il.runtime_invoke(method, rebuilt, args, &exc) : nullptr;
                        if (!field || !value || exc) { ok = false; continue; }
                        int result = *static_cast<int*>(il.object_unbox(value));
                        il.field_set_value(parameter, field, &result);
                    }
                    void* speedMethod = FindMethodTyped(il.object_get_class(rebuilt), "GetACTSPD", {T_BOOL});
                    void* speedField = il.class_get_field_from_name(il.object_get_class(parameter), "FIGURE_ACTSPEED");
                    void* speedArgs[] = {&noManjuu}; exc = nullptr;
                    void* speedBox = speedMethod ? il.runtime_invoke(speedMethod, rebuilt, speedArgs, &exc) : nullptr;
                    if (speedBox && speedField && !exc) {
                        int speed = *static_cast<int*>(il.object_unbox(speedBox));
                        il.field_set_value(parameter, speedField, &speed);
                    } else ok = false;
                }
            }
            if (ok) ++changed; else ++failed;
        }
        if (!changed) return L"No friend slots were updated. Check that the selected slot contains a friend.";
        return L"Maxed level, job levels, friend power, population, and friendship for " + std::to_wstring(changed) + L" friend slot(s)." +
               (failed ? L" " + std::to_wstring(failed) + L" slot(s) failed." : L"") + L" Save in-game to keep the changes.";
    }
    void* exc = nullptr;
    if (f.addComNpcFriend) {
        if (in.size() != 1 || in[0].kind != 'i') return L"Computer friend template input is misconfigured.";
        const int npcIndex = in[0].i;
        if (npcIndex < 0 || npcIndex >= 32) return L"Computer friend template must be from 1 to 32.";

        void* gameData = il.runtime_invoke(f.rootM, nullptr, nullptr, &exc);
        if (exc || !gameData) return L"Game data is unavailable - load a save first.";
        void* gameDataClass = il.object_get_class(gameData);
        void* countMethod = FindMethodTyped(gameDataClass, "GetFsFriendCOUNT", {});
        void* addMethod = FindMethodTyped(gameDataClass, "SetFsFriend", {T_CLASS});
        if (!countMethod || !addMethod) return L"Game friend-list methods could not be resolved.";

        auto readFriendCount = [&](int& count) -> bool {
            exc = nullptr;
            void* boxed = il.runtime_invoke(countMethod, gameData, nullptr, &exc);
            if (exc || !boxed) return false;
            count = *(int32_t*)il.object_unbox(boxed);
            return true;
        };
        int before = 0;
        if (!readFriendCount(before)) return L"Could not read the current friend count.";
        if (before >= 20) return L"The friend list is full (20 of 20). Remove a friend before adding another.";

        void* friendState = il.object_new(f.fKlass);
        if (!friendState) return L"Could not create a FriendState object.";
        void* ctor = FindMethodTyped(f.fKlass, ".ctor", {});
        exc = nullptr;
        il.runtime_invoke(ctor, friendState, nullptr, &exc);
        if (exc) return L"FriendState initialization failed.";

        auto invokeFriend = [&](const char* name, int type, void* arg) -> bool {
            void* method = FindMethodTyped(f.fKlass, name, {type});
            if (!method) return false;
            void* args[1] = {arg};
            exc = nullptr;
            il.runtime_invoke(method, friendState, args, &exc);
            return exc == nullptr;
        };
        if (IsFriendsMenuOpen()) return L"Close the in-game Friends menu before adding a friend.";
        if (!InitializeComputerFriend(friendState, npcIndex))
            return L"The game could not initialize the complete computer friend record.";
        uint8_t isFriend = 1;
        uint8_t friendship = (uint8_t)std::clamp((int)GetPrivateProfileIntW(L"friend_add_com_npc", L"default_friendship", 255, (g_dir + L"\\features.ini").c_str()), 0, 255);
        int population = std::clamp((int)GetPrivateProfileIntW(L"friend_add_com_npc", L"default_population", 999, (g_dir + L"\\features.ini").c_str()), 0, 999);
        if (!invokeFriend("SetFriend", T_BOOL, &isFriend) || !invokeFriend("SetFriendship", T_U1, &friendship) || !invokeFriend("SetPopulation", T_I4, &population))
            return L"The game could not finish the computer friend record.";
        if (!GiveRandomFriendName(friendState)) return L"The game could not assign the computer friend's name.";
        void* verify = FindMethodTyped(f.fKlass, "VerifyData", {});
        exc = nullptr; il.runtime_invoke(verify, friendState, nullptr, &exc);
        if (exc) return L"The game could not validate the generated friend record.";
        void* invalidMethod = FindMethodTyped(f.fKlass, "IsInvalid", {});
        exc = nullptr; void* invalidBox = il.runtime_invoke(invalidMethod, friendState, nullptr, &exc);
        if (exc || !invalidBox || *(uint8_t*)il.object_unbox(invalidBox))
            return L"The game marked this generated computer friend as invalid.";

        void* addArgs[1] = {friendState};
        exc = nullptr;
        il.runtime_invoke(addMethod, gameData, addArgs, &exc);
        if (exc) return L"The game rejected the generated friend.";
        int after = 0;
        if (!readFriendCount(after)) return L"Friend was submitted, but the new friend count could not be verified.";
        if (after <= before) return L"The game did not add the friend. Try another computer friend template.";
        Log("friend_add_com_npc: template %d, friend count %d -> %d", npcIndex, before, after);
        return L"Added " + std::wstring((npcIndex % 4)==0?L"Friend-bot":(npcIndex % 4)==1?L"Buddy-bot":(npcIndex % 4)==2?L"Pal-bot":L"Amigo-bot") + L" template " + std::to_wstring(npcIndex + 1) + L" (table level " + std::to_wstring(npcIndex / 4 + 1) + L") to the next open slot (" + std::to_wstring(after) + L"/20). Save in-game to keep the change.";
    }
    void* obj = nullptr;
    if (!f.noRoot) {
        obj = il.runtime_invoke(f.rootM, nullptr, nullptr, &exc);
        if (exc || !obj) return L"Load a save first. For Norende options, open the Norende screen too.";
        for (auto& st : f.chain) {
            std::vector<int> types; std::vector<void*> args;
            for (int idx : st.inputs) {
                if (idx < 0 || idx >= (int)in.size()) return L"Bad input index in chain";
                types.push_back(TypeOfKind(in[idx].kind)); args.push_back(PtrOf(in[idx]));
            }
            void* m = FindMethodTyped(il.object_get_class(obj), st.name, types);
            if (!m) return L"Chain step not found: " + Widen(st.name);
            exc = nullptr; obj = il.runtime_invoke(m, obj, args.empty() ? nullptr : args.data(), &exc);
            if (exc || !obj) return L"This option isn't available on the current screen, or the selected slot is empty.";
        }
    }

    // Key items (including asterisks) and costumes represent unique story state. Check the
    // live party inventory before using the game's normal AddItem method, and
    // fail closed if this game build cannot answer the ownership query.
    if (isUniqueItem) {
        bool checkAvailable = false;
        if (PartyAlreadyHasUniqueItem(obj, in[0].i, checkAvailable))
            return L"You already have this unique item. No duplicate was added.";
        if (!checkAvailable)
            return L"The game could not verify ownership of this unique item, so nothing was added.";
    }

    std::vector<void*> targets{obj};
    for (auto& lv : f.levels) {                       // expand: each target -> its items (members -> jobs, ...)
        std::vector<void*> next;
        for (void* cur : targets) {
            if (!cur) return L"foreach without an object";
            void* k = il.object_get_class(cur);
            bool fixedRange=lv.count.rfind("range:",0)==0;
            void* mc = fixedRange?nullptr:FindMethodTyped(k, lv.count, {}); void* mi = FindMethodTyped(k, lv.item, {T_I4});
            if ((!fixedRange&&!mc) || !mi) return L"foreach methods not found: " + Widen(lv.count);
            int count=0;
            if(fixedRange) count=std::max(0,std::min(256,atoi(lv.count.c_str()+6)));
            else {exc=nullptr;void* boxed=il.runtime_invoke(mc,cur,nullptr,&exc);if(exc||!boxed)return L"count call failed";count=*(int32_t*)il.object_unbox(boxed);}
            for (int i = 0; i < count && i < 256; ++i) {
                void* a[1] = {&i}; exc = nullptr; void* it = il.runtime_invoke(mi, cur, a, &exc);
                if (!exc && it) next.push_back(it);
            }
        }
        targets = next;
        if (targets.empty()) return L"Nothing changed. Check that the selected characters, jobs, or tasks are available.";
    }
    if(f.filterM){
        std::vector<void*> filtered; filtered.reserve(targets.size());
        for(void* t:targets){if(!t)continue;void* fm=FindMethodTyped(il.object_get_class(t),f.filter,{});if(!fm)return L"Target filter not found at run time: "+Widen(f.filter);exc=nullptr;void* boxed=il.runtime_invoke(fm,t,nullptr,&exc);if(exc||!boxed)return L"Target filter failed: "+Widen(f.filter);if(*(uint8_t*)il.object_unbox(boxed))filtered.push_back(t);}
        targets=std::move(filtered); if(targets.empty())return L"No matching characters are available right now.";
    }

    int applied = 0; std::wstring extra;
    for (void* t : targets) {
        void* k = t ? il.object_get_class(t) : f.fKlass;
        for (auto& act : f.actions) {
            std::vector<Slot> fa(act.args.size());
            for (size_t i = 0; i < fa.size(); ++i) {
                const Arg& a = act.args[i];
                if (a.input >= 0) { if (a.input >= (int)in.size()) return L"Bad input index in final_args"; fa[i] = in[a.input]; }
                else {
                    fa[i].kind = a.kind;
                    switch (a.kind) {
                    case 'l': fa[i].l = a.num; break;
                    case 'b': case 'y': fa[i].b = (uint8_t)a.num; break;
                    case 'f': fa[i].f = (float)a.fl; break;
                    case 's': fa[i].s = il.string_new(a.str.c_str()); break;
                    case 'n': break;
                    default:  fa[i].i = (int32_t)a.num;
                    }
                }
            }
            if (!act.reportInt.empty()) {                 // diagnostic: show a number returned by the game
                if (!t) return L"No object for this action";
                std::vector<int> reportTypes; std::vector<void*> reportArgs;
                for (auto& arg : fa) { reportTypes.push_back(TypeOfKind(arg.kind)); reportArgs.push_back(PtrOf(arg)); }
                void* rm = FindMethodTyped(k, act.reportInt, reportTypes);
                if (!rm) return L"Method not found at run time: " + Widen(act.reportInt);
                exc = nullptr; void* boxed = il.runtime_invoke(rm, t, reportArgs.empty() ? nullptr : reportArgs.data(), &exc);
                if (exc || !boxed) return L"Report call failed: " + Widen(act.reportInt);
                extra += L"   " + Widen(act.reportInt) + L" = " + std::to_wstring(*(int32_t*)il.object_unbox(boxed));
                continue;
            }
            if (!act.staticArr.empty()) {                 // overwrite (or restore) a static float[] such as TuningData.ExpTuneAmount
                void* fld = il.class_get_field_from_name(k, act.staticArr.c_str());
                void* arr = nullptr; if (fld) il.field_static_get_value(fld, &arr);
                if (!arr) return L"Static array not available yet: " + Widen(act.staticArr);
                size_t len = (size_t)il.array_length(arr);
                if (len == 0 || len > 64) return L"Unexpected array size for " + Widen(act.staticArr);
                float* el = (float*)((char*)arr + 0x20);
                std::string key = f.fClass + "." + act.staticArr;
                if (!g_origArr.count(key)) g_origArr[key] = std::vector<float>(el, el + len);
                if (act.restore) { auto& o = g_origArr[key]; for (size_t i = 0; i < len && i < o.size(); ++i) el[i] = o[i]; }
                else {
                    if (fa.empty()) return L"static_array needs a value in final_args";
                    float v = fa[0].kind == 'f' ? fa[0].f : (float)fa[0].i;
                    for (size_t i = 0; i < len; ++i) el[i] = v;
                }
                continue;
            }
            if (!t) return L"No object for this action";
            if (!act.field.empty()) {
                void* fld = il.class_get_field_from_name(k, act.field.c_str());
                if (!fld || fa.empty()) return L"Field not found at run time: " + Widen(act.field);
                il.field_set_value(t, fld, PtrOf(fa[0]));
            } else {
                std::vector<int> types; std::vector<void*> args;
                for (auto& sl : fa) { types.push_back(TypeOfKind(sl.kind)); args.push_back(PtrOf(sl)); }
                void* m = FindMethodTyped(k, act.method, types);
                if (!m) return L"Method not found at run time: " + Widen(act.method);
                exc = nullptr; il.runtime_invoke(m, t, args.empty() ? nullptr : args.data(), &exc);
                if (exc) return L"Game threw an exception (wrong args or wrong game state)";
            }
        }
        ++applied;
    }
    return f.label + L" completed." + (uniqueItemQuantityReduced ? L" This item is unique, so quantity was limited to 1." : L"") + extra;
}

// ---------------- Item database (ItemTable.TryGetParam / get_NAME), built on first search ----------------
struct ItemEntry { int id, type; std::wstring name, folded; };
static std::vector<ItemEntry> g_items; static bool g_itemsLoaded = false;
static const wchar_t* ItemKind(int type, const std::wstring& name) {
    if (name.find(L'*') != std::wstring::npos) return L"Asterisk";
    switch (type) {
    case 1: return L"Sword"; case 2: return L"Axe"; case 3: return L"Spear";
    case 4: return L"Rod"; case 5: return L"Staff"; case 6: return L"Dagger";
    case 7: return L"Bow"; case 8: return L"Katana"; case 9: return L"Knuckles";
    case 10: return L"Hat"; case 11: return L"Helmet"; case 12: return L"Armor";
    case 13: return L"Light Armor"; case 14: return L"Shield"; case 15: return L"Accessory";
    case 16: return L"Consumable"; case 17: return L"Magic"; case 18: return L"Key Item";
    case 19: return L"Costume"; default: return L"Item";
    }
}
static std::wstring Folded(std::wstring text) { for (auto& ch : text) ch = (wchar_t)towlower(ch); return text; }
static bool ItemKindMatches(int type, const std::wstring& name, const std::wstring& query) {
    std::wstring kind = Folded(ItemKind(type, name));
    if (kind.find(query) != std::wstring::npos) return true;
    std::wstring singular = query; if (singular.size() > 3 && singular.back() == L's') singular.pop_back();
    if (kind.find(singular) != std::wstring::npos) return true;
    if ((query == L"weapon" || query == L"weapons") && type >= 1 && type <= 9) return true;
    if ((query == L"headgear" || query == L"head" || query == L"hat" || query == L"helmet") && (type == 10 || type == 11)) return true;
    if ((query == L"armor" || query == L"armour") && (type == 12 || type == 13)) return true;
    if ((query == L"item" || query == L"items" || query == L"consumables") && type == 16) return true;
    if ((query == L"spell" || query == L"spells") && type == 17) return true;
    if ((query == L"asterisk" || query == L"asterisks" || query == L"job") && type == 18 && name.find(L'*') != std::wstring::npos) return true;
    if ((query == L"key" || query == L"key items") && type == 18) return true;
    if ((query == L"equipment" || query == L"equip" || query == L"gear") && ((type >= 1 && type <= 15) || type == 19)) return true;
    if ((query == L"clothing" || query == L"clothes") && (type == 13 || type == 19)) return true;
    return false;
}

static std::wstring StrOf(void* s) {
    if (!s) return L"";
    int n = il.string_length(s);
    return std::wstring(il.string_chars(s), n > 0 ? n : 0);
}
static std::wstring LoadItems() {            // returns an error message, or "" on success
    if (g_itemsLoaded) return L"";
    void* k = FindClassN(g_domain, "", "ItemTable");
    if (!k) return L"ItemTable class not found";
    void* tryGet = FindMethodTyped(k, "TryGetParam", {T_I4}); void* getName = FindMethodTyped(k, "get_NAME", {}); void* getType = FindMethodTyped(k, "get_TYPE", {});
    if (!tryGet || !getName || !getType) return L"ItemTable.TryGetParam / item name or type lookup not found";
    for (int id = 1; id < 100000; ++id) {
        void* exc = nullptr; void* a[1] = {&id};
        void* row = il.runtime_invoke(tryGet, nullptr, a, &exc);
        if (exc || !row) continue;
        void* ne = nullptr; std::wstring nm = StrOf(il.runtime_invoke(getName, row, nullptr, &ne));
        void* te = nullptr; void* boxedType = il.runtime_invoke(getType, row, nullptr, &te);
        if (ne || te || !boxedType) continue;
        int type = *(int*)il.object_unbox(boxedType);
        std::wstring folded=ne?L"":nm; for(auto& ch:folded) ch=(wchar_t)towlower(ch);
        g_items.push_back({id,type,nm,std::move(folded)});
    }
    g_itemsLoaded = true;
    Log("item table: %zu entries", g_items.size());
    for (size_t i = 0; i < g_items.size() && i < 30; ++i) Log("  item %d = %ls", g_items[i].id, g_items[i].name.c_str());
    return L"";
}
static bool IsKnownItemId(int id) {
    if (!LoadItems().empty()) return false;
    auto found = std::find_if(g_items.begin(), g_items.end(), [id](const ItemEntry& item) { return item.id == id; });
    if (found == g_items.end()) return false;
    Log("item spawn request id=%d type=%d name=%ls", id, found->type, found->name.c_str());
    return true;
}

static int ItemTypeById(int id) {
    if (!LoadItems().empty()) return -1;
    auto item = std::find_if(g_items.begin(), g_items.end(), [id](const ItemEntry& entry) { return entry.id == id; });
    return item == g_items.end() ? -1 : item->type;
}

// Only guard items whose duplicate ownership can conflict with progression or
// character equipment state: key items (including asterisks) and costumes
// restricted to one character. Perma-Job Clothes (30428) is intentionally
// excluded because it is a universal costume.
static bool IsProtectedUniqueItem(int id, int itemType) {
    if (itemType == 18) return true;
    if (itemType != 19 || id == 30428) return false;
    switch (id) {
    case 30422: case 30423: case 30424: case 30425: case 30426: case 30427:
    case 30510: case 30511: case 30512: case 30513: case 30514:
        return true;
    default:
        return false;
    }
}

static bool PartyAlreadyHasUniqueItem(void* party, int id, bool& checkAvailable) {
    checkAvailable = false;
    if (!party) return false;
    void* klass = il.object_get_class(party);
    // PartyState exposes the count of an item by its game ID. This uses the
    // game's own inventory model, so the check agrees with natural rewards.
    void* countMethod = FindMethodTyped(klass, "GetItemCount", {T_I4});
    if (countMethod) {
        void* args[] = {&id}; void* exception = nullptr;
        void* countBox = il.runtime_invoke(countMethod, party, args, &exception);
        if (!exception && countBox) {
            checkAvailable = true;
            return *(int32_t*)il.object_unbox(countBox) > 0;
        }
    }
    // Some game builds expose the inventory lookup itself instead of the
    // count accessor. A non-null result means the party already owns the ID.
    void* itemMethod = FindMethodTyped(klass, "GetItem", {T_I4});
    if (itemMethod) {
        void* args[] = {&id}; void* exception = nullptr;
        void* item = il.runtime_invoke(itemMethod, party, args, &exception);
        if (!exception) {
            checkAvailable = true;
            return item != nullptr;
        }
    }
    return false;
}

// ---------------- Menu window (tabs = categories) ----------------
enum { ID_TAB = 99, ID_SUBTAB = 98, ID_LIST = 100, ID_EDIT0 = 101, ID_LAB0 = 111, ID_DESC = 120, ID_RUN = 130, ID_STATUS = 131, ID_UNLOAD = 140, ID_HELP = 142, ID_SETTINGS = 143,
       ID_SEARCH = 150, ID_SBTN = 151, ID_RESULTS = 152, ID_SLABEL = 153, ID_RESULTS_SCROLL = 154, ID_LIST_SCROLL = 155, MAXARGS = 3 };
static HWND g_wnd, g_tab, g_subtab, g_list, g_listScroll, g_edit[MAXARGS], g_lab[MAXARGS], g_desc, g_descScroll, g_status, g_search, g_results, g_resultsScroll, g_slabel, g_sbtn, g_help, g_tip;
enum class ScrollContent { Description, FeatureList, SearchResults };
struct SlimScrollState { ScrollContent content; HWND bar = nullptr; int thumbTop = 0, thumbHeight = 0, maxTop = 0, trackHeight = 0, dragOffset = 0; bool dragging = false, hover = false; };
static SlimScrollState g_descScrollState{ScrollContent::Description}, g_listScrollState{ScrollContent::FeatureList}, g_resultsScrollState{ScrollContent::SearchResults};
static std::vector<std::wstring> g_cats;     // tab order
static std::vector<int> g_rows;              // list row -> feature index, for the current tab
static int g_curCat = 0, g_statSub = 0, g_xpJobSub = 0;
static const int WIN_W = 600, WIN_H_ITEMS = 780;
static bool g_layoutHasSubtabs = false;
static HBRUSH g_bgBrush, g_panelBrush, g_editBrush;
static HFONT g_font, g_titleFont;
static const COLORREF C_BG = RGB(27, 29, 30), C_PANEL = RGB(38, 40, 41), C_FIELD = RGB(47, 49, 50), C_TEXT = RGB(240, 239, 235), C_MUTED = RGB(166, 165, 160), C_ACCENT = RGB(210, 178, 112);

static std::wstring ActiveCategory() {
    if (g_curCat >= 0 && g_curCat < (int)g_cats.size() && g_cats[g_curCat] == L"Stats")
        return g_statSub == 0 ? L"Stats (Party)" : L"Stats (One)";
    if (g_curCat >= 0 && g_curCat < (int)g_cats.size() && g_cats[g_curCat] == L"XP/Job")
        return g_xpJobSub == 0 ? L"XP" : L"Job";
    return g_curCat >= 0 && g_curCat < (int)g_cats.size() ? g_cats[g_curCat] : L"";
}
static int TabItemWidth(int itemCount, int stripWidth) {
    if (itemCount <= 0) return std::max(1, stripWidth);
    // SysTabControl32 needs room for its own end caps and borders. Sizing
    // items to the full strip width makes native scroll arrows appear after
    // resizing and at some DPI scales.
    constexpr int controlSlack = 32;
    return std::max(1, (stripWidth - controlSlack) / itemCount);
}
static void ApplyLayout(bool subtabs) {
    g_layoutHasSubtabs = subtabs;
    RECT client{}; GetClientRect(g_wnd, &client);
    const int width = client.right, height = client.bottom, inner = std::max(1, width - 35);
    // Keep the category strip flush with the content width, with only a tiny
    // vertical gap between rows. The old 48 px reservation left a conspicuous
    // empty tail after the final tab.
    const int shift = 28 + (subtabs ? 32 : 0);
    if(g_subtab) ShowWindow(g_subtab,subtabs?SW_SHOW:SW_HIDE);
    SetWindowPos(g_tab, nullptr, 12, 42, inner, 30, SWP_NOZORDER);
    const int categoryTabWidth = TabItemWidth((int)g_cats.size(), inner);
    SendMessageW(g_tab, TCM_SETITEMSIZE, 0, MAKELPARAM(categoryTabWidth, 25));
    SetWindowPos(g_subtab, nullptr, 12, 74, inner, 30, SWP_NOZORDER);
    const int subtabWidth = TabItemWidth(2, inner);
    SendMessageW(g_subtab, TCM_SETITEMSIZE, 0, MAKELPARAM(subtabWidth, 25));
    SetWindowPos(GetDlgItem(g_wnd, ID_SETTINGS), nullptr, width - 132, 6, 32, 27, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(g_wnd, ID_HELP), nullptr, width - 92, 6, 32, 27, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(g_wnd, ID_UNLOAD + 1), nullptr, width - 52, 6, 32, 27, SWP_NOZORDER);
    SetWindowPos(g_list, nullptr, 12, 46 + shift, inner - 13, 190, SWP_NOZORDER);
    SetWindowPos(g_listScroll, nullptr, width - 35, 46 + shift, 10, 190, SWP_NOZORDER);
    SetWindowPos(g_desc, nullptr, 12, 244 + shift, std::max(1, width - 50), 58, SWP_NOZORDER);
    SetWindowPos(g_descScroll, nullptr, width - 35, 244 + shift, 10, 58, SWP_NOZORDER);
    for (int i = 0; i < MAXARGS; ++i) {
        SetWindowPos(g_lab[i], nullptr, 12, 312 + shift + i * 34, 195, 24, SWP_NOZORDER);
        SetWindowPos(g_edit[i], nullptr, 212, 309 + shift + i * 34, std::max(1, width - 235), 28, SWP_NOZORDER);
    }
    SetWindowPos(GetDlgItem(g_wnd, ID_RUN), nullptr, 12, 416 + shift, 220, 38, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(g_wnd, ID_UNLOAD), nullptr, width - 243, 416 + shift, 220, 38, SWP_NOZORDER);
    SetWindowPos(g_status, nullptr, 12, 464 + shift, inner, 44, SWP_NOZORDER);
    SetWindowPos(g_slabel, nullptr, 12, 518 + shift, inner, 22, SWP_NOZORDER);
    SetWindowPos(g_search, nullptr, 12, 544 + shift, std::max(1, width - 150), 28, SWP_NOZORDER);
    SetWindowPos(g_sbtn, nullptr, width - 130, 543 + shift, 107, 30, SWP_NOZORDER);
    const int resultHeight = std::max(44, height - (580 + shift) - 20);
    SetWindowPos(g_results, nullptr, 12, 580 + shift, inner - 13, resultHeight, SWP_NOZORDER);
    SetWindowPos(g_resultsScroll, nullptr, width - 35, 580 + shift, 10, resultHeight, SWP_NOZORDER);
}

static void SetText(HWND h, const std::wstring& s) { SetWindowTextW(h, s.c_str()); }
static HWND ScrollContentWindow(const SlimScrollState& state) {
    switch (state.content) {
    case ScrollContent::Description: return g_desc;
    case ScrollContent::FeatureList: return g_list;
    case ScrollContent::SearchResults: return g_results;
    }
    return nullptr;
}
static int ScrollCount(const SlimScrollState& state) {
    HWND content = ScrollContentWindow(state);
    if (!content) return 0;
    return state.content == ScrollContent::Description
        ? (int)SendMessageW(content, EM_GETLINECOUNT, 0, 0)
        : (int)SendMessageW(content, LB_GETCOUNT, 0, 0);
}
static int ScrollPage(const SlimScrollState& state) {
    HWND content = ScrollContentWindow(state);
    if (!content) return 1;
    if (state.content == ScrollContent::Description) return 2;
    RECT rc{}; GetClientRect(content, &rc);
    const int itemHeight = std::max(1, (int)SendMessageW(content, LB_GETITEMHEIGHT, 0, 0));
    return std::max(1, (int)(rc.bottom - rc.top) / itemHeight);
}
static int ScrollTop(const SlimScrollState& state) {
    HWND content = ScrollContentWindow(state);
    if (!content) return 0;
    return state.content == ScrollContent::Description
        ? (int)SendMessageW(content, EM_GETFIRSTVISIBLELINE, 0, 0)
        : (int)SendMessageW(content, LB_GETTOPINDEX, 0, 0);
}
static void UpdateSlimScrollbar(SlimScrollState& state) {
    if (!state.bar) return;
    const int count = ScrollCount(state), page = ScrollPage(state), top = std::max(0, ScrollTop(state));
    RECT rc{}; GetClientRect(state.bar, &rc);
    constexpr int inset = 4;
    state.trackHeight = std::max(0, (int)(rc.bottom - rc.top) - inset * 2);
    state.maxTop = std::max(0, count - page);
    if (count > page && state.trackHeight > 0) {
        state.thumbHeight = std::clamp(state.trackHeight * page / count, 22, state.trackHeight);
        const int travel = std::max(0, state.trackHeight - state.thumbHeight);
        state.thumbTop = inset + (state.maxTop ? travel * std::clamp(top, 0, state.maxTop) / state.maxTop : 0);
        if (!IsWindowVisible(state.bar)) ShowWindow(state.bar, SW_SHOWNA);
    } else {
        state.thumbHeight = 0; state.thumbTop = inset;
        if (IsWindowVisible(state.bar)) ShowWindow(state.bar, SW_HIDE);
    }
    InvalidateRect(state.bar, nullptr, FALSE);
}
static void UpdateDescriptionScroll() { UpdateSlimScrollbar(g_descScrollState); }
static void UpdateFeatureListScroll() { UpdateSlimScrollbar(g_listScrollState); }
static void UpdateResultsScroll() { UpdateSlimScrollbar(g_resultsScrollState); }

static void ScrollContentTo(SlimScrollState& state, int position) {
    HWND content = ScrollContentWindow(state);
    if (!content) return;
    const int page = ScrollPage(state), maxTop = std::max(0, ScrollCount(state) - page);
    const int target = std::clamp(position, 0, maxTop), current = ScrollTop(state);
    if (state.content == ScrollContent::Description) SendMessageW(content, EM_LINESCROLL, 0, target - current);
    else SendMessageW(content, LB_SETTOPINDEX, target, 0);
    UpdateSlimScrollbar(state);
}
static void ScrollContentBy(SlimScrollState& state, int command, int trackPosition = 0) {
    const int page = ScrollPage(state), top = ScrollTop(state);
    switch (command) {
    case SB_LINEUP: ScrollContentTo(state, top - 1); break;
    case SB_LINEDOWN: ScrollContentTo(state, top + 1); break;
    case SB_PAGEUP: ScrollContentTo(state, top - page); break;
    case SB_PAGEDOWN: ScrollContentTo(state, top + page); break;
    case SB_THUMBTRACK: case SB_THUMBPOSITION: ScrollContentTo(state, trackPosition); break;
    case SB_TOP: ScrollContentTo(state, 0); break;
    case SB_BOTTOM: ScrollContentTo(state, ScrollCount(state) - page); break;
    default: break;
    }
}
static LRESULT CALLBACK SlimScrollbarProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    constexpr int inset = 4;
    auto* state = (SlimScrollState*)GetWindowLongPtrW(window, GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        auto* create = (CREATESTRUCTW*)lParam; state = (SlimScrollState*)create->lpCreateParams;
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)state); if (state) state->bar = window;
    }
    if (!state) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(window, &ps); RECT rc{}; GetClientRect(window, &rc);
        HBRUSH bg = CreateSolidBrush(C_BG); FillRect(dc, &rc, bg); DeleteObject(bg);
        if (state->thumbHeight > 0) {
            const int x = (rc.right - 4) / 2;
            RECT track{x - 1, inset, x + 2, rc.bottom - inset};
            HBRUSH trackBrush = CreateSolidBrush(RGB(55, 57, 58)); FillRect(dc, &track, trackBrush); DeleteObject(trackBrush);
            const COLORREF color = state->hover ? RGB(175, 163, 139) : RGB(126, 123, 116);
            RECT thumb{x - (state->hover ? 2 : 1), state->thumbTop, x + (state->hover ? 3 : 2), state->thumbTop + state->thumbHeight};
            HBRUSH brush = CreateSolidBrush(color); HPEN pen = CreatePen(PS_SOLID, 1, color);
            HGDIOBJ oldBrush = SelectObject(dc, brush), oldPen = SelectObject(dc, pen);
            RoundRect(dc, thumb.left, thumb.top, thumb.right, thumb.bottom, 5, 5);
            SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(brush); DeleteObject(pen);
        }
        EndPaint(window, &ps); return 0;
    }
    case WM_MOUSEMOVE: {
        if (!state->hover) {
            state->hover = true; TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
            TrackMouseEvent(&tracking); InvalidateRect(window, nullptr, FALSE);
        }
        if (state->dragging && state->maxTop > 0) {
            const int y = GET_Y_LPARAM(lParam) - state->dragOffset;
            const int travel = std::max(1, state->trackHeight - state->thumbHeight);
            const int relative = std::clamp(y - inset, 0, travel);
            ScrollContentBy(*state, SB_THUMBTRACK, relative * state->maxTop / travel);
        }
        return 0;
    }
    case WM_MOUSELEAVE: state->hover = false; InvalidateRect(window, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN: {
        const int y = GET_Y_LPARAM(lParam);
        if (state->thumbHeight <= 0) return 0;
        if (y >= state->thumbTop && y < state->thumbTop + state->thumbHeight) {
            state->dragging = true; state->dragOffset = y - state->thumbTop; SetCapture(window);
        } else ScrollContentBy(*state, y < state->thumbTop ? SB_PAGEUP : SB_PAGEDOWN);
        return 0;
    }
    case WM_LBUTTONUP: if (state->dragging) { state->dragging = false; ReleaseCapture(); } return 0;
    case WM_CAPTURECHANGED: state->dragging = false; return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
static LRESULT CALLBACK ScrollWheelProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                         UINT_PTR subclassId, DWORD_PTR referenceData) {
    auto* state = (SlimScrollState*)referenceData;
    if (message == WM_MOUSEWHEEL && state) {
        const int notches = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
        if (notches) ScrollContentBy(*state, notches > 0 ? SB_LINEUP : SB_LINEDOWN);
        return 0;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, ScrollWheelProc, subclassId);
    return DefSubclassProc(window, message, wParam, lParam);
}

static HWND g_settingsWnd, g_settingEdit[9];
static std::wstring g_hotkeyTooltip;
static HWND g_hotkeyToast;
static UINT_PTR g_hotkeyToastTimer = 0;
static BYTE g_overlayAlpha = 166;
static void RefreshHotkeyTooltip();
static constexpr int HOTKEY_BINDING_COUNT = 21;
static HWND g_hotkeyCombo[HOTKEY_BINDING_COUNT]{};
static std::wstring g_hotkeyIds[HOTKEY_BINDING_COUNT];
static const wchar_t* g_hotkeyKeys[HOTKEY_BINDING_COUNT] = {
    L"F2",L"F3",L"F4",L"F5",L"F6",L"F7",L"F8",L"F9",L"F10",L"F11",L"F12",
    L"NumPad0",L"NumPad1",L"NumPad2",L"NumPad3",L"NumPad4",L"NumPad5",L"NumPad6",L"NumPad7",L"NumPad8",L"NumPad9"
};
static const wchar_t* g_hotkeyDefaultIds[HOTKEY_BINDING_COUNT] = {
    L"cure_all_status",L"max_hp",L"max_mp",L"infinite_hp",L"infinite_mp",L"battle_infinite_bp",L"battle_steal_100",
    L"xp_multiplier",L"jp_multiplier",L"xp_jp_multiplier",L"restore_rates",
    L"",L"",L"",L"",L"",L"",L"",L"",L"",L""
};
static std::wstring GetText(HWND h);
static const wchar_t* g_settingSection[] = {L"Overlay",L"ItemDefaults",L"ItemDefaults",L"ItemDefaults",L"ItemDefaults",L"friend_add_com_npc",L"friend_add_com_npc",L"xp_multiplier",L"jp_multiplier"};
static const wchar_t* g_settingKey[] = {L"transparency_percent",L"consumables",L"weapons",L"armor",L"accessories",L"default_friendship",L"default_population",L"arg_defaults",L"arg_defaults"};
static const wchar_t* g_settingLabel[] = {L"Transparency percent",L"Consumable quantity",L"Weapon quantity",L"Armor quantity",L"Accessory quantity",L"New friend friendship",L"New friend population",L"XP multiplier",L"JP multiplier"};
static std::wstring ReadIniSetting(const wchar_t* section,const wchar_t* key,const wchar_t* fallback) {
    for(const auto& item:LoadIni(g_dir+L"\\features.ini")) if(item.name==section) return Get(item,key,fallback);
    return fallback;
}
static bool WriteIniSetting(const wchar_t* section,const wchar_t* key,const std::wstring& value) {
    std::wstring path=g_dir+L"\\features.ini";std::ifstream file(path,std::ios::binary);if(!file)return false;
    std::string bytes((std::istreambuf_iterator<char>(file)),{});std::wstring text=Widen(bytes);
    if(text.size()&&text[0]==0xFEFF)text.erase(0,1);
    std::vector<std::wstring> lines;std::wistringstream input(text);std::wstring line;while(std::getline(input,line)){if(!line.empty()&&line.back()==L'\r')line.pop_back();lines.push_back(line);}
    std::wstring current;bool inserted=false,foundSection=false;
    for(size_t i=0;i<lines.size();++i){std::wstring t=lines[i];size_t a=t.find_first_not_of(L" \t"),b=t.find_last_not_of(L" \t");if(a!=std::wstring::npos&&t[a]==L'['&&b!=std::wstring::npos&&t[b]==L']'){
            if(current==section&&!inserted){lines.insert(lines.begin()+i,L""+std::wstring(key)+L"="+value);inserted=true;++i;}
            current=t.substr(a+1,b-a-1);if(current==section)foundSection=true;continue;}
        if(current==section){size_t eq=t.find(L'=');if(eq!=std::wstring::npos){std::wstring k=t.substr(0,eq);k.erase(0,k.find_first_not_of(L" \t"));size_t end=k.find_last_not_of(L" \t");if(end!=std::wstring::npos)k.resize(end+1);if(k==key){lines[i]=std::wstring(key)+L"="+value;inserted=true;break;}}}
    }
    if(!inserted){if(!foundSection){lines.push_back(L"");lines.push_back(L"["+std::wstring(section)+L"]");}lines.push_back(std::wstring(key)+L"="+value);}
    std::wstring out;for(const auto& l:lines)out+=l+L"\r\n";std::string utf8=Narrow(out);std::ofstream dst(path,std::ios::binary|std::ios::trunc);dst.write(utf8.data(),utf8.size());return dst.good();
}
static void LoadHotkeyBindings() {
    for (int i=0;i<HOTKEY_BINDING_COUNT;++i)
        g_hotkeyIds[i]=ReadIniSetting(L"Hotkeys",g_hotkeyKeys[i],g_hotkeyDefaultIds[i]);
}
static void SelectHotkeyCombo(HWND combo, const std::wstring& id) {
    int count=(int)SendMessageW(combo,CB_GETCOUNT,0,0);
    for(int i=0;i<count;++i){
        LRESULT data=SendMessageW(combo,CB_GETITEMDATA,i,0);
        if(data==CB_ERR)continue;
        int feature=(int)data;
        if(feature<0 ? id.empty() : (feature<(int)g_features.size()&&g_features[feature].id==id)){
            SendMessageW(combo,CB_SETCURSEL,i,0);return;
        }
    }
    SendMessageW(combo,CB_SETCURSEL,0,0);
}
static LRESULT CALLBACK SettingsProc(HWND h,UINT m,WPARAM w,LPARAM l) {
    if (m == WM_NCHITTEST) {
        POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ScreenToClient(h, &point);
        if (point.y >= 0 && point.y < 40 && point.x >= 0 && point.x < 548) return HTCAPTION;
    }
    if(m==WM_COMMAND&&LOWORD(w)==IDOK){
        bool ok=true;for(int i=0;i<9;++i){wchar_t v[128]{};GetWindowTextW(g_settingEdit[i],v,128);if(wcslen(v)==0)ok=false;else ok=WriteIniSetting(g_settingSection[i],g_settingKey[i],v)&&ok;}
        for(int i=0;i<HOTKEY_BINDING_COUNT;++i){
            LRESULT row=SendMessageW(g_hotkeyCombo[i],CB_GETCURSEL,0,0);
            LRESULT value=row==CB_ERR?-1:SendMessageW(g_hotkeyCombo[i],CB_GETITEMDATA,row,0);
            std::wstring id=(value>=0&&value<(LRESULT)g_features.size())?g_features[(size_t)value].id:L"";
            ok=WriteIniSetting(L"Hotkeys",g_hotkeyKeys[i],id)&&ok;
            g_hotkeyIds[i]=id;
        }
        if(!ok){MessageBoxW(h,L"One or more values could not be saved. Check the values and try again.",L"Settings",MB_ICONERROR);return 0;}
        wchar_t value[128]{};GetWindowTextW(g_settingEdit[0],value,128);int tr=std::clamp(_wtoi(value),0,100);g_overlayAlpha=(BYTE)((255*(100-tr)+50)/100);SetLayeredWindowAttributes(g_wnd,0,g_overlayAlpha,LWA_ALPHA);
        for(auto& f:g_features){if(f.id==L"xp_multiplier"||f.id==L"jp_multiplier"){size_t i=f.id==L"xp_multiplier"?7:8;if(!f.argDefaults.empty())f.argDefaults[0]=GetText(g_settingEdit[i]);if(!f.argValues.empty())f.argValues[0]=f.argDefaults[0];}}
        for(auto& f:g_features) if(f.id==L"xp_jp_multiplier"&&f.argDefaults.size()>=2&&f.argValues.size()>=2){f.argDefaults[0]=GetText(g_settingEdit[7]);f.argDefaults[1]=GetText(g_settingEdit[8]);f.argValues[0]=f.argDefaults[0];f.argValues[1]=f.argDefaults[1];}
        RefreshHotkeyTooltip();
        SetText(g_status,L"Settings saved to features.ini.");EnableWindow(g_wnd,TRUE);DestroyWindow(h);g_settingsWnd=nullptr;return 0;
    }
    if(m==WM_COMMAND&&LOWORD(w)==IDCANCEL){EnableWindow(g_wnd,TRUE);DestroyWindow(h);g_settingsWnd=nullptr;return 0;}
    if(m==WM_CLOSE){EnableWindow(g_wnd,TRUE);DestroyWindow(h);g_settingsWnd=nullptr;return 0;}
    if(m==WM_CTLCOLORSTATIC){SetTextColor((HDC)w,C_TEXT);SetBkColor((HDC)w,C_BG);return (LRESULT)g_bgBrush;}
    if(m==WM_CTLCOLORBTN){SetTextColor((HDC)w,C_TEXT);SetBkColor((HDC)w,C_PANEL);return (LRESULT)g_panelBrush;}
    if(m==WM_CTLCOLOREDIT||m==WM_CTLCOLORLISTBOX){SetTextColor((HDC)w,C_TEXT);SetBkColor((HDC)w,C_FIELD);return (LRESULT)g_editBrush;}
    return DefWindowProcW(h,m,w,l);
}
static std::wstring GetText(HWND h){int n=GetWindowTextLengthW(h);std::wstring t(n+1,L'\0');if(n)GetWindowTextW(h,t.data(),n+1);t.resize(n);return t;}
static void OpenSettings(){if(g_settingsWnd){SetForegroundWindow(g_settingsWnd);return;}WNDCLASSW wc{};wc.lpfnWndProc=SettingsProc;wc.hInstance=g_self;wc.lpszClassName=L"BDFFHDSettings";wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.hbrBackground=g_bgBrush;RegisterClassW(&wc);
    constexpr int settingsWidth=600, settingsHeight=860;
    g_settingsWnd=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_LAYERED,wc.lpszClassName,L"Trainer settings",WS_POPUP,0,0,settingsWidth,settingsHeight,g_wnd,nullptr,g_self,nullptr);
    int transparency=std::clamp((int)GetPrivateProfileIntW(L"Overlay",L"transparency_percent",35,(g_dir+L"\\features.ini").c_str()),0,100);
    SetLayeredWindowAttributes(g_settingsWnd,0,(BYTE)((255*(100-transparency)+50)/100),LWA_ALPHA);
    SetWindowRgn(g_settingsWnd,CreateRoundRectRgn(0,0,settingsWidth,settingsHeight,18,18),TRUE);
    BOOL dark=TRUE;DwmSetWindowAttribute(g_settingsWnd,20,&dark,sizeof(dark));const COLORREF noBorder=0xFFFFFFFE;DwmSetWindowAttribute(g_settingsWnd,34,&noBorder,sizeof(noBorder));
    CreateWindowW(L"STATIC",L"Settings",WS_CHILD|WS_VISIBLE,18,10,200,28,g_settingsWnd,nullptr,g_self,nullptr);
    CreateWindowW(L"BUTTON",L"×",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,558,8,30,28,g_settingsWnd,(HMENU)IDCANCEL,g_self,nullptr);
    for(int i=0;i<9;++i){int y=50+i*34;CreateWindowW(L"STATIC",g_settingLabel[i],WS_CHILD|WS_VISIBLE,18,y+2,190,24,g_settingsWnd,nullptr,g_self,nullptr);g_settingEdit[i]=CreateWindowW(L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL,214,y,350,27,g_settingsWnd,(HMENU)(INT_PTR)(160+i),g_self,nullptr);SetText(g_settingEdit[i],ReadIniSetting(g_settingSection[i],g_settingKey[i],i==0?L"35":i==1?L"10":(i>=5&&i<=6)?(i==5?L"255":L"999"):i==7?L"2":i==8?L"5":L"4"));}
    CreateWindowW(L"STATIC",L"Hotkey assignments",WS_CHILD|WS_VISIBLE,18,364,510,26,g_settingsWnd,nullptr,g_self,nullptr);
    CreateWindowW(L"STATIC",L"Function keys",WS_CHILD|WS_VISIBLE,18,390,250,22,g_settingsWnd,nullptr,g_self,nullptr);
    CreateWindowW(L"STATIC",L"Numpad keys",WS_CHILD|WS_VISIBLE,306,390,250,22,g_settingsWnd,nullptr,g_self,nullptr);
    LoadHotkeyBindings();
    for(int i=0;i<HOTKEY_BINDING_COUNT;++i){
        const bool right=i>=11; int row=right?i-11:i; int x=right?306:18; int y=416+row*32;
        const int labelWidth=right?76:70;
        CreateWindowW(L"STATIC",g_hotkeyKeys[i],WS_CHILD|WS_VISIBLE, x,y+3,labelWidth,24,g_settingsWnd,nullptr,g_self,nullptr);
        g_hotkeyCombo[i]=CreateWindowW(L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|CBS_DROPDOWNLIST|CBS_SORT,x+labelWidth,y,190,240,g_settingsWnd,(HMENU)(INT_PTR)(220+i),g_self,nullptr);
        LRESULT disabled=SendMessageW(g_hotkeyCombo[i],CB_ADDSTRING,0,(LPARAM)L"(unassigned)"); SendMessageW(g_hotkeyCombo[i],CB_SETITEMDATA,disabled,(LPARAM)-1);
        for(size_t n=0;n<g_features.size();++n){
            if(!g_features[n].ok)continue;
            LRESULT item=SendMessageW(g_hotkeyCombo[i],CB_ADDSTRING,0,(LPARAM)g_features[n].label.c_str());
            if(item!=CB_ERR&&item!=CB_ERRSPACE)SendMessageW(g_hotkeyCombo[i],CB_SETITEMDATA,item,(LPARAM)n);
        }
        SelectHotkeyCombo(g_hotkeyCombo[i],g_hotkeyIds[i]);
    }
    CreateWindowW(L"BUTTON",L"Save",WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,392,800,90,32,g_settingsWnd,(HMENU)IDOK,g_self,nullptr);CreateWindowW(L"BUTTON",L"Cancel",WS_CHILD|WS_VISIBLE,492,800,90,32,g_settingsWnd,(HMENU)IDCANCEL,g_self,nullptr);
    EnumChildWindows(g_settingsWnd,[](HWND c,LPARAM font)->BOOL{SendMessageW(c,WM_SETFONT,(WPARAM)font,TRUE);return TRUE;},(LPARAM)g_font);
    EnableWindow(g_wnd,FALSE);ShowWindow(g_settingsWnd,SW_SHOW);SetForegroundWindow(g_settingsWnd);
}
static void SaveOverlayPlacement() {
    if (!g_wnd || !IsWindow(g_wnd)) return;
    RECT r{}; if (!GetWindowRect(g_wnd, &r)) return;
    WriteIniSetting(L"Overlay", L"window_x", std::to_wstring(r.left));
    WriteIniSetting(L"Overlay", L"window_y", std::to_wstring(r.top));
    WriteIniSetting(L"Overlay", L"window_width", std::to_wstring(r.right-r.left));
    WriteIniSetting(L"Overlay", L"window_height", std::to_wstring(r.bottom-r.top));
}

static std::wstring LabelFor(const Feature& f) {
    return f.label + (f.ok ? (IsToggle(f) ? (f.active ? L"      [ ON ]" : L"      [ off ]") : L"") : L"      (unavailable)");
}
static int SelFeature() {
    int r = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
    return (r >= 0 && r < (int)g_rows.size()) ? g_rows[r] : -1;
}
static void RefreshList() {
    if (g_cats.empty()) return;
    int keep = SelFeature();
    int top = (int)SendMessageW(g_list, LB_GETTOPINDEX, 0, 0);
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0); g_rows.clear();
    for (size_t i = 0; i < g_features.size(); ++i) {
        if (g_features[i].category != ActiveCategory()) continue;
        g_rows.push_back((int)i);
        SendMessageW(g_list, LB_ADDSTRING, 0, (LPARAM)LabelFor(g_features[i]).c_str());
    }
    for (size_t r = 0; r < g_rows.size(); ++r) if (g_rows[r] == keep) SendMessageW(g_list, LB_SETCURSEL, r, 0);
    if (top >= 0) SendMessageW(g_list, LB_SETTOPINDEX, top, 0);
    UpdateFeatureListScroll();
}

static int g_shown = -1;     // entry whose values are currently in the input boxes
static void SaveInputs() {   // remember what was typed for the entry being shown, so it comes back when you return to it
    if (g_shown < 0 || g_shown >= (int)g_features.size()) return;
    Feature& f = g_features[g_shown];
    for (size_t i = 0; i < f.argTypes.size() && i < MAXARGS && i < f.argValues.size(); ++i) {
        wchar_t b[256]; GetWindowTextW(g_edit[i], b, 256); f.argValues[i] = b;
    }
}

static void OnSelect() {
    SaveInputs();                                    // store the entry we are leaving
    for (int i = 0; i < MAXARGS; ++i) { ShowWindow(g_edit[i], SW_HIDE); ShowWindow(g_lab[i], SW_HIDE); }
    int sel = SelFeature();
    g_shown = sel;
    if (sel < 0) { SetText(g_desc, L""); UpdateDescriptionScroll(); SetText(g_status, L"Pick an option from the list."); return; }
    Feature& f = g_features[sel];
    for (size_t i = 0; i < f.argTypes.size() && i < MAXARGS; ++i) {
        SetText(g_lab[i], (i < f.argLabels.size() ? f.argLabels[i] : L"Value " + std::to_wstring(i + 1)) + L":");
        SetText(g_edit[i], i < f.argValues.size() ? f.argValues[i] : L"");   // this entry's own values, never another entry's
        ShowWindow(g_edit[i], SW_SHOW); ShowWindow(g_lab[i], SW_SHOW);
    }
    std::wstring d = f.notes;
    if (f.id == L"friend_add_com_npc") {
        int n = f.argValues.empty() ? 1 : std::clamp(_wtoi(f.argValues[0].c_str()),1,32);
        static const wchar_t* names[] = {L"Friend-bot",L"Buddy-bot",L"Pal-bot",L"Amigo-bot"};
        d = L"Template " + std::to_wstring(n) + L" — " + names[(n-1)%4] + L", built-in level band " + std::to_wstring((n-1)/4+1) + L" of 8. The game has 32 supported guest entries across four friend types. " + f.notes;
    }
    if (IsToggle(f)) {
        d += L"\r\nState: ";
        d += f.active ? L"ON" : L"OFF";
        if (f.active && f.waitingForState) d += L" (waiting for battle)";
        d += L". Press Run / Toggle to change it.";
    } else {
        d += f.usedSinceLaunch ? L"\r\nState: Run since this menu was launched." : L"\r\nState: Not run since this menu was launched.";
    }
    SetText(g_desc, f.ok ? d : (L"This option isn't available with the current game version or state.\r\n" + d));
    SendMessageW(g_desc, EM_SETSEL, 0, 0);
    UpdateDescriptionScroll();
    if (!f.ok) SetText(g_status, L"This option isn't available right now.");
    else if (IsToggle(f)) {
        std::wstring state = f.label + (f.active ? L" is ON" : L" is OFF");
        if (f.active && f.waitingForState) state += L" - waiting for battle";
        SetText(g_status, state);
    } else SetText(g_status, L"Enter the value(s) above, then choose Run.");
}

static void SelectCategory(int c) {
    if (c < 0 || c >= (int)g_cats.size()) return;
    g_curCat = c;
    bool stats = (g_cats[c] == L"Stats");
    bool xpjob = (g_cats[c] == L"XP/Job");
    bool items = (g_cats[c] == L"Items");
    if(stats || xpjob) {
        SendMessageW(g_subtab, TCM_DELETEALLITEMS, 0, 0);
        const wchar_t* labels[2] = {stats ? L"Party-wide" : L"XP", stats ? L"Individual" : L"Job"};
        for (int i=0;i<2;++i) { TCITEMW ti{}; ti.mask=TCIF_TEXT; ti.pszText=(LPWSTR)labels[i]; SendMessageW(g_subtab,TCM_INSERTITEMW,i,(LPARAM)&ti); }
        SendMessageW(g_subtab, TCM_SETCURSEL, stats?g_statSub:g_xpJobSub, 0);
    }
    SendMessageW(g_list, LB_SETCURSEL, (WPARAM)-1, 0);
    RefreshList();
    if (!g_rows.empty()) SendMessageW(g_list, LB_SETCURSEL, 0, 0);
    OnSelect();
    int show = items ? SW_SHOW : SW_HIDE;
    ShowWindow(g_slabel, show); ShowWindow(g_search, show); ShowWindow(g_sbtn, show); ShowWindow(g_results, show);
    ApplyLayout(stats || xpjob);
    UpdateResultsScroll();
    if (!items) ShowWindow(g_resultsScroll, SW_HIDE);
    UpdateFeatureListScroll();
    RedrawWindow(g_wnd,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN|RDW_UPDATENOW);
}

static void OnResultPick();

static void DoSearch() {
    wchar_t q[128]; GetWindowTextW(g_search, q, 128);
    std::wstring err = LoadItems();                         // first call reads the item table (about a second)
    if (!err.empty()) { SetText(g_status, err); return; }
    std::wstring needle = Folded(q);
    std::vector<std::pair<int, size_t>> hits;               // (rank, index): 0 = name starts with it, 1 = name contains it, 2 = ID matches
    for (size_t i = 0; i < g_items.size(); ++i) {
        const std::wstring& nm = g_items[i].folded;
        int rank = needle.empty() ? 1 : 9;
        if (!needle.empty()) {
            size_t p = nm.find(needle);
            if (p == 0) rank = 0; else if (p != std::wstring::npos) rank = 1;
            else if (std::to_wstring(g_items[i].id).find(needle) != std::wstring::npos) rank = 2;
            else if (ItemKindMatches(g_items[i].type, g_items[i].name, needle)) rank = 3;
        }
        if (rank < 9) hits.push_back({rank, i});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const std::pair<int, size_t>& x, const std::pair<int, size_t>& y) { return x.first < y.first; });
    SendMessageW(g_results, LB_RESETCONTENT, 0, 0);
    int shown = 0;
    for (auto& h : hits) {
        const ItemEntry& it = g_items[h.second];
        const wchar_t* kind = ItemKind(it.type, it.name);
        std::wstring row = std::to_wstring(it.id) + L"   " + (it.name.empty() ? L"(no name)" : it.name) + L" [" + kind + L"]";
        LRESULT index = SendMessageW(g_results, LB_ADDSTRING, 0, (LPARAM)row.c_str());
        if (index != LB_ERR && index != LB_ERRSPACE)
            SendMessageW(g_results, LB_SETITEMDATA, (WPARAM)index, (LPARAM)it.id);
        ++shown;
    }
    if (shown > 0) SetText(g_status, std::to_wstring(shown) + L" matching item(s). Choose one from the scrollable results list.");
    else SetText(g_status, L"No items match.");
    UpdateResultsScroll();
}

static void ApplyItemQuantityDefault(int id) {
    int current = SelFeature();
    if (current < 0 || g_features[current].id.rfind(L"add_item", 0) != 0 || g_features[current].argTypes.size() < 2) return;
    if (!LoadItems().empty()) return;
    auto item = std::find_if(g_items.begin(), g_items.end(), [id](const ItemEntry& entry) { return entry.id == id; });
    if (item == g_items.end()) return;
    int type = item->type;
    const wchar_t* key = L"other"; int fallback = 10;
    if (type >= 1 && type <= 9) { key = L"weapons"; fallback = 4; }
    else if ((type >= 10 && type <= 14) || type == 19) { key = L"armor"; fallback = 4; }
    else if (type == 15) { key = L"accessories"; fallback = 4; }
    else if (type == 16) key = L"consumables";
    const bool uniqueItem = IsProtectedUniqueItem(id, type);
    if (uniqueItem) { key = L"other"; fallback = 1; }
    int quantity = uniqueItem ? 1 : std::clamp((int)GetPrivateProfileIntW(L"ItemDefaults", key, fallback, (g_dir + L"\\features.ini").c_str()), 0, 99);
    SetText(g_edit[1], std::to_wstring(quantity));
    SaveInputs();
}

static void OnResultPick() {
    int sel = (int)SendMessageW(g_results, LB_GETCURSEL, 0, 0);
    if (sel < 0) return;
    LRESULT itemData = SendMessageW(g_results, LB_GETITEMDATA, sel, 0);
    if (itemData == LB_ERR) return;
    int idValue = (int)(INT_PTR)itemData;
    wchar_t row[300]; SendMessageW(g_results, LB_GETTEXT, sel, (LPARAM)row);
    std::wstring id = std::to_wstring(idValue);
    int cur = SelFeature();
    if (cur < 0 || g_features[cur].id.rfind(L"add_item", 0) != 0) {       // jump to 'Add item' so the ID has somewhere to go
        for (size_t r = 0; r < g_rows.size(); ++r)
            if (g_features[g_rows[r]].id == L"add_item") { SendMessageW(g_list, LB_SETCURSEL, r, 0); OnSelect(); break; }
    }
    SetText(g_edit[0], id);
    ApplyItemQuantityDefault(idValue);
    SaveInputs();
    SetText(g_status, L"Selected: " + std::wstring(row) + L"   -   Up/Down to change, Enter to set the quantity.");
    UpdateResultsScroll();
}

static WNDPROC g_oldEdit;
static LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN) {
        if (h == g_search && (w == VK_DOWN || w == VK_UP)) {
            int n = (int)SendMessageW(g_results, LB_GETCOUNT, 0, 0);
            if (n > 0) {
                int cur = (int)SendMessageW(g_results, LB_GETCURSEL, 0, 0);
                cur = cur < 0 ? 0 : cur + (w == VK_DOWN ? 1 : -1);
                cur = cur < 0 ? 0 : (cur >= n ? n - 1 : cur);
                SendMessageW(g_results, LB_SETCURSEL, cur, 0); OnResultPick();
            }
            return 0;
        }
        if (w == VK_RETURN) {
            if (h == g_search) {                              // confirm the item, then jump to the quantity box
                if (SendMessageW(g_results, LB_GETCURSEL, 0, 0) < 0 && SendMessageW(g_results, LB_GETCOUNT, 0, 0) > 0) { SendMessageW(g_results, LB_SETCURSEL, 0, 0); OnResultPick(); }
                HWND qty = IsWindowVisible(g_edit[1]) ? g_edit[1] : g_edit[0];
                SetFocus(qty); SendMessageW(qty, EM_SETSEL, 0, -1);
            } else PostMessageW(g_wnd, WM_COMMAND, MAKEWPARAM(ID_RUN, BN_CLICKED), 0);   // Enter in an input box = Run
            return 0;
        }
    }
    if (m == WM_CHAR && w == VK_RETURN) return 0;            // no "ding"
    return CallWindowProcW(g_oldEdit, h, m, w, l);
}

static Feature* FindFeature(const wchar_t* id) {
    for(auto& f:g_features) if(f.id==id) return &f;
    return nullptr;
}
static LRESULT CALLBACK HotkeyToastProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_TIMER && wParam == g_hotkeyToastTimer) {
        ShowWindow(window, SW_HIDE); KillTimer(window, g_hotkeyToastTimer); g_hotkeyToastTimer = 0; return 0;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint);
        RECT bounds{}; GetClientRect(window, &bounds);
        HBRUSH brush = CreateSolidBrush(C_BG); FillRect(dc, &bounds, brush); DeleteObject(brush);
        RECT accent{0, 0, 3, bounds.bottom}; brush = CreateSolidBrush(C_ACCENT); FillRect(dc, &accent, brush); DeleteObject(brush);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, C_TEXT);
        HFONT font = CreateFontW(18,0,0,0,FW_MEDIUM,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
        HGDIOBJ old = SelectObject(dc, font);
        RECT text = bounds; InflateRect(&text, -16, -10);
        DrawTextW(dc, GetText(window).c_str(), -1, &text, DT_LEFT|DT_VCENTER|DT_WORDBREAK|DT_END_ELLIPSIS);
        SelectObject(dc, old); DeleteObject(font); EndPaint(window, &paint); return 0;
    }
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(window, message, wParam, lParam);
}
static void ShowHotkeyToast(const std::wstring& text) {
    if (IsWindowVisible(g_wnd)) return;
    if (!g_hotkeyToast) {
        WNDCLASSW wc{}; wc.lpfnWndProc = HotkeyToastProc; wc.hInstance = g_self; wc.lpszClassName = L"BDFFHDHotkeyToast";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW); RegisterClassW(&wc);
        g_hotkeyToast = CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_LAYERED,
            wc.lpszClassName, L"", WS_POPUP, 0, 0, 350, 72, nullptr, nullptr, g_self, nullptr);
        if (!g_hotkeyToast) return;
        SetWindowRgn(g_hotkeyToast, CreateRoundRectRgn(0, 0, 350, 72, 14, 14), TRUE);
    }
    SetWindowTextW(g_hotkeyToast, text.c_str());
    InvalidateRect(g_hotkeyToast, nullptr, TRUE);
    UpdateWindow(g_hotkeyToast);
    SetLayeredWindowAttributes(g_hotkeyToast, 0, g_overlayAlpha, LWA_ALPHA);
    POINT cursor{}; GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)}; GetMonitorInfoW(monitor, &info);
    const int x = info.rcWork.left + ((info.rcWork.right - info.rcWork.left) - 350) / 2;
    const int y = info.rcWork.top + 28;
    SetWindowPos(g_hotkeyToast, HWND_TOPMOST, x, y, 350, 72, SWP_NOACTIVATE|SWP_SHOWWINDOW);
    ShowWindow(g_hotkeyToast, SW_SHOWNOACTIVATE);
    if (g_hotkeyToastTimer) KillTimer(g_hotkeyToast, g_hotkeyToastTimer);
    // Give each display a new timer ID. A queued WM_TIMER from the previous
    // toast can then never hide this newly refreshed message early.
    ++g_hotkeyToastTimer;
    if (!g_hotkeyToastTimer) ++g_hotkeyToastTimer;
    SetTimer(g_hotkeyToast, g_hotkeyToastTimer, 1800, nullptr);
}
static void RunHotkeyFeature(const wchar_t* id) {
    Feature* f=FindFeature(id);
    if(!f||!f->ok){SetText(g_status,L"Hotkey feature unavailable.");ShowHotkeyToast(L"Hotkey option is unavailable.");return;}
    SaveInputs();
    if(IsToggle(*f)){
        f->active=!f->active; f->lastTick=0; f->waitingForState=false;
        ApplyConstantState(*f);
        SetText(g_status,f->label+(f->active?L" is now ON":L" is now OFF"));
        RefreshList();
        if (SelFeature() >= 0 && &g_features[SelFeature()] == f) OnSelect();
        std::wstring state=f->label+(f->active?L" is now ON":L" is now OFF");
        SetText(g_status,state); ShowHotkeyToast(state);
        return;
    }
    std::wstring result=Execute(*f,f->argValues);
    f->usedSinceLaunch=true;
    if (SelFeature() >= 0 && &g_features[SelFeature()] == f) OnSelect();
    SetText(g_status,result);
    std::wstring toast = result;
    if (id == std::wstring(L"xp_multiplier"))
        toast = L"XP multiplier activated (" + (f->argValues.empty() ? L"current value" : f->argValues[0]) + L"×).";
    else if (id == std::wstring(L"jp_multiplier"))
        toast = L"JP multiplier activated (" + (f->argValues.empty() ? L"current value" : f->argValues[0]) + L"×).";
    else if (id == std::wstring(L"xp_jp_multiplier")) {
        const std::wstring xp = f->argValues.size() > 0 ? f->argValues[0] : L"current";
        const std::wstring jp = f->argValues.size() > 1 ? f->argValues[1] : L"current";
        toast = L"XP and JP multipliers activated (XP " + xp + L"×, JP " + jp + L"×).";
    } else if (id == std::wstring(L"restore_rates")) toast = L"XP and JP rates reset.";
    ShowHotkeyToast(toast);
    Log("hotkey %ls -> %ls",id,result.c_str());
}
static void RefreshHotkeyTooltip() {
    if (!g_tip || !g_help) return;
    g_hotkeyTooltip=L"F1  Show / hide menu";
    for(int i=0;i<HOTKEY_BINDING_COUNT;++i){
        if(g_hotkeyIds[i].empty())continue;
        Feature* feature=FindFeature(g_hotkeyIds[i].c_str());
        if(!feature)continue;
        g_hotkeyTooltip+=L"\n";g_hotkeyTooltip+=g_hotkeyKeys[i];g_hotkeyTooltip+=L"  ";g_hotkeyTooltip+=feature->label;
    }
    g_hotkeyTooltip+=L"\nConfigure assignments in Settings.";
    TOOLINFOW tip{};tip.cbSize=sizeof(tip);tip.uFlags=TTF_IDISHWND|TTF_SUBCLASS;tip.hwnd=g_wnd;tip.uId=(UINT_PTR)g_help;tip.lpszText=g_hotkeyTooltip.data();
    SendMessageW(g_tip,TTM_UPDATETIPTEXTW,0,(LPARAM)&tip);
}

static void ReturnFocusToGame() {
    HWND game = nullptr;
    EnumWindows([](HWND window, LPARAM context) -> BOOL {
        DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
        if (pid != GetCurrentProcessId() || !IsWindowVisible(window)) return TRUE;
        wchar_t name[128]{}; GetClassNameW(window, name, 128);
        if (wcscmp(name, L"UnityWndClass") != 0) return TRUE;
        *(HWND*)context = window;
        return FALSE;
    }, (LPARAM)&game);
    if (game) { SetForegroundWindow(game); BringWindowToTop(game); }
}

static void HideOverlay() {
    ShowWindow(g_wnd, SW_HIDE);
    ReturnFocusToGame();
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) return 1;
    static bool keyWasDown[22]{};
    switch (m) {
    case WM_NCPAINT:
        // Keep the custom layered overlay frameless when it loses activation;
        // resizing remains enabled through the custom WM_NCHITTEST handling.
        return 0;
    case WM_NCACTIVATE:
        return TRUE;
    case WM_NCCALCSIZE:
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = (MINMAXINFO*)l; info->ptMinTrackSize.x = 580; info->ptMinTrackSize.y = 700; return 0;
    }
    case WM_SIZE:
        if (g_tab && g_subtab && g_list) {
            ApplyLayout(g_layoutHasSubtabs);
            UpdateFeatureListScroll(); UpdateDescriptionScroll(); UpdateResultsScroll();
            if (g_curCat < 0 || g_curCat >= (int)g_cats.size() || g_cats[g_curCat] != L"Items") ShowWindow(g_resultsScroll, SW_HIDE);
            RECT wr{}; GetWindowRect(h, &wr); SetWindowRgn(h, CreateRoundRectRgn(0, 0, wr.right-wr.left, wr.bottom-wr.top, 18, 18), TRUE);
            // Clear stale pixels at both old and new child-control positions.
            // Invalidating only the layered parent left resize afterimages
            // until the pointer happened to repaint an individual button.
            RedrawWindow(h, nullptr, nullptr,
                RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        }
        return 0;
    case WM_EXITSIZEMOVE:
        SaveOverlayPlacement(); return 0;
    case WM_TIMER: {
        std::wstring friendUpdate = MaintainComputerFriends();
        if (!friendUpdate.empty()) SetText(g_status, friendUpdate);
        for(int i=0;i<12;++i){
            bool down=(GetAsyncKeyState(VK_F1+i)&0x8000)!=0;
            if(down&&!keyWasDown[i]){
                if(i==0){if(IsWindowVisible(h))HideOverlay();else{ShowWindow(h,SW_SHOWNOACTIVATE);SetWindowPos(h,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);}}
                else if(i-1<HOTKEY_BINDING_COUNT&&!g_hotkeyIds[i-1].empty()) RunHotkeyFeature(g_hotkeyIds[i-1].c_str());
            }
            keyWasDown[i]=down;
        }
        for(int i=0;i<10;++i){
            const int keyIndex=12+i, bindingIndex=11+i;
            bool down=(GetAsyncKeyState(VK_NUMPAD0+i)&0x8000)!=0;
            if(down&&!keyWasDown[keyIndex]&&!g_hotkeyIds[bindingIndex].empty())RunHotkeyFeature(g_hotkeyIds[bindingIndex].c_str());
            keyWasDown[keyIndex]=down;
        }
        DWORD now = GetTickCount();
        int bpTutorialChange = UpdateBpTutorialGuard(now);
        if (bpTutorialChange && SelFeature() >= 0 && g_features[SelFeature()].id == L"battle_infinite_bp") {
            SetText(g_status, bpTutorialChange > 0
                ? L"Infinite BP temporarily paused during the Brave/Default tutorial (still ON)."
                : L"Infinite BP resumed after the Brave/Default tutorial.");
        }
        for (auto& ft : g_features) {
            if (!ft.constant.empty() || !ft.active || now - ft.lastTick < (DWORD)ft.repeatMs) continue;
            ft.lastTick = now;
            std::wstring r = Execute(ft, std::vector<std::wstring>());
            if (r.rfind(L"Done", 0) == 0) {
                if (ft.waitingForState) { ft.waitingForState = false; SetText(g_status, ft.label + L" resumed"); Log("toggle %ls resumed", ft.id.c_str()); if (SelFeature() >= 0 && &g_features[SelFeature()] == &ft) { OnSelect(); SetText(g_status, ft.label + L" resumed"); } }
            } else if (ft.persistUnavailable && (r.find(L"Root object unavailable") != std::wstring::npos || r.find(L"Chain step returned nothing") != std::wstring::npos || r.find(L"Nothing to do") != std::wstring::npos)) {
                if (!ft.waitingForState) { ft.waitingForState = true; SetText(g_status, ft.label + L" waiting for battle (still ON)"); Log("toggle %ls waiting for battle", ft.id.c_str()); if (SelFeature() >= 0 && &g_features[SelFeature()] == &ft) { OnSelect(); SetText(g_status, ft.label + L" waiting for battle (still ON)"); } }
            } else { ft.active = false; SetText(g_status, ft.label + L" stopped: " + r); Log("toggle %ls stopped: %ls", ft.id.c_str(), r.c_str()); RefreshList(); if (SelFeature() >= 0 && &g_features[SelFeature()] == &ft) { OnSelect(); SetText(g_status, ft.label + L" stopped: " + r); } }
        }
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR* nh=(NMHDR*)l;
        if(nh&&nh->hwndFrom==g_tab&&nh->code==TCN_SELCHANGE) SelectCategory((int)SendMessageW(g_tab,TCM_GETCURSEL,0,0));
        else if(nh&&nh->hwndFrom==g_subtab&&nh->code==TCN_SELCHANGE){SaveInputs();int sub=(int)SendMessageW(g_subtab,TCM_GETCURSEL,0,0);if(g_curCat>=0&&g_curCat<(int)g_cats.size()&&g_cats[g_curCat]==L"Stats")g_statSub=sub;else g_xpJobSub=sub;RefreshList();if(!g_rows.empty())SendMessageW(g_list,LB_SETCURSEL,0,0);OnSelect();ApplyLayout(true);RedrawWindow(g_wnd,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN|RDW_UPDATENOW);}
        return 0;
    }
    case WM_CTLCOLORDLG: case WM_CTLCOLORSTATIC: {
        HDC dc=(HDC)w; HWND child=(HWND)l; SetBkMode(dc,TRANSPARENT);
        SetTextColor(dc,child==g_status?C_ACCENT:C_TEXT); return (LRESULT)(child==g_status?g_panelBrush:g_bgBrush);
    }
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: {
        HDC dc=(HDC)w; SetBkColor(dc,C_FIELD); SetTextColor(dc,C_TEXT); return (LRESULT)g_editBrush;
    }
    case WM_CTLCOLORBTN: {
        HDC dc=(HDC)w; SetBkColor(dc,C_PANEL); SetTextColor(dc,C_TEXT); return (LRESULT)g_panelBrush;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{}; HDC dc=BeginPaint(h,&ps); RECT r{}; GetClientRect(h,&r);
        HBRUSH bg=CreateSolidBrush(C_BG); FillRect(dc,&r,bg); DeleteObject(bg);
        RECT head{0,0,r.right,38}; HBRUSH hb=CreateSolidBrush(RGB(25,26,27)); FillRect(dc,&head,hb); DeleteObject(hb);
        RECT accent{12,36,r.right-12,38}; HBRUSH ab=CreateSolidBrush(C_ACCENT); FillRect(dc,&accent,ab); DeleteObject(ab);
        SetBkMode(dc,TRANSPARENT); SetTextColor(dc,C_TEXT); HFONT title=g_titleFont;
        HFONT old=(HFONT)SelectObject(dc,title); RECT tr{16,7,290,29}; DrawTextW(dc,L"BRAVELY DEFAULT HD",-1,&tr,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        SelectObject(dc,old);
        SetTextColor(dc,C_MUTED);
        HFONT hintOld=(HFONT)SelectObject(dc,g_font);
        RECT sr{294,9,r.right-144,28};
        DrawTextW(dc,L"F1 TO HIDE / SHOW",-1,&sr,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
        SelectObject(dc,hintOld);
        EndPaint(h,&ps); return 0;
    }
    case WM_NCHITTEST: {
        POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)}; ScreenToClient(h,&p); RECT r{}; GetClientRect(h,&r);
        constexpr int edge=8;
        const bool left=p.x<edge, right=p.x>=r.right-edge, top=p.y<edge, bottom=p.y>=r.bottom-edge;
        if(top&&left)return HTTOPLEFT;
        if(top&&right)return HTTOPRIGHT;
        if(bottom&&left)return HTBOTTOMLEFT;
        if(bottom&&right)return HTBOTTOMRIGHT;
        if(left)return HTLEFT;
        if(right)return HTRIGHT;
        if(top)return HTTOP;
        if(bottom)return HTBOTTOM;
        if(p.y<36&&p.x<r.right-140)return HTCAPTION;
        return HTCLIENT;
    }
    case WM_COMMAND:
        if (LOWORD(w) == ID_LIST && HIWORD(w) == LBN_SELCHANGE) OnSelect();
        else if (LOWORD(w) == ID_SBTN || (LOWORD(w) == ID_SEARCH && HIWORD(w) == EN_CHANGE)) DoSearch();
        else if (LOWORD(w) == ID_RESULTS && HIWORD(w) == LBN_SELCHANGE) OnResultPick();
        else if (LOWORD(w) == ID_EDIT0 && HIWORD(w) == EN_KILLFOCUS) {
            wchar_t id[32]{}; GetWindowTextW(g_edit[0], id, 32); ApplyItemQuantityDefault(_wtoi(id));
        }
        else if (LOWORD(w) == ID_RUN) {
            int sel = SelFeature();
            if (sel < 0) { SetText(g_status, L"Pick an entry from the list first."); return 0; }
            Feature& ft = g_features[sel];
            std::vector<std::wstring> text;
            for (size_t i = 0; i < ft.argTypes.size() && i < MAXARGS; ++i) {
                wchar_t b[256]; GetWindowTextW(g_edit[i], b, 256); text.push_back(b);
            }
            std::wstring r;
            if (IsToggle(ft) && ft.ok) { ft.active = !ft.active; ft.lastTick = 0; ft.waitingForState = false; ApplyConstantState(ft); r = ft.label + (ft.active ? L" is now ON" : L" is now OFF"); RefreshList(); }
            else { r = Execute(ft, text); ft.usedSinceLaunch = true; }
            Log("run %ls -> %ls", ft.id.c_str(), r.c_str());
            OnSelect();
            SetText(g_status, r);
        } else if (LOWORD(w) == ID_UNLOAD) { SaveOverlayPlacement(); g_unload = true; PostQuitMessage(0); }
        else if (LOWORD(w) == ID_UNLOAD + 1) {
            int choice = MessageBoxW(g_wnd,
                L"Unload the trainer completely?\n\nYes: unload the trainer\nNo: hide the menu and keep the trainer running",
                L"Close BDFFHD Trainer", MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
            if (choice == IDYES) { SaveOverlayPlacement(); g_unload = true; PostQuitMessage(0); }
            else if (choice == IDNO) HideOverlay();
        }
        else if (LOWORD(w) == ID_SETTINGS) OpenSettings();
        return 0;
    case WM_CLOSE: SaveOverlayPlacement(); HideOverlay(); return 0;   // F1 brings it back
    }
    return DefWindowProcW(h, m, w, l);
}

static void BuildMenuWindow() {
    LoadHotkeyBindings();
    INITCOMMONCONTROLSEX icc{sizeof(icc),ICC_TAB_CLASSES|ICC_WIN95_CLASSES}; InitCommonControlsEx(&icc);
    const wchar_t* pref[] = {L"Items",L"Battle",L"XP/Job",L"Stats",L"Norende",L"Friends"};
    for(const wchar_t* p:pref){bool found=false;for(auto& f:g_features)if((std::wstring(p)==L"Stats"&&(f.category==L"Stats (Party)"||f.category==L"Stats (One)"))||(std::wstring(p)==L"XP/Job"&&(f.category==L"XP"||f.category==L"Job"))||f.category==p){found=true;break;}if(found)g_cats.push_back(p);}
    for(auto& f:g_features){bool known=f.category==L"Stats (Party)"||f.category==L"Stats (One)"||f.category==L"XP"||f.category==L"Job"||std::find(g_cats.begin(),g_cats.end(),f.category)!=g_cats.end();if(!known)g_cats.push_back(f.category);}

    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = g_self; wc.lpszClassName = L"BDFFHDMenu";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    g_bgBrush=CreateSolidBrush(C_BG); g_panelBrush=CreateSolidBrush(C_PANEL); g_editBrush=CreateSolidBrush(C_FIELD);
    int windowX=(int)GetPrivateProfileIntW(L"Overlay",L"window_x",40,(g_dir+L"\\features.ini").c_str());
    int windowY=(int)GetPrivateProfileIntW(L"Overlay",L"window_y",40,(g_dir+L"\\features.ini").c_str());
    int windowW=(int)GetPrivateProfileIntW(L"Overlay",L"window_width",WIN_W,(g_dir+L"\\features.ini").c_str());
    int windowH=(int)GetPrivateProfileIntW(L"Overlay",L"window_height",WIN_H_ITEMS+38,(g_dir+L"\\features.ini").c_str());
    POINT anchor{windowX,windowY}; HMONITOR monitor=MonitorFromPoint(anchor,MONITOR_DEFAULTTONULL); MONITORINFO mi{sizeof(mi)}; RECT work{};
    if (monitor && GetMonitorInfoW(monitor, &mi)) work = mi.rcWork;
    else if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0))
        work = {0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN)};
    const int workLeft=static_cast<int>(work.left), workTop=static_cast<int>(work.top);
    const int workRight=static_cast<int>(work.right), workBottom=static_cast<int>(work.bottom);
    windowW=std::clamp(windowW,580,std::max(580,workRight-workLeft)); windowH=std::clamp(windowH,700,std::max(700,workBottom-workTop));
    windowX=std::clamp(windowX,workLeft,std::max(workLeft,workRight-windowW)); windowY=std::clamp(windowY,workTop,std::max(workTop,workBottom-windowH));
    g_wnd = CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_LAYERED,wc.lpszClassName,L"BDFFHD Trainer",
                            WS_POPUP|WS_THICKFRAME|WS_CLIPCHILDREN,windowX,windowY,windowW,windowH,nullptr,nullptr,g_self,nullptr);
    int transparency = std::clamp((int)GetPrivateProfileIntW(L"Overlay", L"transparency_percent", 35, (g_dir + L"\\features.ini").c_str()), 0, 100);
    g_overlayAlpha = (BYTE)((255 * (100 - transparency) + 50) / 100);
    SetLayeredWindowAttributes(g_wnd, 0, g_overlayAlpha, LWA_ALPHA);
    SetWindowRgn(g_wnd,CreateRoundRectRgn(0,0,windowW,windowH,18,18),TRUE);
    BOOL dark=TRUE; DwmSetWindowAttribute(g_wnd,20,&dark,sizeof(dark));
    const COLORREF noDwmBorder = 0xFFFFFFFE; // DWMWA_COLOR_NONE
    DwmSetWindowAttribute(g_wnd,34,&noDwmBorder,sizeof(noDwmBorder)); // DWMWA_BORDER_COLOR
    g_titleFont = CreateFontW(-17,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    HFONT font = g_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

    g_tab=CreateWindowExW(0,L"SysTabControl32",L"",WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS|TCS_FIXEDWIDTH,12,42,565,30,g_wnd,(HMENU)ID_TAB,g_self,nullptr);
    SendMessageW(g_tab,TCM_SETITEMSIZE,0,MAKELPARAM(TabItemWidth((int)g_cats.size(),565),25));
    for(size_t i=0;i<g_cats.size();++i){TCITEMW ti{};ti.mask=TCIF_TEXT;ti.pszText=(LPWSTR)g_cats[i].c_str();SendMessageW(g_tab,TCM_INSERTITEMW,i,(LPARAM)&ti);}
    SetWindowTheme(g_tab,L"Explorer",nullptr);
    g_subtab=CreateWindowExW(0,L"SysTabControl32",L"",WS_CHILD|WS_CLIPSIBLINGS|TCS_FIXEDWIDTH,12,74,565,30,g_wnd,(HMENU)ID_SUBTAB,g_self,nullptr);
    SendMessageW(g_subtab,TCM_SETITEMSIZE,0,MAKELPARAM(TabItemWidth(2,565),25));
    for(const wchar_t* s:{L"Party-wide",L"Individual"}){TCITEMW ti{};ti.mask=TCIF_TEXT;ti.pszText=(LPWSTR)s;SendMessageW(g_subtab,TCM_INSERTITEMW,SendMessageW(g_subtab,TCM_GETITEMCOUNT,0,0),(LPARAM)&ti);}
    SetWindowTheme(g_subtab,L"Explorer",nullptr);
    g_list = CreateWindowW(L"LISTBOX",L"",WS_CHILD|WS_VISIBLE|WS_BORDER|LBS_NOTIFY,12,46,552,190,g_wnd,(HMENU)ID_LIST,g_self,nullptr);
    SetWindowSubclass(g_list, ScrollWheelProc, 1, (DWORD_PTR)&g_listScrollState);
    g_desc = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 12, 244, 550, 58, g_wnd, (HMENU)ID_DESC, g_self, nullptr);
    SetWindowSubclass(g_desc, ScrollWheelProc, 1, (DWORD_PTR)&g_descScrollState);
    SetWindowTheme(g_desc,L"Explorer",nullptr);

    for (int i = 0; i < MAXARGS; ++i) {
        g_lab[i]  = CreateWindowW(L"STATIC", L"", WS_CHILD, 12, 312 + i * 34, 195, 24, g_wnd, (HMENU)(INT_PTR)(ID_LAB0 + i), g_self, nullptr);
        g_edit[i] = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, 212, 309 + i * 34, 365, 26, g_wnd, (HMENU)(INT_PTR)(ID_EDIT0 + i), g_self, nullptr);
    }
    CreateWindowW(L"BUTTON", L"Run / Toggle", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 12, 416, 220, 38, g_wnd, (HMENU)ID_RUN, g_self, nullptr);
    CreateWindowW(L"BUTTON", L"Unload menu", WS_CHILD | WS_VISIBLE, 357, 416, 220, 38, g_wnd, (HMENU)ID_UNLOAD, g_self, nullptr);
    CreateWindowW(L"BUTTON",L"×",WS_CHILD|WS_VISIBLE,548,6,32,27,g_wnd,(HMENU)(ID_UNLOAD+1),g_self,nullptr);
    g_help=CreateWindowW(L"BUTTON",L"?",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,508,6,32,27,g_wnd,(HMENU)ID_HELP,g_self,nullptr);
    CreateWindowW(L"BUTTON",L"⚙",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,468,6,32,27,g_wnd,(HMENU)ID_SETTINGS,g_self,nullptr);
    g_tip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,L"",WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX|TTS_BALLOON,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,g_wnd,nullptr,g_self,nullptr);
    SendMessageW(g_tip,TTM_SETMAXTIPWIDTH,0,270);
    TOOLINFOW tip{};tip.cbSize=sizeof(tip);tip.uFlags=TTF_IDISHWND|TTF_SUBCLASS;tip.hwnd=g_wnd;tip.uId=(UINT_PTR)g_help;
    tip.lpszText=(LPWSTR)L"F1  Show / hide menu";
    SendMessageW(g_tip,TTM_ADDTOOLW,0,(LPARAM)&tip);
    RefreshHotkeyTooltip();
    g_status = CreateWindowW(L"STATIC", L"Ready.", WS_CHILD | WS_VISIBLE | SS_SUNKEN, 12, 464, 565, 44, g_wnd, (HMENU)ID_STATUS, g_self, nullptr);
    g_slabel = CreateWindowW(L"STATIC", L"Item search - start typing a name or ID.  Up/Down = browse,  Enter = pick:", WS_CHILD, 12, 518, 565, 22, g_wnd, (HMENU)ID_SLABEL, g_self, nullptr);
    g_search = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, 12, 544, 450, 28, g_wnd, (HMENU)ID_SEARCH, g_self, nullptr);
    g_sbtn   = CreateWindowW(L"BUTTON", L"Search", WS_CHILD, 470, 543, 107, 30, g_wnd, (HMENU)ID_SBTN, g_self, nullptr);
    g_results = CreateWindowW(L"LISTBOX", L"", WS_CHILD | WS_BORDER | LBS_NOTIFY, 12, 580, 550, 150, g_wnd, (HMENU)ID_RESULTS, g_self, nullptr);
    WNDCLASSW scrollClass{}; scrollClass.lpfnWndProc = SlimScrollbarProc; scrollClass.hInstance = g_self;
    scrollClass.lpszClassName = L"BDFFHDSlimScrollbar"; scrollClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&scrollClass);
    g_listScroll = CreateWindowExW(0, L"BDFFHDSlimScrollbar", L"", WS_CHILD, 565, 46, 10, 190, g_wnd, (HMENU)ID_LIST_SCROLL, g_self, &g_listScrollState);
    g_descScroll = CreateWindowExW(0, L"BDFFHDSlimScrollbar", L"", WS_CHILD, 565, 244, 10, 58, g_wnd, nullptr, g_self, &g_descScrollState);
    g_resultsScroll = CreateWindowExW(0, L"BDFFHDSlimScrollbar", L"", WS_CHILD, 565, 580, 10, 150, g_wnd, (HMENU)ID_RESULTS_SCROLL, g_self, &g_resultsScrollState);
    SetWindowSubclass(g_results, ScrollWheelProc, 1, (DWORD_PTR)&g_resultsScrollState);
    g_oldEdit = (WNDPROC)SetWindowLongPtrW(g_search, GWLP_WNDPROC, (LONG_PTR)EditProc);
    for (int i = 0; i < MAXARGS; ++i) SetWindowLongPtrW(g_edit[i], GWLP_WNDPROC, (LONG_PTR)EditProc);
    EnumChildWindows(g_wnd, [](HWND c, LPARAM f) -> BOOL { SendMessageW(c,WM_SETFONT,(WPARAM)f,TRUE); return TRUE; },(LPARAM)font);
    SendMessageW(g_list, LB_SETITEMHEIGHT, 0, 26); SendMessageW(g_results, LB_SETITEMHEIGHT, 0, 24);
    UpdateFeatureListScroll(); UpdateDescriptionScroll(); UpdateResultsScroll();

    if (g_cats.empty()) SetText(g_status, L"No entries found - check features.ini sits next to the DLL.");
    else {SendMessageW(g_tab,TCM_SETCURSEL,0,0);SelectCategory(0);}
    SetTimer(g_wnd,1,100,nullptr);
    ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
}

static void ExitPayload(DWORD code) {
    if (g_log) { fclose(g_log); g_log = nullptr; }
    FreeLibraryAndExitThread(g_self, code);
}

static DWORD WINAPI MainThread(LPVOID) {
    g_dir = ModuleDir(g_self);
    g_log = _wfopen((g_dir + L"\\payload.log").c_str(), L"w");
    Log("payload loaded");
    HMODULE ga = nullptr;
    for (int i = 0; i < 600 && !(ga = GetModuleHandleW(L"GameAssembly.dll")); ++i) Sleep(200);
    if (!ga || !LoadApi(ga)) { Log("IL2CPP API unavailable - aborting (exports may be stripped in this build)"); ExitPayload(1); return 1; }
    void* domain = nullptr; size_t n = 0;
    for (int i = 0; i < 600; ++i) {   // wait until the managed runtime has loaded its assemblies
        domain = il.domain_get();
        if (domain) { il.domain_get_assemblies(domain, &n); if (n > 5) break; }
        Sleep(200);
    }
    if (!domain || n <= 5) { Log("IL2CPP assemblies did not finish loading"); ExitPayload(1); return 1; }
    g_domain = domain;
    void* attachedThread = il.thread_attach(domain);
    if (!attachedThread) { Log("could not attach trainer thread"); ExitPayload(1); return 1; }
    Log("domain ready, %zu assemblies", n);
    LoadFeatures(domain);
    // Resolve item types before the inventory hook is enabled so its first
    // natural reward does not trigger the item-table scan mid-game.
    const std::wstring itemLoadError = LoadItems();
    if (!itemLoadError.empty()) Log("unique item guard item table unavailable: %ls", itemLoadError.c_str());
    ConfigureConstantFeatures();
    BuildMenuWindow();
    MSG msg;
    while (!g_unload && GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    KillTimer(g_wnd, 1);
    DestroyWindow(g_wnd);
    if (g_tip) DestroyWindow(g_tip);
    DeleteObject(g_font); DeleteObject(g_titleFont);
    DeleteObject(g_bgBrush); DeleteObject(g_panelBrush); DeleteObject(g_editBrush);
    UnregisterClassW(L"BDFFHDSlimScrollbar", g_self);
    UnregisterClassW(L"BDFFHDMenu", g_self);
    ReturnFocusToGame();
    Log("unloading");
    bool hooksStopped = StopConstantHooks();
    il.thread_detach(attachedThread);
    if (!hooksStopped) { if (g_log) { fclose(g_log); g_log = nullptr; } return 1; }
    ExitPayload(0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE m, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { g_self = m; DisableThreadLibraryCalls(m); HANDLE thread = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr); if (thread) CloseHandle(thread); }
    return TRUE;
}

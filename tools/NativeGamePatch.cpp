#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace {
constexpr const char* kSupportedHashes[] = {
    "87F7F501C288FF92B213113C25FD5D993FDB9783797D80971E46D92A9496E5A6", // clean supported game build
    "79E2112DA6E44B0A4DF39A5BBA50C7B8758C5342854CB0D92DA5A87522F71E08", // earlier dialogue patch state
    "F627B245511E4DAF4F9B3CF15C0C2F83AD0C840FDCA0A0CF478D547F1A9819BA", // original local dialogue baseline
    "AD04CC75DAF0B80381627E2DF84DE936760EFF99E901E9ADEF1B6B19AA1E3858"  // current local dialogue build
};
// Exact GameAssembly produced by the already-installed native release. When
// the patcher is rerun on it, refresh the data tables without patching the
// assembly a second time.
constexpr const char* kAlreadyNativePatchedHash = "F44CEAFD44F413C9B8A2EE1C6C3638FB69A354AF236E41492C65D84D84421A4F";
// Same native release after making the Abilink list order match the raw friend
// slot index consumed by the game's link coroutine.
constexpr const char* kAlreadyNativePatchedFriendOrderHash = "6F6415602BE7CADBA7C816734B7784DF0BDF16E1DB3AC1C0455BC6B4A6B83935";
constexpr const char* kAlreadyNativePatchedOldFriendOrderHash = "091315B64A451F38C33A4669DB9E32F58462E66F359466A9BDE96C870E936FDA";
constexpr const char* kAlreadyLimitBreakCharmPatchedHash = "E456258089FE27761E9B8FE2C4A1F3541E0A376E22038B7F4FB8DAA595B41F28";
constexpr const char* kAlreadyCharmAbilitiesPatchedHash = "B5DFA51B1B5BFC1F0D80C941AE7F332777A3C869E3580AB6E5036FA2B1354CC6";
constexpr const char* kAlreadyGraceDraftPatchedHash = "2394826A3A838B129659D245E3B8F5777C921D0BA4BCE2F209D8D467C576D5A3";
constexpr const char* kAlreadyGraceOfGodsPatchedHash = "09A9AAC6B82B8C7533D19A581CBDECF0D2AFB9B1D1C5ECF213C67A077D83D3A5";
// Native release with the high steal-rate override, rebuilt from the supported
// AD04 dialogue baseline. This lets the patcher safely refresh it from backup.
constexpr const char* kAlreadyGuaranteedStealPatchedHash = "3F80A0ED850271C5C493067B652012B4B424C841E537776C54724B7CB5DDAEC2";
// Native 1.2 build with independent Kleptomaniac rolls and the equip-only
// Scouter panel hook, generated from the verified AD04 dialogue baseline.
constexpr const char* kAlreadyScouterKleptomaniacPatchedHash = "B82E3FC0CC2553511CDC2CE135C96920EFD83819C66BAEB32F3AA4642F109A1E";
constexpr const char* kLegacyUnsafeNativePatchedHashes[] = {
    "2582B3E7555DC914D62B673E9B2F3D1FE816F2988AF43CD2934FAFB21D639DE7",
    "C9D1F44EC965B41A82C846FD69C928AF2675A5142FF8E481AD35255C3DB699AF"
};
constexpr uint32_t kHookRva = 0x63E058;
constexpr uint32_t kPreserveRva = 0x63E065;
constexpr uint32_t kLookupRva = 0x666550;
constexpr uint32_t kProperRva = 0x63D970;
constexpr uint32_t kHelperOffset = 0x000;
constexpr uint32_t kLookupOffset = 0x0D0;
constexpr uint32_t kProperOffset = 0x0D5;
constexpr uint32_t kTrampolineOffset = 0x0DA;
constexpr uint32_t kDamageHookRva = 0x76A040;
constexpr uint32_t kDamageStubOffset = 0x120;
constexpr uint32_t kDamageTrampolineOffset = 0x180;
constexpr uint32_t kStealRateHookRva = 0x465550;
constexpr uint32_t kStealRateStubOffset = 0x1A0;
constexpr uint32_t kSupportAbilityHookRva = 0x641360;
constexpr uint32_t kSupportAbilityStubOffset = 0x200;
constexpr uint32_t kSupportAbilityTrampolineOffset = 0x280;
constexpr uint32_t kScouterHookRva = 0x5C897C;
constexpr uint32_t kScouterStubOffset = 0xC00;
constexpr uint32_t kScouterHelperOffset = 0xC40;
constexpr uint32_t kScouterTrampolineOffset = 0xD80;
constexpr uint32_t kGraceGetBpRva = 0x99C480;
constexpr uint32_t kGraceGetBpStubOffset = 0x300;
constexpr uint32_t kGraceGetBpTrampolineOffset = 0x400;
constexpr uint32_t kGraceSetBpRva = 0x99F1B0;
constexpr uint32_t kGraceSetBpStubOffset = 0x500;
constexpr uint32_t kGraceSetBpTrampolineOffset = 0x600;
constexpr uint32_t kGraceCheckCostRva = 0x99DD50;
constexpr uint32_t kGraceCheckCostStubOffset = 0x700;
constexpr uint32_t kGraceCheckCostTrampolineOffset = 0x800;
constexpr uint32_t kGraceUseCostRva = 0x9A7770;
constexpr uint32_t kGraceUseCostStubOffset = 0x900;
constexpr uint32_t kGraceUseCostTrampolineOffset = 0xA00;
constexpr uint32_t kGraceTutorialHelperOffset = 0xB00;
constexpr uint32_t kBtlGetInstanceRva = 0x44AC90;
constexpr uint32_t kBtlGetSequenceCtrlRva = 0x44AC30;
constexpr uint32_t kIsBdTutorialRva = 0x47AE50;
constexpr uint32_t kTextRva = 0x1000;

// Position-independent x64 helper, generated from tools/native_matk.asm.
constexpr uint8_t kNativeCode[] = {
    0x53,0x56,0x57,0x48,0x83,0xEC,0x40,0x48,0x8B,0xF9,0x8B,0xF0,0x33,0xDB,0x48,0x8B,
    0x47,0x78,0x48,0x85,0xC0,0x74,0x4C,0x8B,0x50,0x10,0x85,0xD2,0x74,0x45,0x89,0x54,
    0x24,0x20,0x8B,0xCA,0x33,0xD2,0xE8,0xA1,0x00,0x00,0x00,0x48,0x85,0xC0,0x74,0x33,
    0x83,0x78,0x14,0x01,0x75,0x2D,0x48,0x89,0x44,0x24,0x28,0x48,0x8B,0xCF,0x8B,0x54,
    0x24,0x20,0x45,0x33,0xC0,0xE8,0x83,0x00,0x00,0x00,0x83,0xE8,0x64,0x48,0x8B,0x54,
    0x24,0x28,0x0F,0xAF,0x82,0x90,0x00,0x00,0x00,0x99,0xB9,0x64,0x00,0x00,0x00,0xF7,
    0xF9,0x03,0xF0,0x48,0x8B,0x87,0x80,0x00,0x00,0x00,0x48,0x85,0xC0,0x74,0x4C,0x8B,
    0x50,0x10,0x85,0xD2,0x74,0x45,0x89,0x54,0x24,0x20,0x8B,0xCA,0x33,0xD2,0xE8,0x49,
    0x00,0x00,0x00,0x48,0x85,0xC0,0x74,0x33,0x83,0x78,0x14,0x01,0x75,0x2D,0x48,0x89,
    0x44,0x24,0x28,0x48,0x8B,0xCF,0x8B,0x54,0x24,0x20,0x45,0x33,0xC0,0xE8,0x2B,0x00,
    0x00,0x00,0x83,0xE8,0x64,0x48,0x8B,0x54,0x24,0x28,0x0F,0xAF,0x82,0x90,0x00,0x00,
    0x00,0x99,0xB9,0x64,0x00,0x00,0x00,0xF7,0xF9,0x03,0xF0,0x8B,0xDE,0x8B,0xC6,0x48,
    0x83,0xC4,0x40,0x5F,0x5E,0x5B,0x4C,0x8D,0x44,0x24,0x40,0xC3,0xC3,0xC3
};

uint32_t AlignUp(uint32_t value, uint32_t align) {
    return (value + align - 1) & ~(align - 1);
}

bool HashFile(const std::vector<uint8_t>& bytes, std::string& hex) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0, resultLength = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &resultLength, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0); return false;
    }
    std::vector<uint8_t> object(objectLength), digest(32);
    bool ok = BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0) >= 0 &&
              BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) >= 0 &&
              BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return false;
    static constexpr char digits[] = "0123456789ABCDEF";
    hex.clear(); hex.reserve(64);
    for (uint8_t b : digest) { hex.push_back(digits[b >> 4]); hex.push_back(digits[b & 15]); }
    return true;
}

uint32_t RvaToOffset(const IMAGE_NT_HEADERS64* nt, const IMAGE_SECTION_HEADER* sections, uint32_t rva) {
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& s = sections[i];
        uint32_t extent = std::max(s.Misc.VirtualSize, s.SizeOfRawData);
        if (rva >= s.VirtualAddress && rva - s.VirtualAddress < extent)
            return s.PointerToRawData + (rva - s.VirtualAddress);
    }
    return 0;
}

bool Patch(std::vector<uint8_t>& file, std::wstring& reason) {
    if (file.size() < sizeof(IMAGE_DOS_HEADER)) { reason = L"The selected file is too small."; return false; }
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(file.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > file.size()) {
        reason = L"The selected file is not a valid GameAssembly.dll."; return false;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(file.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        reason = L"The selected GameAssembly.dll is not the supported 64-bit build."; return false;
    }
    std::string hash;
    if (!HashFile(file, hash) || std::none_of(std::begin(kSupportedHashes), std::end(kSupportedHashes), [&](const char* supported) { return hash == supported; })) {
        reason = L"This GameAssembly.dll version is not supported (SHA-256 " + std::wstring(hash.begin(), hash.end()) + L"). The patcher only modifies the verified game build."; return false;
    }
    auto* sections = IMAGE_FIRST_SECTION(nt);
    uint32_t hook = RvaToOffset(nt, sections, kHookRva);
    uint32_t preserve = RvaToOffset(nt, sections, kPreserveRva);
    if (!hook || !preserve || hook + 10 > file.size() || preserve + 2 > file.size() ||
        memcmp(file.data() + hook, "\x4C\x8D\x44\x24\x38\xBA\xEF\x03\x00\x00", 10) ||
        memcmp(file.data() + preserve, "\x8B\xD8", 2)) {
        reason = L"The game file has unexpected code at the patch locations."; return false;
    }

    // Apply the requested dialogue timing changes directly in the game assembly.
    struct DialoguePatch { uint32_t rva; const uint8_t* finalBytes; size_t length; std::vector<std::vector<uint8_t>> accepted; };
    const uint8_t fieldZero[] = {0xC7,0x83,0x0C,0x01,0x00,0x00,0x00,0x00,0x00,0x00};
    const uint8_t partyHalf[] = {0xC7,0x43,0x6C,0x00,0x00,0x00,0x3F};
    const uint8_t blackZero[] = {0xC7,0x46,0x18,0x00,0x00,0x00,0x00};
    const uint8_t blackWait[] = {0x75,0x55};
    std::vector<DialoguePatch> dialogue = {
        {0x691B98, fieldZero, sizeof(fieldZero), {{0xC7,0x83,0x0C,0x01,0x00,0x00,0x00,0x00,0x80,0x3F}, {0xC7,0x83,0x0C,0x01,0x00,0x00,0x00,0x00,0x00,0x3F}, {0xC7,0x83,0x0C,0x01,0x00,0x00,0xCD,0xCC,0xCC,0x3D}, {0xC7,0x83,0x0C,0x01,0x00,0x00,0x00,0x00,0x00,0x00}}},
        {0x691BC8, fieldZero, sizeof(fieldZero), {{0xC7,0x83,0x0C,0x01,0x00,0x00,0x33,0x33,0x33,0x3F}, {0xC7,0x83,0x0C,0x01,0x00,0x00,0x00,0x00,0x00,0x3F}, {0xC7,0x83,0x0C,0x01,0x00,0x00,0xCD,0xCC,0xCC,0x3D}, {0xC7,0x83,0x0C,0x01,0x00,0x00,0x00,0x00,0x00,0x00}}},
        {0x6E78E2, partyHalf, sizeof(partyHalf), {{0xC7,0x43,0x6C,0x9A,0x99,0x99,0x3F}, {0xC7,0x43,0x6C,0x00,0x00,0x00,0x3F}}},
        {0x6AA8FB, blackZero, sizeof(blackZero), {{0xC7,0x46,0x18,0x00,0x00,0x80,0x3F}, {0xC7,0x46,0x18,0x00,0x00,0x00,0x00}}},
        {0x6BD88D, blackWait, sizeof(blackWait), {{0x75,0x13}, {0x75,0x55}}}
    };
    for (const auto& p : dialogue) {
        uint32_t off = RvaToOffset(nt, sections, p.rva);
        if (!off || off + p.length > file.size()) { reason = L"A dialogue patch location could not be resolved."; return false; }
        bool accepted = false;
        for (const auto& old : p.accepted) if (old.size() == p.length && !memcmp(file.data() + off, old.data(), p.length)) { accepted = true; break; }
        if (!accepted) { reason = L"A dialogue patch location has an unexpected value."; return false; }
        memcpy(file.data() + off, p.finalBytes, p.length);
    }

    // Abilink's list is populated by friend slot index, but the game's
    // selection coroutine later resolves the selected row as a raw slot.
    // Date sorting the visible rows therefore links a different friend. Keep
    // the list in ascending slot order so the displayed row and stored slot
    // are the same. The method's first six bytes are replaced with
    // `return a0index - a1index`.
    {
        uint32_t off = RvaToOffset(nt, sections, 0x8A2AD0);
        static constexpr uint8_t expected[] = {0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x30};
        static constexpr uint8_t replacement[] = {0x8B,0xC2,0x41,0x2B,0xC0,0xC3,0x90,0x90,0x90,0x90};
        if (!off || off + sizeof(replacement) > file.size() ||
            memcmp(file.data() + off, expected, sizeof(expected))) {
            reason = L"The Abilink friend-order location has unexpected code."; return false;
        }
        memcpy(file.data() + off, replacement, sizeof(replacement));
    }

    // CheckDamageRange normally clamps Limit Break damage to 99,999. The
    // Charm of the Limit Breaker opts a wearer into the game's existing
    // 999,999 overflow path, but only while the Limit Break flag is active.
    // BtlActionCalc.m_pAttacker (0x28) -> BtlChara.m_parameter (0x108) ->
    // BtlCharaParameter.equipAccessories (0x50) stores equipped item IDs.
    uint32_t damageHookFileOffset = 0;
    {
        uint32_t off = RvaToOffset(nt, sections, kDamageHookRva);
        damageHookFileOffset = off;
        static constexpr uint8_t expected[] = {0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10};
        if (!off || off + sizeof(expected) > file.size() ||
            memcmp(file.data() + off, expected, sizeof(expected))) {
            reason = L"The damage-cap location has unexpected code."; return false;
        }
    }

    uint32_t stealRateHookFileOffset = 0;
    {
        uint32_t off = RvaToOffset(nt, sections, kStealRateHookRva);
        stealRateHookFileOffset = off;
        static constexpr uint8_t expected[] = {0x8B,0x41,0x5C,0xC3,0xCC};
        if (!off || off + sizeof(expected) > file.size() || memcmp(file.data() + off, expected, sizeof(expected))) {
            reason = L"The steal-rate location has unexpected code."; return false;
        }
    }
    uint32_t supportAbilityHookFileOffset = 0;
    {
        uint32_t off = RvaToOffset(nt, sections, kSupportAbilityHookRva);
        supportAbilityHookFileOffset = off;
        static constexpr uint8_t expected[] = {0x48,0x83,0xEC,0x28,0x45,0x33,0xC9};
        if (!off || off + sizeof(expected) > file.size() || memcmp(file.data() + off, expected, sizeof(expected))) {
            reason = L"The Rob Blind support-ability location has unexpected code."; return false;
        }
    }
    uint32_t scouterHookFileOffset = 0;
    {
        uint32_t off = RvaToOffset(nt, sections, kScouterHookRva);
        scouterHookFileOffset = off;
        static constexpr uint8_t expected[] = {0x48,0x85,0xC9,0x0F,0x84,0x41,0x03,0x00,0x00};
        if (!off || off + sizeof(expected) > file.size() || memcmp(file.data() + off, expected, sizeof(expected))) {
            reason = L"The Scouter enemy-information location has unexpected code."; return false;
        }
    }
    const struct GraceHook { uint32_t rva; size_t length; } graceHooks[] = {
        {kGraceGetBpRva, 11},
        {kGraceSetBpRva, 5},
        {kGraceCheckCostRva, 5},
        {kGraceUseCostRva, 7}
    };
    uint32_t graceHookFileOffsets[4]{};
    static constexpr uint8_t graceGetBpExpected[] = {0x48,0x83,0xEC,0x28,0x48,0x8B,0x81,0x40,0x01,0x00,0x00};
    static constexpr uint8_t graceSetBpExpected[] = {0x48,0x89,0x5C,0x24,0x08};
    static constexpr uint8_t graceCheckCostExpected[] = {0x48,0x89,0x5C,0x24,0x08};
    static constexpr uint8_t graceUseCostExpected[] = {0x40,0x55,0x56,0x48,0x83,0xEC,0x28};
    const uint8_t* graceExpected[] = {graceGetBpExpected, graceSetBpExpected, graceCheckCostExpected, graceUseCostExpected};
    for (size_t i = 0; i < std::size(graceHooks); ++i) {
        uint32_t off = RvaToOffset(nt, sections, graceHooks[i].rva);
        graceHookFileOffsets[i] = off;
        if (!off || off + graceHooks[i].length > file.size() ||
            memcmp(file.data() + off, graceExpected[i], graceHooks[i].length)) {
            reason = L"A Grace of Gods BP behavior location has unexpected code."; return false;
        }
    }

    uint32_t sectionTable = static_cast<uint32_t>(reinterpret_cast<uint8_t*>(sections + nt->FileHeader.NumberOfSections) - file.data());
    if (sectionTable + sizeof(IMAGE_SECTION_HEADER) > nt->OptionalHeader.SizeOfHeaders) {
        reason = L"The PE header has no free section slot."; return false;
    }
    uint32_t lastVirtualEnd = 0, rawEnd = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& s = sections[i];
        lastVirtualEnd = (std::max<uint32_t>)(lastVirtualEnd, s.VirtualAddress + (std::max<uint32_t>)(s.Misc.VirtualSize, s.SizeOfRawData));
        rawEnd = (std::max<uint32_t>)(rawEnd, s.PointerToRawData + s.SizeOfRawData);
    }
    uint32_t newRva = AlignUp(lastVirtualEnd, nt->OptionalHeader.SectionAlignment);
    uint32_t rawPtr = AlignUp((std::max<uint32_t>)(rawEnd, static_cast<uint32_t>(file.size())), nt->OptionalHeader.FileAlignment);
    uint32_t rawSize = AlignUp((std::max<uint32_t>)(static_cast<uint32_t>(sizeof(kNativeCode)), kScouterTrampolineOffset + 32), nt->OptionalHeader.FileAlignment);
    file.resize(rawPtr + rawSize, 0);
    uint8_t* code = file.data() + rawPtr;
    memcpy(code, kNativeCode, sizeof(kNativeCode));

    auto redirect = [&](uint32_t stubOffset, uint32_t targetRva) {
        int32_t rel = static_cast<int32_t>(targetRva - (newRva + stubOffset + 5));
        code[stubOffset] = 0xE9;
        memcpy(code + stubOffset + 1, &rel, sizeof(rel));
    };
    auto redirectCall = [&](uint32_t callOffset, uint32_t stubOffset) {
        int32_t rel = static_cast<int32_t>((newRva + stubOffset) - (newRva + callOffset + 5));
        code[callOffset] = 0xE8;
        memcpy(code + callOffset + 1, &rel, sizeof(rel));
    };
    // The checked-in helper bytes originate in MASM; rebase its four calls to
    // the now-separated 5-byte jump stubs rather than the old overlapping ret stubs.
    redirectCall(0x026, kLookupOffset);
    redirectCall(0x045, kProperOffset);
    redirectCall(0x07E, kLookupOffset);
    redirectCall(0x09D, kProperOffset);
    redirect(kLookupOffset, kLookupRva);
    redirect(kProperOffset, kProperRva);
    static constexpr uint8_t trampoline[] = {
        0x48,0x89,0xF9,                         // mov rcx,rdi (CharacterState)
        0xE8,0,0,0,0,                           // call NativeMatkAdjust
        0x45,0x33,0xC9,                         // xor r9d,r9d
        0xC6,0x44,0x24,0x38,0x00,               // mov byte ptr [rsp+38h],0
        0x4C,0x8D,0x44,0x24,0x38,               // lea r8,[rsp+38h]
        0xBA,0xEF,0x03,0x00,0x00,               // mov edx,1007 (original ability id)
        0xE9,0,0,0,0                            // jump to original continuation
    };
    static_assert(kTrampolineOffset + sizeof(trampoline) <= 0x200);
    memcpy(code + kTrampolineOffset, trampoline, sizeof(trampoline));
    int32_t helperRel = static_cast<int32_t>((newRva + kHelperOffset) -
        (newRva + kTrampolineOffset + 8));
    memcpy(code + kTrampolineOffset + 4, &helperRel, sizeof(helperRel));
    int32_t resumeRel = static_cast<int32_t>((kHookRva + 10) -
        (newRva + kTrampolineOffset + sizeof(trampoline)));
    memcpy(code + kTrampolineOffset + sizeof(trampoline) - 4, &resumeRel, sizeof(resumeRel));
    uint8_t replacement[10] = {0xE9,0,0,0,0,0x90,0x90,0x90,0x90,0x90};
    int32_t hookRel = static_cast<int32_t>((newRva + kTrampolineOffset) - (kHookRva + 5));
    memcpy(replacement + 1, &hookRel, sizeof(hookRel));
    memcpy(file.data() + hook, replacement, sizeof(replacement));

    // This tiny entry shim reads the currently equipped accessory IDs without
    // calling managed code. If the Limit Break flag is set and item 30158 is
    // equipped, it marks this damage record for the game's 999,999 cap and
    // clears bLimitBreak so the overflow branch applies the higher clamp.
    // Only RAX is saved; the other incoming arguments remain intact.
    static constexpr uint8_t damageStub[] = {
        0x50,                                           // push rax
        0x45,0x84,0xC9,                                 // test r9b,r9b
        0x74,0x45,                                       // jz skip
        0x48,0x8B,0x41,0x28,                             // mov rax,[rcx+28h] (attacker)
        0x48,0x85,0xC0,                                 // test rax,rax
        0x74,0x3C,                                       // jz skip
        0x48,0x8B,0x80,0x08,0x01,0x00,0x00,             // mov rax,[rax+108h] (parameter)
        0x48,0x85,0xC0,                                 // test rax,rax
        0x74,0x30,                                       // jz skip
        0x48,0x8B,0x40,0x50,                             // mov rax,[rax+50h] (accessory IDs)
        0x48,0x85,0xC0,                                 // test rax,rax
        0x74,0x27,                                       // jz skip
        0x48,0x83,0x78,0x18,0x01,                       // cmp qword [rax+18h],1
        0x72,0x20,                                       // jb skip
        0x81,0x78,0x20,0xCE,0x75,0x00,0x00,             // cmp dword [rax+20h],30158
        0x74,0x10,                                       // je found
        0x48,0x83,0x78,0x18,0x02,                       // cmp qword [rax+18h],2
        0x72,0x10,                                       // jb skip
        0x81,0x78,0x24,0xCE,0x75,0x00,0x00,             // cmp dword [rax+24h],30158
        0x75,0x07,                                       // jne skip
        0xC6,0x42,0x5E,0x01,                             // found: mov byte [rdx+5Eh],1 (isOverFlow)
        0x45,0x33,0xC9,                                 // xor r9d,r9d
        0x58,                                           // skip: pop rax
        0xE9,0,0,0,0                                    // jump to original prologue trampoline
    };
    static_assert(kDamageStubOffset + sizeof(damageStub) <= kDamageTrampolineOffset);
    memcpy(code + kDamageStubOffset, damageStub, sizeof(damageStub));
    int32_t damageResumeRel = static_cast<int32_t>((newRva + kDamageTrampolineOffset) -
        (newRva + kDamageStubOffset + sizeof(damageStub)));
    memcpy(code + kDamageStubOffset + sizeof(damageStub) - 4, &damageResumeRel, sizeof(damageResumeRel));
    static constexpr uint8_t damageTrampoline[] = {
        0x48,0x89,0x5C,0x24,0x08,                         // original first 5 bytes
        0x48,0x89,0x74,0x24,0x10,                         // original next 5 bytes
        0xE9,0,0,0,0                                      // jump to CheckDamageRange + 10
    };
    memcpy(code + kDamageTrampolineOffset, damageTrampoline, sizeof(damageTrampoline));
    int32_t damageContinuationRel = static_cast<int32_t>((kDamageHookRva + 10) -
        (newRva + kDamageTrampolineOffset + sizeof(damageTrampoline)));
    memcpy(code + kDamageTrampolineOffset + sizeof(damageTrampoline) - 4,
        &damageContinuationRel, sizeof(damageContinuationRel));
    uint8_t damageReplacement[10] = {0xE9,0,0,0,0,0x90,0x90,0x90,0x90,0x90};
    int32_t damageHookRel = static_cast<int32_t>((newRva + kDamageStubOffset) - (kDamageHookRva + 5));
    memcpy(damageReplacement + 1, &damageHookRel, sizeof(damageHookRel));
    memcpy(file.data() + damageHookFileOffset, damageReplacement, sizeof(damageReplacement));

    // Kleptomaniac's Charm reuses the same steal-rate getter as the Trainer's
    // 100% steal option. BtlChara.parameter.equipAccessories contains IDs.
    static constexpr uint8_t stealRateStub[] = {
        0x48,0x8B,0x81,0x08,0x01,0x00,0x00,             // mov rax,[rcx+108h] (parameter)
        0x48,0x85,0xC0,                                 // test rax,rax
        0x74,0x2F,                                       // jz fallback
        0x48,0x8B,0x40,0x50,                             // mov rax,[rax+50h] (accessory IDs)
        0x48,0x85,0xC0,                                 // test rax,rax
        0x74,0x26,                                       // jz fallback
        0x48,0x83,0x78,0x18,0x01,                       // cmp qword [rax+18h],1
        0x72,0x1F,                                       // jb fallback
        0x81,0x78,0x20,0xCF,0x75,0x00,0x00,             // cmp dword [rax+20h],30159
        0x74,0x10,                                       // je found
        0x48,0x83,0x78,0x18,0x02,                       // cmp qword [rax+18h],2
        0x72,0x0F,                                       // jb fallback
        0x81,0x78,0x24,0xCF,0x75,0x00,0x00,             // cmp dword [rax+24h],30159
        0x75,0x06,                                       // jne fallback
        0xB8,0x10,0x27,0x00,0x00,                       // found: mov eax,10000
        0xC3,                                           // ret
        0x8B,0x41,0x5C,0xC3                             // fallback: original getter
    };
    static_assert(kStealRateStubOffset + sizeof(stealRateStub) <= kSupportAbilityStubOffset);
    memcpy(code + kStealRateStubOffset, stealRateStub, sizeof(stealRateStub));
    uint8_t stealRateReplacement[5] = {0xE9,0,0,0,0};
    int32_t stealRateHookRel = static_cast<int32_t>((newRva + kStealRateStubOffset) - (kStealRateHookRva + 5));
    memcpy(stealRateReplacement + 1, &stealRateHookRel, sizeof(stealRateHookRel));
    memcpy(file.data() + stealRateHookFileOffset, stealRateReplacement, sizeof(stealRateReplacement));

    // Kleptomaniac is a cloned native support ability (1566). Keep learned
    // Rob Blind (1527) independent; the game's own support-ability effect
    // handler then performs both item rolls when both are enabled.
    static constexpr uint8_t supportAbilityStub[] = {
        0x81,0xFA,0x1E,0x06,0x00,0x00,                   // cmp edx,1566
        0x75,0x1B,                                       // jne fallback
        0x48,0x8B,0x81,0x88,0x00,0x00,0x00,             // mov rax,[rcx+88h] (accessory ItemState)
        0x48,0x85,0xC0,                                 // test rax,rax
        0x74,0x0F,                                       // jz fallback
        0x81,0x78,0x10,0xCF,0x75,0x00,0x00,             // cmp dword [rax+10h],30159
        0x75,0x06,                                       // jne fallback
        0xB8,0x01,0x00,0x00,0x00,                       // mov eax,1
        0xC3,                                           // ret
        0xE9,0,0,0,0                                    // fallback: original trampoline
    };
    static_assert(kSupportAbilityStubOffset + sizeof(supportAbilityStub) <= kSupportAbilityTrampolineOffset);
    memcpy(code + kSupportAbilityStubOffset, supportAbilityStub, sizeof(supportAbilityStub));
    int32_t supportResumeRel = static_cast<int32_t>((newRva + kSupportAbilityTrampolineOffset) -
        (newRva + kSupportAbilityStubOffset + sizeof(supportAbilityStub)));
    memcpy(code + kSupportAbilityStubOffset + sizeof(supportAbilityStub) - 4, &supportResumeRel, sizeof(supportResumeRel));
    static constexpr uint8_t supportAbilityTrampoline[] = {
        0x48,0x83,0xEC,0x28,0x45,0x33,0xC9,             // original first seven bytes
        0xE9,0,0,0,0                                    // jump to IsEnableSupportAbility + 7
    };
    memcpy(code + kSupportAbilityTrampolineOffset, supportAbilityTrampoline, sizeof(supportAbilityTrampoline));
    int32_t supportContinuationRel = static_cast<int32_t>((kSupportAbilityHookRva + 7) -
        (newRva + kSupportAbilityTrampolineOffset + sizeof(supportAbilityTrampoline)));
    memcpy(code + kSupportAbilityTrampolineOffset + sizeof(supportAbilityTrampoline) - 4,
        &supportContinuationRel, sizeof(supportContinuationRel));
    uint8_t supportReplacement[7] = {0xE9,0,0,0,0,0x90,0x90};
    int32_t supportHookRel = static_cast<int32_t>((newRva + kSupportAbilityStubOffset) - (kSupportAbilityHookRva + 5));
    memcpy(supportReplacement + 1, &supportHookRel, sizeof(supportHookRel));
    memcpy(file.data() + supportAbilityHookFileOffset, supportReplacement, sizeof(supportReplacement));

    // The Scouter accessory raises each active enemy panel to the game's full
    // information level while any party member wears item 30163. Scan the
    // current battle-character array directly; this helper calls only the
    // game's own singleton accessors and reads the known x64 object fields.
    std::vector<uint8_t> scouterHelper;
    auto emitScouter = [&](std::initializer_list<uint8_t> bytes) {
        scouterHelper.insert(scouterHelper.end(), bytes.begin(), bytes.end());
    };
    auto scouterBranch = [&](uint8_t condition) {
        emitScouter({0x0F,condition,0,0,0,0});
        return scouterHelper.size() - 4;
    };
    auto patchScouterBranch = [&](size_t displacement, size_t target) {
        int32_t rel = static_cast<int32_t>(static_cast<int64_t>(target) - static_cast<int64_t>(displacement + 4));
        memcpy(scouterHelper.data() + displacement, &rel, sizeof(rel));
    };
    auto scouterCall = [&](uint32_t targetRva) {
        size_t pos = scouterHelper.size(); emitScouter({0xE8,0,0,0,0});
        int32_t rel = static_cast<int32_t>(targetRva - (newRva + kScouterHelperOffset + static_cast<uint32_t>(pos) + 5));
        memcpy(scouterHelper.data() + pos + 1, &rel, sizeof(rel));
    };
    emitScouter({0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x48,0x83,0xEC,0x28});
    scouterCall(kBtlGetInstanceRva);
    emitScouter({0x48,0x85,0xC0}); const size_t noInstance = scouterBranch(0x84);
    emitScouter({0x48,0x89,0xC1}); scouterCall(0x44AB40); // GetBtlCharaManager
    emitScouter({0x48,0x85,0xC0}); const size_t noManager = scouterBranch(0x84);
    emitScouter({0x48,0x8B,0x58,0x20,0x48,0x85,0xDB}); const size_t noArray = scouterBranch(0x84);
    emitScouter({0x31,0xF6}); // xor esi,esi (battle character index)
    const size_t loop = scouterHelper.size();
    emitScouter({0x3B,0x73,0x18}); const size_t endArray = scouterBranch(0x83); // index >= array length
    emitScouter({0x48,0x63,0xC6,0x48,0x8B,0x7C,0xC3,0x20,0x48,0x85,0xFF}); const size_t nextNull = scouterBranch(0x84);
    emitScouter({0x83,0x7F,0x2C,0x00}); const size_t nextNonPlayer = scouterBranch(0x85);
    emitScouter({0x48,0x8B,0x87,0x08,0x01,0x00,0x00,0x48,0x85,0xC0}); const size_t nextNoParam = scouterBranch(0x84);
    emitScouter({0x4C,0x8B,0x60,0x50,0x4D,0x85,0xE4}); const size_t nextNoAccessories = scouterBranch(0x84);
    emitScouter({0x49,0x83,0x7C,0x24,0x18,0x01}); const size_t checkSecond = scouterBranch(0x82);
    emitScouter({0x41,0x81,0x7C,0x24,0x20,0xD3,0x75,0x00,0x00}); const size_t foundFirst = scouterBranch(0x84);
    const size_t second = scouterHelper.size(); patchScouterBranch(checkSecond, second);
    emitScouter({0x49,0x83,0x7C,0x24,0x18,0x02}); const size_t next = scouterBranch(0x82);
    emitScouter({0x41,0x81,0x7C,0x24,0x24,0xD3,0x75,0x00,0x00}); const size_t foundSecond = scouterBranch(0x84);
    const size_t advance = scouterHelper.size();
    emitScouter({0xFF,0xC6}); // inc esi
    const size_t loopJump = scouterHelper.size(); emitScouter({0xE9,0,0,0,0});
    int32_t loopRel = static_cast<int32_t>((newRva + kScouterHelperOffset + static_cast<uint32_t>(loop)) -
        (newRva + kScouterHelperOffset + static_cast<uint32_t>(loopJump) + 5));
    memcpy(scouterHelper.data() + loopJump + 1, &loopRel, sizeof(loopRel));
    const size_t found = scouterHelper.size(); emitScouter({0xB8,0x01,0x00,0x00,0x00});
    const size_t returnJump = scouterHelper.size(); emitScouter({0xE9,0,0,0,0});
    const size_t notFound = scouterHelper.size(); emitScouter({0x31,0xC0});
    const size_t epilogue = scouterHelper.size(); emitScouter({0x48,0x83,0xC4,0x28,0x41,0x5E,0x41,0x5D,0x41,0x5C,0x5F,0x5E,0x5B,0xC3});
    for (size_t fix : {noInstance,noManager,noArray,endArray}) patchScouterBranch(fix,notFound);
    for (size_t fix : {nextNull,nextNonPlayer,nextNoParam,nextNoAccessories,next}) patchScouterBranch(fix,advance);
    patchScouterBranch(foundFirst,found); patchScouterBranch(foundSecond,found);
    patchScouterBranch(returnJump + 1,epilogue);
    if (kScouterHelperOffset + scouterHelper.size() > kScouterTrampolineOffset) {
        reason = L"The Scouter helper exceeded its reserved patch space."; return false;
    }
    memcpy(code + kScouterHelperOffset, scouterHelper.data(), scouterHelper.size());

    static constexpr uint8_t scouterStub[] = {
        0xE8,0,0,0,0,                                   // call accessory detector
        0x85,0xC0,                                       // test eax,eax
        0x74,0x05,                                       // jz original check
        0xBE,0x05,0x00,0x00,0x00,                       // mov esi,5 (full enemy info)
        0xE9,0,0,0,0                                    // jump to original test/branch trampoline
    };
    memcpy(code + kScouterStubOffset, scouterStub, sizeof(scouterStub));
    int32_t scouterHelperRel = static_cast<int32_t>((newRva + kScouterHelperOffset) - (newRva + kScouterStubOffset + 5));
    memcpy(code + kScouterStubOffset + 1, &scouterHelperRel, sizeof(scouterHelperRel));
    const size_t setLevelJump = kScouterStubOffset + 14;
    int32_t setLevelRel = static_cast<int32_t>((newRva + kScouterTrampolineOffset) - (newRva + setLevelJump + 5));
    memcpy(code + setLevelJump + 1, &setLevelRel, sizeof(setLevelRel));
    static constexpr uint8_t scouterTrampolinePrefix[] = {0x48,0x85,0xC9,0x0F,0x84};
    memcpy(code + kScouterTrampolineOffset, scouterTrampolinePrefix, sizeof(scouterTrampolinePrefix));
    int32_t originalNullBranch = static_cast<int32_t>((kScouterHookRva + 9 + 0x341) - (newRva + kScouterTrampolineOffset + 9));
    memcpy(code + kScouterTrampolineOffset + 5, &originalNullBranch, sizeof(originalNullBranch));
    code[kScouterTrampolineOffset + 9] = 0xE9;
    int32_t scouterResumeRel = static_cast<int32_t>((kScouterHookRva + 9) - (newRva + kScouterTrampolineOffset + 14));
    memcpy(code + kScouterTrampolineOffset + 10, &scouterResumeRel, sizeof(scouterResumeRel));
    uint8_t scouterReplacement[9] = {0xE9,0,0,0,0,0x90,0x90,0x90,0x90};
    int32_t scouterHookRel = static_cast<int32_t>((newRva + kScouterStubOffset) - (kScouterHookRva + 5));
    memcpy(scouterReplacement + 1, &scouterHookRel, sizeof(scouterHookRel));
    memcpy(file.data() + scouterHookFileOffset, scouterReplacement, sizeof(scouterReplacement));

    auto append = [](std::vector<uint8_t>& out, std::initializer_list<uint8_t> bytes) {
        out.insert(out.end(), bytes.begin(), bytes.end());
    };
    auto branch = [&](std::vector<uint8_t>& out, uint8_t condition) {
        append(out, {0x0F, condition, 0, 0, 0, 0});
        return out.size() - 4;
    };
    auto patchBranch = [](std::vector<uint8_t>& out, size_t displacement, size_t target) {
        int32_t rel = static_cast<int32_t>(static_cast<int64_t>(target) - static_cast<int64_t>(displacement + 4));
        memcpy(out.data() + displacement, &rel, sizeof(rel));
    };
    auto patchCodeJump = [&](std::vector<uint8_t>& out, uint32_t sourceOffset, size_t instruction, uint32_t targetOffset) {
        int32_t rel = static_cast<int32_t>((newRva + targetOffset) - (newRva + sourceOffset + instruction + 5));
        memcpy(out.data() + instruction + 1, &rel, sizeof(rel));
    };
    auto appendCallToOffset = [&](std::vector<uint8_t>& out, uint32_t sourceOffset, uint32_t targetOffset) {
        const size_t call = out.size(); append(out, {0xE8,0,0,0,0});
        int32_t rel = static_cast<int32_t>((newRva + targetOffset) - (newRva + sourceOffset + call + 5));
        memcpy(out.data() + call + 1, &rel, sizeof(rel));
    };
    auto graceCheck = [&](std::vector<uint8_t>& out, std::vector<size_t>& fallbackBranches, size_t& foundBranch) {
        append(out, {0x48,0x8B,0x81,0x08,0x01,0x00,0x00}); // mov rax,[rcx+108h] (BtlCharaParameter)
        append(out, {0x48,0x85,0xC0});
        fallbackBranches.push_back(branch(out, 0x84));
        append(out, {0x48,0x8B,0x40,0x50}); // mov rax,[rax+50h] (equipped accessories)
        append(out, {0x48,0x85,0xC0});
        fallbackBranches.push_back(branch(out, 0x84));
        append(out, {0x48,0x83,0x78,0x18,0x01}); // cmp qword [rax+18h],1
        fallbackBranches.push_back(branch(out, 0x82));
        append(out, {0x81,0x78,0x20,0xD2,0x75,0x00,0x00}); // cmp dword [rax+20h],30162
        foundBranch = branch(out, 0x84);
        append(out, {0x48,0x83,0x78,0x18,0x02});
        fallbackBranches.push_back(branch(out, 0x82));
        append(out, {0x81,0x78,0x24,0xD2,0x75,0x00,0x00}); // cmp dword [rax+24h],30162
        fallbackBranches.push_back(branch(out, 0x85));
    };
    auto installGraceDetour = [&](uint32_t rva, uint32_t stubOffset, uint32_t trampolineOffset,
                                  uint32_t fileOffset, const uint8_t* original, size_t length,
                                  std::vector<uint8_t>& stub, const std::vector<size_t>& jmpInstructions) -> bool {
        if (stubOffset + stub.size() > trampolineOffset || trampolineOffset + length + 5 > rawSize) {
            reason = L"The Grace of Gods native hook layout exceeds its reserved code space.";
            return false;
        }
        for (size_t jump : jmpInstructions) patchCodeJump(stub, stubOffset, jump, trampolineOffset);
        memcpy(code + stubOffset, stub.data(), stub.size());
        std::vector<uint8_t> trampoline(original, original + length);
        trampoline.push_back(0xE9); trampoline.resize(trampoline.size() + 4);
        int32_t resume = static_cast<int32_t>((rva + length) - (newRva + trampolineOffset + trampoline.size()));
        memcpy(trampoline.data() + trampoline.size() - 4, &resume, sizeof(resume));
        memcpy(code + trampolineOffset, trampoline.data(), trampoline.size());
        std::vector<uint8_t> replacement(length, 0x90);
        replacement[0] = 0xE9;
        int32_t detour = static_cast<int32_t>((newRva + stubOffset) - (rva + 5));
        memcpy(replacement.data() + 1, &detour, sizeof(detour));
        memcpy(file.data() + fileOffset, replacement.data(), replacement.size());
        return true;
    };

    // Query the same game flag the Trainer watches. This lets the accessory
    // preserve the Brave/Default tutorial's intended BP behavior by itself.
    {
        std::vector<uint8_t> checker;
        append(checker, {0x48,0x83,0xEC,0x28,0x33,0xC9}); // aligned stack; null MethodInfo
        auto callRva = [&](uint32_t targetRva) {
            const size_t call = checker.size(); append(checker, {0xE8,0,0,0,0});
            int32_t rel = static_cast<int32_t>(targetRva - (newRva + kGraceTutorialHelperOffset + call + 5));
            memcpy(checker.data() + call + 1, &rel, sizeof(rel));
        };
        callRva(kBtlGetInstanceRva);
        append(checker, {0x48,0x85,0xC0});
        const size_t noInstance = branch(checker,0x84);
        append(checker, {0x48,0x89,0xC1,0x33,0xD2}); // RCX=this, RDX=null MethodInfo
        callRva(kBtlGetSequenceCtrlRva);
        append(checker, {0x48,0x85,0xC0});
        const size_t noSequence = branch(checker,0x84);
        append(checker, {0x48,0x89,0xC1,0x33,0xD2});
        callRva(kIsBdTutorialRva);
        append(checker, {0x48,0x83,0xC4,0x28,0xC3});
        const size_t noTutorialState = checker.size();
        append(checker, {0xB8,0x01,0x00,0x00,0x00,0x48,0x83,0xC4,0x28,0xC3}); // fail closed
        patchBranch(checker,noInstance,noTutorialState);
        patchBranch(checker,noSequence,noTutorialState);
        memcpy(code + kGraceTutorialHelperOffset,checker.data(),checker.size());
    }

    // Grace of Gods uses BtlChara's own BP/cost methods. The accessory ID is
    // checked on that character, so party members without the charm are untouched.
    {
        std::vector<uint8_t> stub; std::vector<size_t> fallback; size_t found = 0;
        graceCheck(stub, fallback, found);
        const size_t foundOffset = stub.size();
        append(stub, {0x48,0x83,0xEC,0x38,0x48,0x89,0x4C,0x24,0x20,0x48,0x89,0x54,0x24,0x28});
        appendCallToOffset(stub,kGraceGetBpStubOffset,kGraceTutorialHelperOffset);
        append(stub, {0x48,0x8B,0x4C,0x24,0x20,0x48,0x8B,0x54,0x24,0x28,0x48,0x83,0xC4,0x38,0x85,0xC0});
        const size_t pausedBranch = branch(stub,0x85);
        append(stub, {0xB8,0x03,0x00,0x00,0x00,0xC3}); // return 3 BP
        const size_t fallbackOffset = stub.size();
        for (size_t fix : fallback) patchBranch(stub, fix, fallbackOffset);
        patchBranch(stub,found,foundOffset);
        patchBranch(stub,pausedBranch,fallbackOffset);
        const size_t jump = stub.size(); append(stub, {0xE9,0,0,0,0});
        const uint32_t jumps[] = {static_cast<uint32_t>(jump)};
        if (!installGraceDetour(kGraceGetBpRva,kGraceGetBpStubOffset,kGraceGetBpTrampolineOffset,
            graceHookFileOffsets[0],graceGetBpExpected,sizeof(graceGetBpExpected),stub,
            std::vector<size_t>(std::begin(jumps),std::end(jumps)))) return false;
    }
    {
        std::vector<uint8_t> stub; std::vector<size_t> fallback; size_t found = 0;
        graceCheck(stub, fallback, found);
        const size_t foundOffset = stub.size();
        append(stub, {0x48,0x83,0xEC,0x38,0x48,0x89,0x4C,0x24,0x20,0x48,0x89,0x54,0x24,0x28,0x4C,0x89,0x44,0x24,0x30});
        appendCallToOffset(stub,kGraceSetBpStubOffset,kGraceTutorialHelperOffset);
        append(stub, {0x48,0x8B,0x4C,0x24,0x20,0x48,0x8B,0x54,0x24,0x28,0x4C,0x8B,0x44,0x24,0x30,0x48,0x83,0xC4,0x38,0x85,0xC0});
        const size_t pausedBranch = branch(stub,0x85);
        append(stub, {0xBA,0x03,0x00,0x00,0x00}); // force requested BP to 3
        const size_t foundJump = stub.size(); append(stub, {0xE9,0,0,0,0});
        const size_t fallbackOffset = stub.size(); const size_t fallbackJump = stub.size(); append(stub, {0xE9,0,0,0,0});
        patchBranch(stub, found, foundOffset);
        for (size_t fix : fallback) patchBranch(stub, fix, fallbackOffset);
        patchBranch(stub,pausedBranch,fallbackOffset);
        const uint32_t jumps[] = {static_cast<uint32_t>(foundJump),static_cast<uint32_t>(fallbackJump)};
        if (!installGraceDetour(kGraceSetBpRva,kGraceSetBpStubOffset,kGraceSetBpTrampolineOffset,
            graceHookFileOffsets[1],graceSetBpExpected,sizeof(graceSetBpExpected),stub,
            std::vector<size_t>(std::begin(jumps),std::end(jumps)))) return false;
    }
    auto makeGraceCostStub = [&](uint32_t stubOffset, uint32_t trampolineOffset, bool useCost) {
        std::vector<uint8_t> stub; std::vector<size_t> fallback; size_t found = 0;
        graceCheck(stub, fallback, found);
        append(stub, {0x48,0x85,0xD2});
        fallback.push_back(branch(stub, 0x84)); // null cost pointer falls through to original
        const size_t costPath = stub.size();
        append(stub, {0x48,0x83,0xEC,0x38});
        append(stub, {0x48,0x89,0x4C,0x24,0x20}); // save BtlChara
        append(stub, {0x48,0x89,0x54,0x24,0x28}); // save cost pointer
        append(stub, {0x4C,0x89,0x44,0x24,0x30}); // save MethodInfo
        appendCallToOffset(stub,stubOffset,kGraceTutorialHelperOffset);
        append(stub, {0x48,0x8B,0x4C,0x24,0x20,0x48,0x8B,0x54,0x24,0x28,0x4C,0x8B,0x44,0x24,0x30});
        append(stub, {0x85,0xC0});
        const size_t tutorialBranch = branch(stub,0x85);
        append(stub, {0x8B,0x42,0x18,0x89,0x44,0x24,0x30}); // save cost.bp
        append(stub, {0xC7,0x42,0x18,0x00,0x00,0x00,0x00}); // skip BP cost only
        const size_t call = stub.size(); append(stub, {0xE8,0,0,0,0});
        int32_t callRel = static_cast<int32_t>((newRva + trampolineOffset) - (newRva + stubOffset + call + 5));
        memcpy(stub.data() + call + 1, &callRel, sizeof(callRel));
        if (!useCost) append(stub, {0x89,0x44,0x24,0x34}); // keep IsUseAbilityCost result
        append(stub, {0x48,0x8B,0x54,0x24,0x28,0x8B,0x44,0x24,0x30,0x89,0x42,0x18}); // restore cost.bp
        if (useCost) {
            append(stub, {0x48,0x8B,0x44,0x24,0x20,0x48,0x8B,0x80,0x40,0x01,0x00,0x00}); // command ctrl
            append(stub, {0x48,0x85,0xC0});
            const size_t skipForce = branch(stub, 0x84);
            append(stub, {0xC7,0x40,0x18,0x03,0x00,0x00,0x00}); // retain full BP after the action
            const size_t epilogue = stub.size(); patchBranch(stub, skipForce, epilogue);
        } else append(stub, {0x8B,0x44,0x24,0x34});
        append(stub, {0x48,0x83,0xC4,0x38,0xC3});
        const size_t tutorialPath = stub.size();
        append(stub, {0x48,0x8B,0x4C,0x24,0x20,0x48,0x8B,0x54,0x24,0x28,0x4C,0x8B,0x44,0x24,0x30,0x85,0xC0});
        const size_t tutorialJump = stub.size(); append(stub, {0xE9,0,0,0,0});
        const size_t fallbackOffset = stub.size(); const size_t fallbackJump = stub.size(); append(stub, {0xE9,0,0,0,0});
        patchBranch(stub,found,costPath);
        patchBranch(stub,tutorialBranch,tutorialPath);
        for (size_t fix : fallback) patchBranch(stub,fix,fallbackOffset);
        const uint32_t jumps[] = {static_cast<uint32_t>(tutorialJump),static_cast<uint32_t>(fallbackJump)};
        return std::pair<std::vector<uint8_t>,std::vector<size_t>>(std::move(stub),std::vector<size_t>(std::begin(jumps),std::end(jumps)));
    };
    {
        auto built = makeGraceCostStub(kGraceCheckCostStubOffset,kGraceCheckCostTrampolineOffset,false);
        if (!installGraceDetour(kGraceCheckCostRva,kGraceCheckCostStubOffset,kGraceCheckCostTrampolineOffset,
            graceHookFileOffsets[2],graceCheckCostExpected,sizeof(graceCheckCostExpected),built.first,built.second)) return false;
    }
    {
        auto built = makeGraceCostStub(kGraceUseCostStubOffset,kGraceUseCostTrampolineOffset,true);
        if (!installGraceDetour(kGraceUseCostRva,kGraceUseCostStubOffset,kGraceUseCostTrampolineOffset,
            graceHookFileOffsets[3],graceUseCostExpected,sizeof(graceUseCostExpected),built.first,built.second)) return false;
    }

    IMAGE_SECTION_HEADER newSection{};
    memcpy(newSection.Name, ".bdffmat", 8);
    newSection.Misc.VirtualSize = rawSize;
    newSection.VirtualAddress = newRva;
    newSection.SizeOfRawData = rawSize;
    newSection.PointerToRawData = rawPtr;
    newSection.Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
    memcpy(file.data() + sectionTable, &newSection, sizeof(newSection));
    auto* updatedDos = reinterpret_cast<IMAGE_DOS_HEADER*>(file.data());
    auto* updatedNt = reinterpret_cast<IMAGE_NT_HEADERS64*>(file.data() + updatedDos->e_lfanew);
    updatedNt->FileHeader.NumberOfSections++;
    updatedNt->OptionalHeader.SizeOfImage = AlignUp(newRva + rawSize, updatedNt->OptionalHeader.SectionAlignment);
    updatedNt->OptionalHeader.SizeOfCode += rawSize;
    reason.clear();
    return true;
}

uint32_t ReadU32(const std::vector<uint8_t>& b, size_t offset) {
    uint32_t v = 0; memcpy(&v, b.data() + offset, sizeof(v)); return v;
}
void WriteU32(std::vector<uint8_t>& b, size_t offset, uint32_t v) {
    memcpy(b.data() + offset, &v, sizeof(v));
}

bool ReadBtb(const std::vector<uint8_t>& input, std::vector<std::vector<int32_t>>& rows,
             uint32_t& recordSize, std::vector<uint8_t>& ascii, std::vector<uint8_t>& utf16,
             std::wstring& reason) {
    if (input.size() < 48 || memcmp(input.data(), "BTBF", 4)) { reason = L"A game item table has an unsupported BTB header."; return false; }
    uint32_t dataOffset = ReadU32(input, 8), dataSize = ReadU32(input, 12);
    uint32_t stringOffset = ReadU32(input, 16), stringSize = ReadU32(input, 20);
    uint32_t utf16Offset = ReadU32(input, 24), utf16Size = ReadU32(input, 28);
    recordSize = ReadU32(input, 32); uint32_t count = ReadU32(input, 36);
    if (!recordSize || recordSize % 4 || dataOffset + dataSize > input.size() ||
        dataOffset + dataSize != stringOffset || stringOffset + stringSize > input.size() ||
        utf16Offset + utf16Size > input.size() || dataSize != recordSize * count) {
        reason = L"A game item table has invalid record or string offsets."; return false;
    }
    size_t ints = recordSize / 4;
    rows.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        rows[i].resize(ints);
        memcpy(rows[i].data(), input.data() + dataOffset + i * recordSize, recordSize);
    }
    ascii.assign(input.begin() + stringOffset, input.begin() + stringOffset + stringSize);
    utf16.assign(input.begin() + utf16Offset, input.begin() + utf16Offset + utf16Size);
    reason.clear(); return true;
}

std::vector<uint8_t> WriteBtb(const std::vector<std::vector<int32_t>>& rows, uint32_t recordSize,
                              const std::vector<uint8_t>& ascii, const std::vector<uint8_t>& utf16) {
    uint32_t dataOffset = 48, dataSize = static_cast<uint32_t>(rows.size()) * recordSize;
    uint32_t stringOffset = dataOffset + dataSize, stringSize = static_cast<uint32_t>(ascii.size());
    uint32_t utf16Offset = stringOffset + stringSize;
    if (utf16Offset & 1) ++utf16Offset;
    uint32_t utf16Size = static_cast<uint32_t>(utf16.size());
    std::vector<uint8_t> output(utf16Offset + utf16Size, 0);
    memcpy(output.data(), "BTBF", 4);
    WriteU32(output, 4, static_cast<uint32_t>(output.size()));
    WriteU32(output, 8, dataOffset); WriteU32(output, 12, dataSize);
    WriteU32(output, 16, stringOffset); WriteU32(output, 20, stringSize);
    WriteU32(output, 24, utf16Offset); WriteU32(output, 28, utf16Size);
    WriteU32(output, 32, recordSize); WriteU32(output, 36, static_cast<uint32_t>(rows.size()));
    WriteU32(output, 40, 0); WriteU32(output, 44, 0);
    for (size_t i = 0; i < rows.size(); ++i)
        memcpy(output.data() + dataOffset + i * recordSize, rows[i].data(), recordSize);
    memcpy(output.data() + stringOffset, ascii.data(), ascii.size());
    memcpy(output.data() + utf16Offset, utf16.data(), utf16.size());
    return output;
}

int32_t AddUtf16(std::vector<uint8_t>& strings, const std::wstring& text) {
    int32_t offset = static_cast<int32_t>(strings.size());
    for (wchar_t c : text) {
        uint16_t ch = static_cast<uint16_t>(c);
        strings.push_back(static_cast<uint8_t>(ch & 0xff));
        strings.push_back(static_cast<uint8_t>(ch >> 8));
    }
    strings.push_back(0); strings.push_back(0);
    return offset;
}

int32_t FindOrAddUtf16(std::vector<uint8_t>& strings, const std::wstring& text) {
    const size_t length = text.size();
    for (size_t offset = 0; offset + (length + 1) * 2 <= strings.size(); offset += 2) {
        bool same = true;
        for (size_t i = 0; i < length; ++i) {
            uint16_t value = static_cast<uint16_t>(strings[offset + i * 2]) |
                             (static_cast<uint16_t>(strings[offset + i * 2 + 1]) << 8);
            if (value != static_cast<uint16_t>(text[i])) { same = false; break; }
        }
        if (same && strings[offset + length * 2] == 0 && strings[offset + length * 2 + 1] == 0)
            return static_cast<int32_t>(offset);
    }
    return AddUtf16(strings, text);
}

std::wstring ReadUtf16(const std::vector<uint8_t>& strings, int32_t offset) {
    if (offset < 0 || static_cast<size_t>(offset) + 1 >= strings.size()) return L"";
    std::wstring result;
    for (size_t i = static_cast<size_t>(offset); i + 1 < strings.size(); i += 2) {
        uint16_t ch = static_cast<uint16_t>(strings[i]) |
                      (static_cast<uint16_t>(strings[i + 1]) << 8);
        if (!ch) break;
        result.push_back(static_cast<wchar_t>(ch));
    }
    return result;
}

bool PatchItems(std::vector<uint8_t>& input, std::wstring& reason, bool patchEnglishExtras) {
    std::vector<std::vector<int32_t>> rows; std::vector<uint8_t> ascii, utf16; uint32_t size = 0;
    if (!ReadBtb(input, rows, size, ascii, utf16, reason)) return false;
    // Permit the known 604-row table from the earlier charm patch so it can be
    // migrated back under the runtime's fixed 600-entry limit below.
    if (size != 163 * 4 || rows.empty() || rows.size() > 700) { reason = L"The item table has an unsupported layout or an unexpectedly large row count."; return false; }
    const struct Item { int id; const wchar_t* name; const wchar_t* desc; int price; int stat[9]; } items[] = {
        {30151,L"Might Charm",L"Raises physical attack by 30 and STR by 15.",50000,{30,0,0,0,15,0,0,0,0}},
        {30152,L"Arcanist Charm",L"Raises magic attack by 30 and MP by 100.",50000,{0,0,30,0,0,0,0,0,0}},
        {30153,L"Bulwark Charm",L"Raises physical and magic defense by 50 and HP by 100.",50000,{0,50,0,50,0,0,0,0,0}},
        {30154,L"Assassin's Charm",L"Raises evasion by 30 and speed by 15.",50000,{0,0,0,0,0,15,0,0,30}},
        {30155,L"Healer's Charm",L"Raises AGI (speed) by 30 and MP by 100.",50000,{0,0,0,0,0,30,0,0,0}},
        {30156,L"Deadeye Charm",L"Raises aim (accuracy) by 50 and DEX by 15.",50000,{0,0,0,0,0,0,15,50,0}},
        {30157,L"Charm of Omnipotence",L"Said to be a trinket that once belonged to a God.",500000,{30,50,30,50,15,45,15,50,30}},
        {30158,L"Charm of the Limit Breaker",L"With Limit Break active, raises the damage cap to 999,999.",500000,{0,0,0,0,0,0,0,0,0}},
        {30159,L"Kleptomaniac's Charm",L"Raises AGI by 30 and grants Kleptomaniac while equipped.",50000,{0,0,0,0,0,0,0,0,0}},
        {30160,L"Angel's Charm",L"Grants immunity to all status ailments.",50000,{0,0,0,0,0,0,0,0,0}},
        {30161,L"Elemental's Charm",L"Absorbs all elemental attacks.",50000,{0,0,0,0,0,0,0,0,0}},
        {30162,L"Grace of Gods",L"Enables one to move with a God's grace.",1000000,{0,0,0,0,0,0,0,0,0}}
    };
    auto existing = [&](int id) { return std::find_if(rows.begin(), rows.end(), [&](const auto& r) { return r[0] == id; }); };
    auto templateRow = existing(30100);
    if (templateRow == rows.end()) { reason = L"The game item table has no supported accessory template."; return false; }
    const auto templateRecord = *templateRow;

    // PartyState owns a fixed ItemState[600]. Appending records to ItemTable
    // shifts its index mapping past that array and crashes the equipment UI.
    // Reuse eight dummy accessory/key-item rows, append only the three earlier
    // extra charms below 600, and repurpose dummy row 90045 for Grace of Gods.
    const int dummyIds[] = {30146,30147,30148,30149,30150,90047,90048,90049};
    const int dummyTypes[] = {15,15,15,15,15,18,18,18};
    bool haveAllDummies = true;
    for (size_t i = 0; i < std::size(dummyIds); ++i) {
        auto row = existing(dummyIds[i]);
        if (row == rows.end() || (*row)[1] != dummyTypes[i]) { haveAllDummies = false; break; }
    }
    int customCount = 0;
    for (int id = 30151; id <= 30162; ++id) if (existing(id) != rows.end()) ++customCount;
    if (!haveAllDummies && customCount < 7) {
        reason = L"The supported dummy accessory/key-item rows or existing custom accessory rows could not be found.";
        return false;
    }

    if (haveAllDummies) {
        rows.erase(std::remove_if(rows.begin(), rows.end(), [](const auto& r) { return r[0] >= 30151 && r[0] <= 30162; }), rows.end());
        if (rows.size() > 600) { reason = L"The item table would exceed the game's 600-item inventory limit."; return false; }
    }
    for (size_t n = 0; n < std::size(items); ++n) {
        const auto& item = items[n];
        auto record = templateRecord;
        record[0] = item.id; record[1] = 15; record[2] = 1;
        record[4] = FindOrAddUtf16(utf16, item.name);
        record[5] = FindOrAddUtf16(utf16, item.desc);
        record[17] = item.price; record[18] = item.price / 2; record[19] = 0;
        for (int field : {23,24,25,26,27,31,32,33,34}) record[field] = 0;
        record[20] = (item.id == 30153 || item.id == 30157) ? 100 : 0;
        // Clear unrelated template bonuses, then apply the requested accessory bonuses.
        record[21] = item.id == 30157 ? 200 : ((item.id == 30152 || item.id == 30155) ? 100 : 0);
        record[23] = item.stat[0]; record[24] = item.stat[1];
        record[25] = item.stat[2]; record[26] = item.stat[3];
        record[27] = item.stat[4]; record[31] = item.stat[5]; record[32] = item.stat[6];
        record[33] = item.stat[7]; record[34] = item.stat[8];
        for (int field = 72; field <= 78; ++field) record[field] = 0;
        for (int field = 87; field <= 98; ++field) record[field] = 0;
        record[86] = 0;
        if (item.id == 30159) { record[31] = 30; record[68] = 200; }
        if (item.id == 30160 || item.id == 30157) {
            record[86] = 2;
            for (int field = 87; field <= 98; ++field) record[field] = 999;
            // Stop is the next CHARA_STATUS_TYPE after Death in this game's
            // status enum. The earlier charm implementation stopped at Death.
            record[99] = 999;
        }
        if (item.id == 30161 || item.id == 30157)
            for (int field = 72; field <= 78; ++field) record[field] = 4;
        record[40] = 1;
        auto target = haveAllDummies && n < std::size(dummyIds) ? existing(dummyIds[n]) : existing(item.id);
        if (target == rows.end() && item.id == 30162) target = existing(90045);
        if (target == rows.end()) {
            if (rows.size() >= 600) { reason = L"The item table has no safe space for all requested charms under the game's 600-item limit."; return false; }
            int sortKey = 0;
            for (const auto& row : rows) sortKey = (std::max)(sortKey, row[3]);
            record[3] = sortKey + 1;
            rows.push_back(std::move(record));
            continue;
        }
        if (target == rows.end()) { reason = L"An item-table replacement row could not be resolved."; return false; }
        // Keep the source row's sort key so migrations and repeat installs are
        // deterministic and do not perturb unrelated table ordering.
        record[3] = (*target)[3];
        *target = std::move(record);
    }
    {
        // Reuse the reserved dummy row without increasing the game's fixed
        // item count. Scouter is a normal equipable accessory, not a battle item.
        auto scouter = existing(30163);
        if (scouter == rows.end()) scouter = existing(90050);
        if (scouter == rows.end() || ((*scouter)[1] != 18 && (*scouter)[1] != 16 && (*scouter)[1] != 15)) {
            reason = L"The reserved Dummy Key Item 10 row for Scouter is missing or has an unsupported type.";
            return false;
        }
        const int sortKey = (*scouter)[3];
        auto accessory = templateRecord;
        accessory[0] = 30163; accessory[1] = 15; accessory[2] = 1; accessory[3] = sortKey;
        accessory[4] = FindOrAddUtf16(utf16, L"Scouter");
        accessory[5] = FindOrAddUtf16(utf16, L"Reveals enemy HP and weaknesses while equipped.");
        accessory[17] = 20; accessory[18] = 10; accessory[19] = 0;
        for (int field : {23,24,25,26,27,31,32,33,34}) accessory[field] = 0;
        for (int field = 72; field <= 78; ++field) accessory[field] = 0;
        for (int field = 87; field <= 98; ++field) accessory[field] = 0;
        accessory[86] = 0; accessory[40] = 1;
        *scouter = std::move(accessory);
    }
    if (patchEnglishExtras) {

        // Petal Token is already a game item; make it purchasable at the
        // same 500 pg price as the community shop tweak.
        auto petal = existing(40126);
        if (petal == rows.end() || (*petal)[1] != 16 || ReadUtf16(utf16, (*petal)[4]) != L"Petal Token") {
            reason = L"The Petal Token item row is missing or has an unsupported layout.";
            return false;
        }
        (*petal)[17] = 500; (*petal)[18] = 125; (*petal)[19] = 0;
    }
    if (rows.size() > 600) { reason = L"The patched item table would exceed the game's 600-item inventory limit."; return false; }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a[0] < b[0]; });
    input = WriteBtb(rows, size, ascii, utf16);
    reason.clear(); return true;
}

bool PatchKleptomaniacAbility(std::vector<uint8_t>& input, std::wstring& reason) {
    std::vector<std::vector<int32_t>> rows; std::vector<uint8_t> ascii, utf16; uint32_t size = 0;
    if (!ReadBtb(input, rows, size, ascii, utf16, reason)) return false;
    if (size != 67 * 4 || rows.empty()) { reason = L"The support ability table has an unsupported layout."; return false; }
    auto find = [&](int id) { return std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row[0] == id; }); };
    auto source = find(1527);
    if (source == rows.end()) { reason = L"The Rob Blind ability record is missing."; return false; }
    auto ability = *source;
    ability[0] = 1566;
    auto target = find(1566);
    if (target == rows.end()) rows.push_back(std::move(ability));
    else *target = std::move(ability);
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a[0] < b[0]; });
    input = WriteBtb(rows, size, ascii, utf16);
    reason.clear(); return true;
}

bool PatchKleptomaniacAbilityAdditional(std::vector<uint8_t>& input, std::wstring& reason) {
    std::vector<std::vector<int32_t>> rows; std::vector<uint8_t> ascii, utf16; uint32_t size = 0;
    if (!ReadBtb(input, rows, size, ascii, utf16, reason)) return false;
    if (size != 3 * 4 || rows.empty()) { reason = L"The support ability additional table has an unsupported layout."; return false; }
    auto source = std::find_if(rows.begin(), rows.end(), [](const auto& row) { return row[0] == 1527; });
    if (source == rows.end()) { reason = L"The Rob Blind additional record is missing."; return false; }
    auto ability = *source; ability[0] = 1566;
    auto target = std::find_if(rows.begin(), rows.end(), [](const auto& row) { return row[0] == 1566; });
    if (target == rows.end()) rows.push_back(std::move(ability)); else *target = std::move(ability);
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a[0] < b[0]; });
    input = WriteBtb(rows, size, ascii, utf16);
    reason.clear(); return true;
}

bool PatchKleptomaniacAbilityDetails(std::vector<uint8_t>& input, std::wstring& reason) {
    std::vector<std::vector<int32_t>> rows; std::vector<uint8_t> ascii, utf16; uint32_t size = 0;
    if (!ReadBtb(input, rows, size, ascii, utf16, reason)) return false;
    if (size != 3 * 4 || rows.empty()) { reason = L"The support ability detail table has an unsupported layout."; return false; }
    auto source = std::find_if(rows.begin(), rows.end(), [](const auto& row) { return row[0] == 1527; });
    if (source == rows.end()) { reason = L"The Rob Blind detail record is missing."; return false; }
    auto ability = *source; ability[0] = 1566;
    ability[1] = FindOrAddUtf16(utf16, L"Kleptomaniac");
    ability[2] = FindOrAddUtf16(utf16, L"Grants an extra independent roll for stolen items.");
    auto target = std::find_if(rows.begin(), rows.end(), [](const auto& row) { return row[0] == 1566; });
    if (target == rows.end()) rows.push_back(std::move(ability)); else *target = std::move(ability);
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a[0] < b[0]; });
    input = WriteBtb(rows, size, ascii, utf16);
    reason.clear(); return true;
}

bool PatchFreelancerStealAbility(std::vector<uint8_t>& input, std::wstring& reason) {
    std::vector<std::vector<int32_t>> rows; std::vector<uint8_t> ascii, utf16; uint32_t size = 0;
    if (!ReadBtb(input, rows, size, ascii, utf16, reason)) return false;
    if (size != 106 * 4 || rows.empty()) { reason = L"The command ability table has an unsupported layout."; return false; }
    auto find = [&](int id) { return std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row[0] == id; }); };
    auto source = find(521);
    if (source == rows.end()) { reason = L"The native Steal command ability record is missing."; return false; }
    auto ability = *source;
    ability[0] = 872;
    ability[2] = 2024; // Freelancer command set
    ability[3] = 1;    // Make Steal available at Freelancer job level 1
    auto target = find(872);
    if (target == rows.end()) rows.push_back(std::move(ability)); else *target = std::move(ability);
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a[0] < b[0]; });
    input = WriteBtb(rows, size, ascii, utf16);
    reason.clear(); return true;
}

bool PatchFreelancerStealAbilityAdditional(std::vector<uint8_t>& input, std::wstring& reason) {
    std::vector<std::vector<int32_t>> rows; std::vector<uint8_t> ascii, utf16; uint32_t size = 0;
    if (!ReadBtb(input, rows, size, ascii, utf16, reason)) return false;
    if (size != 3 * 4 || rows.empty()) { reason = L"The command ability additional table has an unsupported layout."; return false; }
    auto source = std::find_if(rows.begin(), rows.end(), [](const auto& row) { return row[0] == 521; });
    if (source == rows.end()) { reason = L"The native Steal command's additional record is missing."; return false; }
    auto ability = *source; ability[0] = 872;
    auto target = std::find_if(rows.begin(), rows.end(), [](const auto& row) { return row[0] == 872; });
    if (target == rows.end()) rows.push_back(std::move(ability)); else *target = std::move(ability);
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a[0] < b[0]; });
    input = WriteBtb(rows, size, ascii, utf16);
    reason.clear(); return true;
}

bool PatchFreelancerStealAbilityDetails(std::vector<uint8_t>& input, std::wstring& reason) {
    std::vector<std::vector<int32_t>> rows; std::vector<uint8_t> ascii, utf16; uint32_t size = 0;
    if (!ReadBtb(input, rows, size, ascii, utf16, reason)) return false;
    if (size != 4 * 4 || rows.empty()) { reason = L"The command detail table has an unsupported layout."; return false; }
    auto source = std::find_if(rows.begin(), rows.end(), [](const auto& row) { return row[0] == 521; });
    if (source == rows.end()) { reason = L"The native Steal command's detail record is missing."; return false; }
    auto ability = *source; ability[0] = 872;
    ability[2] = FindOrAddUtf16(utf16, L"Steal");
    ability[3] = FindOrAddUtf16(utf16, L"Has a 25% chance to steal an item from an enemy.");
    auto target = std::find_if(rows.begin(), rows.end(), [](const auto& row) { return row[0] == 872; });
    if (target == rows.end()) rows.push_back(std::move(ability)); else *target = std::move(ability);
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a[0] < b[0]; });
    input = WriteBtb(rows, size, ascii, utf16);
    reason.clear(); return true;
}

bool PatchFreelancerJobTable(std::vector<uint8_t>& input, std::wstring& reason) {
    std::vector<std::vector<int32_t>> rows; std::vector<uint8_t> ascii, utf16; uint32_t size = 0;
    if (!ReadBtb(input, rows, size, ascii, utf16, reason)) return false;
    if (size != 17 * 4 || rows.empty() || rows[0][0] != 1 || rows[0][16] != 2024) {
        reason = L"Freelancer's level-one ability table has an unsupported layout."; return false;
    }
    auto& abilities = rows[0];
    if (std::find(abilities.begin() + 13, abilities.begin() + 16, 872) == abilities.begin() + 16) {
        auto empty = std::find(abilities.begin() + 13, abilities.begin() + 16, 0);
        if (empty == abilities.begin() + 16) { reason = L"Freelancer's level-one ability slots are full."; return false; }
        *empty = 872;
    }
    input = WriteBtb(rows, size, ascii, utf16);
    reason.clear(); return true;
}

bool PatchEarlyMonsterDrops(std::vector<uint8_t>& input, const std::vector<uint8_t>* baseline,
                            std::wstring& reason) {
    std::vector<std::vector<int32_t>> rows; std::vector<uint8_t> ascii, utf16; uint32_t size = 0;
    if (!ReadBtb(input, rows, size, ascii, utf16, reason)) return false;
    if (size != 113 * 4 || rows.size() != 485) { reason = L"The monster drop table has an unsupported layout."; return false; }
    std::vector<std::vector<int32_t>> baselineRows;
    if (baseline && !baseline->empty()) {
        std::vector<uint8_t> baselineAscii, baselineUtf16; uint32_t baselineSize = 0;
        if (!ReadBtb(*baseline, baselineRows, baselineSize, baselineAscii, baselineUtf16, reason) ||
            baselineSize != size || baselineRows.size() != rows.size()) {
            reason = L"The original monster drop backup has an unsupported layout."; return false;
        }
    }
    size_t changed = 0;
    for (auto& monster : rows) {
        // These are the low-level field monsters available before the Thief
        // asterisk route. Merge rather than replace the existing drop list so
        // status cures and other vanilla drops remain obtainable.
        if (monster[0] >= 50100 && monster[0] <= 51331 && monster[3] <= 15) {
            const std::vector<int32_t>* original = nullptr;
            if (!baselineRows.empty()) {
                auto base = std::find_if(baselineRows.begin(), baselineRows.end(), [&](const auto& row) { return row[0] == monster[0]; });
                if (base != baselineRows.end()) original = &*base;
            }
            bool wasOldDropPatch = original && monster[94] == 40000 && monster[96] == 40001;
            std::vector<std::pair<int32_t,int32_t>> drops;
            auto addDrop = [&](int32_t itemId, int32_t chance) {
                if (itemId <= 0) return;
                auto existing = std::find_if(drops.begin(), drops.end(), [&](const auto& drop) { return drop.first == itemId; });
                if (existing == drops.end()) drops.emplace_back(itemId, chance > 0 ? chance : 5);
            };
            auto mergeDrops = [&](const std::vector<int32_t>& row, bool skipOldInsertions) {
                for (int slot = 0; slot < 5; ++slot) {
                    const int itemCol = 94 + slot * 2;
                    if (skipOldInsertions && ((slot == 0 && row[itemCol] == 40000) ||
                                              (slot == 1 && row[itemCol] == 40001))) continue;
                    addDrop(row[itemCol], row[itemCol + 1]);
                }
            };
            if (original) mergeDrops(*original, false);
            mergeDrops(monster, wasOldDropPatch);
            auto ensureDrop = [&](int32_t itemId, int32_t defaultChance) {
                auto found = std::find_if(drops.begin(), drops.end(), [&](const auto& drop) { return drop.first == itemId; });
                if (found == drops.end()) drops.emplace_back(itemId, defaultChance);
                else if (found->second <= 0) found->second = defaultChance;
            };
            ensureDrop(40000, 5); // Potion, common drop
            ensureDrop(40001, 1); // Ether, rare drop
            if (drops.size() > 5) {
                reason = L"An early monster's existing drop list is full; the patch will not remove its cure or other drops.";
                return false;
            }
            for (int slot = 0; slot < 5; ++slot) {
                const int itemCol = 94 + slot * 2;
                monster[itemCol] = slot < static_cast<int>(drops.size()) ? drops[slot].first : 0;
                monster[itemCol + 1] = slot < static_cast<int>(drops.size()) ? drops[slot].second : 0;
            }
            ++changed;
        }
    }
    if (changed < 5) { reason = L"Too few early-game monsters matched the supported drop-table range."; return false; }
    input = WriteBtb(rows, size, ascii, utf16);
    reason.clear(); return true;
}

bool PatchAsteriskBossSteals(std::vector<uint8_t>& input, std::wstring& reason) {
    std::vector<std::vector<int32_t>> rows; std::vector<uint8_t> ascii, utf16; uint32_t size = 0;
    if (!ReadBtb(input, rows, size, ascii, utf16, reason)) return false;
    if (size != 113 * 4 || rows.size() != 485) { reason = L"The monster steal table has an unsupported layout."; return false; }
    const struct BossSteal { int monsterId; int rareItemId; } bossSteals[] = {
        {65000,10005}, // Knight: Defender
        {65100,10301}, // Black Mage: Wizard's Rod
        {67000,10511}, // Thief: Kunai
        {65200,10406}, // White Mage: Sage's Staff
        {65300,10806}, // Monk: Toxic Claws (game item name)
        {67100,20010}, // Merchant: Blessed Shield
        {65800,10518}  // Spellfencer: Magic Knife
    };
    for (const auto& boss : bossSteals) {
        auto row = std::find_if(rows.begin(), rows.end(), [&](const auto& record) { return record[0] == boss.monsterId; });
        if (row == rows.end()) { reason = L"A required asterisk boss row is missing from the monster table."; return false; }
        (*row)[104] = boss.monsterId == 65800 ? 10011 : 40009; // Rune Blade for Spellfencer, Hi-Potion otherwise
        if ((*row)[105] <= 0) (*row)[105] = 100;
        (*row)[106] = boss.rareItemId;
        if ((*row)[107] <= 0) (*row)[107] = 1;
    }
    input = WriteBtb(rows, size, ascii, utf16);
    reason.clear(); return true;
}

bool PatchMonsterData(std::vector<uint8_t>& input, const std::vector<uint8_t>* baseline,
                      std::wstring& reason) {
    if (!PatchEarlyMonsterDrops(input, baseline, reason)) return false;
    return PatchAsteriskBossSteals(input, reason);
}

bool PatchShop(std::vector<uint8_t>& input, std::wstring& reason, bool addScouter) {
    std::vector<std::vector<int32_t>> rows; std::vector<uint8_t> ascii, utf16; uint32_t size = 0;
    if (!ReadBtb(input, rows, size, ascii, utf16, reason)) return false;
    if (size != 46 * 4 || rows.size() != 11) { reason = L"The Norende reward table layout is not supported."; return false; }
    auto& shop = rows[7];
    const int entries[][3] = {{4,30151,30152},{5,30153,30154},{6,30155,30156},{7,30159,30160},{8,30161,0},{9,30162,0},{10,30157,30158}};
    for (const auto& entry : entries) {
        for (int column = 0; column < (entry[2] ? 2 : 1); ++column) {
            int pos = entry[0] * 4 + 2 + column;
            int id = entry[column + 1];
            if (shop[pos] && shop[pos] != id) { reason = L"A Norende accessory shop slot is already occupied."; return false; }
            shop[pos] = id;
        }
    }
    if (addScouter) {
        auto trader = std::find_if(rows.begin(), rows.end(), [](const auto& row) { return row[0] == 15; });
        if (trader == rows.end() || trader->size() <= 3 || ((*trader)[3] && (*trader)[3] != 30163 && (*trader)[3] != 90050)) {
            reason = L"The supported Trader Village shop row for Scouter is missing or already occupied.";
            return false;
        }
        (*trader)[3] = 30163;
    }
    input = WriteBtb(rows, size, ascii, utf16);
    reason.clear(); return true;
}

bool PatchScouterDetails(std::vector<uint8_t>& input, std::wstring& reason) {
    std::vector<std::vector<int32_t>> rows; std::vector<uint8_t> ascii, utf16; uint32_t size = 0;
    if (!ReadBtb(input, rows, size, ascii, utf16, reason)) return false;
    if (size != 3 * 4) { reason = L"The item detail table has an unsupported layout."; return false; }
    auto row = std::find_if(rows.begin(), rows.end(), [](const auto& record) { return record[0] == 30163 || record[0] == 90050; });
    if (row == rows.end()) { reason = L"The Scouter detail row is missing from the English item detail table."; return false; }
    (*row)[0] = 30163;
    (*row)[1] = FindOrAddUtf16(utf16, L"Scouter");
    (*row)[2] = FindOrAddUtf16(utf16, L"Reveals enemy HP and weaknesses while equipped.");
    input = WriteBtb(rows, size, ascii, utf16);
    reason.clear(); return true;
}

bool PatchFloremItemShop(std::vector<uint8_t>& input, std::wstring& reason) {
    std::vector<std::vector<int32_t>> rows; std::vector<uint8_t> ascii, utf16; uint32_t size = 0;
    if (!ReadBtb(input, rows, size, ascii, utf16, reason)) return false;
    if (size != 2 * 4 || (rows.size() != 11 && rows.size() != 12)) {
        reason = L"The Florem item shop table has an unsupported layout."; return false;
    }
    bool hasPotion = false, hasPetal = false;
    for (const auto& row : rows) {
        if (row[0] == 40000) hasPotion = true;
        if (row[0] == 40126) hasPetal = true;
    }
    // Migrate the previous replacement-style install: Potion used to occupy
    // the first slot and was replaced by Petal Token. Restore it there, then
    // add a separate purchasable Petal Token row at the end.
    if (!hasPotion && !rows.empty() && rows.front()[0] == 40126) {
        rows.front()[0] = 40000;
        hasPotion = true;
        hasPetal = false;
    }
    if (!hasPotion) {
        reason = L"The expected Potion entry is missing from the Florem item shop.";
        return false;
    }
    if (!hasPetal) {
        if (rows.size() != 11) {
            reason = L"The Florem item shop has no safe entry for a separate Petal Token listing.";
            return false;
        }
        rows.push_back({40126, 1});
        hasPetal = true;
    }
    if (!hasPotion || !hasPetal) {
        reason = L"The Florem item shop could not retain both Potion and Petal Token entries.";
        return false;
    }
    input = WriteBtb(rows, size, ascii, utf16);
    reason.clear(); return true;
}

std::wstring SelectGameAssembly() {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter = L"GameAssembly.dll\0GameAssembly.dll\0All files\0*.*\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    dialog.lpstrTitle = L"Select the game's GameAssembly.dll";
    if (!GetOpenFileNameW(&dialog)) return {};
    return path;
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int) {
    std::wstring selected;
    bool quiet = false;
    if (commandLine && *commandLine) {
        std::wstring raw = commandLine;
        while (!raw.empty() && iswspace(raw.front())) raw.erase(raw.begin());
        if (!raw.empty() && raw.front() == L'"') {
            size_t end = raw.find(L'"', 1);
            selected = end == std::wstring::npos ? raw.substr(1) : raw.substr(1, end - 1);
            raw = end == std::wstring::npos ? L"" : raw.substr(end + 1);
        } else {
            size_t end = raw.find_first_of(L" \t");
            selected = raw.substr(0, end);
            raw = end == std::wstring::npos ? L"" : raw.substr(end + 1);
        }
        while (!raw.empty() && iswspace(raw.front())) raw.erase(raw.begin());
        quiet = raw == L"--quiet";
    } else selected = SelectGameAssembly();
    if (selected.empty()) return 0;
    auto notify = [&](const std::wstring& message, bool error = false) {
        if (!quiet) { MessageBoxW(nullptr, message.c_str(), L"BDFFHD game-file patch", error ? MB_ICONERROR : MB_ICONINFORMATION); return; }
        wchar_t tempPath[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tempPath);
        std::ofstream log(std::filesystem::path(tempPath) / L"BDFFHDNativePatch.log", std::ios::trunc);
        log << (error ? "ERROR: " : "OK: ");
        for (wchar_t c : message) log << (c < 128 ? static_cast<char>(c) : '?');
    };
    std::filesystem::path gameAssembly(selected);
    std::filesystem::path streaming = gameAssembly.parent_path() / L"BDFFHD_Data" / L"StreamingAssets";
    struct FileUpdate { std::filesystem::path path; std::vector<uint8_t> contents; bool preserveExistingBackup = false; };
    std::vector<FileUpdate> updates;
    auto load = [&](const std::filesystem::path& path, FileUpdate*& item) -> bool {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) return false;
        updates.push_back({path, std::vector<uint8_t>((std::istreambuf_iterator<char>(stream)), {}), false});
        item = &updates.back();
        return true;
    };
    FileUpdate* assemblyUpdate = nullptr;
    std::wstring reason;
    bool assemblyAlreadyPatched = false;
    bool rebuiltExistingPatch = false;
    std::string currentAssemblyHash;
    {
        std::ifstream stream(gameAssembly, std::ios::binary);
        std::vector<uint8_t> current((std::istreambuf_iterator<char>(stream)), {});
        if ((stream.good() || stream.eof()) && HashFile(current, currentAssemblyHash))
            assemblyAlreadyPatched = currentAssemblyHash == kAlreadyNativePatchedHash ||
                                     currentAssemblyHash == kAlreadyNativePatchedFriendOrderHash ||
                                     currentAssemblyHash == kAlreadyNativePatchedOldFriendOrderHash ||
                                     currentAssemblyHash == kAlreadyLimitBreakCharmPatchedHash ||
                                     currentAssemblyHash == kAlreadyCharmAbilitiesPatchedHash ||
                                     currentAssemblyHash == kAlreadyGraceDraftPatchedHash ||
                                     currentAssemblyHash == kAlreadyGraceOfGodsPatchedHash ||
                                     currentAssemblyHash == kAlreadyGuaranteedStealPatchedHash ||
                                     currentAssemblyHash == kAlreadyScouterKleptomaniacPatchedHash;
    }
    const bool hasLegacyUnsafeHook = std::any_of(std::begin(kLegacyUnsafeNativePatchedHashes),
        std::end(kLegacyUnsafeNativePatchedHashes), [&](const char* hash) { return currentAssemblyHash == hash; });
    if (hasLegacyUnsafeHook) {
        std::filesystem::path backup = gameAssembly.wstring() + L".bdffhd-backup";
        std::ifstream stream(backup, std::ios::binary);
        if (!stream) {
            notify(L"This GameAssembly has an older unsafe M.Atk hook and no adjacent .bdffhd-backup. Restore the original GameAssembly.dll with Steam file verification, then run this patcher again.", true);
            return 1;
        }
        updates.push_back({gameAssembly, std::vector<uint8_t>((std::istreambuf_iterator<char>(stream)), {}), true});
        assemblyUpdate = &updates.back();
        if (!Patch(assemblyUpdate->contents, reason)) {
            notify(L"The saved GameAssembly backup is not a supported clean/dialogue-patched build. Restore the original game assembly with Steam file verification.", true);
            return 1;
        }
        rebuiltExistingPatch = true;
    } else if (assemblyAlreadyPatched) {
        // Rebuild known earlier releases from the first-known-good adjacent
        // backup so each native hook is installed exactly once and safely.
        std::filesystem::path backup = gameAssembly.wstring() + L".bdffhd-backup";
        std::ifstream stream(backup, std::ios::binary);
        if (!stream) {
            notify(L"This installed native patch needs an update, but its original .bdffhd-backup is missing. Restore the original GameAssembly.dll with Steam file verification, then run this patcher again.", true);
            return 1;
        }
        updates.push_back({gameAssembly, std::vector<uint8_t>((std::istreambuf_iterator<char>(stream)), {}), true});
        assemblyUpdate = &updates.back();
        if (!Patch(assemblyUpdate->contents, reason)) {
            notify(L"The saved GameAssembly backup is not a supported clean/dialogue-patched build. Restore the original game assembly with Steam file verification.", true);
            return 1;
        }
        rebuiltExistingPatch = true;
    } else {
        if (!load(gameAssembly, assemblyUpdate) || !Patch(assemblyUpdate->contents, reason)) {
            if (reason.empty()) reason = L"Could not read the selected GameAssembly.dll.";
            notify(reason, true);
            return 1;
        }
    }
    const std::filesystem::path itemPaths[] = {
        streaming / L"Common" / L"Paramater" / L"ItemTable.btb",
        streaming / L"Common_en" / L"Paramater" / L"ItemTable.btb",
        streaming / L"Common" / L"Colony" / L"PlantParameter.btb",
        streaming / L"Common_en" / L"Colony" / L"PlantParameter.btb"
    };
    for (int i = 0; i < 4; ++i) {
        FileUpdate* item = nullptr;
        if (!load(itemPaths[i], item)) {
            reason = L"Could not read the required game data table: " + itemPaths[i].wstring();
            notify(reason, true); return 1;
        }
        bool ok = i < 2 ? PatchItems(item->contents, reason, i == 1)
                        : PatchShop(item->contents, reason, i == 3);
        if (!ok) { notify(reason, true); return 1; }
    }
    struct SupportTablePatch { const wchar_t* relativePath; bool (*patch)(std::vector<uint8_t>&, std::wstring&); };
    const SupportTablePatch supportPatches[] = {
        {L"Common\\Paramater\\SupportAbility.btb", PatchKleptomaniacAbility},
        {L"Common\\Paramater\\SupportAbilityAL.btb", PatchKleptomaniacAbility},
        {L"Common\\Paramater\\SupportAbilityAdditional.btb", PatchKleptomaniacAbilityAdditional},
        {L"Common\\Paramater\\DetailInfoSupportTable.btb", PatchKleptomaniacAbilityDetails},
        {L"Common_en\\Paramater\\SupportAbility.btb", PatchKleptomaniacAbility},
        {L"Common_en\\Paramater\\SupportAbilityAL.btb", PatchKleptomaniacAbility},
        {L"Common_en\\Paramater\\DetailInfoSupportTable.btb", PatchKleptomaniacAbilityDetails}
    };
    for (const auto& patch : supportPatches) {
        FileUpdate* item = nullptr;
        const auto path = streaming / patch.relativePath;
        if (!load(path, item) || !patch.patch(item->contents, reason)) {
            if (reason.empty()) reason = L"Could not read a required support ability table: " + path.wstring();
            notify(reason, true); return 1;
        }
    }
    const SupportTablePatch freelancerPatches[] = {
        {L"Common\\Paramater\\CommandAbility.btb", PatchFreelancerStealAbility},
        {L"Common\\Paramater\\CommandAbilityAL.btb", PatchFreelancerStealAbility},
        {L"Common\\Paramater\\CommandAbilityAdditional.btb", PatchFreelancerStealAbilityAdditional},
        {L"Common\\Paramater\\DetailInfoCommandTable.btb", PatchFreelancerStealAbilityDetails},
        {L"Common\\Paramater\\JobTable00.btb", PatchFreelancerJobTable},
        {L"Common_en\\Paramater\\CommandAbility.btb", PatchFreelancerStealAbility},
        {L"Common_en\\Paramater\\CommandAbilityAL.btb", PatchFreelancerStealAbility},
        {L"Common_en\\Paramater\\DetailInfoCommandTable.btb", PatchFreelancerStealAbilityDetails},
        {L"Common_en\\Paramater\\JobTable00.btb", PatchFreelancerJobTable}
    };
    for (const auto& patch : freelancerPatches) {
        FileUpdate* item = nullptr;
        const auto path = streaming / patch.relativePath;
        if (!load(path, item) || !patch.patch(item->contents, reason)) {
            if (reason.empty()) reason = L"Could not read a required Freelancer Steal table: " + path.wstring();
            notify(reason, true); return 1;
        }
    }
    const wchar_t* monsterTables[] = {
        L"Common\\Battle\\MonsterData.btb",
        L"Common_en\\Battle\\MonsterData.btb"
    };
    for (const auto* relativePath : monsterTables) {
        FileUpdate* item = nullptr;
        const auto path = streaming / relativePath;
        if (!load(path, item)) {
            if (reason.empty()) reason = L"Could not read a required monster drop/steal table: " + path.wstring();
            notify(reason, true); return 1;
        }
        std::vector<uint8_t> baseline;
        std::ifstream backup(path.wstring() + L".bdffhd-backup", std::ios::binary);
        if (backup) baseline.assign(std::istreambuf_iterator<char>(backup), {});
        if (!PatchMonsterData(item->contents, baseline.empty() ? nullptr : &baseline, reason)) {
            notify(reason, true); return 1;
        }
    }
    {
        FileUpdate* item = nullptr;
        const auto path = streaming / L"Common_en" / L"Paramater" / L"DetailInfoItemTable.btb";
        if (!load(path, item) || !PatchScouterDetails(item->contents, reason)) {
            if (reason.empty()) reason = L"Could not read the English item detail table.";
            notify(reason, true); return 1;
        }
    }
    {
        FileUpdate* item = nullptr;
        const auto path = streaming / L"Common_en" / L"Shop" / L"TW_13_Item.spb";
        if (!load(path, item) || !PatchFloremItemShop(item->contents, reason)) {
            if (reason.empty()) reason = L"Could not read the Florem item shop table.";
            notify(reason, true); return 1;
        }
    }
    // Avoid touching unchanged data tables. This also allows a safe reapply
    // when an earlier release already left rollback sidecars beside them.
    updates.erase(std::remove_if(updates.begin(), updates.end(), [](const FileUpdate& update) {
        std::ifstream original(update.path, std::ios::binary);
        if (!original) return false;
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(original)), {});
        return bytes == update.contents;
    }), updates.end());
    for (auto& update : updates) {
        std::wstring source = update.path.wstring();
        std::wstring backup = source + L".bdffhd-backup";
        if (GetFileAttributesW(backup.c_str()) != INVALID_FILE_ATTRIBUTES) {
            // Preserve the first-known-good rollback copy across patcher updates.
            // Never replace an existing backup with an already modified file.
            update.preserveExistingBackup = true;
        }
    }
    std::vector<std::filesystem::path> temps;
    for (const auto& update : updates) {
        std::filesystem::path temp = update.path; temp += L".bdffhd-temp";
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(update.contents.data()), static_cast<std::streamsize>(update.contents.size()));
        output.close();
        if (!output) {
            for (const auto& p : temps) DeleteFileW(p.c_str());
            DeleteFileW(temp.c_str());
            notify(L"Could not stage all game-file changes. Nothing was installed.", true); return 1;
        }
        temps.push_back(temp);
    }
    for (const auto& update : updates) {
        std::wstring source = update.path.wstring();
        std::wstring backup = source + L".bdffhd-backup";
        if (!update.preserveExistingBackup && !CopyFileW(source.c_str(), backup.c_str(), TRUE)) {
            for (const auto& p : temps) DeleteFileW(p.c_str());
            notify(L"Could not back up every original game file. No game files were changed.", true); return 1;
        }
    }
    for (size_t i = 0; i < updates.size(); ++i) {
        if (!MoveFileExW(temps[i].c_str(), updates[i].path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            notify(L"A file could not be replaced. Original backups are beside the game files with a .bdffhd-backup suffix; restore them before retrying.", true); return 1;
        }
    }
    const wchar_t* success = rebuiltExistingPatch
        ? L"The native game patch now includes the equip-only Scouter accessory, Kleptomaniac's independent steal roll, and Freelancer's Steal command from the start. Early enemies now drop Potions and Ethers, and asterisk bosses have the updated steals. Buyable Petal Tokens and Grace of Gods are included too. Original backups were preserved. Start the game normally; no mod loader is required."
        : L"Native M.Atk weapon proficiency, all charm effects, the equip-only Scouter accessory, Kleptomaniac's independent steal roll, Freelancer's Steal command, early Potion/Ether drops, updated asterisk boss steals, buyable Petal Tokens, Grace of Gods, dialogue timing, and Norende/shop edits are installed in the game files. Rob Blind and Kleptomaniac use separate game-native item rolls. The item table stays within the game's 600-item limit. Original files were backed up beside them with a .bdffhd-backup suffix. Start the game normally; no mod loader is required.";
    notify(success);
    return 0;
}

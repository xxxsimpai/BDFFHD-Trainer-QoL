#include "common.h"
#include <fstream>
#include <sstream>

std::wstring DirOf(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? L"." : p.substr(0, i);
}
std::wstring ModuleDir(HMODULE m) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(m, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    return DirOf(std::wstring(buf, n));
}
std::string Narrow(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string o(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &o[0], n, nullptr, nullptr);
    return o;
}
std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring o(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &o[0], n);
    return o;
}
std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out; std::wstring cur;
    for (wchar_t c : s) { if (c == sep) { out.push_back(cur); cur.clear(); } else cur += c; }
    if (!cur.empty() || !out.empty()) out.push_back(cur);
    return out;
}

static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n"), b = s.find_last_not_of(L" \t\r\n");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

std::vector<IniSection> LoadIni(const std::wstring& path) {
    std::vector<IniSection> out;
    std::ifstream in(path, std::ios::binary);
    if (!in) return out;
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() >= 3 && (unsigned char)line[0] == 0xEF) line.erase(0, 3);  // BOM
        std::wstring w = Trim(Widen(line));
        if (w.empty() || w[0] == L';' || w[0] == L'#') continue;
        if (w.front() == L'[' && w.back() == L']') { out.push_back({Trim(w.substr(1, w.size() - 2)), {}}); continue; }
        size_t eq = w.find(L'=');
        if (eq == std::wstring::npos || out.empty()) continue;
        out.back().kv[Trim(w.substr(0, eq))] = Trim(w.substr(eq + 1));
    }
    return out;
}
std::wstring Get(const IniSection& s, const std::wstring& key, const std::wstring& def) {
    auto it = s.kv.find(key);
    return it == s.kv.end() ? def : it->second;
}

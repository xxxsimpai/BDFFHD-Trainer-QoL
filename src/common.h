#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <map>
#include <string>
#include <vector>

std::wstring DirOf(const std::wstring& path);
std::wstring ModuleDir(HMODULE m);
std::string  Narrow(const std::wstring& s);
std::wstring Widen(const std::string& s);
std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep);

// Minimal INI: [section] key=value, full-line ';' or '#' comments. Section order preserved.
struct IniSection { std::wstring name; std::map<std::wstring, std::wstring> kv; };
std::vector<IniSection> LoadIni(const std::wstring& path);
std::wstring Get(const IniSection& s, const std::wstring& key, const std::wstring& def = L"");

#pragma once

#include <string>
#include "rapidjson/document.h"

#define DebugA(...) { char msg[2048]; sprintf_s(msg, __VA_ARGS__); OutputDebugStringA(msg); }
#define DebugW(...) { wchar_t msg[2048]; wsprintf(msg, __VA_ARGS__); OutputDebugStringW(msg); }

struct Tools {
    static std::wstring ToWString(const std::string &);
    static std::wstring ToWString(const char *);
    static std::wstring ToWString(const rapidjson::Value &);

    static std::wstring UrlEncode(const std::wstring &);
    static void OutputLastError();
};


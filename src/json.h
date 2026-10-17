// Minimal JSON parser (enough for bbsmenu.json)
#pragma once
#include <string>
#include <vector>
#include <utility>
#include <cstdlib>
#include <cwchar>

struct JV {
    enum Type { Null, Bool, Num, Str, Arr, Obj } t = Null;
    std::wstring s;
    double n = 0;
    std::vector<JV> a;
    std::vector<std::pair<std::wstring, JV>> o;

    const JV* get(const wchar_t* key) const {
        for (auto& kv : o) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    std::wstring str(const wchar_t* key) const {
        const JV* v = get(key);
        return (v && v->t == Str) ? v->s : std::wstring();
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::wstring& src) : m_s(src), m_p(0) {}
    bool parse(JV& out) {
        try { ws(); out = value(); return true; } catch (...) { return false; }
    }
private:
    const std::wstring& m_s;
    size_t m_p;
    struct Err {};
    void ws() { while (m_p < m_s.size() && (m_s[m_p] == ' ' || m_s[m_p] == '\t' || m_s[m_p] == '\n' || m_s[m_p] == '\r' || m_s[m_p] == 0xFEFF)) m_p++; }
    wchar_t cur() { if (m_p >= m_s.size()) throw Err(); return m_s[m_p]; }
    void expect(wchar_t c) { if (cur() != c) throw Err(); m_p++; }
    bool lit(const wchar_t* w) {
        size_t n = wcslen(w);
        if (m_s.compare(m_p, n, w) == 0) { m_p += n; return true; }
        return false;
    }
    static void put(std::wstring& r, unsigned long cp) {
        if (sizeof(wchar_t) == 2 && cp > 0xFFFF) {
            cp -= 0x10000;
            r += (wchar_t)(0xD800 + (cp >> 10));
            r += (wchar_t)(0xDC00 + (cp & 0x3FF));
        } else r += (wchar_t)cp;
    }
    unsigned hex4() {
        if (m_p + 4 > m_s.size()) throw Err();
        unsigned v = (unsigned)wcstoul(m_s.substr(m_p, 4).c_str(), nullptr, 16);
        m_p += 4;
        return v;
    }
    std::wstring string() {
        expect(L'"');
        std::wstring r;
        for (;;) {
            wchar_t c = cur(); m_p++;
            if (c == L'"') break;
            if (c != L'\\') { r += c; continue; }
            c = cur(); m_p++;
            switch (c) {
                case L'n': r += L'\n'; break;
                case L't': r += L'\t'; break;
                case L'r': r += L'\r'; break;
                case L'b': r += L'\b'; break;
                case L'f': r += L'\f'; break;
                case L'u': {
                    unsigned cp = hex4();
                    if (sizeof(wchar_t) == 4 && cp >= 0xD800 && cp < 0xDC00 &&
                        m_p + 1 < m_s.size() && m_s[m_p] == L'\\' && m_s[m_p + 1] == L'u') {
                        m_p += 2;
                        unsigned lo = hex4();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    put(r, cp);
                    break;
                }
                default: r += c;
            }
        }
        return r;
    }
    JV value() {
        ws();
        JV v;
        wchar_t c = cur();
        if (c == L'{') {
            m_p++; v.t = JV::Obj; ws();
            if (cur() == L'}') { m_p++; return v; }
            for (;;) {
                ws(); std::wstring k = string(); ws(); expect(L':');
                v.o.emplace_back(k, value()); ws();
                if (cur() == L',') { m_p++; continue; }
                expect(L'}'); break;
            }
        } else if (c == L'[') {
            m_p++; v.t = JV::Arr; ws();
            if (cur() == L']') { m_p++; return v; }
            for (;;) {
                v.a.push_back(value()); ws();
                if (cur() == L',') { m_p++; continue; }
                expect(L']'); break;
            }
        } else if (c == L'"') {
            v.t = JV::Str; v.s = string();
        } else if (lit(L"true")) { v.t = JV::Bool; v.n = 1; }
        else if (lit(L"false")) { v.t = JV::Bool; }
        else if (lit(L"null")) { }
        else {
            wchar_t* end = nullptr;
            v.n = wcstod(m_s.c_str() + m_p, &end);
            if (end == m_s.c_str() + m_p) throw Err();
            m_p = end - m_s.c_str(); v.t = JV::Num;
        }
        return v;
    }
};

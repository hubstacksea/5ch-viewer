// Text helpers: Shift_JIS conversion, URL encoding, HTML cleanup
#pragma once
#include <wx/wx.h>
#include <wx/strconv.h>
#include <string>

inline wxCSConv& SjisConv() {
    static wxCSConv conv(wxFONTENCODING_CP932);
    return conv;
}

inline wxString SjisToStr(const char* p, size_t n) {
    if (!n) return wxString();
    wxString s(p, SjisConv(), n);
    if (s.empty()) s = wxString::From8BitData(p, n);   // undecodable line: keep something visible
    return s;
}

// Encode to Shift_JIS; characters not representable become &#NNNN; (as 5ch expects)
inline std::string StrToSjis(const wxString& s) {
    std::string out;
    for (size_t i = 0; i < s.length(); i++) {
        wxString ch(s[i]);
        wxCharBuffer b = ch.mb_str(SjisConv());
        if (b.data() && b.length() > 0) out.append(b.data(), b.length());
        else {
            char tmp[24];
            snprintf(tmp, sizeof tmp, "&#%lu;", (unsigned long)(wxUint32)s[i].GetValue());
            out += tmp;
        }
    }
    return out;
}

inline std::string UrlEncode(const std::string& in) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : in) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '*') o += (char)c;
        else if (c == ' ') o += '+';
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

inline wxString DecodeEntities(const wxString& in) {
    if (in.Find('&') == wxNOT_FOUND) return in;
    wxString out;
    size_t i = 0, n = in.length();
    while (i < n) {
        if (in[i] == '&') {
            size_t semi = in.find(';', i);
            if (semi != wxString::npos && semi - i <= 10) {
                wxString e = in.Mid(i + 1, semi - i - 1);
                wxString r;
                bool ok = true;
                if (e == "lt") r = "<";
                else if (e == "gt") r = ">";
                else if (e == "amp") r = "&";
                else if (e == "quot") r = "\"";
                else if (e == "apos") r = "'";
                else if (e == "nbsp") r = " ";
                else if (e.length() > 1 && e[0] == '#') {
                    unsigned long cp = 0;
                    bool h = e.length() > 2 && (e[1] == 'x' || e[1] == 'X');
                    ok = (h ? e.Mid(2).ToULong(&cp, 16) : e.Mid(1).ToULong(&cp, 10)) && cp > 0 && cp < 0x110000;
                    if (ok) r = wxString(wxUniChar((wxUint32)cp));
                } else ok = false;
                if (ok) { out += r; i = semi + 1; continue; }
            }
        }
        out += in[i++];
    }
    return out;
}

// HTML fragment -> plain text (<br> -> newline, tags stripped, entities decoded)
inline wxString HtmlToText(const wxString& in) {
    wxString out;
    size_t i = 0, n = in.length();
    while (i < n) {
        if (in[i] == '<') {
            size_t e = in.find('>', i);
            if (e == wxString::npos) { out += in.Mid(i); break; }
            wxString tag = in.Mid(i + 1, e - i - 1).Lower();
            if (tag.StartsWith("br") || tag == "/p" || tag == "/div") out += '\n';
            i = e + 1;
        } else out += in[i++];
    }
    return DecodeEntities(out);
}

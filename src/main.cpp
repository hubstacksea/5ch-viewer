// 5ch viewer - wxWidgets (Windows / Linux)
//   left : board tree   right-top : thread list   right-bottom : tabbed thread readers + post form
#include <wx/wx.h>
#include <wx/splitter.h>
#include <wx/treectrl.h>
#include <wx/listctrl.h>
#include <wx/webrequest.h>
#include <wx/config.h>
#include <wx/fileconf.h>
#include <wx/stdpaths.h>
#include <wx/filename.h>
#include <wx/regex.h>
#include <wx/fontdlg.h>
#include <wx/fontenum.h>
#include <wx/clipbrd.h>
#include <wx/timer.h>
#include <wx/utils.h>
#include <wx/display.h>
#include <wx/tokenzr.h>
#include <wx/file.h>
#include <wx/datetime.h>
#include <wx/html/htmlwin.h>
#include <wx/html/htmlcell.h>
#include <wx/aui/auibook.h>
#include <wx/popupwin.h>
#include <wx/dir.h>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <vector>
#include <cwchar>
#include <ctime>

#include "json.h"
#include "textutil.h"

#define U(s) wxString::FromUTF8(s)

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif
static const char* APP_UA = "Monazilla/1.00 (5chViewer/" APP_VERSION ")";

// ---------------------------------------------------------------- data types
struct Board {
    wxString name, host, bbs, cat;
    wxString Key() const { return host + "/" + bbs; }
    wxString BaseUrl() const { return "https://" + host + "/" + bbs + "/"; }
};

struct ThreadInfo {
    wxString key, title;
    int count = 0, order = 0;
    long created = 0;
    double ikioi = 0;
    bool fresh = false;   // created after the board was last looked at
};

struct Post {
    int no = 0;
    wxString name, mail, date, id, body;
};

struct HttpResult {
    bool ok = false;
    int status = 0;
    std::string body;
    wxString err, setCookie;
};

enum ScrollMode { SCR_KEEP, SCR_NEW, SCR_TOP, SCR_BOTTOM };

// One tab: an HTML reader plus the thread data it shows
class ThreadView : public wxHtmlWindow {
public:
    ThreadView(wxWindow* p, int id) : wxHtmlWindow(p, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxHW_DEFAULT_STYLE), uid(id) {
        SetBorders(0);
        // wxHtml's own paint walks every off-screen cell of the whole page (O(page) per scroll step).
        // Paint only what is visible instead; fall back to the stock paint while a text selection exists.
        Bind(wxEVT_PAINT, &ThreadView::OnFastPaint, this);
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) { selActive = false; e.Skip(); });
        Bind(wxEVT_MOTION, [this](wxMouseEvent& e) { if (e.LeftIsDown()) selActive = true; e.Skip(); });
        Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent& e) { selActive = true; e.Skip(); });
        // after a click/drag, decide whether a real selection remains (a plain link click must not leave the slow path on)
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& e) {
            if (selActive) selActive = !SelectionToText().empty();
            e.Skip();
        });
        Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) { if (e.ControlDown() && e.GetKeyCode() == 'A') selActive = true; e.Skip(); });
    }
    bool selActive = false;
    wxBitmap backBuf;

    static void DrawVisible(wxHtmlCell* c, wxDC& dc, int x, int y, int y1, int y2, wxHtmlRenderingInfo& info) {
        if (typeid(*c) != typeid(wxHtmlContainerCell)) { c->Draw(dc, x, y, y1, y2, info); return; }
        wxHtmlContainerCell* cc = static_cast<wxHtmlContainerCell*>(c);
        int xl = x + cc->GetPosX(), yl = y + cc->GetPosY();
        wxColour bk = cc->GetBackgroundColour();
        if (bk.IsOk()) {
            int r1 = std::max(yl, y1), r2 = std::min(yl + cc->GetHeight() - 1, y2);
            dc.SetBrush(wxBrush(bk, wxBRUSHSTYLE_SOLID));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(xl, r1, cc->GetWidth(), r2 - r1 + 1);
        }
        for (wxHtmlCell* ch = cc->GetFirstChild(); ch; ch = ch->GetNext()) {
            int top = yl + ch->GetPosY();
            if (top <= y2 && top + ch->GetHeight() > y1) DrawVisible(ch, dc, xl, yl, y1, y2, info);
            else if (!dynamic_cast<wxHtmlContainerCell*>(ch)) ch->DrawInvisible(dc, xl, yl, info);   // font/colour state cells
        }
    }

    void OnFastPaint(wxPaintEvent& ev) {
        if (selActive || !GetInternalRepresentation()) { wxHtmlWindow::OnPaint(ev); return; }
        wxPaintDC dcPaint(this);
        wxSize sz = GetClientSize();
        if (sz.x <= 0 || sz.y <= 0) return;
        int vx, vy;
        GetViewStart(&vx, &vy);
        const wxRect rect = GetUpdateRegion().GetBox();
        wxDC* dc;
        wxMemoryDC dcm;
        if (IsDoubleBuffered()) dc = &dcPaint;
        else {
            if (!backBuf.IsOk() || backBuf.GetSize() != sz) backBuf.Create(sz.x, sz.y);
            dcm.SelectObject(backBuf);
            dc = &dcm;
        }
        dc->GetImpl()->SetWindow(this);
        PrepareDC(*dc);
        dc->SetBackground(GetBackgroundColour());
        dc->Clear();
        dc->SetMapMode(wxMM_TEXT);
        dc->SetBackgroundMode(wxBRUSHSTYLE_TRANSPARENT);
        dc->SetLayoutDirection(GetLayoutDirection());
        wxHtmlRenderingInfo rinfo;
        wxDefaultHtmlRenderingStyle rstyle(this);
        rinfo.SetStyle(&rstyle);
        DrawVisible(GetInternalRepresentation(), *dc, 0, 0, vy * wxHTML_SCROLL_STEP + rect.GetTop(),
                    vy * wxHTML_SCROLL_STEP + rect.GetBottom(), rinfo);
        if (dc != &dcPaint) {
            dc->SetDeviceOrigin(0, 0);
            dcPaint.Blit(0, rect.GetTop(), sz.x, rect.GetBottom() - rect.GetTop() + 1, dc, 0, rect.GetTop());
        }
    }
    // ScrollToAnchor() pops up a warning for a missing anchor (e.g. the post is outside the current view
    // mode): check first and let callers fall back silently
    bool GotoAnchor(const wxString& a) {
        wxHtmlContainerCell* root = GetInternalRepresentation();
        if (!root || !root->Find(wxHTML_COND_ISANCHOR, &a)) return false;
        return ScrollToAnchor(a);
    }
    int uid;
    Board board;
    wxString key, title;
    std::vector<Post> posts;
    std::map<wxString, std::vector<int>> idPosts;
    std::map<int, std::vector<int>> backrefs;
    int markPost = 0;          // first "new" post number (0 = none)
    int unread = 0;            // new posts not yet seen (background tab)
    int unreadReplies = 0;     // of which replies to own posts
    std::set<int> mine;        // own post numbers
    std::set<int> replyToMe;   // posts quoting an own post
    int gen = 0;
    bool dirty = false;        // needs re-render when it becomes visible
    bool loading = false;
    bool settled = false;
    bool dead = false;         // dat is gone from the server (dat落ち): shown from cache, not auto-refreshed
    int jumpTo = 0;
    bool forceFull = false;    // next load ignores the cache (dat was rewritten)
    bool useOyster = false;    // next load reads the past-log path (/bbs/oyster/xxxx/key.dat)            // render, then scroll to this post      // first scroll positioning done (window was laid out)
    wxString idFilter;
    ScrollMode scroll = SCR_NEW;
    int keepNo = 0, keepDelta = 0, renderW = 0;   // scroll anchor (post number + pixel offset)
    int keepY = 0;
    std::vector<int> shown;    // post numbers in the order they were rendered
};

enum {
    ID_BOARD_RELOAD = wxID_HIGHEST + 1, ID_THREAD_RELOAD, ID_MENU_UPDATE, ID_OPEN_URL, ID_FAV, ID_BROWSER, ID_FIND,
    ID_NG, ID_FONT, ID_ZOOM_IN, ID_ZOOM_OUT, ID_POSTPANEL, ID_NEWTHREAD, ID_SEND, ID_TIMER, ID_HOVER_TIMER,
    ID_CTX_NGID, ID_CTX_REPLY, ID_CTX_QUOTE, ID_CTX_COPY, ID_CTX_COPYPOST, ID_CTX_IDFILTER, ID_CTX_NGPOST, ID_CTX_MINE,
    ID_BOARDFILTER, ID_THFILTER, ID_NEXT_NEW, ID_THREAD_COPYURL, ID_CLOSE_TAB, ID_CLOSE_OTHERS, ID_CLOSE_ALL,
    ID_RELOAD_ALL, ID_TOP, ID_BOTTOM, ID_NEWPOS, ID_VIEWMODE, ID_TAB_NEXT, ID_TAB_PREV,
    ID_RESIZE_TIMER, ID_POST_TIMER, ID_AUTO0, ID_AUTO30, ID_AUTO60, ID_AUTO120, ID_AUTO300
};

static wxString Hex(const wxColour& c) { return wxString::Format("#%02x%02x%02x", c.Red(), c.Green(), c.Blue()); }
static wxColour Mix(const wxColour& a, const wxColour& b, double t) {
    return wxColour((unsigned char)(a.Red() * (1 - t) + b.Red() * t), (unsigned char)(a.Green() * (1 - t) + b.Green() * t),
                    (unsigned char)(a.Blue() * (1 - t) + b.Blue() * t));
}

struct Palette {
    bool dark;
    wxColour bg, fg, bgAlt, bgNew, link, num, name, gray, accent, accentNew, idHot, idWarm, banner;
    wxColour mine, reply, bgReply;   // own posts / replies to own posts
    Palette() {
        bg = wxSystemSettings::GetColour(wxSYS_COLOUR_LISTBOX);
        fg = wxSystemSettings::GetColour(wxSYS_COLOUR_LISTBOXTEXT);
        dark = (bg.Red() * 299 + bg.Green() * 587 + bg.Blue() * 114) / 1000 < 128;
        bgAlt = Mix(bg, fg, 0.05);
        if (dark) {
            bgNew = Mix(bg, wxColour(255, 200, 80), 0.14);
            link = wxColour(120, 175, 255); num = wxColour(130, 190, 255); name = wxColour(120, 215, 120);
            gray = wxColour(150, 150, 150); accent = wxColour(90, 100, 120); accentNew = wxColour(255, 170, 60);
            idHot = wxColour(255, 100, 100); idWarm = wxColour(255, 180, 90); banner = wxColour(255, 170, 60);
            mine = wxColour(110, 170, 255); reply = wxColour(255, 110, 190); bgReply = Mix(bg, reply, 0.13);
        } else {
            bgNew = Mix(bg, wxColour(255, 190, 60), 0.16);
            link = wxColour(0, 70, 200); num = wxColour(30, 80, 180); name = wxColour(0, 125, 0);
            gray = wxColour(110, 110, 110); accent = wxColour(170, 180, 200); accentNew = wxColour(235, 130, 0);
            idHot = wxColour(210, 0, 0); idWarm = wxColour(200, 110, 0); banner = wxColour(200, 90, 0);
            mine = wxColour(40, 90, 230); reply = wxColour(215, 0, 120); bgReply = Mix(bg, reply, 0.09);
        }
    }
};

static wxRegEx& TokenRegex() {
    static wxRegEx re;
    static bool init = false;
    if (!init) {
        init = true;
        re.Compile(U("(>>?|＞＞?)[0-9０-９]+([-,][0-9０-９]+)*|h?ttps?://[A-Za-z0-9._~:/?#@!$&'()*+,;=%\\[\\]-]+"), wxRE_ADVANCED);
    }
    return re;
}

// next anchor/URL token in s: start/len. Trims a ')' that closes a parenthesis opened before the URL.
static bool NextToken(const wxString& s, size_t& st, size_t& ln) {
    wxRegEx& re = TokenRegex();
    if (s.empty() || !re.Matches(s) || !re.GetMatch(&st, &ln, 0)) return false;
    wxString tok = s.Mid(st, ln);
    if (tok.Contains("://")) {
        while (ln > 0 && (tok.Last() == '.' || tok.Last() == ',' ||
                          (tok.Last() == ')' && tok.Freq('(') < tok.Freq(')')))) {
            tok.RemoveLast();
            ln--;
        }
    }
    return ln > 0;
}

static bool IsUrlToken(const wxString& tok) { return tok.Contains("://"); }
static wxString UrlOf(const wxString& tok) { return tok.StartsWith("ttp") ? "h" + tok : tok; }

// escape plain text for HTML; keeps runs of spaces and newlines
static wxString HtmlEsc(const wxString& s) {
    wxString o;
    o.Alloc(s.length() + 16);
    bool prevSpace = true;   // leading space must not collapse
    bool spaceRun = false;
    int run = 0;             // length of the current unbreakable ASCII run (long URLs would widen the page)
    for (size_t i = 0; i < s.length(); i++) {
        wxUniChar c = s[i];
        if (c.GetValue() < 128 && c != ' ' && c != '\n' && c != '\t') {
            if (++run >= 48) { o += ' '; run = 0; prevSpace = true; }
        } else run = 0;
        if (c == '<') { o += "&lt;"; prevSpace = false; }
        else if (c == '>') { o += "&gt;"; prevSpace = false; }
        else if (c == '&') { o += "&amp;"; prevSpace = false; }
        else if (c == '"') { o += "&quot;"; prevSpace = false; }
        else if (c == '\n') { o += "<br>"; prevSpace = true; }
        else if (c == '\t') { o += "&nbsp;&nbsp;&nbsp;&nbsp;"; prevSpace = true; }
        else if (c == ' ') {   // alternate nbsp / space: keeps width but leaves break points
            if (prevSpace) { o += "&nbsp;"; prevSpace = false; spaceRun = true; }
            else if (spaceRun) { o += " "; prevSpace = true; spaceRun = false; }
            else { o += " "; prevSpace = true; spaceRun = false; }
        }
        else { o += c; prevSpace = false; }
    }
    return o;
}

// escape for attribute values / short inline text. nbsp=true keeps the text on one line (one wxHtml word cell:
// far cheaper to lay out than many small words)
static wxString TextEsc(const wxString& s, bool nbsp = false) {
    wxString o;
    o.Alloc(s.length() + 8);
    for (size_t i = 0; i < s.length(); i++) {
        wxUniChar c = s[i];
        if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '&') o += "&amp;";
        else if (c == '"') o += "&quot;";
        else if (nbsp && (c == ' ' || c == '\t')) o += "&nbsp;";
        else if (c == '\n' || c == '\r') o += ' ';
        else o += c;
    }
    return o;
}

static wxString NormBody(const wxString& s) {   // compare posted text with what the dat returns
    wxString o;
    for (size_t i = 0; i < s.length(); i++) {
        wxUniChar c = s[i];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == 0x3000) continue;
        o += c;
    }
    return o;
}

static wxString NormAnchor(wxString t) {
    wxString o;
    for (size_t i = 0; i < t.length(); i++) {
        wxUniChar c = t[i];
        if (c == '>' || c == 0xFF1E) continue;
        if (c >= 0xFF10 && c <= 0xFF19) c = wxUniChar('0' + (c.GetValue() - 0xFF10));
        o += c;
    }
    return o;
}

// ================================================================ hover popup
class PopupView : public wxPopupWindow {
public:
    // wxBORDER_SIMPLE is not drawn for popups on every platform (e.g. GTK without a compositor):
    // draw a 1px frame ourselves by letting the popup's own background show around the HTML view
    explicit PopupView(wxWindow* parent) : wxPopupWindow(parent, wxBORDER_NONE) {
        SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
        html = new wxHtmlWindow(this, wxID_ANY, wxPoint(1, 1), wxSize(558, 98), wxHW_SCROLLBAR_AUTO | wxBORDER_NONE);
        html->SetBorders(0);
    }
    wxHtmlWindow* html;
    wxString href;
    wxRect Bounds() const { return wxRect(GetScreenPosition(), GetSize()); }
};

// virtual list control that asks the frame for its text
class ThreadList : public wxListCtrl {
public:
    ThreadList(wxWindow* p, wxWindowID id, std::function<wxString(long, long)> t, std::function<wxItemAttr*(long)> a)
        : wxListCtrl(p, id, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_VIRTUAL | wxLC_SINGLE_SEL),
          m_t(std::move(t)), m_a(std::move(a)) {}
    wxString OnGetItemText(long item, long col) const override { return m_t(item, col); }
    wxItemAttr* OnGetItemAttr(long item) const override { return m_a(item); }
private:
    std::function<wxString(long, long)> m_t;
    std::function<wxItemAttr*(long)> m_a;
};

class BoardData : public wxTreeItemData {
public:
    explicit BoardData(int i) : idx(i) {}
    int idx;
};

// One board tab: filter box + thread list + the data behind it
class BoardTab : public wxPanel {
public:
    BoardTab(wxWindow* p, int id) : wxPanel(p), uid(id) {}
    int uid, gen = 0, sortCol = 0;
    bool sortAsc = true;
    wxLongLong lastLoad = 0;
    long seenBase = -1;   // newest thread creation time seen in an earlier session (-1: not loaded)
    long seenMax = 0;
    Board board;
    std::vector<ThreadInfo> threads;
    std::vector<int> view;
    wxTextCtrl* filter = nullptr;
    wxButton* reload = nullptr;
    wxStaticText* stamp = nullptr;   // last update time / progress
    ThreadList* list = nullptr;
};

// ================================================================ main frame
class MainFrame : public wxFrame {
public:
    MainFrame();

private:
    // ---- UI
    wxSplitterWindow *m_split1 = nullptr, *m_split2 = nullptr;
    wxTextCtrl* m_boardFilter = nullptr;
    wxTreeCtrl* m_tree = nullptr;
    wxAuiNotebook* m_bnb = nullptr;
    std::map<int, BoardTab*> m_btabs;
    int m_nextBid = 1;
    std::vector<std::vector<wxString>> m_restoreBoards;
    wxString m_restoreBoardSel;
    wxAuiNotebook* m_nb = nullptr;
    wxChoice* m_viewMode = nullptr;
    wxStaticText* m_info = nullptr;
    wxPanel* m_postPanel = nullptr;
    wxTextCtrl *m_pName = nullptr, *m_pMail = nullptr, *m_pMsg = nullptr;
    wxCheckBox* m_sage = nullptr;
    wxButton* m_sendBtn = nullptr;
    wxStaticText* m_postTarget = nullptr;
    wxTimer m_timer, m_hoverTimer;
    wxFont m_font;
    int m_fontPt = 11;
    int m_autoSec = 0;
    PopupView* m_popup = nullptr;
    bool m_overLink = false;
    wxLongLong m_lastLinkTick = 0;
    mutable wxItemAttr m_attrNew, m_attrAlt, m_attrFresh;
    static wxString SeenKey(const Board& b) { return "seen/" + b.host + "_" + b.bbs; }

    // ---- data
    std::vector<Board> m_boards;
    std::set<wxString> m_favs;
    std::map<wxString, int> m_readCount;   // "host/bbs/key" -> posts read
    std::map<wxString, int> m_lastTop;     // "host/bbs/key" -> post at the top of the view when last seen
    std::map<wxString, std::set<int>> m_mine;   // "host/bbs/key" -> own post numbers
    struct PendingMine { wxString tkey, body; int tries; };
    std::vector<PendingMine> m_pendingMine;     // posted, not yet identified in the dat
    wxTimer m_postTimer;
    int m_postUid = 0;
    bool m_rebuildingTree = false;
    std::map<int, ThreadView*> m_views;
    int m_nextUid = 1;
    wxString m_ngText;
    std::vector<wxString> m_ngWords, m_ngIds, m_ngNames;
    std::set<std::pair<wxString, int>> m_ngPosts;   // (board/key, post no)
    wxString m_findText;
    std::map<wxString, wxString> m_cookies;
    std::vector<std::vector<wxString>> m_restore;
    wxString m_restoreSel;
    ThreadView* m_ctxView = nullptr;
    int m_ctxPost = -1;

    // ---- http
    struct Pending { wxWebRequest req; std::function<void(HttpResult&)> cb; };
    std::map<int, Pending> m_pending;
    std::vector<wxWebRequest> m_trash;
    void Http(const wxString& url, const std::string* postBody, const std::map<wxString, wxString>& headers,
              std::function<void(HttpResult&)> cb);
    void OnWebState(wxWebRequestEvent& e);

    // ---- helpers
    wxString DataDir();
    wxString DataFile(const wxString& n) { return DataDir() + wxFileName::GetPathSeparator() + n; }
    bool ReadFile(const wxString& path, std::string& out);
    bool WriteFile(const wxString& path, const std::string& data);
    void LoadConfig();
    void SaveConfig();
    void SaveReadState();
    void LoadReadState();
    void SaveCookies();
    void ParseNg();
    void ApplyFont();
    void Status(const wxString& s) {
        SetStatusText(s, 0);
        if (getenv("V5CH_DEBUG")) fprintf(stderr, "[status] %s\n", (const char*)s.utf8_str());
    }

    // ---- boards / thread list
    void BuildMenu();
    void BuildUi();
    void LoadBoardMenu(bool forceNet);
    void ParseBoardMenu(const std::string& json);
    void RebuildTree();
    void SelectBoard(int idx);
    BoardTab* CurBoardTab();
    const Board* CurBoard() { BoardTab* t = CurBoardTab(); return t ? &t->board : nullptr; }
    BoardTab* OpenBoardTab(const Board& b, bool activate);
    void LoadSubject(BoardTab* t);
    void ParseSubject(BoardTab* t, const std::string& data);
    void ApplyThreadView(BoardTab* t);
    void ApplyAllBoards();
    int FindBoard(const wxString& host, const wxString& bbs);
    wxString ListText(BoardTab* t, long item, long col) const;
    wxItemAttr* ListAttr(BoardTab* t, long item) const;
    int ReadOf(const Board& b, const wxString& key) const {
        auto it = m_readCount.find(b.Key() + "/" + key);
        return it == m_readCount.end() ? 0 : it->second;
    }

    // ---- tabs / threads
    ThreadView* CurView();
    ThreadView* FindView(int uid) { auto it = m_views.find(uid); return it == m_views.end() ? nullptr : it->second; }
    ThreadView* OpenThreadTab(const Board& b, const wxString& key, const wxString& title, bool activate);
    void CloseTab(int idx);
    void LoadView(ThreadView* v);
    void ParseDat(ThreadView* v, const std::string& data);
    void RenderView(ThreadView* v);
    void ApplyScroll(ThreadView* v);
    wxString BuildHtml(ThreadView* v);
    wxString PostHtml(ThreadView* v, int idx, bool isNew, bool odd, bool popup, const Palette& pal, int maxW, wxDC& dc);
    int CharW(wxDC& dc, wxUniChar c);
    std::map<wxUniChar::value_type, int> m_cw;
    int m_cwPt = -1;
    wxString m_cwFace;
    wxTimer m_resizeTimer;
    int TopPx(ThreadView* v);
    wxString BodyHtml(const wxString& body, int maxW, wxDC& dc, const std::set<int>* hot = nullptr, const wxString& hotColor = wxString());
    static wxString TKey(ThreadView* v) { return v->board.Key() + "/" + v->key; }
    void ComputeMine(ThreadView* v);
    void MatchPendingMine(ThreadView* v, size_t from);
    bool OpenBbsUrl(const wxString& url);
    wxString PlainPost(ThreadView* v, int idx);
    bool IsNg(ThreadView* v, const Post& p);
    void UpdateTabLabel(ThreadView* v);
    void MarkRead(ThreadView* v);
    void RememberTop(ThreadView* v);
    void PruneCache();
    void UpdateInfo();
    void SetHtmlFonts(wxHtmlWindow* h);
    bool AnchorY(ThreadView* v, int no, int& y);
    int ModeFirst(ThreadView* v);
    void JumpToPost(ThreadView* v, int no);

    int PostNoAtY(ThreadView* v, int y, int* delta);
    int PostAtClick(ThreadView* v, wxPoint clientPt);
    void ReloadAll(bool manual = false);
    void ReRenderAll();

    // ---- popup
    void ShowPopup(ThreadView* v, const wxString& href);
    void HidePopup();

    // ---- posting
    void DoPost(const Board& b, const wxString& key, int uid, bool newThread, const wxString& subject, const wxString& name,
                const wxString& mail, const wxString& msg, const std::vector<std::pair<wxString, wxString>>* confirmFields,
                int depth);
    void SendPost();
    bool ConfirmPost(const wxString& terms);
    void ShowAbout();
    void NewThread();
    void InsertReply(ThreadView* v, int idx, bool quote);

    // ---- events
    void OnTreeSel(wxTreeEvent& e);
    void OnTreeRight(wxTreeEvent& e);
    void OnBoardPageChanged(wxAuiNotebookEvent& e);
    void OnHtmlLink(wxHtmlLinkEvent& e);
    void OnHtmlMotion(wxMouseEvent& e);
    void OnHtmlRight(wxMouseEvent& e);
    void OnHtmlWheel(wxMouseEvent& e);
    void OnPageChanged(wxAuiNotebookEvent& e);
    void OnPageClose(wxAuiNotebookEvent& e);
    void OnTabRight(wxAuiNotebookEvent& e);
    void OnTabMiddle(wxAuiNotebookEvent& e);
    void OnMenuAction(wxCommandEvent& e);
    void OnTimer(wxTimerEvent&) { ReloadAll(false); }
    void OnHoverTimer(wxTimerEvent&);
    void OnClose(wxCloseEvent& e);
    void OnPostKey(wxKeyEvent& e);
    void OpenUrlDialog();
    void NgDialog();
    void ToggleFav();
    void FindInThread();
    void SetZoom(int d);
    void SetAuto(int sec);
    void CloseTabs(int keepIdx);
    void AddNgId(const wxString& id);
    wxString ThreadUrl(ThreadView* v) { return "https://" + v->board.host + "/test/read.cgi/" + v->board.bbs + "/" + v->key + "/"; }
    void CopyText(const wxString& s) {
        if (wxTheClipboard->Open()) { wxTheClipboard->SetData(new wxTextDataObject(s)); wxTheClipboard->Close(); }
    }
};

// ================================================================ construction
MainFrame::MainFrame()
    : wxFrame(nullptr, wxID_ANY, "5ch Viewer", wxDefaultPosition, wxSize(1200, 800)), m_timer(this, ID_TIMER),
      m_hoverTimer(this, ID_HOVER_TIMER), m_resizeTimer(this, ID_RESIZE_TIMER) {
#ifdef __WXMSW__
    SetIcon(wxIcon("aaaa"));   // from app.rc
#endif
    LoadConfig();
    LoadReadState();
    BuildMenu();
    BuildUi();
    CreateStatusBar(2);
    int w[] = {-1, 260};
    SetStatusWidths(2, w);
    ApplyFont();

    Bind(wxEVT_WEBREQUEST_STATE, &MainFrame::OnWebState, this);
    Bind(wxEVT_TIMER, &MainFrame::OnTimer, this, ID_TIMER);
    Bind(wxEVT_TIMER, &MainFrame::OnHoverTimer, this, ID_HOVER_TIMER);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        for (auto& kv : m_views) kv.second->dirty = true;
        ThreadView* cv = CurView();
        if (cv) { cv->scroll = cv->settled ? SCR_KEEP : SCR_NEW; RenderView(cv); }
    }, ID_RESIZE_TIMER);
    Bind(wxEVT_CLOSE_WINDOW, &MainFrame::OnClose, this);
    Bind(wxEVT_MENU, &MainFrame::OnMenuAction, this);
    m_popup = new PopupView(this);
    // links inside the popup act on the current thread (jump / reply / browser)
    m_popup->html->Bind(wxEVT_HTML_LINK_CLICKED, [this](wxHtmlLinkEvent& e) {
        wxString href = e.GetLinkInfo().GetHref();
        ThreadView* v = CurView();
        HidePopup();
        if (!v) return;
        long n = 0;
        if (href.StartsWith("http")) { if (!OpenBbsUrl(href)) wxLaunchDefaultBrowser(href); }
        else if (href.StartsWith("num:") && href.Mid(4).ToLong(&n)) InsertReply(v, (int)n - 1, false);
        else if (href.StartsWith("res:") && href.Mid(4).BeforeFirst('-').BeforeFirst(',').ToLong(&n)) JumpToPost(v, (int)n);
        else if (href.StartsWith("id:")) { v->idFilter = href.Mid(3); v->scroll = SCR_TOP; RenderView(v); }
    });
    m_hoverTimer.Start(120);
    m_postTimer.SetOwner(this, ID_POST_TIMER);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        ThreadView* t = FindView(m_postUid);
        if (t) { t->scroll = SCR_BOTTOM; LoadView(t); }
    }, ID_POST_TIMER);
    CallAfter([this] { PruneCache(); });

    // restore tabs from the last session (cached content first, then refreshed from the network)
    for (auto& t : m_restore) {
        Board b;
        b.host = t[0]; b.bbs = t[1]; b.name = t[2]; b.cat = U("その他");
        OpenThreadTab(b, t[3], t[4], false);
    }
    for (size_t i = 0; i < m_nb->GetPageCount(); i++) {
        ThreadView* v = dynamic_cast<ThreadView*>(m_nb->GetPage(i));
        if (v && v->board.Key() + "/" + v->key == m_restoreSel) m_nb->SetSelection(i);
    }
    for (auto& t : m_restoreBoards) {
        Board b;
        b.host = t[0]; b.bbs = t[1]; b.name = t[2]; b.cat = U("その他");
        OpenBoardTab(b, false);
    }
    for (size_t i = 0; i < m_bnb->GetPageCount(); i++) {
        BoardTab* t = dynamic_cast<BoardTab*>(m_bnb->GetPage(i));
        if (t && t->board.Key() == m_restoreBoardSel) m_bnb->SetSelection(i);
    }
    ThreadView* cv = CurView();
    if (cv && cv->dirty) RenderView(cv);
    UpdateInfo();
    LoadBoardMenu(false);
    // apply splitter positions once the frame has its real size
    CallAfter([this] {
        wxConfigBase* c = wxConfigBase::Get();
        int h = m_split2->GetSize().y;
        int s2 = (int)c->ReadLong("layout/s2", h * 3 / 10);
        s2 = std::max(80, std::min(s2, h - 260));
        m_split2->SetSashPosition(s2);
        m_split1->SetSashPosition((int)c->ReadLong("layout/s1", 240));
    });
}

wxString MainFrame::DataDir() {
    wxString d = wxStandardPaths::Get().GetUserDataDir();
    if (!wxDirExists(d)) wxFileName::Mkdir(d, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    return d;
}

bool MainFrame::ReadFile(const wxString& path, std::string& out) {
    wxFile f;
    if (!wxFileExists(path) || !f.Open(path)) return false;
    wxFileOffset len = f.Length();
    out.resize((size_t)len);
    if (len > 0 && f.Read(&out[0], (size_t)len) != len) return false;
    return true;
}

bool MainFrame::WriteFile(const wxString& path, const std::string& data) {
    wxFile f(path, wxFile::write);
    return f.IsOpened() && f.Write(data.data(), data.size()) == data.size();
}

void MainFrame::LoadConfig() {
    wxConfigBase* c = wxConfigBase::Get();
    int x = c->ReadLong("win/x", -1), y = c->ReadLong("win/y", -1);
    SetSize(c->ReadLong("win/w", 1200), c->ReadLong("win/h", 800));
    if (x >= 0 && y >= 0 && wxDisplay::GetFromPoint(wxPoint(x, y)) != wxNOT_FOUND) Move(x, y);
    if (c->ReadBool("win/max", false)) Maximize();
    wxStringTokenizer tk(c->Read("favs", ""), "\n");
    while (tk.HasMoreTokens()) m_favs.insert(tk.GetNextToken());
    m_ngText = c->Read("ng", "");
    ParseNg();
    wxStringTokenizer ct(c->Read("cookies", ""), "\n");
    while (ct.HasMoreTokens()) {
        wxString kv = ct.GetNextToken();
        int eq = kv.Find('=');
        if (eq > 0) m_cookies[kv.Left(eq)] = kv.Mid(eq + 1);
    }
    m_fontPt = c->ReadLong("font/pt", 11);
    wxString face = c->Read("font/face", "");
    if (face.empty()) {
        const char* cands[] = {"Meiryo UI", "Meiryo", "Yu Gothic UI", "Noto Sans CJK JP", "IPAexGothic", "IPAPGothic", "Sans"};
        for (auto f : cands)
            if (wxFontEnumerator::IsValidFacename(f)) { face = f; break; }
    }
    m_font = wxFont(m_fontPt, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, face);
    if (!m_font.IsOk()) m_font = *wxNORMAL_FONT;
    m_autoSec = c->ReadLong("auto/sec", 0);
    if (m_autoSec > 0 && m_autoSec < 60) m_autoSec = 60;   // keep the load on the servers reasonable
    m_restoreBoardSel = c->Read("btabs/sel", "");
    long nb = c->ReadLong("btabs/count", 0);
    for (long i = 0; i < nb; i++) {
        wxStringTokenizer t(c->Read(wxString::Format("btabs/%ld", i), ""), "\t", wxTOKEN_RET_EMPTY_ALL);
        std::vector<wxString> f;
        while (t.HasMoreTokens()) f.push_back(t.GetNextToken());
        if (f.size() >= 3) m_restoreBoards.push_back(f);
    }
    m_restoreSel = c->Read("tabs/sel", "");
    long n = c->ReadLong("tabs/count", 0);
    for (long i = 0; i < n; i++) {
        wxStringTokenizer t(c->Read(wxString::Format("tabs/%ld", i), ""), "\t", wxTOKEN_RET_EMPTY_ALL);
        std::vector<wxString> f;
        while (t.HasMoreTokens()) f.push_back(t.GetNextToken());
        if (f.size() >= 5) m_restore.push_back(f);
    }
}

void MainFrame::SaveConfig() {
    wxConfigBase* c = wxConfigBase::Get();
    c->Write("win/max", IsMaximized());
    if (!IsMaximized() && !IsIconized()) {
        c->Write("win/x", (long)GetPosition().x);
        c->Write("win/y", (long)GetPosition().y);
        c->Write("win/w", (long)GetSize().x);
        c->Write("win/h", (long)GetSize().y);
    }
    c->Write("layout/s1", (long)m_split1->GetSashPosition());
    c->Write("layout/s2", (long)m_split2->GetSashPosition());
    c->Write("layout/post", m_postPanel->IsShown());
    c->Write("layout/mode", (long)m_viewMode->GetSelection());
    wxString favs;
    for (auto& f : m_favs) favs += f + "\n";
    c->Write("favs", favs);
    c->Write("ng", m_ngText);
    c->Write("font/pt", (long)m_fontPt);
    c->Write("font/face", m_font.GetFaceName());
    c->Write("post/name", m_pName->GetValue());
    c->Write("post/sage", m_sage->GetValue());
    c->Write("auto/sec", (long)m_autoSec);
    {
        long nb = 0;
        for (size_t i = 0; i < m_bnb->GetPageCount(); i++) {
            BoardTab* t = dynamic_cast<BoardTab*>(m_bnb->GetPage(i));
            if (t) c->Write(wxString::Format("btabs/%ld", nb++), t->board.host + "\t" + t->board.bbs + "\t" + t->board.name);
        }
        c->Write("btabs/count", nb);
        for (auto& kv : m_btabs)
            if (kv.second->seenMax > 0) c->Write(SeenKey(kv.second->board), kv.second->seenMax);
        BoardTab* ct = CurBoardTab();
        c->Write("btabs/sel", ct ? ct->board.Key() : wxString());
    }
    long n = 0;
    for (size_t i = 0; i < m_nb->GetPageCount(); i++) {
        ThreadView* v = dynamic_cast<ThreadView*>(m_nb->GetPage(i));
        if (!v) continue;
        c->Write(wxString::Format("tabs/%ld", n++),
                 v->board.host + "\t" + v->board.bbs + "\t" + v->board.name + "\t" + v->key + "\t" + v->title);
    }
    c->Write("tabs/count", n);
    ThreadView* cv = CurView();
    c->Write("tabs/sel", cv ? cv->board.Key() + "/" + cv->key : wxString());
    SaveCookies();
    c->Flush();
    SaveReadState();
}

void MainFrame::SaveCookies() {
    wxString s;
    for (auto& kv : m_cookies) s += kv.first + "=" + kv.second + "\n";
    wxConfigBase::Get()->Write("cookies", s);
}

void MainFrame::LoadReadState() {
    std::string md;
    if (ReadFile(DataFile("mine.tsv"), md)) {
        wxStringTokenizer tk(wxString::FromUTF8(md.c_str()), "\n");
        while (tk.HasMoreTokens()) {
            wxString l = tk.GetNextToken();
            wxString key = l.BeforeFirst('\t');
            wxStringTokenizer nums(l.AfterFirst('\t'), ",");
            long n;
            while (nums.HasMoreTokens()) if (nums.GetNextToken().ToLong(&n) && n > 0) m_mine[key].insert((int)n);
        }
    }
    std::string d;
    if (!ReadFile(DataFile("read.tsv"), d)) return;
    wxStringTokenizer tk(wxString::FromUTF8(d.c_str()), "\n");
    while (tk.HasMoreTokens()) {
        wxString l = tk.GetNextToken();
        int t = l.Find('\t');
        long n;
        if (t <= 0) continue;
        wxArrayString f = wxSplit(l, '\t');
        if (f.size() >= 2 && f[1].ToLong(&n)) m_readCount[f[0]] = (int)n;
        if (f.size() >= 3 && f[2].ToLong(&n) && n > 0) m_lastTop[f[0]] = (int)n;
    }
}

void MainFrame::SaveReadState() {
    {
        wxString m;
        for (auto& kv : m_mine) {
            if (kv.second.empty()) continue;
            m += kv.first + "\t";
            bool first = true;
            for (int n : kv.second) { m += wxString::Format(first ? "%d" : ",%d", n); first = false; }
            m += "\n";
        }
        WriteFile(DataFile("mine.tsv"), std::string(m.utf8_str()));
    }
    wxString s;
    for (auto& kv : m_readCount) {
        auto it = m_lastTop.find(kv.first);
        s += wxString::Format("%s\t%d\t%d\n", kv.first, kv.second, it == m_lastTop.end() ? 0 : it->second);
    }
    WriteFile(DataFile("read.tsv"), std::string(s.utf8_str()));
}

void MainFrame::ParseNg() {
    m_ngWords.clear(); m_ngIds.clear(); m_ngNames.clear();
    wxStringTokenizer tk(m_ngText, "\n");
    while (tk.HasMoreTokens()) {
        wxString l = tk.GetNextToken().Trim().Trim(false);
        if (l.empty()) continue;
        if (l.StartsWith("ID:")) m_ngIds.push_back(l.Mid(3));
        else if (l.StartsWith("name:")) m_ngNames.push_back(l.Mid(5));
        else m_ngWords.push_back(l);
    }
}

bool MainFrame::IsNg(ThreadView* v, const Post& p) {
    if (m_ngPosts.count({v->board.Key() + "/" + v->key, p.no})) return true;
    if (!p.id.empty() && std::find(m_ngIds.begin(), m_ngIds.end(), p.id) != m_ngIds.end()) return true;
    for (auto& n : m_ngNames) if (p.name.Contains(n)) return true;
    for (auto& w : m_ngWords) if (p.body.Contains(w)) return true;
    return false;
}

void MainFrame::AddNgId(const wxString& id) {
    m_ngText += (m_ngText.empty() || m_ngText.EndsWith("\n") ? "" : "\n") + wxString("ID:") + id + "\n";
    ParseNg();
    ReRenderAll();
}

void MainFrame::ReRenderAll() {
    for (auto& kv : m_views) kv.second->dirty = true;
    ThreadView* v = CurView();
    if (v) { v->scroll = SCR_KEEP; RenderView(v); }
}

void MainFrame::SetHtmlFonts(wxHtmlWindow* h) {
    int p = m_fontPt;
    int sizes[7] = {p - 4, p - 2, p, p + 2, p + 4, p + 6, p + 8};
    wxString face = m_font.GetFaceName();
    h->SetFonts(face, face, sizes);
}

void MainFrame::ApplyFont() {
    m_font.SetPointSize(m_fontPt);
    wxFont lf = *wxNORMAL_FONT;
    lf.SetPointSize(std::max(8, m_fontPt - 1));
    for (auto& kv : m_btabs) kv.second->list->SetFont(lf);
    m_tree->SetFont(lf);
    ReRenderAll();
}

// ================================================================ UI building
void MainFrame::BuildMenu() {
    wxMenuBar* mb = new wxMenuBar;
    wxMenu* f = new wxMenu;
    f->Append(ID_OPEN_URL, U("URLを開く...\tCtrl+O"));
    f->Append(ID_MENU_UPDATE, U("板一覧を更新"));
    f->AppendSeparator();
    f->Append(wxID_EXIT, U("終了\tCtrl+Q"));
    mb->Append(f, U("ファイル(&F)"));

    wxMenu* b = new wxMenu;
    b->Append(ID_BOARD_RELOAD, U("スレ一覧を更新\tF5"));
    b->Append(ID_FAV, U("お気に入りに追加/解除\tCtrl+D"));
    b->Append(ID_NEWTHREAD, U("スレッドを立てる..."));
    mb->Append(b, U("板(&B)"));

    wxMenu* t = new wxMenu;
    t->Append(ID_THREAD_RELOAD, U("このスレッドを更新\tF6"));
    t->Append(ID_RELOAD_ALL, U("全タブを更新\tShift+F6"));
    t->Append(ID_NEXT_NEW, U("新着位置へ\tF7"));
    t->Append(ID_TOP, U("先頭へ\tCtrl+Home"));
    t->Append(ID_BOTTOM, U("最新レスへ\tCtrl+End"));
    t->AppendSeparator();
    t->Append(ID_FIND, U("スレ内検索...\tCtrl+F"));
    t->Append(ID_BROWSER, U("ブラウザで開く"));
    t->Append(ID_THREAD_COPYURL, U("URLをコピー"));
    t->AppendSeparator();
    t->Append(ID_CLOSE_TAB, U("タブを閉じる\tCtrl+W"));
    t->Append(ID_CLOSE_ALL, U("すべてのタブを閉じる"));
    t->Append(ID_TAB_NEXT, U("次のタブ\tCtrl+PgDn"));
    t->Append(ID_TAB_PREV, U("前のタブ\tCtrl+PgUp"));
    mb->Append(t, U("スレッド(&T)"));

    wxMenu* a = new wxMenu;
    a->AppendRadioItem(ID_AUTO0, U("自動更新しない"));
    a->AppendRadioItem(ID_AUTO60, U("60秒ごと"));
    a->AppendRadioItem(ID_AUTO120, U("2分ごと"));
    a->AppendRadioItem(ID_AUTO300, U("5分ごと"));
    mb->Append(a, U("自動更新(&A)"));

    wxMenu* v = new wxMenu;
    v->Append(ID_ZOOM_IN, U("文字を大きく (Ctrl+ホイール)"));
    v->Append(ID_ZOOM_OUT, U("文字を小さく (Ctrl+ホイール)"));
    v->Append(ID_FONT, U("フォント..."));
    v->AppendCheckItem(ID_POSTPANEL, U("書き込み欄を表示\tCtrl+E"))->Check(true);
    mb->Append(v, U("表示(&V)"));

    wxMenu* tl = new wxMenu;
    tl->Append(ID_NG, U("NG設定..."));
    mb->Append(tl, U("ツール(&L)"));
    wxMenu* hm = new wxMenu;
    hm->Append(wxID_ABOUT, U("このソフトについて / ライセンス"));
    mb->Append(hm, U("ヘルプ(&H)"));
    SetMenuBar(mb);
    int ids[] = {ID_AUTO0, ID_AUTO60, ID_AUTO120, ID_AUTO300};
    int secs[] = {0, 60, 120, 300};
    for (int i = 0; i < 4; i++) if (secs[i] == m_autoSec) mb->Check(ids[i], true);
}

void MainFrame::BuildUi() {
    wxConfigBase* c = wxConfigBase::Get();
    m_split1 = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxSP_LIVE_UPDATE | wxSP_3D);
    m_split1->SetMinimumPaneSize(120);

    wxPanel* left = new wxPanel(m_split1);
    m_boardFilter = new wxTextCtrl(left, ID_BOARDFILTER);
    m_boardFilter->SetHint(U("板を検索"));
    m_tree = new wxTreeCtrl(left, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                            wxTR_HAS_BUTTONS | wxTR_HIDE_ROOT | wxTR_LINES_AT_ROOT | wxTR_SINGLE);
    wxBoxSizer* ls = new wxBoxSizer(wxVERTICAL);
    ls->Add(m_boardFilter, 0, wxEXPAND | wxALL, 3);
    ls->Add(m_tree, 1, wxEXPAND);
    left->SetSizer(ls);

    m_split2 = new wxSplitterWindow(m_split1, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxSP_LIVE_UPDATE | wxSP_3D);
    m_split2->SetMinimumPaneSize(80);

    m_bnb = new wxAuiNotebook(m_split2, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                              wxAUI_NB_DEFAULT_STYLE | wxAUI_NB_TAB_MOVE | wxAUI_NB_CLOSE_ON_ALL_TABS |
                                  wxAUI_NB_WINDOWLIST_BUTTON | wxAUI_NB_SCROLL_BUTTONS);

    wxPanel* bottom = new wxPanel(m_split2);
    wxPanel* bar = new wxPanel(bottom);
    wxBoxSizer* bh = new wxBoxSizer(wxHORIZONTAL);
    auto mk = [&](int id, const wxString& label) {
        wxButton* b = new wxButton(bar, id, label, wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
        bh->Add(b, 0, wxRIGHT, 3);
        b->Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) { wxCommandEvent ev(wxEVT_MENU, e.GetId()); OnMenuAction(ev); });
    };
    mk(ID_THREAD_RELOAD, U("更新"));
    mk(ID_TOP, U("▲先頭"));
    mk(ID_NEWPOS, U("新着へ"));
    mk(ID_BOTTOM, U("▼最新"));
    mk(ID_FIND, U("検索"));
    mk(ID_BROWSER, U("ブラウザ"));
    wxString modes[] = {U("全レス"), U("最新100"), U("最新50"), U("新着のみ")};
    m_viewMode = new wxChoice(bar, ID_VIEWMODE, wxDefaultPosition, wxDefaultSize, 4, modes);
    m_viewMode->SetSelection(c->ReadLong("layout/mode", 0));
    bh->Add(m_viewMode, 0, wxRIGHT, 6);
    m_info = new wxStaticText(bar, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    bh->Add(m_info, 1, wxALIGN_CENTER_VERTICAL);
    bar->SetSizer(bh);
    m_viewMode->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        ThreadView* v = CurView();
        if (v) { v->scroll = SCR_NEW; v->dirty = true; RenderView(v); }
    });

    m_nb = new wxAuiNotebook(bottom, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                             wxAUI_NB_DEFAULT_STYLE | wxAUI_NB_TAB_MOVE | wxAUI_NB_CLOSE_ON_ALL_TABS |
                                 wxAUI_NB_WINDOWLIST_BUTTON | wxAUI_NB_SCROLL_BUTTONS);

    m_postPanel = new wxPanel(bottom);
    m_pName = new wxTextCtrl(m_postPanel, wxID_ANY, c->Read("post/name", ""));
    m_pName->SetHint(U("名前"));
    m_pMail = new wxTextCtrl(m_postPanel, wxID_ANY);
    m_pMail->SetHint(U("メール"));
    m_sage = new wxCheckBox(m_postPanel, wxID_ANY, "sage");
    m_sage->SetValue(c->ReadBool("post/sage", true));
    m_sendBtn = new wxButton(m_postPanel, ID_SEND, U("書き込む"));
    m_postTarget = new wxStaticText(m_postPanel, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    m_pMsg = new wxTextCtrl(m_postPanel, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 80), wxTE_MULTILINE);
    // no SetHint(): on multi-line controls wx emulates hints with real text, which would be posted
    m_pMsg->SetToolTip(U("本文 (Ctrl+Enterで送信 / レス番号クリックで返信指定)"));
    wxBoxSizer* ph = new wxBoxSizer(wxHORIZONTAL);
    ph->Add(m_pName, 1, wxRIGHT, 4);
    ph->Add(m_pMail, 1, wxRIGHT, 4);
    ph->Add(m_sage, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    ph->Add(m_sendBtn, 0);
    wxBoxSizer* pv = new wxBoxSizer(wxVERTICAL);
    pv->Add(m_postTarget, 0, wxEXPAND | wxBOTTOM, 2);
    pv->Add(ph, 0, wxEXPAND | wxBOTTOM, 2);
    pv->Add(m_pMsg, 1, wxEXPAND);
    m_postPanel->SetSizer(pv);
    bool showPost = c->ReadBool("layout/post", true);
    m_postPanel->Show(showPost);
    GetMenuBar()->Check(ID_POSTPANEL, showPost);

    wxBoxSizer* bs = new wxBoxSizer(wxVERTICAL);
    bs->Add(bar, 0, wxEXPAND | wxALL, 2);
    bs->Add(m_nb, 1, wxEXPAND);
    bs->Add(m_postPanel, 0, wxEXPAND | wxALL, 3);
    bottom->SetSizer(bs);

    m_split2->SplitHorizontally(m_bnb, bottom, (int)c->ReadLong("layout/s2", 260));
    m_split1->SplitVertically(left, m_split2, (int)c->ReadLong("layout/s1", 240));
    m_split1->SetSashGravity(0.0);
    m_split2->SetSashGravity(0.35);

    wxColour hl = wxSystemSettings::GetColour(wxSYS_COLOUR_LISTBOX);
    wxColour tx = wxSystemSettings::GetColour(wxSYS_COLOUR_LISTBOXTEXT);
    bool dark = (hl.Red() + hl.Green() + hl.Blue()) / 3 < 128;
    m_attrNew.SetTextColour(dark ? wxColour(130, 190, 255) : wxColour(0, 80, 210));
    wxFont bf = *wxNORMAL_FONT;
    bf.SetWeight(wxFONTWEIGHT_BOLD);
    m_attrNew.SetFont(bf);
    m_attrAlt.SetBackgroundColour(Mix(hl, tx, 0.045));
    m_attrFresh.SetTextColour(dark ? wxColour(120, 215, 120) : wxColour(0, 130, 40));

    m_tree->Bind(wxEVT_TREE_SEL_CHANGED, &MainFrame::OnTreeSel, this);
    // double-click / Enter on a board: open (or switch to) its tab and refresh it
    m_tree->Bind(wxEVT_TREE_ITEM_ACTIVATED, [this](wxTreeEvent& e) {
        BoardData* d = dynamic_cast<BoardData*>(m_tree->GetItemData(e.GetItem()));
        if (!d) { e.Skip(); return; }
        bool existed = false;
        for (auto& kv : m_btabs) existed |= kv.second->board.Key() == m_boards[d->idx].Key();
        SelectBoard(d->idx);
        if (existed) LoadSubject(CurBoardTab());
    });
    m_tree->Bind(wxEVT_TREE_ITEM_RIGHT_CLICK, &MainFrame::OnTreeRight, this);
    m_boardFilter->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { RebuildTree(); });
    m_bnb->Bind(wxEVT_AUINOTEBOOK_PAGE_CHANGED, &MainFrame::OnBoardPageChanged, this);
    m_bnb->Bind(wxEVT_AUINOTEBOOK_TAB_RIGHT_UP, [this](wxAuiNotebookEvent& e) {
        BoardTab* t = dynamic_cast<BoardTab*>(m_bnb->GetPage(e.GetSelection()));
        if (!t) return;
        wxMenu m;
        m.Append(1, U("更新\tF5"));
        m.Append(2, U("ブラウザで開く"));
        m.Append(3, m_favs.count(t->board.Key()) ? U("お気に入りから外す") : U("お気に入りに追加"));
        m.AppendSeparator();
        m.Append(4, U("閉じる"));
        switch (GetPopupMenuSelectionFromUser(m)) {
            case 1: LoadSubject(t); break;
            case 2: wxLaunchDefaultBrowser(t->board.BaseUrl()); break;
            case 3:
                if (m_favs.count(t->board.Key())) m_favs.erase(t->board.Key()); else m_favs.insert(t->board.Key());
                RebuildTree();
                break;
            case 4: {
                int idx = m_bnb->GetPageIndex(t);
                m_btabs.erase(t->uid);
                if (idx != wxNOT_FOUND) m_bnb->DeletePage(idx);
                break;
            }
        }
    });
    m_bnb->Bind(wxEVT_AUINOTEBOOK_TAB_MIDDLE_UP, [this](wxAuiNotebookEvent& e) {
        BoardTab* t = dynamic_cast<BoardTab*>(m_bnb->GetPage(e.GetSelection()));
        if (!t) return;
        m_btabs.erase(t->uid);
        m_bnb->DeletePage(e.GetSelection());
    });
    m_bnb->Bind(wxEVT_AUINOTEBOOK_PAGE_CLOSE, [this](wxAuiNotebookEvent& e) {
        BoardTab* t = dynamic_cast<BoardTab*>(m_bnb->GetPage(e.GetSelection()));
        if (t) m_btabs.erase(t->uid);
        e.Skip();
    });
    m_nb->Bind(wxEVT_AUINOTEBOOK_PAGE_CHANGED, &MainFrame::OnPageChanged, this);
    m_nb->Bind(wxEVT_AUINOTEBOOK_PAGE_CLOSE, &MainFrame::OnPageClose, this);
    m_nb->Bind(wxEVT_AUINOTEBOOK_TAB_RIGHT_UP, &MainFrame::OnTabRight, this);
    m_nb->Bind(wxEVT_AUINOTEBOOK_TAB_MIDDLE_UP, &MainFrame::OnTabMiddle, this);
    m_pMsg->Bind(wxEVT_KEY_DOWN, &MainFrame::OnPostKey, this);
    m_sendBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { SendPost(); });
    m_sage->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
        if (m_sage->GetValue()) m_pMail->SetValue("sage");
        else if (m_pMail->GetValue() == "sage") m_pMail->Clear();
    });
    if (m_sage->GetValue()) m_pMail->SetValue("sage");
    if (m_autoSec > 0) m_timer.Start(m_autoSec * 1000);
    UpdateInfo();
}

// ================================================================ HTTP
void MainFrame::Http(const wxString& url, const std::string* postBody, const std::map<wxString, wxString>& headers,
                     std::function<void(HttpResult&)> cb) {
    static int s_reqId = 100000;   // ids must be unique: the default (wxID_ANY) would mix up concurrent requests
    wxWebRequest req = wxWebSession::GetDefault().CreateRequest(this, url, ++s_reqId);
    if (!req.IsOk()) {
        HttpResult r;
        r.err = U("リクエストを作成できません");
        cb(r);
        return;
    }
    req.SetHeader("User-Agent", APP_UA);
    for (auto& h : headers) req.SetHeader(h.first, h.second);
    req.SetStorage(wxWebRequest::Storage_Memory);
    if (postBody) {
        req.SetMethod("POST");
        req.SetData(wxString::FromAscii(postBody->c_str()), "application/x-www-form-urlencoded");
    }
    int id = req.GetId();
    m_pending[id] = Pending{req, std::move(cb)};
    req.Start();
}

void MainFrame::OnWebState(wxWebRequestEvent& e) {
    wxWebRequest::State st = e.GetState();
    if (st != wxWebRequest::State_Completed && st != wxWebRequest::State_Failed &&
        st != wxWebRequest::State_Cancelled && st != wxWebRequest::State_Unauthorized)
        return;
    int id = e.GetRequest().GetId();
    auto it = m_pending.find(id);
    if (it == m_pending.end()) return;
    HttpResult r;
    if (st == wxWebRequest::State_Completed) {
        r.ok = true;
        wxWebResponse resp = e.GetResponse();
        r.status = resp.GetStatus();
        r.setCookie = resp.GetHeader("Set-Cookie");
        wxInputStream* s = resp.GetStream();
        if (s) {
            char buf[32768];
            for (;;) {
                s->Read(buf, sizeof buf);
                size_t n = s->LastRead();
                if (!n) break;
                r.body.append(buf, n);
            }
        }
    } else {
        wxWebResponse fr = e.GetResponse();
        if (fr.IsOk()) r.status = fr.GetStatus();
        r.err = e.GetErrorDescription();
        if (r.err.empty()) r.err = U("通信に失敗しました");
    }
    auto cb = std::move(it->second.cb);
    m_trash.push_back(it->second.req);
    m_pending.erase(it);
    CallAfter([this] { m_trash.clear(); });
    cb(r);
}

// ================================================================ boards
void MainFrame::LoadBoardMenu(bool forceNet) {
    wxString cache = DataFile("bbsmenu.json");
    std::string d;
    if (!forceNet && ReadFile(cache, d) && !d.empty()) {
        ParseBoardMenu(d);
        if (!m_boards.empty()) return;
    }
    Status(U("板一覧を取得中..."));
    Http("https://menu.5ch.io/bbsmenu.json", nullptr, {}, [this, cache](HttpResult& r) {
        if (!r.ok || r.status != 200) {
            Status(wxString::Format(U("板一覧の取得に失敗: %s (HTTP %d)"), r.err, r.status));
            return;
        }
        WriteFile(cache, r.body);
        ParseBoardMenu(r.body);
        Status(wxString::Format(U("板一覧: %d 板"), (int)m_boards.size()));
    });
}

void MainFrame::ParseBoardMenu(const std::string& json) {
    std::wstring ws = wxString::FromUTF8(json.c_str(), json.size()).ToStdWstring();
    JV root;
    JsonParser jp(ws);
    if (!jp.parse(root)) { Status(U("板一覧の解析に失敗")); return; }
    const JV* menu = root.get(L"menu_list");
    if (!menu) return;
    wxRegEx re("^https?://([a-z0-9.-]+)/([A-Za-z0-9_]+)/?$", wxRE_ADVANCED | wxRE_ICASE);
    m_boards.clear();
    for (auto& cat : menu->a) {
        wxString cname = wxString(cat.str(L"category_name"));
        const JV* cc = cat.get(L"category_content");
        if (!cc) continue;
        for (auto& b : cc->a) {
            wxString url = wxString(b.str(L"url"));
            if (!re.Matches(url)) continue;
            wxString host = re.GetMatch(url, 1), bbs = re.GetMatch(url, 2);
            if (!(host.EndsWith("5ch.net") || host.EndsWith("5ch.io") || host.EndsWith("bbspink.com") || host.EndsWith("2ch.sc"))) continue;
            if (host.StartsWith("www.") || host.StartsWith("menu.") || host.StartsWith("info.")) continue;
            Board bd;
            bd.name = wxString(b.str(L"board_name"));
            bd.host = host; bd.bbs = bbs; bd.cat = cname;
            m_boards.push_back(bd);
        }
    }
    RebuildTree();
}

void MainFrame::RebuildTree() {
    wxString flt = m_boardFilter->GetValue().Lower();
    m_rebuildingTree = true;
    m_tree->Freeze();
    m_tree->DeleteAllItems();
    wxTreeItemId root = m_tree->AddRoot("root");
    wxTreeItemId favNode = m_tree->AppendItem(root, U("★ お気に入り"));
    std::map<wxString, wxTreeItemId> cats;
    for (size_t i = 0; i < m_boards.size(); i++) {
        const Board& b = m_boards[i];
        bool match = flt.empty() || b.name.Lower().Contains(flt) || b.bbs.Lower().Contains(flt);
        if (m_favs.count(b.Key()) && match) m_tree->AppendItem(favNode, b.name, -1, -1, new BoardData((int)i));
        if (!match) continue;
        auto it = cats.find(b.cat);
        if (it == cats.end()) it = cats.emplace(b.cat, m_tree->AppendItem(root, b.cat)).first;
        m_tree->AppendItem(it->second, b.name, -1, -1, new BoardData((int)i));
    }
    m_tree->Expand(favNode);
    if (!flt.empty()) for (auto& kv : cats) m_tree->Expand(kv.second);
    m_tree->Thaw();
    m_rebuildingTree = false;
}

int MainFrame::FindBoard(const wxString& host, const wxString& bbs) {
    for (size_t i = 0; i < m_boards.size(); i++)
        if (m_boards[i].bbs == bbs && m_boards[i].host == host) return (int)i;
    for (size_t i = 0; i < m_boards.size(); i++)
        if (m_boards[i].bbs == bbs) return (int)i;
    return -1;
}

void MainFrame::OnTreeSel(wxTreeEvent& e) {
    if (m_rebuildingTree) return;
    BoardData* d = dynamic_cast<BoardData*>(m_tree->GetItemData(e.GetItem()));
    if (d) SelectBoard(d->idx);
}

void MainFrame::OnTreeRight(wxTreeEvent& e) {
    BoardData* d = dynamic_cast<BoardData*>(m_tree->GetItemData(e.GetItem()));
    if (!d) return;
    int idx = d->idx;
    wxMenu m;
    bool fav = m_favs.count(m_boards[idx].Key()) > 0;
    m.Append(1, fav ? U("お気に入りから外す") : U("お気に入りに追加"));
    m.Append(2, U("ブラウザで開く"));
    int r = GetPopupMenuSelectionFromUser(m);
    if (r == 1) {
        if (fav) m_favs.erase(m_boards[idx].Key()); else m_favs.insert(m_boards[idx].Key());
        RebuildTree();
    } else if (r == 2) wxLaunchDefaultBrowser(m_boards[idx].BaseUrl());
}

void MainFrame::ToggleFav() {
    const Board* b = CurBoard();
    if (!b) return;
    wxString k = b->Key();
    if (m_favs.count(k)) m_favs.erase(k); else m_favs.insert(k);
    RebuildTree();
}

BoardTab* MainFrame::CurBoardTab() {
    int s = m_bnb->GetSelection();
    return s == wxNOT_FOUND ? nullptr : dynamic_cast<BoardTab*>(m_bnb->GetPage(s));
}

void MainFrame::SelectBoard(int idx) {
    const Board& b = m_boards[idx];
    for (size_t i = 0; i < m_bnb->GetPageCount(); i++) {
        BoardTab* t = dynamic_cast<BoardTab*>(m_bnb->GetPage(i));
        if (t && t->board.Key() == b.Key()) {
            if ((int)i != m_bnb->GetSelection()) m_bnb->SetSelection(i);
            return;
        }
    }
    OpenBoardTab(b, true);
}

BoardTab* MainFrame::OpenBoardTab(const Board& b, bool activate) {
    BoardTab* t = new BoardTab(m_bnb, m_nextBid++);
    t->board = b;
    t->filter = new wxTextCtrl(t, wxID_ANY);
    t->filter->SetHint(U("スレッドを絞り込み"));
    t->list = new ThreadList(
        t, wxID_ANY, [this, t](long i, long col) { return ListText(t, i, col); }, [this, t](long i) { return ListAttr(t, i); });
    ThreadList* l = t->list;
    l->AppendColumn("No", wxLIST_FORMAT_RIGHT, 45);
    l->AppendColumn(U("タイトル"), wxLIST_FORMAT_LEFT, 520);
    l->AppendColumn(U("レス"), wxLIST_FORMAT_RIGHT, 55);
    l->AppendColumn(U("既読"), wxLIST_FORMAT_RIGHT, 55);
    l->AppendColumn(U("新着"), wxLIST_FORMAT_RIGHT, 55);
    l->AppendColumn(U("勢い"), wxLIST_FORMAT_RIGHT, 65);
    l->AppendColumn(U("作成日時"), wxLIST_FORMAT_LEFT, 140);
    wxFont lf = *wxNORMAL_FONT;
    lf.SetPointSize(std::max(8, m_fontPt - 1));
    l->SetFont(lf);
    // one-click refresh right above the list (F5 and the tab's context menu do the same)
    t->reload = new wxButton(t, wxID_ANY, U("↻ 更新"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    t->reload->SetToolTip(U("このスレ一覧を更新 (F5)"));
    t->stamp = new wxStaticText(t, wxID_ANY, "");
    t->stamp->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    wxBoxSizer* hs = new wxBoxSizer(wxHORIZONTAL);
    hs->Add(t->reload, 0, wxRIGHT, 4);
    hs->Add(t->filter, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    hs->Add(t->stamp, 0, wxALIGN_CENTER_VERTICAL);
    wxBoxSizer* ts = new wxBoxSizer(wxVERTICAL);
    ts->Add(hs, 0, wxEXPAND | wxALL, 3);
    ts->Add(l, 1, wxEXPAND);
    t->SetSizer(ts);
    int bid = t->uid;
    t->reload->Bind(wxEVT_BUTTON, [this, bid](wxCommandEvent&) {
        auto it = m_btabs.find(bid);
        if (it != m_btabs.end()) LoadSubject(it->second);
    });
    t->filter->Bind(wxEVT_KEY_DOWN, [t](wxKeyEvent& e) {   // Esc clears the filter
        if (e.GetKeyCode() == WXK_ESCAPE && !t->filter->IsEmpty()) t->filter->Clear();
        else e.Skip();
    });
    t->filter->Bind(wxEVT_TEXT, [this, bid](wxCommandEvent&) { auto it = m_btabs.find(bid); if (it != m_btabs.end()) ApplyThreadView(it->second); });
    l->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this, bid](wxListEvent& e) {
        auto it = m_btabs.find(bid);
        if (it == m_btabs.end()) return;
        BoardTab* bt = it->second;
        long i = e.GetIndex();
        if (i < 0 || i >= (long)bt->view.size()) return;
        const ThreadInfo& th = bt->threads[bt->view[i]];
        OpenThreadTab(bt->board, th.key, th.title, true);
    });
    l->Bind(wxEVT_LIST_COL_CLICK, [this, bid](wxListEvent& e) {
        auto it = m_btabs.find(bid);
        if (it == m_btabs.end()) return;
        BoardTab* bt = it->second;
        int c = e.GetColumn();
        if (c == bt->sortCol) bt->sortAsc = !bt->sortAsc;
        else { bt->sortCol = c; bt->sortAsc = (c <= 1); }
        ApplyThreadView(bt);
    });
    m_btabs[t->uid] = t;
    m_bnb->AddPage(t, b.name, activate);
    LoadSubject(t);
    return t;
}

void MainFrame::OnBoardPageChanged(wxAuiNotebookEvent& e) {
    BoardTab* t = dynamic_cast<BoardTab*>(m_bnb->GetPage(e.GetSelection()));
    if (t) {
        SetTitle("5ch Viewer - " + t->board.name);
        // coming back to a list that is several minutes old: refresh it without being asked
        if (t->lastLoad > 0 && wxGetLocalTimeMillis() - t->lastLoad >= 180000) LoadSubject(t);
    }
    e.Skip();
}

void MainFrame::LoadSubject(BoardTab* t) {
    if (!t) return;
    const Board b = t->board;
    int uid = t->uid;
    int gen = ++t->gen;
    t->lastLoad = wxGetLocalTimeMillis();
    t->reload->Disable();
    t->stamp->SetLabel(U("更新中…"));
    t->Layout();
    Status(b.name + U(" のスレ一覧を取得中..."));
    Http(b.BaseUrl() + "subject.txt", nullptr, {{"Referer", b.BaseUrl()}}, [this, uid, gen, b](HttpResult& r) {
        auto it = m_btabs.find(uid);
        if (it == m_btabs.end() || it->second->gen != gen) return;
        BoardTab* bt = it->second;
        bt->reload->Enable();
        bt->stamp->SetLabel((r.ok && r.status == 200 ? U("更新 ") : U("更新失敗 ")) + wxDateTime::Now().Format("%H:%M:%S"));
        bt->Layout();
        if (!r.ok || r.status != 200) {
            Status(wxString::Format(U("%s: スレ一覧の取得に失敗 %s (HTTP %d)"), b.name, r.err, r.status));
            return;
        }
        ParseSubject(it->second, r.body);
        int fresh = 0;
        for (auto& t : it->second->threads) fresh += t.fresh;
        Status(wxString::Format(U("%s: %d スレッド"), b.name, (int)it->second->threads.size()) +
               (fresh ? wxString::Format(U(" (新スレ %d)"), fresh) : wxString()));
    });
}

void MainFrame::ParseSubject(BoardTab* bt, const std::string& data) {
    bt->threads.clear();
    long now = (long)time(nullptr);
    size_t p = 0;
    int order = 0;
    while (p < data.size()) {
        size_t e = data.find('\n', p);
        if (e == std::string::npos) e = data.size();
        std::string line = data.substr(p, e - p);
        p = e + 1;
        size_t sep = line.find("<>");
        if (sep == std::string::npos) continue;
        ThreadInfo t;
        std::string fn = line.substr(0, sep);
        t.key = wxString::FromAscii(fn.substr(0, fn.rfind(".dat")).c_str());
        wxString title = SjisToStr(line.data() + sep + 2, line.size() - sep - 2);
        title.Trim();
        int lp = title.rfind(" (");
        if (lp != wxNOT_FOUND && title.EndsWith(")")) {
            long n = 0;
            title.Mid(lp + 2, title.length() - lp - 3).ToLong(&n);
            t.count = (int)n;
            title = title.Left(lp);
        }
        t.title = DecodeEntities(title);
        t.order = ++order;
        t.key.ToLong(&t.created);
        double days = std::max(1.0, (now - t.created) / 86400.0);
        t.ikioi = t.created > 0 ? t.count / days : 0;
        bt->threads.push_back(t);
    }
    // mark threads created since the previous session ("新")
    if (bt->seenBase < 0) bt->seenBase = wxConfigBase::Get()->ReadLong(SeenKey(bt->board), 0);
    for (auto& t : bt->threads) {
        if (t.created <= 0 || t.created > now + 86400) continue;   // ads / pinned threads use far-future keys
        t.fresh = bt->seenBase > 0 && t.created > bt->seenBase;
        bt->seenMax = std::max(bt->seenMax, t.created);
    }
    ApplyThreadView(bt);
}

void MainFrame::ApplyAllBoards() {
    for (auto& kv : m_btabs) ApplyThreadView(kv.second);
}

void MainFrame::ApplyThreadView(BoardTab* bt) {
    wxString flt = bt->filter->GetValue().Lower();
    auto& threads = bt->threads;
    auto& view = bt->view;
    view.clear();
    for (size_t i = 0; i < threads.size(); i++)
        if (flt.empty() || threads[i].title.Lower().Contains(flt)) view.push_back((int)i);
    int col = bt->sortCol;
    bool asc = bt->sortAsc;
    const Board& cur = bt->board;
    auto readOf = [&](const ThreadInfo& t) { return ReadOf(cur, t.key); };
    std::stable_sort(view.begin(), view.end(), [&](int a, int b) {
        const ThreadInfo &x = threads[a], &y = threads[b];
        double dx = 0, dy = 0;
        switch (col) {
            case 0: dx = x.order; dy = y.order; break;
            case 1: { int c = x.title.CmpNoCase(y.title); return asc ? c < 0 : c > 0; }
            case 2: dx = x.count; dy = y.count; break;
            case 3: dx = readOf(x); dy = readOf(y); break;
            case 4: dx = readOf(x) ? x.count - readOf(x) : 0; dy = readOf(y) ? y.count - readOf(y) : 0; break;
            case 5: dx = x.ikioi; dy = y.ikioi; break;
            case 6: dx = x.created; dy = y.created; break;
        }
        return asc ? dx < dy : dx > dy;
    });
    bt->list->SetItemCount((long)view.size());
    bt->list->Refresh();
}

wxString MainFrame::ListText(BoardTab* bt, long item, long col) const {
    if (item < 0 || item >= (long)bt->view.size()) return "";
    const ThreadInfo& t = bt->threads[bt->view[item]];
    int rd = ReadOf(bt->board, t.key);
    switch (col) {
        case 0: return wxString::Format("%d", t.order);
        case 1: return (rd ? U("● ") : (t.fresh ? U("新 ") : U("   "))) + t.title;
        case 2: return wxString::Format("%d", t.count);
        case 3: return rd ? wxString::Format("%d", rd) : "";
        case 4: return (rd && t.count > rd) ? wxString::Format("+%d", t.count - rd) : "";
        case 5: return wxString::Format("%.1f", t.ikioi);
        case 6: return t.created > 0 ? wxDateTime((time_t)t.created).Format("%Y/%m/%d %H:%M") : wxString();
    }
    return "";
}

wxItemAttr* MainFrame::ListAttr(BoardTab* bt, long item) const {
    if (item < 0 || item >= (long)bt->view.size()) return nullptr;
    const ThreadInfo& t = bt->threads[bt->view[item]];
    int rd = ReadOf(bt->board, t.key);
    if (rd && t.count > rd) return &m_attrNew;
    if (!rd && t.fresh) return &m_attrFresh;
    return (item & 1) ? &m_attrAlt : nullptr;
}

// ================================================================ tabs / thread loading
ThreadView* MainFrame::CurView() {
    int s = m_nb->GetSelection();
    return s == wxNOT_FOUND ? nullptr : dynamic_cast<ThreadView*>(m_nb->GetPage(s));
}

ThreadView* MainFrame::OpenThreadTab(const Board& b, const wxString& key, const wxString& title, bool activate) {
    for (size_t i = 0; i < m_nb->GetPageCount(); i++) {
        ThreadView* v = dynamic_cast<ThreadView*>(m_nb->GetPage(i));
        if (v && v->key == key && v->board.Key() == b.Key()) {
            if (activate) m_nb->SetSelection(i);
            v->scroll = SCR_KEEP;
            LoadView(v);
            return v;
        }
    }
    ThreadView* v = new ThreadView(m_nb, m_nextUid++);
    v->board = b;
    v->key = key;
    v->title = title;
    v->scroll = SCR_NEW;
    v->dirty = true;
    SetHtmlFonts(v);
    v->Bind(wxEVT_HTML_LINK_CLICKED, &MainFrame::OnHtmlLink, this);
    v->Bind(wxEVT_MOTION, &MainFrame::OnHtmlMotion, this);
    v->Bind(wxEVT_RIGHT_UP, &MainFrame::OnHtmlRight, this);
    v->Bind(wxEVT_MOUSEWHEEL, &MainFrame::OnHtmlWheel, this);
    v->Bind(wxEVT_SIZE, [this, v](wxSizeEvent& e) {
        e.Skip();
        if (v->renderW == 0 && v->dirty && e.GetSize().x >= 200 && CurView() == v) {
            int uid = v->uid;
            CallAfter([this, uid] { ThreadView* t = FindView(uid); if (t && t->dirty) RenderView(t); });
        } else if (std::abs(e.GetSize().x - v->renderW) > 8 && v->renderW > 0) m_resizeTimer.StartOnce(250);
    });
    m_views[v->uid] = v;
    m_nb->AddPage(v, title.Left(18), activate);
    UpdateTabLabel(v);
    LoadView(v);
    return v;
}

void MainFrame::CloseTab(int idx) {
    ThreadView* v = dynamic_cast<ThreadView*>(m_nb->GetPage(idx));
    RememberTop(v);
    if (v) m_views.erase(v->uid);
    HidePopup();
    m_nb->DeletePage(idx);
    UpdateInfo();
}

void MainFrame::UpdateTabLabel(ThreadView* v) {
    int idx = m_nb->GetPageIndex(v);
    if (idx == wxNOT_FOUND) return;
    wxString t = v->title;
    if (t.length() > 16) t = t.Left(16) + U("…");
    if (v->unread > 0)
        t = (v->unreadReplies > 0 ? wxString::Format(U("(+%d ↩%d) "), v->unread, v->unreadReplies)
                                  : wxString::Format("(+%d) ", v->unread)) + t;
    if (v->dead) t = U("[落] ") + t;
    m_nb->SetPageText(idx, t);
}

void MainFrame::RememberTop(ThreadView* v) {
    if (!v || v->shown.empty() || !v->idFilter.empty()) return;
    int no = PostNoAtY(v, TopPx(v) + 4, nullptr);
    if (no > 0) m_lastTop[v->board.Key() + "/" + v->key] = no;
}

// dat cache grows forever otherwise: drop caches untouched for 60 days (open tabs are kept)
void MainFrame::PruneCache() {
    wxArrayString files;
    wxDir::GetAllFiles(DataDir(), &files, "*.dat", wxDIR_FILES);
    std::set<wxString> open;
    for (auto& kv : m_views) open.insert(kv.second->board.bbs + "_" + kv.second->key + ".dat");
    wxDateTime limit = wxDateTime::Now() - wxTimeSpan::Days(60);
    int removed = 0;
    for (auto& f : files) {
        wxFileName fn(f);
        if (open.count(fn.GetFullName())) continue;
        if (fn.GetModificationTime().IsValid() && fn.GetModificationTime() < limit && wxRemoveFile(f)) removed++;
    }
    if (removed && getenv("V5CH_DEBUG")) fprintf(stderr, "[cache] removed %d old dat files\n", removed);
}

void MainFrame::MarkRead(ThreadView* v) {
    v->unread = 0;
    v->unreadReplies = 0;
    if (!v->posts.empty()) m_readCount[v->board.Key() + "/" + v->key] = (int)v->posts.size();
    UpdateTabLabel(v);
    ApplyAllBoards();
}

void MainFrame::UpdateInfo() {
    ThreadView* v = CurView();
    if (!v) {
        m_info->SetLabel("");
        m_postTarget->SetLabel(U("(スレッドを開くとここに書き込み先が表示されます)"));
        return;
    }
    int newc = v->markPost > 0 ? (int)v->posts.size() - v->markPost + 1 : 0;
    m_info->SetLabel(wxString::Format(U("%s  —  %d レス%s"), v->title, (int)v->posts.size(),
                                      newc > 0 ? wxString::Format(U("  (新着 %d)"), newc) : wxString()));
    m_postTarget->SetLabel(U("書き込み先: ") + v->board.name + U(" / ") + v->title + U("   [Ctrl+Enterで送信 / レス番号クリックで返信指定]"));
}

void MainFrame::LoadView(ThreadView* v) {
    const Board b = v->board;
    const wxString key = v->key;
    int uid = v->uid;
    int gen = ++v->gen;
    wxString cachePath = DataFile(b.bbs + "_" + key + ".dat");
    std::string cached;
    bool haveCache = ReadFile(cachePath, cached) && !cached.empty();
    if (v->posts.empty() && haveCache) {
        ParseDat(v, cached);
        int rd = ReadOf(b, key);
        v->markPost = (rd > 0 && rd < (int)v->posts.size()) ? rd + 1 : 0;
        v->scroll = SCR_NEW;
        v->dirty = true;
        if (CurView() == v) RenderView(v);
    }
    // differential fetch: ask from the last byte (must be the trailing newline) so a rewritten dat
    // (deleted posts) is detected instead of silently appending to the wrong offset
    bool oyster = v->useOyster;
    v->useOyster = false;
    bool useRange = !oyster && haveCache && !v->forceFull && cached.back() == '\n';
    v->forceFull = false;
    std::map<wxString, wxString> hd{{"Referer", b.BaseUrl()}};
    if (useRange) hd["Range"] = wxString::Format("bytes=%lu-", (unsigned long)cached.size() - 1);
    v->loading = true;
    Status(v->title + U(" を取得中..."));
    // dat落ち threads are served from the past-log path documented on the 5ch wiki (Monazilla/develop)
    wxString url = oyster ? "https://" + b.host + "/" + b.bbs + "/oyster/" + key.Left(4) + "/" + key + ".dat"
                          : "https://" + b.host + "/" + b.bbs + "/dat/" + key + ".dat";
    Http(url, nullptr, hd, [this, uid, gen, b, key, cachePath, cached, haveCache, useRange, oyster](HttpResult& r) {
        ThreadView* v = FindView(uid);
        if (!v || gen != v->gen) return;
        v->loading = false;
        std::string full;
        bool gone = r.status == 404 || r.status == 410 || r.status == 403;
        if (r.ok && r.status == 206 && useRange) {
            if (r.body.empty() || r.body[0] != '\n') { v->forceFull = true; LoadView(v); return; }   // dat was rewritten
            full = cached + r.body.substr(1);
        } else if (r.ok && r.status == 200) full = r.body;
        else if (r.status == 416 && useRange) { v->forceFull = true; LoadView(v); return; }          // dat shrank
        else if (r.status == 304 && haveCache) full = cached;
        if (!gone && !full.empty() && full.find("<>") == std::string::npos) gone = true;   // error page instead of a dat
        if (gone && !oyster) { v->useOyster = true; LoadView(v); return; }                  // try the past log
        if (full.empty() || gone) {
            if (gone) {
                bool was = v->dead;
                v->dead = true;
                UpdateTabLabel(v);
                if (!was) { v->dirty = true; v->scroll = SCR_KEEP; if (CurView() == v) RenderView(v); }
            }
            wxString why = gone ? U("dat落ちしていて過去ログも取得できません") : wxString::Format(U("通信エラー %s (HTTP %d)"), r.err, r.status);
            Status(v->title + ": " + why + (haveCache ? U(" - キャッシュを表示中") : wxString()));
            if (!haveCache && v->posts.empty()) {
                Palette pal;
                v->SetPage("<html><body bgcolor=\"" + Hex(pal.bg) + "\" text=\"" + Hex(pal.fg) + "\"><br><center><b>" + TextEsc(v->title) +
                           "</b><br><br>" + TextEsc(why) + "<br><br><a href=\"reload:\">" + U("再試行") + "</a></center></body></html>");
            }
            return;
        }
        if (oyster != v->dead) { v->dead = oyster; v->dirty = true; UpdateTabLabel(v); }   // past log: no further updates
        if (!haveCache || full.size() != cached.size()) WriteFile(cachePath, full);
        size_t oldSize = v->posts.size();
        int rd = ReadOf(b, key);
        ParseDat(v, full);
        MatchPendingMine(v, oldSize);
        size_t n = v->posts.size();
        bool isCur = (CurView() == v);
        int newReplies = 0;
        if (oldSize > 0)
            for (int r : v->replyToMe) newReplies += r > (int)oldSize;
        if (newReplies && !isCur) v->unreadReplies += newReplies;
        ScrollMode keepMode = v->scroll == SCR_BOTTOM ? SCR_BOTTOM : SCR_KEEP;   // posting asks for "bottom"
        if (oldSize == 0) {
            v->markPost = (rd > 0 && rd < (int)n) ? rd + 1 : 0;
            v->unread = isCur ? 0 : (rd > 0 && (int)n > rd ? (int)n - rd : 0);
            v->scroll = SCR_NEW;
        } else {
            if (!v->settled) { v->scroll = SCR_NEW; keepMode = SCR_NEW; }
            if (n > oldSize) {
                if (v->unread == 0 && v->settled) v->markPost = (int)oldSize + 1;
                if (!isCur) v->unread += (int)(n - oldSize);
            }
            v->scroll = keepMode;
        }
        if (n != oldSize || oldSize == 0) {
            v->dirty = true;
            if (isCur) RenderView(v);
        }
        if (isCur) MarkRead(v);
        for (auto& kv : m_btabs)
            if (kv.second->board.Key() == b.Key())
                for (auto& t : kv.second->threads) if (t.key == key) t.count = std::max(t.count, (int)n);
        UpdateTabLabel(v);
        UpdateInfo();
        ApplyAllBoards();
        Status(wxString::Format(oyster ? U("%s: %d レス (過去ログ)") : U("%s: %d レス"), v->title, (int)n) +
               (newReplies ? wxString::Format(U("  ★ 自分への返信 %d 件"), newReplies) : wxString()));
    });
}

void MainFrame::ReloadAll(bool manual) {
    for (auto& kv : m_views)
        if (!kv.second->loading && (manual || !kv.second->dead)) LoadView(kv.second);
    // thread lists change slowly and subject.txt is comparatively large: refresh them at most every 3 minutes
    wxLongLong now = wxGetLocalTimeMillis();
    for (auto& kv : m_btabs)
        if (manual || now - kv.second->lastLoad >= 180000) LoadSubject(kv.second);
}

void MainFrame::ParseDat(ThreadView* v, const std::string& data) {
    v->posts.clear();
    v->idPosts.clear();
    v->backrefs.clear();
    size_t p = 0;
    int no = 0;
    wxRegEx idre("ID:([A-Za-z0-9+/?.!]+)", wxRE_ADVANCED);
    while (p < data.size()) {
        size_t e = data.find('\n', p);
        if (e == std::string::npos) e = data.size();
        wxString line = SjisToStr(data.data() + p, e - p);
        p = e + 1;
        if (line.empty()) continue;
        std::vector<wxString> f;
        size_t s = 0;
        for (;;) {
            size_t q = line.find("<>", s);
            if (q == wxString::npos) { f.push_back(line.Mid(s)); break; }
            f.push_back(line.Mid(s, q - s));
            s = q + 2;
        }
        if (f.size() < 4) continue;
        Post ps;
        ps.no = ++no;
        ps.name = HtmlToText(f[0]);
        ps.mail = HtmlToText(f[1]);
        ps.date = HtmlToText(f[2]);
        wxString body = HtmlToText(f[3]);
        body.Trim().Trim(false);
        wxString nb;
        wxStringTokenizer tk(body, "\n", wxTOKEN_RET_EMPTY_ALL);
        while (tk.HasMoreTokens()) {
            wxString l = tk.GetNextToken();
            if (l.StartsWith(" ")) l = l.Mid(1);
            if (l.EndsWith(" ")) l.RemoveLast();
            nb += l;
            if (tk.HasMoreTokens()) nb += "\n";
        }
        ps.body = nb;
        if (idre.Matches(ps.date)) {
            ps.id = idre.GetMatch(ps.date, 1);
            ps.date = ps.date.Left(ps.date.Find("ID:")).Trim();
        }
        if (no == 1 && f.size() >= 5) v->title = DecodeEntities(f[4]).Trim().Trim(false);
        if (!ps.id.empty()) v->idPosts[ps.id].push_back((int)v->posts.size());
        v->posts.push_back(ps);
    }
    // back references: which later posts quote >>N
    for (auto& ps : v->posts) {
        wxString body = ps.body;
        std::set<int> seen;
        size_t st, ln;
        while (NextToken(body, st, ln)) {
            wxString tok = body.Mid(st, ln);
            body = body.Mid(st + ln);
            if (IsUrlToken(tok)) continue;
            wxStringTokenizer parts(NormAnchor(tok), ",");
            while (parts.HasMoreTokens()) {
                wxString part = parts.GetNextToken();
                long a = 0, b = 0;
                int dash = part.Find('-');
                if (dash != wxNOT_FOUND) { part.Left(dash).ToLong(&a); part.Mid(dash + 1).ToLong(&b); }
                else { part.ToLong(&a); b = a; }
                if (b < a || b - a > 30) b = a;
                for (long n = a; n <= b; n++)
                    if (n >= 1 && n < ps.no && seen.insert((int)n).second) v->backrefs[(int)n].push_back(ps.no);
            }
        }
    }
    ComputeMine(v);
}

void MainFrame::ComputeMine(ThreadView* v) {
    v->mine.clear();
    v->replyToMe.clear();
    auto it = m_mine.find(TKey(v));
    if (it == m_mine.end()) return;
    for (int n : it->second)
        if (n <= (int)v->posts.size()) v->mine.insert(n);
    for (int n : v->mine) {
        auto br = v->backrefs.find(n);
        if (br != v->backrefs.end())
            for (int r : br->second) if (!v->mine.count(r)) v->replyToMe.insert(r);
    }
}

// a post we sent shows up in a later dat download: find it by its text (newest match first)
void MainFrame::MatchPendingMine(ThreadView* v, size_t from) {
    wxString tk = TKey(v);
    for (size_t i = 0; i < m_pendingMine.size();) {
        PendingMine& pm = m_pendingMine[i];
        if (pm.tkey != tk) { i++; continue; }
        int found = 0;
        size_t lo = from > 20 ? from - 20 : 0;
        for (size_t k = v->posts.size(); k-- > lo;)
            if (!m_mine[tk].count(v->posts[k].no) && NormBody(v->posts[k].body) == pm.body) { found = v->posts[k].no; break; }
        if (found) {
            m_mine[tk].insert(found);
            Status(wxString::Format(U(">>%d を自分の書き込みとして記録しました"), found));
        }
        if (found || ++pm.tries >= 5) m_pendingMine.erase(m_pendingMine.begin() + i);
        else i++;
    }
    ComputeMine(v);
}

// ================================================================ rendering
wxString MainFrame::PlainPost(ThreadView* v, int idx) {
    const Post& p = v->posts[idx];
    wxString h = wxString::Format("%d ", p.no) + p.name + " " + p.date;
    if (!p.id.empty()) h += " ID:" + p.id;
    return h + "\n" + p.body;
}

int MainFrame::CharW(wxDC& dc, wxUniChar c) {
    if (m_cwPt != m_fontPt || m_cwFace != m_font.GetFaceName()) {
        m_cw.clear();
        m_cwPt = m_fontPt;
        m_cwFace = m_font.GetFaceName();
    }
    auto it = m_cw.find(c.GetValue());
    if (it != m_cw.end()) return it->second;
    wxCoord w = 0, h = 0;
    dc.GetTextExtent(wxString(c), &w, &h);
    m_cw[c.GetValue()] = w;
    return w;
}

// wxHtml only breaks lines at spaces, so Japanese text would never wrap.  Lay the lines out here
// (measuring real glyph widths) and emit explicit <br> where the line is full.
wxString MainFrame::BodyHtml(const wxString& body, int maxW, wxDC& dc, const std::set<int>* hot, const wxString& hotColor) {
    struct Atom { wxUniChar c; int link; };
    std::vector<wxString> hrefs;
    std::vector<bool> hrefHot;   // anchor pointing at one of the own posts
    std::vector<Atom> atoms;
    atoms.reserve(body.length());
    wxString rest = body;
    auto addPlain = [&](const wxString& t) {
        for (size_t i = 0; i < t.length(); i++) {
            if (t[i] == '\t') for (int k = 0; k < 4; k++) atoms.push_back({' ', -1});
            else if (t[i] != '\r') atoms.push_back({t[i], -1});
        }
    };
    while (!rest.empty()) {
        size_t st, ln;
        if (!NextToken(rest, st, ln)) { addPlain(rest); break; }
        addPlain(rest.Left(st));
        wxString tok = rest.Mid(st, ln);
        hrefs.push_back(IsUrlToken(tok) ? UrlOf(tok) : "res:" + NormAnchor(tok));
        bool isHot = false;
        if (hot && !hot->empty() && !IsUrlToken(tok)) {
            wxStringTokenizer parts(NormAnchor(tok), ",-");
            long n;
            while (parts.HasMoreTokens()) if (parts.GetNextToken().ToLong(&n) && hot->count((int)n)) isHot = true;
        }
        hrefHot.push_back(isHot);
        for (size_t i = 0; i < tok.length(); i++) atoms.push_back({tok[i], (int)hrefs.size() - 1});
        rest = rest.Mid(st + ln);
    }

    wxString out;
    int openLink = -1;
    auto setLink = [&](int l) {
        if (l == openLink) return;
        if (openLink >= 0) out += hrefHot[openLink] ? "</b></font></a>" : "</a>";
        if (l >= 0) out += "<a href=\"" + TextEsc(hrefs[l]) + "\">" + (hrefHot[l] ? "<font color=\"" + hotColor + "\"><b>" : "");
        openLink = l;
    };
    auto putChar = [&](const Atom& a) {
        setLink(a.link);
        wxUniChar c = a.c;
        if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '&') out += "&amp;";
        else if (c == '"') out += "&quot;";
        else out += c;
    };
    const int sw = std::max(1, CharW(dc, ' '));
    size_t n = atoms.size(), i = 0;
    int lineW = 0;
    while (i < n) {
        if (atoms[i].c == '\n') { out += "<br>"; lineW = 0; i++; continue; }
        int sp = 0;
        while (i < n && atoms[i].c == ' ') { sp++; i++; }
        if (i >= n || atoms[i].c == '\n') continue;      // trailing spaces
        size_t j = i;
        int ww = 0;
        while (j < n && atoms[j].c != ' ' && atoms[j].c != '\n') ww += CharW(dc, atoms[j].c), j++;
        int spw = sp * sw;
        auto emitSpaces = [&](bool leading) {
            for (int k = 0; k < sp; k++) out += (k == 0 && !leading) ? " " : "&nbsp;";
            lineW += spw;
        };
        if (lineW == 0) emitSpaces(true);
        else if (lineW + spw + ww <= maxW) emitSpaces(false);
        else if (ww <= maxW || lineW + spw >= maxW) { out += "<br>"; lineW = 0; }
        else emitSpaces(false);
        for (size_t k = i; k < j; k++) {
            int w = CharW(dc, atoms[k].c);
            if (lineW + w > maxW && lineW > 0) { out += "<br>"; lineW = 0; }
            putChar(atoms[k]);
            lineW += w;
        }
        i = j;
    }
    setLink(-1);
    return out;
}

wxString MainFrame::PostHtml(ThreadView* v, int idx, bool isNew, bool odd, bool popup, const Palette& pal, int maxW, wxDC& dc) {
    const Post& p = v->posts[idx];
    bool isMine = v->mine.count(p.no) > 0, toMe = v->replyToMe.count(p.no) > 0;
    wxColour bgc = isNew ? pal.bgNew : (toMe ? pal.bgReply : (odd ? pal.bgAlt : pal.bg));
    wxColour bar = isMine ? pal.mine : (toMe ? pal.reply : (isNew ? pal.accentNew : pal.accent));
    wxString h;
    h += wxString::Format("<a name=\"p%d\"></a>", p.no);
    h += "<table width=\"100%\" cellspacing=0 cellpadding=0><tr><td width=5 bgcolor=\"" + Hex(bar) + "\"></td><td bgcolor=\"" +
         Hex(bgc) + "\"><table width=\"100%\" cellspacing=0 cellpadding=8><tr><td>";
    // header: few word cells and font changes -- the header used to be ~60% of the page layout time
    h += wxString::Format("<b><a href=\"num:%d\"><font color=\"%s\">%d</font></a></b>&nbsp;&nbsp;", p.no, Hex(pal.num), p.no);
    if (isMine) h += "<font color=\"" + Hex(pal.mine) + "\"><b>" + U("[自分]") + "</b></font>&nbsp;";
    if (toMe) h += "<font color=\"" + Hex(pal.reply) + "\"><b>" + U("[↩自分宛]") + "</b></font>&nbsp;";
    bool defName = p.name.Contains(U("名無し"));
    h += "<font color=\"" + Hex(defName ? pal.gray : pal.name) + "\">" + (defName ? "" : "<b>") + TextEsc(p.name, true) +
         (defName ? "" : "</b>") + "</font>";
    if (p.mail == "sage") h += " <font size=2 color=\"" + Hex(pal.gray) + "\">sage</font>";
    else if (!p.mail.empty()) h += " <font color=\"" + Hex(pal.gray) + "\">[" + TextEsc(p.mail, true) + "]</font>";
    h += " <font size=2 color=\"" + Hex(pal.gray) + "\">" + TextEsc(p.date, true) + "</font>";
    if (!p.id.empty()) {
        auto it = v->idPosts.find(p.id);
        size_t total = it != v->idPosts.end() ? it->second.size() : 1;
        wxColour ic = total >= 5 ? pal.idHot : (total >= 2 ? pal.idWarm : pal.gray);
        wxString label = "ID:" + TextEsc(p.id);
        if (total >= 2) {
            int nth = (int)(std::find(it->second.begin(), it->second.end(), idx) - it->second.begin()) + 1;
            label = "<b>" + label + wxString::Format("</b>&nbsp;(%d/%d)", nth, (int)total);
        }
        h += " <a href=\"id:" + TextEsc(p.id) + "\"><font size=2 color=\"" + Hex(ic) + "\">" + label + "</font></a>";
    }
    h += "<br>";
    h += "<table cellspacing=0 cellpadding=0><tr><td width=14></td><td>";
    if (IsNg(v, p)) h += "<font color=\"" + Hex(pal.gray) + "\"><i>" + U("あぼーん") + "</i></font>";
    else h += BodyHtml(p.body, maxW, dc, &v->mine, Hex(pal.reply));
    auto br = v->backrefs.find(p.no);
    if (!popup && br != v->backrefs.end()) {
        // the label pops up every reply at once; each >>N pops up / jumps to one
        wxString all;
        for (int n : br->second) all += (all.empty() ? "" : ",") + wxString::Format("%d", n);
        h += "<br><a href=\"res:" + all + "\"><font size=2>" + wxString::Format(U("返信(%d):"), (int)br->second.size()) + "</font></a>";
        int shown = 0;
        for (int n : br->second) {
            if (++shown > 12) { h += " ..."; break; }
            h += wxString::Format(" <a href=\"res:%d\"><font size=2>&gt;&gt;%d</font></a>", n, n);
        }
    }
    h += "</td></tr></table>";
    h += "</td></tr></table></td></tr></table>";
    if (!popup) h += "<table width=\"100%\" cellspacing=0 cellpadding=0><tr><td height=5></td></tr></table>";
    return h;
}

wxString MainFrame::BuildHtml(ThreadView* v) {
    Palette pal;
    wxClientDC dc(v);
    dc.SetFont(m_font);
    // use the outer width: the client width changes when the vertical scrollbar appears, which would
    // otherwise look like a resize and trigger a second full layout
    int cw = v->GetSize().x;
    if (cw < 200) cw = 800;
    int maxW = std::max(150, cw - 5 - 16 - 14 - 6 - wxSystemSettings::GetMetric(wxSYS_VSCROLL_X) - 14);
    v->renderW = cw;
    int n = (int)v->posts.size();
    int first = v->idFilter.empty() ? ModeFirst(v) : 0;
    int last = n;
    int begin = first;

    wxString h = "<html><body bgcolor=\"" + Hex(pal.bg) + "\" text=\"" + Hex(pal.fg) + "\" link=\"" + Hex(pal.link) + "\">";
    if (v->dead)
        h += "<table width=\"100%\" cellpadding=6 cellspacing=0><tr><td bgcolor=\"" + Hex(pal.bgNew) + "\"><font color=\"" +
             Hex(pal.banner) + "\"><b>" + U("このスレッドはdat落ちしています(過去ログ / 保存済みの内容を表示中・自動更新しません)") + "</b></font></td></tr></table><br>";
    if (!v->idFilter.empty()) {
        h += "<table width=\"100%\" cellpadding=8 cellspacing=0><tr><td bgcolor=\"" + Hex(pal.bgNew) + "\"><b>ID:" +
             HtmlEsc(v->idFilter) + U("</b> で抽出中 &nbsp; <a href=\"clear:\">[解除]</a></td></tr></table><br>");
    } else if (begin == 0 && first > 0) {
        h += "<center><a href=\"all:\">" + wxString::Format(U("▲ 以前の %d レスを表示"), first) + "</a></center><br>";
    }
    int shown = 0;
    v->shown.clear();
    for (int i = begin; i < last; i++) {
        const Post& p = v->posts[i];
        if (!v->idFilter.empty() && p.id != v->idFilter) continue;
        bool isNew = v->markPost > 0 && p.no >= v->markPost;
        if (isNew && p.no == v->markPost && v->idFilter.empty()) {
            h += "<a name=\"new\"></a><table width=\"100%\" cellpadding=4 cellspacing=0><tr><td align=center><b><font color=\"" +
                 Hex(pal.banner) + "\">" + wxString::Format(U("▼ ここから新着 %d 件 ▼"), n - p.no + 1) + "</font></b></td></tr></table>";
        }
        v->shown.push_back(p.no);
        h += PostHtml(v, i, isNew, (shown++ & 1) != 0, false, pal, maxW, dc);
    }
    h += "<br><center><a href=\"reload:\">" + wxString::Format(U("— %d レス — ↻ 更新 —"), n) + "</a></center><br><br>";
    h += "</body></html>";
    return h;
}

int MainFrame::ModeFirst(ThreadView* v) {
    int n = (int)v->posts.size();
    switch (m_viewMode->GetSelection()) {
        case 1: return std::max(0, n - 100);
        case 2: return std::max(0, n - 50);
        case 3: return v->markPost > 0 ? std::max(0, v->markPost - 2) : std::max(0, n - 1);
    }
    return 0;
}

// bring an arbitrary post into view
void MainFrame::JumpToPost(ThreadView* v, int no) {
    if (no < 1 || no > (int)v->posts.size()) return;
    if (std::binary_search(v->shown.begin(), v->shown.end(), no) && v->GotoAnchor(wxString::Format("p%d", no))) {
        // a deferred scroll fix-up (tab switch / first layout) must keep this position, not the old mode
        v->scroll = SCR_KEEP;
        v->keepNo = no;
        v->keepDelta = 0;
        return;
    }
    // not rendered (view mode / ID filter): show everything and jump after the re-render
    v->idFilter.clear();
    m_viewMode->SetSelection(0);
    v->jumpTo = no;
    RenderView(v);
}

void MainFrame::RenderView(ThreadView* v) {
    if (m_nb->GetPageIndex(v) == wxNOT_FOUND) return;
    if (v->GetClientSize().x < 200) {   // not laid out yet: the size handler renders once the width is known
        v->dirty = true;
        return;
    }
    if (v->jumpTo > 0) {
        v->keepNo = v->jumpTo;
        v->keepDelta = 0;
        v->scroll = SCR_KEEP;
        v->jumpTo = 0;
    } else if (v->scroll == SCR_KEEP) {
        // remember which post is at the top, so reflow (resize / new posts) keeps the reading position
        v->keepNo = PostNoAtY(v, TopPx(v), &v->keepDelta);
    }
    wxStopWatch sw;
    wxString html = BuildHtml(v);
    long tBuild = sw.Time();
    SetHtmlFonts(v);
    v->Freeze();
    v->SetPage(html);
    v->Thaw();
    if (getenv("V5CH_DEBUG")) fprintf(stderr, "[render] %d posts: build %ld ms, layout %ld ms\n", (int)v->posts.size(), tBuild, sw.Time() - tBuild);
    v->dirty = false;
    v->selActive = false;
    ApplyScroll(v);
    CallAfter([this, uid = v->uid] { ThreadView* vv = FindView(uid); if (vv) { ApplyScroll(vv); if (vv->GetClientSize().x >= 200) vv->settled = true; } });
}

void MainFrame::ApplyScroll(ThreadView* v) {
    switch (v->scroll) {
        case SCR_KEEP: {
            int y = 0, ppy = 1;
            v->GetScrollPixelsPerUnit(nullptr, &ppy);
            if (v->keepNo == 0 || !AnchorY(v, v->keepNo, y)) { v->Scroll(0, 0); break; }
            v->Scroll(-1, std::max(0, y + v->keepDelta) / std::max(1, ppy));
            break;
        }
        case SCR_NEW:
            if (v->markPost > 0 && v->GotoAnchor("new")) break;
            if (v->markPost == 0 && ReadOf(v->board, v->key) == 0) { v->Scroll(0, 0); break; }
            if (v->markPost == 0) {   // everything read: go back to where the reader left off
                auto it = m_lastTop.find(v->board.Key() + "/" + v->key);
                if (it != m_lastTop.end() && v->GotoAnchor(wxString::Format("p%d", it->second))) break;
            }
            v->Scroll(-1, 1000000);
            break;
        case SCR_TOP: v->Scroll(0, 0); break;
        case SCR_BOTTOM: v->Scroll(-1, 1000000); break;
    }
}

int MainFrame::TopPx(ThreadView* v) {
    int u = 0, ppy = 1;
    v->GetViewStart(nullptr, &u);
    v->GetScrollPixelsPerUnit(nullptr, &ppy);
    return u * std::max(1, ppy);
}

bool MainFrame::AnchorY(ThreadView* v, int no, int& y) {
    wxHtmlContainerCell* root = v->GetInternalRepresentation();
    if (!root) return false;
    wxString name = wxString::Format("p%d", no);
    const wxHtmlCell* c = root->Find(wxHTML_COND_ISANCHOR, &name);
    if (!c) return false;
    y = c->GetAbsPos().y;
    return true;
}

// number of the post whose top is at or above pixel row y (binary search over the rendered posts)
int MainFrame::PostNoAtY(ThreadView* v, int y, int* delta) {
    int lo = 0, hi = (int)v->shown.size() - 1, best = -1, by = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, my = 0;
        if (!AnchorY(v, v->shown[mid], my)) { hi = mid - 1; continue; }
        if (my <= y) { best = mid; by = my; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (best < 0) return 0;
    if (delta) *delta = y - by;
    return v->shown[best];
}

int MainFrame::PostAtClick(ThreadView* v, wxPoint pt) {
    int ux, uy;
    v->CalcUnscrolledPosition(pt.x, pt.y, &ux, &uy);
    int no = PostNoAtY(v, uy, nullptr);
    return no > 0 ? no - 1 : -1;   // index into posts
}

// ================================================================ popup
void MainFrame::ShowPopup(ThreadView* v, const wxString& href) {
    if (m_popup->IsShown() && m_popup->href == href) return;
    Palette pal;
    wxString html = "<html><body bgcolor=\"" + Hex(pal.bg) + "\" text=\"" + Hex(pal.fg) + "\" link=\"" + Hex(pal.link) + "\">";
    std::vector<int> idxs;
    int resTotal = 0;
    if (href.StartsWith("res:")) {
        wxStringTokenizer tk(href.Mid(4), ",");
        while (tk.HasMoreTokens()) {
            wxString part = tk.GetNextToken();
            long a = 0, b = 0;
            int dash = part.Find('-');
            if (dash != wxNOT_FOUND) { part.Left(dash).ToLong(&a); part.Mid(dash + 1).ToLong(&b); }
            else { part.ToLong(&a); b = a; }
            if (b < a) b = a;
            for (long n = a; n <= b && n - a < 1000; n++)
                if (n >= 1 && n <= (long)v->posts.size()) {
                    if (idxs.size() < 8) idxs.push_back((int)n - 1);
                    resTotal++;
                }
        }
    } else if (href.StartsWith("id:")) {
        auto it = v->idPosts.find(href.Mid(3));
        if (it != v->idPosts.end())
            for (int i : it->second) { if (idxs.size() >= 8) break; idxs.push_back(i); }
    }
    if (idxs.empty()) return;
    int total = href.StartsWith("id:") ? (int)v->idPosts[href.Mid(3)].size() : resTotal;
    wxHtmlWindow* h = m_popup->html;
    wxClientDC dc(h);
    dc.SetFont(m_font);
    for (int i : idxs) {
        bool isNew = v->markPost > 0 && v->posts[i].no >= v->markPost;
        html += PostHtml(v, i, isNew, false, true, pal, 500 - wxSystemSettings::GetMetric(wxSYS_VSCROLL_X), dc);
    }
    if (total > (int)idxs.size())
        html += "<center><font size=2 color=\"" + Hex(pal.gray) + "\">" + wxString::Format(U("…ほか %d 件"), total - (int)idxs.size()) + "</font></center>";
    html += "</body></html>";
    SetHtmlFonts(h);
    h->SetSize(wxSize(560, 100));
    h->SetPage(html);
    int ph = 100;
    if (h->GetInternalRepresentation()) ph = h->GetInternalRepresentation()->GetHeight() + 4;
    wxPoint mp = wxGetMousePosition();
    int dispIdx = wxDisplay::GetFromPoint(mp);
    wxRect area = wxDisplay(dispIdx == wxNOT_FOUND ? 0 : dispIdx).GetClientArea();
    ph = std::min(ph, std::min(460, area.height / 2));
    wxSize sz(560, ph);
    wxPoint pos(mp.x + 6, mp.y + 18);
    if (pos.x + sz.x > area.GetRight()) pos.x = area.GetRight() - sz.x;
    if (pos.y + sz.y > area.GetBottom()) pos.y = mp.y - sz.y - 6;
    if (pos.x < area.x) pos.x = area.x;
    m_popup->SetSize(pos.x, pos.y, sz.x, sz.y);
    h->SetSize(1, 1, sz.x - 2, sz.y - 2);
    m_popup->href = href;
    m_popup->Show();
}

void MainFrame::HidePopup() {
    if (m_popup && m_popup->IsShown()) m_popup->Hide();
    if (m_popup) m_popup->href.clear();
}

void MainFrame::OnHoverTimer(wxTimerEvent&) {
    if (!m_popup->IsShown()) return;
    wxPoint mp = wxGetMousePosition();
    if (m_popup->Bounds().Contains(mp)) return;
    ThreadView* v = CurView();
    if (v && m_overLink && wxRect(v->GetScreenPosition(), v->GetSize()).Contains(mp)) return;
    if ((wxGetLocalTimeMillis() - m_lastLinkTick) > 250) HidePopup();
}

// wxHtml reports hover only for movement *inside* a cell it already entered, so small links such as
// ">>12" were often crossed without a popup. Hit-test every mouse move ourselves instead.
void MainFrame::OnHtmlMotion(wxMouseEvent& e) {
    e.Skip();
    ThreadView* v = dynamic_cast<ThreadView*>(e.GetEventObject());
    if (!v || e.LeftIsDown() || !v->GetInternalRepresentation()) return;
    int ux, uy;
    v->CalcUnscrolledPosition(e.GetX(), e.GetY(), &ux, &uy);
    wxString href;
    if (wxHtmlCell* c = v->GetInternalRepresentation()->FindCellByPos(ux, uy)) {
        wxPoint rel = wxPoint(ux, uy) - c->GetAbsPos();
        if (wxHtmlLinkInfo* li = c->GetLink(rel.x, rel.y)) href = li->GetHref();
    }
    m_lastLinkTick = wxGetLocalTimeMillis();
    m_overLink = href.StartsWith("res:") || href.StartsWith("id:");
    if (m_overLink) ShowPopup(v, href);
}

// ================================================================ html interactions
void MainFrame::InsertReply(ThreadView* v, int idx, bool quote) {
    if (idx < 0 || idx >= (int)v->posts.size()) return;
    if (!m_postPanel->IsShown()) {
        m_postPanel->Show();
        GetMenuBar()->Check(ID_POSTPANEL, true);
        m_postPanel->GetParent()->Layout();
    }
    const Post& p = v->posts[idx];
    wxString t = wxString::Format(">>%d\n", p.no);
    if (quote) {
        wxStringTokenizer tk(p.body, "\n");
        int n = 0;
        while (tk.HasMoreTokens() && n++ < 10) t += "> " + tk.GetNextToken() + "\n";
    }
    m_pMsg->SetInsertionPointEnd();
    m_pMsg->WriteText(t);
    m_pMsg->SetFocus();
}

void MainFrame::OnHtmlLink(wxHtmlLinkEvent& e) {
    ThreadView* v = dynamic_cast<ThreadView*>(e.GetEventObject());
    if (!v) return;
    wxString href = e.GetLinkInfo().GetHref();
    v->selActive = false;   // a link click is not a text selection
    HidePopup();
    if (href.StartsWith("http")) {
        const wxMouseEvent* me = e.GetLinkInfo().GetEvent();
        if ((me && me->ShiftDown()) || !OpenBbsUrl(href)) wxLaunchDefaultBrowser(href);   // Shift+click: browser
    } else if (href.StartsWith("num:")) {
        long n = 0;
        href.Mid(4).ToLong(&n);
        InsertReply(v, (int)n - 1, false);
    } else if (href.StartsWith("res:")) {
        long n = 0;
        wxString first = href.Mid(4).BeforeFirst('-').BeforeFirst(',');
        if (first.ToLong(&n) && n >= 1) JumpToPost(v, (int)n);
    } else if (href.StartsWith("id:")) {
        wxString id = href.Mid(3);
        wxMenu m;
        m.Append(1, U("このIDで抽出"));
        m.Append(2, "ID:" + id + U(" をNGに追加"));
        m.Append(3, U("IDをコピー"));
        int r = GetPopupMenuSelectionFromUser(m);
        if (r == 1) { v->idFilter = id; v->scroll = SCR_TOP; RenderView(v); }
        else if (r == 2) AddNgId(id);
        else if (r == 3) CopyText("ID:" + id);
    } else if (href == "clear:") {
        v->idFilter.clear(); v->scroll = SCR_NEW; RenderView(v);
    } else if (href == "all:") {
        m_viewMode->SetSelection(0); v->scroll = SCR_TOP; RenderView(v);
    } else if (href == "reload:") {
        v->scroll = SCR_KEEP;
        LoadView(v);
    }
}

void MainFrame::OnHtmlRight(wxMouseEvent& e) {
    ThreadView* v = dynamic_cast<ThreadView*>(e.GetEventObject());
    if (!v) { e.Skip(); return; }
    int idx = PostAtClick(v, e.GetPosition());
    m_ctxView = v;
    m_ctxPost = idx;
    wxMenu m;
    if (idx >= 0) {
        const Post& p = v->posts[idx];
        m.Append(ID_CTX_REPLY, wxString::Format(U(">>%d に返信"), p.no));
        m.Append(ID_CTX_QUOTE, wxString::Format(U(">>%d を引用して返信"), p.no));
        m.AppendSeparator();
        if (!p.id.empty()) {
            m.Append(ID_CTX_IDFILTER, "ID:" + p.id + U(" で抽出"));
            m.Append(ID_CTX_NGID, "ID:" + p.id + U(" をNG"));
        }
        m.Append(ID_CTX_MINE, v->mine.count(p.no) ? U("自分の書き込みの印を外す") : U("自分の書き込みとして印を付ける"));
        m.Append(ID_CTX_NGPOST, U("このレスをNG(あぼーん)"));
        m.AppendSeparator();
        m.Append(ID_CTX_COPYPOST, U("このレスをコピー"));
    }
    m.Append(ID_CTX_COPY, U("選択範囲をコピー"))->Enable(!v->SelectionToText().empty());
    PopupMenu(&m);
}

void MainFrame::OnHtmlWheel(wxMouseEvent& e) {
    if (e.ControlDown()) SetZoom(e.GetWheelRotation() > 0 ? 1 : -1);
    else e.Skip();
}

void MainFrame::SetZoom(int d) {
    m_fontPt = std::max(6, std::min(40, m_fontPt + d));
    ApplyFont();
}

// ================================================================ tab events
void MainFrame::OnPageChanged(wxAuiNotebookEvent& e) {
    HidePopup();
    if (e.GetOldSelection() != wxNOT_FOUND && e.GetOldSelection() < (int)m_nb->GetPageCount())
        RememberTop(dynamic_cast<ThreadView*>(m_nb->GetPage(e.GetOldSelection())));
    ThreadView* v = dynamic_cast<ThreadView*>(m_nb->GetPage(e.GetSelection()));
    if (v) {
        if (v->dirty) {
            if (v->unread > 0 && v->markPost > 0) v->scroll = SCR_NEW;
            RenderView(v);
        }
        MarkRead(v);
    }
    UpdateInfo();
    e.Skip();
}

void MainFrame::OnPageClose(wxAuiNotebookEvent& e) {
    ThreadView* v = dynamic_cast<ThreadView*>(m_nb->GetPage(e.GetSelection()));
    RememberTop(v);
    if (v) m_views.erase(v->uid);
    HidePopup();
    CallAfter([this] { UpdateInfo(); });
    e.Skip();
}

void MainFrame::OnTabRight(wxAuiNotebookEvent& e) {
    int idx = e.GetSelection();
    ThreadView* v = dynamic_cast<ThreadView*>(m_nb->GetPage(idx));
    if (!v) return;
    wxMenu m;
    m.Append(1, U("更新"));
    m.Append(2, U("ブラウザで開く"));
    m.Append(3, U("URLをコピー"));
    m.AppendSeparator();
    m.Append(4, U("閉じる"));
    m.Append(5, U("他のタブを閉じる"));
    m.Append(6, U("すべて閉じる"));
    switch (GetPopupMenuSelectionFromUser(m)) {
        case 1: v->scroll = SCR_KEEP; LoadView(v); break;
        case 2: wxLaunchDefaultBrowser(ThreadUrl(v)); break;
        case 3: CopyText(ThreadUrl(v)); break;
        case 4: CloseTab(idx); break;
        case 5: CloseTabs(idx); break;
        case 6: CloseTabs(-1); break;
    }
}

void MainFrame::OnTabMiddle(wxAuiNotebookEvent& e) { CloseTab(e.GetSelection()); }

void MainFrame::CloseTabs(int keepIdx) {
    ThreadView* keep = keepIdx >= 0 ? dynamic_cast<ThreadView*>(m_nb->GetPage(keepIdx)) : nullptr;
    for (int i = (int)m_nb->GetPageCount() - 1; i >= 0; i--) {
        ThreadView* v = dynamic_cast<ThreadView*>(m_nb->GetPage(i));
        if (!v || v == keep) continue;
        m_views.erase(v->uid);
        m_nb->DeletePage(i);
    }
    HidePopup();
    UpdateInfo();
}

// ================================================================ posting
bool MainFrame::ConfirmPost(const wxString& terms) {
    wxDialog dlg(this, wxID_ANY, U("書き込み確認"), wxDefaultPosition, wxSize(620, 520), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    wxStaticText* head = new wxStaticText(&dlg, wxID_ANY, U("掲示板から以下の確認がありました。内容を読み、同意する場合のみ書き込んでください。"));
    wxTextCtrl* body = new wxTextCtrl(&dlg, wxID_ANY, terms, wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY);
    wxBoxSizer* s = new wxBoxSizer(wxVERTICAL);
    s->Add(head, 0, wxALL, 8);
    s->Add(body, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);
    wxStdDialogButtonSizer* bs = new wxStdDialogButtonSizer;
    wxButton* ok = new wxButton(&dlg, wxID_OK, U("同意して書き込む"));
    bs->AddButton(ok);
    bs->AddButton(new wxButton(&dlg, wxID_CANCEL, U("キャンセル")));
    bs->Realize();
    s->Add(bs, 0, wxEXPAND | wxALL, 8);
    dlg.SetSizer(s);
    body->SetInsertionPoint(0);
    ok->SetDefault();
    return dlg.ShowModal() == wxID_OK;
}

void MainFrame::ShowAbout() {
    wxString notices;
    wxFileName exe(wxStandardPaths::Get().GetExecutablePath());
    wxString cands[] = {exe.GetPathWithSep() + "THIRD_PARTY_NOTICES.txt",
                        exe.GetPathWithSep() + "../share/doc/5chviewer/THIRD_PARTY_NOTICES.txt",
                        exe.GetPathWithSep() + "packaging/THIRD_PARTY_NOTICES.txt",
                        "/usr/share/doc/5chviewer/THIRD_PARTY_NOTICES.txt"};
    for (auto& c : cands) if (wxFileExists(c)) { wxFileName f(c); f.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE); notices = f.GetFullPath(); break; }
    wxString text = U("5ch Viewer ") + APP_VERSION + "\n\n" +
        U("非公式の5ch閲覧ソフトです。5ch(5ちゃんねる)の運営とは関係ありません。\n"
          "本ソフトは MIT License で提供されます (LICENSE)。\n"
          "投稿内容の権利は投稿者・運営に帰属します。\n\n"
          "使用しているソフトウェア: wxWidgets 3.2 (wxWindows Library Licence)、libpng、zlib、"
          "Henry Spencer 正規表現ライブラリ、") +
#ifdef __WXMSW__
        U("GCC/mingw-w64 ランタイム、NSIS(インストーラー)") +
#else
        U("libcurl") +
#endif
        "\n\n" + (notices.empty() ? U("ライセンス表記: THIRD_PARTY_NOTICES.txt") : U("ライセンス表記: ") + notices);
    if (notices.empty()) { wxMessageBox(text, U("このソフトについて"), wxOK | wxICON_INFORMATION, this); return; }
    wxMessageDialog dlg(this, text, U("このソフトについて"), wxYES_NO | wxICON_INFORMATION);
    dlg.SetYesNoLabels(U("ライセンス表記を開く"), U("閉じる"));
    if (dlg.ShowModal() == wxID_YES) wxLaunchDefaultApplication(notices);
}

void MainFrame::SendPost() {
    ThreadView* v = CurView();
    if (!v) {
        wxMessageBox(U("書き込むスレッドを開いてください"), U("書き込み"));
        return;
    }
    wxString msg = m_pMsg->GetValue();
    if (msg.Trim().empty()) return;
    m_sendBtn->Disable();
    DoPost(v->board, v->key, v->uid, false, "", m_pName->GetValue(), m_pMail->GetValue(), msg, nullptr, 0);
}

void MainFrame::NewThread() {
    const Board* cb = CurBoard();
    if (!cb) return;
    wxDialog dlg(this, wxID_ANY, U("スレッドを立てる - ") + cb->name, wxDefaultPosition, wxSize(520, 340),
                 wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    wxTextCtrl* subj = new wxTextCtrl(&dlg, wxID_ANY);
    subj->SetHint(U("スレッドタイトル"));
    wxTextCtrl* name = new wxTextCtrl(&dlg, wxID_ANY, m_pName->GetValue());
    wxTextCtrl* mail = new wxTextCtrl(&dlg, wxID_ANY, m_pMail->GetValue());
    wxTextCtrl* msg = new wxTextCtrl(&dlg, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE);
    wxBoxSizer* s = new wxBoxSizer(wxVERTICAL);
    s->Add(subj, 0, wxEXPAND | wxALL, 4);
    wxBoxSizer* h = new wxBoxSizer(wxHORIZONTAL);
    h->Add(name, 1, wxRIGHT, 4);
    h->Add(mail, 1);
    s->Add(h, 0, wxEXPAND | wxALL, 4);
    s->Add(msg, 1, wxEXPAND | wxALL, 4);
    s->Add(dlg.CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 4);
    dlg.SetSizer(s);
    if (dlg.ShowModal() != wxID_OK) return;
    if (subj->GetValue().empty() || msg->GetValue().empty()) return;
    DoPost(*cb, "", 0, true, subj->GetValue(), name->GetValue(), mail->GetValue(), msg->GetValue(), nullptr, 0);
}

void MainFrame::DoPost(const Board& b, const wxString& key, int uid, bool newThread, const wxString& subject,
                       const wxString& name, const wxString& mail, const wxString& msg,
                       const std::vector<std::pair<wxString, wxString>>* confirm, int depth) {
    std::vector<std::pair<std::string, std::string>> fields;
    auto add = [&](const char* k, const wxString& val) { fields.emplace_back(k, StrToSjis(val)); };
    if (confirm) {
        for (auto& kv : *confirm) fields.emplace_back(std::string(kv.first.ToAscii()), StrToSjis(kv.second));
    } else {
        add("bbs", b.bbs);
        if (newThread) add("subject", subject);
        else add("key", key);
        add("time", wxString::Format("%ld", (long)time(nullptr) - 60));
        add("FROM", name);
        add("mail", mail);
        add("MESSAGE", msg);
        add("submit", newThread ? U("新規スレッド作成") : U("書き込む"));
        add("oekaki_thread1", "");
    }
    std::string body;
    for (auto& f : fields) {
        if (!body.empty()) body += "&";
        body += f.first + "=" + UrlEncode(f.second);
    }
    wxString referer = newThread ? b.BaseUrl() : "https://" + b.host + "/test/read.cgi/" + b.bbs + "/" + key + "/";
    std::map<wxString, wxString> hd{{"Referer", referer}, {"Origin", "https://" + b.host}};
#ifndef __WXMSW__    // libcurl backend has no cookie engine; WinHTTP keeps cookies itself
    if (!m_cookies.empty()) {
        wxString ck;
        for (auto& kv : m_cookies) ck += (ck.empty() ? "" : "; ") + kv.first + "=" + kv.second;
        hd["Cookie"] = ck;
    }
#endif
    Status(U("書き込み中..."));
    Http("https://" + b.host + "/test/bbs.cgi", &body, hd,
         [this, newThread, subject, name, mail, msg, depth, b, key, uid](HttpResult& r) {
        m_sendBtn->Enable();
        if (!r.ok) {
            Status(U("書き込み失敗: ") + r.err);
            wxMessageBox(U("通信エラー: ") + r.err, U("書き込み"), wxICON_ERROR);
            return;
        }
        if (!r.setCookie.empty()) {
            wxString first = r.setCookie.BeforeFirst(';');
            int eq = first.Find('=');
            if (eq > 0) { m_cookies[first.Left(eq).Trim().Trim(false)] = first.Mid(eq + 1); SaveCookies(); }
        }
        wxString html = SjisToStr(r.body.data(), r.body.size());
        if (html.Contains(U("書きこみました")) || html.Contains(U("書き込みました"))) {
            Status(U("書き込みました"));
            if (!newThread) m_pendingMine.push_back({b.Key() + "/" + key, NormBody(msg), 0});
            m_pMsg->Clear();
            if (newThread) CallAfter([this] { LoadSubject(CurBoardTab()); });
            else {
                ThreadView* tv = FindView(uid);
                if (tv) tv->scroll = SCR_BOTTOM;
                // the server needs a moment before the new post shows up in the dat
                m_postUid = uid;
                m_postTimer.StartOnce(800);
            }
            return;
        }
        // confirmation page: hidden fields must be re-submitted
        std::vector<std::pair<wxString, wxString>> hidden;
        wxRegEx tagRe("<input[^>]*>", wxRE_ADVANCED | wxRE_ICASE);
        wxRegEx nameRe("name=[\"']?([A-Za-z0-9_]+)", wxRE_ADVANCED | wxRE_ICASE);
        wxRegEx valRe("value=\"([^\"]*)\"|value='([^']*)'", wxRE_ADVANCED | wxRE_ICASE);
        wxString rest = html;
        bool hasMsg = false;
        while (tagRe.Matches(rest)) {
            size_t st, ln;
            tagRe.GetMatch(&st, &ln, 0);
            wxString tag = rest.Mid(st, ln);
            rest = rest.Mid(st + ln);
            if (!nameRe.Matches(tag)) continue;
            wxString nm = nameRe.GetMatch(tag, 1), val;
            if (valRe.Matches(tag)) val = valRe.GetMatch(tag, 1).empty() ? valRe.GetMatch(tag, 2) : valRe.GetMatch(tag, 1);
            wxString low = tag.Lower();
            if (!low.Contains("hidden") && !low.Contains("submit")) continue;
            if (nm == "MESSAGE") hasMsg = true;
            hidden.emplace_back(nm, DecodeEntities(val));
        }
        if (hasMsg && depth < 2) {
            // the confirmation page carries the posting terms: show it and let the user decide
            bool hasSubmit = false;
            for (auto& h : hidden) if (h.first == "submit") hasSubmit = true;
            if (!hasSubmit) hidden.emplace_back("submit", U("上記全てを承諾して書き込む"));
            wxString terms = HtmlToText(html);
            wxRegEx blank("\\n[ \\t\\n]*\\n+", wxRE_ADVANCED);
            blank.ReplaceAll(&terms, "\n\n");
            terms.Trim().Trim(false);
            if (!ConfirmPost(terms)) { Status(U("書き込みを中止しました")); return; }
            m_sendBtn->Disable();
            DoPost(b, key, uid, newThread, subject, name, mail, msg, &hidden, depth + 1);
            return;
        }
        wxString txt = HtmlToText(html);
        wxRegEx ws("\\n[ \\t\\n]*\\n+", wxRE_ADVANCED);
        ws.ReplaceAll(&txt, "\n");
        if (txt.length() > 1500) txt = txt.Left(1500) + "...";
        Status(U("書き込みに失敗しました"));
        wxMessageBox(txt, U("書き込み結果"), wxICON_WARNING);
    });
}

void MainFrame::OnPostKey(wxKeyEvent& e) {
    if (e.GetKeyCode() == WXK_RETURN && e.ControlDown()) SendPost();
    else e.Skip();
}

// ================================================================ dialogs / actions
static bool Is5chHost(const wxString& host) {
    wxString h = host.Lower();
    return h.EndsWith(".5ch.net") || h.EndsWith(".5ch.io") || h.EndsWith(".bbspink.com") || h.EndsWith(".2ch.sc");
}

// thread / board URLs of 5ch open as tabs here instead of the browser; returns false for other URLs
bool MainFrame::OpenBbsUrl(const wxString& url) {
    static wxRegEx rt("^https?://([a-z0-9.-]+)/test/read\\.cgi/([A-Za-z0-9_]+)/([0-9]+)(/([^?#]*))?", wxRE_ADVANCED | wxRE_ICASE);
    static wxRegEx ri("^https?://itest\\.([a-z0-9.-]+)/([a-z0-9]+)/test/read\\.cgi/([A-Za-z0-9_]+)/([0-9]+)(/([^?#]*))?", wxRE_ADVANCED | wxRE_ICASE);
    static wxRegEx rb("^https?://([a-z0-9.-]+)/([A-Za-z0-9_]+)/?$", wxRE_ADVANCED | wxRE_ICASE);
    wxString host, bbs, key, rest;
    if (ri.Matches(url)) {   // smartphone layout: itest.5ch.net/<server>/test/read.cgi/<bbs>/<key>/
        host = ri.GetMatch(url, 2) + "." + ri.GetMatch(url, 1);
        bbs = ri.GetMatch(url, 3); key = ri.GetMatch(url, 4); rest = ri.GetMatch(url, 6);
    } else if (rt.Matches(url)) {
        host = rt.GetMatch(url, 1); bbs = rt.GetMatch(url, 2); key = rt.GetMatch(url, 3); rest = rt.GetMatch(url, 5);
    } else if (rb.Matches(url)) {
        host = rb.GetMatch(url, 1); bbs = rb.GetMatch(url, 2);
        if (host.StartsWith("www.") || host.StartsWith("menu.") || host.StartsWith("itest.")) return false;
    } else return false;
    if (!Is5chHost(host)) return false;
    int idx = FindBoard(host, bbs);
    if (idx < 0) {
        Board b;
        b.name = bbs; b.host = host; b.bbs = bbs; b.cat = U("その他");
        m_boards.push_back(b);
        idx = (int)m_boards.size() - 1;
        RebuildTree();
    }
    if (key.empty()) { SelectBoard(idx); return true; }
    long no = 0;
    rest.BeforeFirst('-').BeforeFirst(',').ToLong(&no);   // ".../123" or ".../123-130": jump to that post
    ThreadView* v = OpenThreadTab(m_boards[idx], key, "(" + key + ")", true);
    if (v && no > 0) {
        if (!v->posts.empty() && v->GetInternalRepresentation()) JumpToPost(v, (int)no);
        else v->jumpTo = (int)no;
    }
    return true;
}

void MainFrame::OpenUrlDialog() {
    wxTextEntryDialog dlg(this, U("スレッドまたは板のURL:"), U("URLを開く"));
    if (dlg.ShowModal() != wxID_OK) return;
    wxString url = dlg.GetValue().Trim().Trim(false);
    if (url.StartsWith("ttp")) url = "h" + url;
    if (!OpenBbsUrl(url)) wxMessageBox(U("5chのスレッドまたは板のURLを入力してください"), U("URLを開く"), wxICON_WARNING);
}

void MainFrame::NgDialog() {
    wxDialog dlg(this, wxID_ANY, U("NG設定"), wxDefaultPosition, wxSize(460, 380), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    wxStaticText* st = new wxStaticText(&dlg, wxID_ANY, U("1行1件。通常=本文に含む語 / ID:xxxx=ID指定 / name:xxxx=名前に含む"));
    wxTextCtrl* t = new wxTextCtrl(&dlg, wxID_ANY, m_ngText, wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE);
    wxBoxSizer* s = new wxBoxSizer(wxVERTICAL);
    s->Add(st, 0, wxALL, 6);
    s->Add(t, 1, wxEXPAND | wxLEFT | wxRIGHT, 6);
    s->Add(dlg.CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 6);
    dlg.SetSizer(s);
    if (dlg.ShowModal() != wxID_OK) return;
    m_ngText = t->GetValue();
    ParseNg();
    ReRenderAll();
}

void MainFrame::FindInThread() {
    ThreadView* v = CurView();
    if (!v) return;
    wxTextEntryDialog dlg(this, U("検索語 (次の該当レスへ移動します):"), U("スレ内検索"), m_findText);
    if (dlg.ShowModal() != wxID_OK) return;
    m_findText = dlg.GetValue();
    if (m_findText.empty() || v->posts.empty()) return;
    wxString q = m_findText.Lower();
    int cur = PostNoAtY(v, TopPx(v) + 2, nullptr);   // start after the post at the top of the view
    int n = (int)v->posts.size(), hits = 0, target = -1;
    for (int k = 0; k < n; k++) {
        const Post& p = v->posts[(cur + k) % n];
        if (p.body.Lower().Contains(q) || p.name.Lower().Contains(q)) {
            hits++;
            if (target < 0) target = (cur + k) % n;
        }
    }
    if (hits == 0) { Status(U("見つかりません: ") + m_findText); return; }
    JumpToPost(v, v->posts[target].no);
    Status(wxString::Format(U("「%s」 %d 件: >>%d へ移動"), m_findText, hits, v->posts[target].no));
}

void MainFrame::SetAuto(int sec) {
    m_autoSec = sec;
    m_timer.Stop();
    if (sec > 0) m_timer.Start(sec * 1000);
    Status(sec ? wxString::Format(U("自動更新: %d秒ごと"), sec) : U("自動更新: 停止"));
}

void MainFrame::OnMenuAction(wxCommandEvent& e) {
    ThreadView* v = CurView();
    switch (e.GetId()) {
        case wxID_EXIT: Close(); break;
        case wxID_ABOUT: ShowAbout(); break;
        case ID_OPEN_URL: OpenUrlDialog(); break;
        case ID_MENU_UPDATE: LoadBoardMenu(true); break;
        case ID_BOARD_RELOAD: LoadSubject(CurBoardTab()); break;
        case ID_FAV: ToggleFav(); break;
        case ID_NEWTHREAD: NewThread(); break;
        case ID_THREAD_RELOAD: if (v) { v->scroll = SCR_KEEP; LoadView(v); } break;
        case ID_RELOAD_ALL: ReloadAll(true); break;
        case ID_NEXT_NEW:
        case ID_NEWPOS:
            if (v && v->markPost > 0) { v->scroll = SCR_NEW; RenderView(v); }
            else Status(U("新着はありません"));
            break;
        case ID_TOP: if (v) { v->scroll = SCR_TOP; RenderView(v); } break;
        case ID_BOTTOM: if (v) { v->scroll = SCR_BOTTOM; RenderView(v); } break;
        case ID_AUTO0: SetAuto(0); break;
        case ID_AUTO60: SetAuto(60); break;
        case ID_AUTO120: SetAuto(120); break;
        case ID_AUTO300: SetAuto(300); break;
        case ID_FIND: FindInThread(); break;
        case ID_BROWSER: if (v) wxLaunchDefaultBrowser(ThreadUrl(v)); break;
        case ID_THREAD_COPYURL: if (v) CopyText(ThreadUrl(v)); break;
        case ID_CLOSE_TAB: { int s = m_nb->GetSelection(); if (s != wxNOT_FOUND) CloseTab(s); break; }
        case ID_CLOSE_ALL: CloseTabs(-1); break;
        case ID_TAB_NEXT: case ID_TAB_PREV: {
            int n = (int)m_nb->GetPageCount();
            if (n > 1) m_nb->SetSelection((m_nb->GetSelection() + (e.GetId() == ID_TAB_NEXT ? 1 : n - 1)) % n);
            break;
        }
        case ID_ZOOM_IN: SetZoom(1); break;
        case ID_ZOOM_OUT: SetZoom(-1); break;
        case ID_FONT: {
            wxFontData fd;
            fd.SetInitialFont(m_font);
            wxFontDialog dlg(this, fd);
            if (dlg.ShowModal() == wxID_OK) {
                m_font = dlg.GetFontData().GetChosenFont();
                m_fontPt = m_font.GetPointSize();
                ApplyFont();
            }
            break;
        }
        case ID_POSTPANEL:
            m_postPanel->Show(e.IsChecked());
            m_postPanel->GetParent()->Layout();
            break;
        case ID_NG: NgDialog(); break;
        case ID_CTX_REPLY: if (m_ctxView) InsertReply(m_ctxView, m_ctxPost, false); break;
        case ID_CTX_QUOTE: if (m_ctxView) InsertReply(m_ctxView, m_ctxPost, true); break;
        case ID_CTX_IDFILTER:
            if (m_ctxView && m_ctxPost >= 0) { m_ctxView->idFilter = m_ctxView->posts[m_ctxPost].id; m_ctxView->scroll = SCR_TOP; RenderView(m_ctxView); }
            break;
        case ID_CTX_NGID: if (m_ctxView && m_ctxPost >= 0) AddNgId(m_ctxView->posts[m_ctxPost].id); break;
        case ID_CTX_MINE:
            if (m_ctxView && m_ctxPost >= 0) {
                int no = m_ctxView->posts[m_ctxPost].no;
                std::set<int>& ms = m_mine[TKey(m_ctxView)];
                if (ms.count(no)) ms.erase(no); else ms.insert(no);
                ComputeMine(m_ctxView);
                m_ctxView->scroll = SCR_KEEP;
                RenderView(m_ctxView);
                SaveReadState();
            }
            break;
        case ID_CTX_NGPOST:
            if (m_ctxView && m_ctxPost >= 0) {
                m_ngPosts.insert({m_ctxView->board.Key() + "/" + m_ctxView->key, m_ctxView->posts[m_ctxPost].no});
                m_ctxView->scroll = SCR_KEEP; RenderView(m_ctxView);
            }
            break;
        case ID_CTX_COPYPOST: if (m_ctxView && m_ctxPost >= 0) CopyText(PlainPost(m_ctxView, m_ctxPost)); break;
        case ID_CTX_COPY: if (m_ctxView) CopyText(m_ctxView->SelectionToText()); break;
        default: e.Skip();
    }
}

void MainFrame::OnClose(wxCloseEvent& e) {
    m_timer.Stop();
    m_hoverTimer.Stop();
    m_postTimer.Stop();
    HidePopup();
    for (auto& kv : m_views) RememberTop(kv.second);
    SaveConfig();
    // no callbacks into a dying frame
    Unbind(wxEVT_WEBREQUEST_STATE, &MainFrame::OnWebState, this);
    for (auto& kv : m_pending) kv.second.req.Cancel();
    m_pending.clear();
    e.Skip();
}

// ================================================================ app
class App : public wxApp {
public:
    bool OnInit() override {
        SetAppName("5chviewer");
        SetVendorName("5chviewer");
        if (!wxApp::OnInit()) return false;
        if (getenv("V5CH_DEBUG")) delete wxLog::SetActiveTarget(new wxLogStderr);   // log to the console instead of dialogs
        wxString dir = wxStandardPaths::Get().GetUserDataDir();
        if (!wxDirExists(dir)) wxFileName::Mkdir(dir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
        wxConfigBase::Set(new wxFileConfig("5chviewer", "", dir + wxFileName::GetPathSeparator() + "settings.ini", "",
                                           wxCONFIG_USE_LOCAL_FILE));
        MainFrame* f = new MainFrame;
        f->Show();
        return true;
    }
};

wxIMPLEMENT_APP(App);

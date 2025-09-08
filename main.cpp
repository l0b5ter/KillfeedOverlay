// main.cpp — PS2 deaths overlay (Hybrid PEAK + Batch, pagination + robust dedupe)
// True transparent background via per-pixel alpha (no fringes) + configurable text color.
//
// Build:
//   g++ -std=gnu++17 -O2 -Wall -Wextra main.cpp -lgdi32 -luser32 -lwininet -mwindows -o Killfeed.exe

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <wininet.h>
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <algorithm>
#include <sstream>
#include <cstdio>

// =========================== Config & Globals ===============================
struct WinCfg { int x=100, y=100, w=520, h=220, alpha=230; };
struct AppCfg {
    std::wstring service_id;
    std::wstring character_name;
    int  world_id = 0;

    WinCfg window;
    int  poll_ms = 1000;
    bool lock_position    = true;
    bool always_on_top    = true;
    bool skip_environment = true;

    // If true: per-pixel alpha (recommended; no fringe).
    // If false: legacy window with uniform alpha.
    bool transparent_bg   = false;

    // Legacy chroma (ignored when transparent_bg=true)
    int  chroma_r         = 255;
    int  chroma_g         = 0;
    int  chroma_b         = 255;

    // Configurable text color
    int  text_r           = 0;
    int  text_g           = 0;
    int  text_b           = 0;
} g_cfg;

static const wchar_t* g_apiHost = L"census.daybreakgames.com";
static bool           g_apiUseHttps = true;
static unsigned short g_apiPort     = 0;

static HWND g_hwnd = nullptr;
static UINT TIMER_MS = 1000;
static const UINT_PTR TIMER_ID = 1;

static std::wstring g_status = L"Waiting for data…";
static std::wstring g_line   = L"(no deaths yet)";
static std::wstring g_characterId;

// De-dup state
static std::unordered_set<std::wstring> g_seenEventIds;
static unsigned long long g_lastDeathTs = 0;

// Synthetic dedupe
static std::unordered_set<std::wstring> g_seenSynth;
static std::deque<std::wstring>         g_seenQueue;
static const size_t                     g_seenMax = 2048;

// Per-attacker counters + name cache
struct Counters { int hs=0; int tot=0; };
static std::unordered_map<std::wstring, Counters>     g_counts;
static std::unordered_map<std::wstring, std::wstring> g_nameCache;
static std::wstring g_lastAttackerId   = L"";
static std::wstring g_lastAttackerName = L"(unknown)";

// Context menu
static HMENU g_ctxMenu = nullptr;
#define IDM_EXIT  1001

// For legacy chroma path (ignored when transparent_bg=true)
static COLORREF g_chroma    = RGB(255,0,255);
static COLORREF g_textColor = RGB(0,0,0);

// =========================== Utilities ======================
template<typename T>
static std::wstring to_wstring_compat(T v){ std::wstringstream ss; ss<<v; return ss.str(); }

static int Clamp255(int v){ return v<0?0:(v>255?255:v); }

static std::wstring Utf8ToWide(const std::string& s){
    if(s.empty()) return L"";
    int need = MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0);
    std::wstring out(need, L'\0');
    if(need) MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),&out[0],need);
    return out;
}

static bool ReadFileUtf8(const std::wstring& path, std::string& out){
    out.clear();
    FILE* f = _wfopen(path.c_str(), L"rb");
    if(!f) return false;
    char buf[4096]; size_t n;
    while((n=fread(buf,1,sizeof(buf),f))>0) out.append(buf, buf+n);
    fclose(f);
    if(out.size()>=3 && (unsigned char)out[0]==0xEF &&
       (unsigned char)out[1]==0xBB && (unsigned char)out[2]==0xBF)
        out.erase(0,3);
    return true;
}

static std::wstring ToLowerAscii(const std::wstring& s){
    std::wstring t = s;
    for(auto& ch : t) if(ch >= L'A' && ch <= L'Z') ch = (wchar_t)(ch - L'A' + L'a');
    return t;
}

static std::wstring GetExecutableDir(){
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* last = nullptr;
    for (wchar_t* p = path; *p; ++p) if (*p == L'\\' || *p == L'/') last = p;
    if (last) *last = L'\0';
    return path;
}

static unsigned long long ParseULL(const std::wstring& w){
    unsigned long long v = 0;
    for(wchar_t c : w){
        if(c < L'0' || c > L'9') break;
        v = v*10 + (unsigned)(c - L'0');
    }
    return v;
}

static void SkipWs(const std::string& j, size_t& i){
    while(i<j.size() && (j[i]==' '||j[i]=='\t'||j[i]=='\n'||j[i]=='\r')) ++i;
}

static bool JsonFindString(const std::string& j, const char* key, std::string& outUtf8){
    size_t i=0; std::string quoted = std::string("\"")+key+"\"";
    while(true){
        size_t kpos = j.find(quoted, i); if(kpos==std::string::npos) return false;
        i = kpos + quoted.size();
        size_t colon = j.find(':', i); if(colon==std::string::npos) return false;
        i = colon+1; SkipWs(j,i);
        if(i>=j.size() || j[i] != '"'){ i = kpos + 1; continue; }
        ++i;
        std::string val;
        while(i<j.size() && j[i]!='"'){
            char c=j[i++];
            if(c=='\\' && i<j.size()){ char esc=j[i++]; val.push_back(esc); }
            else val.push_back(c);
        }
        if(i<j.size() && j[i]=='"') ++i;
        outUtf8 = val;
        return true;
    }
}

static bool JsonFindInt(const std::string& j, const char* key, int& out){
    size_t i=0; std::string quoted = std::string("\"")+key+"\"";
    while(true){
        size_t kpos = j.find(quoted, i); if(kpos==std::string::npos) return false;
        i = kpos + quoted.size();
        size_t colon = j.find(':', i); if(colon==std::string::npos) return false;
        i = colon+1; SkipWs(j,i);
        size_t start=i; if(i<j.size() && (j[i]=='-'||j[i]=='+')) ++i;
        while(i<j.size() && j[i]>='0'&&j[i]<='9') ++i;
        if(start==i){ i = kpos + 1; continue; }
        out = std::stoi(j.substr(start, i-start)); return true;
    }
}

static bool JsonFindBool(const std::string& j, const char* key, bool& out){
    size_t i=0; std::string quoted = std::string("\"")+key+"\"";
    while(true){
        size_t kpos = j.find(quoted, i); if(kpos==std::string::npos) return false;
        i = kpos + quoted.size();
        size_t colon = j.find(':', i); if(colon==std::string::npos) return false;
        i = colon+1; SkipWs(j,i);
        if(j.compare(i,4,"true")==0){ out=true;  return true; }
        if(j.compare(i,5,"false")==0){ out=false; return true; }
        i = kpos + 1;
    }
}

static bool JsonFindStringInFirstArrayObj(const std::string& j,
                                          const char* keyArray,
                                          const char* innerKey,
                                          std::string& outValUtf8)
{
    std::string ka = std::string("\"")+keyArray+"\"";
    size_t a = j.find(ka); if(a==std::string::npos) return false;
    size_t lb = j.find('[', a); if(lb==std::string::npos) return false;
    size_t ob = j.find('{', lb); if(ob==std::string::npos) return false;
    std::string sub = j.substr(ob, std::min<size_t>(j.size()-ob, 4096));
    return JsonFindString(sub, innerKey, outValUtf8);
}

// ====================== Networking (WinINet) ======================
static std::string FetchUrlBody(const std::wstring& path){
    std::string body;

    HINTERNET hInternet = InternetOpenW(L"PS2Overlay/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if(!hInternet){ g_status = L"InternetOpen failed"; return body; }

    INTERNET_PORT port = g_apiPort ? g_apiPort : (g_apiUseHttps ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT);
    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_KEEP_CONNECTION;
    if(g_apiUseHttps) flags |= INTERNET_FLAG_SECURE;

    HINTERNET hConnect = InternetConnectW(hInternet, g_apiHost, port, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    if(!hConnect){ g_status = L"InternetConnect failed"; InternetCloseHandle(hInternet); return body; }

    const wchar_t* accept[] = { L"*/*", nullptr };
    HINTERNET hReq = HttpOpenRequestW(hConnect, L"GET", path.c_str(), NULL, NULL, accept, flags, 0);
    if(!hReq){ g_status = L"HttpOpenRequest failed"; InternetCloseHandle(hConnect); InternetCloseHandle(hInternet); return body; }

    if(!HttpSendRequestW(hReq, NULL, 0, NULL, 0)){
        g_status = L"HttpSendRequest failed";
        InternetCloseHandle(hReq); InternetCloseHandle(hConnect); InternetCloseHandle(hInternet);
        return body;
    }

    char buf[4096]; DWORD rd=0;
    while(InternetReadFile(hReq, buf, sizeof(buf), &rd) && rd>0) body.append(buf, buf+rd);

    InternetCloseHandle(hReq); InternetCloseHandle(hConnect); InternetCloseHandle(hInternet);
    return body;
}

// ========================== API builders ==========================
static std::wstring BuildCharacterByNamePath(){
    std::wstring lower = ToLowerAscii(g_cfg.character_name);
    return L"/" + g_cfg.service_id + L"/get/ps2:v2/character?name.first_lower=" + lower;
}
static std::wstring BuildLatestDeathJoinedDesc(const std::wstring& charId){
    return L"/" + g_cfg.service_id +
           L"/get/ps2:v2/characters_event/?character_id=" + charId +
           L"&type=DEATH&c:limit=1&c:sort=timestamp:desc"
           L"&c:join=character^on:attacker_character_id^to:character_id^inject_at:attacker^show:name.first";
}
static std::wstring BuildDeathsSincePath(const std::wstring& charId,
                                         unsigned long long afterTs,
                                         int start,
                                         int limit)
{
    if (limit <= 0) limit = 1000;
    unsigned long long afterTsSafe = (afterTs > 0) ? (afterTs - 1) : 0;

    return L"/" + g_cfg.service_id +
           L"/get/ps2:v2/characters_event/?character_id=" + charId +
           L"&type=DEATH"
           L"&after=" + to_wstring_compat(afterTsSafe) +
           L"&c:limit=" + to_wstring_compat(limit) +
           L"&c:start=" + to_wstring_compat(start) +
           L"&c:sort=timestamp:asc"
           L"&c:join=character^on:attacker_character_id^to:character_id^inject_at:attacker^show:name.first";
}

// ========================== Parsers ============================
static bool ParseCharacterId(const std::string& body, std::wstring& outId){
    std::string idUtf8;
    if(!JsonFindStringInFirstArrayObj(body, "character_list", "character_id", idUtf8))
        return false;
    outId = Utf8ToWide(idUtf8);
    return !outId.empty();
}

static bool TryParseInjectedAttackerName(const std::string& segment, std::wstring& outName){
    size_t pos = segment.find("\"attacker\"");
    if (pos == std::string::npos) return false;
    size_t npos = segment.find("\"name\"", pos);
    if (npos == std::string::npos) return false;
    size_t fpos = segment.find("\"first\"", npos);
    if (fpos == std::string::npos) return false;
    std::string firstUtf8;
    if(!JsonFindString(segment.substr(fpos, 512), "first", firstUtf8)) return false;
    outName = Utf8ToWide(firstUtf8);
    return !outName.empty();
}

struct LatestOne {
    std::wstring attackerId;
    std::wstring attackerName;
    std::wstring eventId;
    unsigned long long ts = 0;
    bool isHS = false;
    bool ok = false;
};

static LatestOne ParseLatestJoinedOne(const std::string& body){
    LatestOne r{};
    std::string s;
    if(!JsonFindString(body, "attacker_character_id", s)) return r;
    r.attackerId = Utf8ToWide(s);
    if(JsonFindString(body, "is_headshot", s)) r.isHS = (s=="1");
    if(JsonFindString(body, "event_id", s))    r.eventId = Utf8ToWide(s);
    if(JsonFindString(body, "timestamp", s))   r.ts = ParseULL(Utf8ToWide(s));
    std::wstring nm; if (TryParseInjectedAttackerName(body, nm)) r.attackerName = nm;
    r.ok = (!r.attackerId.empty() && r.ts!=0);
    return r;
}

struct DeathEvent {
    std::wstring attackerId;
    std::wstring attackerName;
    bool isHS = false;
    std::wstring eventId;
    unsigned long long ts = 0;
};

// Brace-balanced batch parser
static std::vector<DeathEvent> ParseDeathBatch(const std::string& body){
    std::vector<DeathEvent> out;
    size_t arrStart = body.find('[');
    if (arrStart == std::string::npos) return out;
    size_t arrEnd = std::string::npos;
    for (int depth = 0, i = (int)arrStart; i < (int)body.size(); ++i){
        char c = body[i];
        if (c == '[') ++depth;
        else if (c == ']'){ --depth; if (depth == 0){ arrEnd = i; break; } }
    }
    if (arrEnd == std::string::npos) return out;
    const std::string arr = body.substr(arrStart, arrEnd - arrStart + 1);

    size_t i = 0;
    while (i < arr.size()){
        size_t objL = arr.find('{', i);
        if (objL == std::string::npos) break;

        int depth = 0;
        size_t j = objL;
        for (; j < arr.size(); ++j){
            char c = arr[j];
            if (c == '{') ++depth;
            else if (c == '}'){ --depth; if (depth == 0){ ++j; break; } }
        }
        if (j <= objL) break;
        std::string evt = arr.substr(objL, j - objL);

        if (evt.find("\"attacker_character_id\"") != std::string::npos){
            DeathEvent e{};
            std::string s;
            if (JsonFindString(evt, "attacker_character_id", s)) e.attackerId = Utf8ToWide(s);
            if (JsonFindString(evt, "is_headshot", s))          e.isHS = (s == "1");
            if (JsonFindString(evt, "event_id", s))             e.eventId = Utf8ToWide(s);
            if (JsonFindString(evt, "timestamp", s))            e.ts = ParseULL(Utf8ToWide(s));
            std::wstring nm; if (TryParseInjectedAttackerName(evt, nm)) e.attackerName = nm;

            if (!e.attackerId.empty() && e.ts != 0ULL) out.push_back(std::move(e));
        }
        i = j;
    }
    return out;
}

// ========================== Topmost/UI helpers =================
static void SetTopMost(HWND hwnd, bool on){
    SetWindowPos(hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0,0,0,0, SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
}
static void ReassertTopMost(){
    if (g_cfg.always_on_top && g_hwnd) {
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0,0,0,0,
                     SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    }
}
static std::wstring GetDisplayNameFor(const std::wstring& attackerId){
    auto it = g_nameCache.find(attackerId);
    if (it != g_nameCache.end()) return it->second;
    return L"(resolving…)";
}
static void EnsureContextMenu(){
    if (!g_ctxMenu) {
        g_ctxMenu = CreatePopupMenu();
        AppendMenuW(g_ctxMenu, MF_STRING, IDM_EXIT, L"Exit");
    }
}

// ========================== Alpha-layer rendering ============================
// Create a 32-bit DIB section (premultiplied ARGB) and backbuffer DC.
static HBITMAP CreateDIB32(HDC ref, int w, int h, void** outBits){
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h; // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB; // we'll manage alpha manually
    void* bits = nullptr;
    HBITMAP hbmp = CreateDIBSection(ref, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (outBits) *outBits = bits;
    return hbmp;
}

// Draws text as WHITE onto black, then converts white intensity -> alpha and tints to config color.
// Finally pushes to the layered window with UpdateLayeredWindow.
static void RepaintLayered(){
    if (!g_hwnd) return;

    RECT rc; GetClientRect(g_hwnd, &rc);
    int W = rc.right - rc.left, H = rc.bottom - rc.top;
    if (W<=0 || H<=0) return;

    HDC screen = GetDC(nullptr);
    HDC memDC  = CreateCompatibleDC(screen);

    void* bits = nullptr;
    HBITMAP dib = CreateDIB32(screen, W, H, &bits);
    HGDIOBJ old = SelectObject(memDC, dib);

    // Clear to black (RGB=0, alpha=0)
    GdiFlush();
    memset(bits, 0x00, W*H*4);

    // GDI text settings
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HGDIOBJ oldFont = SelectObject(memDC, hFont);
    SetBkMode(memDC, TRANSPARENT);
    SetTextColor(memDC, RGB(255,255,255)); // draw in white

    // Layout
    TEXTMETRICW tm{}; GetTextMetricsW(memDC,&tm);
    int lineH = tm.tmHeight + 6;
    int y = 6, left = 8;

    // Status
    RECT r{left,y, W-8, y+lineH};
    DrawTextW(memDC, g_status.c_str(), (int)g_status.size(), &r, DT_LEFT|DT_SINGLELINE|DT_NOPREFIX);
    y += lineH + 2;

    // Latest line
    RECT r2{left,y, W-8, y+lineH};
    DrawTextW(memDC, g_line.c_str(), (int)g_line.size(), &r2, DT_LEFT|DT_SINGLELINE|DT_NOPREFIX);
    y += lineH + 6;

    // Top 3 nemeses
    struct Row { std::wstring id; Counters cnt; };
    std::vector<Row> entries; entries.reserve(g_counts.size());
    for (auto it = g_counts.begin(); it != g_counts.end(); ++it) {
        if (it->first == L"0" && g_cfg.skip_environment) continue;
        if (it->second.tot <= 0) continue;
        entries.push_back(Row{it->first, it->second});
    }
    std::sort(entries.begin(), entries.end(),
        [&](const Row& A, const Row& B){
            if (A.cnt.tot != B.cnt.tot) return A.cnt.tot > B.cnt.tot;
            if (A.cnt.hs  != B.cnt.hs)  return A.cnt.hs  > B.cnt.hs;
            return GetDisplayNameFor(A.id) < GetDisplayNameFor(B.id);
        });

    int shown = 0;
    for (size_t i = 0; i < entries.size() && shown < 3; ++i) {
        const Row& e = entries[i];
        std::wstring name = GetDisplayNameFor(e.id);
        std::wstring line = to_wstring_compat(shown+1) + L") " + name +
                            L"  " + to_wstring_compat(e.cnt.hs) + L"/" + to_wstring_compat(e.cnt.tot);
        RECT r3{left,y, W-8, y+lineH};
        DrawTextW(memDC, line.c_str(), (int)line.size(), &r3, DT_LEFT|DT_SINGLELINE|DT_NOPREFIX);
        y += lineH; ++shown;
    }

    // Convert white->alpha; set RGB to configured color (premultiplied)
    unsigned char* p = (unsigned char*)bits;
    const unsigned cR = (unsigned)g_cfg.text_r;
    const unsigned cG = (unsigned)g_cfg.text_g;
    const unsigned cB = (unsigned)g_cfg.text_b;

    for (int i=0;i<W*H;++i){
        unsigned char B = p[0], G = p[1], R = p[2];
        unsigned char A = (R>G?R:G); if (B>A) A = B; // A = max(R,G,B)
        p[2] = (unsigned char)((cR * A + 127) / 255); // R premultiplied
        p[1] = (unsigned char)((cG * A + 127) / 255); // G
        p[0] = (unsigned char)((cB * A + 127) / 255); // B
        p[3] = A;                                     // A
        p += 4;
    }

    // Push to window
    POINT ptPos{0,0};
    SIZE  sz{W,H};
    POINT ptSrc{0,0};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};

    LONG ex = GetWindowLongW(g_hwnd, GWL_EXSTYLE);
    if(!(ex & WS_EX_LAYERED)) SetWindowLongW(g_hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);

    RECT wr; GetWindowRect(g_hwnd, &wr);
    POINT wndPos{wr.left, wr.top};

    UpdateLayeredWindow(g_hwnd, screen, &wndPos, &sz, memDC, &ptSrc, 0, &bf, ULW_ALPHA);

    // Cleanup
    SelectObject(memDC, oldFont);
    SelectObject(memDC, old);
    DeleteObject(dib);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screen);
}

// ========================== Legacy (uniform alpha) painting ===================
static void PaintLegacyOpaque(HDC hdc, RECT rc){
    // Fill white background (legacy path)
    HBRUSH hFill = CreateSolidBrush(RGB(255,255,255));
    FillRect(hdc, &rc, hFill);
    DeleteObject(hFill);

    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT hOld  = (HFONT)SelectObject(hdc, hFont);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, g_textColor); // use configured color

    TEXTMETRICW tm{}; GetTextMetricsW(hdc,&tm);
    int lineH = tm.tmHeight + 6;
    int y = rc.top + 6, left = rc.left + 8;

    RECT r{left,y, rc.right-8, y+lineH};
    DrawTextW(hdc, g_status.c_str(), (int)g_status.size(), &r, DT_LEFT|DT_SINGLELINE|DT_NOPREFIX);
    y += lineH + 2;

    RECT r2{left,y, rc.right-8, y+lineH};
    DrawTextW(hdc, g_line.c_str(), (int)g_line.size(), &r2, DT_LEFT|DT_SINGLELINE|DT_NOPREFIX);
    y += lineH + 6;

    struct Row { std::wstring id; Counters cnt; };
    std::vector<Row> entries; entries.reserve(g_counts.size());
    for (auto it = g_counts.begin(); it != g_counts.end(); ++it) {
        if (it->first == L"0" && g_cfg.skip_environment) continue;
        if (it->second.tot <= 0) continue;
        entries.push_back(Row{it->first, it->second});
    }
    std::sort(entries.begin(), entries.end(),
        [&](const Row& A, const Row& B){
            if (A.cnt.tot != B.cnt.tot) return A.cnt.tot > B.cnt.tot;
            if (A.cnt.hs  != B.cnt.hs)  return A.cnt.hs  > B.cnt.hs;
            return GetDisplayNameFor(A.id) < GetDisplayNameFor(B.id);
        });

    int shown = 0;
    for (size_t i = 0; i < entries.size() && shown < 3; ++i) {
        const Row& e = entries[i];
        std::wstring name = GetDisplayNameFor(e.id);
        std::wstring line = to_wstring_compat(shown+1) + L") " + name +
                            L"  " + to_wstring_compat(e.cnt.hs) + L"/" + to_wstring_compat(e.cnt.tot);
        RECT r3{left,y, rc.right-8, y+lineH};
        DrawTextW(hdc, line.c_str(), (int)line.size(), &r3, DT_LEFT|DT_SINGLELINE|DT_NOPREFIX);
        y += lineH; ++shown;
    }
    SelectObject(hdc, hOld);
}

// ========================== Window/config helpers =================
static void SetTopMostIfNeeded(){
    if (g_cfg.always_on_top && g_hwnd) {
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0,0,0,0, SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    }
}

static void ApplyWindowConfig(HWND hwnd){
    SetWindowPos(hwnd, nullptr, g_cfg.window.x, g_cfg.window.y, g_cfg.window.w, g_cfg.window.h,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if(!(ex & WS_EX_LAYERED)) SetWindowLongW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);

    if (!g_cfg.transparent_bg){
        // Uniform alpha for legacy opaque window
        BYTE a = (BYTE) (g_cfg.window.alpha<0?0:(g_cfg.window.alpha>255?255:g_cfg.window.alpha));
        SetLayeredWindowAttributes(hwnd, 0, a, LWA_ALPHA);
    }
    SetTopMostIfNeeded();
}

static bool LoadConfigFromFile(){
    std::wstring cfgPath = GetExecutableDir() + L"\\config.json";
    std::string j; if(!ReadFileUtf8(cfgPath, j)) return false;

    std::string s;
    if(JsonFindString(j, "service_id", s))      g_cfg.service_id      = Utf8ToWide(s);
    if(JsonFindString(j, "character_name", s))  g_cfg.character_name  = Utf8ToWide(s);

    int v;
    if(JsonFindInt(j, "poll_ms", v))            g_cfg.poll_ms         = v;
    if(JsonFindInt(j, "world_id", v))           g_cfg.world_id        = v;

    bool bflag;
    if(JsonFindBool(j, "transparent_bg", bflag)) g_cfg.transparent_bg = bflag;
    if(JsonFindInt(j, "chroma_r", v))            g_cfg.chroma_r = Clamp255(v);
    if(JsonFindInt(j, "chroma_g", v))            g_cfg.chroma_g = Clamp255(v);
    if(JsonFindInt(j, "chroma_b", v))            g_cfg.chroma_b = Clamp255(v);
    g_chroma = RGB(g_cfg.chroma_r, g_cfg.chroma_g, g_cfg.chroma_b);

    // NEW: text color
    if(JsonFindInt(j, "text_r", v)) g_cfg.text_r = Clamp255(v);
    if(JsonFindInt(j, "text_g", v)) g_cfg.text_g = Clamp255(v);
    if(JsonFindInt(j, "text_b", v)) g_cfg.text_b = Clamp255(v);
    g_textColor = RGB(g_cfg.text_r, g_cfg.text_g, g_cfg.text_b);

    // window
    size_t p = j.find("\"window\"");
    if(p!=std::string::npos){
        size_t b = j.find('{', p), e=b; int depth=0;
        for(; e<j.size(); ++e){ if(j[e]=='{') ++depth; else if(j[e]=='}'){ --depth; if(depth==0){ ++e; break; } } }
        if(b!=std::string::npos && e!=std::string::npos){
            std::string win = j.substr(b, e-b);
            if(JsonFindInt(win, "x", v))        g_cfg.window.x     = v;
            if(JsonFindInt(win, "y", v))        g_cfg.window.y     = v;
            if(JsonFindInt(win, "w", v))        g_cfg.window.w     = v;
            if(JsonFindInt(win, "h", v))        g_cfg.window.h     = v;
            if(JsonFindInt(win, "alpha", v))    g_cfg.window.alpha = v;
        }
    }

    bool b;
    if(JsonFindBool(j, "lock_position", b))     g_cfg.lock_position    = b;
    if(JsonFindBool(j, "always_on_top", b))     g_cfg.always_on_top    = b;
    if(JsonFindBool(j, "skip_environment", b))  g_cfg.skip_environment = b;

    if(g_cfg.service_id.empty()) g_cfg.service_id = L"s:example";
    TIMER_MS = (g_cfg.poll_ms>100 ? (UINT)g_cfg.poll_ms : 1000);
    return true;
}

// ========================== Common apply (de-dupe + counters) =================
static void RememberSynth(const std::wstring& key){
    if (g_seenSynth.insert(key).second) {
        g_seenQueue.push_back(key);
        if (g_seenQueue.size() > g_seenMax) {
            g_seenSynth.erase(g_seenQueue.front());
            g_seenQueue.pop_front();
        }
    }
}
static bool SeenSynth(const std::wstring& key){
    return g_seenSynth.find(key) != g_seenSynth.end();
}

static bool ApplyDeathEvent(const DeathEvent& e){
    if (!e.eventId.empty()){
        if (g_seenEventIds.count(e.eventId)) return false;
    } else {
        std::wstring key = to_wstring_compat(e.ts) + L"|" + e.attackerId + (e.isHS?L"|1":L"|0");
        if (SeenSynth(key)) return false;
    }

    if (g_cfg.skip_environment && e.attackerId == L"0"){
        if (!e.eventId.empty()) g_seenEventIds.insert(e.eventId);
        else {
            std::wstring key = to_wstring_compat(e.ts) + L"|" + e.attackerId + (e.isHS?L"|1":L"|0");
            RememberSynth(key);
        }
        if (e.ts > g_lastDeathTs) g_lastDeathTs = e.ts;
        return false;
    }

    if(!e.attackerName.empty()) g_nameCache[e.attackerId] = e.attackerName;

    g_lastAttackerId   = e.attackerId;
    g_lastAttackerName = (g_nameCache.find(e.attackerId)!=g_nameCache.end())
                          ? g_nameCache[e.attackerId] : L"(resolving…)";
    Counters &c = g_counts[e.attackerId];
    c.tot += 1;
    if (e.isHS) c.hs += 1;

    if (!e.eventId.empty()) g_seenEventIds.insert(e.eventId);
    else {
        std::wstring key = to_wstring_compat(e.ts) + L"|" + e.attackerId + (e.isHS?L"|1":L"|0");
        RememberSynth(key);
    }

    if (e.ts > g_lastDeathTs) g_lastDeathTs = e.ts;
    return true;
}

// ========================== Core polling (Hybrid PEAK + Batch) ================
static void PollOnce(){
    if(!g_characterId.empty() || !g_cfg.character_name.empty()){
        if (g_characterId.empty()){
            std::string body = FetchUrlBody(BuildCharacterByNamePath());
            std::wstring cid;
            if(!body.empty() && ParseCharacterId(body, cid)){
                g_characterId = cid;
                g_status = L"Character ID = " + g_characterId;
            } else {
                g_status = L"Could not resolve character_id from name";
                return;
            }
        }
    } else {
        g_status = L"Set character_name in config.json";
        return;
    }

    std::string peekJ = FetchUrlBody(BuildLatestDeathJoinedDesc(g_characterId));
    LatestOne latest = ParseLatestJoinedOne(peekJ);
    if(!latest.ok){ g_status = L"No latest death found"; return; }

    const unsigned long long peekTs = latest.ts;
    bool appliedPeek = false;

    bool unseenPeek = false;
    if(!latest.eventId.empty()){
        unseenPeek = (g_seenEventIds.find(latest.eventId) == g_seenEventIds.end());
    } else {
        std::wstring key = to_wstring_compat(latest.ts) + L"|" + latest.attackerId + (latest.isHS?L"|1":L"|0");
        unseenPeek = !SeenSynth(key);
    }

    if (unseenPeek){
        DeathEvent e{};
        e.attackerId   = latest.attackerId;
        e.attackerName = latest.attackerName;
        e.isHS         = latest.isHS;
        e.eventId      = latest.eventId;
        e.ts           = latest.ts;

        bool applied = ApplyDeathEvent(e);
        if (applied){
            appliedPeek = true;
            Counters c = g_counts[g_lastAttackerId];
            g_line = L"Killed by " + g_lastAttackerName +
                     L"  -  " + to_wstring_compat(c.hs) + L"/" + to_wstring_compat(c.tot) +
                     L"  (+1)";
            g_status = L"Peek applied";
            if(g_hwnd) {
                if (g_cfg.transparent_bg) RepaintLayered(); else InvalidateRect(g_hwnd,nullptr,TRUE);
            }
        }
    }

    const int PAGE = 1000;
    int start = 0;
    int appliedBatch = 0;
    bool anyPageRows = false;

    while(true){
        std::wstring path = BuildDeathsSincePath(g_characterId, g_lastDeathTs, start, PAGE);
        std::string  body = FetchUrlBody(path);
        std::vector<DeathEvent> events = ParseDeathBatch(body);
        if (events.empty()) break;
        anyPageRows = true;

        for (size_t i=0;i<events.size();++i){
            const DeathEvent& e = events[i];
            if (e.ts > peekTs) break;
            if (ApplyDeathEvent(e)) ++appliedBatch;
        }

        if ((int)events.size() < PAGE) break;
        start += PAGE;
    }

    if (appliedBatch == 0 && !appliedPeek){
        if (anyPageRows && peekTs > g_lastDeathTs) g_lastDeathTs = peekTs;
        g_status = L"No new deaths";
    } else if (appliedBatch > 0 && appliedPeek){
        Counters c = g_counts[g_lastAttackerId];
        g_line = L"Killed by " + g_lastAttackerName +
                 L"  -  " + to_wstring_compat(c.hs) + L"/" + to_wstring_compat(c.tot) +
                 L"  (+1 peek, +" + to_wstring_compat(appliedBatch) + L" batch)";
        g_status = L"Updated (peek+batch)";
        if(g_hwnd){ if (g_cfg.transparent_bg) RepaintLayered(); else InvalidateRect(g_hwnd,nullptr,TRUE); }
    } else if (appliedBatch > 0){
        Counters c = g_counts[g_lastAttackerId];
        g_line = L"Killed by " + g_lastAttackerName +
                 L"  -  " + to_wstring_compat(c.hs) + L"/" + to_wstring_compat(c.tot) +
                 L"  (+" + to_wstring_compat(appliedBatch) + L")";
        g_status = L"Updated (batch)";
        if(g_hwnd){ if (g_cfg.transparent_bg) RepaintLayered(); else InvalidateRect(g_hwnd,nullptr,TRUE); }
    }
    ReassertTopMost();
}

// ========================== Window plumbing ===================
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam){
    switch(msg){
        case WM_CREATE:
            SetTimer(hwnd, TIMER_ID, TIMER_MS, nullptr);
            EnsureContextMenu();
            return 0;

        case WM_WINDOWPOSCHANGING:
            if (g_cfg.lock_position) {
                WINDOWPOS* wp = reinterpret_cast<WINDOWPOS*>(lParam);
                if (wp) {
                    wp->x  = g_cfg.window.x;
                    wp->y  = g_cfg.window.y;
                    wp->cx = g_cfg.window.w;
                    wp->cy = g_cfg.window.h;
                    if (g_cfg.always_on_top) wp->hwndInsertAfter = HWND_TOPMOST;
                }
            }
            return 0;

        case WM_ACTIVATE:
        case WM_ACTIVATEAPP:
        case WM_NCACTIVATE:
            ReassertTopMost();
            return 0;

        case WM_NCHITTEST:
            return HTCLIENT;

        case WM_CONTEXTMENU: {
            EnsureContextMenu();
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if ((SHORT)LOWORD(lParam) == -1 && (SHORT)HIWORD(lParam) == -1) {
                RECT rc; GetWindowRect(hwnd, &rc);
                pt.x = rc.left + 10; pt.y = rc.top + 10;
            }
            SetForegroundWindow(hwnd);
            TrackPopupMenu(g_ctxMenu, TPM_RIGHTBUTTON | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
            PostMessageW(hwnd, WM_NULL, 0, 0);
            return 0;
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == IDM_EXIT) { DestroyWindow(hwnd); return 0; }
            break;

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) { DestroyWindow(hwnd); return 0; }
            break;

        case WM_SYSCOMMAND:
            if ((wParam & 0xFFF0) == SC_CLOSE) { DestroyWindow(hwnd); return 0; }
            break;

        case WM_TIMER:
            if(wParam==TIMER_ID){
                PollOnce();
                ReassertTopMost();
            }
            return 0;

        case WM_ERASEBKGND:
            return 1; // we'll draw everything

        case WM_PAINT:{
            if (!g_cfg.transparent_bg){
                PAINTSTRUCT ps; HDC hdc=BeginPaint(hwnd,&ps);
                RECT rc; GetClientRect(hwnd,&rc);
                PaintLegacyOpaque(hdc, rc);
                EndPaint(hwnd,&ps);
            } else {
                PAINTSTRUCT ps; BeginPaint(hwnd,&ps); EndPaint(hwnd,&ps);
                RepaintLayered();
            }
        } return 0;

        case WM_SIZE:
            if (g_cfg.transparent_bg) RepaintLayered();
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, TIMER_ID);
            if (g_ctxMenu) { DestroyMenu(g_ctxMenu); g_ctxMenu = nullptr; }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// =============================== Entry =======================================
int APIENTRY WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow){
    if(!LoadConfigFromFile()) g_status = L"config.json not found; using defaults";
    else g_status = L"Loaded config.json";

    DWORD exStyle = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_APPWINDOW;
    DWORD style   = WS_POPUP;

    const wchar_t* cls = L"PS2DeathsOverlay";
    WNDCLASSW wc{};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
    wc.lpszClassName = cls;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    if(!RegisterClassW(&wc)){
        MessageBoxW(nullptr,L"RegisterClassW failed",L"Error",MB_ICONERROR);
        return 1;
    }

    g_hwnd = CreateWindowExW(exStyle, cls, L"", style,
                             g_cfg.window.x, g_cfg.window.y, g_cfg.window.w, g_cfg.window.h,
                             nullptr, nullptr, hInst, nullptr);
    if(!g_hwnd){
        MessageBoxW(nullptr,L"CreateWindowExW failed",L"Error",MB_ICONERROR);
        return 1;
    }

    ApplyWindowConfig(g_hwnd);
    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    // Pre-resolve character id to speed up first paint
    if (g_characterId.empty() && !g_cfg.character_name.empty()){
        std::string body = FetchUrlBody(BuildCharacterByNamePath());
        std::wstring cid;
        if(!body.empty() && ParseCharacterId(body, cid)) g_characterId = cid;
    }

    ReassertTopMost();
    SetForegroundWindow(g_hwnd);
    SetFocus(g_hwnd);

    if (g_cfg.transparent_bg) RepaintLayered();

    MSG msg;
    while(GetMessageW(&msg,nullptr,0,0)>0){ TranslateMessage(&msg); DispatchMessageW(&msg); }
    return (int)msg.wParam;
}

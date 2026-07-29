/* ModSorter - Client/Server Mod-Sortierer fuer Fabric (COBBLEVERSE, 1.21.1)
 *
 * Liest jede .jar im gewaehlten Mods-Ordner, extrahiert fabric.mod.json
 * (per Windows-eigenem tar.exe) und liest das "environment"-Feld:
 *   "client" -> Client-only,  "server" -> Server-only,  sonst -> Beide.
 *
 * Kopieren legt Unterordner client\ und server\ an:
 *   client\  <- Client-Mods + Beide-Mods
 *   server\  <- Server-Mods + Beide-Mods
 * Originale bleiben unangetastet.
 *
 * Modrinth-Abgleich (primaer): matcht jede .jar per SHA-1 gegen die Modrinth-API
 * und liest client_side/server_side. Fallback bleibt fabric.mod.json (lokal).
 *
 * Build (MinGW):
 *   gcc main.c -o ModSorter.exe -mwindows -O2 -lgdi32 -lole32 -lshell32 -ldwmapi -lwinhttp -lbcrypt
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <uxtheme.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* comctl32 v6 -> Voraussetzung fuer die dunklen Scrollbalken (SetWindowTheme) */
#ifdef _MSC_VER
#pragma comment(linker, "\"/manifestdependency:type='win32' "                  \
    "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "              \
    "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#define ID_BTN_CHOOSE  1001
#define ID_BTN_COPY    1002
#define ID_BTN_SCAN    1003
#define ID_EDIT_PATH   1004
#define ID_BTN_SERVER  1005
#define ID_BTN_PICK    1006
#define ID_LIST_CLIENT  1010
#define ID_LIST_SERVER  1011
#define ID_LIST_BOTH    1012
#define ID_LIST_UNKNOWN 1013
#define ID_HDR_CLIENT   1020
#define ID_HDR_SERVER   1021
#define ID_HDR_BOTH     1022
#define ID_HDR_UNKNOWN  1023
#define ID_LBL_PATH    1030
#define ID_LBL_STATUS  1031

/* --- Farbschema --- */
static const COLORREF CBG     = RGB( 22,  22,  26);   /* Fensterhintergrund */
static const COLORREF CCARD   = RGB( 31,  31,  36);   /* Spaltenkarten + Listen */
static const COLORREF CINPUT  = RGB( 40,  40,  47);   /* Buttons / Eingabefeld */
static const COLORREF CINPUTH = RGB( 52,  52,  61);   /* dieselben, Mauszeiger drueber */
static const COLORREF CTEXT   = RGB(232, 232, 237);
static const COLORREF CDIM    = RGB(203, 203, 212);   /* Listeneintraege */
static const COLORREF CMUTED  = RGB(142, 142, 154);
static const COLORREF CBORDER = RGB( 52,  52,  61);
static const COLORREF CACC    = RGB( 96, 165, 250);   /* Primaerbutton */
static const COLORREF CACCH   = RGB(126, 184, 255);
static const COLORREF CACCP   = RGB( 62, 134, 224);
/* je eine Farbe pro Kategorie - macht die vier Spalten unterscheidbar */
static const COLORREF CCLIENT = RGB( 96, 165, 250);   /* blau    */
static const COLORREF CSERVER = RGB( 74, 222, 128);   /* gruen   */
static const COLORREF CBOTH   = RGB(167, 139, 250);   /* violett */
static const COLORREF CUNKNW  = RGB(251, 191,  36);   /* bernstein */

static HBRUSH gbrBg, gbrCard, gbrInput;
static HFONT  gFont, gFontB, gFontSm;

/* --- DPI-Skalierung: alle Layoutmasse laufen durch S() --- */
static int gDpi = 96;
static int S(int v) { return MulDiv(v, gDpi, 96); }

/* Kartengeometrie + Zaehler, gezeichnet in WM_PAINT */
static RECT gCard[4];
static RECT gEditBox;                  /* gezeichneter Rahmen um das Pfadfeld */
static RECT gSearchBox;                /* dito im Auswahlfenster */
static int  gTextH = 16;               /* Zeilenhoehe der Schrift */
static int  gHdrH = 42;
static int  gCnt[4] = { 0, 0, 0, 0 };
static int  gBarY = 0;                 /* Oberkante der unteren Leiste */
static char   gFolder[MAX_PATH] = "";
static char   gStartPath[MAX_PATH] = "";   /* optionaler Ordner von der Kommandozeile */
static char   gServerOut[MAX_PATH] = "";   /* --server <ziel>: Batch-Modus */
static char   gMrpackUrl[900] = "";        /* --mrpack <url>: Online-Batch */
static int    gPackIsCf = 0;               /* --cfpack statt --mrpack */
static int    gAutoQuit = 0;
static HWND   gBtnChoose, gBtnCopy, gBtnScan, gBtnServer, gBtnPick, gEditPath;
static HWND   gListC, gListS, gListB, gListU;
static HWND   gLblStatus;
static WNDPROC gEditOrigProc, gBtnOrigProc, gListOrigProc;
static int     gScanned = 0;            /* wurde schon einmal gescannt? */

enum { ENV_CLIENT, ENV_SERVER, ENV_BOTH, ENV_UNKNOWN };

/* Welche Metadatendatei eine Mod mitbringt -> welcher Loader */
enum { META_NONE, META_FABRIC, META_TOML };
enum { LOADER_FABRIC, LOADER_NEOFORGE, LOADER_FORGE };
static int gLoader = LOADER_FABRIC;

/* Metadaten einer Mod holen; kind sagt, welches Format es war. */
static char *run_capture(char *cmd);          /* fwd */
static char *read_mod_meta(const char *folder, const char *file,
                           int *kind, int *isNeo)
{
    static const char *paths[3] = { "fabric.mod.json",
                                    "META-INF/neoforge.mods.toml",
                                    "META-INF/mods.toml" };
    for (int i = 0; i < 3; i++) {
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "tar.exe -xOf \"%s\\%s\" %s",
                 folder, file, paths[i]);
        char *r = run_capture(cmd);
        if (r && r[0]) {
            if (kind)  *kind = (i == 0) ? META_FABRIC : META_TOML;
            if (isNeo) *isNeo = (i == 1);
            return r;
        }
        if (r) free(r);
    }
    if (kind) *kind = META_NONE;
    return NULL;
}

static void scan_folder(HWND hwnd);   /* fwd */

/* ================== Fortschrittsanzeige ==================
 * Downloads laufen im UI-Thread. Damit das Fenster waehrend eines grossen
 * Downloads nicht einfriert, pumpt die Leseschleife die Nachrichten mit und
 * zeichnet einen Balken. gCancel bricht sauber ab, wenn das Fenster
 * geschlossen wird. */
static HWND   gMainWnd   = NULL;
static int    gBusy      = 0;
static int    gCancel    = 0;
static double gProgFrac  = -1.0;      /* -1 = unbestimmt, kein Balken */
static DWORD  gProgStart = 0;
static DWORD  gProgLastPaint = 0;
static char   gLabel[160] = "";       /* was gerade geladen wird */

static void pump(void)
{
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) {
            PostQuitMessage((int)m.wParam);
            gCancel = 1;
            return;
        }
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
}

/* Dauer als m:ss */
static void fmt_time(double sec, char *out, int outsz)
{
    if (sec < 0 || sec > 359999) { snprintf(out, outsz, "--:--"); return; }
    int s = (int)(sec + 0.5);
    snprintf(out, outsz, "%d:%02d", s / 60, s % 60);
}

/* Byte-Zahl lesbar machen */
static void fmt_size(double b, char *out, int outsz)
{
    if (b >= 1024.0 * 1024.0 * 1024.0) snprintf(out, outsz, "%.1f GB", b / (1024.0*1024.0*1024.0));
    else if (b >= 1024.0 * 1024.0)     snprintf(out, outsz, "%.1f MB", b / (1024.0*1024.0));
    else if (b >= 1024.0)              snprintf(out, outsz, "%.0f KB", b / 1024.0);
    else                               snprintf(out, outsz, "%.0f B", b);
}

static void prog_begin(const char *text)
{
    gBusy = 1;
    gCancel = 0;
    gProgFrac = -1.0;
    gProgStart = GetTickCount();
    gProgLastPaint = 0;
    if (gLblStatus && text)
        SetWindowTextA(gLblStatus, text);
    if (gMainWnd)
        InvalidateRect(gMainWnd, NULL, FALSE);
    pump();
}

/* frac < 0 -> nur Text, kein Balken. force=1 umgeht die Zeitdrossel. */
static void prog_set(const char *text, double frac, int force)
{
    DWORD now = GetTickCount();
    if (!force && now - gProgLastPaint < 100)
        return;                       /* hoechstens 10x pro Sekunde neu zeichnen */
    gProgLastPaint = now;
    gProgFrac = frac;
    if (gLblStatus && text)
        SetWindowTextA(gLblStatus, text);
    if (gMainWnd)
        InvalidateRect(gMainWnd, NULL, FALSE);
    pump();
}

static void prog_end(const char *text)
{
    gBusy = 0;
    gProgFrac = -1.0;
    if (gLblStatus && text)
        SetWindowTextA(gLblStatus, text);
    if (gMainWnd)
        InvalidateRect(gMainWnd, NULL, FALSE);
    pump();
}

/* ---- GDI+ nur fuer das Laden der Pack-Icons.
 * Die offiziellen Header sind C++-only, daher die flache API selbst deklariert. */
typedef struct GpImage GpImage;
typedef struct {
    UINT32 GdiplusVersion;
    void  *DebugEventCallback;
    BOOL   SuppressBackgroundThread;
    BOOL   SuppressExternalCodecs;
} GdiplusStartupInput_;
int WINAPI GdiplusStartup(ULONG_PTR *token, const GdiplusStartupInput_ *in, void *out);
void WINAPI GdiplusShutdown(ULONG_PTR token);
int WINAPI GdipCreateBitmapFromStream(IStream *s, GpImage **img);
int WINAPI GdipCreateHBITMAPFromBitmap(GpImage *bmp, HBITMAP *hbm, DWORD bkgnd);
int WINAPI GdipDisposeImage(GpImage *img);

static ULONG_PTR gGdiplusToken = 0;

/* ================== Zeichenhelfer ================== */

/* Dunkle Scrollbalken fuer die Standard-Steuerelemente einschalten.
 * SetPreferredAppMode ist undokumentiert (uxtheme.dll, Ordinal 135) - deshalb
 * nur per GetProcAddress und mit stillem Rueckfall auf helle Scrollbalken. */
typedef enum { APPMODE_DEFAULT = 0, APPMODE_ALLOWDARK = 1,
               APPMODE_FORCEDARK = 2 } PreferredAppMode;
typedef PreferredAppMode (WINAPI *SetPreferredAppMode_t)(PreferredAppMode);
typedef void (WINAPI *FlushMenuThemes_t)(void);

static void enable_dark_controls(void)
{
    HMODULE ux = LoadLibraryA("uxtheme.dll");
    if (!ux)
        return;
    SetPreferredAppMode_t setMode =
        (SetPreferredAppMode_t)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    FlushMenuThemes_t flush =
        (FlushMenuThemes_t)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (setMode)
        setMode(APPMODE_FORCEDARK);
    if (flush)
        flush();
}

/* zwei Farben mischen; pct = Anteil von b in Prozent */
static COLORREF mix(COLORREF a, COLORREF b, int pct)
{
    return RGB((GetRValue(a) * (100 - pct) + GetRValue(b) * pct) / 100,
               (GetGValue(a) * (100 - pct) + GetGValue(b) * pct) / 100,
               (GetBValue(a) * (100 - pct) + GetBValue(b) * pct) / 100);
}

/* gefuellte, abgerundete Flaeche mit optionalem Rand */
static void round_box(HDC dc, RECT r, int rad, COLORREF fill, COLORREF border)
{
    HBRUSH br = CreateSolidBrush(fill);
    HPEN   pn = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, pn);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pn);
}

/* gefuellter Kreis (Kategoriepunkt im Kartenkopf) */
static void dot(HDC dc, int cx, int cy, int r, COLORREF c)
{
    HBRUSH br = CreateSolidBrush(c);
    HPEN   pn = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, pn);
    Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pn);
}

/* 1px-Linie */
static void hline(HDC dc, int x1, int x2, int y, COLORREF c)
{
    RECT r = { x1, y, x2, y + 1 };
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

/* -- Buttons: Hover-Zustand mitfuehren (in GWLP_USERDATA) -- */
static LRESULT CALLBACK BtnProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_MOUSEMOVE) {
        if (!GetWindowLongPtrA(h, GWLP_USERDATA)) {
            SetWindowLongPtrA(h, GWLP_USERDATA, 1);
            TRACKMOUSEEVENT t;
            t.cbSize = sizeof(t);
            t.dwFlags = TME_LEAVE;
            t.hwndTrack = h;
            t.dwHoverTime = 0;
            TrackMouseEvent(&t);
            InvalidateRect(h, NULL, FALSE);
        }
    } else if (m == WM_MOUSELEAVE) {
        SetWindowLongPtrA(h, GWLP_USERDATA, 0);
        InvalidateRect(h, NULL, FALSE);
    }
    return CallWindowProcA(gBtnOrigProc, h, m, w, l);
}

/* -- Leere Liste: dezenten Hinweis statt blanker Flaeche zeichnen -- */
static LRESULT CALLBACK ListProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    LRESULT r = CallWindowProcA(gListOrigProc, h, m, w, l);
    if (m == WM_PAINT && SendMessageA(h, LB_GETCOUNT, 0, 0) == 0) {
        HDC dc = GetDC(h);
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(dc, &rc, gbrCard);      /* sonst bleibt alter Text stehen */
        rc.top += S(16);
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, gFont);
        SetTextColor(dc, RGB(98, 98, 108));
        DrawTextA(dc, gScanned ? "none" : "not scanned yet", -1, &rc,
                  DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        ReleaseDC(h, dc);
    }
    return r;
}

/* -- Enter im Pfadfeld loest das Scannen aus (statt zu piepen) -- */
static LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if ((m == WM_KEYDOWN || m == WM_CHAR) && w == VK_RETURN) {
        if (m == WM_KEYDOWN)
            SendMessageA(GetParent(h), WM_COMMAND,
                         MAKEWPARAM(ID_BTN_SCAN, 0), 0);
        return 0;   /* beide schlucken -> kein System-Piep */
    }
    return CallWindowProcA(gEditOrigProc, h, m, w, l);
}

/* -- externen Prozess starten und Ausgabe einsammeln (ohne Konsolenfenster) --
 * wantErr=1: stderr mit einsammeln (java -version schreibt dorthin) */
static char *run_capture_ex(char *cmd, int wantErr)
{
    SECURITY_ATTRIBUTES sa;
    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0))
        return NULL;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    HANDLE hNul = CreateFileA("NUL", GENERIC_WRITE,
                              FILE_SHARE_WRITE | FILE_SHARE_READ,
                              &sa, OPEN_EXISTING, 0, NULL);

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError  = wantErr ? wr : hNul;
    si.hStdInput  = NULL;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(wr);
    if (hNul)
        CloseHandle(hNul);
    if (!ok) {
        CloseHandle(rd);
        return NULL;
    }

    DWORD cap = 4096, len = 0;
    char *buf = (char *)malloc(cap);
    if (buf) {
        for (;;) {
            if (len + 1024 > cap) {
                DWORD ncap = cap * 2;
                char *nb = (char *)realloc(buf, ncap);
                if (!nb)
                    break;
                buf = nb;
                cap = ncap;
            }
            DWORD got = 0;
            if (!ReadFile(rd, buf + len, cap - len - 1, &got, NULL) || got == 0)
                break;
            len += got;
        }
        buf[len] = 0;
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return buf;
}

static char *run_capture(char *cmd) { return run_capture_ex(cmd, 0); }

/* -- exakten quotierten Schluessel "key" in [b,e) suchen -- */
static int has_key(const char *b, const char *e, const char *key)
{
    size_t k = strlen(key);
    for (const char *p = b; p + k + 1 < e; p++)
        if (p[0] == '\"' && strncmp(p + 1, key, k) == 0 && p[1 + k] == '\"')
            return 1;
    return 0;
}

/* -- String-Wert zu "key" aus [b,e) lesen -> out -- */
static int json_str(const char *b, const char *e, const char *key,
                    char *out, int outsz)
{
    size_t k = strlen(key);
    for (const char *p = b; p + k + 1 < e; p++) {
        if (p[0] == '\"' && strncmp(p + 1, key, k) == 0 && p[1 + k] == '\"') {
            const char *q = p + 1 + k + 1;
            while (q < e && (*q == ' ' || *q == '\t' || *q == '\r' ||
                             *q == '\n' || *q == ':'))
                q++;
            if (q < e && *q == '\"') {
                q++;
                int i = 0;
                while (q < e && *q != '\"' && i < outsz - 1) {
                    if (*q == '\\' && q + 1 < e)
                        q++;
                    out[i++] = *q++;
                }
                out[i] = 0;
                return 1;
            }
            return 0;
        }
    }
    return 0;
}

/* -- Seite einer Mod aus fabric.mod.json bestimmen --
 * 1. explizites "environment": "client"/"server" gewinnt immer.
 * 2. sonst ("*" oder fehlend) anhand der Entrypoints:
 *      nur client               -> Client-only
 *      nur server               -> Server-only
 *      main / client+server     -> Server/Client (Beide)
 *      environment == "*"       -> Server/Client (Autor sagt: beide)
 *      gar keine Angabe         -> Unbekannt (z.B. Content-/Datapack-Mods)
 */
static int detect_env(const char *json)
{
    if (!json)
        return ENV_UNKNOWN;

    int envStar = 0;   /* environment-Feld vorhanden und == "*" */

    /* 1. explizites environment-Feld */
    const char *p = strstr(json, "\"environment\"");
    if (p) {
        p += 13;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':')
            p++;
        if (*p == '\"') {
            p++;
            char val[32];
            int i = 0;
            while (*p && *p != '\"' && i < 31)
                val[i++] = *p++;
            val[i] = 0;
            if (_stricmp(val, "client") == 0)
                return ENV_CLIENT;
            if (_stricmp(val, "server") == 0)
                return ENV_SERVER;
            envStar = 1;    /* "*" = Autor sagt: beide Seiten */
        }
    }

    /* 2. Entrypoints-Block auswerten */
    int hasMain = 0, hasClient = 0, hasServer = 0;
    const char *ep = strstr(json, "\"entrypoints\"");
    if (ep) {
        const char *b = strchr(ep, '{');
        if (b) {
            const char *e = b;
            int depth = 0;
            for (; *e; e++) {
                if (*e == '{') depth++;
                else if (*e == '}') { depth--; if (depth == 0) { e++; break; } }
            }
            hasMain   = has_key(b, e, "main");
            hasClient = has_key(b, e, "client");
            hasServer = has_key(b, e, "server");
        }
    }

    if (!hasMain) {
        if (hasClient && !hasServer) return ENV_CLIENT;
        if (hasServer && !hasClient) return ENV_SERVER;
    }
    if (hasMain || (hasClient && hasServer))
        return ENV_BOTH;
    if (envStar)
        return ENV_BOTH;          /* explizit "*" ohne widersprechende Entrypoints */

    return ENV_UNKNOWN;           /* keine Seiten-Info gefunden */
}

/* ================== Modrinth-Abgleich (online) ================== */

/* SHA-1 einer Datei als 40-stelligen Hex-String */
static int sha1_file(const char *path, char out_hex[41])
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hh = NULL;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, NULL, 0) != 0 ||
        BCryptCreateHash(alg, &hh, NULL, 0, NULL, 0, 0) != 0) {
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
        CloseHandle(f);
        return 0;
    }
    BYTE buf[65536];
    DWORD got;
    while (ReadFile(f, buf, sizeof(buf), &got, NULL) && got > 0)
        BCryptHashData(hh, buf, got, 0);
    BYTE digest[20];
    BCryptFinishHash(hh, digest, sizeof(digest), 0);
    BCryptDestroyHash(hh);
    BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    static const char *hx = "0123456789abcdef";
    for (int i = 0; i < 20; i++) {
        out_hex[i * 2]     = hx[digest[i] >> 4];
        out_hex[i * 2 + 1] = hx[digest[i] & 15];
    }
    out_hex[40] = 0;
    return 1;
}

/* CurseForge-Schluessel aus der Datei neben der exe.
 * Bewusst nicht im Quelltext - der ist oeffentlich, der Schluessel privat. */
static char gCfKey[96] = "";

static void load_cf_key(void)
{
    char p[MAX_PATH];
    if (!GetModuleFileNameA(NULL, p, MAX_PATH))
        return;
    char *sl = strrchr(p, '\\');
    if (!sl) return;
    strcpy(sl + 1, "curseforge.key");

    FILE *f = fopen(p, "rb");
    if (!f) return;
    size_t n = fread(gCfKey, 1, sizeof(gCfKey) - 1, f);
    fclose(f);
    gCfKey[n] = 0;
    while (n > 0 && (gCfKey[n-1] == '\n' || gCfKey[n-1] == '\r' ||
                     gCfKey[n-1] == ' ' || gCfKey[n-1] == '\t'))
        gCfKey[--n] = 0;
}

/* HTTPS-Anfrage; extra = zusaetzliche Kopfzeilen (oder NULL).
 * Gibt malloc'd Body zurueck (oder NULL). */
static char *https_request_hdr(const wchar_t *host, const wchar_t *verb,
                               const wchar_t *path, const char *body,
                               int bodyLen, DWORD *outLen,
                               const wchar_t *extra);

static char *https_request(const wchar_t *host, const wchar_t *verb,
                           const wchar_t *path, const char *body,
                           int bodyLen, DWORD *outLen)
{
    return https_request_hdr(host, verb, path, body, bodyLen, outLen, NULL);
}

static char *https_request_hdr(const wchar_t *host, const wchar_t *verb,
                               const wchar_t *path, const char *body,
                               int bodyLen, DWORD *outLen,
                               const wchar_t *extra)
{
    char *result = NULL;
    DWORD total = 0;
    HINTERNET s = WinHttpOpen(L"ModSorter/1.0 (Fabric mod sorter)",
                              WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s)
        return NULL;
    WinHttpSetTimeouts(s, 5000, 5000, 10000, 20000);
    HINTERNET c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (c) {
        HINTERNET r = WinHttpOpenRequest(c, verb, path, NULL,
                                         WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE);
        if (r) {
            wchar_t hdrBuf[400];
            hdrBuf[0] = 0;
            if (body)
                wcscat(hdrBuf, L"Content-Type: application/json\r\n");
            if (extra)
                wcsncat(hdrBuf, extra, 380 - wcslen(hdrBuf));
            const wchar_t *hdr = hdrBuf[0] ? hdrBuf : WINHTTP_NO_ADDITIONAL_HEADERS;
            DWORD hdrLen = hdrBuf[0] ? (DWORD)-1L : 0;
            BOOL ok = WinHttpSendRequest(r, hdr, hdrLen, (LPVOID)body,
                                         body ? bodyLen : 0,
                                         body ? bodyLen : 0, 0);
            if (ok && WinHttpReceiveResponse(r, NULL)) {
                DWORD cap = 65536;
                result = (char *)malloc(cap);
                while (result) {
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(r, &avail) || avail == 0)
                        break;
                    if (total + avail + 1 > cap) {
                        while (total + avail + 1 > cap) cap *= 2;
                        char *nb = (char *)realloc(result, cap);
                        if (!nb) break;
                        result = nb;
                    }
                    DWORD rd = 0;
                    if (!WinHttpReadData(r, result + total, avail, &rd) || rd == 0)
                        break;
                    total += rd;
                }
                if (result) result[total] = 0;
            }
            WinHttpCloseHandle(r);
        }
        WinHttpCloseHandle(c);
    }
    WinHttpCloseHandle(s);
    if (outLen) *outLen = total;
    if (result && total == 0) { free(result); result = NULL; }
    return result;
}

/* Modrinth client_side/server_side -> ENV_* */
static int cat_from_sides(const char *cs, const char *ss)
{
    int c = cs[0] && _stricmp(cs, "unsupported") != 0 && _stricmp(cs, "unknown") != 0;
    int s = ss[0] && _stricmp(ss, "unsupported") != 0 && _stricmp(ss, "unknown") != 0;
    if (c && !s) return ENV_CLIENT;
    if (s && !c) return ENV_SERVER;
    if (c && s)  return ENV_BOTH;
    return ENV_UNKNOWN;
}

/* Fuellt out_cat[i] mit ENV_* aus Modrinth, oder -1 wenn dort nicht gefunden.
 * Rueckgabe: Anzahl aufgeloester Mods (bzw. -1 wenn Modrinth nicht erreichbar). */
static int modrinth_lookup(char (*hashes)[41], int n, int *out_cat)
{
    for (int i = 0; i < n; i++) out_cat[i] = -1;
    if (n <= 0) return 0;

    /* --- 1. version_files: hash -> project_id --- */
    char *body = (char *)malloc((size_t)n * 45 + 64);
    int len = sprintf(body, "{\"hashes\":[");
    for (int i = 0; i < n; i++)
        len += sprintf(body + len, "%s\"%s\"", i ? "," : "", hashes[i]);
    len += sprintf(body + len, "],\"algorithm\":\"sha1\"}");

    DWORD rlen = 0;
    char *resp = https_request(L"api.modrinth.com", L"POST",
                               L"/v2/version_files", body, len, &rlen);
    free(body);
    if (!resp)
        return -1;   /* offline / Fehler -> Fallback */

    char (*vfHash)[41] = malloc(sizeof(*vfHash) * n);
    char (*vfPid)[16]  = malloc(sizeof(*vfPid) * n);
    int vfN = 0;
    const char *end = resp + rlen;
    const char *p = strchr(resp, '{');
    if (p) {
        p++;
        while (p < end && vfN < n) {
            while (p < end && *p != '\"' && *p != '}') p++;
            if (p >= end || *p == '}') break;
            p++;
            char key[64]; int ki = 0;
            while (p < end && *p != '\"') {
                if (*p == '\\' && p + 1 < end) p++;
                if (ki < 63) key[ki++] = *p;
                p++;
            }
            key[ki] = 0;
            if (p < end) p++;
            while (p < end && *p != ':') p++;
            if (p < end) p++;
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
            if (p < end && *p == '{') {
                const char *o = p; int depth = 0, instr = 0;
                for (; p < end; p++) {
                    char ch = *p;
                    if (instr) { if (ch == '\\') p++; else if (ch == '\"') instr = 0; }
                    else { if (ch == '\"') instr = 1; else if (ch == '{') depth++;
                           else if (ch == '}') { depth--; if (depth == 0) { p++; break; } } }
                }
                char pid[16];
                if (json_str(o, p, "project_id", pid, sizeof(pid))) {
                    strcpy(vfHash[vfN], key);
                    strcpy(vfPid[vfN], pid);
                    vfN++;
                }
            }
            while (p < end && *p != ',' && *p != '}') p++;
            if (p < end && *p == ',') p++;
        }
    }
    free(resp);

    if (vfN == 0) { free(vfHash); free(vfPid); return 0; }

    /* --- eindeutige project_ids sammeln --- */
    char (*pidUniq)[16] = malloc(sizeof(*pidUniq) * vfN);
    int uN = 0;
    for (int i = 0; i < vfN; i++) {
        int seen = 0;
        for (int j = 0; j < uN; j++)
            if (strcmp(pidUniq[j], vfPid[i]) == 0) { seen = 1; break; }
        if (!seen) strcpy(pidUniq[uN++], vfPid[i]);
    }

    /* --- 2. projects?ids=[...] -> client_side/server_side --- */
    size_t qcap = (size_t)uN * 20 + 64;
    char *q = (char *)malloc(qcap);
    int ql = sprintf(q, "/v2/projects?ids=%%5B");
    for (int i = 0; i < uN; i++)
        ql += sprintf(q + ql, "%s%%22%s%%22", i ? "%2C" : "", pidUniq[i]);
    ql += sprintf(q + ql, "%%5D");
    wchar_t wpath[8200];
    MultiByteToWideChar(CP_UTF8, 0, q, -1, wpath, 8200);
    free(q);

    DWORD rlen2 = 0;
    char *resp2 = https_request(L"api.modrinth.com", L"GET", wpath, NULL, 0, &rlen2);

    char (*pcId)[16] = malloc(sizeof(*pcId) * uN);
    int  *pcCat = (int *)malloc(sizeof(int) * uN);
    int pcN = 0;
    if (resp2) {
        const char *e2 = resp2 + rlen2;
        const char *pp = strchr(resp2, '[');
        if (pp) {
            pp++;
            while (pp < e2 && pcN < uN) {
                while (pp < e2 && *pp != '{' && *pp != ']') pp++;
                if (pp >= e2 || *pp == ']') break;
                const char *o = pp; int depth = 0, instr = 0;
                for (; pp < e2; pp++) {
                    char ch = *pp;
                    if (instr) { if (ch == '\\') pp++; else if (ch == '\"') instr = 0; }
                    else { if (ch == '\"') instr = 1; else if (ch == '{') depth++;
                           else if (ch == '}') { depth--; if (depth == 0) { pp++; break; } } }
                }
                char id[16], cs[24], ss[24];
                if (json_str(o, pp, "id", id, sizeof(id))) {
                    if (!json_str(o, pp, "client_side", cs, sizeof(cs))) cs[0] = 0;
                    if (!json_str(o, pp, "server_side", ss, sizeof(ss))) ss[0] = 0;
                    strcpy(pcId[pcN], id);
                    pcCat[pcN] = cat_from_sides(cs, ss);
                    pcN++;
                }
            }
        }
        free(resp2);
    }

    /* --- 3. je Datei: hash -> pid -> Kategorie --- */
    int resolved = 0;
    for (int i = 0; i < n; i++) {
        const char *pid = NULL;
        for (int j = 0; j < vfN; j++)
            if (strcmp(vfHash[j], hashes[i]) == 0) { pid = vfPid[j]; break; }
        if (!pid) continue;
        for (int j = 0; j < pcN; j++)
            if (strcmp(pcId[j], pid) == 0) { out_cat[i] = pcCat[j]; resolved++; break; }
    }

    free(vfHash); free(vfPid); free(pidUniq); free(pcId); free(pcCat);
    return resolved;
}

/* ================== Server-Pack erzeugen ================== */

/* Objekt-/Wert-Grenzen zu "key" in [b,e) finden -> [*vb,*ve) */
static int json_obj(const char *b, const char *e, const char *key,
                    const char **vb, const char **ve)
{
    size_t k = strlen(key);
    for (const char *p = b; p + k + 1 < e; p++) {
        if (p[0] == '\"' && strncmp(p + 1, key, k) == 0 && p[1 + k] == '\"') {
            const char *q = p + 1 + k + 1;
            while (q < e && (*q == ' ' || *q == '\t' || *q == '\r' ||
                             *q == '\n' || *q == ':'))
                q++;
            if (q >= e || *q != '{')
                return 0;
            const char *o = q;
            int depth = 0, instr = 0;
            for (; q < e; q++) {
                char ch = *q;
                if (instr) { if (ch == '\\') q++; else if (ch == '\"') instr = 0; }
                else { if (ch == '\"') instr = 1; else if (ch == '{') depth++;
                       else if (ch == '}') { depth--; if (depth == 0) { q++; break; } } }
            }
            *vb = o; *ve = q;
            return 1;
        }
    }
    return 0;
}

/* prueft "key": true in [b,e) */
static int json_true(const char *b, const char *e, const char *key)
{
    size_t k = strlen(key);
    for (const char *p = b; p + k + 1 < e; p++) {
        if (p[0] == '\"' && strncmp(p + 1, key, k) == 0 && p[1 + k] == '\"') {
            const char *q = p + 1 + k + 1;
            while (q < e && (*q == ' ' || *q == '\t' || *q == '\r' ||
                             *q == '\n' || *q == ':'))
                q++;
            return (q + 4 <= e && strncmp(q, "true", 4) == 0);
        }
    }
    return 0;
}

/* -- JSON-Wert ab p ueberspringen -- */
static const char *json_skip_value(const char *p, const char *e)
{
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (p >= e) return p;
    if (*p == '\"') {
        p++;
        while (p < e && *p != '\"') { if (*p == '\\') p++; p++; }
        if (p < e) p++;
        return p;
    }
    if (*p == '{' || *p == '[') {
        char open = *p, close = (open == '{') ? '}' : ']';
        int depth = 0, instr = 0;
        for (; p < e; p++) {
            char ch = *p;
            if (instr) { if (ch == '\\') p++; else if (ch == '\"') instr = 0; }
            else {
                if (ch == '\"') instr = 1;
                else if (ch == open) depth++;
                else if (ch == close) { depth--; if (depth == 0) { p++; break; } }
            }
        }
        return p;
    }
    while (p < e && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}

/* -- Wert eines Schluessels auf OBERSTER Ebene des Objekts finden.
 * Noetig, weil einfaches Suchen nach "id" sonst in verschachtelten Objekten
 * landet: CurseForge stellt z.B. "screenshots":[{"id":...}] voran. -- */
static const char *json_find_top(const char *b, const char *e, const char *key)
{
    const char *p = b;
    if (p >= e || *p != '{')
        return NULL;
    p++;
    size_t kl = strlen(key);
    while (p < e) {
        while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' ||
                         *p == '\n' || *p == ',')) p++;
        if (p >= e || *p == '}' || *p != '\"')
            return NULL;
        p++;
        const char *ks = p;
        while (p < e && *p != '\"') { if (*p == '\\') p++; p++; }
        size_t len = (size_t)(p - ks);
        if (p < e) p++;
        while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (p < e && *p == ':') p++;
        while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (len == kl && strncmp(ks, key, kl) == 0)
            return p;
        p = json_skip_value(p, e);
    }
    return NULL;
}

/* Zahl bzw. Zeichenkette eines Schluessels auf oberster Ebene */
static int json_int_top(const char *b, const char *e, const char *key)
{
    const char *v = json_find_top(b, e, key);
    return v ? atoi(v) : 0;
}

static int json_str_top(const char *b, const char *e, const char *key,
                        char *out, int outsz)
{
    const char *v = json_find_top(b, e, key);
    out[0] = 0;
    if (!v || v >= e || *v != '\"')
        return 0;
    v++;
    int i = 0;
    while (v < e && *v != '\"' && i < outsz - 1) {
        if (*v == '\\' && v + 1 < e) v++;
        out[i++] = *v++;
    }
    out[i] = 0;
    return 1;
}

/* -- Schluessel eines Objekts sammeln (b zeigt auf '{') -- */
static int json_keys(const char *b, const char *e, char (*out)[64], int max)
{
    const char *p = b;
    if (p >= e || *p != '{') return 0;
    p++;
    int n = 0;
    while (p < e && n < max) {
        while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' ||
                         *p == '\n' || *p == ',')) p++;
        if (p >= e || *p == '}' || *p != '\"') break;
        p++;
        int i = 0;
        while (p < e && *p != '\"') {
            if (*p == '\\') p++;
            if (i < 63) out[n][i++] = *p;
            p++;
        }
        out[n][i] = 0;
        if (p < e) p++;
        n++;
        while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (p < e && *p == ':') p++;
        p = json_skip_value(p, e);
    }
    return n;
}

/* -- String-Array zu "key" sammeln -- */
static int json_str_array(const char *b, const char *e, const char *key,
                          char (*out)[64], int max)
{
    size_t k = strlen(key);
    for (const char *p = b; p + k + 1 < e; p++) {
        if (p[0] == '\"' && strncmp(p + 1, key, k) == 0 && p[1 + k] == '\"') {
            const char *q = p + 1 + k + 1;
            while (q < e && (*q == ' ' || *q == '\t' || *q == '\r' ||
                             *q == '\n' || *q == ':')) q++;
            if (q >= e || *q != '[') return 0;
            q++;
            int n = 0;
            while (q < e && n < max) {
                while (q < e && (*q == ' ' || *q == '\t' || *q == '\r' ||
                                 *q == '\n' || *q == ',')) q++;
                if (q >= e || *q == ']') break;
                if (*q != '\"') break;
                q++;
                int i = 0;
                while (q < e && *q != '\"') {
                    if (*q == '\\') q++;
                    if (i < 63) out[n][i++] = *q;
                    q++;
                }
                out[n][i] = 0;
                if (q < e) q++;
                n++;
            }
            return n;
        }
    }
    return 0;
}

/* Metadaten einer Mod aus ihrer fabric.mod.json */
typedef struct {
    char file[MAX_PATH];
    char ids[6][64];   int nids;    /* id + provides */
    char deps[48][64]; int ndeps;   /* depends-Schluessel */
    int  server;                    /* 1 = gehoert ins Server-Pack */
    int  promoted;                  /* 1 = nur als Abhaengigkeit dazugekommen */
} ModInfo;

/* JSON-Escapes aufloesen. Noetig, weil manche Mods die Operatoren kodieren:
 * ">=1.20" ist in Wahrheit ">=1.20". */
static void json_unescape(const char *s, char *out, int outsz)
{
    int o = 0;
    for (const char *p = s; *p && o < outsz - 1; p++) {
        if (*p == '\\' && p[1] == 'u') {
            int v = 0, ok = 1;
            for (int i = 2; i <= 5; i++) {
                char c = p[i], d;
                if (c >= '0' && c <= '9') d = (char)(c - '0');
                else if (c >= 'a' && c <= 'f') d = (char)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') d = (char)(c - 'A' + 10);
                else { ok = 0; break; }
                v = v * 16 + d;
            }
            if (ok) {
                if (v > 0 && v < 128) out[o++] = (char)v;
                p += 5;
                continue;
            }
        }
        if (*p == '\\' && p[1]) { p++; out[o++] = *p; continue; }
        out[o++] = *p;
    }
    out[o] = 0;
}

/* String-Wert zu "key" roh holen und Escapes aufloesen.
 * Bei einem Array wird der erste Eintrag genommen. */
static int json_str_unesc(const char *b, const char *e, const char *key,
                          char *out, int outsz)
{
    size_t k = strlen(key);
    for (const char *p = b; p + k + 1 < e; p++) {
        if (p[0] == '\"' && strncmp(p + 1, key, k) == 0 && p[1 + k] == '\"') {
            const char *q = p + 1 + k + 1;
            while (q < e && (*q == ' ' || *q == '\t' || *q == '\r' ||
                             *q == '\n' || *q == ':')) q++;
            if (q < e && *q == '[') {
                q++;
                while (q < e && *q != '\"' && *q != ']') q++;
            }
            if (q >= e || *q != '\"') return 0;
            q++;
            char raw[192];
            int i = 0;
            while (q < e && *q != '\"' && i < (int)sizeof(raw) - 1)
                raw[i++] = *q++;
            raw[i] = 0;
            json_unescape(raw, out, outsz);
            return 1;
        }
    }
    return 0;
}

/* Legt der Ausdruck genau eine Version fest ("1.20.1")? Bereiche wie
 * ">=1.20" oder "~1.20" tun das nicht. */
static int is_exact_version(const char *s)
{
    int digits = 0, dots = 0;
    for (const char *p = s; *p; p++) {
        if (*p >= '0' && *p <= '9') digits++;
        else if (*p == '.') dots++;
        else if (*p == ' ') continue;
        else return 0;
    }
    return digits > 0 && dots == 2;
}

/* "1.20.1" -> "1.20" */
static void family_of(const char *v, char *out, int outsz)
{
    int o = 0, dots = 0;
    for (const char *p = v; *p && o < outsz - 1; p++) {
        if (*p == '.' && ++dots == 2) break;
        out[o++] = *p;
    }
    out[o] = 0;
}

/* erste x.y[.z]-Zahl aus einem Versionsausdruck ziehen */
static void first_version(const char *q, const char *lim, char *out, int outsz)
{
    int vi = 0, started = 0;
    for (; *q && q < lim && vi < outsz - 1; q++) {
        if (*q >= '0' && *q <= '9') { started = 1; out[vi++] = *q; }
        else if (*q == '.' && started) out[vi++] = '.';
        else if (started) break;
    }
    out[vi] = 0;
    while (vi > 0 && out[vi - 1] == '.') out[--vi] = 0;
}

/* ---- mods.toml (NeoForge/Forge) ---- */

/* eine Zeile  key = "wert"  zerlegen */
static int toml_kv(const char *line, char *key, int ksz, char *val, int vsz)
{
    const char *eq = strchr(line, '=');
    if (!eq)
        return 0;
    int k = 0;
    for (const char *p = line; p < eq && k < ksz - 1; p++)
        if (*p != ' ' && *p != '\t')
            key[k++] = *p;
    key[k] = 0;
    const char *v = eq + 1;
    while (*v == ' ' || *v == '\t')
        v++;
    if (*v != '\"' && *v != '\'')
        return 0;
    char q = *v++;
    int i = 0;
    while (*v && *v != q && i < vsz - 1)
        val[i++] = *v++;
    val[i] = 0;
    return key[0] != 0;
}

/* Version aus einem Maven-Bereich ziehen.
 * "[1.21.1]" und "1.21.1" legen fest -> Rueckgabe 1.
 * "[1.21,1.22)" ist ein Bereich -> untere Grenze, Rueckgabe 0. */
static int maven_range_version(const char *r, char *out, int outsz)
{
    while (*r == ' ')
        r++;
    int bracketed = (*r == '[' || *r == '(');
    const char *body = bracketed ? r + 1 : r;
    first_version(body, body + strlen(body), out, outsz);
    if (!out[0])
        return 0;
    if (!bracketed)
        return 1;
    if (!strchr(body, ',') && strchr(r, ']'))
        return 1;
    return 0;
}

/* einen fertig gelesenen Dependency-Block verwerten */
static void flush_dep(ModInfo *m, char *id, char *type, char *range,
                      char *mcRange, int mcSz)
{
    if (id[0]) {
        int required = (!type[0] || _stricmp(type, "required") == 0);
        if (_stricmp(id, "minecraft") == 0) {
            if (range[0] && !mcRange[0]) {
                strncpy(mcRange, range, mcSz - 1);
                mcRange[mcSz - 1] = 0;
            }
        } else if (required && _stricmp(id, "neoforge") != 0 &&
                   _stricmp(id, "forge") != 0 && _stricmp(id, "java") != 0) {
            if (m->ndeps < 48) {
                strncpy(m->deps[m->ndeps], id, 63);
                m->deps[m->ndeps][63] = 0;
                m->ndeps++;
            }
        }
    }
    id[0] = 0; type[0] = 0; range[0] = 0;
}

/* mods.toml auswerten: eigene IDs, Abhaengigkeiten, Minecraft-Bereich */
static void parse_toml(const char *t, ModInfo *m, char *mcRange, int mcSz)
{
    enum { SEC_OTHER, SEC_MODS, SEC_DEPS } sec = SEC_OTHER;
    char depId[64] = "", depType[24] = "", depRange[80] = "";
    mcRange[0] = 0;

    const char *p = t;
    while (*p) {
        const char *eol = strchr(p, '\n');
        if (!eol)
            eol = p + strlen(p);
        char line[512];
        int n = (int)(eol - p);
        if (n > 511) n = 511;
        memcpy(line, p, n);
        line[n] = 0;

        char *s = line;
        while (*s == ' ' || *s == '\t' || *s == '\r') s++;
        int len = (int)strlen(s);
        while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' || s[len-1] == '\r'))
            s[--len] = 0;

        if (s[0] == '[') {
            flush_dep(m, depId, depType, depRange, mcRange, mcSz);
            if (strncmp(s, "[[mods]]", 8) == 0)                  sec = SEC_MODS;
            else if (strncmp(s, "[[dependencies.", 15) == 0)     sec = SEC_DEPS;
            else                                                 sec = SEC_OTHER;
        } else if (s[0] && s[0] != '#') {
            char key[40], val[160];
            if (toml_kv(s, key, sizeof(key), val, sizeof(val))) {
                if (strcmp(key, "modId") == 0) {
                    if (sec == SEC_MODS) {
                        if (m->nids < 6) {
                            strncpy(m->ids[m->nids], val, 63);
                            m->ids[m->nids][63] = 0;
                            m->nids++;
                        }
                    } else if (sec == SEC_DEPS) {
                        flush_dep(m, depId, depType, depRange, mcRange, mcSz);
                        strncpy(depId, val, 63); depId[63] = 0;
                    }
                } else if (sec == SEC_DEPS && strcmp(key, "type") == 0) {
                    strncpy(depType, val, 23); depType[23] = 0;
                } else if (sec == SEC_DEPS && strcmp(key, "versionRange") == 0) {
                    strncpy(depRange, val, 79); depRange[79] = 0;
                }
            }
        }
        p = (*eol) ? eol + 1 : eol;
    }
    flush_dep(m, depId, depType, depRange, mcRange, mcSz);
}

/* Aus den gesammelten Stimmen die Minecraft-Version bestimmen.
 * WICHTIG: nicht einfach die haeufigste nehmen. ">=1.20" (haeufig, unspezifisch)
 * wird von 1.20.1 erfuellt - "1.20.1" (exakt) aber NICHT von 1.20. Exakte
 * Festlegungen wiegen deshalb schwerer als Bereichsangaben. */
static void pick_mc(char (*ver)[24], int *cnt, int *exact, int nv,
                    char *out, int outsz)
{
    out[0] = 0;
    if (nv <= 0)
        return;

    /* 1. haeufigste Versionsfamilie (major.minor) ermitteln */
    char fam[32][12];
    int  fcnt[32], nf = 0;
    for (int i = 0; i < nv; i++) {
        char f[12];
        family_of(ver[i], f, sizeof(f));
        int found = 0;
        for (int k = 0; k < nf; k++)
            if (strcmp(fam[k], f) == 0) { fcnt[k] += cnt[i]; found = 1; break; }
        if (!found && nf < 32) { strcpy(fam[nf], f); fcnt[nf] = cnt[i]; nf++; }
    }
    int bf = 0;
    for (int k = 1; k < nf; k++)
        if (fcnt[k] > fcnt[bf]) bf = k;

    /* 2. innerhalb der Familie: haeufigste EXAKTE Festlegung */
    int best = -1;
    for (int i = 0; i < nv; i++) {
        char f[12];
        family_of(ver[i], f, sizeof(f));
        if (strcmp(f, fam[bf]) != 0 || !exact[i])
            continue;
        if (best < 0 || exact[i] > exact[best])
            best = i;
    }

    /* 3. sonst haeufigste vollstaendige x.y.z in der Familie */
    if (best < 0) {
        for (int i = 0; i < nv; i++) {
            char f[12];
            family_of(ver[i], f, sizeof(f));
            if (strcmp(f, fam[bf]) != 0)
                continue;
            int dots = 0;
            for (const char *p = ver[i]; *p; p++)
                if (*p == '.') dots++;
            if (dots != 2)
                continue;
            if (best < 0 || cnt[i] > cnt[best])
                best = i;
        }
    }

    const char *pick = (best >= 0) ? ver[best] : fam[bf];
    strncpy(out, pick, outsz - 1);
    out[outsz - 1] = 0;
}

/* Alle Mods einlesen: IDs, Abhaengigkeiten, MC-Version, Java-Anforderung.
 * lists/isServer beschreiben die vier Spalten. */
static ModInfo *collect_mods(HWND hwnd, HWND lists[4], const int isServer[4],
                             int *nOut, char *mcOut, int mcSz, int *javaOut)
{
    int total = 0;
    for (int l = 0; l < 4; l++)
        total += (int)SendMessageA(lists[l], LB_GETCOUNT, 0, 0);
    if (total == 0) { *nOut = 0; return NULL; }

    ModInfo *mi = (ModInfo *)calloc(total, sizeof(ModInfo));
    char (*ver)[24] = malloc(sizeof(*ver) * 32);
    int  *vcnt   = malloc(sizeof(int) * 32);
    int  *vexact = malloc(sizeof(int) * 32);
    int nv = 0, maxJava = 0, idx = 0;
    int nFabric = 0, nNeo = 0, nForge = 0;

    for (int l = 0; l < 4; l++) {
        int n = (int)SendMessageA(lists[l], LB_GETCOUNT, 0, 0);
        for (int i = 0; i < n; i++) {
            char name[MAX_PATH];
            SendMessageA(lists[l], LB_GETTEXT, i, (LPARAM)name);
            ModInfo *m = &mi[idx++];
            strncpy(m->file, name, MAX_PATH - 1);
            m->server = isServer[l];

            if ((idx % 10) == 0) {
                char st[128];
                snprintf(st, sizeof(st), "Reading mod metadata ... (%d/%d)", idx, total);
                SetWindowTextA(gLblStatus, st);
                UpdateWindow(hwnd);
            }

            int kind = META_NONE, isNeo = 0;
            char *js = read_mod_meta(gFolder, name, &kind, &isNeo);
            if (!js) continue;
            const char *e = js + strlen(js);

            /* ---- NeoForge / Forge: mods.toml ---- */
            if (kind == META_TOML) {
                if (isNeo) nNeo++; else nForge++;
                char mcRange[80];
                parse_toml(js, m, mcRange, sizeof(mcRange));
                if (mcRange[0]) {
                    char mv[24];
                    int ex = maven_range_version(mcRange, mv, sizeof(mv));
                    if (strlen(mv) >= 3) {
                        int found = 0;
                        for (int z = 0; z < nv; z++)
                            if (strcmp(ver[z], mv) == 0) {
                                vcnt[z]++; vexact[z] += ex; found = 1; break;
                            }
                        if (!found && nv < 32) {
                            strcpy(ver[nv], mv); vcnt[nv] = 1; vexact[nv] = ex; nv++;
                        }
                    }
                }
                free(js);
                continue;
            }
            nFabric++;

            char v[64];
            if (json_str(js, e, "id", v, sizeof(v)) && v[0]) {
                strncpy(m->ids[m->nids], v, 63);
                m->ids[m->nids][63] = 0;
                m->nids++;
            }
            char prov[5][64];
            int np = json_str_array(js, e, "provides", prov, 5);
            for (int k = 0; k < np && m->nids < 6; k++) {
                strncpy(m->ids[m->nids], prov[k], 63);
                m->ids[m->nids][63] = 0;
                m->nids++;
            }

            const char *db, *de;
            if (json_obj(js, e, "depends", &db, &de)) {
                m->ndeps = json_keys(db, de, m->deps, 48);

                /* Minecraft-Anforderung: Rohwert holen, Escapes aufloesen,
                 * Version ziehen und getrennt zaehlen, ob exakt festgelegt */
                char raw[128], mv[24];
                if (json_str_unesc(db, de, "minecraft", raw, sizeof(raw))) {
                    first_version(raw, raw + strlen(raw), mv, sizeof(mv));
                    if (strlen(mv) >= 3) {
                        int ex = is_exact_version(raw);
                        int found = 0;
                        for (int z = 0; z < nv; z++)
                            if (strcmp(ver[z], mv) == 0) {
                                vcnt[z]++;
                                vexact[z] += ex;
                                found = 1;
                                break;
                            }
                        if (!found && nv < 32) {
                            strcpy(ver[nv], mv);
                            vcnt[nv] = 1;
                            vexact[nv] = ex;
                            nv++;
                        }
                    }
                }

                /* Java-Anforderung */
                if (json_str_unesc(db, de, "java", raw, sizeof(raw))) {
                    char jv[24];
                    first_version(raw, raw + strlen(raw), jv, sizeof(jv));
                    int jn = atoi(jv);
                    if (jn > 0 && jn < 100 && jn > maxJava)
                        maxJava = jn;
                }
            }
            free(js);
        }
    }

    pick_mc(ver, vcnt, vexact, nv, mcOut, mcSz);
    free(ver); free(vcnt); free(vexact);

    /* Loader des Packs = haeufigstes Metadatenformat */
    if (nNeo >= nFabric && nNeo >= nForge)        gLoader = LOADER_NEOFORGE;
    else if (nForge >= nFabric && nForge >= nNeo) gLoader = LOADER_FORGE;
    else                                          gLoader = LOADER_FABRIC;

    if (javaOut) *javaOut = maxJava;
    *nOut = total;
    return mi;
}

/* Abhaengigkeiten aufloesen: alles was ein Server-Mod braucht, muss mit.
 * Auch client-klassifizierte Bibliotheken (z.B. athena, fusion).
 * Rueckgabe: Anzahl nachtraeglich ergaenzter Mods. */
static int resolve_server_deps(ModInfo *mi, int n)
{
    static const char *builtin[] = { "minecraft", "java", "fabricloader",
                                     "fabric-loader", "mixinextras" };
    int added = 0, changed = 1;
    while (changed) {
        changed = 0;
        for (int i = 0; i < n; i++) {
            if (!mi[i].server) continue;
            for (int d = 0; d < mi[i].ndeps; d++) {
                const char *need = mi[i].deps[d];
                int skip = 0;
                for (int b = 0; b < 5; b++)
                    if (_stricmp(need, builtin[b]) == 0) { skip = 1; break; }
                if (skip) continue;
                for (int j = 0; j < n; j++) {
                    if (mi[j].server) continue;
                    for (int k = 0; k < mi[j].nids; k++) {
                        if (_stricmp(mi[j].ids[k], need) == 0) {
                            mi[j].server = 1;
                            mi[j].promoted = 1;
                            added++;
                            changed = 1;
                            break;
                        }
                    }
                    if (mi[j].server && mi[j].promoted) break;
                }
            }
        }
    }
    return added;
}

/* neueste stabile Loader- und Installer-Version von meta.fabricmc.net */
static int fabric_versions(const char *mc, char *loader, int ls,
                           char *installer, int is)
{
    char path[128];
    snprintf(path, sizeof(path), "/v2/versions/loader/%s", mc);
    wchar_t wp[128];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 128);

    DWORD len = 0;
    char *r = https_request(L"meta.fabricmc.net", L"GET", wp, NULL, 0, &len);
    if (!r) return 0;

    int ok = 0;
    const char *e = r + len, *p = r;
    while (p < e) {
        while (p < e && *p != '{') p++;
        if (p >= e) break;
        const char *o = p; int depth = 0, instr = 0;
        for (; p < e; p++) {
            char ch = *p;
            if (instr) { if (ch == '\\') p++; else if (ch == '\"') instr = 0; }
            else { if (ch == '\"') instr = 1; else if (ch == '{') depth++;
                   else if (ch == '}') { depth--; if (depth == 0) { p++; break; } } }
        }
        const char *lb, *le;
        if (json_obj(o, p, "loader", &lb, &le) && json_true(lb, le, "stable")) {
            if (json_str(lb, le, "version", loader, ls)) { ok = 1; break; }
        }
    }
    free(r);
    if (!ok) return 0;

    /* Installer */
    r = https_request(L"meta.fabricmc.net", L"GET", L"/v2/versions/installer",
                      NULL, 0, &len);
    if (!r) return 0;
    ok = 0;
    e = r + len; p = r;
    while (p < e) {
        while (p < e && *p != '{') p++;
        if (p >= e) break;
        const char *o = p; int depth = 0, instr = 0;
        for (; p < e; p++) {
            char ch = *p;
            if (instr) { if (ch == '\\') p++; else if (ch == '\"') instr = 0; }
            else { if (ch == '\"') instr = 1; else if (ch == '{') depth++;
                   else if (ch == '}') { depth--; if (depth == 0) { p++; break; } } }
        }
        if (json_true(o, p, "stable") && json_str(o, p, "version", installer, is)) {
            ok = 1; break;
        }
    }
    free(r);
    return ok;
}

/* Java-Version eines Interpreters ermitteln (0 = nicht nutzbar) */
static int java_version_of(const char *exe)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "\"%s\" -version", exe);
    char *o = run_capture_ex(cmd, 1);
    if (!o) return 0;
    int v = 0;
    const char *p = strstr(o, "version \"");
    if (p) {
        p += 9;
        v = atoi(p);
        if (v == 1) {            /* altes Schema 1.8.0 -> 8 */
            const char *d = strchr(p, '.');
            v = d ? atoi(d + 1) : 0;
        }
    }
    free(o);
    return v;
}

typedef struct { char exe[MAX_PATH]; int ver; } JavaCand;

static void java_add(JavaCand *c, int *n, int max, const char *exe)
{
    if (*n >= max) return;
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) return;
    int v = java_version_of(exe);
    if (v <= 0) return;
    for (int i = 0; i < *n; i++)          /* Duplikate vermeiden */
        if (_stricmp(c[i].exe, exe) == 0) return;
    strncpy(c[*n].exe, exe, MAX_PATH - 1);
    c[*n].exe[MAX_PATH - 1] = 0;
    c[*n].ver = v;
    (*n)++;
}

/* In einem Ordner voller JDK/JRE-Unterordner nach java.exe suchen */
static void java_scan_dir(const char *base, JavaCand *c, int *n, int max)
{
    char pat[MAX_PATH];
    snprintf(pat, sizeof(pat), "%s\\*", base);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.') continue;
        char exe[MAX_PATH];
        snprintf(exe, sizeof(exe), "%s\\%s\\bin\\java.exe", base, fd.cFileName);
        java_add(c, n, max, exe);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* Passendes Java waehlen. WICHTIG: nicht einfach das neueste nehmen -
 * Mods wie Cobblemon fordern eine EXAKTE Version ("java": "21"), eine
 * neuere JVM laesst die Mod-Aufloesung fehlschlagen. Daher:
 *   1. exakte Uebereinstimmung mit want
 *   2. sonst aeltestes Java >= want
 *   3. sonst neuestes ueberhaupt
 * Rueckgabe: gewaehlte Hauptversion, 0 wenn keins gefunden. */
static int find_best_java(int want, char *out, int outsz)
{
    JavaCand c[32];
    int n = 0;
    out[0] = 0;

    char buf[MAX_PATH];
    if (GetEnvironmentVariableA("APPDATA", buf, MAX_PATH)) {
        char d[MAX_PATH];
        snprintf(d, sizeof(d), "%s\\ModrinthApp\\meta\\java_versions", buf);
        java_scan_dir(d, c, &n, 32);
        snprintf(d, sizeof(d), "%s\\.minecraft\\runtime", buf);
        java_scan_dir(d, c, &n, 32);
    }
    if (GetEnvironmentVariableA("ProgramFiles", buf, MAX_PATH)) {
        const char *subs[] = { "Java", "Eclipse Adoptium", "Microsoft",
                               "Zulu", "Amazon Corretto" };
        for (int i = 0; i < 5; i++) {
            char d[MAX_PATH];
            snprintf(d, sizeof(d), "%s\\%s", buf, subs[i]);
            java_scan_dir(d, c, &n, 32);
        }
    }
    if (GetEnvironmentVariableA("JAVA_HOME", buf, MAX_PATH)) {
        char exe[MAX_PATH];
        snprintf(exe, sizeof(exe), "%s\\bin\\java.exe", buf);
        java_add(c, &n, 32, exe);
    }
    java_add(c, &n, 32, "java");           /* aus dem PATH */

    if (n == 0) return 0;

    int pick = -1;
    for (int i = 0; i < n; i++)                       /* 1. exakt */
        if (c[i].ver == want) { pick = i; break; }
    if (pick < 0)                                     /* 2. aeltestes >= want */
        for (int i = 0; i < n; i++)
            if (c[i].ver >= want && (pick < 0 || c[i].ver < c[pick].ver)) pick = i;
    if (pick < 0)                                     /* 3. neuestes */
        for (int i = 0; i < n; i++)
            if (pick < 0 || c[i].ver > c[pick].ver) pick = i;

    strncpy(out, c[pick].exe, outsz - 1);
    out[outsz - 1] = 0;
    return c[pick].ver;
}

/* NeoForge-Serie aus der MC-Version: 1.21.1 -> "21.1", 1.21 -> "21.0".
 * Fuer 1.20.1 nutzt NeoForge noch die Forge-Zaehlung 47.x. */
static void neoforge_series(const char *mc, char *out, int outsz)
{
    int a = 0, b = 0, c = 0;
    sscanf(mc, "%d.%d.%d", &a, &b, &c);
    if (a == 1 && b == 20 && c == 1) {
        strncpy(out, "47", outsz - 1);
        out[outsz - 1] = 0;
        return;
    }
    snprintf(out, outsz, "%d.%d", b, c);
}

/* Java-Version, die diese Minecraft-Version braucht */
static int java_for_mc(const char *mc)
{
    int a = 0, b = 0, c = 0;
    sscanf(mc, "%d.%d.%d", &a, &b, &c);
    if (b > 20 || (b == 20 && c >= 5)) return 21;
    if (b >= 18) return 17;
    if (b == 17) return 16;
    return 8;
}

/* neuestes stabiles NeoForge-Release der passenden Serie ermitteln */
static int neoforge_latest(const char *mc, char *out, int outsz)
{
    char series[16];
    neoforge_series(mc, series, sizeof(series));
    size_t sl = strlen(series);

    DWORD len = 0;
    char *r = https_request(L"maven.neoforged.net", L"GET",
                            L"/api/maven/versions/releases/net%2Fneoforged%2Fneoforge",
                            NULL, 0, &len);
    if (!r)
        return 0;

    char best[40] = "";
    int bestPatch = -1;
    const char *e = r + len;
    for (const char *p = r; p < e; p++) {
        if (*p != '\"')
            continue;
        p++;
        char v[48];
        int i = 0;
        while (p < e && *p != '\"' && i < 47)
            v[i++] = *p++;
        v[i] = 0;
        if (strncmp(v, series, sl) != 0 || v[sl] != '.')
            continue;
        if (strstr(v, "beta") || strstr(v, "alpha") || strstr(v, "rc"))
            continue;
        int patch = atoi(v + sl + 1);
        if (patch > bestPatch) {
            bestPatch = patch;
            strncpy(best, v, sizeof(best) - 1);
            best[sizeof(best) - 1] = 0;
        }
    }
    free(r);
    if (!best[0])
        return 0;
    strncpy(out, best, outsz - 1);
    out[outsz - 1] = 0;
    return 1;
}

/* Neueste Forge-Version fuer eine Minecraft-Version.
 * maven-metadata.xml listet Eintraege der Form <version>1.20.1-47.4.22</version>.
 * Rueckgabe in out: die vollstaendige Artefaktversion "1.20.1-47.4.22". */
static int forge_latest(const char *mc, char *out, int outsz)
{
    DWORD len = 0;
    char *r = https_request(L"maven.minecraftforge.net", L"GET",
                            L"/net/minecraftforge/forge/maven-metadata.xml",
                            NULL, 0, &len);
    if (!r)
        return 0;

    char prefix[32];
    snprintf(prefix, sizeof(prefix), "%s-", mc);
    size_t pl = strlen(prefix);

    char best[48] = "";
    int bA = -1, bB = -1, bC = -1;
    const char *e = r + len;
    for (const char *p = r; p + 9 < e; p++) {
        if (strncmp(p, "<version>", 9) != 0)
            continue;
        p += 9;
        char v[64];
        int i = 0;
        while (p < e && *p != '<' && i < 63)
            v[i++] = *p++;
        v[i] = 0;
        if (strncmp(v, prefix, pl) != 0)
            continue;
        int a = 0, b = 0, c = 0;
        sscanf(v + pl, "%d.%d.%d", &a, &b, &c);
        if (a > bA || (a == bA && b > bB) || (a == bA && b == bB && c > bC)) {
            bA = a; bB = b; bC = c;
            strncpy(best, v, sizeof(best) - 1);
            best[sizeof(best) - 1] = 0;
        }
    }
    free(r);
    if (!best[0])
        return 0;
    strncpy(out, best, outsz - 1);
    out[outsz - 1] = 0;
    return 1;
}

/* Prozess starten und auf das Ende warten (fuer den NeoForge-Installer) */
static int run_wait(const char *cmd, const char *dir, DWORD timeoutMs)
{
    char buf[2048];
    strncpy(buf, cmd, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessA(NULL, buf, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, dir, &si, &pi))
        return 0;
    DWORD w = WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD code = 1;
    if (w == WAIT_OBJECT_0)
        GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (w == WAIT_OBJECT_0) && (code == 0);
}

/* "https://host/pfad" in Host und Pfad zerlegen */
static int url_split(const char *url, wchar_t *whost, int hn,
                     wchar_t *wpath, int pn)
{
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) p += 8;
    else if (strncmp(p, "http://", 7) == 0) p += 7;
    else return 0;
    const char *slash = strchr(p, '/');
    if (!slash) return 0;
    char host[256];
    int n = (int)(slash - p);
    if (n <= 0 || n > 255) return 0;
    memcpy(host, p, n);
    host[n] = 0;
    MultiByteToWideChar(CP_UTF8, 0, host, -1, whost, hn);
    MultiByteToWideChar(CP_UTF8, 0, slash, -1, wpath, pn);
    return 1;
}

/* Datei per HTTPS herunterladen - stueckweise direkt auf die Platte.
 * NICHT ueber https_request: das puffert die ganze Datei im Speicher, was bei
 * grossen Modpacks (COBBLEVERSE: 227 MB) in einem laenger laufenden Prozess
 * an der Speicherfragmentierung scheitert und eine abgeschnittene Datei
 * hinterlaesst. */
static int download_file(const wchar_t *host, const wchar_t *path, const char *dest)
{
    HINTERNET s = WinHttpOpen(L"ModSorter/1.0 (Fabric mod sorter)",
                              WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s)
        return 0;
    WinHttpSetTimeouts(s, 10000, 10000, 30000, 60000);

    int ok = 0;
    HINTERNET c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (c) {
        HINTERNET r = WinHttpOpenRequest(c, L"GET", path, NULL,
                                         WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE);
        if (r) {
            if (WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(r, NULL)) {

                DWORD code = 0, cl = sizeof(code);
                WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE |
                                       WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &code, &cl,
                                    WINHTTP_NO_HEADER_INDEX);
                if (code >= 200 && code < 300) {
                    /* Groesse fuer den Fortschritt, falls der Server sie nennt */
                    double expect = 0;
                    wchar_t wlen[32];
                    DWORD wl = sizeof(wlen);
                    if (WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH,
                                            WINHTTP_HEADER_NAME_BY_INDEX, wlen,
                                            &wl, WINHTTP_NO_HEADER_INDEX))
                        expect = _wtof(wlen);

                    FILE *f = fopen(dest, "wb");
                    if (f) {
                        static char buf[65536];
                        double done = 0;
                        DWORD t0 = GetTickCount();
                        int failed = 0;
                        for (;;) {
                            DWORD got = 0;
                            if (!WinHttpReadData(r, buf, sizeof(buf), &got)) {
                                failed = 1;
                                break;
                            }
                            if (got == 0)
                                break;                 /* fertig */
                            if (fwrite(buf, 1, got, f) != got) {
                                failed = 1;
                                break;
                            }
                            done += got;

                            if (gBusy && gLabel[0]) {
                                double el = (GetTickCount() - t0) / 1000.0;
                                char s1[32], s2[32], eta[16], line[300];
                                fmt_size(done, s1, sizeof(s1));
                                if (expect > 0) {
                                    fmt_size(expect, s2, sizeof(s2));
                                    double rate = el > 0.3 ? done / el : 0;
                                    fmt_time(rate > 0 ? (expect - done) / rate : -1,
                                             eta, sizeof(eta));
                                    snprintf(line, sizeof(line),
                                             "%s  \x95  %s / %s  \x95  %.1f MB/s  \x95  %s left",
                                             gLabel, s1, s2,
                                             rate / (1024.0 * 1024.0), eta);
                                    prog_set(line, done / expect, 0);
                                } else {
                                    snprintf(line, sizeof(line), "%s  \x95  %s",
                                             gLabel, s1);
                                    prog_set(line, -1.0, 0);
                                }
                                if (gCancel) { failed = 1; break; }
                            }
                        }
                        fclose(f);
                        ok = !failed;
                        if (!ok)
                            DeleteFileA(dest);         /* keine halben Dateien */
                    }
                }
            }
            WinHttpCloseHandle(r);
        }
        WinHttpCloseHandle(c);
    }
    WinHttpCloseHandle(s);
    return ok;
}

/* Bild von einer URL laden und als HBITMAP zurueckgeben (NULL bei Fehler).
 * Transparenz wird auf die Kartenfarbe gelegt, damit es sauber aussieht. */
static HBITMAP load_icon(const char *url)
{
    if (!url || !url[0])
        return NULL;
    wchar_t whost[256], wpath[2048];
    if (!url_split(url, whost, 256, wpath, 2048))
        return NULL;

    DWORD len = 0;
    char *data = https_request(whost, L"GET", wpath, NULL, 0, &len);
    if (!data || len == 0) {
        if (data) free(data);
        return NULL;
    }

    IStream *st = SHCreateMemStream((const BYTE *)data, len);
    free(data);
    if (!st)
        return NULL;

    GpImage *img = NULL;
    HBITMAP hbm = NULL;
    if (GdipCreateBitmapFromStream(st, &img) == 0 && img) {
        /* 0xAARRGGBB - Kartenfarbe als Hintergrund fuer transparente Bereiche */
        GdipCreateHBITMAPFromBitmap(img, &hbm, 0xFF1F1F24);
        GdipDisposeImage(img);
    }
    st->lpVtbl->Release(st);
    return hbm;
}

/* Datei anhand einer vollstaendigen URL herunterladen */
static int download_url(const char *url, const char *dest)
{
    wchar_t whost[256], wpath[2048];
    if (!url_split(url, whost, 256, wpath, 2048))
        return 0;
    return download_file(whost, wpath, dest);
}

/* Text schreiben; nl_crlf=1 -> \r\n (bat), 0 -> \n (sh/Unix) */
static int write_text(const char *path, const char *text, int nl_crlf)
{
    FILE *f = fopen(path, "wb");     /* binaer: verhindert ungewollte CRLF */
    if (!f) return 0;
    for (const char *p = text; *p; p++) {
        if (*p == '\n' && nl_crlf) fputc('\r', f);
        fputc(*p, f);
    }
    fclose(f);
    return 1;
}

/* Ordner rekursiv kopieren; gibt Anzahl kopierter Dateien zurueck */
static int copy_tree(const char *src, const char *dst)
{
    CreateDirectoryA(dst, NULL);
    char pat[1024];
    snprintf(pat, sizeof(pat), "%s\\*", src);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    int n = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        char s[1024], d[1024];
        snprintf(s, sizeof(s), "%s\\%s", src, fd.cFileName);
        snprintf(d, sizeof(d), "%s\\%s", dst, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            n += copy_tree(s, d);
        else if (CopyFileA(s, d, FALSE))
            n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

/* -- Mods-Ordner scannen und in die drei Listen einsortieren -- */
static void scan_folder(HWND hwnd)
{
    SendMessageA(gListC, LB_RESETCONTENT, 0, 0);
    SendMessageA(gListS, LB_RESETCONTENT, 0, 0);
    SendMessageA(gListB, LB_RESETCONTENT, 0, 0);
    SendMessageA(gListU, LB_RESETCONTENT, 0, 0);
    if (gFolder[0] == 0)
        return;

    SetCursor(LoadCursor(NULL, IDC_WAIT));
    SetWindowTextA(gLblStatus, "Scanning mods ...");
    UpdateWindow(hwnd);

    /* --- 1. Alle .jar-Dateinamen einsammeln --- */
    char pat[MAX_PATH];
    snprintf(pat, sizeof(pat), "%s\\*.jar", gFolder);
    int cap = 64, total = 0;
    char (*names)[MAX_PATH] = malloc(sizeof(*names) * cap);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            if (total >= cap) {
                cap *= 2;
                names = realloc(names, sizeof(*names) * cap);
            }
            strncpy(names[total], fd.cFileName, MAX_PATH - 1);
            names[total][MAX_PATH - 1] = 0;
            total++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    /* --- 2. SHA-1 bilden und bei Modrinth nachschlagen --- */
    int alloc = total ? total : 1;
    char (*hashes)[41] = malloc(sizeof(*hashes) * alloc);
    int  *cat = malloc(sizeof(int) * alloc);
    for (int i = 0; i < total; i++) {
        char full[1024];
        snprintf(full, sizeof(full), "%s\\%s", gFolder, names[i]);
        if (!sha1_file(full, hashes[i]))
            hashes[i][0] = 0;
    }
    SetWindowTextA(gLblStatus, "Querying Modrinth ...");
    UpdateWindow(hwnd);
    int resolved = modrinth_lookup(hashes, total, cat);
    int online = (resolved >= 0);
    int fromModrinth = (resolved > 0) ? resolved : 0;

    /* --- 3. Klassifizieren: Modrinth zuerst, sonst lokale fabric.mod.json --- */
    int nc = 0, ns = 0, nb = 0, nu = 0, fromLocal = 0;
    for (int i = 0; i < total; i++) {
        int env = cat[i];
        if (env < 0) {
            char cmd[1024];
            snprintf(cmd, sizeof(cmd),
                     "tar.exe -xOf \"%s\\%s\" fabric.mod.json",
                     gFolder, names[i]);
            char *js = run_capture(cmd);
            env = detect_env(js);
            if (js)
                free(js);
            fromLocal++;
        }
        HWND lb = (env == ENV_CLIENT) ? gListC
                : (env == ENV_SERVER) ? gListS
                : (env == ENV_BOTH)   ? gListB
                                      : gListU;
        SendMessageA(lb, LB_ADDSTRING, 0, (LPARAM)names[i]);
        if (env == ENV_CLIENT)      nc++;
        else if (env == ENV_SERVER) ns++;
        else if (env == ENV_BOTH)   nb++;
        else                        nu++;
    }

    free(names);
    free(hashes);
    free(cat);

    char t[256];
    gScanned = 1;
    gCnt[0] = nc; gCnt[1] = ns; gCnt[2] = nb; gCnt[3] = nu;
    for (int i = 0; i < 4; i++) {          /* nur die Kartenkoepfe neu zeichnen */
        RECT h = gCard[i];
        h.bottom = h.top + gHdrH;
        InvalidateRect(hwnd, &h, FALSE);
    }
    if (online)
        snprintf(t, sizeof(t),
                 "%d mods detected  \x95  %d via Modrinth, %d from local metadata",
                 total, fromModrinth, fromLocal);
    else
        snprintf(t, sizeof(t),
                 "%d mods detected  \x95  Modrinth unreachable, used local metadata",
                 total);
    SetWindowTextA(gLblStatus, t);

    EnableWindow(gBtnCopy, total > 0);
    EnableWindow(gBtnServer, total > 0);
    SetCursor(LoadCursor(NULL, IDC_ARROW));
}

/* -- sortierte Jars in client\ und server\ kopieren -- */
static void copy_sorted(HWND hwnd)
{
    if (gFolder[0] == 0)
        return;

    char cdir[MAX_PATH], sdir[MAX_PATH];
    snprintf(cdir, sizeof(cdir), "%s\\client", gFolder);
    snprintf(sdir, sizeof(sdir), "%s\\server", gFolder);
    CreateDirectoryA(cdir, NULL);
    CreateDirectoryA(sdir, NULL);

    struct { HWND lb; int toC, toS; } sets[4] = {
        { gListC, 1, 0 },   /* Client-only    -> client */
        { gListS, 0, 1 },   /* Server-only    -> server */
        { gListB, 1, 1 },   /* Server/Client  -> client + server */
        { gListU, 1, 1 },   /* Unbekannt      -> client + server (sicher) */
    };

    int copied = 0, failed = 0;
    for (int s = 0; s < 4; s++) {
        int n = (int)SendMessageA(sets[s].lb, LB_GETCOUNT, 0, 0);
        for (int i = 0; i < n; i++) {
            char name[MAX_PATH];
            SendMessageA(sets[s].lb, LB_GETTEXT, i, (LPARAM)name);
            char src[1024];
            snprintf(src, sizeof(src), "%s\\%s", gFolder, name);
            if (sets[s].toC) {
                char d[1024];
                snprintf(d, sizeof(d), "%s\\%s", cdir, name);
                if (CopyFileA(src, d, FALSE)) copied++; else failed++;
            }
            if (sets[s].toS) {
                char d[1024];
                snprintf(d, sizeof(d), "%s\\%s", sdir, name);
                if (CopyFileA(src, d, FALSE)) copied++; else failed++;
            }
        }
    }

    char t[256];
    snprintf(t, sizeof(t),
             "Done: %d file(s) copied to client\\ and server\\%s",
             copied,
             failed ? " (some failed)" : ".");
    SetWindowTextA(gLblStatus, t);
}

static const char *SCRIPT_BAT_HEAD =
"@echo off\n"
"title Minecraft Server\n"
"cd /d \"%~dp0\"\n"
"\n"
"REM ===== Adjust memory here =====\n"
"set MIN_RAM=2G\n"
"set MAX_RAM=6G\n"
"REM Different Java version? Just change the path below.\n";

static const char *SCRIPT_BAT_TAIL =
"REM ==============================\n"
"\n"
"\"%JAVA_CMD%\" -version >nul 2>&1\n"
"if errorlevel 1 (\n"
"    echo Java was not found ^(JAVA_CMD=%JAVA_CMD%^).\n"
"    echo Please install Java or adjust JAVA_CMD above.\n"
"    pause\n"
"    exit /b 1\n"
")\n"
"\n"
"findstr /i /c:\"eula=true\" eula.txt >nul 2>&1\n"
"if errorlevel 1 (\n"
"    echo.\n"
"    echo You must accept the Minecraft EULA first:\n"
"    echo   1. Open eula.txt\n"
"    echo   2. Change eula=false to eula=true\n"
"    echo   EULA: https://aka.ms/MinecraftEULA\n"
"    echo.\n"
"    pause\n"
"    exit /b 1\n"
")\n"
"\n";

/* nach der Startzeile */
static const char *SCRIPT_BAT_END =
"echo.\n"
"echo Server stopped.\n"
"pause\n";

static const char *SCRIPT_SH =
"#!/usr/bin/env bash\n"
"cd \"$(dirname \"$0\")\" || exit 1\n"
"\n"
"# ===== Adjust memory here =====\n"
"MIN_RAM=2G\n"
"MAX_RAM=6G\n"
"# Different Java version? Enter the full path, e.g.:\n"
"# JAVA_CMD=/usr/lib/jvm/java-25-openjdk/bin/java\n"
"JAVA_CMD=java\n"
"# ==============================\n"
"\n"
"if ! \"$JAVA_CMD\" -version >/dev/null 2>&1; then\n"
"    echo \"Java was not found (JAVA_CMD=$JAVA_CMD).\"\n"
"    echo \"Please install Java or adjust JAVA_CMD above.\"\n"
"    exit 1\n"
"fi\n"
"\n"
"if ! grep -qi '^eula=true' eula.txt 2>/dev/null; then\n"
"    echo\n"
"    echo \"You must accept the Minecraft EULA first:\"\n"
"    echo \"  1. Open eula.txt\"\n"
"    echo \"  2. Change eula=false to eula=true\"\n"
"    echo \"  EULA: https://aka.ms/MinecraftEULA\"\n"
"    echo\n"
"    exit 1\n"
"fi\n"
"\n";

static const char *EULA_TXT =
"# Minecraft EULA\n"
"# By changing this to eula=true you agree to the Minecraft EULA:\n"
"# https://aka.ms/MinecraftEULA\n"
"# This has to be set deliberately - the server will not start otherwise.\n"
"eula=false\n";

/* -- Server-Pack in den Zielordner erzeugen.
 * silent = 0: Rueckfrage vor dem Bauen, Fehler und Ergebnis als Dialog
 *         = 1: gar keine Dialoge (Batch-Modus ueber die Kommandozeile)
 *         = 2: keine Rueckfrage, aber Fehler und Ergebnis werden gezeigt
 *              (Online-Ablauf - dort wurde die Absicht schon zweimal geklickt) */
static void build_server_pack(HWND hwnd, const char *out, int silent)
{
    if (gFolder[0] == 0)
        return;

    HWND lists[4] = { gListC, gListS, gListB, gListU };
    const int isServer[4] = { 0, 1, 1, 1 };
    if ((int)SendMessageA(gListS, LB_GETCOUNT, 0, 0) +
        (int)SendMessageA(gListB, LB_GETCOUNT, 0, 0) +
        (int)SendMessageA(gListU, LB_GETCOUNT, 0, 0) == 0) {
        if (silent != 1)
            MessageBoxA(hwnd, "No mods scanned. Please scan a mods folder first.",
                        "ModSorter", MB_OK | MB_ICONWARNING);
        return;
    }

    /* Profil-Wurzel = Elternordner, wenn der gewaehlte Ordner "mods" heisst */
    char root[MAX_PATH];
    strncpy(root, gFolder, MAX_PATH - 1);
    root[MAX_PATH - 1] = 0;
    char *sl = strrchr(root, '\\');
    if (sl && _stricmp(sl + 1, "mods") == 0)
        *sl = 0;

    if (_stricmp(out, gFolder) == 0 || _stricmp(out, root) == 0) {
        if (silent != 1)
            MessageBoxA(hwnd,
                        "Please choose a different target folder - not the mods\n"
                        "or profile folder itself.", "ModSorter", MB_OK | MB_ICONWARNING);
        return;
    }
    CreateDirectoryA(out, NULL);

    /* Versionen ermitteln */
    SetCursor(LoadCursor(NULL, IDC_WAIT));
    SetWindowTextA(gLblStatus, "Detecting Minecraft version ...");
    UpdateWindow(hwnd);

    char mc[24] = "";
    int reqJava = 0, nMods = 0;
    ModInfo *mi = collect_mods(hwnd, lists, isServer, &nMods, mc, sizeof(mc), &reqJava);
    if (!mi || mc[0] == 0) {
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        if (mi) free(mi);
        if (silent != 1)
            MessageBoxA(hwnd, "Could not detect the Minecraft version from the mods.",
                        "ModSorter", MB_OK | MB_ICONERROR);
        else
            SetWindowTextA(gLblStatus, "Error: could not detect the Minecraft version");
        return;
    }

    /* Abhaengigkeiten aufloesen (client-klassifizierte Libs nachziehen) */
    int promoted = resolve_server_deps(mi, nMods);
    int modCount = 0;
    for (int i = 0; i < nMods; i++)
        if (mi[i].server) modCount++;
    if (reqJava < java_for_mc(mc))          /* Mindestanforderung der MC-Version */
        reqJava = java_for_mc(mc);

    /* Passendes Java suchen (die von den Mods geforderte Version) */
    SetWindowTextA(gLblStatus, "Looking for a matching Java version ...");
    UpdateWindow(hwnd);
    char javaExe[MAX_PATH] = "";
    int javaVer = find_best_java(reqJava, javaExe, sizeof(javaExe));

    const char *loaderName = (gLoader == LOADER_NEOFORGE) ? "NeoForge"
                           : (gLoader == LOADER_FORGE)    ? "Forge"
                                                          : "Fabric";
    SetWindowTextA(gLblStatus, "Querying loader versions ...");
    UpdateWindow(hwnd);
    char loader[48] = "", installer[32] = "";
    int haveLoader;
    if (gLoader == LOADER_NEOFORGE) {
        haveLoader = neoforge_latest(mc, loader, sizeof(loader));
        if (reqJava < java_for_mc(mc))       /* mods.toml nennt kein Java */
            reqJava = java_for_mc(mc);
    } else if (gLoader == LOADER_FORGE) {
        haveLoader = forge_latest(mc, loader, sizeof(loader));
        if (reqJava < java_for_mc(mc))
            reqJava = java_for_mc(mc);
    } else {
        haveLoader = fabric_versions(mc, loader, sizeof(loader),
                                     installer, sizeof(installer));
    }
    SetCursor(LoadCursor(NULL, IDC_ARROW));

    if (!haveLoader) {
        free(mi);
        char em[240];
        snprintf(em, sizeof(em),
                 "Could not fetch %s versions for Minecraft %s.\n"
                 "Please check your internet connection and try again.",
                 loaderName, mc);
        if (silent != 1)
            MessageBoxA(hwnd, em, "ModSorter", MB_OK | MB_ICONERROR);
        else
            SetWindowTextA(gLblStatus, "Error: loader metadata unreachable");
        return;
    }

    char javaInfo[MAX_PATH + 96];
    if (javaVer == reqJava)
        snprintf(javaInfo, sizeof(javaInfo),
                 "Java %d found (required by the mods)", javaVer);
    else if (javaVer)
        snprintf(javaInfo, sizeof(javaInfo),
                 "Java %d used - mods require %d!", javaVer, reqJava);
    else
        snprintf(javaInfo, sizeof(javaInfo),
                 "no Java found - please install Java %d", reqJava);

    (void)javaInfo;

    SetCursor(LoadCursor(NULL, IDC_WAIT));

    /* 1. Mods kopieren */
    char dir[MAX_PATH];
    snprintf(dir, sizeof(dir), "%s\\mods", out);
    CreateDirectoryA(dir, NULL);
    int copiedMods = 0;
    for (int i = 0; i < nMods; i++) {
        if (!mi[i].server) continue;
        char s[1024], d[1024];
        snprintf(s, sizeof(s), "%s\\%s", gFolder, mi[i].file);
        snprintf(d, sizeof(d), "%s\\%s", dir, mi[i].file);
        if (CopyFileA(s, d, FALSE))
            copiedMods++;
    }
    SetWindowTextA(gLblStatus, "Copying configuration ...");
    UpdateWindow(hwnd);

    /* 2. serverrelevante Ordner kopieren */
    static const char *folders[] = { "config", "defaultconfigs", "kubejs",
                                     "scripts", "datapacks" };
    int copiedCfg = 0, gotDatapacks = 0;
    for (int i = 0; i < 5; i++) {
        char s[1024], d[1024];
        snprintf(s, sizeof(s), "%s\\%s", root, folders[i]);
        DWORD a = GetFileAttributesA(s);
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        snprintf(d, sizeof(d), "%s\\%s", out, folders[i]);
        copiedCfg += copy_tree(s, d);
        if (i == 4) gotDatapacks = 1;
    }

    /* 3. Serverkern einrichten - je nach Loader unterschiedlich */
    int gotJar = 0;
    char launchBat[512] = "", launchSh[512] = "";

    if (gLoader == LOADER_NEOFORGE || gLoader == LOADER_FORGE) {
        /* NeoForge und Forge nutzen denselben Installer-Mechanismus,
         * nur andere Maven-Quelle und anderen Pfad zu den args-Dateien. */
        int neo = (gLoader == LOADER_NEOFORGE);
        char ipath[300], argdir[200], st[160];

        if (neo) {
            snprintf(ipath, sizeof(ipath),
                     "/releases/net/neoforged/neoforge/%s/neoforge-%s-installer.jar",
                     loader, loader);
            snprintf(argdir, sizeof(argdir),
                     "libraries/net/neoforged/neoforge/%s", loader);
        } else {
            snprintf(ipath, sizeof(ipath),
                     "/net/minecraftforge/forge/%s/forge-%s-installer.jar",
                     loader, loader);
            snprintf(argdir, sizeof(argdir),
                     "libraries/net/minecraftforge/forge/%s", loader);
        }

        snprintf(st, sizeof(st), "Downloading %s installer ...", loaderName);
        SetWindowTextA(gLblStatus, st);
        UpdateWindow(hwnd);

        wchar_t wip[300];
        MultiByteToWideChar(CP_UTF8, 0, ipath, -1, wip, 300);
        char instPath[MAX_PATH];
        snprintf(instPath, sizeof(instPath), "%s\\loader-installer.jar", out);

        int got = neo
            ? download_file(L"maven.neoforged.net", wip, instPath)
            : download_file(L"maven.minecraftforge.net", wip, instPath);

        if (got) {
            snprintf(st, sizeof(st),
                     "Installing %s server (may take 1-2 minutes) ...", loaderName);
            SetWindowTextA(gLblStatus, st);
            UpdateWindow(hwnd);
            char cmd[1200];
            snprintf(cmd, sizeof(cmd),
                     "\"%s\" -jar \"%s\" --installServer \"%s\"",
                     javaExe[0] ? javaExe : "java", instPath, out);
            run_wait(cmd, out, 900000);
            DeleteFileA(instPath);
            char logp[MAX_PATH];
            snprintf(logp, sizeof(logp), "%s\\loader-installer.jar.log", out);
            DeleteFileA(logp);

            /* Erfolg daran messen, ob die Startargumente entstanden sind */
            char chk[MAX_PATH];
            snprintf(chk, sizeof(chk), "%s\\%s\\win_args.txt", out, argdir);
            for (char *q = chk; *q; q++)
                if (*q == '/') *q = '\\';
            gotJar = (GetFileAttributesA(chk) != INVALID_FILE_ATTRIBUTES);
        }

        snprintf(launchBat, sizeof(launchBat),
                 "\"%%JAVA_CMD%%\" -Xms%%MIN_RAM%% -Xmx%%MAX_RAM%% "
                 "@%s/win_args.txt nogui\n", argdir);
        snprintf(launchSh, sizeof(launchSh),
                 "exec \"$JAVA_CMD\" -Xms$MIN_RAM -Xmx$MAX_RAM "
                 "@%s/unix_args.txt nogui\n", argdir);
    } else {
        /* Fabric: Server-Launcher-Jar laden */
        SetWindowTextA(gLblStatus, "Downloading Fabric server launcher ...");
        UpdateWindow(hwnd);
        char apath[256];
        snprintf(apath, sizeof(apath), "/v2/versions/loader/%s/%s/%s/server/jar",
                 mc, loader, installer);
        wchar_t wap[256];
        MultiByteToWideChar(CP_UTF8, 0, apath, -1, wap, 256);
        char jarPath[MAX_PATH];
        snprintf(jarPath, sizeof(jarPath), "%s\\fabric-server-launch.jar", out);
        gotJar = download_file(L"meta.fabricmc.net", wap, jarPath);

        strcpy(launchBat,
               "\"%JAVA_CMD%\" -Xms%MIN_RAM% -Xmx%MAX_RAM% "
               "-jar fabric-server-launch.jar nogui\n");
        strcpy(launchSh,
               "exec \"$JAVA_CMD\" -Xms$MIN_RAM -Xmx$MAX_RAM "
               "-jar fabric-server-launch.jar nogui\n");
    }

    /* 4. Konfig- und Startdateien schreiben */
    char p[MAX_PATH], buf[1400];
    const char *packName = strrchr(root, '\\');
    packName = packName ? packName + 1 : "Modpack";

    size_t blen = strlen(SCRIPT_BAT_HEAD) + strlen(SCRIPT_BAT_TAIL) +
                  strlen(SCRIPT_BAT_END) + MAX_PATH + 640;
    char *bat = (char *)malloc(blen);
    strcpy(bat, SCRIPT_BAT_HEAD);
    strcat(bat, "set JAVA_CMD=");
    strcat(bat, javaExe[0] ? javaExe : "java");
    strcat(bat, "\n");
    strcat(bat, SCRIPT_BAT_TAIL);
    strcat(bat, launchBat);
    strcat(bat, SCRIPT_BAT_END);
    snprintf(p, sizeof(p), "%s\\start.bat", out);
    write_text(p, bat, 1);                           /* CRLF fuer Windows */
    free(bat);

    size_t slen = strlen(SCRIPT_SH) + 640;
    char *sh = (char *)malloc(slen);
    strcpy(sh, SCRIPT_SH);
    strcat(sh, launchSh);
    snprintf(p, sizeof(p), "%s\\start.sh", out);
    write_text(p, sh, 0);                            /* LF fuer Linux */
    free(sh);

    snprintf(p, sizeof(p), "%s\\eula.txt", out);
    write_text(p, EULA_TXT, 1);

    snprintf(buf, sizeof(buf),
             "#Minecraft server properties (generated by ModSorter)\n"
             "motd=%s\n"
             "server-port=25565\n"
             "online-mode=true\n"
             "max-players=10\n"
             "difficulty=normal\n"
             "gamemode=survival\n"
             "level-name=world\n"
             "view-distance=10\n"
             "simulation-distance=8\n"
             "allow-flight=true\n"
             "spawn-protection=0\n"
             "enable-command-block=false\n"
             "white-list=false\n", packName);
    snprintf(p, sizeof(p), "%s\\server.properties", out);
    write_text(p, buf, 1);

    snprintf(buf, sizeof(buf),
             "Server pack for: %s\n"
             "Generated by ModSorter\n"
             "\n"
             "Minecraft %s  /  %s %s\n"
             "Mods included: %d (client-only mods were left out,\n"
             "%d libraries were added as dependencies)\n"
             "\n"
             "HOW TO START:\n"
             "  1. Open eula.txt and change eula=false to eula=true\n"
             "     (this accepts https://aka.ms/MinecraftEULA)\n"
             "  2. Windows: double-click start.bat\n"
             "     Linux:   chmod +x start.sh  &&  ./start.sh\n"
             "\n"
             "JAVA: version %d\n"
             "  The mods in this pack require Java %d. start.bat already points\n"
             "  at a matching installation:\n"
             "  %s\n"
             "  NOTE: a NEWER Java version does not necessarily work - some mods\n"
             "  (e.g. Cobblemon) require exactly this version.\n"
             "  On Linux, set JAVA_CMD in start.sh to a Java %d.\n"
             "\n"
             "Memory: adjust MAX_RAM at the top of start.bat / start.sh.\n"
             "The first start downloads the Minecraft server and takes longer.\n"
             "%s"
             "\n"
             "Note: players need the same modpack with the client mods\n"
             "in order to connect.\n",
             packName, mc, loaderName, loader, copiedMods, promoted, reqJava, reqJava,
             javaVer ? javaExe : "(none found - please install one)", reqJava,
             gotDatapacks
                 ? "\nNote: datapacks\\ was copied along - depending on the pack,\n"
                   "datapacks belong in world\\datapacks\\.\n"
                 : "");
    snprintf(p, sizeof(p), "%s\\README.txt", out);
    write_text(p, buf, 1);

    SetCursor(LoadCursor(NULL, IDC_ARROW));

    char done[900];
    snprintf(done, sizeof(done),
             "Server pack created.\n\n"
             "Folder:  %s\n"
             "Mods:    %d  (%d added as dependencies)\n"
             "Configs: %d files\n"
             "Java:    %s\n"
             "Server:  %s\n"
             "Scripts: start.bat (Windows), start.sh (Linux)\n\n"
             "Next step: open eula.txt, set eula=true,\n"
             "then run start.bat or start.sh.%s",
             out, copiedMods, promoted, copiedCfg, javaInfo,
             gotJar ? ((gLoader == LOADER_FABRIC) ? "fabric-server-launch.jar"
                                                  : "loader installed")
                    : "FAILED",
             gotJar ? "" : "\n\nThe server could not be set up -\n"
                           "please check your internet connection and try again.");
    free(mi);

    (void)done;

    /* Kein Erfolgsdialog - das Ergebnis steht in der Statuszeile.
     * Fehler werden weiterhin als Meldung gezeigt. */
    char st[300];
    if (gotJar)
        snprintf(st, sizeof(st),
                 "Server pack ready  \x95  %d mods (+%d deps), %d configs, "
                 "Java %d  \x95  %s",
                 copiedMods, promoted, copiedCfg, reqJava, out);
    else
        snprintf(st, sizeof(st),
                 "Server pack incomplete - the loader could not be installed");
    prog_end(st);

    if (!gotJar && silent != 1)
        MessageBoxA(hwnd,
                    "The server could not be set up completely.\n"
                    "Please check your internet connection and try again.",
                    "ModSorter", MB_OK | MB_ICONWARNING);
}

/* -- Zielordner waehlen und Server-Pack erstellen -- */
static void create_server_pack(HWND hwnd)
{
    if (gFolder[0] == 0)
        return;
    char out[MAX_PATH];
    if (pick_folder(hwnd,
                    "Choose a target folder for the server pack (an empty folder is recommended)",
                    NULL, out, sizeof(out)))
        build_server_pack(hwnd, out, 0);
}

/* -- Moderner Ordnerdialog (Vista+): mit Adressleiste und Eingabefeld,
 *    in das man den Pfad direkt tippen oder einfuegen kann.
 *    initial = Ordner, in dem der Dialog oeffnen soll (darf leer sein). -- */
static int pick_folder(HWND owner, const char *title, const char *initial,
                       char *out, int outsz)
{
    IFileOpenDialog *dlg = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL,
                                  CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog,
                                  (void **)&dlg);
    if (FAILED(hr) || !dlg) {
        /* Rueckfall auf den alten Dialog, falls die Schnittstelle fehlt */
        BROWSEINFOA bi;
        ZeroMemory(&bi, sizeof(bi));
        bi.hwndOwner = owner;
        bi.lpszTitle = title;
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
        if (!pidl)
            return 0;
        int ok = SHGetPathFromIDListA(pidl, out) ? 1 : 0;
        CoTaskMemFree(pidl);
        return ok;
    }

    DWORD opts = 0;
    dlg->lpVtbl->GetOptions(dlg, &opts);
    dlg->lpVtbl->SetOptions(dlg, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                                        FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);

    wchar_t wt[256];
    MultiByteToWideChar(CP_ACP, 0, title, -1, wt, 256);
    dlg->lpVtbl->SetTitle(dlg, wt);

    if (initial && initial[0]) {
        wchar_t wi[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, initial, -1, wi, MAX_PATH);
        IShellItem *si = NULL;
        if (SUCCEEDED(SHCreateItemFromParsingName(wi, NULL, &IID_IShellItem,
                                                  (void **)&si)) && si) {
            dlg->lpVtbl->SetFolder(dlg, si);
            si->lpVtbl->Release(si);
        }
    }

    int ok = 0;
    if (SUCCEEDED(dlg->lpVtbl->Show(dlg, owner))) {
        IShellItem *item = NULL;
        if (SUCCEEDED(dlg->lpVtbl->GetResult(dlg, &item)) && item) {
            PWSTR path = NULL;
            if (SUCCEEDED(item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH,
                                                       &path)) && path) {
                WideCharToMultiByte(CP_ACP, 0, path, -1, out, outsz, NULL, NULL);
                CoTaskMemFree(path);
                ok = 1;
            }
            item->lpVtbl->Release(item);
        }
    }
    dlg->lpVtbl->Release(dlg);
    return ok;
}

/* ================== Modpack-Auswahl ueber alle Launcher ================== */

typedef struct {
    char launcher[32];
    char name[128];
    char path[MAX_PATH];      /* lokal: mods-Ordner  |  online: Modrinth-Slug */
    int  mods;                /* lokal: Anzahl Jars  |  online: Downloads */
    int  online;
    /* nur fuer Online-Treffer */
    char author[80];
    char desc[300];
    char updated[16];         /* JJJJ-MM-TT */
    char tags[4][28];
    int  ntags;
    int  follows;
    char iconUrl[400];
    HBITMAP icon;                 /* wird im Hintergrund nachgeladen */
} Instance;

static Instance *gInst = NULL;
static int gInstN = 0, gInstCap = 0;
static void free_icons(void);      /* fwd */
static int gPickResult = -1, gPickServer = 0;
static HWND gPickWnd = NULL, gPickList = NULL;

static int count_jars(const char *dir)
{
    char pat[MAX_PATH];
    snprintf(pat, sizeof(pat), "%s\\*.jar", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    int n = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

static void inst_add(const char *launcher, const char *name, const char *path)
{
    int n = count_jars(path);
    if (n == 0)
        return;                       /* leere Instanzen nicht anzeigen */
    for (int i = 0; i < gInstN; i++)  /* Duplikate vermeiden */
        if (_stricmp(gInst[i].path, path) == 0)
            return;
    if (gInstN >= gInstCap) {
        gInstCap = gInstCap ? gInstCap * 2 : 32;
        gInst = (Instance *)realloc(gInst, sizeof(Instance) * gInstCap);
    }
    Instance *it = &gInst[gInstN++];
    strncpy(it->launcher, launcher, 31); it->launcher[31] = 0;
    strncpy(it->name, name, 127);       it->name[127] = 0;
    strncpy(it->path, path, MAX_PATH - 1); it->path[MAX_PATH - 1] = 0;
    it->mods = n;
    it->online = 0;
}

/* Unterordner von base durchgehen und base\<inst>\<sub>\ als mods-Ordner pruefen.
 * sub darf leer sein. */
static void scan_launcher(const char *launcher, const char *base, const char *sub)
{
    char pat[MAX_PATH];
    snprintf(pat, sizeof(pat), "%s\\*", base);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.') continue;
        char mods[MAX_PATH];
        if (sub && sub[0])
            snprintf(mods, sizeof(mods), "%s\\%s\\%s\\mods", base, fd.cFileName, sub);
        else
            snprintf(mods, sizeof(mods), "%s\\%s\\mods", base, fd.cFileName);
        DWORD a = GetFileAttributesA(mods);
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY))
            inst_add(launcher, fd.cFileName, mods);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* Alle bekannten Launcher absuchen */
static void enum_instances(void)
{
    free_icons();
    gInstN = 0;
    char app[MAX_PATH] = "", usr[MAX_PATH] = "", loc[MAX_PATH] = "", d[MAX_PATH];
    GetEnvironmentVariableA("APPDATA", app, MAX_PATH);
    GetEnvironmentVariableA("USERPROFILE", usr, MAX_PATH);
    GetEnvironmentVariableA("LOCALAPPDATA", loc, MAX_PATH);

    if (app[0]) {
        snprintf(d, sizeof(d), "%s\\ModrinthApp\\profiles", app);
        scan_launcher("Modrinth", d, NULL);
        snprintf(d, sizeof(d), "%s\\com.modrinth.theseus\\profiles", app);
        scan_launcher("Modrinth", d, NULL);
        snprintf(d, sizeof(d), "%s\\PrismLauncher\\instances", app);
        scan_launcher("Prism", d, ".minecraft");
        scan_launcher("Prism", d, "minecraft");
        snprintf(d, sizeof(d), "%s\\MultiMC\\instances", app);
        scan_launcher("MultiMC", d, ".minecraft");
        snprintf(d, sizeof(d), "%s\\ATLauncher\\instances", app);
        scan_launcher("ATLauncher", d, NULL);
        snprintf(d, sizeof(d), "%s\\gdlauncher_next\\instances", app);
        scan_launcher("GDLauncher", d, NULL);
        snprintf(d, sizeof(d), "%s\\.technic\\modpacks", app);
        scan_launcher("Technic", d, NULL);
        /* Vanilla-Launcher hat nur eine Instanz */
        snprintf(d, sizeof(d), "%s\\.minecraft\\mods", app);
        DWORD a = GetFileAttributesA(d);
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY))
            inst_add("Minecraft", ".minecraft", d);
    }
    if (usr[0]) {
        snprintf(d, sizeof(d), "%s\\curseforge\\minecraft\\Instances", usr);
        scan_launcher("CurseForge", d, NULL);
        snprintf(d, sizeof(d), "%s\\Documents\\curseforge\\minecraft\\Instances", usr);
        scan_launcher("CurseForge", d, NULL);
        snprintf(d, sizeof(d), "%s\\Twitch\\Minecraft\\Instances", usr);
        scan_launcher("CurseForge", d, NULL);
    }
    if (loc[0]) {
        snprintf(d, sizeof(d), "%s\\.ftba\\instances", loc);
        scan_launcher("FTB App", d, NULL);
        snprintf(d, sizeof(d), "%s\\Programs\\gdlauncher_carbon\\instances", loc);
        scan_launcher("GDLauncher", d, NULL);
    }

    /* nach Launcher, dann Name sortieren */
    for (int i = 1; i < gInstN; i++) {
        Instance t = gInst[i];
        int j = i - 1;
        while (j >= 0 && (_stricmp(gInst[j].launcher, t.launcher) > 0 ||
                          (_stricmp(gInst[j].launcher, t.launcher) == 0 &&
                           _stricmp(gInst[j].name, t.name) > 0))) {
            gInst[j + 1] = gInst[j];
            j--;
        }
        gInst[j + 1] = t;
    }
}

/* ---- Online-Suche bei Modrinth ---- */

/* Text fuer eine URL kodieren */
static void url_encode(const char *s, char *out, int outsz)
{
    static const char *hex = "0123456789ABCDEF";
    int o = 0;
    for (const char *p = s; *p && o < outsz - 4; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            out[o++] = (char)c;
        else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = 0;
}

/* Modpacks bei Modrinth suchen und in gInst eintragen */
/* Modrinth liefert UTF-8, gezeichnet wird mit den ANSI-Funktionen */
static void utf8_fix(char *s, int sz)
{
    if (!s || !s[0])
        return;
    wchar_t w[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, 1024) == 0)
        return;
    WideCharToMultiByte(CP_ACP, 0, w, -1, s, sz, "?", NULL);
}

/* geladene Icons freigeben */
static void free_icons(void)
{
    for (int i = 0; i < gInstN; i++)
        if (gInst[i].icon) {
            DeleteObject(gInst[i].icon);
            gInst[i].icon = NULL;
        }
}

/* sort: 0 = Relevanz, 1 = Downloads, 2 = zuletzt aktualisiert */
static void search_modrinth(const char *query, int offset, int sort)
{
    char q[512];
    url_encode(query, q, sizeof(q));
    const char *idx = (sort == 1) ? "downloads"
                    : (sort == 2) ? "updated"
                                  : "relevance";

    char path[900];
    snprintf(path, sizeof(path),
             "/v2/search?query=%s&facets=%%5B%%5B%%22project_type:modpack%%22%%5D%%5D"
             "&limit=20&offset=%d&index=%s", q, offset, idx);
    wchar_t wp[900];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 900);

    DWORD len = 0;
    char *r = https_request(L"api.modrinth.com", L"GET", wp, NULL, 0, &len);
    if (!r)
        return;

    const char *e = r + len;
    const char *p = strstr(r, "\"hits\"");
    if (p) {
        p = strchr(p, '[');
        if (p) p++;
        while (p && p < e) {
            while (p < e && *p != '{' && *p != ']') p++;
            if (p >= e || *p == ']') break;
            const char *o = p;
            int depth = 0, instr = 0;
            for (; p < e; p++) {
                char ch = *p;
                if (instr) { if (ch == '\\') p++; else if (ch == '\"') instr = 0; }
                else { if (ch == '\"') instr = 1; else if (ch == '{') depth++;
                       else if (ch == '}') { depth--; if (depth == 0) { p++; break; } } }
            }
            char title[128], slug[96];
            if (json_str(o, p, "title", title, sizeof(title)) &&
                json_str(o, p, "slug", slug, sizeof(slug))) {
                if (gInstN >= gInstCap) {
                    gInstCap = gInstCap ? gInstCap * 2 : 32;
                    gInst = (Instance *)realloc(gInst, sizeof(Instance) * gInstCap);
                }
                Instance *it = &gInst[gInstN++];
                memset(it, 0, sizeof(*it));
                strcpy(it->launcher, "Modrinth");
                strncpy(it->name, title, 127); it->name[127] = 0;
                strncpy(it->path, slug, MAX_PATH - 1); it->path[MAX_PATH - 1] = 0;
                it->online = 1;

                /* Zahlen */
                const char *d = strstr(o, "\"downloads\"");
                if (d && d < p) {
                    const char *q2 = d + 11;
                    while (q2 < p && (*q2 == ' ' || *q2 == ':')) q2++;
                    it->mods = atoi(q2);
                }
                d = strstr(o, "\"follows\"");
                if (d && d < p) {
                    const char *q2 = d + 9;
                    while (q2 < p && (*q2 == ' ' || *q2 == ':')) q2++;
                    it->follows = atoi(q2);
                }

                json_str(o, p, "author", it->author, sizeof(it->author));
                json_str(o, p, "description", it->desc, sizeof(it->desc));

                char dt[40];
                if (json_str(o, p, "date_modified", dt, sizeof(dt))) {
                    strncpy(it->updated, dt, 10);
                    it->updated[10] = 0;
                }

                char cats[6][64];
                int nc = json_str_array(o, p, "display_categories", cats, 6);
                if (nc == 0)
                    nc = json_str_array(o, p, "categories", cats, 6);
                for (int c = 0; c < nc && it->ntags < 4; c++) {
                    if (cats[c][0] >= 'a' && cats[c][0] <= 'z')
                        cats[c][0] = (char)(cats[c][0] - 'a' + 'A');
                    strncpy(it->tags[it->ntags], cats[c], 27);
                    it->tags[it->ntags][27] = 0;
                    it->ntags++;
                }
                utf8_fix(it->name, sizeof(it->name));
                utf8_fix(it->author, sizeof(it->author));
                utf8_fix(it->desc, sizeof(it->desc));
                for (int t = 0; t < it->ntags; t++)
                    utf8_fix(it->tags[t], sizeof(it->tags[t]));

                /* nur merken - geladen wird spaeter im Hintergrund */
                json_str(o, p, "icon_url", it->iconUrl, sizeof(it->iconUrl));
            }
            while (p < e && *p != ',' && *p != ']') p++;
            if (p < e && *p == ',') p++;
        }
    }
    free(r);
}

/* ---- Versionen eines Modrinth-Packs ---- */
typedef struct {
    char name[96];        /* version_number */
    char mc[32];          /* erste game_version */
    char loader[24];      /* erster loader */
    char url[700];        /* Download der .mrpack bzw. der CurseForge-Zip */
    char filename[200];
    int  cfFileId;        /* nur CurseForge */
    int  cfModId;
} PackVersion;

static PackVersion *gVer = NULL;
static int gVerN = 0, gVerCap = 0;

static int modrinth_versions(const char *slug)
{
    gVerN = 0;
    char path[300];
    snprintf(path, sizeof(path), "/v2/project/%s/version", slug);
    wchar_t wp[300];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 300);

    DWORD len = 0;
    char *r = https_request(L"api.modrinth.com", L"GET", wp, NULL, 0, &len);
    if (!r)
        return 0;

    const char *e = r + len;
    const char *p = strchr(r, '[');
    if (p) p++;
    while (p && p < e) {
        while (p < e && *p != '{' && *p != ']') p++;
        if (p >= e || *p == ']') break;
        const char *o = p;
        int depth = 0, instr = 0;
        for (; p < e; p++) {
            char ch = *p;
            if (instr) { if (ch == '\\') p++; else if (ch == '\"') instr = 0; }
            else { if (ch == '\"') instr = 1; else if (ch == '{') depth++;
                   else if (ch == '}') { depth--; if (depth == 0) { p++; break; } } }
        }

        char vn[96], url[700], fn[200];
        if (json_str(o, p, "version_number", vn, sizeof(vn)) &&
            json_str(o, p, "url", url, sizeof(url)) &&
            json_str(o, p, "filename", fn, sizeof(fn))) {
            char gv[8][64], ld[8][64];
            int ng = json_str_array(o, p, "game_versions", gv, 8);
            int nl = json_str_array(o, p, "loaders", ld, 8);
            if (gVerN >= gVerCap) {
                gVerCap = gVerCap ? gVerCap * 2 : 32;
                gVer = (PackVersion *)realloc(gVer, sizeof(PackVersion) * gVerCap);
            }
            PackVersion *v = &gVer[gVerN++];
            strncpy(v->name, vn, 95);  v->name[95] = 0;
            strncpy(v->url, url, 699); v->url[699] = 0;
            strncpy(v->filename, fn, 199); v->filename[199] = 0;
            strncpy(v->mc, ng ? gv[ng - 1] : "?", 31); v->mc[31] = 0;
            strncpy(v->loader, nl ? ld[0] : "?", 23);  v->loader[23] = 0;
        }
        while (p < e && *p != ',' && *p != ']') p++;
        if (p < e && *p == ',') p++;
    }
    free(r);
    return gVerN;
}

/* Anfrage an api.curseforge.com mit Schluessel */
static char *cf_request(const char *path, const char *body, DWORD *outLen)
{
    if (!gCfKey[0])
        return NULL;
    wchar_t wp[900], hdr[200];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 900);
    wchar_t wkey[128];
    MultiByteToWideChar(CP_UTF8, 0, gCfKey, -1, wkey, 128);
    _snwprintf(hdr, 200, L"x-api-key: %s\r\nAccept: application/json\r\n", wkey);
    return https_request_hdr(L"api.curseforge.com", body ? L"POST" : L"GET",
                             wp, body, body ? (int)strlen(body) : 0, outLen, hdr);
}

/* Modpacks bei CurseForge suchen und an gInst anhaengen */
static void search_curseforge(const char *query, int offset, int sort)
{
    if (!gCfKey[0])
        return;
    char q[512];
    url_encode(query, q, sizeof(q));
    /* CurseForge: 2 = Beliebtheit, 3 = zuletzt aktualisiert, 6 = Downloads */
    int sf = (sort == 1) ? 6 : (sort == 2) ? 3 : 2;
    char path[800];
    snprintf(path, sizeof(path),
             "/v1/mods/search?gameId=432&classId=4471&searchFilter=%s"
             "&pageSize=20&index=%d&sortField=%d&sortOrder=desc", q, offset, sf);

    DWORD len = 0;
    char *r = cf_request(path, NULL, &len);
    if (!r)
        return;

    const char *e = r + len;
    const char *p = strstr(r, "\"data\"");
    if (p) p = strchr(p, '[');
    if (p) p++;
    while (p && p < e) {
        while (p < e && *p != '{' && *p != ']') p++;
        if (p >= e || *p == ']') break;
        const char *o = p;
        int depth = 0, instr = 0;
        for (; p < e; p++) {
            char ch = *p;
            if (instr) { if (ch == '\\') p++; else if (ch == '\"') instr = 0; }
            else { if (ch == '\"') instr = 1; else if (ch == '{') depth++;
                   else if (ch == '}') { depth--; if (depth == 0) { p++; break; } } }
        }

        /* Nur Schluessel der obersten Ebene lesen - das Objekt enthaelt
         * screenshots, categories, authors und latestFiles mit gleichen
         * Feldnamen. */
        char name[128];
        if (!json_str_top(o, p, "name", name, sizeof(name)))
            continue;
        int id = json_int_top(o, p, "id");
        if (!id)
            continue;

        if (gInstN >= gInstCap) {
            gInstCap = gInstCap ? gInstCap * 2 : 32;
            gInst = (Instance *)realloc(gInst, sizeof(Instance) * gInstCap);
        }
        Instance *it = &gInst[gInstN++];
        memset(it, 0, sizeof(*it));
        strcpy(it->launcher, "CurseForge");
        strncpy(it->name, name, 127); it->name[127] = 0;
        snprintf(it->path, MAX_PATH, "%d", id);
        it->online = 2;

        it->mods = json_int_top(o, p, "downloadCount");
        json_str_top(o, p, "summary", it->desc, sizeof(it->desc));
        /* Autor: erster Eintrag in "authors" */
        const char *ap = json_find_top(o, p, "authors");
        if (ap)
            json_str(ap, p, "name", it->author, sizeof(it->author));
        char dt[40];
        if (json_str_top(o, p, "dateModified", dt, sizeof(dt))) {
            strncpy(it->updated, dt, 10);
            it->updated[10] = 0;
        }
        utf8_fix(it->name, sizeof(it->name));
        utf8_fix(it->author, sizeof(it->author));
        utf8_fix(it->desc, sizeof(it->desc));

        /* Logo nur merken - geladen wird spaeter im Hintergrund */
        const char *lp = json_find_top(o, p, "logo");
        if (lp)
            json_str(lp, p, "url", it->iconUrl, sizeof(it->iconUrl));

        while (p < e && *p != ',' && *p != ']') p++;
        if (p < e && *p == ',') p++;
    }
    free(r);
}

/* Dateien (Versionen) eines CurseForge-Modpacks holen */
static int curseforge_versions(int modId)
{
    gVerN = 0;
    char path[200];
    snprintf(path, sizeof(path), "/v1/mods/%d/files?pageSize=50", modId);
    DWORD len = 0;
    char *r = cf_request(path, NULL, &len);
    if (!r)
        return 0;

    const char *e = r + len;
    const char *p = strstr(r, "\"data\"");
    if (p) p = strchr(p, '[');
    if (p) p++;
    while (p && p < e) {
        while (p < e && *p != '{' && *p != ']') p++;
        if (p >= e || *p == ']') break;
        const char *o = p;
        int depth = 0, instr = 0;
        for (; p < e; p++) {
            char ch = *p;
            if (instr) { if (ch == '\\') p++; else if (ch == '\"') instr = 0; }
            else { if (ch == '\"') instr = 1; else if (ch == '{') depth++;
                   else if (ch == '}') { depth--; if (depth == 0) { p++; break; } } }
        }

        char disp[96];
        if (!json_str_top(o, p, "displayName", disp, sizeof(disp)))
            continue;
        int fid = json_int_top(o, p, "id");
        if (!fid) continue;

        if (gVerN >= gVerCap) {
            gVerCap = gVerCap ? gVerCap * 2 : 32;
            gVer = (PackVersion *)realloc(gVer, sizeof(PackVersion) * gVerCap);
        }
        PackVersion *v = &gVer[gVerN++];
        memset(v, 0, sizeof(*v));
        strncpy(v->name, disp, 95); v->name[95] = 0;
        utf8_fix(v->name, sizeof(v->name));
        v->cfFileId = fid;
        v->cfModId = modId;
        json_str_top(o, p, "fileName", v->filename, sizeof(v->filename));
        json_str_top(o, p, "downloadUrl", v->url, sizeof(v->url));

        /* gameVersions enthaelt MC-Version und Loader gemischt */
        char gv[12][64];
        int ng = json_str_array(o, p, "gameVersions", gv, 12);
        strcpy(v->mc, "?");
        strcpy(v->loader, "?");
        for (int i = 0; i < ng; i++) {
            if (gv[i][0] >= '0' && gv[i][0] <= '9') {
                if (strcmp(v->mc, "?") == 0) { strncpy(v->mc, gv[i], 31); v->mc[31] = 0; }
            } else if (_stricmp(gv[i], "Forge") == 0 || _stricmp(gv[i], "Fabric") == 0 ||
                       _stricmp(gv[i], "NeoForge") == 0 || _stricmp(gv[i], "Quilt") == 0) {
                strncpy(v->loader, gv[i], 23); v->loader[23] = 0;
            }
        }
        while (p < e && *p != ',' && *p != ']') p++;
        if (p < e && *p == ',') p++;
    }
    free(r);
    return gVerN;
}

/* ---- Icons im Hintergrund nachladen ----
 * Die Treffer erscheinen sofort; die Bilder trudeln nach und werden per
 * Nachricht an das Fenster gemeldet. Nur der UI-Thread fasst gInst an. */
#define WM_ICON_READY (WM_APP + 1)

typedef struct { LONG gen; int idx; HBITMAP bmp; } IconMsg;
typedef struct { LONG gen; int n; char (*urls)[400]; } IconTask;

static volatile LONG gIconGen = 0;     /* zaehlt bei jeder neuen Trefferliste hoch */

static DWORD WINAPI icon_worker(LPVOID param)
{
    IconTask *t = (IconTask *)param;
    for (int i = 0; i < t->n; i++) {
        if (gIconGen != t->gen)
            break;                     /* Liste wurde inzwischen ersetzt */
        if (!t->urls[i][0])
            continue;
        HBITMAP b = load_icon(t->urls[i]);
        if (!b)
            continue;
        HWND w = gPickWnd;
        if (gIconGen != t->gen || !w) { DeleteObject(b); break; }
        IconMsg *m = (IconMsg *)malloc(sizeof(IconMsg));
        m->gen = t->gen; m->idx = i; m->bmp = b;
        if (!PostMessageA(w, WM_ICON_READY, 0, (LPARAM)m)) {
            DeleteObject(b);
            free(m);
            break;
        }
    }
    free(t->urls);
    free(t);
    return 0;
}

/* Ladeauftrag aus der aktuellen Trefferliste erzeugen */
static void start_icon_loader(void)
{
    InterlockedIncrement(&gIconGen);
    if (gInstN <= 0)
        return;
    IconTask *t = (IconTask *)malloc(sizeof(IconTask));
    t->gen = gIconGen;
    t->n = gInstN;
    t->urls = (char (*)[400])malloc(sizeof(char[400]) * gInstN);
    for (int i = 0; i < gInstN; i++) {
        strncpy(t->urls[i], gInst[i].icon ? "" : gInst[i].iconUrl, 399);
        t->urls[i][399] = 0;
    }
    HANDLE h = CreateThread(NULL, 0, icon_worker, t, 0, NULL);
    if (h) CloseHandle(h);
    else { free(t->urls); free(t); }
}

#define ID_PICK_LIST   2001
#define ID_PICK_LOAD   2002
#define ID_PICK_SERVER 2003
#define ID_PICK_CANCEL 2004
#define ID_PICK_SEARCH 2005
#define ID_PICK_GO     2006
#define ID_PICK_BACK   2007
#define ID_PICK_SRC    2008
#define ID_PICK_SORT   2009
#define ID_PICK_MORE   2010

static int  gSrcFilter = 0;      /* 0 = beide, 1 = nur Modrinth, 2 = nur CurseForge */
static int  gSortMode  = 0;      /* 0 = Relevanz, 1 = Downloads, 2 = aktualisiert */
static int  gPage      = 0;
static char gQuery[256] = "";

static const char *src_label(void)
{
    return gSrcFilter == 1 ? "Source: Modrinth"
         : gSrcFilter == 2 ? "Source: CurseForge"
                           : "Source: All";
}
static const char *sort_label(void)
{
    return gSortMode == 1 ? "Sort: Downloads"
         : gSortMode == 2 ? "Sort: Updated"
                          : "Sort: Relevance";
}

/* Suche ausfuehren; append=1 haengt die naechste Seite an */
static void do_search(HWND hwnd, int append)
{
    (void)hwnd;
    if (!append) {
        free_icons();
        gInstN = 0;
        gPage = 0;
    } else {
        gPage++;
    }
    int off = gPage * 20;
    if (gSrcFilter != 2) search_modrinth(gQuery, off, gSortMode);
    if (gSrcFilter != 1) search_curseforge(gQuery, off, gSortMode);
    start_icon_loader();          /* Bilder kommen im Hintergrund nach */
}

/* 0 = installierte Packs, 1 = Online-Treffer, 2 = Versionsliste */
static int  gPickMode = 0;
static char gPickSlug[128] = "", gPickName[128] = "";
static char gPickUrl[700] = "";
static int  gPickOnlineResult = 0;
static int  gPickSource = 1;          /* 1 = Modrinth, 2 = CurseForge */
static HWND gPickSearch = NULL;
static WNDPROC gSearchOrig = NULL;

static LRESULT CALLBACK SearchProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if ((m == WM_KEYDOWN || m == WM_CHAR) && w == VK_RETURN) {
        if (m == WM_KEYDOWN)
            SendMessageA(GetParent(h), WM_COMMAND, MAKEWPARAM(ID_PICK_GO, 0), 0);
        return 0;
    }
    return CallWindowProcA(gSearchOrig, h, m, w, l);
}

/* Liste neu befuellen - je nach Modus */
static void pick_fill(HWND hwnd)
{
    SendMessageA(gPickList, LB_RESETCONTENT, 0, 0);
    int n = (gPickMode == 2) ? gVerN : gInstN;
    for (int i = 0; i < n; i++) {
        const char *txt = (gPickMode == 2) ? gVer[i].name : gInst[i].name;
        int p = (int)SendMessageA(gPickList, LB_ADDSTRING, 0, (LPARAM)txt);
        SendMessageA(gPickList, LB_SETITEMDATA, p, i);
    }
    if (n)
        SendMessageA(gPickList, LB_SETCURSEL, 0, 0);
    EnableWindow(GetDlgItem(hwnd, ID_PICK_LOAD), n > 0 && gPickMode != 1);
    EnableWindow(GetDlgItem(hwnd, ID_PICK_SERVER), n > 0);
    EnableWindow(GetDlgItem(hwnd, ID_PICK_BACK), gPickMode != 0);
    /* Nachladen und Sortierung nur in der Trefferliste sinnvoll */
    EnableWindow(GetDlgItem(hwnd, ID_PICK_MORE), gPickMode == 1);
    EnableWindow(GetDlgItem(hwnd, ID_PICK_SORT), gPickMode != 2);
    EnableWindow(GetDlgItem(hwnd, ID_PICK_SRC),  gPickMode != 2);
    InvalidateRect(hwnd, NULL, FALSE);
}

static void pick_layout(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom, m = S(16), btnH = S(36), gap = S(8);

    /* Suchzeile - Feld mittig im gezeichneten Rahmen */
    int sy = S(44), bGo = S(104);
    int sw = W - 2 * m - gap - bGo;
    gSearchBox.left = m;  gSearchBox.top = sy;
    gSearchBox.right = m + sw;  gSearchBox.bottom = sy + btnH;
    int seh = gTextH + S(4);
    MoveWindow(gPickSearch, m + S(2), sy + (btnH - seh) / 2, sw - S(4), seh, TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_PICK_GO), W - m - bGo, sy, bGo, btnH, TRUE);

    /* Filterzeile: Quelle, Sortierung, rechts das Nachladen */
    int fy = sy + btnH + S(8), fh = S(30);
    int bSrc = S(158), bSort = S(150), bMore = S(128);
    MoveWindow(GetDlgItem(hwnd, ID_PICK_SRC), m, fy, bSrc, fh, TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_PICK_SORT), m + bSrc + gap, fy, bSort, fh, TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_PICK_MORE), W - m - bMore, fy, bMore, fh, TRUE);

    int top = fy + fh + S(12);
    int listH = H - top - m - btnH - S(12);
    if (listH < S(80)) listH = S(80);
    MoveWindow(gPickList, m, top, W - 2 * m, listH, TRUE);

    int by = H - m - btnH;
    int bS = S(216), bL = S(104), bC = S(104), bB = S(96);
    MoveWindow(GetDlgItem(hwnd, ID_PICK_SERVER), W - m - bS, by, bS, btnH, TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_PICK_LOAD), W - m - bS - gap - bL, by, bL, btnH, TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_PICK_CANCEL), m, by, bC, btnH, TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_PICK_BACK), m + bC + gap, by, bB, btnH, TRUE);
}

static void pick_finish(HWND hwnd, int server)
{
    int sel = (int)SendMessageA(gPickList, LB_GETCURSEL, 0, 0);
    if (sel < 0)
        return;
    int idx = (int)SendMessageA(gPickList, LB_GETITEMDATA, sel, 0);
    if (server >= 0)                      /* -1 = zuvor gewaehlte Absicht behalten */
        gPickServer = server;

    if (gPickMode == 1) {                 /* Online-Treffer -> Versionen zeigen */
        if (idx < 0 || idx >= gInstN) return;
        strncpy(gPickSlug, gInst[idx].path, 127); gPickSlug[127] = 0;
        strncpy(gPickName, gInst[idx].name, 127); gPickName[127] = 0;
        gPickSource = gInst[idx].online;   /* 1 = Modrinth, 2 = CurseForge */
        SetCursor(LoadCursor(NULL, IDC_WAIT));
        int n = (gPickSource == 2) ? curseforge_versions(atoi(gPickSlug))
                                   : modrinth_versions(gPickSlug);
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        if (n <= 0) {
            char em[400];
            snprintf(em, sizeof(em),
                     "No versions found for this modpack.\n\n"
                     "Pack:   %s\n"
                     "Source: %s\n"
                     "Id:     %s",
                     gPickName,
                     gPickSource == 2 ? "CurseForge" : "Modrinth",
                     gPickSlug);
            MessageBoxA(hwnd, em, "ModSorter", MB_OK | MB_ICONWARNING);
            return;
        }
        gPickMode = 2;
        pick_fill(hwnd);
        return;
    }

    if (gPickMode == 2) {                 /* Version gewaehlt -> fertig */
        if (idx < 0 || idx >= gVerN) return;
        strncpy(gPickUrl, gVer[idx].url, sizeof(gPickUrl) - 1);
        gPickUrl[sizeof(gPickUrl) - 1] = 0;
        gPickOnlineResult = 1;
        DestroyWindow(hwnd);
        return;
    }

    gPickResult = idx;                    /* installiertes Pack */
    DestroyWindow(hwnd);
}

static LRESULT CALLBACK PickProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, gbrBg);
        SetBkMode(dc, TRANSPARENT);
        char head[200];
        if (gPickMode == 2)
            snprintf(head, sizeof(head), "Choose version  \x95  %s", gPickName);
        else if (gPickMode == 1) {
            int nMr = 0, nCf = 0;
            for (int i = 0; i < gInstN; i++) {
                if (gInst[i].online == 2) nCf++;
                else                      nMr++;
            }
            if (nMr && nCf)
                snprintf(head, sizeof(head),
                         "%d results  \x95  %d Modrinth, %d CurseForge",
                         gInstN, nMr, nCf);
            else if (nCf)
                snprintf(head, sizeof(head), "%d results on CurseForge", nCf);
            else
                snprintf(head, sizeof(head), "%d results on Modrinth", nMr);
        }
        else
            snprintf(head, sizeof(head),
                     gInstN ? "%d installed modpacks  \x95  search above for more"
                            : "No installed modpacks  \x95  search by name above",
                     gInstN);
        RECT t = { S(16), S(14), rc.right - S(16), S(40) };
        SelectObject(dc, gFontB);
        SetTextColor(dc, CTEXT);
        DrawTextA(dc, head, -1, &t,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

        /* Rahmen ums Suchfeld */
        round_box(dc, gSearchBox, S(8), CINPUT,
                  GetFocus() == gPickSearch ? CACC : CBORDER);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, CTEXT);
        SetBkColor(dc, CINPUT);
        return (LRESULT)gbrInput;
    }

    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, CDIM);
        SetBkColor(dc, CCARD);
        return (LRESULT)gbrCard;
    }

    case WM_ICON_READY: {
        IconMsg *m = (IconMsg *)lp;
        if (m) {
            if (m->gen == gIconGen && m->idx >= 0 && m->idx < gInstN &&
                !gInst[m->idx].icon) {
                gInst[m->idx].icon = m->bmp;
                /* nur die betroffene Zeile neu zeichnen */
                RECT ir;
                if (SendMessageA(gPickList, LB_GETITEMRECT, m->idx,
                                 (LPARAM)&ir) != LB_ERR)
                    InvalidateRect(gPickList, &ir, FALSE);
            } else {
                DeleteObject(m->bmp);
            }
            free(m);
        }
        return 0;
    }

    case WM_MEASUREITEM: {
        LPMEASUREITEMSTRUCT mi = (LPMEASUREITEMSTRUCT)lp;
        if (mi->CtlType == ODT_LISTBOX)
            mi->itemHeight = (gPickMode == 1) ? S(104) : S(34);
        return TRUE;
    }

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT d = (LPDRAWITEMSTRUCT)lp;

        if (d->CtlType == ODT_LISTBOX) {
            if ((int)d->itemID < 0)
                return TRUE;
            int idx = (int)SendMessageA(d->hwndItem, LB_GETITEMDATA, d->itemID, 0);
            int lim = (gPickMode == 2) ? gVerN : gInstN;
            if (idx < 0 || idx >= lim)
                return TRUE;
            int sel = (d->itemState & ODS_SELECTED) != 0;

            /* Doppelpufferung: erst abseits zeichnen, dann in einem Zug
             * kopieren - sonst flackert die Liste beim Scrollen. */
            HDC   realDC = d->hDC;
            RECT  realR  = d->rcItem;
            int   bw = realR.right - realR.left, bh = realR.bottom - realR.top;
            HDC   mdc = CreateCompatibleDC(realDC);
            HBITMAP mbm = CreateCompatibleBitmap(realDC, bw, bh);
            HGDIOBJ oldBm = SelectObject(mdc, mbm);
            d->hDC = mdc;
            d->rcItem.left = 0; d->rcItem.top = 0;
            d->rcItem.right = bw; d->rcItem.bottom = bh;

            RECT r = d->rcItem;
            SetBkMode(d->hDC, TRANSPARENT);

            /* ---- Online-Treffer: Karte wie auf der Modrinth-Seite ---- */
            if (gPickMode == 1 && gInst[idx].online) {
                Instance *it = &gInst[idx];
                RECT card = { r.left, r.top + S(3), r.right, r.bottom - S(3) };
                round_box(d->hDC, card, S(10),
                          sel ? mix(CCARD, CACC, 14) : CCARD,
                          sel ? CACC : CBORDER);

                int pad = S(12);
                int isz = S(58);
                RECT ic = { card.left + pad, card.top + pad,
                            card.left + pad + isz, card.top + pad + isz };
                if (it->icon) {
                    HDC mem = CreateCompatibleDC(d->hDC);
                    HGDIOBJ old = SelectObject(mem, it->icon);
                    BITMAP bm;
                    GetObject(it->icon, sizeof(bm), &bm);
                    SetStretchBltMode(d->hDC, HALFTONE);
                    StretchBlt(d->hDC, ic.left, ic.top, isz, isz,
                               mem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
                    SelectObject(mem, old);
                    DeleteDC(mem);
                } else {
                    /* Ersatzkachel mit Anfangsbuchstaben, Farbe aus dem Namen */
                    unsigned hash = 0;
                    for (const char *z = it->name; *z; z++)
                        hash = hash * 31u + (unsigned char)*z;
                    static const COLORREF pal[6] = {
                        RGB(96,165,250), RGB(74,222,128), RGB(167,139,250),
                        RGB(251,191,36), RGB(244,114,182), RGB(45,212,191) };
                    COLORREF ac = pal[hash % 6];
                    round_box(d->hDC, ic, S(10), mix(CCARD, ac, 22), mix(CCARD, ac, 45));

                    char ini[3] = { 0, 0, 0 };
                    const char *z = it->name;
                    while (*z == ' ') z++;
                    if (*z) ini[0] = *z;
                    const char *sp2 = strchr(z, ' ');
                    if (sp2 && sp2[1]) ini[1] = sp2[1];
                    for (int k = 0; k < 2; k++)
                        if (ini[k] >= 'a' && ini[k] <= 'z')
                            ini[k] = (char)(ini[k] - 'a' + 'A');
                    SelectObject(d->hDC, gFontB);
                    SetTextColor(d->hDC, ac);
                    DrawTextA(d->hDC, ini, -1, &ic,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                }

                int tx = ic.right + S(14);
                int ty = card.top + S(10);

                /* Titel + Autor */
                SelectObject(d->hDC, gFontB);
                SIZE ts;
                GetTextExtentPoint32A(d->hDC, it->name, (int)strlen(it->name), &ts);
                RECT tr = { tx, ty, card.right - S(150), ty + S(20) };
                SetTextColor(d->hDC, CTEXT);
                DrawTextA(d->hDC, it->name, -1, &tr,
                          DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                if (it->author[0] && tx + ts.cx + S(8) < card.right - S(160)) {
                    char by[96];
                    snprintf(by, sizeof(by), "by %s", it->author);
                    SelectObject(d->hDC, gFontSm);
                    RECT ar = { tx + ts.cx + S(8), ty + S(2),
                                card.right - S(150), ty + S(20) };
                    SetTextColor(d->hDC, CMUTED);
                    DrawTextA(d->hDC, by, -1, &ar,
                              DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                }

                /* Beschreibung, zwei Zeilen */
                if (it->desc[0]) {
                    SelectObject(d->hDC, gFont);
                    RECT dr = { tx, ty + S(21), card.right - S(150), ty + S(58) };
                    SetTextColor(d->hDC, CDIM);
                    DrawTextA(d->hDC, it->desc, -1, &dr,
                              DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
                }

                /* Tags als Pillen */
                SelectObject(d->hDC, gFontSm);
                int px = tx, py = card.bottom - S(28);
                for (int t = 0; t < it->ntags; t++) {
                    SIZE ps;
                    GetTextExtentPoint32A(d->hDC, it->tags[t],
                                          (int)strlen(it->tags[t]), &ps);
                    int pw = ps.cx + S(16);
                    if (px + pw > card.right - S(150)) break;
                    RECT pr = { px, py, px + pw, py + S(20) };
                    round_box(d->hDC, pr, S(10), mix(CCARD, CTEXT, 7),
                              mix(CCARD, CTEXT, 14));
                    SetTextColor(d->hDC, CMUTED);
                    DrawTextA(d->hDC, it->tags[t], -1, &pr,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                    px += pw + S(6);
                }

                /* rechts: Downloads, Favoriten, Datum */
                char dl[32];
                if (it->mods >= 1000000)      snprintf(dl, sizeof(dl), "%.2fM", it->mods / 1000000.0);
                else if (it->mods >= 1000)    snprintf(dl, sizeof(dl), "%.1fK", it->mods / 1000.0);
                else                          snprintf(dl, sizeof(dl), "%d", it->mods);
                char stats[80];
                snprintf(stats, sizeof(stats), "%s Downloads", dl);
                RECT sr = { card.right - S(146), card.top + S(12),
                            card.right - S(12), card.top + S(30) };
                SelectObject(d->hDC, gFontB);
                SetTextColor(d->hDC, CDIM);
                DrawTextA(d->hDC, stats, -1, &sr,
                          DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

                if (it->follows > 0) {      /* CurseForge liefert das nicht */
                    char fav[48];
                    snprintf(fav, sizeof(fav), "%d followers", it->follows);
                    RECT fr = { card.right - S(146), card.top + S(32),
                                card.right - S(12), card.top + S(50) };
                    SelectObject(d->hDC, gFontSm);
                    SetTextColor(d->hDC, CMUTED);
                    DrawTextA(d->hDC, fav, -1, &fr,
                              DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
                }

                if (it->updated[0]) {
                    RECT ur = { card.right - S(146), card.bottom - S(26),
                                card.right - S(12), card.bottom - S(8) };
                    SetTextColor(d->hDC, CMUTED);
                    DrawTextA(d->hDC, it->updated, -1, &ur,
                              DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
                }
                goto lb_done;
            }

            /* ---- kompakte Zeile: installierte Packs und Versionen ---- */
            HBRUSH bg = CreateSolidBrush(sel ? mix(CCARD, CACC, 16) : CCARD);
            FillRect(d->hDC, &r, bg);
            DeleteObject(bg);
            if (sel) {
                RECT b = { r.left, r.top + S(4), r.left + S(3), r.bottom - S(4) };
                HBRUSH ab = CreateSolidBrush(CACC);
                FillRect(d->hDC, &b, ab);
                DeleteObject(ab);
            }

            char right[96];
            const char *left;
            if (gPickMode == 2) {
                snprintf(right, sizeof(right), "MC %s  \x95  %s",
                         gVer[idx].mc, gVer[idx].loader);
                left = gVer[idx].name;
            } else {
                snprintf(right, sizeof(right), "%d Mods  \x95  %s",
                         gInst[idx].mods, gInst[idx].launcher);
                left = gInst[idx].name;
            }
            SelectObject(d->hDC, gFontSm);
            SIZE sz;
            GetTextExtentPoint32A(d->hDC, right, (int)strlen(right), &sz);
            RECT rr = { r.right - S(12) - sz.cx, r.top, r.right - S(12), r.bottom };
            SetTextColor(d->hDC, CMUTED);
            DrawTextA(d->hDC, right, -1, &rr,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            RECT rl = { r.left + S(14), r.top, rr.left - S(10), r.bottom };
            SelectObject(d->hDC, gFont);
            SetTextColor(d->hDC, sel ? CTEXT : CDIM);
            DrawTextA(d->hDC, left, -1, &rl,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE |
                      DT_END_ELLIPSIS | DT_NOPREFIX);

        lb_done:
            BitBlt(realDC, realR.left, realR.top, bw, bh, mdc, 0, 0, SRCCOPY);
            SelectObject(mdc, oldBm);
            DeleteObject(mbm);
            DeleteDC(mdc);
            d->hDC = realDC;
            d->rcItem = realR;
            return TRUE;
        }

        if (d->CtlType == ODT_BUTTON) {
            int primary = ((int)d->CtlID == ID_PICK_SERVER);
            int pressed = (d->itemState & ODS_SELECTED) != 0;
            int dis     = (d->itemState & ODS_DISABLED) != 0;
            int hot     = (int)GetWindowLongPtrA(d->hwndItem, GWLP_USERDATA);
            FillRect(d->hDC, &d->rcItem, gbrBg);
            COLORREF fill, border, text;
            if (dis) {
                fill = mix(CBG, CINPUT, 55);
                border = mix(CBG, CBORDER, 60);
                text = RGB(108, 108, 118);
            } else if (primary) {
                fill = pressed ? CACCP : (hot ? CACCH : CACC);
                border = fill;
                text = RGB(14, 20, 30);
            } else {
                fill = (pressed || hot) ? CINPUTH : CINPUT;
                border = hot ? mix(CBORDER, CTEXT, 18) : CBORDER;
                text = CTEXT;
            }
            RECT b = d->rcItem;
            round_box(d->hDC, b, S(8), fill, border);
            char txt[80];
            GetWindowTextA(d->hwndItem, txt, sizeof(txt));
            SetBkMode(d->hDC, TRANSPARENT);
            SelectObject(d->hDC, gFontB);
            SetTextColor(d->hDC, text);
            DrawTextA(d->hDC, txt, -1, &b,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            return TRUE;
        }
        break;
    }

    case WM_SIZE:
        pick_layout(hwnd);
        return 0;

    case WM_COMMAND:
        if (HIWORD(wp) == LBN_DBLCLK && LOWORD(wp) == ID_PICK_LIST) {
            pick_finish(hwnd, -1);        /* Absicht des vorherigen Schritts behalten */
            return 0;
        }
        if (LOWORD(wp) == ID_PICK_SEARCH &&
            (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS)) {
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        switch (LOWORD(wp)) {
        case ID_PICK_GO: {
            char q[256];
            GetWindowTextA(gPickSearch, q, sizeof(q));
            char *s = q;
            while (*s == ' ') s++;
            /* leeres Feld UND kein Quellenfilter -> installierte Packs zeigen.
             * Mit gewaehlter Quelle wird auch ohne Begriff gestoebert. */
            if (!*s && gSrcFilter == 0) {
                gPickMode = 0;
                enum_instances();
            } else {
                strncpy(gQuery, s, sizeof(gQuery) - 1);
                gQuery[sizeof(gQuery) - 1] = 0;
                SetCursor(LoadCursor(NULL, IDC_WAIT));
                do_search(hwnd, 0);
                SetCursor(LoadCursor(NULL, IDC_ARROW));
                gPickMode = 1;
            }
            pick_fill(hwnd);
            return 0;
        }
        case ID_PICK_MORE:
            if (gPickMode == 1) {
                SetCursor(LoadCursor(NULL, IDC_WAIT));
                do_search(hwnd, 1);
                SetCursor(LoadCursor(NULL, IDC_ARROW));
                pick_fill(hwnd);
            }
            return 0;
        case ID_PICK_SRC:
            gSrcFilter = (gSrcFilter + 1) % 3;
            SetWindowTextA(GetDlgItem(hwnd, ID_PICK_SRC), src_label());
            if (gPickMode == 1 || gSrcFilter != 0)
                SendMessageA(hwnd, WM_COMMAND, MAKEWPARAM(ID_PICK_GO, 0), 0);
            return 0;
        case ID_PICK_SORT:
            gSortMode = (gSortMode + 1) % 3;
            SetWindowTextA(GetDlgItem(hwnd, ID_PICK_SORT), sort_label());
            if (gPickMode == 1)
                SendMessageA(hwnd, WM_COMMAND, MAKEWPARAM(ID_PICK_GO, 0), 0);
            return 0;
        case ID_PICK_BACK:
            if (gPickMode == 2) {            /* zurueck zur Trefferliste */
                gPickMode = 1;
                pick_fill(hwnd);
            } else if (gPickMode == 1) {     /* zurueck zu den installierten */
                gPickMode = 0;
                SetWindowTextA(gPickSearch, "");
                enum_instances();
                pick_fill(hwnd);
            }
            return 0;
        case ID_PICK_LOAD:   pick_finish(hwnd, 0); return 0;
        case ID_PICK_SERVER: pick_finish(hwnd, 1); return 0;
        case ID_PICK_CANCEL: DestroyWindow(hwnd);  return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        gPickWnd = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* Modales Auswahlfenster; Rueckgabe: Index in gInst oder -1 */
static int pick_modpack(HWND owner, int *alsoServer)
{
    static int reg = 0;
    if (!reg) {
        WNDCLASSA wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc = PickProc;
        wc.hInstance = (HINSTANCE)GetWindowLongPtrA(owner, GWLP_HINSTANCE);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = gbrBg;
        wc.lpszClassName = "ModSorterPick";
        RegisterClassA(&wc);
        reg = 1;
    }

    enum_instances();
    gPickResult = -1;
    gPickServer = 0;
    gPickMode = 0;
    gPickOnlineResult = 0;
    gPickUrl[0] = 0;

    RECT orc;
    GetWindowRect(owner, &orc);
    int W = S(920), H = S(640);
    int x = orc.left + ((orc.right - orc.left) - W) / 2;
    int y = orc.top + ((orc.bottom - orc.top) - H) / 2;

    gPickWnd = CreateWindowExA(WS_EX_DLGMODALFRAME, "ModSorterPick",
                               "Choose modpack",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                               x, y, W, H, owner, NULL,
                               (HINSTANCE)GetWindowLongPtrA(owner, GWLP_HINSTANCE),
                               NULL);
    if (!gPickWnd)
        return -1;

    BOOL dark = TRUE;
    DwmSetWindowAttribute(gPickWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    gPickList = CreateWindowExA(0, "LISTBOX", "",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                                LBS_NOINTEGRALHEIGHT | LBS_HASSTRINGS |
                                LBS_OWNERDRAWVARIABLE | LBS_NOTIFY,
                                0, 0, 10, 10, gPickWnd,
                                (HMENU)ID_PICK_LIST, NULL, NULL);
    SendMessageA(gPickList, WM_SETFONT, (WPARAM)gFont, TRUE);
    SetWindowTheme(gPickList, L"DarkMode_Explorer", NULL);

    gPickSearch = CreateWindowExA(0, "EDIT", "",
                                  WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                  0, 0, 10, 10, gPickWnd,
                                  (HMENU)ID_PICK_SEARCH, NULL, NULL);
    SendMessageA(gPickSearch, WM_SETFONT, (WPARAM)gFont, TRUE);
    SendMessageA(gPickSearch, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(S(10), S(10)));
    SetWindowTheme(gPickSearch, L"DarkMode_CFD", NULL);
    gSearchOrig = (WNDPROC)SetWindowLongPtrA(gPickSearch, GWLP_WNDPROC,
                                             (LONG_PTR)SearchProc);

    struct { int id; const char *t; } btns[8] = {
        { ID_PICK_CANCEL, "Cancel" },
        { ID_PICK_BACK,   "Back" },
        { ID_PICK_LOAD,   "Load" },
        { ID_PICK_SERVER, "Create server pack ..." },
        { ID_PICK_GO,     "Search" },
        { ID_PICK_SRC,    src_label() },
        { ID_PICK_SORT,   sort_label() },
        { ID_PICK_MORE,   "More results" },
    };
    for (int i = 0; i < 8; i++) {
        HWND b = CreateWindowA("BUTTON", btns[i].t,
                               WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                               0, 0, 10, 10, gPickWnd,
                               (HMENU)(INT_PTR)btns[i].id, NULL, NULL);
        SendMessageA(b, WM_SETFONT, (WPARAM)gFontB, TRUE);
        SetWindowLongPtrA(b, GWLP_WNDPROC, (LONG_PTR)BtnProc);
    }

    pick_fill(gPickWnd);
    pick_layout(gPickWnd);
    ShowWindow(gPickWnd, SW_SHOW);
    SetFocus(gPickSearch);

    EnableWindow(owner, FALSE);
    MSG msg;
    while (gPickWnd && GetMessageA(&msg, NULL, 0, 0)) {
        if (msg.message == WM_QUIT) {
            PostQuitMessage((int)msg.wParam);
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);

    if (alsoServer)
        *alsoServer = gPickServer;
    if (gPickOnlineResult)
        return -2;                  /* Online-Pack gewaehlt, URL in gPickUrl */
    return gPickResult;
}

/* Verzeichnisse fuer einen Dateipfad anlegen (mods/x/y.jar -> mods, mods/x) */
static void ensure_dirs(const char *full)
{
    char t[MAX_PATH];
    strncpy(t, full, MAX_PATH - 1);
    t[MAX_PATH - 1] = 0;
    for (char *p = t; *p; p++) {
        if (*p == '/') *p = '\\';
    }
    for (char *p = t + 1; *p; p++) {
        if (*p == '\\') {
            *p = 0;
            CreateDirectoryA(t, NULL);
            *p = '\\';
        }
    }
}

/* Ein Modrinth-Modpack (.mrpack) herunterladen und in dest entpacken,
 * sodass ein ganz normaler Modpack-Ordner mit mods\ und config\ entsteht. */
static int download_mrpack(HWND hwnd, const char *url, const char *dest)
{
    CreateDirectoryA(dest, NULL);
    char pack[MAX_PATH];
    snprintf(pack, sizeof(pack), "%s\\pack.mrpack", dest);

    prog_begin("Downloading modpack ...");
    snprintf(gLabel, sizeof(gLabel), "Modpack archive");
    if (!download_url(url, pack)) {
        gLabel[0] = 0;
        prog_end(gCancel ? "Cancelled." : "Error: could not download the modpack");
        return 0;
    }
    gLabel[0] = 0;

    /* overrides/ enthaelt Konfiguration und mitgelieferte Dateien */
    prog_set("Extracting modpack ...", -1.0, 1);
    char cmd[1400];
    snprintf(cmd, sizeof(cmd),
             "tar.exe -xf \"%s\" -C \"%s\" overrides server-overrides", pack, dest);
    run_wait(cmd, dest, 300000);
    char ov[MAX_PATH], so[MAX_PATH];
    snprintf(ov, sizeof(ov), "%s\\overrides", dest);
    snprintf(so, sizeof(so), "%s\\server-overrides", dest);
    if (GetFileAttributesA(ov) != INVALID_FILE_ATTRIBUTES) {
        copy_tree(ov, dest);
        snprintf(cmd, sizeof(cmd), "cmd.exe /c rmdir /s /q \"%s\"", ov);
        run_wait(cmd, dest, 60000);
    }
    if (GetFileAttributesA(so) != INVALID_FILE_ATTRIBUTES) {
        copy_tree(so, dest);
        snprintf(cmd, sizeof(cmd), "cmd.exe /c rmdir /s /q \"%s\"", so);
        run_wait(cmd, dest, 60000);
    }

    /* Index lesen und alle Dateien laden */
    snprintf(cmd, sizeof(cmd), "tar.exe -xOf \"%s\" modrinth.index.json", pack);
    char *idx = run_capture(cmd);
    if (!idx) {
        prog_end("Error: modrinth.index.json is missing from the pack");
        DeleteFileA(pack);
        return 0;
    }

    /* Anzahl der Dateien vorab zaehlen, damit der Balken stimmt */
    int fileCount = 0;
    for (const char *cp = idx; (cp = strstr(cp, "\"downloads\"")) != NULL; cp += 11)
        fileCount++;

    const char *e = idx + strlen(idx);
    const char *p = strstr(idx, "\"files\"");
    int total = 0, done = 0, failed = 0;
    if (p) {
        p = strchr(p, '[');
        if (p) p++;
        while (p && p < e) {
            while (p < e && *p != '{' && *p != ']') p++;
            if (p >= e || *p == ']') break;
            const char *o = p;
            int depth = 0, instr = 0;
            for (; p < e; p++) {
                char ch = *p;
                if (instr) { if (ch == '\\') p++; else if (ch == '\"') instr = 0; }
                else { if (ch == '\"') instr = 1; else if (ch == '{') depth++;
                       else if (ch == '}') { depth--; if (depth == 0) { p++; break; } } }
            }
            /* Pfad und erste Download-URL aus dem Eintrag holen.
             * "downloads" ist ein Array; json_str_array kuerzt zu stark,
             * daher hier direkt den ersten String lesen. */
            char rel[400], durl[900];
            rel[0] = 0;
            durl[0] = 0;
            json_str(o, p, "path", rel, sizeof(rel));
            const char *dp = strstr(o, "\"downloads\"");
            if (dp && dp < p) {
                const char *q = strchr(dp, '[');
                if (q) q = strchr(q, '\"');
                if (q) {
                    q++;
                    int i = 0;
                    while (q < p && *q != '\"' && i < (int)sizeof(durl) - 1)
                        durl[i++] = *q++;
                    durl[i] = 0;
                }
            }
            if (rel[0] && durl[0]) {
                total++;
                char full[MAX_PATH];
                snprintf(full, sizeof(full), "%s\\%s", dest, rel);
                for (char *z = full; *z; z++)
                    if (*z == '/') *z = '\\';
                ensure_dirs(full);

                const char *base = strrchr(rel, '/');
                base = base ? base + 1 : rel;
                snprintf(gLabel, sizeof(gLabel), "[%d/%d] %s",
                         total, fileCount ? fileCount : total, base);
                prog_set(gLabel, fileCount ? (double)(total - 1) / fileCount : -1.0, 1);

                if (download_url(durl, full)) done++; else failed++;
                if (gCancel) break;
            }
            while (p < e && *p != ',' && *p != ']') p++;
            if (p < e && *p == ',') p++;
        }
    }
    free(idx);
    DeleteFileA(pack);
    gLabel[0] = 0;

    char st[220];
    if (gCancel)
        snprintf(st, sizeof(st), "Cancelled - %d of %d files downloaded", done, total);
    else
        snprintf(st, sizeof(st), "Modpack downloaded: %d of %d files%s",
                 done, total, failed ? " (some failed)" : "");
    prog_end(st);
    return !gCancel && done > 0;
}

/* Ein CurseForge-Modpack laden und in dest auspacken.
 * Format: ZIP mit manifest.json (Liste aus projectID/fileID) und overrides\.
 * Die Download-Links muessen einzeln aufgeloest werden; wo der Autor die
 * Weitergabe gesperrt hat, liefert die API keinen Link - solche Dateien
 * werden uebersprungen und am Ende aufgelistet. */
static int download_cfpack(HWND hwnd, const char *url, const char *dest,
                           char *blockedOut, int blockedSz, int *blockedN)
{
    (void)hwnd;
    CreateDirectoryA(dest, NULL);
    char pack[MAX_PATH];
    snprintf(pack, sizeof(pack), "%s\\pack.zip", dest);

    prog_begin("Downloading modpack ...");
    snprintf(gLabel, sizeof(gLabel), "Modpack archive");
    if (!download_url(url, pack)) {
        gLabel[0] = 0;
        prog_end(gCancel ? "Cancelled." : "Error: could not download the modpack");
        return 0;
    }
    gLabel[0] = 0;

    prog_set("Extracting modpack ...", -1.0, 1);
    char cmd[1400];
    snprintf(cmd, sizeof(cmd), "tar.exe -xf \"%s\" -C \"%s\" overrides", pack, dest);
    run_wait(cmd, dest, 300000);
    char ov[MAX_PATH];
    snprintf(ov, sizeof(ov), "%s\\overrides", dest);
    if (GetFileAttributesA(ov) != INVALID_FILE_ATTRIBUTES) {
        copy_tree(ov, dest);
        snprintf(cmd, sizeof(cmd), "cmd.exe /c rmdir /s /q \"%s\"", ov);
        run_wait(cmd, dest, 60000);
    }

    snprintf(cmd, sizeof(cmd), "tar.exe -xOf \"%s\" manifest.json", pack);
    char *man = run_capture(cmd);
    if (!man) {
        prog_end("Error: manifest.json is missing from the pack");
        DeleteFileA(pack);
        return 0;
    }

    /* projectID/fileID-Paare einsammeln */
    int cap = 256, n = 0;
    int (*ids)[2] = malloc(sizeof(*ids) * cap);
    const char *e = man + strlen(man);
    for (const char *p = man; (p = strstr(p, "\"projectID\"")) != NULL; ) {
        int pid = 0, fid = 0;
        const char *q = p + 11;
        while (q < e && (*q == ' ' || *q == ':')) q++;
        pid = atoi(q);
        const char *fp = strstr(q, "\"fileID\"");
        if (fp) {
            q = fp + 8;
            while (q < e && (*q == ' ' || *q == ':')) q++;
            fid = atoi(q);
        }
        if (pid && fid) {
            if (n >= cap) { cap *= 2; ids = realloc(ids, sizeof(*ids) * cap); }
            ids[n][0] = pid; ids[n][1] = fid; n++;
        }
        p = fp ? fp + 8 : p + 11;
    }
    free(man);

    char mods[MAX_PATH];
    snprintf(mods, sizeof(mods), "%s\\mods", dest);
    CreateDirectoryA(mods, NULL);

    int done = 0, blocked = 0;
    if (blockedN) *blockedN = 0;
    if (blockedOut && blockedSz) blockedOut[0] = 0;

    for (int i = 0; i < n && !gCancel; i++) {
        /* Datei-Info holen: Name und Download-Link */
        char path[128];
        snprintf(path, sizeof(path), "/v1/mods/%d/files/%d", ids[i][0], ids[i][1]);
        DWORD rl = 0;
        char *fr = cf_request(path, NULL, &rl);
        if (!fr) continue;

        char fname[200] = "", furl[900] = "";
        json_str(fr, fr + rl, "fileName", fname, sizeof(fname));
        json_str(fr, fr + rl, "downloadUrl", furl, sizeof(furl));
        free(fr);

        if (!fname[0]) continue;

        snprintf(gLabel, sizeof(gLabel), "[%d/%d] %s", i + 1, n, fname);
        prog_set(gLabel, (double)i / n, 1);

        if (!furl[0]) {                 /* Autor hat die Weitergabe gesperrt */
            blocked++;
            if (blockedOut && (int)strlen(blockedOut) + 220 < blockedSz) {
                char line[220];
                snprintf(line, sizeof(line), "  %s\n", fname);
                strcat(blockedOut, line);
            }
            continue;
        }
        char full[MAX_PATH];
        snprintf(full, sizeof(full), "%s\\%s", mods, fname);
        if (download_url(furl, full)) done++;
    }
    free(ids);
    DeleteFileA(pack);
    gLabel[0] = 0;
    if (blockedN) *blockedN = blocked;

    char st[260];
    if (gCancel)
        snprintf(st, sizeof(st), "Cancelled - %d of %d mods downloaded", done, n);
    else
        snprintf(st, sizeof(st), "Modpack downloaded: %d of %d mods%s",
                 done, n, blocked ? " (some blocked by their authors)" : "");
    prog_end(st);
    return !gCancel && done > 0;
}

static void choose_folder(HWND hwnd)
{
    /* Dialog dort oeffnen, wo der aktuelle Pfad hinzeigt */
    char cur[MAX_PATH];
    GetWindowTextA(gEditPath, cur, sizeof(cur));

    char path[MAX_PATH];
    if (pick_folder(hwnd, "Choose mods folder", cur, path, sizeof(path))) {
        strncpy(gFolder, path, MAX_PATH - 1);
        gFolder[MAX_PATH - 1] = 0;
        SetWindowTextA(gEditPath, gFolder);
        scan_folder(hwnd);
    }
}

/* -- Pfad aus dem Eingabefeld uebernehmen, pruefen und scannen -- */
static void scan_from_edit(HWND hwnd)
{
    char buf[MAX_PATH];
    GetWindowTextA(gEditPath, buf, sizeof(buf));

    /* fuehrende/anhaengende Leerzeichen und Anfuehrungszeichen entfernen */
    char *s = buf;
    while (*s == ' ' || *s == '\t' || *s == '\"')
        s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\"' || s[n - 1] == '\\'))
        s[--n] = 0;

    if (*s == 0) {
        SetWindowTextA(gLblStatus, "Please enter or choose a path.");
        return;
    }
    DWORD attr = GetFileAttributesA(s);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        SetWindowTextA(gLblStatus,
                       "Folder not found - please enter a valid path.");
        return;
    }
    strncpy(gFolder, s, MAX_PATH - 1);
    gFolder[MAX_PATH - 1] = 0;
    scan_folder(hwnd);
}

/* -- Steuerelemente positionieren -- */
static int      list_index(HWND h);      /* fwd */
static COLORREF list_accent(int i);      /* fwd */

static void layout(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    int m = S(16), gap = S(8), btnH = S(38);

    /* --- obere Leiste: Modpack-Auswahl, Pfadfeld, dann Aktionen --- */
    int bPick = S(170), bBrowse = S(142), bScan = S(104);
    int ew = W - 2 * m - bPick - bBrowse - bScan - 3 * gap;
    if (ew < S(120)) ew = S(120);
    int x0 = m;
    MoveWindow(gBtnPick, x0, m, bPick, btnH, TRUE);        x0 += bPick + gap;

    /* Rahmen so hoch wie die Buttons, das Feld selbst nur so hoch wie der
     * Text und darin mittig - ein einzeiliges EDIT setzt seinen Text sonst
     * immer nach oben. */
    gEditBox.left = x0;  gEditBox.top = m;
    gEditBox.right = x0 + ew;  gEditBox.bottom = m + btnH;
    int eh = gTextH + S(4);
    MoveWindow(gEditPath, x0 + S(2), m + (btnH - eh) / 2, ew - S(4), eh, TRUE);
    x0 += ew + gap;
    MoveWindow(gBtnChoose, x0, m, bBrowse, btnH, TRUE);    x0 += bBrowse + gap;
    MoveWindow(gBtnScan, x0, m, bScan, btnH, TRUE);

    /* --- untere Leiste --- */
    int by = H - m - btnH;
    gBarY = by - S(14);
    int bSrv = S(206), bCopy = S(228);
    MoveWindow(gBtnServer, W - m - bSrv, by, bSrv, btnH, TRUE);
    MoveWindow(gBtnCopy, W - m - bSrv - gap - bCopy, by, bCopy, btnH, TRUE);
    int stw = W - m - bSrv - gap - bCopy - gap - m;
    if (stw < S(40)) stw = S(40);
    MoveWindow(gLblStatus, m, by + (btnH - S(18)) / 2, stw, S(18), TRUE);

    /* --- vier Karten --- */
    gHdrH = S(42);
    int cardTop = m + btnH + S(18);
    int cardH = gBarY - S(14) - cardTop;
    if (cardH < S(90)) cardH = S(90);

    int colGap = S(12);
    int colW = (W - 2 * m - 3 * colGap) / 4;
    HWND lists[4] = { gListC, gListS, gListB, gListU };
    int x = m;
    for (int i = 0; i < 4; i++) {
        gCard[i].left = x;
        gCard[i].top = cardTop;
        gCard[i].right = x + colW;
        gCard[i].bottom = cardTop + cardH;
        /* Liste sitzt innerhalb der Karte, Rand + runde Ecken bleiben sichtbar */
        MoveWindow(lists[i], x + S(2), cardTop + gHdrH,
                   colW - S(4), cardH - gHdrH - S(3), TRUE);
        x += colW + colGap;
    }
}

/* Kopfbereich einer Karte zeichnen: Punkt, Titel, Zahl-Pille, Trennlinie */
static void draw_card(HDC dc, int i, const char *title)
{
    RECT c = gCard[i];
    COLORREF acc = list_accent(i);

    round_box(dc, c, S(10), CCARD, CBORDER);

    int cy = c.top + gHdrH / 2;
    dot(dc, c.left + S(17), cy, S(4), acc);

    /* Zahl rechts als dezente Pille in der Kategoriefarbe */
    char num[16];
    snprintf(num, sizeof(num), "%d", gCnt[i]);
    SelectObject(dc, gFontSm);
    SIZE sz;
    GetTextExtentPoint32A(dc, num, (int)strlen(num), &sz);
    int pw = sz.cx + S(18);
    if (pw < S(34)) pw = S(34);
    RECT pill = { c.right - S(12) - pw, cy - S(11), c.right - S(12), cy + S(11) };
    round_box(dc, pill, S(11), mix(CCARD, acc, 18), mix(CCARD, acc, 30));
    SetTextColor(dc, acc);
    DrawTextA(dc, num, -1, &pill, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RECT t = { c.left + S(30), c.top, pill.left - S(8), c.top + gHdrH };
    SelectObject(dc, gFontB);
    SetTextColor(dc, CTEXT);
    DrawTextA(dc, title, -1, &t,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    hline(dc, c.left + S(1), c.right - S(1), c.top + gHdrH - 1, CBORDER);
}

static HWND mk_static(HWND parent, int id, const char *txt, HFONT f)
{
    HWND h = CreateWindowA("STATIC", txt, WS_CHILD | WS_VISIBLE | SS_LEFT,
                           0, 0, 10, 10, parent, (HMENU)(INT_PTR)id,
                           NULL, NULL);
    SendMessageA(h, WM_SETFONT, (WPARAM)f, TRUE);
    return h;
}

static HWND mk_list(HWND parent, int id)
{
    /* kein WS_BORDER: der Rahmen kommt von der Karte darunter.
     * LBS_OWNERDRAWFIXED -> Eintraege werden selbst gezeichnet (Padding,
     * eigene Auswahlfarbe, Ellipsis). DarkMode_Explorer faerbt die Scrollbar. */
    HWND h = CreateWindowExA(0, "LISTBOX", "",
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                             LBS_NOINTEGRALHEIGHT | LBS_HASSTRINGS |
                             LBS_OWNERDRAWFIXED | LBS_NOTIFY,
                             0, 0, 10, 10, parent, (HMENU)(INT_PTR)id,
                             NULL, NULL);
    SendMessageA(h, WM_SETFONT, (WPARAM)gFont, TRUE);
    SetWindowTheme(h, L"DarkMode_Explorer", NULL);
    gListOrigProc = (WNDPROC)SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)ListProc);
    return h;
}

static HWND mk_button(HWND parent, int id, const char *txt)
{
    HWND h = CreateWindowA("BUTTON", txt,
                           WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                           0, 0, 10, 10, parent, (HMENU)(INT_PTR)id,
                           NULL, NULL);
    SendMessageA(h, WM_SETFONT, (WPARAM)gFontB, TRUE);
    gBtnOrigProc = (WNDPROC)SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)BtnProc);
    return h;
}

/* Akzentfarbe / Index einer Liste */
static int list_index(HWND h)
{
    if (h == gListC) return 0;
    if (h == gListS) return 1;
    if (h == gListB) return 2;
    return 3;
}
static COLORREF list_accent(int i)
{
    static const COLORREF a[4] = { RGB(96,165,250), RGB(74,222,128),
                                   RGB(167,139,250), RGB(251,191,36) };
    return a[i];
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        int fh  = -MulDiv(10, gDpi, 72);
        int fhs = -MulDiv(9,  gDpi, 72);
        gFont   = CreateFontA(fh, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
        gFontB  = CreateFontA(fh, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                              0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
        gFontSm = CreateFontA(fhs, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                              0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");

        gMainWnd = hwnd;

        {   /* Zeilenhoehe der Schrift fuer die Feldhoehe */
            HDC dc = GetDC(hwnd);
            HGDIOBJ of = SelectObject(dc, gFont);
            TEXTMETRICA tm;
            GetTextMetricsA(dc, &tm);
            gTextH = tm.tmHeight;
            SelectObject(dc, of);
            ReleaseDC(hwnd, dc);
        }

        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                              &dark, sizeof(dark));

        gBtnPick   = mk_button(hwnd, ID_BTN_PICK,   "Choose modpack ...");
        gBtnChoose = mk_button(hwnd, ID_BTN_CHOOSE, "Browse ...");
        gBtnScan   = mk_button(hwnd, ID_BTN_SCAN,   "Scan");
        gBtnCopy   = mk_button(hwnd, ID_BTN_COPY,   "Copy to client\\ + server\\");
        gBtnServer = mk_button(hwnd, ID_BTN_SERVER, "Create server pack ...");
        EnableWindow(gBtnCopy, FALSE);
        EnableWindow(gBtnServer, FALSE);

        gEditPath = CreateWindowExA(0, "EDIT", "",
                                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                    0, 0, 10, 10, hwnd,
                                    (HMENU)ID_EDIT_PATH, NULL, NULL);
        SendMessageA(gEditPath, WM_SETFONT, (WPARAM)gFont, TRUE);
        SendMessageA(gEditPath, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(S(10), S(10)));
        SetWindowTheme(gEditPath, L"DarkMode_CFD", NULL);
        gEditOrigProc = (WNDPROC)SetWindowLongPtrA(gEditPath, GWLP_WNDPROC,
                                                   (LONG_PTR)EditProc);

        gLblStatus = mk_static(hwnd, ID_LBL_STATUS,
                               "Enter a mods folder and press Enter \x95 or use Browse",
                               gFont);

        gListC = mk_list(hwnd, ID_LIST_CLIENT);
        gListS = mk_list(hwnd, ID_LIST_SERVER);
        gListB = mk_list(hwnd, ID_LIST_BOTH);
        gListU = mk_list(hwnd, ID_LIST_UNKNOWN);

        layout(hwnd);

        if (gMrpackUrl[0] && gServerOut[0]) {
            /* Online-Batch: Pack laden, scannen, Server bauen */
            char packDir[MAX_PATH], srvDir[MAX_PATH];
            CreateDirectoryA(gServerOut, NULL);
            snprintf(packDir, sizeof(packDir), "%s\\modpack", gServerOut);
            snprintf(srvDir, sizeof(srvDir), "%s\\server", gServerOut);
            static char blk[4000];
            int nblk = 0;
            int got = gPackIsCf
                ? download_cfpack(hwnd, gMrpackUrl, packDir, blk, sizeof(blk), &nblk)
                : download_mrpack(hwnd, gMrpackUrl, packDir);
            if (got) {
                snprintf(gFolder, MAX_PATH, "%s\\mods", packDir);
                SetWindowTextA(gEditPath, gFolder);
                scan_folder(hwnd);
                build_server_pack(hwnd, srvDir, 1);
            }
            if (gAutoQuit)
                PostMessageA(hwnd, WM_CLOSE, 0, 0);
        } else if (gStartPath[0]) {        /* Ordner von der Kommandozeile */
            SetWindowTextA(gEditPath, gStartPath);
            PostMessageA(hwnd, WM_COMMAND, MAKEWPARAM(ID_BTN_SCAN, 0), 0);
        }
        return 0;
    }

    case WM_SIZE:
        layout(hwnd);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = S(1000);
        mm->ptMinTrackSize.y = S(460);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;                       /* alles in WM_PAINT, verhindert Flackern */

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, gbrBg);
        SetBkMode(dc, TRANSPARENT);

        /* Rahmen um das Eingabefeld (es hat selbst keinen mehr) */
        round_box(dc, gEditBox, S(8), CINPUT,
                  GetFocus() == gEditPath ? CACC : CBORDER);

        static const char *titles[4] = { "Client-only", "Server-only",
                                         "Server / Client", "Unknown" };
        for (int i = 0; i < 4; i++)
            draw_card(dc, i, titles[i]);

        hline(dc, S(16), rc.right - S(16), gBarY, CBORDER);

        /* Fortschrittsbalken direkt ueber der unteren Leiste */
        if (gBusy) {
            RECT tr = { S(16), gBarY - S(9), rc.right - S(16), gBarY - S(3) };
            round_box(dc, tr, S(3), mix(CBG, CTEXT, 8), mix(CBG, CTEXT, 12));
            if (gProgFrac >= 0.0) {
                double f = gProgFrac > 1.0 ? 1.0 : gProgFrac;
                int w = (int)((tr.right - tr.left) * f);
                if (w > S(4)) {
                    RECT fr = { tr.left, tr.top, tr.left + w, tr.bottom };
                    round_box(dc, fr, S(3), CACC, CACC);
                }
            } else {
                /* unbestimmt: laufender Balken */
                int span = tr.right - tr.left;
                int w = span / 5;
                int pos = (int)((GetTickCount() / 6) % (DWORD)(span + w)) - w;
                int l = tr.left + (pos < 0 ? 0 : pos);
                int r2 = tr.left + pos + w;
                if (r2 > tr.right) r2 = tr.right;
                if (r2 > l) {
                    RECT fr = { l, tr.top, r2, tr.bottom };
                    round_box(dc, fr, S(3), CACC, CACC);
                }
            }
        }

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, CDIM);
        SetBkColor(dc, CCARD);
        return (LRESULT)gbrCard;        /* Flaeche unter dem letzten Eintrag */
    }

    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, CTEXT);
        SetBkColor(dc, CINPUT);
        return (LRESULT)gbrInput;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, CMUTED);
        SetBkColor(dc, CBG);
        return (LRESULT)gbrBg;
    }

    case WM_MEASUREITEM: {
        LPMEASUREITEMSTRUCT mi = (LPMEASUREITEMSTRUCT)lp;
        if (mi->CtlType == ODT_LISTBOX)
            mi->itemHeight = S(26);
        return TRUE;
    }

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT d = (LPDRAWITEMSTRUCT)lp;

        /* ---- Listeneintrag ---- */
        if (d->CtlType == ODT_LISTBOX) {
            if ((int)d->itemID < 0)
                return TRUE;
            int idx = list_index(d->hwndItem);
            COLORREF acc = list_accent(idx);
            int sel = (d->itemState & ODS_SELECTED) != 0;

            RECT r = d->rcItem;
            HBRUSH bg = CreateSolidBrush(sel ? mix(CCARD, acc, 16) : CCARD);
            FillRect(d->hDC, &r, bg);
            DeleteObject(bg);

            if (sel) {                  /* Akzentbalken am linken Rand */
                RECT b = { r.left, r.top + S(3), r.left + S(3), r.bottom - S(3) };
                HBRUSH ab = CreateSolidBrush(acc);
                FillRect(d->hDC, &b, ab);
                DeleteObject(ab);
            }

            char txt[MAX_PATH];
            txt[0] = 0;
            SendMessageA(d->hwndItem, LB_GETTEXT, d->itemID, (LPARAM)txt);
            r.left += S(12);
            r.right -= S(8);
            SetBkMode(d->hDC, TRANSPARENT);
            SelectObject(d->hDC, gFont);
            SetTextColor(d->hDC, sel ? CTEXT : CDIM);
            DrawTextA(d->hDC, txt, -1, &r, DT_LEFT | DT_VCENTER |
                      DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            return TRUE;
        }

        /* ---- Button ---- */
        if (d->CtlType == ODT_BUTTON) {
            int id      = (int)d->CtlID;
            int primary = (id == ID_BTN_SCAN || id == ID_BTN_SERVER);
            int pressed = (d->itemState & ODS_SELECTED) != 0;
            int dis     = (d->itemState & ODS_DISABLED) != 0;
            int hot     = (int)GetWindowLongPtrA(d->hwndItem, GWLP_USERDATA);

            FillRect(d->hDC, &d->rcItem, gbrBg);   /* fuer saubere runde Ecken */

            COLORREF fill, border, text;
            if (dis) {
                fill = mix(CBG, CINPUT, 55);
                border = mix(CBG, CBORDER, 60);
                text = RGB(108, 108, 118);
            } else if (primary) {
                fill = pressed ? CACCP : (hot ? CACCH : CACC);
                border = fill;
                text = RGB(14, 20, 30);            /* dunkle Schrift auf Blau */
            } else {
                fill = pressed ? CINPUTH : (hot ? CINPUTH : CINPUT);
                border = hot ? mix(CBORDER, CTEXT, 18) : CBORDER;
                text = CTEXT;
            }
            RECT b = d->rcItem;
            round_box(d->hDC, b, S(8), fill, border);

            char txt[80];
            GetWindowTextA(d->hwndItem, txt, sizeof(txt));
            SetBkMode(d->hDC, TRANSPARENT);
            SelectObject(d->hDC, gFontB);
            SetTextColor(d->hDC, text);
            DrawTextA(d->hDC, txt, -1, &b, DT_CENTER | DT_VCENTER |
                      DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            return TRUE;
        }
        break;
    }


    case WM_CLOSE:
        if (gBusy) {                  /* laufenden Download sauber abbrechen */
            gCancel = 1;
            return 0;
        }
        break;

    case WM_COMMAND:
        /* waehrend eines Downloads keine neuen Aktionen annehmen */
        if (gBusy && HIWORD(wp) == BN_CLICKED)
            return 0;
        /* Fokusrahmen ums Pfadfeld neu zeichnen */
        if (LOWORD(wp) == ID_EDIT_PATH &&
            (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS)) {
            RECT er = gEditBox;
            InflateRect(&er, S(3), S(3));
            InvalidateRect(hwnd, &er, FALSE);
            return 0;
        }
        switch (LOWORD(wp)) {
        case ID_BTN_PICK: {
            int alsoServer = 0;
            int sel = pick_modpack(hwnd, &alsoServer);

            if (sel == -2) {                 /* Online-Pack: erst herunterladen */
                char dest[MAX_PATH];
                if (!pick_folder(hwnd, alsoServer
                        ? "Choose a folder - 'modpack' and 'server' are created inside"
                        : "Choose a folder to download the modpack into",
                        NULL, dest, sizeof(dest)))
                    return 0;
                /* NUR den Pack-Namen bereinigen - nicht den ganzen Pfad,
                 * sonst wird der Doppelpunkt im Laufwerk zerstoert (C: -> C_) */
                char safe[160];
                strncpy(safe, gPickName, sizeof(safe) - 1);
                safe[sizeof(safe) - 1] = 0;
                for (char *z = safe; *z; z++)
                    if (*z == ':' || *z == '*' || *z == '?' || *z == '\"' ||
                        *z == '<' || *z == '>' || *z == '|' ||
                        *z == '\\' || *z == '/') *z = '_';
                size_t sl2 = strlen(safe);        /* Punkte am Ende sind unzulaessig */
                while (sl2 > 0 && (safe[sl2 - 1] == '.' || safe[sl2 - 1] == ' '))
                    safe[--sl2] = 0;
                if (!safe[0])
                    strcpy(safe, "Modpack");

                /* <ziel>\<Pack>\modpack  und  <ziel>\<Pack>\server */
                char base[MAX_PATH], packDir[MAX_PATH], srvDir[MAX_PATH];
                snprintf(base, sizeof(base), "%s\\%s", dest, safe);
                CreateDirectoryA(base, NULL);
                snprintf(packDir, sizeof(packDir), "%s\\modpack", base);
                snprintf(srvDir, sizeof(srvDir), "%s\\server", base);

                SetCursor(LoadCursor(NULL, IDC_WAIT));
                static char blocked[4000];
                int nBlocked = 0;
                int ok = (gPickSource == 2)
                    ? download_cfpack(hwnd, gPickUrl, packDir,
                                      blocked, sizeof(blocked), &nBlocked)
                    : download_mrpack(hwnd, gPickUrl, packDir);
                SetCursor(LoadCursor(NULL, IDC_ARROW));
                if (!ok) {
                    MessageBoxA(hwnd,
                                "The modpack could not be downloaded completely.\n"
                                "Please check your internet connection.",
                                "ModSorter", MB_OK | MB_ICONERROR);
                    return 0;
                }
                if (nBlocked > 0) {
                    char msg[4400];
                    snprintf(msg, sizeof(msg),
                             "%d mod(s) could not be downloaded because their\n"
                             "authors disabled third-party distribution.\n\n"
                             "Please download these from the CurseForge website\n"
                             "and put them into the mods folder yourself:\n\n%s",
                             nBlocked, blocked);
                    MessageBoxA(hwnd, msg, "ModSorter", MB_OK | MB_ICONWARNING);
                }
                snprintf(gFolder, MAX_PATH, "%s\\mods", packDir);
                SetWindowTextA(gEditPath, gFolder);
                scan_folder(hwnd);
                /* direkt bauen - kein zweiter Ordnerdialog, keine Rueckfrage */
                if (alsoServer)
                    build_server_pack(hwnd, srvDir, 2);
                return 0;
            }

            if (sel >= 0 && sel < gInstN) {
                strncpy(gFolder, gInst[sel].path, MAX_PATH - 1);
                gFolder[MAX_PATH - 1] = 0;
                SetWindowTextA(gEditPath, gFolder);
                scan_folder(hwnd);
                if (alsoServer)
                    create_server_pack(hwnd);
            }
            return 0;
        }
        case ID_BTN_CHOOSE:
            choose_folder(hwnd);
            return 0;
        case ID_BTN_SCAN:
            scan_from_edit(hwnd);
            if (gServerOut[0]) {                 /* Batch: direkt Server-Pack bauen */
                build_server_pack(hwnd, gServerOut, 1);
                gServerOut[0] = 0;
                if (gAutoQuit)
                    PostMessageA(hwnd, WM_CLOSE, 0, 0);
            }
            return 0;
        case ID_BTN_COPY:
            copy_sorted(hwnd);
            return 0;
        case ID_BTN_SERVER:
            create_server_pack(hwnd);
            return 0;
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)prev;
    SetProcessDPIAware();                 /* scharfe Schrift auf skalierten Displays */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    enable_dark_controls();
    load_cf_key();

    GdiplusStartupInput_ gsi;
    ZeroMemory(&gsi, sizeof(gsi));
    gsi.GdiplusVersion = 1;
    GdiplusStartup(&gGdiplusToken, &gsi, NULL);

    HDC sdc = GetDC(NULL);
    gDpi = GetDeviceCaps(sdc, LOGPIXELSX);
    ReleaseDC(NULL, sdc);
    if (gDpi < 96) gDpi = 96;

    /* Startargumente:  ModSorter.exe "<mods>" [--server "<ziel>" [--quit]]  */
    if (cmd && *cmd) {
        char line[MAX_PATH * 2 + 32];
        strncpy(line, cmd, sizeof(line) - 1);
        line[sizeof(line) - 1] = 0;

        if (strstr(line, "--quit")) gAutoQuit = 1;

        /* --mrpack auf einer Kopie auswerten, damit --server unversehrt bleibt */
        {
            char copy[sizeof(line)];
            strncpy(copy, line, sizeof(copy) - 1);
            copy[sizeof(copy) - 1] = 0;
            char *mp = strstr(copy, "--mrpack");
            if (!mp) {
                mp = strstr(copy, "--cfpack");
                if (mp) gPackIsCf = 1;
            }
            if (mp) {
                char *u = mp + 8;
                char *stop = strstr(u, " --");
                if (stop) *stop = 0;
                while (*u == ' ' || *u == '\"') u++;
                strncpy(gMrpackUrl, u, sizeof(gMrpackUrl) - 1);
                gMrpackUrl[sizeof(gMrpackUrl) - 1] = 0;
                size_t ul = strlen(gMrpackUrl);
                while (ul > 0 && (gMrpackUrl[ul-1] == ' ' || gMrpackUrl[ul-1] == '\"'))
                    gMrpackUrl[--ul] = 0;
                *mp = 0;                       /* aus dem Original entfernen */
                char *orig = strstr(line, gPackIsCf ? "--cfpack" : "--mrpack");
                if (orig) {
                    char *rest = strstr(orig + 8, " --");
                    if (rest) memmove(orig, rest, strlen(rest) + 1);
                    else *orig = 0;
                }
            }
        }

        char *sw = strstr(line, "--server");
        if (sw) {
            char *dst = sw + 8;
            *sw = 0;                              /* Mods-Pfad endet hier */
            char *q = strstr(dst, "--quit");
            if (q) *q = 0;
            while (*dst == ' ' || *dst == '\"') dst++;
            strncpy(gServerOut, dst, MAX_PATH - 1);
            gServerOut[MAX_PATH - 1] = 0;
            size_t dl = strlen(gServerOut);
            while (dl > 0 && (gServerOut[dl - 1] == ' ' || gServerOut[dl - 1] == '\"' ||
                              gServerOut[dl - 1] == '\\'))
                gServerOut[--dl] = 0;
        }

        char *sp = line;
        while (*sp == ' ' || *sp == '\"') sp++;
        strncpy(gStartPath, sp, MAX_PATH - 1);
        gStartPath[MAX_PATH - 1] = 0;
        size_t sl = strlen(gStartPath);
        while (sl > 0 && (gStartPath[sl - 1] == ' ' || gStartPath[sl - 1] == '\"'))
            gStartPath[--sl] = 0;
    }

    gbrBg    = CreateSolidBrush(CBG);
    gbrCard  = CreateSolidBrush(CCARD);
    gbrInput = CreateSolidBrush(CINPUT);

    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = gbrBg;
    wc.lpszClassName = "ModSorterWin";
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowExA(
        0, "ModSorterWin",
        "ModSorter  -  Client/Server Mod Sorter (Fabric / NeoForge / Forge)",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, S(1260), S(660),
        NULL, NULL, hInst, NULL);

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    CoUninitialize();
    return 0;
}

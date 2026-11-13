// BmFix: stutter fixes for Batman: Arkham Asylum GOTY (32-bit DLL, no C runtime library).
// The game loads this file as its faultrep.dll before its own code starts. A test harness can also inject it as
// BmFixHook.dll. It changes the Direct3DCreate9 import of the exe, wraps IDirect3D9::CreateDevice, and then hooks
// the vtable (the table of method addresses) of the device. The fixes are in the sections below.
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <stdarg.h>
#include <d3d9.h>
__declspec(dllimport) HRESULT WINAPI SHGetFolderPathW(HWND, int, HANDLE, DWORD, LPWSTR);

// ---------------------------------------------------------------- freestanding helpers
void *memset(void *d, int c, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
int memcmp(const void *a, const void *b, size_t n) { const unsigned char *x = a, *y = b; for (; n; n--, x++, y++) if (*x != *y) return *x - *y; return 0; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }
static __inline unsigned long long rdtsc(void) { unsigned lo, hi; __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi)); return ((unsigned long long)hi << 32) | lo; }
static unsigned tpu = 1;   // rdtsc ticks per microsecond
static unsigned us(unsigned long long t) { if (t > 0xFFFFFFFFull) return 0xFFFFFFFFu / tpu; return (unsigned)t / tpu; }

// ---------------------------------------------------------------- log (buffered, written by a background thread)
static CRITICAL_SECTION logcs;
static char logbuf[1 << 20], wbuf[1 << 20];
static int loglen, logdrop;
static HANDLE logfile = INVALID_HANDLE_VALUE;
static char combobuf[64 * 1024]; static int combolen;
static HANDLE combofile = INVALID_HANDLE_VALUE;
static void logf(const char *fmt, ...)
{
    char tmp[600]; va_list ap; int n;
    va_start(ap, fmt); n = wvsprintfA(tmp, fmt, ap); va_end(ap);
    EnterCriticalSection(&logcs);
    if (loglen + n + 2 < (int)sizeof logbuf) { memcpy(logbuf + loglen, tmp, n); loglen += n; logbuf[loglen++] = '\r'; logbuf[loglen++] = '\n'; }
    else logdrop++;
    LeaveCriticalSection(&logcs);
}
static void physxPoll(void);
static DWORD WINAPI logthread(LPVOID p)
{
    for (;;)
    {
        int n; DWORD w;
        Sleep(200);
        physxPoll();
        EnterCriticalSection(&logcs); n = loglen; memcpy(wbuf, logbuf, n); loglen = 0; LeaveCriticalSection(&logcs);
        if (n && logfile != INVALID_HANDLE_VALUE) WriteFile(logfile, wbuf, n, &w, NULL);
        EnterCriticalSection(&logcs); n = combolen; memcpy(wbuf, combobuf, n); combolen = 0; LeaveCriticalSection(&logcs);
        if (n && combofile != INVALID_HANDLE_VALUE) WriteFile(combofile, wbuf, n, &w, NULL);
    }
    return 0;
}
static void qpcstr(char *s) { LARGE_INTEGER q; QueryPerformanceCounter(&q); wsprintfA(s, "%08lX%08lX", (unsigned long)q.HighPart, (unsigned long)q.LowPart); }   // hex QPC

// ---------------------------------------------------------------- state
enum { I_Present = 17, I_CreateTexture = 23, I_CreateVertexBuffer = 26, I_CreateIndexBuffer = 27,
       I_DrawPrimitive = 81, I_DrawIndexedPrimitive = 82, I_DrawPrimitiveUP = 83, I_DrawIndexedPrimitiveUP = 84,
       I_CreateVertexDeclaration = 86, I_SetVertexDeclaration = 87, I_SetFVF = 89, I_CreateVertexShader = 91,
       I_SetVertexShader = 92, I_CreatePixelShader = 106, I_SetPixelShader = 107, I_MAX = 120 };
static void *orig[I_MAX];
static void *d3dCreateOrig, *createDeviceOrig;
volatile DWORD rtid __attribute__((used));   // render thread (the one calling Present)
static unsigned frame;
static int opt_preload = 1, opt_trace = 0, opt_prof = 0, opt_warm = 0, opt_bg = 0, opt_hookon = 1, opt_dxvk = 0, warmBudget = 32;

static void patch(void **vt, int i, void *fn)
{
    DWORD old;
    if (vt[i] == fn) return;
    VirtualProtect(&vt[i], 4, PAGE_EXECUTE_READWRITE, &old);
    orig[i] = vt[i]; vt[i] = fn;
    VirtualProtect(&vt[i], 4, old, &old);
}

static void profResource(void *obj, int kind);
static void profFrame(unsigned frame_us, unsigned f);
// ---------------------------------------------------------------- preload
#define PRE_QMAX 4096
#define PRE_BUDGET 4
#define PRE_DELAY 3
#define PRE_MINBUF (16 * 1024)
static IDirect3DResource9 *pq[PRE_QMAX];
static unsigned pqf[PRE_QMAX], ph, pt, ptotal;
static void pre_enqueue(void *r)
{
    if (!r || pt - ph >= PRE_QMAX) return;
    IUnknown_AddRef((IUnknown *)r);
    pq[pt & (PRE_QMAX - 1)] = (IDirect3DResource9 *)r; pqf[pt & (PRE_QMAX - 1)] = frame; pt++;
}
static void pre_run(void)
{
    int n;
    for (n = 0; n < PRE_BUDGET && ph != pt && frame - pqf[ph & (PRE_QMAX - 1)] >= PRE_DELAY; n++)
    {
        IDirect3DResource9 *r = pq[ph & (PRE_QMAX - 1)];
        IDirect3DResource9_PreLoad(r); IDirect3DResource9_Release(r); ph++; ptotal++;
    }
}
typedef HRESULT (WINAPI *PFN_CT)(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9 **, HANDLE *);
typedef HRESULT (WINAPI *PFN_CVB)(IDirect3DDevice9 *, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9 **, HANDLE *);
typedef HRESULT (WINAPI *PFN_CIB)(IDirect3DDevice9 *, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9 **, HANDLE *);
static HRESULT WINAPI H_CreateTexture(IDirect3DDevice9 *d, UINT w, UINT h, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DTexture9 **pp, HANDLE *s)
{
    HRESULT hr = ((PFN_CT)orig[I_CreateTexture])(d, w, h, l, u, f, p, pp, s);
    if (SUCCEEDED(hr) && opt_prof) profResource(*pp, 1);
    if (SUCCEEDED(hr) && p == D3DPOOL_MANAGED && opt_preload && GetCurrentThreadId() == rtid) pre_enqueue(*pp);
    return hr;
}
static HRESULT WINAPI H_CreateVertexBuffer(IDirect3DDevice9 *d, UINT len, DWORD u, DWORD fvf, D3DPOOL p, IDirect3DVertexBuffer9 **pp, HANDLE *s)
{
    HRESULT hr = ((PFN_CVB)orig[I_CreateVertexBuffer])(d, len, u, fvf, p, pp, s);
    if (SUCCEEDED(hr) && opt_prof) profResource(*pp, 2);
    if (SUCCEEDED(hr) && p == D3DPOOL_MANAGED && len >= PRE_MINBUF && opt_preload && GetCurrentThreadId() == rtid) pre_enqueue(*pp);
    return hr;
}
static HRESULT WINAPI H_CreateIndexBuffer(IDirect3DDevice9 *d, UINT len, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DIndexBuffer9 **pp, HANDLE *s)
{
    HRESULT hr = ((PFN_CIB)orig[I_CreateIndexBuffer])(d, len, u, f, p, pp, s);
    if (SUCCEEDED(hr) && opt_prof) profResource(*pp, 3);
    if (SUCCEEDED(hr) && p == D3DPOOL_MANAGED && len >= PRE_MINBUF && opt_preload && GetCurrentThreadId() == rtid) pre_enqueue(*pp);
    return hr;
}

// ---------------------------------------------------------------- shader warm-up: identity tables (BMFIX_WARM=1)
// The NVIDIA D3D9 driver compiles a shader the first time that the game draws with it, not when the game creates
// it. Each new pixel shader costs 0.3-0.5 ms, and a camera turn into a new area can draw dozens in one frame.
// BmFix records each combination of vertex shader, pixel shader, and declaration that the game draws. It stores
// them by bytecode hash in BmFixCombos.bin. In later sessions, BmFix draws each recorded combination one time
// as soon as its shaders exist, which is while the game loads. It draws a triangle with no area, so nothing
// shows on the screen.
#define HM_N 32768
typedef struct { DWORD h; void *p; } HEnt;
static HEnt vsBy[HM_N], psBy[HM_N], vdBy[HM_N];
static unsigned nCreates, lastCreateFrame, lastFrameUs;
static void hput(HEnt *t, DWORD h, void *p)
{
    unsigned i = (h * 2654435761u) >> 17, k;
    if (!h) return;
    for (k = 0; k < HM_N; k++, i = (i + 1) & (HM_N - 1))
    {
        if (t[i].h == h) return;
        if (!t[i].h) { t[i].h = h; t[i].p = p; IUnknown_AddRef((IUnknown *)p); nCreates++; lastCreateFrame = frame; return; }   // kept alive for replays
    }
}
static void *hget(HEnt *t, DWORD h)
{
    unsigned i = (h * 2654435761u) >> 17, k;
    if (!h) return 0;
    for (k = 0; k < HM_N; k++, i = (i + 1) & (HM_N - 1)) { if (t[i].h == h) return t[i].p; if (!t[i].h) return 0; }
    return 0;
}
#define DI_N 2048
static struct { void *p; WORD stride[8]; } dinfo[DI_N];
static int ndinfo;
static void declInfo(void *p, const D3DVERTEXELEMENT9 *e)
{
    static const BYTE sz[] = { 4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8, 4, 4, 4, 8, 0 };
    int n;
    if (ndinfo >= DI_N) return;
    dinfo[ndinfo].p = p;
    for (n = 0; n < 8; n++) dinfo[ndinfo].stride[n] = 0;
    for (n = 0; n < 64 && e[n].Stream != 0xFF; n++)
        if (e[n].Stream < 8 && e[n].Type < 18)
        {
            WORD end = (WORD)(e[n].Offset + sz[e[n].Type]);
            if (end > dinfo[ndinfo].stride[e[n].Stream]) dinfo[ndinfo].stride[e[n].Stream] = end;
        }
    ndinfo++;
}
static WORD *declStrides(void *p) { int i; for (i = 0; i < ndinfo; i++) if (dinfo[i].p == p) return dinfo[i].stride; return 0; }

// ---------------------------------------------------------------- bytecode copies for the background compiler (BMFIX_BG=1)
static CRITICAL_SECTION bgcs;
typedef struct { DWORD h; void *code; } CEnt;
static CEnt vsCode[HM_N], psCode[HM_N], vdCode[HM_N];
static unsigned bgBytes;
static void cput(CEnt *t, DWORD h, const void *src, unsigned bytes)
{
    unsigned i = (h * 2654435761u) >> 17, k;
    if (!h) return;
    EnterCriticalSection(&bgcs);
    for (k = 0; k < HM_N; k++, i = (i + 1) & (HM_N - 1))
    {
        if (t[i].h == h) break;
        if (!t[i].h)
        {
            void *c = HeapAlloc(GetProcessHeap(), 0, bytes);
            if (c) { memcpy(c, src, bytes); t[i].code = c; t[i].h = h; bgBytes += bytes; }
            break;
        }
    }
    LeaveCriticalSection(&bgcs);
}
static void *cget(CEnt *t, DWORD h)
{
    unsigned i = (h * 2654435761u) >> 17, k; void *r = 0;
    if (!h) return 0;
    EnterCriticalSection(&bgcs);
    for (k = 0; k < HM_N; k++, i = (i + 1) & (HM_N - 1)) { if (t[i].h == h) { r = t[i].code; break; } if (!t[i].h) break; }
    LeaveCriticalSection(&bgcs);
    return r;
}

// ---------------------------------------------------------------- shader / declaration identity
#define SH_N 65536
static struct { void *p; DWORD h; } shmap[SH_N];
static DWORD fnv(const void *b, unsigned n) { const unsigned char *p = b; DWORD h = 2166136261u; while (n--) { h ^= *p++; h *= 16777619u; } return h; }
static unsigned slot(void *p) { return ((DWORD)p >> 3) * 2654435761u >> 16; }
static void sh_put(void *p, DWORD h)
{
    unsigned i = slot(p), k;
    for (k = 0; k < SH_N; k++, i = (i + 1) & (SH_N - 1))
        if (shmap[i].p == p || shmap[i].p == 0) { shmap[i].p = p; shmap[i].h = h; return; }
}
static DWORD sh_get(void *p)
{
    unsigned i = slot(p), k;
    if (!p) return 0;
    for (k = 0; k < SH_N; k++, i = (i + 1) & (SH_N - 1)) { if (shmap[i].p == p) return shmap[i].h; if (!shmap[i].p) return 0; }
    return 0;
}
static unsigned shaderBytes(const DWORD *f)
{
    // walk the token stream to D3DSIO_END, skipping comments and (SM2+) instruction lengths
    unsigned i = 1, ver = f[0] & 0xFFFF;
    while (i < 65536)
    {
        DWORD t = f[i];
        if (t == 0x0000FFFF) return 4 * (i + 1);
        if ((t & 0xFFFF) == 0xFFFE) i += 1 + ((t >> 16) & 0x7FFF);
        else if (ver >= 0x200) i += 1 + ((t >> 24) & 0xF);
        else i++;
    }
    return 4 * i;
}
typedef HRESULT (WINAPI *PFN_CVS)(IDirect3DDevice9 *, const DWORD *, IDirect3DVertexShader9 **);
typedef HRESULT (WINAPI *PFN_CPS)(IDirect3DDevice9 *, const DWORD *, IDirect3DPixelShader9 **);
typedef HRESULT (WINAPI *PFN_CVD)(IDirect3DDevice9 *, const D3DVERTEXELEMENT9 *, IDirect3DVertexDeclaration9 **);
static unsigned nVS, nPS, nVD;
static unsigned long long createTicks;
static HRESULT WINAPI H_CreateVertexShader(IDirect3DDevice9 *d, const DWORD *f, IDirect3DVertexShader9 **pp)
{
    unsigned long long t0 = rdtsc();
    HRESULT hr = ((PFN_CVS)orig[I_CreateVertexShader])(d, f, pp);
    createTicks += rdtsc() - t0;
    if (SUCCEEDED(hr) && f) { unsigned nb = shaderBytes(f); DWORD h = fnv(f, nb) | 1; sh_put(*pp, h); nVS++; if (opt_warm) hput(vsBy, h, *pp); if (opt_bg) cput(vsCode, h, f, nb); }
    return hr;
}
static HRESULT WINAPI H_CreatePixelShader(IDirect3DDevice9 *d, const DWORD *f, IDirect3DPixelShader9 **pp)
{
    unsigned long long t0 = rdtsc();
    HRESULT hr = ((PFN_CPS)orig[I_CreatePixelShader])(d, f, pp);
    createTicks += rdtsc() - t0;
    if (SUCCEEDED(hr) && f) { unsigned nb = shaderBytes(f); DWORD h = fnv(f, nb) & ~1u; sh_put(*pp, h); nPS++; if (opt_warm) hput(psBy, h, *pp); if (opt_bg) cput(psCode, h, f, nb); }
    return hr;
}
static HRESULT WINAPI H_CreateVertexDeclaration(IDirect3DDevice9 *d, const D3DVERTEXELEMENT9 *e, IDirect3DVertexDeclaration9 **pp)
{
    HRESULT hr = ((PFN_CVD)orig[I_CreateVertexDeclaration])(d, e, pp);
    if (SUCCEEDED(hr) && e)
    {
        unsigned n = 0; DWORD h; while (n < 64 && e[n].Stream != 0xFF) n++;
        h = fnv(e, 8 * n) | 1; sh_put(*pp, h); nVD++;
        if (opt_warm && !hget(vdBy, h)) { hput(vdBy, h, *pp); declInfo(*pp, e); }
        if (opt_bg) cput(vdCode, h, e, 8 * (n + 1));
    }
    return hr;
}

// ---------------------------------------------------------------- current bindings + first-seen combinations
static void *curVS, *curPS, *curVD; static DWORD curFVF;
typedef HRESULT (WINAPI *PFN_SetP)(IDirect3DDevice9 *, void *);
typedef HRESULT (WINAPI *PFN_SetD)(IDirect3DDevice9 *, DWORD);
static HRESULT WINAPI H_SetVertexShader(IDirect3DDevice9 *d, void *s) { curVS = s; return ((PFN_SetP)orig[I_SetVertexShader])(d, s); }
static HRESULT WINAPI H_SetPixelShader(IDirect3DDevice9 *d, void *s) { curPS = s; return ((PFN_SetP)orig[I_SetPixelShader])(d, s); }
static HRESULT WINAPI H_SetVertexDeclaration(IDirect3DDevice9 *d, void *s) { curVD = s; curFVF = 0; return ((PFN_SetP)orig[I_SetVertexDeclaration])(d, s); }
static HRESULT WINAPI H_SetFVF(IDirect3DDevice9 *d, DWORD f) { curFVF = f; curVD = 0; return ((PFN_SetD)orig[I_SetFVF])(d, f); }

#define SEEN_N 65536
static struct { void *vs, *ps, *vd; } seen[SEEN_N];
static unsigned nSeen;
static int seen_insert(void)
{
    void *vd = curVD ? curVD : (void *)(curFVF | 0x80000000u);
    unsigned i = (((DWORD)curVS * 31u + (DWORD)curPS) * 31u + (DWORD)vd) * 2654435761u >> 16, k;
    if (nSeen > SEEN_N * 3 / 4) return 0;
    for (k = 0; k < SEEN_N; k++, i = (i + 1) & (SEEN_N - 1))
    {
        if (seen[i].vs == curVS && seen[i].ps == curPS && seen[i].vd == vd) return 0;
        if (!seen[i].vs && !seen[i].ps && !seen[i].vd) { seen[i].vs = curVS; seen[i].ps = curPS; seen[i].vd = vd; nSeen++; return 1; }
    }
    return 0;
}


// ---------------------------------------------------------------- shader warm-up: record + replay
#define PEND_N 65536
#define KN_N 131072
typedef struct { DWORD vs, ps, vd; } Combo;
static Combo pend[PEND_N], known[KN_N];
static unsigned char pendDone[PEND_N];
static int npend, nwarmed, nknown, nrecorded;
static int known_add(DWORD vs, DWORD ps, DWORD vd)
{
    unsigned i = ((vs * 31u + ps) * 31u + vd) * 2654435761u >> 15, k;
    if (nknown > KN_N * 3 / 4) return 0;
    for (k = 0; k < KN_N; k++, i = (i + 1) & (KN_N - 1))
    {
        if (known[i].vs == vs && known[i].ps == ps && known[i].vd == vd) return 0;
        if (!known[i].vs) { known[i].vs = vs; known[i].ps = ps; known[i].vd = vd; nknown++; return 1; }
    }
    return 0;
}
static void warm_record(void)
{
    Combo c;
    c.vs = sh_get(curVS); c.ps = sh_get(curPS); c.vd = curVD ? sh_get(curVD) : 0;
    if (!c.vs || !c.ps || !c.vd || !known_add(c.vs, c.ps, c.vd)) return;
    EnterCriticalSection(&logcs);
    if (combolen + 12 <= (int)sizeof combobuf) { memcpy(combobuf + combolen, &c, 12); combolen += 12; nrecorded++; }
    LeaveCriticalSection(&logcs);
}
static void warm_load(const WCHAR *path)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE)
    {
        DWORD got = 0; Combo c;
        while (npend < PEND_N && ReadFile(h, &c, 12, &got, NULL) && got == 12)
            if (c.vs && c.ps && c.vd && known_add(c.vs, c.ps, c.vd)) pend[npend++] = c;
        CloseHandle(h);
    }
    combofile = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}
typedef HRESULT (WINAPI *PFN_DP)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
static IDirect3DVertexBuffer9 *warmVB;
static unsigned lastScanCreates = 0xFFFFFFFFu, lastScanFrame;
static void warm_run(IDirect3DDevice9 *d)
{
    IDirect3DVertexShader9 *svs = 0; IDirect3DPixelShader9 *sps = 0; IDirect3DVertexDeclaration9 *svd = 0; DWORD sfvf = 0;
    IDirect3DVertexBuffer9 *svb[8]; UINT soff[8], sstr[8], sfrq[8];
    int i, s, n = 0, began = 0, scannedAll = 1, budget;
    if (nwarmed >= npend) return;
    // Use the full budget while the game loads (it creates shaders, or the frames are long). Otherwise, use 2 in each frame.
    budget = (frame - lastCreateFrame < 30 || lastFrameUs > 25000) ? warmBudget : 2;
    if (nCreates == lastScanCreates && frame - lastScanFrame < 300) return;   // nothing new to match since the last full scan
    if (!warmVB)
    {
        void *m;
        if (FAILED(((PFN_CVB)orig[I_CreateVertexBuffer])(d, 65536, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &warmVB, NULL))) { warmVB = 0; return; }
        if (SUCCEEDED(IDirect3DVertexBuffer9_Lock(warmVB, 0, 0, &m, 0))) { memset(m, 0, 65536); IDirect3DVertexBuffer9_Unlock(warmVB); }
    }
    for (i = 0; i < npend; i++)
    {
        void *vs, *ps, *vd; WORD *st;
        if (pendDone[i]) continue;
        if (n >= budget) { scannedAll = 0; break; }
        vs = hget(vsBy, pend[i].vs); ps = hget(psBy, pend[i].ps); vd = hget(vdBy, pend[i].vd);
        if (!vs || !ps || !vd || !(st = declStrides(vd))) continue;
        if (!n)
        {
            IDirect3DDevice9_GetVertexShader(d, &svs); IDirect3DDevice9_GetPixelShader(d, &sps);
            IDirect3DDevice9_GetVertexDeclaration(d, &svd); IDirect3DDevice9_GetFVF(d, &sfvf);
            for (s = 0; s < 8; s++) { svb[s] = 0; soff[s] = sstr[s] = 0; sfrq[s] = 1; IDirect3DDevice9_GetStreamSource(d, s, &svb[s], &soff[s], &sstr[s]); IDirect3DDevice9_GetStreamSourceFreq(d, s, &sfrq[s]); }
            began = SUCCEEDED(IDirect3DDevice9_BeginScene(d));
        }
        ((PFN_SetP)orig[I_SetVertexDeclaration])(d, vd);
        ((PFN_SetP)orig[I_SetVertexShader])(d, vs);
        ((PFN_SetP)orig[I_SetPixelShader])(d, ps);
        for (s = 0; s < 8; s++) if (st[s]) { IDirect3DDevice9_SetStreamSource(d, s, warmVB, 0, st[s]); IDirect3DDevice9_SetStreamSourceFreq(d, s, 1); }
        ((PFN_DP)orig[I_DrawPrimitive])(d, D3DPT_TRIANGLELIST, 0, 1);
        pendDone[i] = 1; nwarmed++; n++;
    }
    if (n)
    {
        if (began) IDirect3DDevice9_EndScene(d);
        if (svd) ((PFN_SetP)orig[I_SetVertexDeclaration])(d, svd); else IDirect3DDevice9_SetFVF(d, sfvf);
        ((PFN_SetP)orig[I_SetVertexShader])(d, svs);
        ((PFN_SetP)orig[I_SetPixelShader])(d, sps);
        for (s = 0; s < 8; s++) { IDirect3DDevice9_SetStreamSource(d, s, svb[s], soff[s], sstr[s]); IDirect3DDevice9_SetStreamSourceFreq(d, s, sfrq[s]); if (svb[s]) IDirect3DVertexBuffer9_Release(svb[s]); }
        if (svs) IDirect3DVertexShader9_Release(svs);
        if (sps) IDirect3DPixelShader9_Release(sps);
        if (svd) IDirect3DVertexDeclaration9_Release(svd);
        if (opt_trace || nwarmed == npend || (frame & 63) == 0) logf("W f=%u warmed %d this frame, %d/%d total", frame, n, nwarmed, npend);
    }
    if (scannedAll) { lastScanCreates = nCreates; lastScanFrame = frame; } else lastScanCreates = 0xFFFFFFFFu;
}

// ---------------------------------------------------------------- background compiler (BMFIX_BG=1)
// A second D3D9 device draws each recorded combination one time from copies of the shader bytecode of the game.
// A low-priority thread owns it, with an 8x8 window that never shows. The goal was to make the driver compile
// the shaders away from the render thread of the game. This helps only if the driver shares compiled shaders
// between the devices of one process. A test showed that the NVIDIA driver does not (2026-11). All 543
// combinations compiled in the background in 1.2 s, but the game still paused at the first draw. This code is
// off unless BMFIX_BG=1. The warm-up (BMFIX_WARM) is the fix that works.
typedef HRESULT (WINAPI *PFN_CD2)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
typedef IDirect3D9 *(WINAPI *PFN_D3DC2)(UINT);
static DWORD bgFlags = 0x42; static D3DFORMAT bgFmt = D3DFMT_A8R8G8B8; static int bgStarted, bgCompiled;
static unsigned char bgDone[PEND_N];
static HEnt bgVS[HM_N], bgPS[HM_N], bgVD[HM_N];
static void bput(HEnt *t, DWORD h, void *p)
{
    unsigned i = (h * 2654435761u) >> 17, k;
    for (k = 0; k < HM_N; k++, i = (i + 1) & (HM_N - 1)) if (!t[i].h) { t[i].h = h; t[i].p = p; return; }
}
static void bgflush(IDirect3DQuery9 *q)
{
    int k;
    if (!q) return;
    IDirect3DQuery9_Issue(q, D3DISSUE_END);
    for (k = 0; k < 5000 && IDirect3DQuery9_GetData(q, NULL, 0, D3DGETDATA_FLUSH) == S_FALSE; k++) Sleep(1);
}
static DWORD WINAPI bgthread(LPVOID arg)
{
    HWND w; IDirect3D9 *d3d; IDirect3DDevice9 *dev = 0; D3DPRESENT_PARAMETERS pp; IDirect3DVertexBuffer9 *vb = 0; IDirect3DQuery9 *q = 0;
    HRESULT hr; int i, s, n, idle = 0, inScene = 0; void *m; unsigned long long tStart = rdtsc(), tBusy = 0;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    w = CreateWindowExW(0, L"STATIC", L"BmFixBg", WS_POPUP, 0, 0, 8, 8, NULL, NULL, NULL, NULL);   // never shown
    d3d = w ? ((PFN_D3DC2)d3dCreateOrig)(D3D_SDK_VERSION) : 0;
    if (!d3d) { logf("bg: no window / Direct3D"); return 0; }
    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth = 8; pp.BackBufferHeight = 8; pp.BackBufferFormat = bgFmt; pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = w; pp.Windowed = TRUE; pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    hr = ((PFN_CD2)createDeviceOrig)(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, w, bgFlags, &pp, &dev);
    logf("bg: device %08X hr=%08X vtable %08X, %d combinations to compile", (DWORD)dev, hr, dev ? *(DWORD *)dev : 0, npend);
    if (FAILED(hr) || !dev) return 0;
    if (FAILED(((PFN_CVB)orig[I_CreateVertexBuffer])(dev, 65536, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb, NULL))) return 0;
    if (SUCCEEDED(IDirect3DVertexBuffer9_Lock(vb, 0, 0, &m, 0))) { memset(m, 0, 65536); IDirect3DVertexBuffer9_Unlock(vb); }
    IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_EVENT, &q);
    for (;;)
    {
        unsigned long long t0 = rdtsc();
        n = 0;
        for (i = 0; i < npend; i++)
        {
            void *vs, *ps, *vd; const D3DVERTEXELEMENT9 *el; WORD st[8]; int e;
            if (bgDone[i]) continue;
            if (!(vs = hget(bgVS, pend[i].vs)))
            {
                const DWORD *c = cget(vsCode, pend[i].vs);
                if (!c || FAILED(((PFN_CVS)orig[I_CreateVertexShader])(dev, c, (IDirect3DVertexShader9 **)&vs))) continue;
                bput(bgVS, pend[i].vs, vs);
            }
            if (!(ps = hget(bgPS, pend[i].ps)))
            {
                const DWORD *c = cget(psCode, pend[i].ps);
                if (!c || FAILED(((PFN_CPS)orig[I_CreatePixelShader])(dev, c, (IDirect3DPixelShader9 **)&ps))) continue;
                bput(bgPS, pend[i].ps, ps);
            }
            el = cget(vdCode, pend[i].vd);
            if (!el) continue;
            if (!(vd = hget(bgVD, pend[i].vd)))
            {
                if (FAILED(((PFN_CVD)orig[I_CreateVertexDeclaration])(dev, el, (IDirect3DVertexDeclaration9 **)&vd))) continue;
                bput(bgVD, pend[i].vd, vd);
            }
            {
                static const BYTE sz[] = { 4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8, 4, 4, 4, 8, 0 };
                for (s = 0; s < 8; s++) st[s] = 0;
                for (e = 0; e < 64 && el[e].Stream != 0xFF; e++)
                    if (el[e].Stream < 8 && el[e].Type < 18 && el[e].Offset + sz[el[e].Type] > st[el[e].Stream]) st[el[e].Stream] = (WORD)(el[e].Offset + sz[el[e].Type]);
            }
            if (!inScene) { IDirect3DDevice9_BeginScene(dev); inScene = 1; }
            ((PFN_SetP)orig[I_SetVertexDeclaration])(dev, vd);
            ((PFN_SetP)orig[I_SetVertexShader])(dev, vs);
            ((PFN_SetP)orig[I_SetPixelShader])(dev, ps);
            for (s = 0; s < 8; s++) if (st[s]) IDirect3DDevice9_SetStreamSource(dev, s, vb, 0, st[s]);
            ((PFN_DP)orig[I_DrawPrimitive])(dev, D3DPT_TRIANGLELIST, 0, 1);
            bgDone[i] = 1; bgCompiled++; n++;
            if ((n & 15) == 0) { IDirect3DDevice9_EndScene(dev); inScene = 0; bgflush(q); }
        }
        if (inScene) { IDirect3DDevice9_EndScene(dev); inScene = 0; bgflush(q); }
        if (n) { tBusy += rdtsc() - t0; logf("bg: compiled %d (%d/%d), busy %u ms, %u ms since start, %u KB bytecode copied", n, bgCompiled, npend, us(tBusy) / 1000, us(rdtsc() - tStart) / 1000, bgBytes / 1024); idle = 0; }
        if (bgCompiled >= npend) break;
        if (!n && ++idle > 3000) break;          // ~10 min without anything new to compile
        Sleep(n ? 1 : 200);
    }
    logf("bg: done, %d/%d combinations compiled on the background device", bgCompiled, npend);
    return 0;
}

// per-frame stats
static unsigned long long fDrawTicks, fMaxDraw, slowDrawTicks, slowFrameTicks, lastPresentEnd;
static unsigned fDraws, fNew, fNewSlow;
static void after_draw(int kind, unsigned long long dt)
{
    int first;
    if (!opt_trace && !opt_warm && !opt_bg) return;
    first = GetCurrentThreadId() == rtid ? seen_insert() : 0;
    if (first && (opt_warm || opt_bg)) warm_record();
    if (!opt_trace) return;
    fDraws++; fDrawTicks += dt; if (dt > fMaxDraw) fMaxDraw = dt;
    if (first) fNew++;
    if (dt > slowDrawTicks || first)
    {
        char q[24]; qpcstr(q);
        if (dt > slowDrawTicks && first) fNewSlow++;
        logf("%s f=%u q=%s k=%d us=%u vs=%08X ps=%08X vd=%08X", first ? "N" : "S", frame, q, kind, us(dt), sh_get(curVS), sh_get(curPS), curVD ? sh_get(curVD) : curFVF);
    }
}
typedef HRESULT (WINAPI *PFN_DIP)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT (WINAPI *PFN_DPUP)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, const void *, UINT);
typedef HRESULT (WINAPI *PFN_DIPUP)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void *, D3DFORMAT, const void *, UINT);
static HRESULT WINAPI H_DrawPrimitive(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT a, UINT b)
{ unsigned long long t0 = rdtsc(); HRESULT hr = ((PFN_DP)orig[I_DrawPrimitive])(d, t, a, b); after_draw(0, rdtsc() - t0); return hr; }
static HRESULT WINAPI H_DrawIndexedPrimitive(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT a, UINT b, UINT c, UINT e, UINT f)
{ unsigned long long t0 = rdtsc(); HRESULT hr = ((PFN_DIP)orig[I_DrawIndexedPrimitive])(d, t, a, b, c, e, f); after_draw(1, rdtsc() - t0); return hr; }
static HRESULT WINAPI H_DrawPrimitiveUP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT a, const void *b, UINT c)
{ unsigned long long t0 = rdtsc(); HRESULT hr = ((PFN_DPUP)orig[I_DrawPrimitiveUP])(d, t, a, b, c); after_draw(2, rdtsc() - t0); return hr; }
static HRESULT WINAPI H_DrawIndexedPrimitiveUP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT a, UINT b, UINT c, const void *e, D3DFORMAT f, const void *g, UINT h)
{ unsigned long long t0 = rdtsc(); HRESULT hr = ((PFN_DIPUP)orig[I_DrawIndexedPrimitiveUP])(d, t, a, b, c, e, f, g, h); after_draw(3, rdtsc() - t0); return hr; }

typedef HRESULT (WINAPI *PFN_Present)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
static HRESULT WINAPI H_Present(IDirect3DDevice9 *d, const RECT *a, const RECT *b, HWND w, const RGNDATA *r)
{
    unsigned long long t0, t1; HRESULT hr;
    rtid = GetCurrentThreadId();
    frame++;
    if (opt_preload) pre_run();
    if (opt_warm) warm_run(d);
    t0 = rdtsc();
    hr = ((PFN_Present)orig[I_Present])(d, a, b, w, r);
    t1 = rdtsc();
    if (opt_trace)
    {
        if (lastPresentEnd && (t1 - lastPresentEnd > slowFrameTicks || (frame & 1023) == 0))
        {
            char q[24]; qpcstr(q);
            logf("P f=%u q=%s frame_us=%u present_us=%u draws=%u draw_us=%u maxdraw_us=%u new=%u newslow=%u create_us=%u | vs=%u ps=%u vd=%u seen=%u preloaded=%u",
                 frame, q, us(t1 - lastPresentEnd), us(t1 - t0), fDraws, us(fDrawTicks), us(fMaxDraw), fNew, fNewSlow, us(createTicks), nVS, nPS, nVD, nSeen, ptotal);
        }
        fDraws = fNew = fNewSlow = 0; fDrawTicks = fMaxDraw = createTicks = 0;
    }
    if (opt_prof && lastPresentEnd) profFrame(us(t1 - lastPresentEnd), frame);
    if (lastPresentEnd) lastFrameUs = us(t1 - lastPresentEnd);
    lastPresentEnd = t1;
    return hr;
}


// ---------------------------------------------------------------- generic per-method timing (BMFIX_PROF=1)
// Each hooked vtable slot gets a 10-byte thunk: "mov eax,slot" and then "jmp prof_common". On the render thread,
// prof_common measures the time of the original call with rdtsc, but not for a call inside another timed call.
// On other threads, the thunk goes directly to the original method.
#define NSLOT 512
void *thunkOrig[NSLOT] __attribute__((used));
unsigned long long thunkTicks[NSLOT] __attribute__((used));
unsigned thunkCount[NSLOT] __attribute__((used)), thunkMax[NSLOT] __attribute__((used));
unsigned pdepth __attribute__((used)), psRet __attribute__((used)), psSlot __attribute__((used)), psHr __attribute__((used));
unsigned long long pt0 __attribute__((used));
static unsigned short slotName[NSLOT];
static int nslots;
static unsigned char *thunkMem;
static unsigned long long snapTicks[NSLOT]; static unsigned snapCount[NSLOT];
extern void prof_common(void);
__asm__(
    ".text\n.globl _prof_common\n_prof_common:\n"
    "  mov %fs:0x24, %ecx\n"
    "  cmp _rtid, %ecx\n"
    "  jne 1f\n"
    "  cmpl $0, _pdepth\n"
    "  jne 1f\n"
    "  pop %edx\n"
    "  mov %edx, _psRet\n"
    "  mov %eax, _psSlot\n"
    "  movl $1, _pdepth\n"
    "  rdtsc\n"
    "  mov %eax, _pt0\n"
    "  mov %edx, _pt0+4\n"
    "  mov _psSlot, %eax\n"
    "  call *_thunkOrig(,%eax,4)\n"
    "  mov %eax, _psHr\n"
    "  rdtsc\n"
    "  sub _pt0, %eax\n"
    "  sbb _pt0+4, %edx\n"
    "  mov _psSlot, %ecx\n"
    "  add %eax, _thunkTicks(,%ecx,8)\n"
    "  adc %edx, _thunkTicks+4(,%ecx,8)\n"
    "  incl _thunkCount(,%ecx,4)\n"
    "  cmp _thunkMax(,%ecx,4), %eax\n"
    "  jbe 2f\n"
    "  mov %eax, _thunkMax(,%ecx,4)\n"
    "2:\n"
    "  movl $0, _pdepth\n"
    "  mov _psHr, %eax\n"
    "  jmp *_psRet\n"
    "1:\n"
    "  jmp *_thunkOrig(,%eax,4)\n");
static void profSlot(void **vt, int idx, int kind)
{
    DWORD old; unsigned char *t; int s;
    if (nslots >= NSLOT) return;
    if (!thunkMem) thunkMem = VirtualAlloc(NULL, 16 * NSLOT, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!thunkMem) return;
    s = nslots++;
    t = thunkMem + 16 * s;
    t[0] = 0xB8; *(DWORD *)(t + 1) = s; t[5] = 0xE9; *(DWORD *)(t + 6) = (DWORD)prof_common - (DWORD)(t + 10);
    thunkOrig[s] = vt[idx]; slotName[s] = (unsigned short)(kind << 8 | idx);
    VirtualProtect(&vt[idx], 4, PAGE_EXECUTE_READWRITE, &old); vt[idx] = t; VirtualProtect(&vt[idx], 4, old, &old);
}
static void *profVt[6][8];
static void profResource(void *obj, int kind)
{
    static const int tex[] = { 9, 17, 18, 19, 20, 21 }, buf[] = { 9, 11, 12 }, qry[] = { 6, 7 }, srf[] = { 13, 14, 15, 16 };
    void **vt = *(void ***)obj; int i;
    for (i = 0; i < 8 && profVt[kind][i]; i++) if (profVt[kind][i] == vt) return;
    if (i == 8) return;
    profVt[kind][i] = vt;
    if (kind == 1) { int k; for (k = 0; k < 6; k++) profSlot(vt, tex[k], kind); }
    else if (kind == 4) { int k; for (k = 0; k < 2; k++) profSlot(vt, qry[k], kind); }
    else if (kind == 5) { int k; for (k = 0; k < 4; k++) profSlot(vt, srf[k], kind); }
    else { int k; for (k = 0; k < 3; k++) profSlot(vt, buf[k], kind); }
    logf("prof: hooked resource vtable kind=%d %08X", kind, (DWORD)vt);
}
static void profFrame(unsigned frame_us, unsigned f)
{
    int s, n, top[10]; unsigned long long d[NSLOT];
    char line[600]; int len;
    for (s = 0; s < nslots; s++) d[s] = thunkTicks[s] - snapTicks[s];
    if (frame_us > 8333)
    {
        for (n = 0; n < 10; n++)
        {
            int b = -1;
            for (s = 0; s < nslots; s++)
            {
                int k, used = 0;
                for (k = 0; k < n; k++) if (top[k] == s) used = 1;
                if (!used && (b < 0 || d[s] > d[b])) b = s;
            }
            top[n] = b;
        }
        { unsigned long long tot = 0; for (s = 0; s < nslots; s++) tot += d[s]; len = wsprintfA(line, "T f=%u frame_us=%u d3d_us=%u |", f, frame_us, us(tot)); }
        for (n = 0; n < 10 && top[n] >= 0; n++)
        {
            s = top[n];
            if (d[s] < (unsigned long long)tpu * 50) break;
            len += wsprintfA(line + len, " %d.%d:%ux:%uus", slotName[s] >> 8, slotName[s] & 0xFF, thunkCount[s] - snapCount[s], us(d[s]));
            if (len > 520) break;
        }
        logf("%s", line);
    }
    for (s = 0; s < nslots; s++) { snapTicks[s] = thunkTicks[s]; snapCount[s] = thunkCount[s]; }
}


// creation hooks used only to find more vtables for the profiler
typedef HRESULT (WINAPI *PFN_CQ)(IDirect3DDevice9 *, D3DQUERYTYPE, IDirect3DQuery9 **);
typedef HRESULT (WINAPI *PFN_CCT)(IDirect3DDevice9 *, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture9 **, HANDLE *);
typedef HRESULT (WINAPI *PFN_CRT)(IDirect3DDevice9 *, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9 **, HANDLE *);
static HRESULT WINAPI H_CreateQuery(IDirect3DDevice9 *d, D3DQUERYTYPE t, IDirect3DQuery9 **pp)
{ HRESULT hr = ((PFN_CQ)orig[118])(d, t, pp); if (SUCCEEDED(hr) && pp && *pp) profResource(*pp, 4); return hr; }
static HRESULT WINAPI H_CreateCubeTexture(IDirect3DDevice9 *d, UINT e, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DCubeTexture9 **pp, HANDLE *s)
{ HRESULT hr = ((PFN_CCT)orig[25])(d, e, l, u, f, p, pp, s); if (SUCCEEDED(hr)) profResource(*pp, 1); return hr; }
static HRESULT WINAPI H_CreateRenderTarget(IDirect3DDevice9 *d, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE m, DWORD q, BOOL b, IDirect3DSurface9 **pp, HANDLE *s)
{ HRESULT hr = ((PFN_CRT)orig[28])(d, w, h, f, m, q, b, pp, s); if (SUCCEEDED(hr)) profResource(*pp, 5); return hr; }
static HRESULT WINAPI H_CreateDepthStencilSurface(IDirect3DDevice9 *d, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE m, DWORD q, BOOL b, IDirect3DSurface9 **pp, HANDLE *s)
{ HRESULT hr = ((PFN_CRT)orig[29])(d, w, h, f, m, q, b, pp, s); if (SUCCEEDED(hr)) profResource(*pp, 5); return hr; }

// ---------------------------------------------------------------- device creation
static void hookDevice(IDirect3DDevice9 *dev)
{
    void **vt = *(void ***)dev;
    patch(vt, I_Present, H_Present);
    patch(vt, I_CreateTexture, H_CreateTexture);
    patch(vt, I_CreateVertexBuffer, H_CreateVertexBuffer);
    patch(vt, I_CreateIndexBuffer, H_CreateIndexBuffer);
    if (opt_trace || opt_warm || opt_bg)
    {
        patch(vt, I_CreateVertexShader, H_CreateVertexShader);
        patch(vt, I_CreatePixelShader, H_CreatePixelShader);
        patch(vt, I_CreateVertexDeclaration, H_CreateVertexDeclaration);
        patch(vt, I_SetVertexShader, H_SetVertexShader);
        patch(vt, I_SetPixelShader, H_SetPixelShader);
        patch(vt, I_SetVertexDeclaration, H_SetVertexDeclaration);
        patch(vt, I_SetFVF, H_SetFVF);
        patch(vt, I_DrawPrimitive, H_DrawPrimitive);
        patch(vt, I_DrawIndexedPrimitive, H_DrawIndexedPrimitive);
        patch(vt, I_DrawPrimitiveUP, H_DrawPrimitiveUP);
        patch(vt, I_DrawIndexedPrimitiveUP, H_DrawIndexedPrimitiveUP);
    }
    if (opt_prof)
    {
        int i;
        patch(vt, 118, H_CreateQuery); patch(vt, 25, H_CreateCubeTexture);
        patch(vt, 28, H_CreateRenderTarget); patch(vt, 29, H_CreateDepthStencilSurface);
        for (i = 3; i < 119; i++) profSlot(vt, i, 0);
    }
    logf("device %08X vtable %08X hooked (preload=%d trace=%d prof=%d warm=%d, %d recorded combinations to replay)", (DWORD)dev, (DWORD)vt, opt_preload, opt_trace, opt_prof, opt_warm, npend);
}
typedef HRESULT (WINAPI *PFN_CD)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
static HRESULT WINAPI H_CreateDevice(IDirect3D9 *d3d, UINT ad, D3DDEVTYPE ty, HWND w, DWORD fl, D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
{
    HRESULT hr = ((PFN_CD)createDeviceOrig)(d3d, ad, ty, w, fl, pp, out);
    logf("CreateDevice hr=%08X flags=%08X %ux%u fmt=%u ms=%u interval=%08X windowed=%d", hr, fl, pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, pp->MultiSampleType, pp->PresentationInterval, pp->Windowed);
    if (SUCCEEDED(hr) && *out) hookDevice(*out);
    if (SUCCEEDED(hr) && opt_bg && !bgStarted) { bgStarted = 1; bgFlags = fl; bgFmt = pp->BackBufferFormat; CloseHandle(CreateThread(NULL, 0, bgthread, NULL, 0, NULL)); }
    return hr;
}
typedef IDirect3D9 *(WINAPI *PFN_D3DC)(UINT);
static WCHAR fixDir[MAX_PATH + 16];
static void wcpy(WCHAR *d, const WCHAR *s);
static void wcat(WCHAR *d, const WCHAR *s);
static int dxvkTried;
static IDirect3D9 *WINAPI H_Direct3DCreate9(UINT sdk)
{
    IDirect3D9 *d3d;
    if (opt_dxvk && !dxvkTried)
    {   // dxvk=1 (drop-in mode): D3D9 on Vulkan from BmFix\dxvk instead of the game's d3d9 import
        WCHAR p[MAX_PATH + 64]; HMODULE m; void *fn = 0;
        dxvkTried = 1;
        wcpy(p, fixDir); wcat(p, L"\\dxvk\\dxvk.conf"); SetEnvironmentVariableW(L"DXVK_CONFIG_FILE", p);
        wcpy(p, fixDir); wcat(p, L"\\logs"); SetEnvironmentVariableW(L"DXVK_LOG_PATH", p);
        wcpy(p, fixDir); wcat(p, L"\\dxvk\\dxvk_d3d9.dll");
        if ((m = LoadLibraryW(p))) fn = (void *)GetProcAddress(m, "Direct3DCreate9");
        if (fn) d3dCreateOrig = fn;
        logf("dxvk: %s", fn ? "loaded" : "BmFix\\dxvk\\dxvk_d3d9.dll not found - native D3D9");
    }
    d3d = ((PFN_D3DC)d3dCreateOrig)(sdk);
    if (d3d && opt_hookon)
    {
        void **vt = *(void ***)d3d; DWORD old;
        if (vt[16] != (void *)H_CreateDevice)
        {
            VirtualProtect(&vt[16], 4, PAGE_EXECUTE_READWRITE, &old);
            createDeviceOrig = vt[16]; vt[16] = (void *)H_CreateDevice;
            VirtualProtect(&vt[16], 4, old, &old);
        }
    }
    logf("Direct3DCreate9(%u) -> %08X", sdk, (DWORD)d3d);
    return d3d;
}


// ---------------------------------------------------------------- level streaming time budget
// UWorld::AddToWorld (exe 1.1, fixed base 0x400000) shows a streamed level over several frames. It can spend up
// to 5 ms in each frame on this work. IsTimeLimitExceeded and the AddToWorld step loops compare the elapsed ms
// with a shared double 5.0 at 0x1E4AA08. If a frame already takes 5.5 ms, this work makes it longer than 8.3 ms.
// BmFix points the five "fld qword ptr [0x1E4AA08]" in those two functions to its own limit. The 42 other uses
// of the constant do not change. BmFix compares the bytes of each site before it changes them.
static double streamLimitMs = 5.0;
static int patchStreaming(double ms)
{
    static const DWORD sites[] = { 0x9B07FB, 0x9D0012, 0x9D0087, 0x9D012E, 0x9D056A };
    static const unsigned char expect[6] = { 0xDD, 0x05, 0x08, 0xAA, 0xE4, 0x01 };
    int i, k;
    if ((DWORD)GetModuleHandleW(NULL) != 0x400000) return 0;
    for (i = 0; i < 5; i++) for (k = 0; k < 6; k++) if (((unsigned char *)sites[i])[k] != expect[k]) return -1 - i;
    streamLimitMs = ms;
    for (i = 0; i < 5; i++)
    {
        DWORD old; void *at = (void *)(sites[i] + 2);
        VirtualProtect(at, 4, PAGE_EXECUTE_READWRITE, &old);
        *(DWORD *)at = (DWORD)&streamLimitMs;
        VirtualProtect(at, 4, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), (void *)0x9B0000, 0x30000);
    return 1;
}
// The two step loops of AddToWorld update 10 components in each step ("push 0Ah" at 0x9D0050 and 0x9D00F7).
// They read the time only between steps, so a smaller step goes less far over the time limit.
static int patchStreamStep(int n)
{
    static const DWORD sites[] = { 0x9D0050, 0x9D00F7 };
    int i;
    if (n < 1 || n > 10 || (DWORD)GetModuleHandleW(NULL) != 0x400000) return 0;
    for (i = 0; i < 2; i++) if (((unsigned char *)sites[i])[0] != 0x6A || ((unsigned char *)sites[i])[1] != 0x0A || ((unsigned char *)sites[i])[2] != 0x8B) return -1;
    for (i = 0; i < 2; i++)
    {
        DWORD old; unsigned char *at = (unsigned char *)sites[i] + 1;
        VirtualProtect(at, 1, PAGE_EXECUTE_READWRITE, &old); *at = (unsigned char)n; VirtualProtect(at, 1, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), (void *)0x9D0000, 0x200);
    return 1;
}
static double envnum(const WCHAR *name, double def)
{
    WCHAR v[32]; DWORD n = GetEnvironmentVariableW(name, v, 32); double r = 0, f = 0.1; WCHAR *c = v; int any = 0;
    if (n == 0 || n >= 32) return def;
    while (*c >= L'0' && *c <= L'9') { r = r * 10 + (*c++ - L'0'); any = 1; }
    if (*c == L'.') for (c++; *c >= L'0' && *c <= L'9'; c++) { r += f * (*c - L'0'); f *= 0.1; any = 1; }
    return any ? r : def;
}

// ---------------------------------------------------------------- small string / file helpers
static int ieq(const char *a, const char *b) { while (*a && *b) { char x = *a++, y = *b++; if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; if (x != y) return 0; } return *a == *b; }
static int nieq(const char *a, const char *b, int n) { while (n--) { char x = *a++, y = *b++; if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; if (x != y) return 0; } return 1; }
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int wlen(const WCHAR *s) { int n = 0; while (s[n]) n++; return n; }
static void wcpy(WCHAR *d, const WCHAR *s) { while ((*d++ = *s++)) ; }
static void wcat(WCHAR *d, const WCHAR *s) { wcpy(d + wlen(d), s); }
static WCHAR wlow(WCHAR c) { return c >= L'A' && c <= L'Z' ? c + 32 : c; }
static int wends(const WCHAR *s, const WCHAR *suf) { int a = wlen(s), b = wlen(suf), i; if (b > a) return 0; for (i = 0; i < b; i++) if (wlow(s[a - b + i]) != wlow(suf[i])) return 0; return 1; }
static int wfind(const WCHAR *s, const WCHAR *sub)
{
    int i, j, n = wlen(sub);
    for (i = 0; s[i]; i++) { for (j = 0; j < n && s[i + j] && wlow(s[i + j]) == wlow(sub[j]); j++) ; if (j == n) return 1; }
    return 0;
}
static void wdir(WCHAR *path) { WCHAR *e = path + wlen(path); while (e > path && e[-1] != L'\\') e--; *e = 0; }   // keeps the trailing backslash
static char *readAll(const WCHAR *path, DWORD *len)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    DWORD n, got = 0; char *b;
    if (h == INVALID_HANDLE_VALUE) return 0;
    n = GetFileSize(h, NULL);
    b = HeapAlloc(GetProcessHeap(), 0, n + 1);
    if (b && !ReadFile(h, b, n, &got, NULL)) got = 0;
    CloseHandle(h);
    if (!b) return 0;
    b[got] = 0; if (len) *len = got;
    return b;
}
static int writeAll(const WCHAR *path, const void *b, DWORD n)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); DWORD w = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    WriteFile(h, b, n, &w, NULL); CloseHandle(h);
    return w == n;
}
// Gets the next line of a text buffer. *s and *n get the line without CR/LF. Returns the start of the line after
// it, or 0 at the end of the buffer.
static const char *nextLine(const char *p, const char **s, int *n)
{
    const char *e = p;
    if (!*p) return 0;
    while (*e && *e != '\n') e++;
    *s = p; *n = (int)(e - p);
    if (*n && p[*n - 1] == '\r') (*n)--;
    return *e ? e + 1 : e;
}

// ---------------------------------------------------------------- PhysX cloth cooking cache (physx_cache=1)
// With PhysX on, the game cooks each cloth mesh (banners, flags, capes) on the game thread the first time that its
// level becomes visible in a session. Each mesh takes 10-20 ms in PhysXCooking.dll!NxCookClothMesh, and the game
// waits for it. The cooked result depends only on the mesh description and the cooking parameters. BmFix keeps it
// in BmFixPhysX.bin. In later sessions, BmFix writes the kept bytes to the stream of the game and does not cook.
// Before BmFix hooks the function, it compares the exact first bytes of NxCookClothMesh and the build of
// PhysXCooking.dll (PhysX 2.8.1, TimeDateStamp 60ED96E7, cooking parameters at RVA 57CB8). If either one is
// different, BmFix does not hook, and the game cooks as usual.
static int opt_physx;
typedef unsigned char (__cdecl *PFN_COOK)(const void *desc, void *stream);
static PFN_COOK cookClothOrig;
static unsigned char *cookMod;
static DWORD cookStamp;
static volatile int pxState;                 // 0 = not hooked yet, 1 = hooked, -1 = gave up
typedef struct { unsigned long long key; unsigned len; unsigned char *data; } PxEnt;
static PxEnt *pxc; static int npxc, cpxc;
static HANDLE pxFile = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION pxcs;
static unsigned pxHits, pxMiss;
static unsigned long long h64(unsigned long long h, const void *p, unsigned n) { const unsigned char *b = p; while (n--) { h ^= *b++; h *= 1099511628211ull; } return h; }
static int readable(const void *p, unsigned n)
{
    MEMORY_BASIC_INFORMATION mbi; const unsigned char *a = p, *e = a + n;
    if (!n) return 1;
    if (!p) return 0;
    while (a < e)
    {
        if (!VirtualQuery(a, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !mbi.Protect) return 0;
        a = (const unsigned char *)mbi.BaseAddress + mbi.RegionSize;
    }
    return 1;
}
// NxClothMeshDesc (PhysX 2.8.1, as read by NxCookClothMesh): +0 numVertices, +4 numTriangles, +8 pointStrideBytes,
// +C triangleStrideBytes, +10 points, +14 triangles, +18 flags (2 = 16-bit indices), +1C vertexMassStrideBytes,
// +20 vertexFlagStrideBytes, +24 vertexMasses, +28 vertexFlags, +2C weldingDistance
static int clothKey(const DWORD *d, unsigned long long *out)
{
    unsigned nv, nt, ps, ts, fl, ms, fs, i, isz; const unsigned char *pts, *tri, *mas, *vfl;
    unsigned long long h = 14695981039346656037ull;
    if (!readable(d, 48)) return 0;
    nv = d[0]; nt = d[1]; ps = d[2]; ts = d[3]; fl = d[6]; ms = d[7]; fs = d[8];
    pts = (const unsigned char *)d[4]; tri = (const unsigned char *)d[5]; mas = (const unsigned char *)d[9]; vfl = (const unsigned char *)d[10];
    isz = (fl & 2) ? 2 : 4;
    if (!nv || nv > 1000000 || nt > 4000000 || ps < 12 || ps > 1024 || !pts) return 0;
    if (tri && nt && (ts < 3 * isz || ts > 1024)) return 0;
    if (!readable(pts, (nv - 1) * ps + 12)) return 0;
    if (tri && nt && !readable(tri, (nt - 1) * ts + 3 * isz)) return 0;
    if (mas && (ms < 4 || ms > 1024 || !readable(mas, (nv - 1) * ms + 4))) return 0;
    if (vfl && (fs < 4 || fs > 1024 || !readable(vfl, (nv - 1) * fs + 4))) return 0;
    h = h64(h, "BmFix cloth 1", 13);
    h = h64(h, &nv, 4); h = h64(h, &nt, 4); h = h64(h, &fl, 4); h = h64(h, &d[11], 4);
    for (i = 0; i < nv; i++) h = h64(h, pts + i * ps, 12);
    if (tri) for (i = 0; i < nt; i++) h = h64(h, tri + i * ts, 3 * isz);
    h = h64(h, mas ? "m" : "-", 1); if (mas) for (i = 0; i < nv; i++) h = h64(h, mas + i * ms, 4);
    h = h64(h, vfl ? "f" : "-", 1); if (vfl) for (i = 0; i < nv; i++) h = h64(h, vfl + i * fs, 4);
    h = h64(h, cookMod + 0x57CB8, 12);       // NxCookingParams (target platform, skin width, hint)
    h = h64(h, &cookStamp, 4);
    *out = h;
    return 1;
}
// A write-only NxStream that records the bytes that the cooker writes. Its vtable has the MSVC thiscall layout:
// destructor, 6 read methods, 6 store methods.
typedef struct Rec { void **vt; unsigned char *buf; unsigned len, cap; } Rec;
#define TC __attribute__((thiscall))
static void recPut(Rec *r, const void *p, unsigned n)
{
    if (r->len + n > r->cap)
    {
        unsigned c = r->cap ? r->cap * 2 : 65536; unsigned char *nb;
        while (c < r->len + n) c *= 2;
        if (!(nb = HeapAlloc(GetProcessHeap(), 0, c))) return;
        if (r->buf) { memcpy(nb, r->buf, r->len); HeapFree(GetProcessHeap(), 0, r->buf); }
        r->buf = nb; r->cap = c;
    }
    memcpy(r->buf + r->len, p, n); r->len += n;
}
static void *TC rec_dtor(Rec *r, unsigned f) { return r; }
static unsigned char TC rec_rb(Rec *r) { return 0; }
static unsigned short TC rec_rw(Rec *r) { return 0; }
static unsigned TC rec_rd(Rec *r) { return 0; }
static float TC rec_rf(Rec *r) { return 0; }
static double TC rec_rdb(Rec *r) { return 0; }
static void TC rec_rbuf(Rec *r, void *b, unsigned n) { memset(b, 0, n); }
static Rec *TC rec_sb(Rec *r, unsigned char v) { recPut(r, &v, 1); return r; }
static Rec *TC rec_sw(Rec *r, unsigned short v) { recPut(r, &v, 2); return r; }
static Rec *TC rec_sd(Rec *r, unsigned v) { recPut(r, &v, 4); return r; }
static Rec *TC rec_sf(Rec *r, float v) { recPut(r, &v, 4); return r; }
static Rec *TC rec_sdb(Rec *r, double v) { recPut(r, &v, 8); return r; }
static Rec *TC rec_sbuf(Rec *r, const void *b, unsigned n) { recPut(r, b, n); return r; }
static void *recVt[13] = { (void *)rec_dtor, (void *)rec_rb, (void *)rec_rw, (void *)rec_rd, (void *)rec_rf, (void *)rec_rdb, (void *)rec_rbuf,
                           (void *)rec_sb, (void *)rec_sw, (void *)rec_sd, (void *)rec_sf, (void *)rec_sdb, (void *)rec_sbuf };
typedef void *(TC *PFN_SBUF)(void *self, const void *b, unsigned n);
static void streamStore(void *s, const void *b, unsigned n) { ((PFN_SBUF)(*(void ***)s)[12])(s, b, n); }   // NxStream::storeBuffer
static void pxAdd(unsigned long long key, unsigned char *data, unsigned len, int save)
{
    EnterCriticalSection(&pxcs);
    if (npxc == cpxc)
    {
        int c = cpxc ? cpxc * 2 : 256; PxEnt *n = HeapAlloc(GetProcessHeap(), 0, c * sizeof(PxEnt));
        if (n) { if (pxc) { memcpy(n, pxc, npxc * sizeof(PxEnt)); HeapFree(GetProcessHeap(), 0, pxc); } pxc = n; cpxc = c; }
    }
    if (npxc < cpxc) { pxc[npxc].key = key; pxc[npxc].len = len; pxc[npxc].data = data; npxc++; }
    LeaveCriticalSection(&pxcs);
    if (save && pxFile != INVALID_HANDLE_VALUE)
    {
        DWORD w; unsigned char hd[16];
        memcpy(hd, "BFPX", 4); memcpy(hd + 4, &key, 8); memcpy(hd + 12, &len, 4);
        WriteFile(pxFile, hd, 16, &w, NULL); WriteFile(pxFile, data, len, &w, NULL);
    }
}
static unsigned char __cdecl H_CookCloth(const void *desc, void *stream)
{
    unsigned long long key, t0 = rdtsc(); int k; unsigned char ok; Rec r;
    if (!clothKey(desc, &key)) return cookClothOrig(desc, stream);
    EnterCriticalSection(&pxcs);
    for (k = 0; k < npxc && pxc[k].key != key; k++) ;
    if (k < npxc)
    {
        unsigned char *data = pxc[k].data; unsigned len = pxc[k].len;
        LeaveCriticalSection(&pxcs);
        streamStore(stream, data, len);
        pxHits++;
        logf("physx: cloth mesh %08X%08X (%u vertices) from cache, %u bytes in %u us", (DWORD)(key >> 32), (DWORD)key, ((const DWORD *)desc)[0], len, us(rdtsc() - t0));
        return 1;
    }
    LeaveCriticalSection(&pxcs);
    r.vt = recVt; r.buf = 0; r.len = r.cap = 0;
    ok = cookClothOrig(desc, &r);
    pxMiss++;
    if (ok && r.len) { streamStore(stream, r.buf, r.len); pxAdd(key, r.buf, r.len, 1); }
    else if (r.buf) HeapFree(GetProcessHeap(), 0, r.buf);
    logf("physx: cloth mesh %08X%08X (%u vertices) cooked in %u us, %u bytes%s", (DWORD)(key >> 32), (DWORD)key, ((const DWORD *)desc)[0], us(rdtsc() - t0), r.len, ok ? " - cached" : " - FAILED");
    return ok;
}
static void physxLoad(const WCHAR *path)
{
    DWORD n = 0; char *b = readAll(path, &n); unsigned o = 0, cnt = 0;
    if (b)
    {
        while (o + 16 <= n && !memcmp(b + o, "BFPX", 4))
        {
            unsigned long long key; unsigned len; unsigned char *d;
            memcpy(&key, b + o + 4, 8); memcpy(&len, b + o + 12, 4);
            if (len == 0 || len > 64 * 1024 * 1024 || o + 16 + len > n) break;
            if (!(d = HeapAlloc(GetProcessHeap(), 0, len))) break;
            memcpy(d, b + o + 16, len); pxAdd(key, d, len, 0); cnt++;
            o += 16 + len;
        }
        HeapFree(GetProcessHeap(), 0, b);
    }
    pxFile = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    logf("physx: cloth cooking cache %ls, %u meshes", path, cnt);
}
// The log thread calls this until PhysXCooking.dll is loaded. The PhysX runtime loads it while the game starts.
static void physxPoll(void)
{
    static const unsigned char pro[9] = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x28, 0x01, 0x00, 0x00 };
    HMODULE m; unsigned char *f, *t; DWORD old; IMAGE_NT_HEADERS *nt;
    if (!opt_physx || pxState) return;
    if (!(m = GetModuleHandleW(L"PhysXCooking.dll"))) return;
    nt = (IMAGE_NT_HEADERS *)((BYTE *)m + ((IMAGE_DOS_HEADER *)m)->e_lfanew);
    f = (unsigned char *)GetProcAddress(m, "NxCookClothMesh");
    if (nt->FileHeader.TimeDateStamp != 0x60ED96E7 || !f || memcmp(f, pro, 9))
    {
        pxState = -1;
        logf("physx: PhysXCooking.dll build %08X not the tested one (60ED96E7) - cloth cache off", nt->FileHeader.TimeDateStamp);
        return;
    }
    cookMod = (unsigned char *)m; cookStamp = nt->FileHeader.TimeDateStamp;
    if (!(t = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))) { pxState = -1; return; }
    memcpy(t, f, 9); t[9] = 0xE9; *(DWORD *)(t + 10) = (DWORD)(f + 9) - (DWORD)(t + 14);   // trampoline: prologue + jmp back
    cookClothOrig = (PFN_COOK)t;
    VirtualProtect(f, 5, PAGE_EXECUTE_READWRITE, &old);
    *(DWORD *)(f + 1) = (DWORD)H_CookCloth - (DWORD)(f + 5); f[0] = 0xE9;
    VirtualProtect(f, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), f, 5);
    pxState = 1;
    logf("physx: NxCookClothMesh hooked (PhysXCooking.dll %08X)", cookStamp);
}

// ---------------------------------------------------------------- BmFix.ini (flags above the first [section], [overrides])
static WCHAR dllDir[MAX_PATH];
static char *cfgText;
static int cfgValue(const char *key, char *out, int cap)
{
    const char *p = cfgText, *s; int n, k = slen(key);
    if (!p) return 0;
    while ((p = nextLine(p, &s, &n)))
    {
        while (n && (*s == ' ' || *s == '\t')) { s++; n--; }
        if (n && *s == '[') return 0;
        if (n > k && s[k] == '=' && nieq(s, key, k))
        {
            int i = 0; s += k + 1; n -= k + 1;
            while (n && (*s == ' ' || *s == '\t')) { s++; n--; }
            while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
            for (; i < n && i < cap - 1; i++) out[i] = s[i];
            out[i] = 0;
            return 1;
        }
    }
    return 0;
}
static int envflag(const WCHAR *name, int def) { WCHAR v[8]; DWORD n = GetEnvironmentVariableW(name, v, 8); return n == 0 ? def : v[0] != L'0'; }
static int optflag(const WCHAR *env, const char *key, int def)   // environment (tests) > BmFix.ini > default
{
    WCHAR v[8]; char c[16];
    if (GetEnvironmentVariableW(env, v, 8)) return v[0] != L'0';
    if (cfgValue(key, c, sizeof c)) return c[0] == '1' || c[0] == 't' || c[0] == 'T';
    return def;
}
static double parsenum(const char *c, double def)
{
    double r = 0, f = 0.1; int any = 0;
    while (*c >= '0' && *c <= '9') { r = r * 10 + (*c++ - '0'); any = 1; }
    if (*c == '.' || *c == ',') for (c++; *c >= '0' && *c <= '9'; c++) { r += f * (*c - '0'); f *= 0.1; any = 1; }
    return any ? r : def;
}
static double optnum(const WCHAR *env, const char *key, double def)
{
    char c[32];
    double e = envnum(env, -1);
    if (e >= 0) return e;
    return cfgValue(key, c, sizeof c) ? parsenum(c, def) : def;
}

// ---------------------------------------------------------------- engine ini copy + localization folder (drop-in mode)
// The engine gets -ENGINEINI= with a read-only copy of the BmEngine.ini of the user. The copy has the
// [overrides] of BmFix.ini, and its LocalizationPaths= points to BmFix\Localization. That folder has copies of the
// .int files of the game and empty files for the ~110 names that the game looks for but does not have.
static WCHAR iniArgW[MAX_PATH + 32];
static int prepLocalization(char *locA, int cap)
{
    WCHAR root[MAX_PATH + 32], intDir[MAX_PATH + 48], game[MAX_PATH + 16], pat[MAX_PATH + 96], src[MAX_PATH + 96], dst[MAX_PATH + 96];
    static const WCHAR *subs[] = { L"Engine\\Localization\\INT\\", L"BmGame\\Localization\\INT\\" };
    int copied = 0, stubs = 0, i; BOOL lossy = FALSE; char *list;
    wcpy(root, fixDir); wcat(root, L"\\Localization"); CreateDirectoryW(root, NULL);
    wcpy(intDir, root); wcat(intDir, L"\\INT"); CreateDirectoryW(intDir, NULL);
    GetModuleFileNameW(NULL, game, MAX_PATH); wdir(game); wcat(game, L"..\\");
    for (i = 0; i < 2; i++)
    {
        WIN32_FIND_DATAW fd; HANDLE h;
        wcpy(pat, game); wcat(pat, subs[i]); wcat(pat, L"*.int");
        h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do
        {
            wcpy(src, game); wcat(src, subs[i]); wcat(src, fd.cFileName);
            wcpy(dst, intDir); wcat(dst, L"\\"); wcat(dst, fd.cFileName);
            SetFileAttributesW(dst, FILE_ATTRIBUTE_NORMAL);
            if (CopyFileW(src, dst, FALSE)) copied++;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    wcpy(pat, root); wcat(pat, L"\\stubs.txt");
    if ((list = readAll(pat, NULL)))
    {
        static const char stub[] = "; empty on purpose: the game looks this file up but never shipped it (BmFix stub)\r\n";
        const char *p = list, *s; int n;
        while ((p = nextLine(p, &s, &n)))
        {
            WCHAR name[128]; int k;
            while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
            if (!n || n > 100) continue;
            for (k = 0; k < n; k++) name[k] = (WCHAR)(unsigned char)s[k];
            name[n] = 0;
            wcpy(dst, intDir); wcat(dst, L"\\"); wcat(dst, name); wcat(dst, L".INT");
            if (GetFileAttributesW(dst) == INVALID_FILE_ATTRIBUTES) writeAll(dst, stub, sizeof stub - 1);
            stubs++;
        }
        HeapFree(GetProcessHeap(), 0, list);
    }
    if (!copied) { logf("localization: game .int files not found - left as is"); return 0; }
    if (!WideCharToMultiByte(CP_ACP, 0, root, -1, locA, cap, NULL, &lossy) || lossy) { logf("localization: folder path not representable in the ini - left as is"); return 0; }
    logf("localization: %d game files + %d stubs in %ls", copied, stubs, root);
    return 1;
}
static void prepare(void)
{
    WCHAR src[MAX_PATH + 128], tmp[MAX_PATH + 32], dst[MAX_PATH + 64];
    char locA[MAX_PATH * 2]; const char *ovk[64], *ovv[64]; int ovkn[64], ovvn[64], nov = 0;
    DWORD n; char *in, *out; int o = 0, haveLoc = 0, locPut = 0, inOv = 0;
    const char *p, *s; int ln;
    if (GetEnvironmentVariableW(L"BMFIX_SRCINI", src, MAX_PATH) == 0)
    {
        WCHAR docs[MAX_PATH];
        if (FAILED(SHGetFolderPathW(NULL, 5 /*CSIDL_PERSONAL*/, NULL, 0, docs))) { logf("ini: Documents folder not found - the game uses its own config"); return; }
        wcpy(src, docs); wcat(src, L"\\Square Enix\\Batman Arkham Asylum GOTY\\BmGame\\Config\\BmEngine.ini");
    }
    in = readAll(src, &n);
    if (!in) { logf("ini: %ls not found - the game uses its own config", src); return; }
    if (n >= 2 && (unsigned char)in[0] == 0xFF && (unsigned char)in[1] == 0xFE) { logf("ini: %ls is UTF-16 - left as is", src); return; }
    // [overrides] of BmFix.ini
    for (p = cfgText; p && (p = nextLine(p, &s, &ln)); )
    {
        while (ln && (*s == ' ' || *s == '\t')) { s++; ln--; }
        if (ln && *s == '[') { inOv = ln >= 11 && nieq(s, "[overrides]", 11); continue; }
        if (inOv && ln && *s != ';' && nov < 64)
        {
            int e = 0; while (e < ln && s[e] != '=') e++;
            if (e > 0 && e < ln) { ovk[nov] = s; ovkn[nov] = e; ovv[nov] = s + e + 1; ovvn[nov] = ln - e - 1; nov++; }
        }
    }
    // localization (Language=int only)
    if (optflag(L"BMFIX_LOCALIZATION", "localization", 1))
    {
        int isInt = 1;
        for (p = in; (p = nextLine(p, &s, &ln)); )
            if (ln >= 9 && nieq(s, "Language=", 9)) { isInt = ln - 9 >= 3 && nieq(s + 9, "int", 3); break; }
        if (isInt) haveLoc = prepLocalization(locA, sizeof locA);
        else logf("localization: language is not INT - left as is");
    }
    out = HeapAlloc(GetProcessHeap(), 0, n + 4096 + 64 * 256);
    if (!out) return;
    for (p = in; (p = nextLine(p, &s, &ln)); )
    {
        int e, k, done = 0;
        if (haveLoc && ln >= 18 && nieq(s, "LocalizationPaths=", 18))
        {
            if (!locPut) { memcpy(out + o, "LocalizationPaths=", 18); o += 18; k = slen(locA); memcpy(out + o, locA, k); o += k; out[o++] = '\r'; out[o++] = '\n'; locPut = 1; }
            continue;
        }
        for (e = 0; e < ln && s[e] != '='; e++) ;
        if (e > 0 && e < ln)
            for (k = 0; k < nov; k++)
                if (ovkn[k] == e && nieq(s, ovk[k], e))
                {
                    memcpy(out + o, s, e + 1); o += e + 1; memcpy(out + o, ovv[k], ovvn[k]); o += ovvn[k];
                    done = 1; break;
                }
        if (!done) { memcpy(out + o, s, ln); o += ln; }
        out[o++] = '\r'; out[o++] = '\n';
    }
    // UE3 stops the -ENGINEINI= value at the first space, so the copy goes to a temp path with no spaces (8.3 name).
    GetTempPathW(MAX_PATH, tmp); wcat(tmp, L"BmFix"); CreateDirectoryW(tmp, NULL);
    if (wfind(tmp, L" ")) { WCHAR sh[MAX_PATH]; if (GetShortPathNameW(tmp, sh, MAX_PATH)) wcpy(tmp, sh); }
    if (wfind(tmp, L" ")) { logf("ini: temp path has spaces (no 8.3 name) - the game uses its own config"); return; }
    wcpy(dst, tmp); wcat(dst, L"\\BmEngine.ini");
    SetFileAttributesW(dst, FILE_ATTRIBUTE_NORMAL);
    if (!writeAll(dst, out, o)) { logf("ini: could not write %ls", dst); return; }
    SetFileAttributesW(dst, FILE_ATTRIBUTE_READONLY);   // UE3 writes a new file over a copy that is not read-only
    wcpy(iniArgW, L" -ENGINEINI="); wcat(iniArgW, dst);
    logf("ini: %ls -> %ls (%d overrides, localization %s)", src, dst, nov, haveLoc ? "on" : "off");
    HeapFree(GetProcessHeap(), 0, in); HeapFree(GetProcessHeap(), 0, out);
}
// The game reads its command line through its GetCommandLineW import. At the first call, BmFix makes the ini copy
// and adds -ENGINEINI=... to the command line. If the command line already has -ENGINEINI (from the test harness
// or the user), BmFix does not add it.
typedef LPWSTR (WINAPI *PFN_GCL)(void);
static PFN_GCL gclOrig;
static LPWSTR volatile gclNew;
static CRITICAL_SECTION cmdcs;
static LPWSTR WINAPI H_GetCommandLineW(void)
{
    if (gclNew) return gclNew;
    EnterCriticalSection(&cmdcs);
    if (!gclNew)
    {
        LPWSTR o = gclOrig();
        if (wfind(o, L"-ENGINEINI=")) { logf("command line already sets -ENGINEINI - kept"); gclNew = o; }
        else
        {
            prepare();
            if (iniArgW[0])
            {
                int a = wlen(o), b = wlen(iniArgW);
                LPWSTR m = HeapAlloc(GetProcessHeap(), 0, 2 * (a + b + 1));
                if (m) { memcpy(m, o, 2 * a); memcpy(m + a, iniArgW, 2 * (b + 1)); gclNew = m; logf("command line +%ls", iniArgW); }
                else gclNew = o;
            }
            else gclNew = o;
        }
    }
    LeaveCriticalSection(&cmdcs);
    return gclNew;
}

// ---------------------------------------------------------------- export forwarding (drop-in: BmFix is the game's faultrep.dll)
// The game imports faultrep.dll (Windows Error Reporting) only for ReportFault, which it calls when it crashes.
// No other mod uses this name, so BmFix works together with d3d9.dll, dinput8.dll, and dxgi.dll mods (ReShade,
// DXVK, ASI loaders). Each export is a thunk to the real faultrep.dll of Windows, which BmFix loads at the first
// call.
static const char *const pxNames[] = { "AddERExcludedApplicationA", "AddERExcludedApplicationW", "BasepReportFault", "CancelHangReporting",
    "CheckForReadOnlyResourceFilter", "CheckPerUserCrossProcessThrottle", "DllCanUnloadNow", "DllGetClassObject", "ReportCoreHang",
    "ReportFault", "ReportHang", "UpdatePerUserLastCrossProcessCollectionTime", "WerReportHang", "WerpGetDebugger",
    "WerpInitiateCrashReporting", "WerpLaunchAeDebug", (const char *)1, (const char *)2 };
#define PX_N 18
void *pxReal[PX_N] __attribute__((used));
static HMODULE pxMod;
extern void px_missing(void);
void __cdecl px_resolve(int i) __attribute__((used));
void __cdecl px_resolve(int i)
{
    if (!pxMod) { WCHAR p[MAX_PATH + 32]; UINT n = GetSystemDirectoryW(p, MAX_PATH); wcpy(p + n, L"\\faultrep.dll"); pxMod = LoadLibraryW(p); }
    pxReal[i] = pxMod ? (void *)GetProcAddress(pxMod, pxNames[i]) : 0;
    if (!pxReal[i]) pxReal[i] = (void *)px_missing;
}
#define PXT(n) ".globl _px_" #n "\n_px_" #n ":\n  mov $" #n ", %eax\n  jmp px_common\n"
__asm__(
    ".text\n"
    "px_common:\n"
    "  mov _pxReal(,%eax,4), %ecx\n"
    "  test %ecx, %ecx\n"
    "  jz 1f\n"
    "  jmp *%ecx\n"
    "1:\n"
    "  pushal\n"
    "  push %eax\n"
    "  call _px_resolve\n"
    "  add $4, %esp\n"
    "  popal\n"
    "  mov _pxReal(,%eax,4), %ecx\n"
    "  jmp *%ecx\n"
    ".globl _px_missing\n_px_missing:\n  xor %eax, %eax\n  ret\n"
    PXT(0) PXT(1) PXT(2) PXT(3) PXT(4) PXT(5) PXT(6) PXT(7) PXT(8) PXT(9) PXT(10) PXT(11) PXT(12) PXT(13) PXT(14) PXT(15) PXT(16) PXT(17));
void BmFixProxy(void) { }   // marker export: "this faultrep.dll is BmFix"

// ---------------------------------------------------------------- init
static void **findImport(HMODULE m, const char *dll, const char *fn)
{
    BYTE *b = (BYTE *)m;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *id = (IMAGE_IMPORT_DESCRIPTOR *)(b + dd->VirtualAddress);
    for (; dd->VirtualAddress && id->Name; id++)
    {
        IMAGE_THUNK_DATA *on, *ft;
        if (!ieq((char *)(b + id->Name), dll)) continue;
        on = (IMAGE_THUNK_DATA *)(b + (id->OriginalFirstThunk ? id->OriginalFirstThunk : id->FirstThunk));
        ft = (IMAGE_THUNK_DATA *)(b + id->FirstThunk);
        for (; on->u1.AddressOfData; on++, ft++)
            if (!(on->u1.Ordinal & IMAGE_ORDINAL_FLAG) && ieq((char *)((IMAGE_IMPORT_BY_NAME *)(b + on->u1.AddressOfData))->Name, fn))
                return (void **)&ft->u1.Function;
    }
    return 0;
}
static void setImport(void **cell, void *fn, void **origOut)
{
    DWORD old;
    VirtualProtect(cell, 4, PAGE_READWRITE, &old);
    if (origOut) *origOut = *cell;
    *cell = fn;
    VirtualProtect(cell, 4, old, &old);
}
static int initialized, proxyMode;
static void init(HMODULE self, int proxy)
{
    WCHAR path[MAX_PATH + 64]; DWORD n; void **cell = 0; LARGE_INTEGER f, q0, q1; unsigned long long t0;
    initialized = 1; proxyMode = proxy;
    InitializeCriticalSection(&logcs); InitializeCriticalSection(&bgcs); InitializeCriticalSection(&cmdcs);
    GetModuleFileNameW(self, dllDir, MAX_PATH); wdir(dllDir);
    if (!GetEnvironmentVariableW(L"BMFIX_DIR", fixDir, MAX_PATH)) { wcpy(fixDir, dllDir); if (proxy) wcat(fixDir, L"BmFix"); else fixDir[wlen(fixDir) - 1] = 0; }
    wcpy(path, fixDir); wcat(path, L"\\BmFix.ini");
    cfgText = readAll(path, NULL);
    opt_hookon = optflag(L"BMFIX_HOOK", "hook", 1);
    opt_dxvk = proxy && optflag(L"BMFIX_DXVK", "dxvk", 0);     // when injected, the host loads DXVK itself
    opt_preload = opt_hookon && !opt_dxvk && optflag(L"BMFIX_PRELOAD", "preload", 1);
    opt_warm = opt_hookon && !opt_dxvk && optflag(L"BMFIX_WARM", "warm", 1);
    opt_trace = opt_hookon && optflag(L"BMFIX_TRACE", "trace", 0);
    opt_prof = opt_hookon && envflag(L"BMFIX_PROF", 0);
    opt_bg = opt_hookon && envflag(L"BMFIX_BG", 0);
    opt_physx = opt_hookon && optflag(L"BMFIX_PHYSX", "physx_cache", 1);
    InitializeCriticalSection(&pxcs);
    warmBudget = (int)envnum(L"BMFIX_WARM_BUDGET", 32);
    n = GetEnvironmentVariableW(L"BMFIX_HOOKLOG", path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { wcpy(path, fixDir); wcat(path, L"\\logs"); CreateDirectoryW(path, NULL); wcat(path, L"\\BmFixHook.log"); }
    logfile = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (opt_warm || opt_bg)
    {
        n = GetEnvironmentVariableW(L"BMFIX_COMBOS", path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) { wcpy(path, fixDir); wcat(path, L"\\BmFixCombos.bin"); }
        warm_load(path);
    }
    if (opt_physx)
    {
        n = GetEnvironmentVariableW(L"BMFIX_PHYSXCACHE", path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) { wcpy(path, fixDir); wcat(path, L"\\BmFixPhysX.bin"); }
        physxLoad(path);
    }
    CloseHandle(CreateThread(NULL, 0, logthread, NULL, 0, NULL));
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&q0); t0 = rdtsc();
    Sleep(proxy ? 10 : 30);
    QueryPerformanceCounter(&q1);
    {
        double ticks = (double)(long long)(rdtsc() - t0), usec = (double)(long long)(q1.QuadPart - q0.QuadPart) * 1e6 / (double)(long long)f.QuadPart;
        tpu = (unsigned)(ticks / usec); if (!tpu) tpu = 1;
    }
    slowDrawTicks = (unsigned long long)tpu * 500;       // draw call > 0.5 ms
    slowFrameTicks = (unsigned long long)tpu * 8333;     // frame > 8.33 ms (120 fps)
    logf("BmFix %s (%ls), settings %ls\\BmFix.ini%s", proxy ? "drop-in faultrep.dll" : "injected", dllDir, fixDir, cfgText ? "" : " (not found: defaults)");
    if (opt_hookon || opt_dxvk)
    {
        // wraps whatever the game's d3d9 import is bound to: the system d3d9.dll or another mod's d3d9.dll (ReShade...)
        cell = findImport(GetModuleHandleW(NULL), "d3d9.dll", "Direct3DCreate9");
        if (cell)
        {
            HMODULE m = 0; WCHAR who[MAX_PATH] = L"?";
            setImport(cell, (void *)H_Direct3DCreate9, &d3dCreateOrig);
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)d3dCreateOrig, &m)) GetModuleFileNameW(m, who, MAX_PATH);
            logf("Direct3DCreate9 hooked (game's d3d9 import is %ls)", who);
        }
        else logf("Direct3DCreate9 import NOT FOUND");
    }
    if (opt_hookon)
    {
        double ms = optnum(L"BMFIX_STREAM_MS", "stream_ms", proxy ? 1.0 : 0);
        int r = ms > 0 ? patchStreaming(ms) : 0;
        logf("level streaming budget: %s (%d.%02d ms)", r == 1 ? "patched" : r == 0 ? "unchanged" : "NOT PATCHED (unexpected exe bytes)", (int)streamLimitMs, (int)(streamLimitMs * 100) % 100);
        { int st = (int)envnum(L"BMFIX_STREAM_STEP", 0); if (st) logf("level streaming step: %d components (%s)", st, patchStreamStep(st) == 1 ? "patched" : "NOT PATCHED"); }
    }
    if (proxy || envflag(L"BMFIX_PREPARE", 0))
    {
        void **c = findImport(GetModuleHandleW(NULL), "kernel32.dll", "GetCommandLineW");
        if (c) setImport(c, (void *)H_GetCommandLineW, (void **)&gclOrig);
        logf("engine ini / localization fix: %s", c ? "on (applied when the game reads its command line)" : "GetCommandLineW import NOT FOUND");
    }
    logf("options: hook=%d preload=%d warm=%d trace=%d physx_cache=%d dxvk=%d, tsc %u/us", opt_hookon, opt_preload, opt_warm, opt_trace, opt_physx, opt_dxvk, tpu);
}

// Injected mode, for a test harness. The harness calls LoadLibraryW while the main thread of the game waits at its
// entry point, and then calls Init from a second remote thread. If the drop-in faultrep.dll already runs, Init
// does nothing.
DWORD WINAPI Init(LPVOID p)
{
    WCHAR v[4];
    if (initialized || GetEnvironmentVariableW(L"BMFIX_PROXY", v, 4)) return 1;
    init((HMODULE)p, envflag(L"BMFIX_FORCEPROXY", 0));
    return d3dCreateOrig || !opt_hookon ? 1 : 0;
}
// Drop-in mode. The game loads this file as its faultrep.dll before its entry point runs. If another program loads
// it from the same folder, that program gets only the forwarding to the real faultrep.dll.
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p)
{
    if (r == DLL_PROCESS_ATTACH)
    {
        WCHAR me[MAX_PATH], exe[MAX_PATH];
        DisableThreadLibraryCalls(h);
        GetModuleFileNameW(h, me, MAX_PATH); GetModuleFileNameW(NULL, exe, MAX_PATH);
        if (wends(me, L"\\faultrep.dll") && wends(exe, L"\\ShippingPC-BmGame.exe"))
        {
            SetEnvironmentVariableW(L"BMFIX_PROXY", L"1");
            init(h, 1);
        }
    }
    return TRUE;
}

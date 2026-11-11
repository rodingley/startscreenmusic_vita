/*
 * Start Screen Music
 *
 * Puts the Music app's controls on the PS Vita's start screen (lock screen):
 * the quick menu's own Music row (volume bar, previous / play / next), laid
 * out like the user's concept and scaled into the clock's box, with the
 * song's cover at the top left (the Music app's own no-cover jacket, read
 * from the Vita, when it has none).
 * Shown while a song is playing or paused, with its title and artist.
 * Every word and picture on it is the song's or Sony's own, from the Vita
 * itself, in the system language.
 *
 * How (each found on 3.65 with test builds, see git history / README):
 * - SceShell's start screen setup function is hooked (offset from Skinpeel's
 *   source); its object's third word is the start screen's page.
 * - The row is the quick menu's template 0x8B23313D from impose_plugin,
 *   opened into the clock's group so it fades with the clock on a peel.
 * - Buttons get ButtonBase::CB_BTN_DECIDE; music commands and status go to
 *   SceShell's music service (method numbers from libSceMusic).
 * - The start screen passes no drags to widgets, so the volume bar reads the
 *   touch panel itself, while a marker listener on the page says the start
 *   screen is still there.
 * - No PAF function is imported: ScePaf isn't loaded when taiHEN starts the
 *   shell's plugins, and one unresolved import keeps the plugin from starting.
 *   PAF functions are looked up at run time instead.
 *
 * - Covers are found, read and made small on a thread of their own
 *   (cover_image.cpp); the music and cover threads sleep until needed.
 *
 * Log: ur0:tai/startscreenmusic_log.txt (the boot before: ..._prev.txt): one
 * line a song, and anything that went wrong.
 */

#include <psp2/avconfig.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/touch.h>
#include <taihen.h>

#include <limits>  // before PAF's headers: their vector uses it without including it
#include <stdarg.h>
#include <stdint.h>

#include <paf.h>

#include "cover_image.h"
#include "updater.h"

// ur0, not ux0: when SceShell starts its plugins the memory card (SD2Vita
// via StorageMgr) is not mounted yet.
#define LOG_PATH "ur0:tai/startscreenmusic_log.txt"
#define LOG_PREV "ur0:tai/startscreenmusic_log_prev.txt"

// No C runtime in SceShell: what the compiler may call, from the kernel's.
extern "C" void* memcpy(void* d, const void* s, size_t n) { return sceClibMemcpy(d, s, n); }
extern "C" void* memset(void* d, int c, size_t n) { return sceClibMemset(d, c, n); }
extern "C" int memcmp(const void* a, const void* b, size_t n) { return sceClibMemcmp(a, b, n); }
// The event listeners below live in static storage; their deleting
// destructors reference operator delete, which must free nothing.
void operator delete(void*) noexcept {}
void operator delete(void*, size_t) noexcept {}

// ── Log ─────────────────────────────────────────────────────────────────────
// Opened and closed per line: a file left open isn't listed by VitaShell or
// FTP until it is closed. Note: the UI and music threads may interleave
// lines; each write is one whole line. (Outside the namespace: updater.cpp
// writes to it too.)

static bool g_logOk;

void logLine(const char* fmt, ...) {
    if (!g_logOk) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = sceClibVsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 2) n = sizeof(buf) - 2;
    buf[n++] = '\n';
    const SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd < 0) return;
    sceIoWrite(fd, buf, n);
    sceIoClose(fd);
}

namespace {

// ── PAF, looked up at run time ─────────────────────────────────────────────
// NIDs: SHA-1 of the mangled name + "PAF" (ScePaf-RE's .emd files), checked
// against Vita Shell Companion's stubs where it uses the same ones.

struct PafFn {
    const char* name;
    uint32_t lib, nid;
    uintptr_t fn;
};
PafFn g_paf[] = {
    {"Plugin::Find", 0x4D9A9DD0, 0x004D98CC, 0},
    {"Plugin::TemplateOpen", 0x4D9A9DD0, 0xAB0DB9B0, 0},
    {"Widget::FindChild", 0x073F8C68, 0x9D304F4A, 0},
    {"Widget::SetPos", 0x073F8C68, 0xE29AB31F, 0},
    {"Widget::SetSize", 0x073F8C68, 0x046AA07D, 0},
    {"Widget::SetColor", 0x073F8C68, 0x627CDA4E, 0},
    {"Widget::SetScale", 0x073F8C68, 0xEE467CF8, 0},
    {"Handler::AddEventListener", 0x073F8C68, 0xFB7FE189, 0},
    {"ButtonBase::Enable", 0x073F8C68, 0x8F1A3A59, 0},
    {"SlideBar::UpdateSlider", 0x073F8C68, 0x705D1035, 0},
    {"MainThreadCallList::Register", 0x3C1965CD, 0x6735B503, 0},
    {"Text::StartAutoScroll", 0x073F8C68, 0xA304D5E7, 0},
};
enum {
    PAF_FIND, PAF_OPEN, PAF_FIND_CHILD, PAF_SET_POS, PAF_SET_SIZE, PAF_SET_COLOR, PAF_SET_SCALE,
    PAF_ADD_LISTENER, PAF_BTN_ENABLE, PAF_SLIDER_UPDATE, PAF_REGISTER, PAF_AUTO_SCROLL
};

// For the cover and Sony's strings: without them the panel still works,
// with the note and empty lines.
PafFn g_pafOpt[] = {
    {"Plugin::GetString", 0x4D9A9DD0, 0x19CEFDA7, 0},
    {"MemFile::Open", 0x3D643CE8, 0x19FE55A8, 0},  // ScePafMisc
    {"Surface::Load", 0xA070D6A7, 0xB228A257, 0},  // ScePafGraphics, from a File
    {"compress::Decompress", 0x3D643CE8, 0x5E39FCE1, 0},  // ScePafMisc, zlib
    {"gutil::GetDefaultSurfacePool", 0xA070D6A7, 0xCDE30B64, 0},
};
enum {
    OPT_GET_STRING, OPT_MEMFILE_OPEN, OPT_SURFACE_LOAD, OPT_DECOMPRESS, OPT_DEFAULT_POOL
};

bool findTable(PafFn* t, unsigned n) {
    bool all = true;
    for (unsigned i = 0; i < n; i++) {
        PafFn& f = t[i];
        if (f.fn) continue;
        const int rc = taiGetModuleExportFunc("ScePaf", f.lib, f.nid, &f.fn);
        if (rc < 0 || !f.fn) {
            f.fn = 0;
            all = false;
            logLine("paf: %s not exported (0x%08X)", f.name, (unsigned)rc);
        }
    }
    return all;
}
bool findPaf() { return findTable(g_paf, sizeof(g_paf) / sizeof(g_paf[0])); }
bool g_optTried;
void findOpt() {
    if (g_optTried) return;
    g_optTried = true;
    findTable(g_pafOpt, sizeof(g_pafOpt) / sizeof(g_pafOpt[0]));
}

typedef paf::Plugin* (*PluginFindFn)(const char*);
typedef int (*TemplateOpenFn)(paf::Plugin*, paf::ui::Widget*, const paf::IDParam&, const void*);
typedef paf::ui::Widget* (*FindChildFn)(paf::ui::Widget*, const paf::IDParam&, int);
// SetPos / SetSize / SetColor / SetScale(math::v4 const&, Timer*, int32_t id,
// int32_t opt, float delay, AnimCB, void*)
typedef int (*SetV4Fn)(paf::ui::Widget*, const float*, void*, int, int, float, void*, void*);
typedef int (*AddListenerFn)(paf::ui::Handler*, int, paf::ui::EventListener*, bool);
typedef void (*EnableFn)(paf::ui::ButtonBase*, bool);
typedef void (*UpdateSliderFn)(paf::ui::SlideBar*, bool);
typedef void (*RegisterFn)(void (*)(void*), void*);
typedef int (*AutoScrollFn)(paf::ui::Widget*, int);
typedef void (*HandlerCB)(int32_t, paf::ui::Handler*, paf::ui::Event*, void*);
typedef const wchar_t* (*GetStringFn)(const paf::Plugin*, const paf::IDParam&);
// Functions returning a SharedPtr or intrusive_ptr (classes with destructors)
// write it where their hidden first argument points; a SharedPtr argument is
// passed as a pointer to a copy the caller owns (the ARM C++ ABI).
typedef void (*MemFileOpenFn)(void* ret, const void* data, size_t size, int32_t* err);
typedef void (*SurfaceLoadFn)(void* ret, paf::graph::SurfacePool*, void* file, void* option);
typedef int32_t (*DecompressFn)(void* dst, size_t dstSize, const void* src, size_t srcSize, int type);
typedef paf::graph::SurfacePool* (*DefaultPoolFn)();

// An IDParam by hash, laid out like paf::IDParam (its string, empty, then
// the hash) without constructing one: its string's destructor would import
// sce_paf_free.
struct HashID {
    char str[sizeof(paf::IDParam) - sizeof(uint32_t)];
    uint32_t hash;
};

paf::ui::Widget* findChild(paf::ui::Widget* root, uint32_t hash) {
    HashID id;
    sceClibMemset(&id, 0, sizeof(id));
    id.hash = hash;
    return ((FindChildFn)g_paf[PAF_FIND_CHILD].fn)(root, *(const paf::IDParam*)&id, 0);
}

void setV4(int fn, int anim, paf::ui::Widget* w, float a, float b, float c, float d) {
    const float v[4] __attribute__((aligned(16))) = {a, b, c, d};
    ((SetV4Fn)g_paf[fn].fn)(w, v, NULL, anim, 0, 0.0f, NULL, NULL);
}
void setPos(paf::ui::Widget* w, float x, float y) { setV4(PAF_SET_POS, 0x10000 /* ANIM_MOVE */, w, x, y, 0, 0); }
void setSize(paf::ui::Widget* w, float x, float y) { setV4(PAF_SET_SIZE, 0x10004 /* ANIM_SIZE */, w, x, y, 0, 0); }
void setScale(paf::ui::Widget* w, float k) { setV4(PAF_SET_SCALE, 0x10005 /* ANIM_SCALE */, w, k, k, 1, 0); }
// The theme's clock colour (its date's, white by default): the panel's
// buttons, symbols, bar, lines and no-cover pictures take it, as the clock does.
float g_tint[3] = {1, 1, 1};
bool tinted() { return g_tint[0] != 1 || g_tint[1] != 1 || g_tint[2] != 1; }
void setTint(paf::ui::Widget* w) { setV4(PAF_SET_COLOR, 0x10001, w, g_tint[0], g_tint[1], g_tint[2], 1); }
// Shown or hidden by its scale (0 draws nothing and takes no touch), with
// calls proven at boot; a test build using PAF's Show/Hide stopped the Vita
// at boot.
float panelScale();
void show(paf::ui::Widget* w, bool on) { setScale(w, on ? panelScale() : 0.0f); }

// A text line's string, from UTF-8: a paf::wstring is {data, length,
// allocator}; this one points at a buffer of ours, which Text copies.
struct WStr {
    const wchar_t* ptr;
    size_t count;
    uint32_t alloc;
};
wchar_t g_lineBuf[2][256];

void showLine(paf::ui::Widget* w, const wchar_t* s, int n) {
    const WStr str = {s, (size_t)n, 0};
    w->SetString(*(const paf::wstring*)&str);
    // A line too long for its box scrolls, as the quick menu's do: PAF's own
    // ticker (Text::StartAutoScroll, horizontal).
    ((AutoScrollFn)g_paf[PAF_AUTO_SCROLL].fn)(w, 1 /* SCROLL_TYPE_HORIZONTAL */);
}

// One of Sony's own strings from impose_plugin, in the system language
// (all 20 of the Vita's languages are in its string table).
paf::Plugin* g_impose;
const wchar_t* sonyString(uint32_t id);

void setLineW(paf::ui::Widget* w, int slot, const wchar_t* s) {
    wchar_t* out = g_lineBuf[slot];
    int n = 0;
    while (s && s[n] && n < 255) {
        out[n] = s[n];
        n++;
    }
    out[n] = 0;
    showLine(w, out, n);
}

void setLine(paf::ui::Widget* w, int slot, const char* utf8) {
    wchar_t* out = g_lineBuf[slot];
    int n = 0;
    for (const uint8_t* s = (const uint8_t*)utf8; *s && n < 255;) {
        uint32_t c = *s++;
        if (c >= 0xF0) {  // outside the 16-bit range PAF text holds
            c = '?';
            for (int k = 0; k < 3 && (*s & 0xC0) == 0x80; k++) s++;
        } else if (c >= 0xE0) {
            c = (c & 0x0F) << 12;
            if ((*s & 0xC0) == 0x80) c |= (*s++ & 0x3F) << 6;
            if ((*s & 0xC0) == 0x80) c |= (*s++ & 0x3F);
        } else if (c >= 0xC0) {
            c = (c & 0x1F) << 6;
            if ((*s & 0xC0) == 0x80) c |= (*s++ & 0x3F);
        }
        out[n++] = (wchar_t)c;
    }
    out[n] = 0;
    showLine(w, out, n);
}

// ── Textures ────────────────────────────────────────────────────────────────
// As Vita Shell Companion handles them: an intrusive_ptr never destroyed by
// this plugin, so nothing here calls Surface::AddRef or Release.
struct TexPtr : paf::intrusive_ptr<paf::graph::Surface> {
    void set(paf::graph::Surface* s) { m_ptr = s; }
};
union TexRef {
    TexPtr ptr;
    explicit TexRef(paf::graph::Surface* s) : ptr() { ptr.set(s); }
    ~TexRef() {}
};
// A widget's texture, with the reference GetTexture adds kept for good.
paf::graph::Surface* holdTexture(paf::ui::Widget* w) {
    TexRef ref(NULL);
    w->GetTexture(ref.ptr);
    return ref.ptr.get();
}
// The widget adds a reference of its own.
void giveTexture(paf::ui::Widget* w, paf::graph::Surface* s) {
    TexRef ref(s);
    w->SetTexture(ref.ptr);
}

// ── Listeners ───────────────────────────────────────────────────────────────

// As paf::ui::EventCBListener, but never freed by PAF: static storage,
// attached again to each new start screen.
class Listener : public paf::ui::EventListener {
public:
    Listener(HandlerCB f, void* d) : m_func(f), m_data(d) { m_autodelete = false; }
    virtual int32_t Do(int32_t type, paf::ui::Handler* self, paf::ui::Event* e) {
        if (!m_killed && m_func) m_func(type, self, e, m_data);
        return 0;
    }
    virtual HandlerCB GetFunc() { return m_func; }
private:
    HandlerCB m_func;
    void* m_data;
};

// The start screen's lifetime: a marker on its page, auto-deleted by PAF
// with the page; its destructor says the start screen is gone.
volatile int g_screenGen;
volatile bool g_screenAlive;
void screenGone(int gen);

class Marker : public paf::ui::EventListener {
public:
    explicit Marker(int gen) : m_gen(gen) { m_autodelete = true; }
    virtual ~Marker() { screenGone(m_gen); }
    virtual int32_t Do(int32_t, paf::ui::Handler*, paf::ui::Event*) { return 0; }
private:
    int m_gen;
};
char g_markerMem[4][sizeof(Marker)] __attribute__((aligned(8)));

// ── The music, from SceShell's music service ───────────────────────────────
// Its client object comes from sceShellSvcGetSvcObj (library 0x773242C7,
// function 0xB31E7F1C, as Vita Shell Companion finds it); invokeSyncMethod
// is its vtable's sixth entry. Method numbers and layouts from libSceMusic.
// The caller is the music player's (type 1, priority 8): the one the Music
// app's status answered on.

struct InData {
    const void* data;
    SceSize size;
};
struct OutBuf {
    void* data;
    SceSize size;
    SceSize written;
};
struct Caller {
    uint32_t unk, type, prio;
};
const Caller kPlayer = {0, 1, 8};

struct Track {
    int32_t unk_00, unk_04;
    uint32_t durationMs;
    char title[0x400], unk_40C[0x400], album[0x400], unk_C0C[0x400], artist[0x400],
        unk_140C[0x400];
    int32_t unk_180C;
};

typedef void* (*GetSvcObjFn)();
typedef int (*InvokeFn)(void* self, uint32_t method, const InData* in, uint32_t inNum,
                        int32_t* res, OutBuf* out, uint32_t outNum);
GetSvcObjFn g_svc;

int svcCall(uint32_t method, const InData* in, uint32_t inNum, void* out, SceSize outSize) {
    void* obj = g_svc ? g_svc() : NULL;
    if (!obj) return -1;
    OutBuf ob = {out, outSize, 0};
    int32_t res = 0;
    const int ret = (*(InvokeFn* const*)obj)[5](obj, method, in, inNum, &res, out ? &ob : NULL, out ? 1 : 0);
    return ret ? ret : res;
}

enum { CMD_PLAY = 1, CMD_STOP = 2, CMD_NEXT = 3, CMD_PREVIOUS = 4 };  // SceMusicCommand

void musicCommand(uint32_t cmd) {
    const uint32_t msg[2] = {cmd, 0};  // command, fade-in
    const InData in[2] = {{msg, sizeof(msg)}, {&kPlayer, sizeof(kPlayer)}};
    const int rc = svcCall(0x30006, in, 2, NULL, 0);
    if (rc < 0) logLine("command %u: 0x%08X", (unsigned)cmd, (unsigned)rc);
}

// What is playing: written by the music thread into the buffer not in use,
// then published by flipping g_nowIdx. Note: a reader slower than two
// polls (1 s) could see a half-written copy; the UI reads in microseconds.
struct Now {
    int state;  // the service's play status: 1 playing, 2 paused (0 none, 10 nothing to play)
    bool loaded;  // a song, in a running Music app
    char title[256], artist[256];  // empty: Sony's "Unknown"
};
Now g_now[2];
volatile int g_nowIdx;
volatile int g_nowSeq;  // bumped on every change

Track g_track;  // the music thread's

void copyStr(char* d, const char* s, int n) {
    sceClibStrncpy(d, s, n - 1);
    d[n - 1] = 0;
}

// The Music app (NPXS10009) running: the service keeps the last song's
// title and a paused state after the app is closed (seen in testing).
// _sceAppMgrGetIdByName(pid out, title id, options), from src/appmgr_stub.S.
extern "C" int ssm_sceAppMgrGetIdByName(SceUID* pid, const char* name, const void* opt);

bool musicAppRunning() {
    SceUID pid = -1;
    const uint32_t opt[2] = {0, 0};  // SceAppMgrGetIdByNameOpt: ignored on 3.60+
    return ssm_sceAppMgrGetIdByName(&pid, "NPXS10009", opt) >= 0 && pid >= 0;
}

// The music and cover threads sleep until there is something to do: these
// wake them (a press or a new start screen; a new song or a cover taken).
SceUID g_wakeMusic = -1, g_wakeCover = -1;
volatile bool g_forgetCovers;  // the Music app closed: its remembered covers can go
volatile int g_fastPolls;      // after a press: look every 50 ms for a moment
void wake(SceUID sema) {
    if (sema >= 0) sceKernelSignalSema(sema, 1);  // already awake: refused, harmlessly
}

// The song whose cover is wanted, for the cover thread: a sequence lock (odd
// while being written).
struct Want {
    char title[256], album[256], artist[256];
};
Want g_req;
volatile int g_reqSeq;
Want g_reqLast;  // the music thread's: the song asked for last

void pollMusic() {
    uint32_t st[0x70 / 4];
    sceClibMemset(st, 0, sizeof(st));
    const InData in = {&kPlayer, sizeof(kPlayer)};
    const int rcS = svcCall(0x30007, &in, 1, st, sizeof(st));

    const Now& was = g_now[g_nowIdx];
    Now& n = g_now[g_nowIdx ^ 1];
    n.state = rcS >= 0 ? (int)st[0x10 / 4] : 0;
    // Whether the Music app runs: looked at once a second, or when the state
    // changes.
    static bool app;
    static SceInt64 appAt;
    const SceInt64 now = sceKernelGetProcessTimeWide();
    if (now >= appAt || n.state != was.state) {
        const bool had = app;
        app = musicAppRunning();
        appAt = now + 1000000;
        if (had && !app) {
            g_forgetCovers = true;
            wake(g_wakeCover);
        }
    }
    // The song (6 KB from the service) only when there is one to show.
    int rcT = -1;
    sceClibMemset(&g_track, 0, sizeof(g_track));
    if (app && (n.state == 1 || n.state == 2)) rcT = svcCall(0x3001B, &in, 1, &g_track, sizeof(g_track));
    n.loaded = app && rcT >= 0 && (n.state == 1 || n.state == 2);
    copyStr(n.title, n.loaded ? g_track.title : "", sizeof(n.title));
    copyStr(n.artist, n.loaded ? g_track.artist : "", sizeof(n.artist));

    // A new song: its cover is looked for.
    if (n.loaded && (sceClibStrcmp(g_track.title, g_reqLast.title) || sceClibStrcmp(g_track.album, g_reqLast.album) ||
                     sceClibStrcmp(g_track.artist, g_reqLast.artist))) {
        copyStr(g_reqLast.title, g_track.title, 256);
        copyStr(g_reqLast.album, g_track.album, 256);
        copyStr(g_reqLast.artist, g_track.artist, 256);
        g_reqSeq++;
        __sync_synchronize();
        sceClibMemcpy(&g_req, &g_reqLast, sizeof(g_req));
        __sync_synchronize();
        g_reqSeq++;
        wake(g_wakeCover);
    }

    if (n.state == was.state && n.loaded == was.loaded && !sceClibStrcmp(n.title, was.title) &&
        !sceClibStrcmp(n.artist, was.artist))
        return;
    g_nowIdx ^= 1;
    g_nowSeq++;
}

// ── The cover ───────────────────────────────────────────────────────────────
// The service gives no file or picture, so the cover thread finds the song in
// ux0:music by its tags (title, then album and artist; ID3 in an MP3, iTunes'
// in an MP4 / M4A) and reads the picture in it (the front cover first), decodes it and makes it a
// small PNG (cover_image.cpp). The UI thread turns that into a texture with
// PAF's own loader (Surface::Load, as Vita Shell Companion loads its covers)
// only while the start screen is up; the texture goes with the start screen. The next song of the same album keeps
// the picture without a search.

// SceSysmem, from src/sysmem_stub.S (vitasdk's stub library names vary).
extern "C" SceUID ssm_sceKernelAllocMemBlock(const char* name, int type, SceSize size, const void* opt);
extern "C" int ssm_sceKernelFreeMemBlock(SceUID uid);
extern "C" int ssm_sceKernelGetMemBlockBase(SceUID uid, void** base);
const int kMemUserRw = 0x0C20D060;  // SCE_KERNEL_MEMBLOCK_TYPE_USER_RW

struct SceKernelFreeMemorySizeInfoSsm {
    int size, sizeUser, sizeCdram, sizePhycont;
};
extern "C" int ssm_sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfoSsm* info);

// A cover is shown 110 pixels wide: it is made at most a 120 square PNG
// (about 56 KB of texture; SceShell's texture pool takes 64 KB, not 256 KB).
const int kCoverSide = 120;
const uint32_t kMaxCoverBytes = 8 * 1024 * 1024;

// The cover thread's answer, taken by the UI thread.
enum { COVER_NONE, COVER_IMAGE, COVER_SAME };
struct CoverMail {
    int kind;
    SceUID uid;
    const uint8_t* data;
    uint32_t size;
};
CoverMail g_mail;
volatile int g_mailSeq, g_mailTaken;

struct Tag {
    char title[256], album[256], artist[256];
    uint32_t picOff, picSize;
    int picType;
};
Tag g_tag;
char g_path[256];
uint8_t g_io[512];
int g_scanned;

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint32_t le32(const uint8_t* p) { return (uint32_t)p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0]; }
uint32_t syncsafe(const uint8_t* p) { return (uint32_t)(p[0] & 0x7F) << 21 | (p[1] & 0x7F) << 14 | (p[2] & 0x7F) << 7 | (p[3] & 0x7F); }

void putUtf8(char* out, int& o, int cap, uint32_t c) {
    if (c < 0x80) {
        if (o + 1 < cap) out[o++] = (char)c;
    } else if (c < 0x800) {
        if (o + 2 < cap) {
            out[o++] = (char)(0xC0 | c >> 6);
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    } else if (o + 3 < cap) {
        out[o++] = (char)(0xE0 | c >> 12);
        out[o++] = (char)(0x80 | (c >> 6 & 0x3F));
        out[o++] = (char)(0x80 | (c & 0x3F));
    }
}

// An ID3 text frame's text, as UTF-8 (the service's lines are UTF-8).
void id3Text(const uint8_t* d, uint32_t n, char* out, int cap) {
    int o = 0;
    const int enc = n ? d[0] : 0;
    d++;
    n = n ? n - 1 : 0;
    if (enc == 0) {  // ISO-8859-1
        for (uint32_t i = 0; i < n && d[i]; i++) putUtf8(out, o, cap, d[i]);
    } else if (enc == 3) {  // UTF-8
        for (uint32_t i = 0; i < n && d[i] && o + 1 < cap; i++) out[o++] = (char)d[i];
    } else {  // UTF-16, with a byte order mark (1) or big-endian (2)
        bool be = enc == 2;
        uint32_t i = 0;
        if (enc == 1 && n >= 2 && ((d[0] == 0xFF && d[1] == 0xFE) || (d[0] == 0xFE && d[1] == 0xFF))) {
            be = d[0] == 0xFE;
            i = 2;
        }
        for (; i + 1 < n; i += 2) {
            uint32_t c = be ? (uint32_t)d[i] << 8 | d[i + 1] : (uint32_t)d[i + 1] << 8 | d[i];
            if (!c) break;
            if (c >= 0xD800 && c < 0xE000) c = '?';
            putUtf8(out, o, cap, c);
        }
    }
    out[o] = 0;
}

// t from an MP3's ID3v2.3 or 2.4 tag; false when it has none.
bool sameText(const char* a, const char* b);

// (wanted: the title looked for; reading stops at a different one, as the
// title comes first in nearly every tag.)
bool readTag(const char* path, Tag& t, const char* wanted) {
    sceClibMemset(&t, 0, sizeof(t));
    const SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return false;
    uint8_t h[10];
    if (sceIoRead(fd, h, 10) != 10 || h[0] != 'I' || h[1] != 'D' || h[2] != '3' || h[3] < 3 || h[3] > 4) {
        sceIoClose(fd);
        return false;
    }
    const int ver = h[3];
    const bool unsync = h[5] & 0x80;
    const uint32_t end = 10 + syncsafe(h + 6);
    uint32_t pos = 10;
    if (h[5] & 0x40) {  // an extended header
        uint8_t e[4];
        if (sceIoRead(fd, e, 4) != 4) {
            sceIoClose(fd);
            return false;
        }
        pos += ver == 4 ? syncsafe(e) : be32(e) + 4;
    }
    while (pos + 10 <= end) {
        uint8_t f[10];
        if (sceIoLseek(fd, pos, SCE_SEEK_SET) != (SceOff)pos || sceIoRead(fd, f, 10) != 10 || !f[0]) break;
        const uint32_t size = ver == 4 ? syncsafe(f + 4) : be32(f + 4);
        if (!size || pos + 10 + size > end) break;
        // Compressed or encrypted frames are left; a group byte or a data
        // length comes before the data.
        const uint8_t fl = f[9];
        const bool skip = ver == 4 ? (fl & 0x0C) : (fl & 0xC0);
        const uint32_t extra = ver == 4 ? ((fl & 0x40) ? 1 : 0) + ((fl & 0x01) ? 4 : 0) : ((fl & 0x20) ? 1 : 0);
        const bool frameUnsync = unsync || (ver == 4 && (fl & 0x02));
        const uint32_t dataOff = pos + 10 + extra, dataSize = size > extra ? size - extra : 0;
        char* text = !sceClibMemcmp(f, "TIT2", 4) ? t.title : !sceClibMemcmp(f, "TALB", 4) ? t.album
                   : !sceClibMemcmp(f, "TPE1", 4) ? t.artist : NULL;
        if (!skip && dataSize && (text || !sceClibMemcmp(f, "APIC", 4))) {
            const uint32_t want = dataSize < sizeof(g_io) ? dataSize : sizeof(g_io);
            if (sceIoLseek(fd, dataOff, SCE_SEEK_SET) == (SceOff)dataOff && sceIoRead(fd, g_io, want) == (int)want) {
                if (text) {
                    id3Text(g_io, want, text, 256);
                    if (text == t.title && !sameText(t.title, wanted)) break;  // another song
                } else if (!frameUnsync) {
                    // APIC: encoding, MIME type, picture type, description, picture.
                    const int enc = g_io[0];
                    uint32_t i = 1;
                    while (i < want && g_io[i]) i++;
                    i++;
                    const int type = i < want ? g_io[i] : -1;
                    i++;
                    if (enc == 1 || enc == 2) {
                        while (i + 1 < want && (g_io[i] || g_io[i + 1])) i += 2;
                        i += 2;
                    } else {
                        while (i < want && g_io[i]) i++;
                        i++;
                    }
                    if (i < want && i < dataSize && (!t.picSize || (type == 3 && t.picType != 3))) {
                        t.picOff = dataOff + i;
                        t.picSize = dataSize - i;
                        t.picType = type;
                    }
                }
            }
        }
        pos += 10 + size;
    }
    sceIoClose(fd);
    return true;
}

// Equal but for case (ASCII) and spaces at either end.
bool sameText(const char* a, const char* b) {
    while (*a == ' ') a++;
    while (*b == ' ') b++;
    int na = 0, nb = 0;
    while (a[na]) na++;
    while (b[nb]) nb++;
    while (na && a[na - 1] == ' ') na--;
    while (nb && b[nb - 1] == ' ') nb--;
    if (na != nb) return false;
    for (int i = 0; i < na; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return false;
    }
    return true;
}

bool tagMatches(const Tag& t, const Want& w) {
    if (!t.title[0] || !sameText(t.title, w.title)) return false;
    if (w.album[0] && t.album[0] && !sameText(t.album, w.album)) return false;
    if (w.artist[0] && t.artist[0] && !sameText(t.artist, w.artist)) return false;
    return true;
}

// The song files the Music app plays that carry a cover: MP3 (ID3) and
// MP4 / M4A (iTunes-style tags). 0 for any other file.
enum { FILE_OTHER, FILE_MP3, FILE_MP4 };
int songFile(const char* name, int n) {
    int dot = n - 1;
    while (dot > 0 && name[dot] != '.') dot--;
    if (dot <= 0 || n - dot > 5) return FILE_OTHER;
    char e[5] = {0, 0, 0, 0, 0};
    for (int i = dot + 1, k = 0; i < n; i++, k++) e[k] = (char)(name[i] | 32);
    if (!sceClibStrcmp(e, "mp3")) return FILE_MP3;
    if (!sceClibStrcmp(e, "m4a") || !sceClibStrcmp(e, "mp4") || !sceClibStrcmp(e, "m4b")) return FILE_MP4;
    return FILE_OTHER;
}

// ── MP4 / M4A tags ──────────────────────────────────────────────────────────
// Boxes: a 32-bit size (1: a 64-bit one follows; 0: to the end), a type,
// then the body. Tags are in moov > udta > meta > ilst (meta has 4 bytes of
// version and flags first in iTunes' files): ©nam, ©alb, ©ART and covr,
// each holding a data box (4 bytes of type, 4 of locale, then the value).

// The box of type inside [start, end) of fd: its body and end.
bool findBox(SceUID fd, uint32_t start, uint32_t end, uint32_t type, uint32_t& body, uint32_t& boxEnd) {
    for (uint32_t pos = start; pos + 8 <= end;) {
        uint8_t h[16];
        if (sceIoLseek(fd, pos, SCE_SEEK_SET) != (SceOff)pos || sceIoRead(fd, h, 16) < 8) return false;
        uint32_t size = be32(h), head = 8;
        if (size == 1) {  // 64-bit size; past 4 GB means past this file's end
            if (be32(h + 8)) return false;
            size = be32(h + 12);
            head = 16;
        } else if (size == 0) {
            size = end - pos;
        }
        if (size < head || size > end - pos) return false;
        if (be32(h + 4) == type) {
            body = pos + head;
            boxEnd = pos + size;
            return true;
        }
        pos += size;
    }
    return false;
}

const uint32_t kMoov = 0x6D6F6F76, kUdta = 0x75647461, kMeta = 0x6D657461, kIlst = 0x696C7374,
               kData = 0x64617461, kCovr = 0x636F7672, kNam = 0xA96E616D, kAlb = 0xA9616C62, kArt = 0xA9415254;

bool readMp4Tag(const char* path, Tag& t) {
    sceClibMemset(&t, 0, sizeof(t));
    const SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return false;
    const SceOff fileSize = sceIoLseek(fd, 0, SCE_SEEK_END);
    const uint32_t size = fileSize > 0 && fileSize < 0xFFFFFFFFll ? (uint32_t)fileSize : 0;
    uint32_t moov = 0, moovEnd = 0, udta = 0, udtaEnd = 0, meta = 0, metaEnd = 0, ilst = 0, ilstEnd = 0;
    bool ok = size && findBox(fd, 0, size, kMoov, moov, moovEnd);
    // meta is in udta (iTunes) or straight in moov.
    ok = ok && ((findBox(fd, moov, moovEnd, kUdta, udta, udtaEnd) && findBox(fd, udta, udtaEnd, kMeta, meta, metaEnd)) ||
                findBox(fd, moov, moovEnd, kMeta, meta, metaEnd));
    if (ok) {
        uint8_t h[8];  // a full box (version and flags) unless its first child (hdlr) starts at once
        const bool full = sceIoLseek(fd, meta, SCE_SEEK_SET) == (SceOff)meta && sceIoRead(fd, h, 8) == 8 &&
                          sceClibMemcmp(h + 4, "hdlr", 4);
        ok = findBox(fd, meta + (full ? 4 : 0), metaEnd, kIlst, ilst, ilstEnd);
    }
    for (uint32_t pos = ilst; ok && pos + 8 <= ilstEnd;) {
        uint8_t h[8];
        if (sceIoLseek(fd, pos, SCE_SEEK_SET) != (SceOff)pos || sceIoRead(fd, h, 8) != 8) break;
        const uint32_t itemSize = be32(h), type = be32(h + 4);
        if (itemSize < 8 || itemSize > ilstEnd - pos) break;
        uint32_t data, dataEnd;
        char* text = type == kNam ? t.title : type == kAlb ? t.album : type == kArt ? t.artist : NULL;
        if ((text || type == kCovr) && findBox(fd, pos + 8, pos + itemSize, kData, data, dataEnd) && dataEnd > data + 8) {
            const uint32_t at = data + 8, len = dataEnd - at;
            if (text) {
                const uint32_t n = len < 255 ? len : 255;
                if (sceIoLseek(fd, at, SCE_SEEK_SET) == (SceOff)at && sceIoRead(fd, text, n) == (int)n) text[n] = 0;
                else text[0] = 0;
            } else if (!t.picSize) {
                t.picOff = at;
                t.picSize = len;
                t.picType = 3;
            }
        }
        pos += itemSize;
    }
    sceIoClose(fd);
    return ok;
}

bool readSongTag(int kind, const Want& w) {
    return kind == FILE_MP3 ? readTag(g_path, g_tag, w.title) : readMp4Tag(g_path, g_tag);
}

// The songs found lately, by their title, album and artist: their files are
// looked at first, so a song or album played again needs no search.
struct Seen {
    uint32_t key;
    char path[256];
};
Seen g_seen[16];
int g_seenNext;

uint32_t wantKey(const Want& w) {
    uint32_t h = 2166136261u;  // FNV-1a over the three
    const char* const parts[3] = {w.title, w.album, w.artist};
    for (int i = 0; i < 3; i++) {
        for (const char* c = parts[i]; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
        h = (h ^ 0xFF) * 16777619u;
    }
    return h ? h : 1;
}

// g_path (n long) and the folders in it, three deep: true when the song's
// file is found (its tag in g_tag, its path in g_path).
bool scanDir(const Want& w, int n, int depth) {
    const SceUID d = sceIoDopen(g_path);
    if (d < 0) return false;
    SceIoDirent e;
    bool hit = false;
    while (!hit && sceIoDread(d, &e) > 0) {
        int len = 0;
        while (e.d_name[len]) len++;
        if (!len || e.d_name[0] == '.' || n + 1 + len >= (int)sizeof(g_path)) continue;
        g_path[n] = '/';
        sceClibMemcpy(g_path + n + 1, e.d_name, len + 1);
        if (SCE_S_ISDIR(e.d_stat.st_mode)) {
            if (depth < 3) hit = scanDir(w, n + 1 + len, depth + 1);
        } else if (const int kind = songFile(e.d_name, len)) {
            g_scanned++;
            hit = readSongTag(kind, w) && tagMatches(g_tag, w);
        }
        if (!hit) g_path[n] = 0;
    }
    sceIoDclose(d);
    return hit;
}

// A memory block of at least n bytes: its uid (-1 for none) and base.
SceUID allocBlock(const char* name, uint32_t n, uint8_t** base) {
    *base = NULL;
    const SceUID uid = ssm_sceKernelAllocMemBlock(name, kMemUserRw, (n + 0xFFF) & ~0xFFFu, NULL);
    if (uid < 0) return -1;
    if (ssm_sceKernelGetMemBlockBase(uid, (void**)base) < 0 || !*base) {
        ssm_sceKernelFreeMemBlock(uid);
        return -1;
    }
    return uid;
}

uint32_t freeUserMemory() {
    SceKernelFreeMemorySizeInfoSsm info;
    sceClibMemset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    return ssm_sceKernelGetFreeMemorySize(&info) >= 0 ? info.sizeUser : 0;
}

// The cover thread's own priority: the lowest (191), or SceShell's default
// when SceShell won't take that.
int g_coverPriority = 191;

// Finding a song's file and reading its picture take almost no processor
// time, only the memory card's: they run at SceShell's default priority, so
// a busy moment (just after starting the Vita) can't hold them up. Making the
// picture small takes the processor: that runs at the thread's own lowest
// priority, so nothing on screen ever waits for it.
void searchPriority(bool search) {
    sceKernelChangeThreadPriority(sceKernelGetThreadId(), search ? 0x10000100 : g_coverPriority);
}
struct BackToLowest {
    ~BackToLowest() { searchPriority(false); }
};

// The last few songs' covers, kept small, so going back to one (previous,
// or a song played again) shows its cover at once. A song without a cover is
// remembered too (no memory). Let go when the Music app closes.
struct Remembered {
    uint32_t key;  // wantKey; 0 for an empty slot
    SceUID uid;    // -1 for a song without a cover
    uint8_t* data;
    uint32_t size;
};
const int kRemembered = 3;  // previous, this and next: at most about 44 KB each
Remembered g_remembered[kRemembered];
int g_rememberNext;

void forget(Remembered& r) {
    if (r.data) ssm_sceKernelFreeMemBlock(r.uid);
    r.key = 0;
    r.uid = -1;
    r.data = NULL;
    r.size = 0;
}

void forgetCovers() {
    for (int i = 0; i < kRemembered; i++) forget(g_remembered[i]);
}

void remember(uint32_t key, const uint8_t* png, uint32_t size) {
    Remembered& r = g_remembered[g_rememberNext];
    g_rememberNext = (g_rememberNext + 1) % kRemembered;
    forget(r);
    if (size) {
        uint8_t* copy = NULL;
        const SceUID uid = allocBlock("StartScreenMusicRemembered", size, &copy);
        if (uid < 0) return;  // short of memory: simply not remembered
        sceClibMemcpy(copy, png, size);
        r.uid = uid;
        r.data = copy;
        r.size = size;
    }
    r.key = key;
}

// The processor time this thread has had, in microseconds.
SceInt64 threadCpuTime() {
    SceKernelThreadInfo info;
    sceClibMemset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0) return 0;
    return (SceInt64)info.runClocks;
}

// ── Covers saved on the memory card ─────────────────────────────────────────
// Each small cover is also kept in COVER_DIR, as the Vita's own media apps
// keep ready-made thumbnails, so a song played before shows its cover at
// once, even after a restart. A file holds the song's path, size and time
// (a song changed or gone has its cover made again) and the small PNG. The
// oldest go once there are more than kSavedMax.
#define COVER_DIR "ux0:data/startscreenmusic/covers"
const int kSavedMax = 300;  // about 13 MB
const char kSavedMagic[4] = {'S', 'S', 'M', 'C'};

struct SavedHead {  // as written: seven 32-bit words
    char magic[4];
    uint32_t pngSize, songSizeLo, songSizeHi, songTimeLo, songTimeHi, pathLen;
};
static_assert(sizeof(SavedHead) == 28, "SavedHead is written as it is");

uint64_t when(const SceDateTime& t) {
    return (((((((uint64_t)t.year * 12 + t.month) * 31 + t.day) * 24 + t.hour) * 60 + t.minute) * 60 + t.second) *
            1000000) +
           t.microsecond;
}

// The file for w's cover: two 32-bit hashes of its title, album and artist.
void savedName(const Want& w, char* out, int cap) {
    uint32_t a = 2166136261u, b = 0x9747B28Cu;
    const char* const parts[3] = {w.title, w.album, w.artist};
    for (int i = 0; i < 3; i++) {
        for (const char* c = parts[i]; *c; c++) {
            a = (a ^ (uint8_t)*c) * 16777619u;
            b = (b ^ (uint8_t)*c) * 0x5BD1E995u;
        }
        a = (a ^ 0xFF) * 16777619u;
        b = (b ^ 0xFF) * 0x5BD1E995u;
    }
    sceClibSnprintf(out, cap, COVER_DIR "/%08x%08x.ssc", (unsigned)a, (unsigned)b);
}

bool songStat(const char* path, uint64_t& size, uint64_t& time) {
    SceIoStat st;
    sceClibMemset(&st, 0, sizeof(st));
    if (sceIoGetstat(path, &st) < 0) return false;
    size = (uint64_t)st.st_size;
    time = when(st.st_mtime);
    return true;
}

// w's saved cover into m: true when there is one and its song is unchanged.
bool loadSaved(const Want& w, CoverMail& m, uint32_t cap) {
    char name[96], song[256];
    savedName(w, name, sizeof(name));
    const SceUID fd = sceIoOpen(name, SCE_O_RDONLY, 0);
    if (fd < 0) return false;
    SavedHead h;
    bool stale = !(sceIoRead(fd, &h, sizeof(h)) == (int)sizeof(h) && !sceClibMemcmp(h.magic, kSavedMagic, 4) &&
                   h.pngSize >= 8 && h.pngSize <= cap && h.pathLen > 0 && h.pathLen < sizeof(song) &&
                   sceIoRead(fd, song, h.pathLen) == (int)h.pathLen);
    if (!stale) {
        song[h.pathLen] = 0;
        uint64_t size = 0, time = 0;
        stale = !songStat(song, size, time) || size != ((uint64_t)h.songSizeHi << 32 | h.songSizeLo) ||
                time != ((uint64_t)h.songTimeHi << 32 | h.songTimeLo);
    }
    uint8_t* png = NULL;
    SceUID uid = -1;
    if (!stale) {
        uid = allocBlock("StartScreenMusicCover", h.pngSize, &png);
        if (uid >= 0)
            stale = !(sceIoRead(fd, png, h.pngSize) == (int)h.pngSize && png[1] == 'P' && png[2] == 'N' &&
                      png[3] == 'G');
    }
    sceIoClose(fd);
    if (stale) sceIoRemove(name);  // made again below
    if (stale || uid < 0) {
        if (uid >= 0) ssm_sceKernelFreeMemBlock(uid);
        return false;
    }
    m.kind = COVER_IMAGE;
    m.uid = uid;
    m.data = png;
    m.size = h.pngSize;
    return true;
}

// Deletes the oldest saved covers once there are more than kSavedMax (at
// most 32 at a time; this runs every 25 covers saved).
void pruneSaved() {
    struct Old {
        uint64_t time;
        char name[24];
    } old[32];
    int olds = 0, count = 0;
    const SceUID d = sceIoDopen(COVER_DIR);
    if (d < 0) return;
    SceIoDirent e;
    while (sceIoDread(d, &e) > 0) {
        int len = 0;
        while (e.d_name[len]) len++;
        if (len < 5 || len >= (int)sizeof(old[0].name) || sceClibStrcmp(e.d_name + len - 4, ".ssc")) continue;
        count++;
        const uint64_t t = when(e.d_stat.st_mtime);
        int slot = olds < 32 ? olds++ : -1;
        if (slot < 0) {  // full: replace the newest kept, if this is older
            int newest = 0;
            for (int i = 1; i < 32; i++)
                if (old[i].time > old[newest].time) newest = i;
            if (t >= old[newest].time) continue;
            slot = newest;
        }
        old[slot].time = t;
        sceClibStrncpy(old[slot].name, e.d_name, sizeof(old[slot].name));
    }
    sceIoDclose(d);
    int excess = count - kSavedMax;
    while (excess-- > 0 && olds > 0) {
        int oldest = 0;
        for (int i = 1; i < olds; i++)
            if (old[i].time < old[oldest].time) oldest = i;
        char path[96];
        sceClibSnprintf(path, sizeof(path), COVER_DIR "/%s", old[oldest].name);
        sceIoRemove(path);
        old[oldest] = old[--olds];
    }
}

// Keeps w's small cover (its song at songPath) on the memory card. Written
// beside it, then renamed: a half-written file never takes its name.
void saveCover(const Want& w, const char* songPath, const uint8_t* png, uint32_t size) {
    static bool folders;
    static int saved;
    if (!folders) {
        sceIoMkdir("ux0:data/startscreenmusic", 0777);
        sceIoMkdir(COVER_DIR, 0777);
        folders = true;
    }
    uint64_t songSize = 0, songTime = 0;
    if (!songStat(songPath, songSize, songTime)) return;
    SavedHead h;
    sceClibMemcpy(h.magic, kSavedMagic, 4);
    h.pngSize = size;
    h.songSizeLo = (uint32_t)songSize;
    h.songSizeHi = (uint32_t)(songSize >> 32);
    h.songTimeLo = (uint32_t)songTime;
    h.songTimeHi = (uint32_t)(songTime >> 32);
    h.pathLen = 0;
    while (songPath[h.pathLen]) h.pathLen++;
    char name[96], temp[100];
    savedName(w, name, sizeof(name));
    sceClibSnprintf(temp, sizeof(temp), "%s.new", name);
    const SceUID fd = sceIoOpen(temp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return;
    const bool ok = sceIoWrite(fd, &h, sizeof(h)) == (int)sizeof(h) &&
                    sceIoWrite(fd, songPath, h.pathLen) == (int)h.pathLen && sceIoWrite(fd, png, size) == (int)size;
    sceIoClose(fd);
    if (ok) {
        sceIoRemove(name);
        if (sceIoRename(temp, name) < 0) sceIoRemove(temp);
    } else {
        sceIoRemove(temp);
    }
    if (saved++ % 25 == 0) pruneSaved();
}

// The cover of w as a small PNG, or none (the jacket then).
CoverMail findCover(const Want& w) {
    CoverMail m = {COVER_NONE, -1, NULL, 0};
    const uint32_t wanted = wantKey(w);
    for (int i = 0; i < kRemembered; i++) {
        const Remembered& r = g_remembered[i];
        if (!r.key || r.key != wanted) continue;
        logLine("song \"%s\": cover remembered", w.title);
        if (!r.data) return m;  // no cover, as before
        uint8_t* copy = NULL;
        const SceUID uid = allocBlock("StartScreenMusicCover", r.size, &copy);
        if (uid < 0) break;  // short of memory: made again below
        sceClibMemcpy(copy, r.data, r.size);
        m.kind = COVER_IMAGE;
        m.uid = uid;
        m.data = copy;
        m.size = r.size;
        return m;
    }
    searchPriority(true);
    BackToLowest lowest;
    {
        const SceInt64 t = sceKernelGetProcessTimeWide();
        if (loadSaved(w, m, pngSize(kCoverSide, kCoverSide, 3))) {
            logLine("song \"%s\": cover saved earlier (%d ms)", w.title,
                    (int)(sceKernelGetProcessTimeWide() - t) / 1000);
            remember(wanted, m.data, m.size);
            return m;
        }
    }
    const SceInt64 t0 = sceKernelGetProcessTimeWide();
    g_scanned = 0;
    const uint32_t key = wantKey(w);
    bool found = false;
    for (int i = 0; i < 16 && !found; i++) {
        if (g_seen[i].key != key) continue;
        sceClibStrncpy(g_path, g_seen[i].path, sizeof(g_path));
        int len = 0;
        while (g_path[len]) len++;
        int slash = len;
        while (slash > 0 && g_path[slash - 1] != '/') slash--;
        const int kind = songFile(g_path + slash, len - slash);
        found = kind && readSongTag(kind, w) && tagMatches(g_tag, w);
    }
    if (!found) {
        sceClibStrncpy(g_path, "ux0:music", sizeof(g_path));
        found = scanDir(w, 9, 0);
        if (found) {
            g_seen[g_seenNext].key = key;
            sceClibStrncpy(g_seen[g_seenNext].path, g_path, sizeof(g_seen[0].path));
            g_seenNext = (g_seenNext + 1) % 16;
        }
    }
    const int ms = (int)(sceKernelGetProcessTimeWide() - t0) / 1000;
    // One line a song in the log: what its cover came to.
    if (!found) {
        logLine("song \"%s\": not found in ux0:music (%d files, %d ms)", w.title, g_scanned, ms);
        return m;
    }
    if (!g_tag.picSize) {
        logLine("song \"%s\": no cover", w.title);
        remember(wanted, NULL, 0);
        return m;
    }
    if (g_tag.picSize > kMaxCoverBytes) {
        logLine("song \"%s\": cover too big (%u bytes)", w.title, (unsigned)g_tag.picSize);
        return m;
    }
    // The picture as it is in the file.
    uint8_t *pic = NULL, *work = NULL, *png = NULL;
    const SceUID picUid = allocBlock("StartScreenMusicPicture", g_tag.picSize, &pic);
    if (picUid < 0) {
        logLine("song \"%s\": no memory for its cover (%u KB free)", w.title, (unsigned)(freeUserMemory() >> 10));
        return m;
    }
    const SceUID fd = sceIoOpen(g_path, SCE_O_RDONLY, 0);
    bool ok = fd >= 0 && sceIoLseek(fd, g_tag.picOff, SCE_SEEK_SET) == (SceOff)g_tag.picOff;
    for (uint32_t got = 0; ok && got < g_tag.picSize;) {
        const int r = sceIoRead(fd, pic + got, g_tag.picSize - got);
        ok = r > 0;
        got += ok ? r : 0;
    }
    if (fd >= 0) sceIoClose(fd);
    searchPriority(false);
    // Decoded a block or a row at a time into the small square
    // (cover_image.cpp): about 115 KB for the square (a PNG's rows and
    // window more), and its small PNG, kept.
    const uint32_t pngCap = pngSize(kCoverSide, kCoverSide, 3);
    const uint32_t workSize = coverWorkSize(kCoverSide, pic, g_tag.picSize);
    const SceUID workUid = ok ? allocBlock("StartScreenMusicDecode", workSize, &work) : -1;
    const SceUID pngUid = workUid >= 0 ? allocBlock("StartScreenMusicCover", pngCap, &png) : -1;
    uint32_t pngLen = 0;
    CoverInfo ci;
    sceClibMemset(&ci, 0, sizeof(ci));
    const SceInt64 t1 = sceKernelGetProcessTimeWide(), cpu1 = threadCpuTime();
    if (pngUid >= 0)
        pngLen = coverToPng(pic, g_tag.picSize, work, workSize, kCoverSide, png, pngCap, &ci);
    const int decodeMs = (int)(sceKernelGetProcessTimeWide() - t1) / 1000;
    const int decodeCpuMs = (int)(threadCpuTime() - cpu1) / 1000;
    if (workUid >= 0) ssm_sceKernelFreeMemBlock(workUid);
    ssm_sceKernelFreeMemBlock(picUid);
    if (!pngLen) {
        logLine("song \"%s\": cover not read (%s, error %d%s, %u KB free)", w.title,
                !ok ? "file" : pngUid < 0 ? "memory" : "picture", ci.error,
                ci.error == 37 ? ": progressive JPEG" : ci.error == 100 ? ": interlaced PNG" : "",
                (unsigned)(freeUserMemory() >> 10));
        if (pngUid >= 0) ssm_sceKernelFreeMemBlock(pngUid);
        return m;
    }
    logLine("song \"%s\": cover %d x %d (found in %d ms, made small in %d ms, %d ms of it working)", w.title,
            ci.width, ci.height, ms, decodeMs, decodeCpuMs);
    remember(wanted, png, pngLen);
    saveCover(w, g_path, png, pngLen);
    m.kind = COVER_IMAGE;
    m.uid = pngUid;
    m.data = png;
    m.size = pngLen;
    return m;
}

// ── Sony's no-cover jacket ──────────────────────────────────────────────────
// What the Music app shows for a song without a cover: in its lists, a
// see-through white square with its note (texture 0xB881B061, 90 x 90).
// Read from the Vita's own copy of the app's resources (an RCO: a header of
// table offsets, then a tree of elements with typed attributes; the texture
// is a zlib-packed GIM), so nothing of Sony's ships with the plugin.
const char kMusicRco[] = "vs0:app/NPXS10009/musicbrowser_plugin.rco";
const uint32_t kJacketId = 0xB881B061;
const uint8_t* g_jacketData;  // the GIM, kept for good (about 10 KB)
uint32_t g_jacketSize;
volatile bool g_jacketReady;

bool readAt(SceUID fd, uint32_t off, void* buf, uint32_t n) {
    return sceIoLseek(fd, off, SCE_SEEK_SET) == (SceOff)off && sceIoRead(fd, buf, n) == (int)n;
}
bool cstrIs(const char* table, uint32_t size, uint32_t off, const char* s) {
    for (uint32_t i = 0;; i++) {
        if (off + i >= size) return false;
        if (table[off + i] != s[i]) return false;
        if (!s[i]) return true;
    }
}

void loadJacket() {
    const SceUID fd = sceIoOpen(kMusicRco, SCE_O_RDONLY, 0);
    if (fd < 0) {
        logLine("jacket: %s 0x%08X", kMusicRco, (unsigned)fd);
        return;
    }
    uint32_t h[2 + 18];
    SceUID mem = -1, out = -1;
    uint8_t* base = NULL;
    const char* why = NULL;
    do {
        if (!readAt(fd, 0, h, sizeof(h)) || sceClibMemcmp(h, "RCOF", 4)) { why = "not an RCO"; break; }
        const uint32_t treeOff = h[2], treeSize = h[3], intOff = h[6], intSize = h[7], strOff = h[8],
                       strSize = h[9], fileOff = h[18];
        if (treeSize > 0x80000 || intSize > 0x10000 || strSize > 0x10000) { why = "tables too big"; break; }
        const uint32_t need = (treeSize + intSize + strSize + 0xFFF) & ~0xFFFu;
        mem = ssm_sceKernelAllocMemBlock("StartScreenMusicRco", kMemUserRw, need, NULL);
        if (mem < 0 || ssm_sceKernelGetMemBlockBase(mem, (void**)&base) < 0) { why = "no memory"; break; }
        uint8_t *tree = base, *ints = base + treeSize;
        const char* strs = (const char*)(ints + intSize);
        if (!readAt(fd, treeOff, tree, treeSize) || !readAt(fd, intOff, ints, intSize) ||
            !readAt(fd, strOff, (void*)strs, strSize)) { why = "not read"; break; }
        // The <texture> whose id is the jacket's: its data's place and size,
        // its size unpacked, and whether it is packed.
        uint32_t srcOff = 0, srcSize = 0, origSize = 0;
        bool packed = false, found = false;
        for (uint32_t o = 0; !found && o + 28 <= treeSize;) {
            const uint32_t* e = (const uint32_t*)(tree + o);
            const uint32_t nattr = e[1];
            if (nattr > (treeSize - o - 28) / 16) break;  // (written so it can't wrap)
            if (cstrIs(strs, strSize, e[0], "texture")) {
                uint32_t so = 0, ss = 0, os = 0;
                bool pk = false, mine = false;
                for (uint32_t i = 0; i < nattr; i++) {
                    const uint32_t* a = (const uint32_t*)(tree + o + 28 + i * 16);
                    if ((a[1] == 11 || a[1] == 12) && cstrIs(strs, strSize, a[0], "id"))
                        mine = intSize >= 8 && a[2] <= intSize - 8 && le32(ints + a[2] + 4) == kJacketId;
                    else if (a[1] == 8 && cstrIs(strs, strSize, a[0], "src")) so = a[2], ss = a[3];
                    else if (a[1] == 1 && cstrIs(strs, strSize, a[0], "origsize")) os = a[2];
                    else if (a[1] == 3 && cstrIs(strs, strSize, a[0], "compress"))
                        pk = a[3] == 2 && strSize >= 2 && a[2] <= strSize - 2 && strs[a[2]] == 'o' &&
                             strs[a[2] + 1] == 'n';
                }
                if (mine) srcOff = so, srcSize = ss, origSize = os, packed = pk, found = true;
            }
            o += 28 + nattr * 16;
        }
        if (!found || !srcSize) { why = "texture not found"; break; }
        const uint32_t size = packed ? origSize : srcSize;
        if (!size || size > 0x40000 || srcSize > need) { why = "texture too big"; break; }
        out = ssm_sceKernelAllocMemBlock("StartScreenMusicJacket", kMemUserRw, (size + 0xFFF) & ~0xFFFu, NULL);
        uint8_t* dst = NULL;
        if (out < 0 || ssm_sceKernelGetMemBlockBase(out, (void**)&dst) < 0) { why = "no memory"; break; }
        if (!readAt(fd, fileOff + srcOff, packed ? base : dst, srcSize)) { why = "texture not read"; break; }
        if (packed) {
            if (!g_pafOpt[OPT_DECOMPRESS].fn) { why = "no Decompress"; break; }
            const int32_t rc = ((DecompressFn)g_pafOpt[OPT_DECOMPRESS].fn)(dst, size, base, srcSize, 1 /* zlib */);
            if (rc < 0 || sceClibMemcmp(dst, "MIG.", 4)) {
                logLine("jacket: Decompress 0x%08X", (unsigned)rc);
                why = "not unpacked";
                break;
            }
        }
        g_jacketData = dst;
        g_jacketSize = size;
        out = -1;  // kept
    } while (0);
    if (out >= 0) ssm_sceKernelFreeMemBlock(out);
    if (mem >= 0) ssm_sceKernelFreeMemBlock(mem);
    sceIoClose(fd);
    if (why) {
        logLine("jacket: %s: the note instead", why);
        return;
    }
    __sync_synchronize();
    g_jacketReady = true;
}

int coverThread(SceSize, void*) {
    findOpt();
    loadJacket();

    Want w, last;
    sceClibMemset(&last, 0, sizeof(last));
    int done = 0;
    for (;;) {
        sceKernelWaitSema(g_wakeCover, 1, NULL);
        if (g_forgetCovers) {
            g_forgetCovers = false;
            forgetCovers();
        }
        if (g_mailSeq != g_mailTaken) continue;  // the last answer not taken yet (taking it wakes this)
        const int seq = g_reqSeq;
        if ((seq & 1) || seq == done) continue;
        __sync_synchronize();
        sceClibMemcpy(&w, &g_req, sizeof(w));
        __sync_synchronize();
        if (g_reqSeq != seq) continue;
        done = seq;
        CoverMail m = {COVER_SAME, -1, NULL, 0};
        if (!w.album[0] || !sameText(w.album, last.album) || !sameText(w.artist, last.artist)) {
            m = findCover(w);
            last = w;
        }
        g_mail = m;
        __sync_synchronize();
        g_mailSeq = seq;
    }
    return 0;
}

// ── The panel ───────────────────────────────────────────────────────────────
// The quick menu's own proportions (bar 380 long, speakers outside it,
// buttons 106 x 46 spaced 137 under it, 62 lower), the whole panel scaled
// evenly into the clock's box: 504 quick-menu units across -> screen x 60
// (the date's and hours' left edge) to 430 (the minutes' fixed right edge,
// power_manage_plugin's layout), top at y 80: as far below the top of the
// start screen's frame (y 64) as the clock's digits end above its bottom
// (495 and 511), 16 pixels (measured on screenshots).
const float kPanelLeft = 60, kPanelRight = 430, kPanelTop = 80;
const float kPanelScale = (kPanelRight - kPanelLeft) / 504.0f;
float panelScale() { return kPanelScale; }
// A panel point in its group's units: the groups sit at the page's centre
// (screen 480, 272) and are scaled about it; y up.
float localX(float nx) { return (kPanelLeft - 480) / kPanelScale + nx; }
float localY(float ny) { return (272 - kPanelTop) / kPanelScale - ny; }

// Parts of the quick menu's Music row, by id.
const uint32_t kNoteIcon = 0xE219E0DE, kHeading = 0x90E6251E, kTitle = 0x66FDAFE3,
               kArtist = 0xF099B450, kNoMusic = 0x5608C548, kBar = 0xDC2C2D38,
               kAppIcon = 0xC6B67573;
const uint32_t kPrev = 0x9F6A44DA, kPlay = 0x56734CD1, kNext = 0x6CA674D6;
const uint32_t kPrevIcon = 0xD639AB27, kPlayIcon = 0xE057A7D0, kPauseIcon = 0xB26E4562,
               kNextIcon = 0x6FA104CC;
const uint32_t kRowTemplate = 0x8B23313D, kClockGroup = 0xCDB7FA69, kDateText = 0xC8692D70;
const uint32_t kUnknown = 0xB8A55B5E;  // impose_plugin's "Unknown"

const wchar_t* sonyString(uint32_t id) {
    if (!g_impose || !g_pafOpt[OPT_GET_STRING].fn) return NULL;
    HashID h;
    sceClibMemset(&h, 0, sizeof(h));
    h.hash = id;
    return ((GetStringFn)g_pafOpt[OPT_GET_STRING].fn)(g_impose, *(const paf::IDParam*)&h);
}

// This start screen's parts (valid while g_screenAlive).
paf::ui::Widget *g_groups[2], *g_titleLine, *g_artistLine, *g_playIcon, *g_pauseIcon, *g_cover;
paf::ui::SlideBar* g_bar;
uint32_t g_barVtable;  // checked before each write, as a second guard
int g_shownSeq = -1;   // the music state the panel shows
bool g_panelShown;     // a song is loaded: the panel is shown and takes taps
int g_playState;       // what the play button does: pause while 1
int g_coverShows;      // the picture on g_cover: 0 the note, else g_uiCover.id

// The cover the UI thread holds (taken from the cover thread's mail).
struct UiCover {
    SceUID uid;  // its memory, -1 for none
    const uint8_t* data;
    uint32_t size;
    int id;       // counts up per picture
    bool failed;  // PAF would not load it: the note
    bool pinned;  // PAF kept the file reading it: never freed
};
UiCover g_uiCover = {-1, NULL, 0, 0, false, false};
paf::graph::Surface* g_noteTex;  // the row's note, held for good
paf::graph::SurfacePool* g_pool;  // power_manage_plugin's

void screenGone(int gen) {
    if (gen == g_screenGen) {
        g_screenAlive = false;
        g_bar = NULL;
        g_cover = NULL;
        g_panelShown = false;
    }
}

void freeUiCover() {
    if (g_uiCover.uid >= 0 && !g_uiCover.pinned) ssm_sceKernelFreeMemBlock(g_uiCover.uid);
    g_uiCover.uid = -1;
    g_uiCover.data = NULL;
    g_uiCover.size = 0;
}

// The cover thread's answer, every UI tick.
void takeCover() {
    const int seq = g_mailSeq;
    if (seq == g_mailTaken) return;
    __sync_synchronize();
    if (g_mail.kind != COVER_SAME) {
        freeUiCover();
        g_uiCover.uid = g_mail.uid;
        g_uiCover.data = g_mail.data;
        g_uiCover.size = g_mail.size;
        g_uiCover.id++;
        g_uiCover.failed = false;
        g_uiCover.pinned = false;
    }
    __sync_synchronize();
    g_mailTaken = seq;
    wake(g_wakeCover);  // a newer song may be waiting
}

// A picture in memory as a texture, with one reference: the caller's.
struct RefCounter {  // paf::common::detail::RefCounter
    void* ptr;
    int32_t counter, weak;
    void (*deleter)(void*);
};
struct SharedPtrRaw {  // paf::common::SharedPtr
    void* ptr;
    RefCounter* rc;
};

// The pools a texture may come from: power_manage_plugin's (Vita Shell
// Companion's), then PAF's default one.
paf::graph::SurfacePool* pool(int i) {
    if (i == 0) return g_pool;
    if (!g_pafOpt[OPT_DEFAULT_POOL].fn) return NULL;
    return ((DefaultPoolFn)g_pafOpt[OPT_DEFAULT_POOL].fn)();
}

// This plugin's reference to a file, let go as SharedPtr's destructor would.
// true when that was the last (the file is gone). Note: its 16-byte
// counter block is left, not freed.
bool dropFile(SharedPtrRaw& f) {
    const bool last = __atomic_sub_fetch(&f.rc->counter, 1, __ATOMIC_ACQ_REL) == 0;
    if (last) f.rc->deleter(f.rc->ptr);
    return last;
}

paf::graph::Surface* loadFile(SharedPtrRaw& file, int& from) {
    for (int i = 0; i < 2; i++) {
        paf::graph::SurfacePool* p = pool(i);
        if (!p) continue;
        void* tex[1] = {NULL};  // an intrusive_ptr<Surface>
        ((SurfaceLoadFn)g_pafOpt[OPT_SURFACE_LOAD].fn)(tex, p, &file, NULL);
        if (tex[0]) {
            from = i;
            return (paf::graph::Surface*)tex[0];
        }
    }
    return NULL;
}

bool g_jacketFailed;  // PAF would not load it

// A picture in memory as a texture, with one reference: the caller's, into
// either pool (in testing, the second took covers when the first was full).
paf::graph::Surface* loadImage(const char* what, const void* data, uint32_t size, bool& pinned) {
    pinned = false;
    if (!g_pafOpt[OPT_SURFACE_LOAD].fn || !g_pafOpt[OPT_MEMFILE_OPEN].fn) return NULL;
    SharedPtrRaw file = {NULL, NULL};
    int32_t err = 0;
    ((MemFileOpenFn)g_pafOpt[OPT_MEMFILE_OPEN].fn)(&file, data, size, &err);
    if (err < 0 || !file.ptr || !file.rc) {
        logLine("%s: MemFile 0x%08X", what, (unsigned)err);
        return NULL;
    }
    int from = -1;
    paf::graph::Surface* s = loadFile(file, from);
    pinned = !dropFile(file);
    if (!s) logLine("%s: not loaded (no room in either texture pool)", what);
    return s;
}

const int kShowNote = 0, kShowJacket = -1;  // g_coverShows, besides a cover's id

// A picture on the cover square, drawn kCoverUnits wide whatever its own
// size (a plane draws its texture at the texture's size; its scale sets how
// big). The plane takes its own reference; this one is let go.
const float kCoverUnits = 150;
void showPicture(paf::graph::Surface* s) {
    giveTexture(g_cover, s);
    const int w = s->GetWidth();
    setScale(g_cover, w > 0 ? kCoverUnits / w : 1);
    s->UnsafeRelease();
}

// No motion: like the clock, the panel is simply there when the start screen
// lights up, and a new song's text and cover just replace the old ones, as
// the clock's digits do.
float g_coverRgb[3] = {1, 1, 1};  // white for a cover; the theme's for the jacket and note

// What the cover square should show: the song's cover, else Sony's
// no-cover jacket, else the quick menu's note (kShowNote).
int coverWant() {
    if (g_uiCover.data && !g_uiCover.failed) return g_uiCover.id;
    if (g_jacketReady && !g_jacketFailed) return kShowJacket;
    return kShowNote;
}

// Puts what is wanted on the square; a picture that won't load falls to the
// next. The jacket is loaded each time it is wanted and goes with the start
// screen, as covers do: the texture pool has little room.
void swapCover() {
    int want = coverWant();
    if (want > 0) {
        bool pinned = false;
        paf::graph::Surface* s = loadImage("cover", g_uiCover.data, g_uiCover.size, pinned);
        g_uiCover.pinned = pinned;
        if (s) {
            showPicture(s);
            g_coverRgb[0] = g_coverRgb[1] = g_coverRgb[2] = 1;
            g_coverShows = want;
            return;
        }
        g_uiCover.failed = true;
        freeUiCover();
        want = coverWant();
    }
    if (want == kShowJacket) {
        bool pinned = false;  // the jacket's buffer is kept anyway
        paf::graph::Surface* j = loadImage("jacket", g_jacketData, g_jacketSize, pinned);
        if (j) {
            showPicture(j);
            sceClibMemcpy(g_coverRgb, g_tint, sizeof(g_coverRgb));
            g_coverShows = kShowJacket;
            return;
        }
        g_jacketFailed = true;
        logLine("jacket: not loaded: the note");
    }
    if (g_noteTex) giveTexture(g_cover, g_noteTex);
    setScale(g_cover, 1);  // the note at its own 80 x 80
    sceClibMemcpy(g_coverRgb, g_tint, sizeof(g_coverRgb));
    g_coverShows = kShowNote;
}

// The cover square, every UI tick while the panel is up.
void applyCover() {
    if (!g_screenAlive || !g_cover || !g_panelShown || coverWant() == g_coverShows) return;
    swapCover();
    setV4(PAF_SET_COLOR, 0x10001, g_cover, g_coverRgb[0], g_coverRgb[1], g_coverRgb[2], 1);
}

// The volume bar: 380 units from panel x 59, at panel y 200.
const float kBarLeft = kPanelLeft + 59 * kPanelScale, kBarWidth = 380 * kPanelScale,
            kBarY = kPanelTop + 200 * kPanelScale;
int g_volShown = -1;
bool g_dragging, g_touching;

struct PeekBar : paf::ui::SlideBar {
    static void set(paf::ui::SlideBar* w, float v) {
        PeekBar* p = static_cast<PeekBar*>(w);
        p->m_value = v;
        p->m_unk_0x294 = (int)((float)p->m_fraction * v);
        ((UpdateSliderFn)g_paf[PAF_SLIDER_UPDATE].fn)(w, false);
    }
};

void showVolume(int vol) {
    if (!g_screenAlive || !g_bar || *(const uint32_t*)g_bar != g_barVtable || vol == g_volShown) return;
    g_volShown = vol;
    PeekBar::set(g_bar, vol / 30.0f);
}

// Every UI tick while the panel is up: a finger that lands on the bar's
// row drags it.
void driveBar() {
    if (!g_screenAlive || !g_panelShown || !g_bar) {
        g_dragging = false;
        return;
    }
    SceTouchData t;
    sceClibMemset(&t, 0, sizeof(t));
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &t, 1) < 0 || t.reportNum == 0) {
        g_dragging = g_touching = false;
        return;
    }
    // Like the quick menu's bar: only a finger that lands on it drags it, so
    // a finger passing over it (pulling the peel) changes nothing.
    const bool landed = !g_touching;
    g_touching = true;
    const int x = t.report[0].x / 2, y = t.report[0].y / 2;  // the panel is 1920 x 1088
    if (!g_dragging) {
        if (!landed || x < kBarLeft - 30 || x > kBarLeft + kBarWidth + 30 || y < kBarY - 30 || y > kBarY + 30)
            return;
        g_dragging = true;
    }
    int vol = (int)((x - kBarLeft) * 30.0f / kBarWidth + 0.5f);
    vol = vol < 0 ? 0 : vol > 30 ? 30 : vol;
    if (vol == g_volShown) return;
    sceAVConfigSetSystemVol(vol);
    // The volume the system took: with AVLS on, it stops at its limit, as
    // the quick menu's bar does.
    int real;
    if (sceAVConfigGetSystemVol(&real) >= 0) vol = real;
    showVolume(vol);
}

// The buttons.
char g_listenerMem[3][sizeof(Listener)] __attribute__((aligned(8)));
Listener* g_listeners[3];

volatile bool g_pollSoon;  // a press: the music thread looks again at once

void onPress(int32_t, paf::ui::Handler*, paf::ui::Event*, void* data) {
    uint32_t cmd = (uint32_t)(uintptr_t)data;
    if (cmd == CMD_PLAY && g_playState == 1) cmd = CMD_STOP;  // one button: pause while playing
    musicCommand(cmd);
    g_fastPolls = 12;  // the new song shows as soon as the service has it
    g_pollSoon = true;
    wake(g_wakeMusic);
}

// The music state onto the panel (UI thread); a song without a title or
// artist shows Sony's "Unknown", in the system language. A new song's title
// goes on at once, as the quick menu's does; its cover follows when ready.
void applyMusic() {
    if (!g_screenAlive || !g_groups[0] || g_shownSeq == g_nowSeq) return;
    g_shownSeq = g_nowSeq;
    const Now& n = g_now[g_nowIdx];
    g_playState = n.state;
    if (n.loaded != g_panelShown) {
        show(g_groups[0], n.loaded);
        show(g_groups[1], n.loaded);
        g_panelShown = n.loaded;
    }
    if (!n.loaded) return;
    if (g_titleLine) {
        if (n.title[0]) setLine(g_titleLine, 0, n.title);
        else setLineW(g_titleLine, 0, sonyString(kUnknown));
    }
    if (g_artistLine) {
        if (n.artist[0]) setLine(g_artistLine, 1, n.artist);
        else setLineW(g_artistLine, 1, sonyString(kUnknown));
    }
    const bool playing = n.state == 1;
    if (g_playIcon) setSize(g_playIcon, playing ? 0 : 64, playing ? 0 : 64);
    if (g_pauseIcon) setSize(g_pauseIcon, playing ? 64 : 0, playing ? 64 : 0);
}

void addPanel(paf::ui::Widget* page) {
    if (!findPaf()) return;
    findOpt();
    if (!g_impose) g_impose = ((PluginFindFn)g_paf[PAF_FIND].fn)("impose_plugin");
    paf::Plugin* const impose = g_impose;
    if (!impose) {
        logLine("panel: impose_plugin not found");
        return;
    }
    if (!g_pool) {
        // PAF's Plugin keeps its texture pool at 0xD4 (Vita Shell Companion).
        paf::Plugin* power = ((PluginFindFn)g_paf[PAF_FIND].fn)("power_manage_plugin");
        if (power) g_pool = *(paf::graph::SurfacePool* const*)((const char*)power + 0xD4);
    }
    // Into the clock's group (full screen, centred like the page), so it
    // fades with the date and time on a peel.
    paf::ui::Widget* parent = findChild(page, kClockGroup);
    if (!parent) parent = page;
    HashID id;
    sceClibMemset(&id, 0, sizeof(id));
    id.hash = kRowTemplate;
    const void* param[2] = {NULL, NULL};  // TemplateOpenParam: no event table, no sibling
    const int rc = ((TemplateOpenFn)g_paf[PAF_OPEN].fn)(impose, parent, *(const paf::IDParam*)&id, param);
    if (rc < 0) {
        logLine("panel: template open 0x%08X", (unsigned)rc);
        return;
    }
    // The template added three children at the end: the heading and bar
    // group, the lines and buttons group, the quick menu's divider.
    paf::ui::Widget* added[3] = {NULL, NULL, NULL};
    int count = 0;
    for (paf::list<paf::ui::Widget*>::iterator it = parent->GetChildrenBegin(); it != parent->GetChildrenEnd(); ++it)
        added[count++ % 3] = *it;
    if (count < 4) return;
    paf::ui::Widget* heading = added[count % 3];
    paf::ui::Widget* controls = added[(count + 1) % 3];
    setSize(added[(count + 2) % 3], 0, 0);  // the divider

    // Layout, in quick-menu units from the panel's top left: the cover 150
    // square at the top left (the row's note plane, centred at 75, 75);
    // title and artist from x 170, centred on the cover (y 55 / 95); bar
    // centre (249, 200); buttons at (112 / 249 / 386, 262). Left-anchored
    // parts count from the group's left edge (-364); text takes its left edge
    // from the position; the title's and artist's vertical alignment shifts
    // them 20. No "Music" heading: the cover says what it is.
    setPos(heading, 0, 0);
    setPos(controls, 0, 0);
    setScale(heading, kPanelScale);
    setScale(controls, kPanelScale);
    struct Place {
        uint32_t id;
        float nx, ny;
        bool left;
        float shift;
    };
    const Place places[] = {
        {kNoteIcon, 75, 75, true, 0},
        {kTitle, 170, 55, true, -20},   {kArtist, 170, 95, true, 20},
        {kNoMusic, 170, 55, true, -20}, {kBar, 249, 200, false, 0},
        {kPrev, 112, 262, false, 0},    {kPrevIcon, 112, 262, false, 0},
        {kPlay, 249, 262, false, 0},    {kPlayIcon, 249, 262, false, 0},
        {kPauseIcon, 249, 262, false, 0},
        {kNext, 386, 262, false, 0},    {kNextIcon, 386, 262, false, 0},
    };
    for (unsigned i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
        paf::ui::Widget* w = findChild(page, places[i].id);
        if (w) setPos(w, localX(places[i].nx) + (places[i].left ? 364 : 0), localY(places[i].ny) + places[i].shift);
    }
    paf::ui::Widget* w;
    if ((w = findChild(page, kAppIcon))) setSize(w, 0, 0);  // a blank square in the quick menu's row
    if ((w = findChild(page, kNoMusic))) setSize(w, 0, 0);  // the panel only shows with a song
    if ((w = findChild(page, kHeading))) setScale(w, 0);    // no "Music" heading
    // The theme's clock colour (white unless a theme sets its own). On the
    // Vita a colour's alpha is its top byte, whatever the headers' getters
    // say (Vita Shell Companion: the quick menu's lines read D8FFFFFF and
    // CCFFFFFF); the colour below it is taken as blue, green, red, the GPU's
    // byte order.
    g_tint[0] = g_tint[1] = g_tint[2] = 1;
    if ((w = findChild(page, kDateText))) {
        const uint32_t c = w->GetColor32();
        if (c >> 24) {
            g_tint[0] = (c & 0xFF) / 255.0f;
            g_tint[1] = (c >> 8 & 0xFF) / 255.0f;
            g_tint[2] = (c >> 16 & 0xFF) / 255.0f;
        }
        if (tinted()) logLine("theme: clock colour %08X", (unsigned)c);
    }
    // The cover square: the note plane (applyCover gives it its colour).
    g_cover = findChild(page, kNoteIcon);
    g_coverShows = 0;
    if (g_cover) {
        setTint(g_cover);
        if (!g_noteTex) g_noteTex = holdTexture(g_cover);
    }

    g_titleLine = findChild(page, kTitle);
    g_artistLine = findChild(page, kArtist);
    // To the panel's right edge; in the quick menu's own colours (title 85%,
    // artist 80% white, impose_plugin's styles).
    if (g_titleLine) setSize(g_titleLine, 334, 40);
    if (g_artistLine) setSize(g_artistLine, 334, 40);
    if (tinted()) {  // the lines keep their own alphas (85% and 80%)
        if (g_titleLine) setTint(g_titleLine);
        if (g_artistLine) setTint(g_artistLine);
    }
    g_playIcon = findChild(page, kPlayIcon);
    g_pauseIcon = findChild(page, kPauseIcon);

    // The active look, always: buttons switched on, in their normal state
    // (Enable alone leaves them dimmed, as Vita Shell Companion found) and
    // white; symbols and bar white.
    const uint32_t buttons[3] = {kPrev, kPlay, kNext};
    const uint32_t cmds[3] = {CMD_PREVIOUS, CMD_PLAY, CMD_NEXT};
    for (int i = 0; i < 3; i++) {
        paf::ui::Widget* b = findChild(page, buttons[i]);
        if (!b) continue;
        if (!g_listeners[i]) g_listeners[i] = new (g_listenerMem[i]) Listener(onPress, (void*)(uintptr_t)cmds[i]);
        ((EnableFn)g_paf[PAF_BTN_ENABLE].fn)((paf::ui::ButtonBase*)b, true);
        ((paf::ui::ButtonBase*)b)->ChangeButtonState(paf::ui::ButtonBase::ST_BTN_NORMAL);
        setTint(b);
        ((AddListenerFn)g_paf[PAF_ADD_LISTENER].fn)(b, 0x10000008 /* CB_BTN_DECIDE */, g_listeners[i], true);
    }
    const uint32_t icons[4] = {kPrevIcon, kPlayIcon, kPauseIcon, kNextIcon};
    for (int i = 0; i < 4; i++)
        if ((w = findChild(page, icons[i]))) setTint(w);
    g_bar = (paf::ui::SlideBar*)findChild(page, kBar);
    if (g_bar) {
        setTint(g_bar);
        if (g_bar->GetChildrenNum() > 0) setTint(*g_bar->GetChildrenBegin());  // the knob
    }
    g_barVtable = g_bar ? *(const uint32_t*)g_bar : 0;
    g_volShown = -1;

    // Hidden until the music thread says a song is loaded.
    show(heading, false);
    show(controls, false);
    g_groups[0] = heading;
    g_groups[1] = controls;
    g_panelShown = false;
    g_shownSeq = -1;

    // This start screen is up until SceShell deletes the marker with its page.
    const int gen = g_screenGen + 1;
    Marker* m = new (g_markerMem[gen % 4]) Marker(gen);
    const int rcMark = ((AddListenerFn)g_paf[PAF_ADD_LISTENER].fn)(page, 0x100006 /* CB_COMMAND */, m, false);
    g_screenGen = gen;
    g_screenAlive = rcMark >= 0;
    int vol;
    if (sceAVConfigGetSystemVol(&vol) >= 0) showVolume(vol);
    if (rcMark < 0) logLine("start screen %d: marker 0x%08X", gen, (unsigned)rcMark);
    g_pollSoon = true;  // the music thread looks at once
    wake(g_wakeMusic);
    applyMusic();
}

// ── The start screen's setup function ──────────────────────────────────────
// Offsets from Skinpeel's source (skinpeel.c, GetOffsets): its 3.60 case,
// and the offset its commented-out cases share for five later NIDs. Checked
// before hooking: the function must save lr, and +0x13E must be a call.

struct ShellFw {
    uint32_t nid, lockInit;  // Thumb function, offset in segment 0
};
const ShellFw kFw[] = {
    {0x0552F692, 0x23B7BE},  // 3.60
    {0x5549BF1F, 0x23B85A},  // 3.65
    {0x34B4D82E, 0x23B85A}, {0x12DAC0F3, 0x23B85A}, {0x0703C828, 0x23B85A}, {0x2053B5A5, 0x23B85A},
};

tai_hook_ref_t g_lockInitRef;
uint32_t g_pafText, g_pafTextSize;  // ScePaf's code: a widget's vtable points into it

uint16_t half(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
bool looksLikePointer(uint32_t v) { return v >= 0x80000000u && v < 0xA0000000u && !(v & 3); }

bool findPafText() {
    if (g_pafTextSize) return true;
    tai_module_info_t ti;
    sceClibMemset(&ti, 0, sizeof(ti));
    ti.size = sizeof(ti);
    if (taiGetModuleInfo("ScePaf", &ti) < 0) return false;
    SceKernelModuleInfo mi;
    sceClibMemset(&mi, 0, sizeof(mi));
    mi.size = sizeof(mi);
    if (sceKernelGetModuleInfo(ti.modid, &mi) < 0) return false;
    g_pafText = (uint32_t)mi.segments[0].vaddr;
    g_pafTextSize = mi.segments[0].memsz;
    return true;
}

// A PAF widget: its vtable in ScePaf's code and a sane size.
bool looksLikeWidget(uint32_t p) {
    if (!looksLikePointer(p) || !findPafText()) return false;
    const uint32_t vt = *(const uint32_t*)p;
    if (vt < g_pafText || vt >= g_pafText + g_pafTextSize) return false;
    const float* sz = (const float*)(p + 0x48);  // m_size
    return sz[0] >= 0 && sz[0] <= 4096 && sz[1] >= 0 && sz[1] <= 4096;
}

uint32_t lockInitHook(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    // TAI_CONTINUE written out: its macro casts to a no-argument function, which C++ rejects.
    typedef uint32_t (*Orig)(uint32_t, uint32_t, uint32_t, uint32_t);
    const _tai_hook_user* cur = (const _tai_hook_user*)g_lockInitRef;
    const _tai_hook_user* next = (const _tai_hook_user*)cur->next;
    const uint32_t ret = next ? ((Orig)next->func)(a, b, c, d) : ((Orig)cur->old)(a, b, c, d);
    // The object's third word is the start screen's page.
    if (looksLikePointer(a) && looksLikeWidget(((const uint32_t*)a)[2]))
        addPanel((paf::ui::Widget*)((const uint32_t*)a)[2]);
    else
        logLine("start screen page not found");
    return ret;
}

void hookStartScreen() {
    tai_module_info_t ti;
    sceClibMemset(&ti, 0, sizeof(ti));
    ti.size = sizeof(ti);
    if (taiGetModuleInfo("SceShell", &ti) < 0) return;
    uint32_t off = 0;
    for (unsigned i = 0; i < sizeof(kFw) / sizeof(kFw[0]); i++)
        if (kFw[i].nid == ti.module_nid) off = kFw[i].lockInit;
    if (!off) {
        logLine("SceShell 0x%08X: firmware not supported", (unsigned)ti.module_nid);
        return;
    }
    SceKernelModuleInfo mi;
    sceClibMemset(&mi, 0, sizeof(mi));
    mi.size = sizeof(mi);
    if (sceKernelGetModuleInfo(ti.modid, &mi) < 0) return;
    const uint8_t* fn = (const uint8_t*)mi.segments[0].vaddr + off;
    const uint16_t h0 = half(fn), h1 = half(fn + 2), b0 = half(fn + 0x13E), b1 = half(fn + 0x140);
    const bool push = (h0 & 0xFF00) == 0xB500 || (h0 == 0xE92D && (h1 & 0x4000));
    const bool call = (b0 & 0xF800) == 0xF000 && (b1 & 0xC000) == 0xC000;
    if (!push || !call) {
        logLine("SceShell 0x%08X: start screen setup not where expected, not hooked", (unsigned)ti.module_nid);
        return;
    }
    tai_offset_args_t args;
    sceClibMemset(&args, 0, sizeof(args));
    args.size = sizeof(args);
    args.modid = ti.modid;
    args.segidx = 0;
    args.offset = off;
    args.thumb = 1;
    args.source = (const void*)lockInitHook;
    const SceUID uid = taiHookFunctionOffsetForUser(&g_lockInitRef, &args);
    logLine("SceShell 0x%08X: start screen hooked (0x%08X)", (unsigned)ti.module_nid, (unsigned)uid);
}

// ── Threads ─────────────────────────────────────────────────────────────────

// SceShell's UI thread, every time round its loop.
SceInt64 g_nextSlow;

volatile bool g_tickRan;  // SceShell's UI loop is running
volatile SceInt64 g_lastTick;  // when it last ran

void tick(void*) {
    g_tickRan = true;
    const SceInt64 t = sceKernelGetProcessTimeWide();
    g_lastTick = t;
    driveBar();
    applyMusic();
    takeCover();
    applyCover();
    if (!g_screenAlive || t < g_nextSlow) return;
    g_nextSlow = t + 100000;
    int vol;  // the knob follows the volume buttons
    if (g_panelShown && !g_dragging && sceAVConfigGetSystemVol(&vol) >= 0) showVolume(vol);
}

int mainThread(SceSize, void*) {
    // ScePaf loads after the shell's plugins start.
    for (int waited = 0; !findPafText(); waited++) {
        if (waited == 120) {
            logLine("ScePaf never loaded");
            return 0;
        }
        sceKernelDelayThread(500 * 1000);
    }
    // Then 5 s for SceShell to finish starting: registering on its UI loop as
    // soon as ScePaf appears stopped the Vita at boot in testing; with the
    // wait it booted every time.
    sceKernelDelayThread(5 * 1000 * 1000);
    if (!findPaf()) return 0;
    ((RegisterFn)g_paf[PAF_REGISTER].fn)(tick, NULL);
    const char* const mods[2] = {"SceShellSvc", "SceShellUtil"};
    for (int i = 0; i < 2 && !g_svc; i++) {
        uintptr_t fn = 0;
        if (taiGetModuleExportFunc(mods[i], 0x773242C7, 0xB31E7F1C, &fn) >= 0) g_svc = (GetSvcObjFn)fn;
    }
    logLine("ready (music service %s)", g_svc ? "found" : "NOT found");
    // The music service only once SceShell's UI loop runs, and 5 s more:
    // a test build that called it at once never got an answer at boot.
    while (!g_tickRan) sceKernelDelayThread(500 * 1000);
    sceKernelDelayThread(5 * 1000 * 1000);
    g_wakeMusic = sceKernelCreateSema("StartScreenMusicWake", 0, 0, 1, NULL);
    g_wakeCover = sceKernelCreateSema("StartScreenMusicCoverWake", 0, 0, 1, NULL);
    // The covers at the lowest priority: their reading never takes time from
    // anything on screen.
    SceUID cover = sceKernelCreateThread("StartScreenMusicCover", coverThread, 191, 0x4000, 0, 0, NULL);
    if (cover < 0) {  // a priority SceShell won't take: its default
        logLine("cover thread at 191: 0x%08X", (unsigned)cover);
        cover = sceKernelCreateThread("StartScreenMusicCover", coverThread, 0x10000100, 0x4000, 0, 0, NULL);
        g_coverPriority = 0x10000100;
    }
    if (cover >= 0) sceKernelStartThread(cover, 0, NULL);
    startUpdateCheck();  // waits two minutes on its own thread, then ends
    // Every 250 ms while the start screen is drawn (its UI loop ran in the
    // last half second; with the screen off it may not), at once after a
    // press or a new start screen, and every 2 s otherwise (to have the next
    // song's cover ready). Asleep in between.
    SceInt64 next = 0;
    for (;;) {
        SceInt64 now = sceKernelGetProcessTimeWide();
        if (g_svc && (now >= next || g_pollSoon)) {
            g_pollSoon = false;
            pollMusic();
            const bool drawn = g_screenAlive && now - g_lastTick < 500000;
            next = now + (g_fastPolls > 0 ? 50000 : drawn ? 250000 : 2000000);
            if (g_fastPolls > 0) g_fastPolls--;
            now = sceKernelGetProcessTimeWide();
        }
        SceUInt wait = next > now ? (SceUInt)(next - now) : 1000;
        if (g_wakeMusic >= 0) sceKernelWaitSema(g_wakeMusic, 1, &wait);
        else sceKernelDelayThread(wait);
    }
    return 0;
}

}  // namespace

extern "C" int module_start(SceSize, const void*) {
    sceIoRemove(LOG_PREV);
    sceIoRename(LOG_PATH, LOG_PREV);
    const SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) {
        sceIoClose(fd);
        g_logOk = true;
    }
    logLine("Start Screen Music %s", SSM_VERSION);
    hookStartScreen();
    const SceUID th = sceKernelCreateThread("StartScreenMusic", mainThread, 0x10000100, 0x4000, 0, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
    return SCE_KERNEL_START_SUCCESS;
}

// Never unloaded (SceShell plugins stay until reboot); add cleanup if that changes.
extern "C" int module_stop(SceSize, const void*) { return SCE_KERNEL_STOP_SUCCESS; }

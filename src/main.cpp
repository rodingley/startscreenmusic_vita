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
// The plugin's code ends just short of 64 KB, where the linker starts its
// data, and vita-elf-create needs about 1 KB after the code for the module's
// own tables ("segment 1 overlaps"). These 2 KB of nothing take the code
// past 64 KB, so the data starts at 128 KB with room to spare.
__attribute__((used)) const uint8_t kCodePad[2048] = {1};

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
    {"Plugin::GetTexture", 0x4D9A9DD0, 0x38FFFE8E, 0},      // for the glass
    {"PlaneObj::SetScaleMode", 0xA070D6A7, 0x14F2FA80, 0},  // ScePafGraphics
    {"Widget::SetRot", 0x073F8C68, 0xF50BF15B, 0},
    {"Text::SetStyleAttribute", 0x073F8C68, 0xCD20EF38, 0},  // the v4 one: a colour
    {"Text::GetStyleAttribute", 0x073F8C68, 0xF3281F1C, 0},  // the v4 one
};
enum {
    OPT_GET_STRING, OPT_MEMFILE_OPEN, OPT_SURFACE_LOAD, OPT_DECOMPRESS, OPT_DEFAULT_POOL, OPT_GET_TEXTURE,
    OPT_SCALE_MODE, OPT_SET_ROT, OPT_TEXT_STYLE, OPT_TEXT_STYLE_GET
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
typedef void (*GetTextureFn)(void* ret, paf::Plugin*, const paf::IDParam&);
typedef int (*TextStyleFn)(paf::ui::Widget*, int attr, uint32_t from, uint32_t len, const float* v4);
typedef int (*TextStyleGetFn)(paf::ui::Widget*, int attr, uint32_t index, float* v4);
typedef int (*SetRotFn)(paf::ui::Widget*, const float*, int, void*, int, int, float, void*, void*);
typedef void (*ScaleModeFn)(paf::graph::PlaneObj*, int, int);

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
wchar_t g_lineBuf[3][256];  // title, artist, the song's time

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
    bool loaded;  // a song in the shell's music player
    bool shown;   // the panel shows, as the quick menu's Music row would
    char title[256], artist[256];  // empty: Sony's "Unknown"
    int played, length;            // seconds (length 0: not known)
};
Now g_now[2];
volatile int g_nowIdx;
volatile int g_nowSeq;  // bumped on every change

Track g_track;  // the music thread's

// The Music app's LiveArea (watched further down): open, and whether it can be
// watched at all (if not, the panel shows whenever the player has a song).
volatile bool g_musicLiveArea;
volatile bool g_liveAreaWatched;

void copyStr(char* d, const char* s, int n) {
    sceClibStrncpy(d, s, n - 1);
    d[n - 1] = 0;
}

// The music and cover threads sleep until there is something to do: these
// wake them (a press or a new start screen; a new song or a cover taken).
SceUID g_wakeMusic = -1, g_wakeCover = -1;
volatile bool g_forgetCovers;  // the song went from the music player: its remembered covers can go
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
    // The song (6 KB from the service) only when the player has one.
    int rcT = -1;
    sceClibMemset(&g_track, 0, sizeof(g_track));
    if (n.state == 1 || n.state == 2) rcT = svcCall(0x3001B, &in, 1, &g_track, sizeof(g_track));
    n.loaded = rcT >= 0 && (n.state == 1 || n.state == 2);
    // The panel shows when the quick menu's Music row does (seen on 3.65):
    // while music plays, and while the player has a song and the Music app's
    // LiveArea is open, the app running or not, wherever the music was
    // started. Closing the LiveArea stops the music and the row goes; it
    // comes back when the LiveArea opens again.
    n.shown = n.loaded && (n.state == 1 || g_musicLiveArea || !g_liveAreaWatched);
    if (was.loaded && !n.loaded) {  // the player has no song now
        g_forgetCovers = true;
        wake(g_wakeCover);
    }
    copyStr(n.title, n.loaded ? g_track.title : "", sizeof(n.title));
    copyStr(n.artist, n.loaded ? g_track.artist : "", sizeof(n.artist));
    // How far in, from the same status (milliseconds at 0x2C, as the Music
    // app reads it); the song's length from the song.
    n.played = n.loaded ? (int)(st[0x2C / 4] / 1000) : 0;
    n.length = n.loaded ? (int)(g_track.durationMs / 1000) : 0;

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

    if (n.state == was.state && n.loaded == was.loaded && n.shown == was.shown && !sceClibStrcmp(n.title, was.title) &&
        !sceClibStrcmp(n.artist, was.artist) && n.played == was.played && n.length == was.length)
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
                    if (text == t.title && wanted && !sameText(t.title, wanted)) break;  // another song
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
// keep ready-made thumbnails, so a song shows its cover at once, even after a
// restart. One cover an album, as in the Music app (one a song for a song
// without an album). A file holds the path, size and time of the song it came
// from (that song changed or gone, the cover is made again) and the small
// PNG. The oldest go once there are more than kSavedMax. Covers saved by
// 1.0.x (".ssc", one a song) are deleted as they are come across.
#define COVER_DIR "ux0:data/startscreenmusic/covers"
#define COVER_EXT ".cov"
const int kSavedMax = 300;  // about 13 MB
const char kSavedMagic[4] = {'S', 'S', 'M', 'D'};

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

// Text into two hashes, as sameText compares it: case (ASCII) and spaces at
// either end don't count.
void hashText(uint32_t& a, uint32_t& b, const char* s) {
    while (*s == ' ') s++;
    int n = 0;
    while (s[n]) n++;
    while (n && s[n - 1] == ' ') n--;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        a = (a ^ (uint8_t)c) * 16777619u;
        b = (b ^ (uint8_t)c) * 0x5BD1E995u;
    }
    a = (a ^ 0xFF) * 16777619u;
    b = (b ^ 0xFF) * 0x5BD1E995u;
}

bool blank(const char* s) {
    while (*s == ' ') s++;
    return !*s;
}

// A cover's key: its album and artist, or, without an album, its song and
// artist.
void coverKey(const char* title, const char* album, const char* artist, uint32_t& a, uint32_t& b) {
    a = 2166136261u;
    b = 0x9747B28Cu;
    const bool byAlbum = !blank(album);
    hashText(a, b, byAlbum ? "album" : "song");
    hashText(a, b, byAlbum ? album : title);
    hashText(a, b, artist);
}

// The file for a cover.
void savedNameOf(const char* title, const char* album, const char* artist, char* out, int cap) {
    uint32_t a, b;
    coverKey(title, album, artist, a, b);
    sceClibSnprintf(out, cap, COVER_DIR "/%08x%08x" COVER_EXT, (unsigned)a, (unsigned)b);
}
void savedName(const Want& w, char* out, int cap) { savedNameOf(w.title, w.album, w.artist, out, cap); }

// The key of w's cover, for the covers kept in memory.
uint32_t albumKey(const Want& w) {
    uint32_t a, b;
    coverKey(w.title, w.album, w.artist, a, b);
    return a ? a : 1;
}

bool songStat(const char* path, uint64_t& size, uint64_t& time) {
    SceIoStat st;
    sceClibMemset(&st, 0, sizeof(st));
    if (sceIoGetstat(path, &st) < 0) return false;
    size = (uint64_t)st.st_size;
    time = when(st.st_mtime);
    return true;
}

// A saved cover's head, read from fd: true when it is whole and the song it
// came from is unchanged (fd is then at the PNG).
bool savedHeadFresh(SceUID fd, SavedHead& h, uint32_t cap) {
    char song[256];
    if (!(sceIoRead(fd, &h, sizeof(h)) == (int)sizeof(h) && !sceClibMemcmp(h.magic, kSavedMagic, 4) &&
          h.pngSize >= 8 && h.pngSize <= cap && h.pathLen > 0 && h.pathLen < sizeof(song) &&
          sceIoRead(fd, song, h.pathLen) == (int)h.pathLen))
        return false;
    song[h.pathLen] = 0;
    uint64_t size = 0, time = 0;
    return songStat(song, size, time) && size == ((uint64_t)h.songSizeHi << 32 | h.songSizeLo) &&
           time == ((uint64_t)h.songTimeHi << 32 | h.songTimeLo);
}

// True when the cover in file name is there and up to date.
bool savedFresh(const char* name) {
    const SceUID fd = sceIoOpen(name, SCE_O_RDONLY, 0);
    if (fd < 0) return false;
    SavedHead h;
    const bool fresh = savedHeadFresh(fd, h, pngSize(kCoverSide, kCoverSide, 3));
    sceIoClose(fd);
    return fresh;
}

// w's saved cover into m: true when there is one and its song is unchanged.
bool loadSaved(const Want& w, CoverMail& m, uint32_t cap) {
    char name[96];
    savedName(w, name, sizeof(name));
    const SceUID fd = sceIoOpen(name, SCE_O_RDONLY, 0);
    if (fd < 0) return false;
    SavedHead h;
    bool stale = !savedHeadFresh(fd, h, cap);
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
// most 32 at a time; this runs every 25 covers saved), and 1.0.x's covers.
// How many are left.
int pruneSaved() {
    struct Old {
        uint64_t time;
        char name[24];
    } old[32];
    int olds = 0, count = 0;
    const SceUID d = sceIoDopen(COVER_DIR);
    if (d < 0) return 0;
    SceIoDirent e;
    while (sceIoDread(d, &e) > 0) {
        int len = 0;
        while (e.d_name[len]) len++;
        if (len < 5 || len >= (int)sizeof(old[0].name)) continue;
        if (!sceClibStrcmp(e.d_name + len - 4, ".ssc")) {  // 1.0.x's
            char path[96];
            sceClibSnprintf(path, sizeof(path), COVER_DIR "/%s", e.d_name);
            sceIoRemove(path);
            continue;
        }
        if (sceClibStrcmp(e.d_name + len - 4, COVER_EXT)) continue;
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
        count--;
    }
    return count;
}

// Keeps a small cover (from the song at songPath) on the memory card as file
// name. Written beside it, then renamed: a half-written file never takes its
// name.
void saveCoverAs(const char* name, const char* songPath, const uint8_t* png, uint32_t size) {
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
    char temp[100];
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
void saveCover(const Want& w, const char* songPath, const uint8_t* png, uint32_t size) {
    char name[96];
    savedName(w, name, sizeof(name));
    saveCoverAs(name, songPath, png, size);
}

// The picture of the song at path (where t says it is) made a small PNG in a
// block of its own: its size (0 for none) and the block (uid, png). Reading
// the file at SceShell's default priority when raise is set (a song asked
// for), else all of it at the thread's lowest (covers made ahead).
const char* g_smallWhy;  // why not, for the log
uint32_t smallCover(const Tag& t, const char* path, bool raise, SceUID& pngUid, uint8_t*& png, CoverInfo& ci,
                    int& decodeMs, int& decodeCpuMs) {
    pngUid = -1;
    png = NULL;
    sceClibMemset(&ci, 0, sizeof(ci));
    decodeMs = decodeCpuMs = 0;
    g_smallWhy = "";
    if (t.picSize > kMaxCoverBytes) {
        g_smallWhy = "too big";
        return 0;
    }
    // The picture as it is in the file.
    uint8_t *pic = NULL, *work = NULL;
    const SceUID picUid = allocBlock("StartScreenMusicPicture", t.picSize, &pic);
    if (picUid < 0) {
        g_smallWhy = "memory";
        return 0;
    }
    if (raise) searchPriority(true);
    const SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    bool ok = fd >= 0 && sceIoLseek(fd, t.picOff, SCE_SEEK_SET) == (SceOff)t.picOff;
    for (uint32_t got = 0; ok && got < t.picSize;) {
        const int r = sceIoRead(fd, pic + got, t.picSize - got);
        ok = r > 0;
        got += ok ? r : 0;
    }
    if (fd >= 0) sceIoClose(fd);
    if (raise) searchPriority(false);
    // Decoded a block or a row at a time into the small square
    // (cover_image.cpp): about 115 KB for the square (a PNG's rows and
    // window more), and its small PNG, kept.
    const uint32_t pngCap = pngSize(kCoverSide, kCoverSide, 3);
    const uint32_t workSize = coverWorkSize(kCoverSide, pic, t.picSize);
    const SceUID workUid = ok ? allocBlock("StartScreenMusicDecode", workSize, &work) : -1;
    pngUid = workUid >= 0 ? allocBlock("StartScreenMusicCover", pngCap, &png) : -1;
    uint32_t pngLen = 0;
    const SceInt64 t1 = sceKernelGetProcessTimeWide(), cpu1 = threadCpuTime();
    if (pngUid >= 0) pngLen = coverToPng(pic, t.picSize, work, workSize, kCoverSide, png, pngCap, &ci);
    decodeMs = (int)(sceKernelGetProcessTimeWide() - t1) / 1000;
    decodeCpuMs = (int)(threadCpuTime() - cpu1) / 1000;
    if (workUid >= 0) ssm_sceKernelFreeMemBlock(workUid);
    ssm_sceKernelFreeMemBlock(picUid);
    if (!pngLen) {
        g_smallWhy = !ok ? "file" : pngUid < 0 ? "memory" : "picture";
        if (pngUid >= 0) ssm_sceKernelFreeMemBlock(pngUid);
        pngUid = -1;
        png = NULL;
    }
    return pngLen;
}

// The cover of w as a small PNG, or none (the jacket then).
CoverMail findCover(const Want& w) {
    CoverMail m = {COVER_NONE, -1, NULL, 0};
    const uint32_t wanted = albumKey(w);
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
    SceUID pngUid;
    uint8_t* png;
    CoverInfo ci;
    int decodeMs, decodeCpuMs;
    const uint32_t pngLen = smallCover(g_tag, g_path, true, pngUid, png, ci, decodeMs, decodeCpuMs);
    if (!pngLen) {
        if (!sceClibStrcmp(g_smallWhy, "too big"))
            logLine("song \"%s\": cover too big (%u bytes)", w.title, (unsigned)g_tag.picSize);
        else
            logLine("song \"%s\": cover not read (%s, error %d%s, %u KB free)", w.title, g_smallWhy, ci.error,
                    ci.error == 37 ? ": progressive JPEG" : ci.error == 100 ? ": interlaced PNG" : "",
                    (unsigned)(freeUserMemory() >> 10));
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

// ── Covers made ahead, as the Music app does ────────────────────────────────
// When the Music LiveArea opens (when the Music app looks for new songs), the
// cover thread goes through ux0:music, one file at a time between the songs
// it is asked for, and makes the cover of every album that has none yet, so
// a song shows its cover at once even the first time it plays. The files
// gone through are listed in LIBRARY_PATH (a hash of the path, the size and
// the time), so the next time only new or changed files are opened. Covers
// are made until there are kSavedMax; past that, songs make theirs when they
// play, as before.
#define LIBRARY_PATH "ux0:data/startscreenmusic/library.bin"
const char kLibraryMagic[4] = {'S', 'S', 'M', 'L'};
struct LibEntry {
    uint32_t path, size, time;
};
static_assert(sizeof(LibEntry) == 12, "LibEntry is written as it is");
const int kLibMax = 4096;  // songs listed (48 KB in memory during a pass)

volatile bool g_libraryWanted;  // the Music LiveArea opened: go through ux0:music

struct Pass {
    bool active;
    int depth;  // the folder being read: dirs[depth], or -1 when done
    SceUID dirs[4];
    int lens[4];  // g_passPath's length at each depth
    LibEntry* old;  // the last pass's list, sorted by path
    int oldCount;
    SceUID oldUid;
    SceUID listFd;  // this pass's list, written as it goes
    LibEntry buf[32];
    int bufCount, listed;
    int saved;  // covers in COVER_DIR
    int files, opened, made, none, unread, over;
    SceInt64 start;
};
Pass g_pass;
char g_passPath[256];

uint32_t pathHash(const char* s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

void sortEntries(LibEntry* e, int n) {  // shell sort by path hash
    for (int gap = n / 2; gap > 0; gap /= 2)
        for (int i = gap; i < n; i++) {
            const LibEntry x = e[i];
            int j = i;
            for (; j >= gap && e[j - gap].path > x.path; j -= gap) e[j] = e[j - gap];
            e[j] = x;
        }
}

// True when the last pass listed this file, unchanged.
bool listedBefore(const LibEntry& le) {
    int lo = 0, hi = g_pass.oldCount - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const LibEntry& o = g_pass.old[mid];
        if (o.path == le.path) return o.size == le.size && o.time == le.time;
        if (o.path < le.path) lo = mid + 1;
        else hi = mid - 1;
    }
    return false;
}

void list(const LibEntry& le) {
    Pass& p = g_pass;
    if (p.listFd < 0 || p.listed >= kLibMax) return;
    p.buf[p.bufCount++] = le;
    p.listed++;
    if (p.bufCount == 32) {
        if (sceIoWrite(p.listFd, p.buf, sizeof(p.buf)) != (int)sizeof(p.buf)) {
            sceIoClose(p.listFd);
            p.listFd = -1;
        }
        p.bufCount = 0;
    }
}

void startPass() {
    Pass& p = g_pass;
    sceClibMemset(&p, 0, sizeof(p));
    p.oldUid = p.listFd = -1;
    p.start = sceKernelGetProcessTimeWide();
    sceIoMkdir("ux0:data/startscreenmusic", 0777);
    sceIoMkdir(COVER_DIR, 0777);
    sceClibStrncpy(g_passPath, "ux0:music", sizeof(g_passPath));
    p.dirs[0] = sceIoDopen(g_passPath);
    if (p.dirs[0] < 0) return;  // no music
    p.lens[0] = 9;
    // The last pass's list.
    const SceUID fd = sceIoOpen(LIBRARY_PATH, SCE_O_RDONLY, 0);
    if (fd >= 0) {
        char magic[4];
        uint32_t count = 0;
        if (sceIoRead(fd, magic, 4) == 4 && !sceClibMemcmp(magic, kLibraryMagic, 4) && sceIoRead(fd, &count, 4) == 4 &&
            count > 0 && count <= (uint32_t)kLibMax) {
            p.oldUid = allocBlock("StartScreenMusicLibrary", count * sizeof(LibEntry), (uint8_t**)&p.old);
            if (p.oldUid >= 0 && sceIoRead(fd, p.old, count * sizeof(LibEntry)) == (int)(count * sizeof(LibEntry))) {
                p.oldCount = (int)count;
                sortEntries(p.old, p.oldCount);
            }
        }
        sceIoClose(fd);
    }
    // This pass's list: its count is written at the end.
    p.listFd = sceIoOpen(LIBRARY_PATH ".new", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (p.listFd >= 0) {
        const uint32_t zero = 0;
        if (sceIoWrite(p.listFd, kLibraryMagic, 4) != 4 || sceIoWrite(p.listFd, &zero, 4) != 4) {
            sceIoClose(p.listFd);
            p.listFd = -1;
        }
    }
    p.saved = pruneSaved();
    p.active = true;
}

void endPass() {
    Pass& p = g_pass;
    while (p.depth >= 0 && p.active) sceIoDclose(p.dirs[p.depth--]);
    if (p.oldUid >= 0) ssm_sceKernelFreeMemBlock(p.oldUid);
    p.oldUid = -1;
    bool listed = false;
    if (p.listFd >= 0) {
        const uint32_t count = (uint32_t)p.listed;
        listed = (!p.bufCount || sceIoWrite(p.listFd, p.buf, p.bufCount * sizeof(LibEntry)) ==
                                     (int)(p.bufCount * sizeof(LibEntry))) &&
                 sceIoLseek(p.listFd, 4, SCE_SEEK_SET) == 4 && sceIoWrite(p.listFd, &count, 4) == 4;
        sceIoClose(p.listFd);
        p.listFd = -1;
    }
    if (listed) {
        sceIoRemove(LIBRARY_PATH);
        if (sceIoRename(LIBRARY_PATH ".new", LIBRARY_PATH) < 0) sceIoRemove(LIBRARY_PATH ".new");
    } else {
        sceIoRemove(LIBRARY_PATH ".new");
    }
    p.active = false;
    // One line, when there were new songs.
    if (p.opened)
        logLine("library: %d songs, %d new; %d covers made, %d songs without one, %d not read, %d over %d covers "
                "(%d s)",
                p.files, p.opened, p.made, p.none, p.unread, p.over, kSavedMax,
                (int)((sceKernelGetProcessTimeWide() - p.start) / 1000000));
}

// A song file at g_passPath: its album's cover made if it has none.
void passFile(int kind, const SceIoStat& st) {
    Pass& p = g_pass;
    p.files++;
    const uint64_t t = when(st.st_mtime);
    const LibEntry le = {pathHash(g_passPath), (uint32_t)st.st_size, (uint32_t)(t ^ (t >> 32))};
    if (listedBefore(le)) {
        list(le);
        return;
    }
    p.opened++;
    Tag& tag = g_tag;
    const bool read = kind == FILE_MP3 ? readTag(g_passPath, tag, NULL) : readMp4Tag(g_passPath, tag);
    if (!read || !tag.title[0]) {  // no tags: nothing to find it by
        list(le);
        return;
    }
    if (!tag.picSize) {
        p.none++;
        list(le);
        return;
    }
    char name[96];
    savedNameOf(tag.title, tag.album, tag.artist, name, sizeof(name));
    if (savedFresh(name)) {  // another song of the album made it
        list(le);
        return;
    }
    if (p.saved >= kSavedMax) {
        p.over++;
        list(le);
        return;
    }
    SceUID pngUid;
    uint8_t* png;
    CoverInfo ci;
    int decodeMs, decodeCpuMs;
    const uint32_t pngLen = smallCover(tag, g_passPath, false, pngUid, png, ci, decodeMs, decodeCpuMs);
    if (!pngLen) {
        p.unread++;
        // Short of memory now: tried again next time. A picture that can't
        // be read stays unread.
        if (sceClibStrcmp(g_smallWhy, "memory")) list(le);
        return;
    }
    saveCoverAs(name, g_passPath, png, pngLen);
    ssm_sceKernelFreeMemBlock(pngUid);
    p.made++;
    p.saved++;
    list(le);
}

// One step: the next entry of the folder being read (folders three deep, as
// scanDir). False once the pass is done.
bool passStep() {
    Pass& p = g_pass;
    while (p.depth >= 0) {
        SceIoDirent e;
        if (sceIoDread(p.dirs[p.depth], &e) <= 0) {
            sceIoDclose(p.dirs[p.depth]);
            p.depth--;
            if (p.depth >= 0) g_passPath[p.lens[p.depth]] = 0;
            continue;
        }
        int len = 0;
        while (e.d_name[len]) len++;
        const int n = p.lens[p.depth];
        if (!len || e.d_name[0] == '.' || n + 1 + len >= (int)sizeof(g_passPath)) continue;
        g_passPath[n] = '/';
        sceClibMemcpy(g_passPath + n + 1, e.d_name, len + 1);
        if (SCE_S_ISDIR(e.d_stat.st_mode)) {
            if (p.depth < 3) {
                const SceUID d = sceIoDopen(g_passPath);
                if (d >= 0) {
                    p.depth++;
                    p.dirs[p.depth] = d;
                    p.lens[p.depth] = n + 1 + len;
                    return true;
                }
            }
            g_passPath[n] = 0;
            continue;
        }
        if (const int kind = songFile(e.d_name, len)) {
            passFile(kind, e.d_stat);
            g_passPath[n] = 0;
            return true;
        }
        g_passPath[n] = 0;
    }
    return false;
}

int coverThread(SceSize, void*) {
    findOpt();
    loadJacket();

    Want w, last;
    sceClibMemset(&last, 0, sizeof(last));
    int done = 0;
    for (;;) {
        // Asleep unless a pass is going: then a look, and on.
        if (g_pass.active) sceKernelPollSema(g_wakeCover, 1);
        else sceKernelWaitSema(g_wakeCover, 1, NULL);
        if (g_forgetCovers) {
            g_forgetCovers = false;
            forgetCovers();
        }
        if (g_libraryWanted && !g_pass.active) {
            g_libraryWanted = false;
            startPass();
        }
        // A song asked for (its answer free to send) comes first; else a step
        // of the pass.
        const int seq = g_reqSeq;
        const bool asked = g_mailSeq == g_mailTaken && !(seq & 1) && seq != done;
        if (!asked) {
            if (g_pass.active && !passStep()) endPass();
            continue;
        }
        __sync_synchronize();
        sceClibMemcpy(&w, &g_req, sizeof(w));
        __sync_synchronize();
        if (g_reqSeq != seq) continue;
        done = seq;
        CoverMail m = {COVER_SAME, -1, NULL, 0};
        const bool newAlbum = !w.album[0] || !sameText(w.album, last.album) || !sameText(w.artist, last.artist);
        if (newAlbum) m = findCover(w);
        // A song skipped while its cover was being looked for: the cover is
        // dropped, so it never shows, and the newer song's is looked for next
        // (its request has already woken this thread).
        if (g_reqSeq != seq) {
            if (m.uid >= 0) ssm_sceKernelFreeMemBlock(m.uid);
            continue;
        }
        if (newAlbum) last = w;
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
// (495 and 511), 16 pixels (measured on screenshots). 208 pixels high.
const float kPanelWidth = 370, kPanelHeight = 208;
const float kPanelScale = kPanelWidth / 504.0f;
float panelScale() { return kPanelScale; }
float g_panelLeft = 60, g_panelTop = 80;  // where the panel is, for the clock (see placePanel)
// A panel point in its group's units: the groups sit at the page's centre
// (screen 480, 272) and are scaled about it; y up.
float localX(float nx) { return (g_panelLeft - 480) / kPanelScale + nx; }
float localY(float ny) { return (272 - g_panelTop) / kPanelScale - ny; }

// A theme can move the clock (theme.xml's m_dateLayout): bottom left (the
// start screen's own, 0 and 3), top left (1) or bottom right (2). The panel
// stays on the left, lined up with the clock:
//   clock bottom left: the panel top left, above it, as above;
//   clock top left: the panel bottom left, below it, 16 pixels above the
//     frame's bottom, as the clock's digits sit;
//   clock bottom right: the panel bottom left, level with it;
//   clock top right (no layout puts it there): the panel top left, level.
// Read from where the clock's group is laid out (centred, y up): x above 0
// is the right, y above 0 the top. Measured: bottom left -430, -222; top
// left -430, 59; bottom right 54, -222.
const uint32_t kClockCorner = 0xDE1F6BE8;
struct PosAccess : paf::ui::Widget {
    const float* pos() const { return (const float*)&m_pos; }
};

void placePanel(paf::ui::Widget* page) {
    g_panelLeft = 60;
    g_panelTop = 80;
    paf::ui::Widget* clock = findChild(page, kClockCorner);
    if (!clock) return;
    const float* p = static_cast<const PosAccess*>(clock)->pos();
    const bool right = p[0] > 0, top = p[1] > 0;
    if (right != top) g_panelTop = 511 - 16 - kPanelHeight;  // the panel bottom left
}

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
paf::ui::Widget *g_groups[4], *g_titleLine, *g_artistLine, *g_playIcon, *g_pauseIcon, *g_cover;
paf::ui::SlideBar* g_bar;
uint32_t g_barVtable;  // checked before each write, as a second guard
int g_shownSeq = -1;   // the music state the panel shows
bool g_panelShown;     // the panel is shown and takes taps
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

extern paf::ui::Widget* g_glass[3];
extern paf::ui::Widget* g_timeLine;
extern paf::ui::Widget* g_dateText;
void showGlass(bool on);
void screenGone(int gen) {
    if (gen == g_screenGen) {
        g_screenAlive = false;
        g_bar = NULL;
        g_cover = NULL;
        g_glass[0] = g_glass[1] = g_glass[2] = NULL;
        g_timeLine = NULL;
        g_dateText = NULL;
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
    return s;
}

const int kShowNote = 0, kShowJacket = -1;  // g_coverShows, besides a cover's id
const int kCoverRetries = 10;  // UI ticks a picture is tried again when the pool is full
int g_coverRetries;

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
// no-cover jacket, else nothing (kShowNote).
int coverWant() {
    if (g_uiCover.data && !g_uiCover.failed) return g_uiCover.id;
    if (g_jacketReady && !g_jacketFailed) return kShowJacket;
    return kShowNote;
}

// The square empty: the quick menu's note texture on it, so a picture's room
// in the pool is let go, but drawn at no size. The note itself never shows.
void showNote() {
    if (g_noteTex) giveTexture(g_cover, g_noteTex);
    setScale(g_cover, 0);
    sceClibMemcpy(g_coverRgb, g_tint, sizeof(g_coverRgb));
    g_coverShows = kShowNote;
}

// Puts what is wanted on the square; a picture that won't load falls to the
// next. The jacket is loaded each time it is wanted and goes with the start
// screen, as covers do: the texture pool has little room. A picture on the
// square keeps its room in the pool (64 KB: about one cover) until it is let
// go, and PAF lets it go a moment later: so the square is emptied first and
// the next picture comes on a later tick (on 3.65, loading it at once failed
// whenever a picture was up). A picture that still won't load is tried again
// for a few ticks before the square falls to the next choice.
void swapCover() {
    int want = coverWant();
    if (want != kShowNote && g_coverShows != kShowNote && g_noteTex) {
        showNote();
        return;
    }
    if (want > 0) {
        bool pinned = false;
        paf::graph::Surface* s = loadImage("cover", g_uiCover.data, g_uiCover.size, pinned);
        g_uiCover.pinned = pinned;
        if (s) {
            showPicture(s);
            g_coverRgb[0] = g_coverRgb[1] = g_coverRgb[2] = 1;
            g_coverShows = want;
            g_coverRetries = 0;
            return;
        }
        if (++g_coverRetries < kCoverRetries) return;
        g_coverRetries = 0;
        logLine("cover: not loaded (no room in either texture pool)");
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
            g_coverRetries = 0;
            return;
        }
        if (++g_coverRetries < kCoverRetries) return;
        g_coverRetries = 0;
        g_jacketFailed = true;
        logLine("jacket: not loaded: the note");
    }
    showNote();
}

// The cover square, every UI tick while the panel is up.
void applyCover() {
    if (!g_screenAlive || !g_cover || !g_panelShown || coverWant() == g_coverShows) return;
    swapCover();
    setV4(PAF_SET_COLOR, 0x10001, g_cover, g_coverRgb[0], g_coverRgb[1], g_coverRgb[2], 1);
}

// The volume bar: 380 units from panel x 59, at panel y 200.
const float kBarWidth = 380 * kPanelScale;
float barLeft() { return g_panelLeft + 59 * kPanelScale; }
float barY() { return g_panelTop + 200 * kPanelScale; }
int g_volShown = -1;
bool g_dragging, g_touching;
// A tap on a button the plugin saw itself: for when something of the start
// screen's lies over the panel (a notification that came in standby takes
// the buttons' touches; the bar's are read here already). Fired a moment
// after the finger lifts, unless the button fired first.
int g_tapButton = -1;      // the button the finger landed on
int g_tapX, g_tapY;        // where
int g_tapWait;             // ticks until the tap fires itself
// One press a tap, whichever comes first: the button's own, or the tap
// firing itself (when the UI is busy, after a skip, the button's own can
// come after it: two presses would pause and play again, or skip twice).
int g_tapSeq, g_tapPressedSeq = -1;  // taps landed; the last one a press was made for
SceUInt64 g_tapPressedAt;
const float kButtonX[3] = {112, 249, 386}, kButtonY = 262;  // panel units
void press(uint32_t cmd);
void pressForTap(uint32_t cmd);

struct PeekBar : paf::ui::SlideBar {
    static void set(paf::ui::SlideBar* w, float v) {
        PeekBar* p = static_cast<PeekBar*>(w);
        p->m_value = v;
        p->m_unk_0x294 = (int)((float)p->m_fraction * v);
        ((UpdateSliderFn)g_paf[PAF_SLIDER_UPDATE].fn)(w, false);
    }
};

// While a finger drags the bar its knob follows the finger smoothly; let go,
// it settles onto its step, as the quick menu's does.
float g_knob;   // where the knob is drawn (0 to 1) while dragged or settling
int g_settle;   // ticks of settling left
void drawKnob(float f) {
    if (*(const uint32_t*)g_bar != g_barVtable) return;
    g_knob = f;
    PeekBar::set(g_bar, f);
}

void showVolume(int vol) {
    if (!g_screenAlive || !g_bar || *(const uint32_t*)g_bar != g_barVtable || vol == g_volShown) return;
    g_volShown = vol;
    PeekBar::set(g_bar, vol / 30.0f);
}

// Every UI tick while the panel is up: a finger that lands on the bar's
// row drags it; a tap on a button is noted.
void driveBar() {
    if (!g_screenAlive || !g_panelShown || !g_bar) {
        g_dragging = false;
        g_tapButton = -1;
        g_tapWait = 0;
        return;
    }
    if (g_settle && !g_dragging) {  // half the way to the step a tick, then on it
        const float step = g_volShown / 30.0f;
        drawKnob(--g_settle ? step + (g_knob - step) * 0.5f : step);
    }
    if (g_tapWait && !--g_tapWait && g_tapPressedSeq != g_tapSeq && g_tapButton >= 0) {
        const uint32_t cmds[3] = {CMD_PREVIOUS, CMD_PLAY, CMD_NEXT};
        pressForTap(cmds[g_tapButton]);
        g_tapButton = -1;
    }
    SceTouchData t;
    sceClibMemset(&t, 0, sizeof(t));
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &t, 1) < 0 || t.reportNum == 0) {
        if (g_touching && g_tapButton >= 0 && !g_tapWait) g_tapWait = 9;  // lifted: 0.15 s for the button's own
        if (g_dragging) g_settle = 6;  // about 0.1 s
        g_dragging = g_touching = false;
        return;
    }
    // Like the quick menu's bar: only a finger that lands on it drags it, so
    // a finger passing over it (pulling the peel) changes nothing.
    const bool landed = !g_touching;
    g_touching = true;
    const int x = t.report[0].x / 2, y = t.report[0].y / 2;  // the panel is 1920 x 1088
    if (landed) {
        g_tapButton = -1;
        g_tapWait = 0;
        g_tapSeq++;
        for (int i = 0; i < 3; i++) {
            const float bx = g_panelLeft + kButtonX[i] * kPanelScale, by = g_panelTop + kButtonY * kPanelScale;
            if (x > bx - 40 && x < bx + 40 && y > by - 20 && y < by + 20) g_tapButton = i;
        }
        g_tapX = x;
        g_tapY = y;
    } else if (g_tapButton >= 0 && (x - g_tapX) * (x - g_tapX) + (y - g_tapY) * (y - g_tapY) > 20 * 20) {
        g_tapButton = -1;  // a drag (the peel), not a tap
    }
    if (!g_dragging) {
        if (!landed || x < barLeft() - 30 || x > barLeft() + kBarWidth + 30 || y < barY() - 30 || y > barY() + 30)
            return;
        g_dragging = true;
    }
    float f = (x - barLeft()) / kBarWidth;
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    const int vol = (int)(f * 30 + 0.5f);
    if (vol != g_volShown) {
        sceAVConfigSetSystemVol(vol);
        // The volume the system took: with AVLS on, it stops at its limit,
        // and so does the knob, as on the quick menu's bar.
        int real;
        g_volShown = sceAVConfigGetSystemVol(&real) >= 0 ? real : vol;
    }
    if (g_volShown < vol && f > g_volShown / 30.0f) f = g_volShown / 30.0f;
    g_settle = 0;
    drawKnob(f);
}

// The buttons.
char g_listenerMem[3][sizeof(Listener)] __attribute__((aligned(8)));
Listener* g_listeners[3];

volatile bool g_pollSoon;  // a press: the music thread looks again at once

// Play and pause show at once, as the quick menu's do, not when the music
// service next says so; for a moment, until it does, what it says before is
// not shown.
SceUInt64 g_heldUntil;
bool g_heldPlaying;

void showPlaying(bool playing) {
    g_playState = playing ? 1 : 0;
    if (g_playIcon) setSize(g_playIcon, playing ? 0 : 64, playing ? 0 : 64);
    if (g_pauseIcon) setSize(g_pauseIcon, playing ? 64 : 0, playing ? 64 : 0);
}

// Play or pause shown at once that the music service never took up: what it
// says shown again, once the moment is over.
void releaseHold() {
    if (!g_heldUntil || sceKernelGetProcessTimeWide() <= g_heldUntil) return;
    g_heldUntil = 0;
    if (g_screenAlive && g_panelShown) showPlaying(g_now[g_nowIdx].state == 1);
}

void press(uint32_t cmd) {
    if (cmd == CMD_PLAY && g_playState == 1) cmd = CMD_STOP;  // one button: pause while playing
    if (cmd == CMD_PLAY || cmd == CMD_STOP) {
        g_heldPlaying = cmd == CMD_PLAY;
        g_heldUntil = sceKernelGetProcessTimeWide() + 1500000;
        showPlaying(g_heldPlaying);
    }
    musicCommand(cmd);
    g_fastPolls = 12;  // the new song shows as soon as the service has it
    g_pollSoon = true;
    wake(g_wakeMusic);
}

void pressForTap(uint32_t cmd) {
    g_tapPressedSeq = g_tapSeq;
    g_tapPressedAt = sceKernelGetProcessTimeWide();
    press(cmd);
}

void onPress(int32_t, paf::ui::Handler*, paf::ui::Event*, void* data) {
    // This tap's press made already (the button's own came late): let go. A
    // press more than 1.5 s on is a tap whose landing the plugin missed.
    if (g_tapPressedSeq == g_tapSeq && sceKernelGetProcessTimeWide() - g_tapPressedAt < 1500000) return;
    pressForTap((uint32_t)(uintptr_t)data);
}

// ── The song's time ─────────────────────────────────────────────────────────
// As the Music app shows it: played / length, on a line under the title and
// artist. In the panel's colour (the theme's clock colour), not the Music
// app's green: the played time bright, the length dimmer, as the artist line
// is. The line is the artist line of a second Music row of the quick
// menu's, the rest of that row hidden: Sony's own text.
paf::ui::Widget* g_timeLine;
const float kTimeY = 146;  // panel units, like the title (74) and artist (110)

void showTime(const Now& n) {
    if (!g_timeLine) return;
    char text[32] = "";
    int played = 0;
    if (n.length > 0) {
        played = sceClibSnprintf(text, sizeof(text), "%02d:%02d", n.played / 60, n.played % 60);
        sceClibSnprintf(text + played, sizeof(text) - played, " / %02d:%02d", n.length / 60, n.length % 60);
    }
    setLine(g_timeLine, 2, text);
    if (g_pafOpt[OPT_TEXT_STYLE].fn && played) {
        static const float dim[4] __attribute__((aligned(16))) = {1, 1, 1, 0.6f};
        static const float bright[4] __attribute__((aligned(16))) = {1, 1, 1, 1};
        const TextStyleFn style = (TextStyleFn)g_pafOpt[OPT_TEXT_STYLE].fn;
        style(g_timeLine, 2 /* colour */, 0, sceClibStrnlen(text, sizeof(text)), dim);
        style(g_timeLine, 2, 0, played, bright);
    }
}

// ── The theme's clock colour ────────────────────────────────────────────────
// The panel takes the colour a theme gives the clock (white without one).
// Where it is: the date's colour, or its text's (a theme sets the text's;
// the date's own reads white, and the text's can come after the panel is
// made, so it is read again when the panel shows). On the Vita a widget
// colour's alpha is its top byte, whatever the headers' getters say (Vita
// Shell Companion: the quick menu's lines read D8FFFFFF and CCFFFFFF); the
// colour below it is taken as blue, green, red, the GPU's byte order.
paf::ui::Widget* g_dateText;  // valid while g_screenAlive
paf::ui::Widget *g_buttons[3], *g_icons[4];

// The colour read into g_tint; true if it changed.
bool readTint() {
    float t[3] = {1, 1, 1};
    if (g_dateText) {
        const uint32_t c = g_dateText->GetColor32();
        float v[4] __attribute__((aligned(16))) = {1, 1, 1, 0};
        if (c >> 24 && (c & 0xFFFFFF) != 0xFFFFFF) {
            t[0] = (c & 0xFF) / 255.0f;
            t[1] = (c >> 8 & 0xFF) / 255.0f;
            t[2] = (c >> 16 & 0xFF) / 255.0f;
        } else if (g_pafOpt[OPT_TEXT_STYLE_GET].fn &&
                   ((TextStyleGetFn)g_pafOpt[OPT_TEXT_STYLE_GET].fn)(g_dateText, 2 /* colour */, 0, v) >= 0 && v[3] > 0) {
            t[0] = v[0];
            t[1] = v[1];
            t[2] = v[2];
        }
    }
    if (!sceClibMemcmp(t, g_tint, sizeof(t))) return false;
    sceClibMemcpy(g_tint, t, sizeof(t));
    logLine("theme: clock colour %d %d %d", (int)(t[0] * 255), (int)(t[1] * 255), (int)(t[2] * 255));
    return true;
}

// The colour onto the panel: buttons, symbols, the bar and its knob always;
// the lines only for a theme's colour (else they keep the quick menu's own
// alphas, title 85%, artist 80%).
void applyTint() {
    for (int i = 0; i < 3; i++)
        if (g_buttons[i]) setTint(g_buttons[i]);
    for (int i = 0; i < 4; i++)
        if (g_icons[i]) setTint(g_icons[i]);
    if (g_bar) {
        setTint(g_bar);
        if (g_bar->GetChildrenNum() > 0) setTint(*g_bar->GetChildrenBegin());  // the knob
    }
    if (tinted()) {
        if (g_titleLine) setTint(g_titleLine);
        if (g_artistLine) setTint(g_artistLine);
        if (g_timeLine) setTint(g_timeLine);
    }
    if (g_cover && g_coverShows == kShowJacket) {  // the no-cover jacket is in the theme's colour too
        sceClibMemcpy(g_coverRgb, g_tint, sizeof(g_coverRgb));
        setV4(PAF_SET_COLOR, 0x10001, g_cover, g_coverRgb[0], g_coverRgb[1], g_coverRgb[2], 1);
    }
}

// The music state onto the panel (UI thread); a song without a title or
// artist shows Sony's "Unknown", in the system language. A new song's title
// goes on at once, as the quick menu's does; its cover follows when ready.
void applyMusic() {
    if (!g_screenAlive || !g_groups[0] || g_shownSeq == g_nowSeq) return;
    g_shownSeq = g_nowSeq;
    const Now& n = g_now[g_nowIdx];
    if (n.shown != g_panelShown) {
        if (n.shown && readTint()) applyTint();
        for (int i = 0; i < 4; i++)
            if (g_groups[i]) show(g_groups[i], n.shown);
        showGlass(n.shown);
        g_panelShown = n.shown;
    }
    if (!n.shown) return;
    if (g_titleLine) {
        if (n.title[0]) setLine(g_titleLine, 0, n.title);
        else setLineW(g_titleLine, 0, sonyString(kUnknown));
    }
    if (g_artistLine) {
        if (n.artist[0]) setLine(g_artistLine, 1, n.artist);
        else setLineW(g_artistLine, 1, sonyString(kUnknown));
    }
    showTime(n);
    bool playing = n.state == 1;
    if (g_heldUntil) {
        if (playing == g_heldPlaying || sceKernelGetProcessTimeWide() > g_heldUntil) g_heldUntil = 0;
        else playing = g_heldPlaying;
    }
    showPlaying(playing);
}

// ── The glass ───────────────────────────────────────────────────────────────
// Behind the panel, so it reads on any wallpaper: Sony's own 3x3 textures
// from power_manage_plugin, stretched as Sony stretches them. The edge: the
// frame Sony draws round the start screen's notifications, its light edge at
// 24% white. The dark: Sony's filled box made for that frame, 2 pixels
// inside it, its corners following the frame's, in black at 30%. PAF draws
// only that box's top corners right (it is 44 texels in a texture 48 wide;
// its bottom ones come out square), so the dark is two halves of it, the
// lower one turned round: four top corners. (A picture of the whole dark,
// made here, doesn't fit in SceShell's texture pools.) Their planes are
// quick menu dividers (impose_plugin), from rows opened before the panel:
// over the start screen's picture, under the panel's buttons, which keep
// their touches.
const uint32_t kPlateTemplate = 0x1FF37238;  // a quick menu row (hidden) and its divider
const uint32_t kFillTex = 0x3982F37E, kFrameTex = 0xE1E2F764;
// Round the panel with 9 pixels to spare at the sides, 10 at the top and
// bottom.
const float kGlassSide = 9, kGlassEnd = 10;
const float kFillInset = 2;
const float kGlassDark = 0.50f;  // the dark's alpha: half, so the lines read on bright wallpapers too
paf::graph::Surface *g_fillTex, *g_frameTex;  // held for good
paf::ui::Widget* g_glass[3];                  // the dark's halves and the frame (valid while g_screenAlive)

struct PlaneAccess : paf::ui::Plane {
    paf::graph::PlaneObj* obj() const { return m_planeobj; }
};

// One of plugin's textures, its reference kept for good.
paf::graph::Surface* pluginTexture(paf::Plugin* plugin, uint32_t hash) {
    if (!plugin || !g_pafOpt[OPT_GET_TEXTURE].fn) return NULL;
    HashID id;
    sceClibMemset(&id, 0, sizeof(id));
    id.hash = hash;
    TexRef ref(NULL);
    ((GetTextureFn)g_pafOpt[OPT_GET_TEXTURE].fn)(&ref.ptr, plugin, *(const paf::IDParam*)&id);
    return ref.ptr.get();
}

// The plate template opened into parent: its divider plane (the row hidden).
paf::ui::Widget* openPlate(paf::Plugin* impose, paf::ui::Widget* parent) {
    int before = 0;
    for (paf::list<paf::ui::Widget*>::iterator it = parent->GetChildrenBegin(); it != parent->GetChildrenEnd(); ++it)
        before++;
    HashID id;
    sceClibMemset(&id, 0, sizeof(id));
    id.hash = kPlateTemplate;
    const void* param[2] = {NULL, NULL};
    const int rc = ((TemplateOpenFn)g_paf[PAF_OPEN].fn)(impose, parent, *(const paf::IDParam*)&id, param);
    if (rc < 0) {
        logLine("glass: template open 0x%08X", (unsigned)rc);
        return NULL;
    }
    paf::ui::Widget* added[2] = {NULL, NULL};
    int count = 0;
    for (paf::list<paf::ui::Widget*>::iterator it = parent->GetChildrenBegin(); it != parent->GetChildrenEnd(); ++it) {
        if (count >= before && count < before + 2) added[count - before] = *it;
        count++;
    }
    if (count != before + 2 || !added[0] || !added[1]) {
        logLine("glass: %d parts added", count - before);
        return NULL;
    }
    setScale(added[0], 0);  // the row
    setSize(added[1], 0, 0);  // nothing until dressed
    return added[1];
}

// A divider made a pane of the glass: tex stretched over a rectangle of the
// screen (its size the plane's, not the texture's), turned round if asked,
// in colour.
void dressGlass(int i, paf::graph::Surface* tex, float left, float top, float right, float bottom, bool turned,
                float r, float g, float b, float a) {
    paf::ui::Widget* w = g_glass[i];
    if (!w) return;
    if (!tex) {
        setSize(w, 0, 0);
        return;
    }
    const uint32_t obj = (uint32_t)((const PlaneAccess*)w)->obj();
    const bool objOk = obj >= 0x80000000u && obj < 0xA0000000u && !(obj & 3);
    if (objOk && g_pafOpt[OPT_SCALE_MODE].fn)
        ((ScaleModeFn)g_pafOpt[OPT_SCALE_MODE].fn)((paf::graph::PlaneObj*)obj, 0, 0);  // SCALE_SIZE both ways
    giveTexture(w, tex);
    setSize(w, right - left, bottom - top);
    // The clock's group is the screen, centred, y up.
    setPos(w, (left + right) / 2 - 480, 272 - (top + bottom) / 2);
    if (turned && g_pafOpt[OPT_SET_ROT].fn) {
        const float rot[4] __attribute__((aligned(16))) = {0, 0, 3.14159265f, 0};  // half a turn, in place
        ((SetRotFn)g_pafOpt[OPT_SET_ROT].fn)(w, rot, 0, NULL, 0x10006 /* ANIM_ROT */, 0, 0.0f, NULL, NULL);
    }
    setV4(PAF_SET_COLOR, 0x10001, w, r, g, b, a);
}

void showGlass(bool on) {
    for (int i = 0; i < 3; i++)
        if (g_glass[i]) setScale(g_glass[i], on ? 1.0f : 0.0f);
}

// The quick menu's Music row opened at the end of parent: its heading and
// bar group and its lines and buttons group (its divider hidden), placed and
// sized as the panel. false if it didn't open.
bool openRow(paf::Plugin* impose, paf::ui::Widget* parent, paf::ui::Widget** heading, paf::ui::Widget** controls) {
    HashID id;
    sceClibMemset(&id, 0, sizeof(id));
    id.hash = kRowTemplate;
    const void* param[2] = {NULL, NULL};  // TemplateOpenParam: no event table, no sibling
    const int rc = ((TemplateOpenFn)g_paf[PAF_OPEN].fn)(impose, parent, *(const paf::IDParam*)&id, param);
    if (rc < 0) {
        logLine("panel: template open 0x%08X", (unsigned)rc);
        return false;
    }
    // The template added three children at the end: the heading and bar
    // group, the lines and buttons group, the quick menu's divider.
    paf::ui::Widget* added[3] = {NULL, NULL, NULL};
    int count = 0;
    for (paf::list<paf::ui::Widget*>::iterator it = parent->GetChildrenBegin(); it != parent->GetChildrenEnd(); ++it)
        added[count++ % 3] = *it;
    if (count < 4) return false;
    *heading = added[count % 3];
    *controls = added[(count + 1) % 3];
    setSize(added[(count + 2) % 3], 0, 0);  // the divider
    for (int i = 0; i < 2; i++) {
        paf::ui::Widget* g = i ? *controls : *heading;
        setPos(g, 0, 0);
        setScale(g, kPanelScale);
    }
    return true;
}

// Every child of group but keep, hidden.
void keepOnly(paf::ui::Widget* group, paf::ui::Widget* keep) {
    for (paf::list<paf::ui::Widget*>::iterator it = group->GetChildrenBegin(); it != group->GetChildrenEnd(); ++it)
        if (*it != keep) setScale(*it, 0);
}

// The song's time: from a second Music row, opened before the panel's (see
// addPanel).
void addTimeLine(paf::Plugin* impose, paf::ui::Widget* parent) {
    g_groups[2] = g_groups[3] = g_timeLine = NULL;
    paf::ui::Widget *heading, *controls;
    if (!openRow(impose, parent, &heading, &controls)) return;
    g_groups[2] = heading;
    g_groups[3] = controls;
    show(heading, false);
    show(controls, false);
    g_timeLine = findChild(controls, kArtist);
    keepOnly(heading, NULL);
    keepOnly(controls, g_timeLine);
    if (!g_timeLine) {
        logLine("time line not found");
        return;
    }
    setSize(g_timeLine, 334, 40);
    setPos(g_timeLine, localX(170) + 364, localY(kTimeY) + 20);
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
    paf::Plugin* power = ((PluginFindFn)g_paf[PAF_FIND].fn)("power_manage_plugin");
    if (!g_pool) {
        // PAF's Plugin keeps its texture pool at 0xD4 (Vita Shell Companion).
        if (power) g_pool = *(paf::graph::SurfacePool* const*)((const char*)power + 0xD4);
    }
    if (power && !g_fillTex) g_fillTex = pluginTexture(power, kFillTex);
    if (power && !g_frameTex) g_frameTex = pluginTexture(power, kFrameTex);
    placePanel(page);
    // Into the clock's group (full screen, centred like the page), so it
    // fades with the date and time on a peel. The glass and the song's time
    // first, so the panel draws over them and its buttons take the touches
    // (the time's row over the buttons kept them from them). Its parts are
    // found in its own groups, not the time row's.
    paf::ui::Widget* parent = findChild(page, kClockGroup);
    if (!parent) parent = page;
    for (int i = 0; i < 3; i++) g_glass[i] = i == 0 || g_glass[i - 1] ? openPlate(impose, parent) : NULL;
    addTimeLine(impose, parent);
    paf::ui::Widget *heading, *controls;
    if (!openRow(impose, parent, &heading, &controls)) return;
    auto part = [heading, controls](uint32_t id) {
        paf::ui::Widget* w = findChild(heading, id);
        return w ? w : findChild(controls, id);
    };
    const float left = g_panelLeft - kGlassSide, top = g_panelTop - kGlassEnd,
                right = g_panelLeft + kPanelWidth + kGlassSide, bottom = g_panelTop + kPanelHeight + kGlassEnd,
                middle = (top + bottom) / 2;
    dressGlass(0, g_fillTex, left + kFillInset, top + kFillInset, right - kFillInset, middle, false, 0, 0, 0,
               kGlassDark);
    dressGlass(1, g_fillTex, left + kFillInset, middle, right - kFillInset, bottom - kFillInset, true, 0, 0, 0,
               kGlassDark);
    dressGlass(2, g_frameTex, left, top, right, bottom, false, 1, 1, 1, 0.24f);
    if (!g_fillTex || !g_frameTex) logLine("glass: textures %s %s", g_fillTex ? "fill" : "-", g_frameTex ? "frame" : "-");

    // Layout, in quick-menu units from the panel's top left: the cover 150
    // square at the top left (the row's note plane, centred at 75, 75);
    // title, artist and the song's time from x 170, the time's digits level
    // with the cover's bottom (y 74 / 110 / 146, the time from addTimeLine);
    // bar centre (249, 200);
    // buttons at (112 / 249 / 386, 262). Left-anchored
    // parts count from the group's left edge (-364); text takes its left edge
    // from the position; the title's and artist's vertical alignment shifts
    // them 20. No "Music" heading: the cover says what it is.
    struct Place {
        uint32_t id;
        float nx, ny;
        bool left;
        float shift;
    };
    const Place places[] = {
        {kNoteIcon, 75, 75, true, 0},
        {kTitle, 170, 74, true, -20},   {kArtist, 170, 110, true, 20},
        {kNoMusic, 170, 74, true, -20}, {kBar, 249, 200, false, 0},
        {kPrev, 112, 262, false, 0},    {kPrevIcon, 112, 262, false, 0},
        {kPlay, 249, 262, false, 0},    {kPlayIcon, 249, 262, false, 0},
        {kPauseIcon, 249, 262, false, 0},
        {kNext, 386, 262, false, 0},    {kNextIcon, 386, 262, false, 0},
    };
    for (unsigned i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
        paf::ui::Widget* w = part(places[i].id);
        if (w) setPos(w, localX(places[i].nx) + (places[i].left ? 364 : 0), localY(places[i].ny) + places[i].shift);
    }
    paf::ui::Widget* w;
    if ((w = part(kAppIcon))) setSize(w, 0, 0);  // a blank square in the quick menu's row
    if ((w = part(kNoMusic))) setSize(w, 0, 0);  // the panel only shows with a song
    if ((w = part(kHeading))) setScale(w, 0);    // no "Music" heading
    // The theme's clock colour (see readTint).
    g_dateText = findChild(page, kDateText);
    readTint();
    // The cover square: the note plane (applyCover gives it its colour),
    // empty until its picture comes.
    g_cover = part(kNoteIcon);
    g_coverShows = 0;
    if (g_cover) {
        setTint(g_cover);
        if (!g_noteTex) g_noteTex = holdTexture(g_cover);
        setScale(g_cover, 0);
    }

    g_titleLine = part(kTitle);
    g_artistLine = part(kArtist);
    // To the panel's right edge; in the quick menu's own colours (title 85%,
    // artist 80% white, impose_plugin's styles).
    if (g_titleLine) setSize(g_titleLine, 334, 40);
    if (g_artistLine) setSize(g_artistLine, 334, 40);
    g_playIcon = part(kPlayIcon);
    g_pauseIcon = part(kPauseIcon);

    // The active look, always: buttons switched on, in their normal state
    // (Enable alone leaves them dimmed, as Vita Shell Companion found) and
    // white; symbols and bar white.
    const uint32_t buttons[3] = {kPrev, kPlay, kNext};
    const uint32_t cmds[3] = {CMD_PREVIOUS, CMD_PLAY, CMD_NEXT};
    for (int i = 0; i < 3; i++) {
        paf::ui::Widget* b = g_buttons[i] = part(buttons[i]);
        if (!b) continue;
        if (!g_listeners[i]) g_listeners[i] = new (g_listenerMem[i]) Listener(onPress, (void*)(uintptr_t)cmds[i]);
        ((EnableFn)g_paf[PAF_BTN_ENABLE].fn)((paf::ui::ButtonBase*)b, true);
        ((paf::ui::ButtonBase*)b)->ChangeButtonState(paf::ui::ButtonBase::ST_BTN_NORMAL);
        ((AddListenerFn)g_paf[PAF_ADD_LISTENER].fn)(b, 0x10000008 /* CB_BTN_DECIDE */, g_listeners[i], true);
    }
    const uint32_t icons[4] = {kPrevIcon, kPlayIcon, kPauseIcon, kNextIcon};
    for (int i = 0; i < 4; i++) g_icons[i] = part(icons[i]);
    g_bar = (paf::ui::SlideBar*)part(kBar);
    applyTint();
    g_barVtable = g_bar ? *(const uint32_t*)g_bar : 0;
    g_volShown = -1;

    // Hidden until the music thread says a song is loaded.
    show(heading, false);
    show(controls, false);
    showGlass(false);
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
    releaseHold();
    takeCover();
    applyCover();
    if (!g_screenAlive || t < g_nextSlow) return;
    g_nextSlow = t + 100000;
    int vol;  // the knob follows the volume buttons
    if (g_panelShown && !g_dragging && sceAVConfigGetSystemVol(&vol) >= 0) showVolume(vol);
}

// ── The Music LiveArea ──────────────────────────────────────────────────────
// SceShell opens an app's LiveArea by loading the PAF plugin
// "livearea_template_<title ID>" and closes it by unloading it (seen on 3.65;
// the Music app is NPXS10009). Its calls to PAF's Plugin::LoadAsync,
// LoadSync and UnloadAsync are watched for that name and passed on unchanged.
const char kMusicLiveArea[] = "livearea_template_NPXS10009";

struct LiveAreaHook {
    uint32_t nid;
    tai_hook_ref_t ref;
};
LiveAreaHook g_laHooks[4] = {
    {0xF702E40A, 0},  // static Plugin::LoadAsync(InitParam const&, ...)
    {0xBE479650, 0},  // static Plugin::LoadSync(InitParam const&, ...)
    {0x664A8AB3, 0},  // static Plugin::UnloadAsync(const char* name, ...)
    {0x1ADB3A22, 0},  // static Plugin::UnloadAsync(Plugin*, ...)
};

bool inUserMemory(uint32_t v) { return v >= 0x80000000u && v < 0xA0000000u; }

void noteLiveArea(int i, uint32_t a0) {
    if (i == 2 ? !inUserMemory(a0) : !looksLikePointer(a0)) return;
    const char* name = i <= 1 ? ((const paf::Plugin::InitParam*)a0)->name.c_str()
                       : i == 2 ? (const char*)a0
                                : ((const paf::Plugin*)a0)->GetName();
    if (!inUserMemory((uint32_t)name) || sceClibStrncmp(name, kMusicLiveArea, sizeof(kMusicLiveArea))) return;
    const bool open = i <= 1;
    if (open == g_musicLiveArea) return;
    g_musicLiveArea = open;
    g_pollSoon = true;  // the panel follows at once
    wake(g_wakeMusic);
    if (open) {  // the Music app looks for new songs now: their covers are made too
        g_libraryWanted = true;
        wake(g_wakeCover);
    }
}

// Six words passed on: enough for each of these (extra ones are read from the
// caller's stack and ignored).
#define LIVEAREA_HOOK(i)                                                                                         \
    uint32_t liveAreaHook##i(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5) {     \
        typedef uint32_t (*F)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);                       \
        noteLiveArea(i, a0);                                                                                     \
        const _tai_hook_user* cur = (const _tai_hook_user*)g_laHooks[i].ref;                                     \
        const _tai_hook_user* next = (const _tai_hook_user*)cur->next;                                           \
        return next ? ((F)next->func)(a0, a1, a2, a3, a4, a5) : ((F)cur->old)(a0, a1, a2, a3, a4, a5);           \
    }
LIVEAREA_HOOK(0)
LIVEAREA_HOOK(1)
LIVEAREA_HOOK(2)
LIVEAREA_HOOK(3)

// Once ScePaf has loaded: SceShell's imports from it resolve only then.
void watchLiveAreas() {
    const void* fns[4] = {(const void*)liveAreaHook0, (const void*)liveAreaHook1, (const void*)liveAreaHook2,
                          (const void*)liveAreaHook3};
    unsigned hooked = 0;
    for (int i = 0; i < 4; i++)
        if (taiHookFunctionImport(&g_laHooks[i].ref, "SceShell", 0x4D9A9DD0, g_laHooks[i].nid, fns[i]) >= 0)
            hooked |= 1u << i;
    g_liveAreaWatched = (hooked & 3) && (hooked & 12);  // something to see it open and close
    if (!g_liveAreaWatched)
        logLine("Music LiveArea not watched (0x%X): the panel shows whenever there is a song", hooked);
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
    watchLiveAreas();
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

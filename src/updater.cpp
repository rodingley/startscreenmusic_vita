/*
 * Updates, quietly.
 *
 * Once a boot, two minutes after SceShell's UI starts and only with Wi-Fi
 * connected, the plugin asks GitHub for the newest release of UPDATE_REPO.
 * If its tag is newer than this version, it downloads the plugin from the
 * repository at that tag (release/startscreenmusic.suprx), checks it is a
 * plugin, and puts it where this one was loaded from. The new version loads
 * on the next restart. Nothing is shown; every failure leaves things as they
 * are.
 *
 * GitHub from a Vita: its root certificates are too old, so current ones are
 * loaded (update_certs.h), and it reaches api.github.com but not
 * *.githubusercontent.com, so the file comes through the contents API as raw
 * data.
 *
 * SceShell's own network is used as it is: its SSL and HTTP libraries are
 * started here only if SceShell hasn't started them, and then stopped again.
 * No network function is imported (a library may not be loaded when the
 * plugin starts): they are looked up when the check runs.
 *
 * GUARD_PATH holds this version, how many of its checks never finished, and
 * the last tag installed. A check that never finishes (SceShell stopping
 * during it) is counted; after three, this version stops checking, so an
 * update can never make the Vita restart over and over. A tag already
 * installed isn't installed again.
 */
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/libssl.h>
#include <psp2/net/http.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <taihen.h>

#include <stdarg.h>
#include <stdint.h>

#include "update_certs.h"
#include "updater.h"

void logLine(const char* fmt, ...);  // main.cpp

namespace {

#define UPDATE_REPO "rodingley/startscreenmusic_vita"
#define RELEASE_URL "https://api.github.com/repos/" UPDATE_REPO "/releases/latest"
#define FILE_URL_FMT "https://api.github.com/repos/" UPDATE_REPO "/contents/release/startscreenmusic.suprx?ref=%s"
#define USER_AGENT "StartScreenMusic/" SSM_VERSION
#define GUARD_PATH "ur0:tai/startscreenmusic_update.txt"

const unsigned kDelaySeconds = 120;
const int kWifiTries = 60;  // every 30 seconds: 30 minutes
const uint32_t kSslPool = 300 * 1024, kHttpPool = 80 * 1024;  // as VitaShell's
const uint32_t kMaxHeader = 16 * 1024;
const uint32_t kMinPlugin = 8 * 1024, kMaxPlugin = 1024 * 1024;

struct FreeMemory {
    int size, sizeUser, sizeCdram, sizePhycont;
};
extern "C" int ssm_sceKernelGetFreeMemorySize(FreeMemory* info);  // sysmem_stub.S

// ── The network libraries, looked up when needed ───────────────────────────
struct NetFn {
    const char* module;
    uint32_t lib, nid;
    uintptr_t fn;
};
NetFn g_net[] = {
    {"SceLibHttp", 0xE8F15CDE, 0x214926D9, 0},    // sceHttpInit
    {"SceLibHttp", 0xE8F15CDE, 0xC9076666, 0},    // sceHttpTerm
    {"SceLibHttp", 0xE8F15CDE, 0x62241DAB, 0},    // sceHttpCreateTemplate
    {"SceLibHttp", 0xE8F15CDE, 0xEC85ECFB, 0},    // sceHttpDeleteTemplate
    {"SceLibHttp", 0xE8F15CDE, 0xC616C200, 0},    // sceHttpCreateConnectionWithURL
    {"SceLibHttp", 0xE8F15CDE, 0xF0F65C15, 0},    // sceHttpDeleteConnection
    {"SceLibHttp", 0xE8F15CDE, 0xBD5DA1D0, 0},    // sceHttpCreateRequestWithURL
    {"SceLibHttp", 0xE8F15CDE, 0x3D3D29AD, 0},    // sceHttpDeleteRequest
    {"SceLibHttp", 0xE8F15CDE, 0x7B51B122, 0},    // sceHttpAddRequestHeader
    {"SceLibHttp", 0xE8F15CDE, 0x9CA58B99, 0},    // sceHttpSendRequest
    {"SceLibHttp", 0xE8F15CDE, 0x27071691, 0},    // sceHttpGetStatusCode
    {"SceLibHttp", 0xE8F15CDE, 0x7EDE3979, 0},    // sceHttpReadData
    {"SceLibHttp", 0xE8F15CDE, 0xFD76E1FB, 0},    // sceHttpSetResponseHeaderMaxSize
    {"SceLibHttp", 0xE8F15CDE, 0x34891C3F, 0},    // sceHttpSetAutoRedirect
    {"SceLibHttp", 0xE8F15CDE, 0x8455B5B3, 0},    // sceHttpSetResolveTimeOut
    {"SceLibHttp", 0xE8F15CDE, 0x237CA86E, 0},    // sceHttpSetConnectTimeOut
    {"SceLibHttp", 0xE8F15CDE, 0x94BF196E, 0},    // sceHttpSetRecvTimeOut
    {"SceLibHttp", 0xE8F15CDE, 0xAE8D7C33, 0},    // sceHttpsLoadCert
    {"SceLibHttp", 0xE8F15CDE, 0x8577833F, 0},    // sceHttpsUnloadCert
    {"SceLibHttp", 0xE8F15CDE, 0x9FBE2869, 0},    // sceHttpsEnableOption
    {"SceLibSsl", 0xCAF5F2E9, 0x3C733316, 0},     // sceSslInit
    {"SceLibSsl", 0xCAF5F2E9, 0x03CE6E3A, 0},     // sceSslTerm
    {"SceLibNetCtl", 0xC6083AE1, 0x6D26AC68, 0},  // sceNetCtlInetGetState
};
enum {
    HTTP_INIT, HTTP_TERM, HTTP_TEMPLATE, HTTP_DEL_TEMPLATE, HTTP_CONNECTION, HTTP_DEL_CONNECTION, HTTP_REQUEST,
    HTTP_DEL_REQUEST, HTTP_ADD_HEADER, HTTP_SEND, HTTP_STATUS, HTTP_READ, HTTP_HEADER_MAX, HTTP_REDIRECT,
    HTTP_RESOLVE_TIMEOUT, HTTP_CONNECT_TIMEOUT, HTTP_RECV_TIMEOUT, HTTPS_LOAD_CERT, HTTPS_UNLOAD_CERT,
    HTTPS_ENABLE, SSL_INIT, SSL_TERM, NETCTL_STATE, NET_COUNT
};

// For the log when GitHub's connection is refused: why, and what GitHub
// showed. Optional: the check works without them.
NetFn g_sslInfo[] = {
    {"SceLibHttp", 0xE8F15CDE, 0x2B79BDE0, 0},  // sceHttpsGetSslError
    {"SceLibHttp", 0xE8F15CDE, 0xA0926037, 0},  // sceHttpsSetSslCallback
    {"SceLibSsl", 0xCAF5F2E9, 0xBD203262, 0},   // sceSslGetMemoryPoolStats
    {"SceLibSsl", 0xCAF5F2E9, 0x9B2F1BC1, 0},   // sceSslGetSubjectName
    {"SceLibSsl", 0xCAF5F2E9, 0x412711E5, 0},   // sceSslGetIssuerName
    {"SceLibSsl", 0xCAF5F2E9, 0x95E14CA6, 0},   // sceSslGetNameEntryCount
    {"SceLibSsl", 0xCAF5F2E9, 0x2A857867, 0},   // sceSslGetNameEntryInfo
    {"SceLibSsl", 0xCAF5F2E9, 0xC73687E4, 0},   // sceSslFreeSslCertName
};
enum { SSL_GET_ERROR, SSL_SET_CALLBACK, SSL_POOL_STATS, SSL_SUBJECT, SSL_ISSUER, SSL_ENTRY_COUNT, SSL_ENTRY_INFO,
       SSL_FREE_NAME, SSL_INFO_COUNT };

// SceSysmodule, to load the HTTPS library if SceShell hasn't.
NetFn g_sysLoad = {"SceSysmodule", 0x03FCF19D, 0x79A0160A, 0};    // sceSysmoduleLoadModule
NetFn g_sysUnload = {"SceSysmodule", 0x03FCF19D, 0x31D87805, 0};  // sceSysmoduleUnloadModule

bool find(NetFn& f) {
    if (!f.fn && (taiGetModuleExportFunc(f.module, f.lib, f.nid, &f.fn) < 0 || !f.fn)) f.fn = 0;
    return f.fn != 0;
}
// The first one missing, or -1 when all are there.
int findAll() {
    int missing = -1;
    for (int i = 0; i < NET_COUNT; i++)
        if (!find(g_net[i]) && missing < 0) missing = i;
    return missing;
}

// What the check came to, for its one log line.
char g_note[400];
void note(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sceClibVsnprintf(g_note, sizeof(g_note), fmt, ap);
    va_end(ap);
}

#define CALL(i, type) ((type)g_net[i].fn)
typedef int (*IntFn)(int);
typedef int (*IntPtrFn)(void*);
typedef int (*UintFn)(unsigned int);
typedef int (*VoidFn)();
typedef int (*TemplateFn)(const char*, int, int);
typedef int (*ConnectionFn)(int, const char*, int);
typedef int (*RequestFn)(int, int, const char*, unsigned long long);
typedef int (*HeaderFn)(int, const char*, const char*, int);
typedef int (*SendFn)(int, const void*, unsigned int);
typedef int (*StatusFn)(int, int*);
typedef int (*ReadFn)(int, void*, unsigned int);
typedef int (*SetUintFn)(int, unsigned int);
typedef int (*SetIntFn)(int, int);
typedef int (*LoadCertFn)(int, const SceHttpsData**, const SceHttpsData*, const SceHttpsData*);
typedef int (*StateFn)(int*);

// ── What GitHub's certificates showed ───────────────────────────────────────
typedef void* (*NameFn)(void*);
typedef int (*EntryCountFn)(void*);
typedef int (*EntryInfoFn)(void*, int, char*, unsigned, char*, unsigned, unsigned*);
typedef int (*PoolStatsFn)(SceSslMemoryPoolStats*);
typedef int (*SslErrorFn)(int, int*, unsigned int*);
typedef int (*SslCallbackFn)(int, SceHttpsCallback, void*);

bool have(int i) { return g_sslInfo[i].fn != 0; }

// A certificate name's CN (or its last entry), into out.
void commonName(void* name, char* out, int cap) {
    out[0] = 0;
    if (!name) return;
    const int count = ((EntryCountFn)g_sslInfo[SSL_ENTRY_COUNT].fn)(name);
    for (int i = 0; i < count && i < 16; i++) {
        char oid[32], value[64];
        unsigned len = 0;
        sceClibMemset(value, 0, sizeof(value));
        if (((EntryInfoFn)g_sslInfo[SSL_ENTRY_INFO].fn)(name, i, oid, sizeof(oid), value, sizeof(value) - 1, &len) < 0)
            continue;
        value[len < sizeof(value) ? len : sizeof(value) - 1] = 0;
        sceClibStrncpy(out, value, cap - 1);
        out[cap - 1] = 0;
        if (!sceClibStrcmp(oid, "CN") || !sceClibStrcmp(oid, "commonName")) break;
    }
    ((IntPtrFn)g_sslInfo[SSL_FREE_NAME].fn)(name);
}

unsigned g_verifyErr;
int g_certCount = -1;
char g_chain[160];

// Called once the Vita has checked GitHub's certificates (only when it does
// check them): notes what it found, and lets the connection go on only when
// they passed.
int certificatesChecked(unsigned int verifyErr, void* const certs[], int count, void*) {
    g_verifyErr = verifyErr;
    g_certCount = count;
    g_chain[0] = 0;
    if (have(SSL_SUBJECT) && have(SSL_ISSUER) && have(SSL_ENTRY_COUNT) && have(SSL_ENTRY_INFO) && have(SSL_FREE_NAME) &&
        count > 0) {
        char subject[48], issuer[48];
        commonName(((NameFn)g_sslInfo[SSL_SUBJECT].fn)(certs[0]), subject, sizeof(subject));
        commonName(((NameFn)g_sslInfo[SSL_ISSUER].fn)(certs[count - 1]), issuer, sizeof(issuer));
        sceClibSnprintf(g_chain, sizeof(g_chain), "%s ... %s", subject, issuer);
    }
    return verifyErr == 0 ? 0 : -1;
}

// ── Requests ────────────────────────────────────────────────────────────────
struct Req {
    int tmpl, conn, req;
};

void close(Req& r) {
    if (r.req >= 0) CALL(HTTP_DEL_REQUEST, IntFn)(r.req);
    if (r.conn >= 0) CALL(HTTP_DEL_CONNECTION, IntFn)(r.conn);
    if (r.tmpl >= 0) CALL(HTTP_DEL_TEMPLATE, IntFn)(r.tmpl);
    r.req = r.conn = r.tmpl = -1;
}

// GET url over HTTPS: true with the answer ready to read on HTTP 200.
bool get(const char* url, const char* accept, Req& r) {
    r.conn = r.req = -1;
    r.tmpl = CALL(HTTP_TEMPLATE, TemplateFn)(USER_AGENT, SCE_HTTP_VERSION_1_1, 1);
    if (r.tmpl < 0) {
        note("template 0x%08X", (unsigned)r.tmpl);
        return false;
    }
    CALL(HTTP_REDIRECT, SetIntFn)(r.tmpl, 0);  // GitHub's API answers these directly
    if (have(SSL_SET_CALLBACK)) ((SslCallbackFn)g_sslInfo[SSL_SET_CALLBACK].fn)(r.tmpl, certificatesChecked, NULL);
    CALL(HTTP_HEADER_MAX, SetUintFn)(r.tmpl, kMaxHeader);
    CALL(HTTP_RESOLVE_TIMEOUT, SetUintFn)(r.tmpl, 10 * 1000 * 1000);
    CALL(HTTP_CONNECT_TIMEOUT, SetUintFn)(r.tmpl, 15 * 1000 * 1000);
    CALL(HTTP_RECV_TIMEOUT, SetUintFn)(r.tmpl, 30 * 1000 * 1000);
    r.conn = CALL(HTTP_CONNECTION, ConnectionFn)(r.tmpl, url, 1);
    if (r.conn >= 0) r.req = CALL(HTTP_REQUEST, RequestFn)(r.conn, SCE_HTTP_METHOD_GET, url, 0);
    if (r.req < 0) {
        note("connection 0x%08X, request 0x%08X", (unsigned)r.conn, (unsigned)r.req);
        close(r);
        return false;
    }
    CALL(HTTP_ADD_HEADER, HeaderFn)(r.req, "Accept", accept, SCE_HTTP_HEADER_OVERWRITE);
    const int sent = CALL(HTTP_SEND, SendFn)(r.req, NULL, 0);
    int status = 0;
    if (sent >= 0) CALL(HTTP_STATUS, StatusFn)(r.req, &status);
    if (sent < 0 || status != 200) {
        int sslErr = 0;
        unsigned detail = 0;
        if (sent < 0 && have(SSL_GET_ERROR)) ((SslErrorFn)g_sslInfo[SSL_GET_ERROR].fn)(r.req, &sslErr, &detail);
        SceSslMemoryPoolStats pool;
        sceClibMemset(&pool, 0, sizeof(pool));
        if (have(SSL_POOL_STATS)) ((PoolStatsFn)g_sslInfo[SSL_POOL_STATS].fn)(&pool);
        note("%s: 0x%08X, HTTP %d; SSL 0x%08X detail 0x%X; certificates %d checked 0x%X [%s]; SSL memory %u of %u KB, "
             "most %u KB",
             url, (unsigned)sent, status, (unsigned)sslErr, detail, g_certCount, g_verifyErr, g_chain,
             pool.currentInuseSize >> 10, pool.poolSize >> 10, pool.maxInuseSize >> 10);
        close(r);
        return false;
    }
    return true;
}

// Up to cap bytes of the answer into buf: how many, or -1.
int readSome(Req& r, uint8_t* buf, uint32_t cap) {
    uint32_t n = 0;
    while (n < cap) {
        const int got = CALL(HTTP_READ, ReadFn)(r.req, buf + n, cap - n);
        if (got < 0) return -1;
        if (got == 0) break;
        n += got;
    }
    return (int)n;
}

// ── Versions ───────────────────────────────────────────────────────────────
// "v0.10", "0.9.1": up to four numbers; how many.
int parseVersion(const char* s, int v[4]) {
    for (int i = 0; i < 4; i++) v[i] = 0;
    if (*s == 'v' || *s == 'V') s++;
    int n = 0;
    while (n < 4 && *s >= '0' && *s <= '9') {
        int x = 0;
        while (*s >= '0' && *s <= '9') {
            if (x < 100000) x = x * 10 + (*s - '0');
            s++;
        }
        v[n++] = x;
        if (*s != '.') break;
        s++;
    }
    return n;
}

bool newer(const char* tag, const char* ours) {
    int a[4], b[4];
    if (!parseVersion(tag, a) || !parseVersion(ours, b)) return false;
    for (int i = 0; i < 4; i++)
        if (a[i] != b[i]) return a[i] > b[i];
    return false;
}

// "tag_name": "..." in GitHub's answer (it comes in the first few KB). Only
// a version's characters are taken: it goes into a URL.
bool tagName(const char* json, char* out, int cap) {
    static const char key[] = "\"tag_name\"";
    for (const char* p = json; *p; p++) {
        if (sceClibStrncmp(p, key, sizeof(key) - 1)) continue;
        p += sizeof(key) - 1;
        while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p++ != '"') return false;
        int n = 0;
        while (*p && *p != '"' && n < cap - 1) {
            const char c = *p++;
            if (!((c >= '0' && c <= '9') || c == '.' || c == 'v' || c == 'V' || c == '-')) return false;
            out[n++] = c;
        }
        out[n] = 0;
        return *p == '"' && n > 0;
    }
    return false;
}

// ── The guard ───────────────────────────────────────────────────────────────
struct Guard {
    int unfinished;      // this version's checks that never finished
    char installed[32];  // the last tag installed, or "-"
};

// The next word of s (up to a space or line end) into out.
const char* word(const char* s, char* out, int cap) {
    while (*s == ' ') s++;
    int n = 0;
    while (*s && *s != ' ' && *s != '\n' && *s != '\r') {
        if (n < cap - 1) out[n++] = *s;
        s++;
    }
    out[n] = 0;
    return s;
}

void readGuard(Guard& g) {
    g.unfinished = 0;
    sceClibStrncpy(g.installed, "-", sizeof(g.installed));
    char buf[96], version[32], count[8];
    sceClibMemset(buf, 0, sizeof(buf));
    const SceUID fd = sceIoOpen(GUARD_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) return;
    sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    const char* p = word(word(buf, version, sizeof(version)), count, sizeof(count));
    word(p, g.installed, sizeof(g.installed));
    if (!g.installed[0]) sceClibStrncpy(g.installed, "-", sizeof(g.installed));
    // Another version's count starts again.
    if (!sceClibStrcmp(version, SSM_VERSION) && count[0] >= '0' && count[0] <= '9') g.unfinished = count[0] - '0';
}

void writeGuard(const Guard& g) {
    char buf[96];
    const int n = sceClibSnprintf(buf, sizeof(buf), "%s %d %s\n", SSM_VERSION, g.unfinished, g.installed);
    const SceUID fd = sceIoOpen(GUARD_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return;
    sceIoWrite(fd, buf, n);
    sceIoClose(fd);
}

// Where this plugin was loaded from (ur0:tai/startscreenmusic.suprx, or
// wherever config.txt names it).
bool ownPath(char* out, int cap) {
    tai_module_info_t ti;
    sceClibMemset(&ti, 0, sizeof(ti));
    ti.size = sizeof(ti);
    if (taiGetModuleInfo("StartScreenMusic", &ti) < 0) return false;
    SceKernelModuleInfo mi;
    sceClibMemset(&mi, 0, sizeof(mi));
    mi.size = sizeof(mi);
    if (sceKernelGetModuleInfo(ti.modid, &mi) < 0 || !mi.path[0]) return false;
    sceClibStrncpy(out, mi.path, cap - 1);
    out[cap - 1] = 0;
    return true;
}

// ── The check ───────────────────────────────────────────────────────────────
uint8_t g_buf[16 * 1024 + 1];

// The new plugin from GitHub straight into temp; true when it is whole and
// is a plugin (a signed ELF: "SCE" and a 0).
bool download(const char* tag, const char* temp, int& size) {
    char url[192];
    sceClibSnprintf(url, sizeof(url), FILE_URL_FMT, tag);
    Req r;
    size = 0;
    if (!get(url, "application/vnd.github.raw+json", r)) return false;
    const SceUID fd = sceIoOpen(temp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    bool ok = fd >= 0;
    while (ok) {
        const int n = readSome(r, g_buf, sizeof(g_buf) - 1);
        if (n <= 0) {
            ok = n == 0;
            break;
        }
        if (size == 0 && (n < 4 || g_buf[0] != 'S' || g_buf[1] != 'C' || g_buf[2] != 'E' || g_buf[3] != 0)) ok = false;
        else if (size + n > (int)kMaxPlugin) ok = false;
        else ok = sceIoWrite(fd, g_buf, n) == n;
        size += n;
    }
    if (fd >= 0) sceIoClose(fd);
    close(r);
    return ok && size >= (int)kMinPlugin;
}

void check(Guard& guard) {
    // The newest release's tag.
    Req r;
    if (!get(RELEASE_URL, "application/vnd.github+json", r)) return;
    const int n = readSome(r, g_buf, sizeof(g_buf) - 1);
    close(r);
    g_buf[n > 0 ? n : 0] = 0;
    char tag[32];
    if (n <= 0 || !tagName((const char*)g_buf, tag, sizeof(tag))) {
        note("no tag in GitHub's answer");
        return;
    }
    if (!newer(tag, SSM_VERSION)) {
        note("%s is the newest", SSM_VERSION);
        return;
    }
    if (!sceClibStrcmp(tag, guard.installed)) {
        note("%s was installed already, yet %s started", tag, SSM_VERSION);
        return;
    }
    char path[256], temp[260];
    if (!ownPath(path, sizeof(path))) {
        note("own path not found");
        return;
    }
    // Written beside it, then swapped in: a half-written file never takes the
    // plugin's name, and the old one comes back if the swap fails.
    char old[260];
    sceClibSnprintf(temp, sizeof(temp), "%s.new", path);
    sceClibSnprintf(old, sizeof(old), "%s.old", path);
    int size = 0;
    bool done = download(tag, temp, size);
    if (done) {
        sceIoRemove(old);
        done = sceIoRename(path, old) >= 0;
        if (done && sceIoRename(temp, path) < 0) {
            sceIoRename(old, path);
            done = false;
        }
        if (done) sceIoRemove(old);
    }
    if (!done) {
        sceIoRemove(temp);
        note("%s not installed (%d bytes)", tag, size);
        return;
    }
    sceClibStrncpy(guard.installed, tag, sizeof(guard.installed));
    note("%s installed in %s (%d bytes), for the next restart", tag, path, size);
}

bool g_loadedHttps, g_startedSsl, g_startedHttp;

void stopNet() {
    // Only what this started: SceShell's own stays as it is.
    if (g_startedHttp) {
        CALL(HTTPS_UNLOAD_CERT, VoidFn)();
        CALL(HTTP_TERM, VoidFn)();
    }
    if (g_startedSsl) CALL(SSL_TERM, VoidFn)();
    if (g_loadedHttps) ((IntFn)g_sysUnload.fn)(SCE_SYSMODULE_HTTPS);
}

int freeKb() {
    FreeMemory m;
    sceClibMemset(&m, 0, sizeof(m));
    m.size = sizeof(m);
    return ssm_sceKernelGetFreeMemorySize(&m) >= 0 ? m.sizeUser >> 10 : 0;
}

int updateThread(SceSize, void*) {
    sceKernelDelayThread(kDelaySeconds * 1000 * 1000);
    Guard guard;
    readGuard(guard);
    if (guard.unfinished >= 3) {
        logLine("update: off for %s (three checks never finished)", SSM_VERSION);
        return sceKernelExitDeleteThread(0);
    }
    // Wi-Fi first: it may be off, or still connecting after the Vita wakes, so
    // look again every 30 seconds for 30 minutes before giving up for this
    // boot.
    if (!find(g_net[NETCTL_STATE])) return sceKernelExitDeleteThread(0);
    int state = 0;
    for (int tries = 0;; tries++) {
        if (CALL(NETCTL_STATE, StateFn)(&state) >= 0 && state == SCE_NETCTL_STATE_CONNECTED) break;
        if (tries == kWifiTries) {
            logLine("update: no Wi-Fi in %d minutes, not checked", kWifiTries / 2);
            return sceKernelExitDeleteThread(0);
        }
        sceKernelDelayThread(30 * 1000 * 1000);
    }
    int missing = findAll();
    if (missing >= 0) {
        // SceShell hasn't loaded the HTTPS library: load it for the check.
        if (find(g_sysLoad) && find(g_sysUnload) && ((IntFn)g_sysLoad.fn)(SCE_SYSMODULE_HTTPS) >= 0)
            g_loadedHttps = true;
        missing = findAll();
    }
    if (missing >= 0) {
        logLine("update: %s 0x%08X not found", g_net[missing].module, (unsigned)g_net[missing].nid);
        stopNet();
        return sceKernelExitDeleteThread(0);
    }
    for (int i = 0; i < SSL_INFO_COUNT; i++) find(g_sslInfo[i]);
    guard.unfinished++;
    writeGuard(guard);
    // SceShell's SSL and HTTP when it has started them, or this check's own.
    int rc = CALL(SSL_INIT, UintFn)(kSslPool);
    g_startedSsl = rc >= 0;
    bool up = rc >= 0 || rc == (int)SCE_SSL_ERROR_ALREADY_INITED;
    if (up) {
        rc = CALL(HTTP_INIT, UintFn)(kHttpPool);
        g_startedHttp = rc >= 0;
        up = rc >= 0 || rc == (int)SCE_HTTP_ERROR_ALREADY_INITED;
    }
    // SceShell keeps working room: no check when the pools left it short.
    const int kb = freeKb();
    int certRc = 0;
    note("network not started (0x%08X)", (unsigned)rc);
    if (up && kb < 256) {
        note("not now");
    } else if (up) {
        static SceHttpsData certs[UPDATE_CERT_COUNT];
        static const SceHttpsData* list[UPDATE_CERT_COUNT];
        for (int i = 0; i < UPDATE_CERT_COUNT; i++) {
            certs[i].ptr = (char*)cert_pem[i];
            certs[i].size = (unsigned int)sceClibStrnlen(cert_pem[i], 8192);
            list[i] = &certs[i];
        }
        certRc = CALL(HTTPS_LOAD_CERT, LoadCertFn)(UPDATE_CERT_COUNT, list, NULL, NULL);
        if (g_startedHttp) CALL(HTTPS_ENABLE, UintFn)(SCE_HTTPS_FLAG_SERVER_VERIFY);
        check(guard);
    }
    stopNet();
    // One line: what came of it, whose SSL and HTTP it used (SceShell's or
    // its own), and the memory SceShell had left during it.
    logLine("update: %s (SSL %s, HTTP %s, %d KB free%s)", g_note, g_startedSsl ? "own" : "SceShell's",
            g_startedHttp ? "own" : "SceShell's", kb, certRc < 0 ? ", certificates refused" : "");
    guard.unfinished = 0;  // finished
    writeGuard(guard);
    return sceKernelExitDeleteThread(0);
}

}  // namespace

void startUpdateCheck() {
    const SceUID t = sceKernelCreateThread("StartScreenMusicUpdate", updateThread, 191, 0x8000, 0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
}

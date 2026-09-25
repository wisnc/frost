#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdint.h>
#include "AudioFileSource.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutput.h"

static const int SD_SCK = 40, SD_MISO = 39, SD_MOSI = 14, SD_CS = 12;

static const char KEY_UP = ';', KEY_DOWN = '.', KEY_LEFT = ',', KEY_RIGHT = '/';
static const char KEY_NEXT = ']', KEY_PREV = '[';
static const char KEY_SHUFFLE = '\\';
static const char KEY_ENQUEUE = '\'';
static const char KEY_VOLUP = '=';
static const char KEY_VOLDN_A = '_', KEY_VOLDN_B = '-';
static const char KEY_SCAN_A = 's', KEY_SCAN_B = 'S';


static const int SCR_W = 240, SCR_H = 135;
static const int MARGIN = 5, GAP = 2, BORDER = 2, PAD = 2, INSET = BORDER + PAD;

static const int LEFT_X = MARGIN, LEFT_Y = MARGIN, LEFT_W = 116, LEFT_H = SCR_H - 2 * MARGIN;
static const int RIGHT_X = LEFT_X + LEFT_W + GAP, RIGHT_W = SCR_W - MARGIN - RIGHT_X;
static const int NP_Y = MARGIN, NP_H = 50;
static const int Q_Y = NP_Y + NP_H + GAP, Q_H = SCR_H - MARGIN - Q_Y;

static const int LC_X = LEFT_X + INSET, LC_Y = LEFT_Y + INSET, LC_W = LEFT_W - 2 * INSET, LC_H = LEFT_H - 2 * INSET;
static const int NC_X = RIGHT_X + INSET, NC_Y = NP_Y + INSET, NC_W = RIGHT_W - 2 * INSET, NC_H = NP_H - 2 * INSET;
static const int QC_X = RIGHT_X + INSET, QC_Y = Q_Y + INSET, QC_W = RIGHT_W - 2 * INSET, QC_H = Q_H - 2 * INSET;

static const lgfx::IFont* FONT = &fonts::lgfxJapanGothic_12;
static const int ROW_H = 13;
static const int BROWSER_ROWS = LC_H / ROW_H;
static const int QUEUE_ROWS   = QC_H / ROW_H;

static const char* CFG_DIR   = "/.frost";
static const char* CFG_PATH  = "/.frost/config";
static const char* IDX_TXT   = "/.frost/index.txt";
static const char* IDX_BIN   = "/.frost/index.bin";

static const int MY_PATH_MAX = 256;
static char     cfgMusicDir[MY_PATH_MAX] = "/Movie";
static int      cfgBrightness = 255;
static uint32_t cfgScreenTimeoutMs = 30000;
static uint32_t cfgAccentRGB = 0xFFFFFF;
static uint32_t cfgBackgroundRGB = 0x000000;
static bool     cfgShuffle = false;
static int      cfgAvOffsetMs = 80;
static bool     cfgBootAnim = true;

static uint16_t COL_ACCENT, COL_BG, COL_TEXT, COL_DIM;

static constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static uint16_t rgb565From24(uint32_t c) { return rgb565((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF); }

static uint32_t blend24(uint32_t a, uint32_t b, int t) {
    int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    int r = ar + ((br - ar) * t) / 255, g = ag + ((bg - ag) * t) / 255, bl = ab + ((bb - ab) * t) / 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}
static int luma24(uint32_t c) { return (299 * ((c >> 16) & 0xFF) + 587 * ((c >> 8) & 0xFF) + 114 * (c & 0xFF)) / 1000; }

static void applyTheme() {
    COL_ACCENT = rgb565From24(cfgAccentRGB);
    COL_BG     = rgb565From24(cfgBackgroundRGB);
    uint32_t pole = luma24(cfgBackgroundRGB) < 128 ? 0xFFFFFF : 0x000000;
    COL_TEXT   = rgb565From24(blend24(cfgAccentRGB, pole, 190));
    COL_DIM    = rgb565From24(blend24(cfgAccentRGB, cfgBackgroundRGB, 130));
}

static bool parseHex24(const char* s, uint32_t &out) {
    if (*s == '#') s++;
    if (strlen(s) != 6) return false;
    uint32_t v = 0;
    for (int i = 0; i < 6; i++) {
        char c = s[i]; int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = (v << 4) | d;
    }
    out = v; return true;
}

static void trimRight(char* s) {
    int n = strlen(s);
    while (n > 0 && (s[n-1] == '\r' || s[n-1] == '\n' || s[n-1] == ' ' || s[n-1] == '\t')) s[--n] = '\0';
}

static void saveConfig() {
    SD.mkdir(CFG_DIR);
    File f = SD.open(CFG_PATH, FILE_WRITE);
    if (!f) return;
    f.printf("movie_dir=%s\n", cfgMusicDir);
    f.printf("brightness=%d\n", cfgBrightness);
    f.printf("screen_timeout=%lu\n", (unsigned long)cfgScreenTimeoutMs);
    f.printf("accent=%06lX\n", (unsigned long)cfgAccentRGB);
    f.printf("background=%06lX\n", (unsigned long)cfgBackgroundRGB);
    f.printf("shuffle=%d\n", cfgShuffle ? 1 : 0);
    f.printf("av_offset=%d\n", cfgAvOffsetMs);
    f.printf("boot_animation=%d\n", cfgBootAnim ? 1 : 0);
    f.close();
}

static void loadConfig() {
    File f = SD.open(CFG_PATH, FILE_READ);
    if (!f) { saveConfig(); return; }
    char line[320];
    while (f.available()) {
        int n = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[n] = '\0';
        trimRight(line);
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char* key = line; const char* val = eq + 1;
        if      (!strcmp(key, "movie_dir"))      { if (val[0] == '/') { strncpy(cfgMusicDir, val, MY_PATH_MAX - 1); cfgMusicDir[MY_PATH_MAX - 1] = '\0'; } }
        else if (!strcmp(key, "brightness"))     { int v = atoi(val); if (v < 0) v = 0; if (v > 255) v = 255; cfgBrightness = v; }
        else if (!strcmp(key, "screen_timeout")) { cfgScreenTimeoutMs = (uint32_t)strtoul(val, nullptr, 10); }
        else if (!strcmp(key, "accent"))         { parseHex24(val, cfgAccentRGB); }
        else if (!strcmp(key, "background"))     { parseHex24(val, cfgBackgroundRGB); }
        else if (!strcmp(key, "shuffle"))        { cfgShuffle = atoi(val) != 0; }
        else if (!strcmp(key, "av_offset"))      { int v = atoi(val); if (v < -1000) v = -1000; if (v > 1000) v = 1000; cfgAvOffsetMs = v; }
        else if (!strcmp(key, "boot_animation")) { cfgBootAnim = atoi(val) != 0; }
    }
    f.close();
    int L = strlen(cfgMusicDir);
    if (L > 1 && cfgMusicDir[L-1] == '/') cfgMusicDir[L-1] = '\0';
}

static const int MAX_ENTRIES = 256, NAME_POOL_SIZE = 8192, MAX_DEPTH = 8;
static char     namePool[NAME_POOL_SIZE];
static uint16_t nameOffset[MAX_ENTRIES];
static bool     entryIsDir[MAX_ENTRIES];
static int      sortIdx[MAX_ENTRIES];
static int      entryCount = 0, poolUsed = 0;

static char currentPath[MY_PATH_MAX] = "/";
static int  cursor = 0, scroll = 0, depth = 0;
static int  cursorStack[MAX_DEPTH], scrollStack[MAX_DEPTH];
static bool rootOk = false;

static const int QUEUE_MAX = 256, QNAME_POOL = 16384;
static char     queuePool[QNAME_POOL];
static uint16_t queueOffset[QUEUE_MAX];
static int      queueCount = 0, queuePoolUsed = 0;
static char     queueFolder[MY_PATH_MAX] = "";
static int      queuePos = 0;

enum PlayState { STOPPED, PLAYING, PAUSED };
static PlayState playState = STOPPED;
static char nowPlaying[64] = "";
static int  volume = 50;

static char curArtist[96] = "";
static char curTitle[96]  = "";
static char curAlbum[96]  = "";
static char npTitle[96] = "";
static char npArtist[96] = "";
static char npAlbum[96] = "";
static char npLine2[2 * 96 + 4] = "";

class AudioFileSourceRing;
static AudioGenerator      *decoder = nullptr;
static AudioFileSourceRing *file = nullptr;
static volatile uint32_t samplesOut = 0;

class AudioOutputM5Speaker : public AudioOutput {
public:
    AudioOutputM5Speaker(m5::Speaker_Class* m5sound, uint8_t ch = 0) { _m5sound = m5sound; _virtual_ch = ch; }
    bool begin() override { return true; }
    bool SetChannels(int chan) override { channels = chan; return true; }
    bool ConsumeSample(int16_t sample[2]) override {
        if (_tri_buffer_index < tri_buf_size) {
            int16_t l = sample[0];
            int16_t r = (channels == 1) ? sample[0] : sample[1];
            _tri_buffer[_tri_index][_tri_buffer_index]   = l;
            _tri_buffer[_tri_index][_tri_buffer_index+1] = r;
            _tri_buffer_index += 2;
            samplesOut++;
            return true;
        }
        flush();
        return false;
    }
    void flush() override {
        if (_tri_buffer_index) {
            _m5sound->playRaw(_tri_buffer[_tri_index], _tri_buffer_index, hertz, true, 1, _virtual_ch);
            _tri_index = _tri_index < 2 ? _tri_index + 1 : 0;
            _tri_buffer_index = 0;
        }
    }
    bool stop() override { flush(); _m5sound->stop(_virtual_ch); return true; }
    uint32_t rate() const { return hertz; }
protected:
    m5::Speaker_Class* _m5sound; uint8_t _virtual_ch;
    static constexpr size_t tri_buf_size = 4096;
    int16_t _tri_buffer[3][tri_buf_size];
    size_t _tri_buffer_index = 0, _tri_index = 0;
};
static AudioOutputM5Speaker *out = nullptr;
alignas(8) static uint8_t mp3Space[AudioGeneratorMP3::preAllocSize()];

static bool needsFullRedraw = true;
static bool redrawBrowser = false, redrawNowPlaying = false, redrawQueue = false;

static bool displayOn = true;
static bool screenIsOff = false;
static unsigned long lastInputTime = 0;
static bool screenVisible() { return displayOn && !screenIsOff; }
static bool panelAsleep = false;
static volatile bool videoDrawEnabled = true;
static SemaphoreHandle_t dispMutex = nullptr;
static void dispLock()   { if (dispMutex) xSemaphoreTakeRecursive(dispMutex, portMAX_DELAY); }
static void dispUnlock() { if (dispMutex) xSemaphoreGiveRecursive(dispMutex); }
static void updateVideoDraw();
static void applyBacklight() {
    auto &d = M5Cardputer.Display;
    dispLock();
    if (screenVisible()) {
        if (panelAsleep) { setCpuFrequencyMhz(240); d.wakeup(); panelAsleep = false; }
        d.setBrightness(cfgBrightness);
        updateVideoDraw();
    } else {
        videoDrawEnabled = false;
        d.setBrightness(0);
        if (!panelAsleep) { d.sleep(); panelAsleep = true; setCpuFrequencyMhz(160); }
    }
    dispUnlock();
}

static bool hasExt(const char* s, const char* ext) {
    int n = (int)strlen(s), el = (int)strlen(ext);
    if (n < el) return false;
    const char* e = s + n - el;
    for (int i = 0; i < el; i++) {
        char a = e[i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}
static bool isAudioFile(const char* nm) {
    return hasExt(nm, ".avi");
}
static const char* baseName(const char* p) {
    const char* s = strrchr(p, '/');
    return s ? s + 1 : p;
}
static void stripExt(const char* in, char* out, size_t outSize) {
    strncpy(out, in, outSize - 1); out[outSize - 1] = '\0';
    char* dot = strrchr(out, '.');
    if (dot && dot != out) *dot = '\0';
}
static int nameCmp(const char* a, const char* b) {
    while (*a && *b) {
        unsigned char ca = *a, cb = *b;
        if (ca >= '0' && ca <= '9' && cb >= '0' && cb <= '9') {
            while (*a == '0') a++;
            while (*b == '0') b++;
            const char *ea = a, *eb = b;
            while (*ea >= '0' && *ea <= '9') ea++;
            while (*eb >= '0' && *eb <= '9') eb++;
            int la = ea - a, lb = eb - b;
            if (la != lb) return la - lb;
            while (a < ea && b < eb) {
                if (*a != *b) return (int)*a - (int)*b;
                a++; b++;
            }
            continue;
        }
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return (int)ca - (int)cb;
        a++; b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
static void joinPath(char* dst, size_t n, const char* dir, const char* name) {
    if (strcmp(dir, "/") == 0) snprintf(dst, n, "/%s", name);
    else snprintf(dst, n, "%s/%s", dir, name);
}

static const char* entryName(int i) { return &namePool[nameOffset[i]]; }
static const char* nameAt(int i)    { return &namePool[nameOffset[sortIdx[i]]]; }
static bool isDirAt(int i)          { return entryIsDir[sortIdx[i]]; }

static void sortEntries() {
    for (int i = 0; i < entryCount; i++) sortIdx[i] = i;
    for (int i = 1; i < entryCount; i++) {
        int key = sortIdx[i], j = i - 1;
        while (j >= 0) {
            int a = sortIdx[j];
            bool swap;
            if (entryIsDir[a] != entryIsDir[key]) swap = (!entryIsDir[a] && entryIsDir[key]);
            else swap = (nameCmp(entryName(a), entryName(key)) > 0);
            if (!swap) break;
            sortIdx[j + 1] = sortIdx[j];
            j--;
        }
        sortIdx[j + 1] = key;
    }
}

static const char* SD_MOUNT = "/sd";
static DIR* openSdDir(const char* path) {
    char vfs[MY_PATH_MAX + 8];
    snprintf(vfs, sizeof(vfs), "%s%s", SD_MOUNT, path);
    return opendir(vfs);
}
static bool direntIsDir(const char* dirPath, const struct dirent* de) {
    if (de->d_type == DT_DIR) return true;
    if (de->d_type == DT_REG) return false;
    char vfs[MY_PATH_MAX + 8];
    snprintf(vfs, sizeof(vfs), "%s%s/%s", SD_MOUNT, dirPath, de->d_name);
    struct stat st;
    return stat(vfs, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool loadDir() {
    entryCount = 0; poolUsed = 0;
    DIR* dir = openSdDir(currentPath);
    if (!dir) return false;
    struct dirent* de;
    while ((de = readdir(dir)) != nullptr && entryCount < MAX_ENTRIES) {
        const char* nm = de->d_name;
        int len = strlen(nm);
        if (nm[0] != '.' && (poolUsed + len + 1) < NAME_POOL_SIZE) {
            bool isDir = direntIsDir(currentPath, de);
            if (isDir || isAudioFile(nm)) {
                nameOffset[entryCount] = poolUsed;
                entryIsDir[entryCount] = isDir;
                memcpy(&namePool[poolUsed], nm, len + 1);
                poolUsed += len + 1;
                entryCount++;
            }
        }
    }
    closedir(dir);
    sortEntries();
    cursor = 0; scroll = 0;
    return true;
}

static void buildQueue(const char* folder) {
    queueCount = 0; queuePoolUsed = 0;
    strncpy(queueFolder, folder, MY_PATH_MAX - 1);
    queueFolder[MY_PATH_MAX - 1] = '\0';

    DIR* dir = openSdDir(folder);
    if (!dir) return;
    struct dirent* de;
    char full[MY_PATH_MAX];
    while ((de = readdir(dir)) != nullptr && queueCount < QUEUE_MAX) {
        const char* nm = de->d_name;
        if (nm[0] == '.' || direntIsDir(folder, de) || !isAudioFile(nm)) continue;
        joinPath(full, sizeof(full), folder, nm);
        int len = strlen(full);
        if ((queuePoolUsed + len + 1) < QNAME_POOL) {
            queueOffset[queueCount] = queuePoolUsed;
            memcpy(&queuePool[queuePoolUsed], full, len + 1);
            queuePoolUsed += len + 1;
            queueCount++;
        }
    }
    closedir(dir);

    for (int i = 1; i < queueCount; i++) {
        uint16_t key = queueOffset[i]; int j = i - 1;
        while (j >= 0 && nameCmp(&queuePool[queueOffset[j]], &queuePool[key]) > 0) {
            queueOffset[j + 1] = queueOffset[j]; j--;
        }
        queueOffset[j + 1] = key;
    }
}

static const char* queueName(int i) { return &queuePool[queueOffset[i]]; }

static void copyBounded(const uint8_t* b, size_t n, char* dst, size_t dstSize) {
    size_t o = 0;
    for (; o < n && o + 1 < dstSize && b[o]; o++) dst[o] = (char)b[o];
    dst[o] = '\0';
}

static const int INDEX_MAX = 4096;
static uint32_t idxHash[INDEX_MAX];
static uint32_t idxOffset[INDEX_MAX];
static int      idxCount = 0;
static File     indexFile;
static const int TITLE_MAX = 96;

static uint32_t fnv1a(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

static void sortIndex() {
    for (int gap = idxCount / 2; gap > 0; gap /= 2) {
        for (int i = gap; i < idxCount; i++) {
            uint32_t h = idxHash[i], o = idxOffset[i]; int j = i;
            while (j >= gap && idxHash[j - gap] > h) { idxHash[j] = idxHash[j - gap]; idxOffset[j] = idxOffset[j - gap]; j -= gap; }
            idxHash[j] = h; idxOffset[j] = o;
        }
    }
}

static void closeIndex() { if (indexFile) indexFile.close(); idxCount = 0; }

static bool loadIndex() {
    closeIndex();
    File b = SD.open(IDX_BIN, FILE_READ);
    if (!b) return false;
    char magic[4]; uint32_t n = 0;
    bool ok = b.read((uint8_t*)magic, 4) == 4 && memcmp(magic, "EMIX", 4) == 0 && b.read((uint8_t*)&n, 4) == 4 && n <= (uint32_t)INDEX_MAX;
    if (ok) {
        for (uint32_t i = 0; i < n; i++) {
            if (b.read((uint8_t*)&idxHash[i], 4) != 4 || b.read((uint8_t*)&idxOffset[i], 4) != 4) { ok = false; break; }
        }
    }
    b.close();
    if (!ok) { idxCount = 0; return false; }
    idxCount = (int)n;
    indexFile = SD.open(IDX_TXT, FILE_READ);
    if (!indexFile) { idxCount = 0; return false; }
    return true;
}

static void takeField(char* dst, size_t n, const char* src) {
    if (!dst || !n) return;
    if (src) { strncpy(dst, src, n - 1); dst[n - 1] = '\0'; } else dst[0] = '\0';
}
static bool readIndexLine(uint32_t off, const char* wantPath, char* title, size_t tl, char* artist, size_t al, char* album, size_t abl) {
    if (!indexFile) return false;
    if (!indexFile.seek(off)) return false;
    static char line[MY_PATH_MAX + 3 * TITLE_MAX + 8];
    int n = indexFile.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = '\0';
    trimRight(line);
    char* t1 = strchr(line, '\t'); if (!t1) return false;
    *t1 = '\0';
    if (strcmp(line, wantPath) != 0) return false;
    char* f2 = t1 + 1;
    char* t2 = strchr(f2, '\t'); char* f3 = nullptr; char* f4 = nullptr;
    if (t2) { *t2 = '\0'; f3 = t2 + 1; char* t3 = strchr(f3, '\t'); if (t3) { *t3 = '\0'; f4 = t3 + 1; } }
    takeField(title, tl, f2);
    takeField(artist, al, f3);
    takeField(album, abl, f4);
    return true;
}

static bool indexLookup(const char* path, char* title, size_t tl, char* artist = nullptr, size_t al = 0, char* album = nullptr, size_t abl = 0) {
    if (idxCount == 0) return false;
    uint32_t h = fnv1a(path);
    int lo = 0, hi = idxCount - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (idxHash[mid] < h) lo = mid + 1;
        else if (idxHash[mid] > h) hi = mid - 1;
        else {
            int i = mid;
            while (i > 0 && idxHash[i - 1] == h) i--;
            for (; i < idxCount && idxHash[i] == h; i++) {
                if (readIndexLine(idxOffset[i], path, title, tl, artist, al, album, abl)) return true;
            }
            return false;
        }
    }
    return false;
}

static void displayTitleFor(const char* path, char* out, size_t outSize) {
    if (indexLookup(path, out, outSize) && out[0]) return;
    stripExt(baseName(path), out, outSize);
}

static uint32_t le32(const uint8_t* p) { return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0]; }
static constexpr uint32_t FCC(const char* s) { return (uint32_t)(uint8_t)s[0] | ((uint32_t)(uint8_t)s[1] << 8) | ((uint32_t)(uint8_t)s[2] << 16) | ((uint32_t)(uint8_t)s[3] << 24); }
static uint32_t rd32(File &f) { uint8_t b[4]; if (f.read(b, 4) != 4) return 0; return le32(b); }

struct AviInfo {
    uint32_t moviStart = 0, moviEnd = 0;
    uint32_t vScale = 1, vRate = 20, totalFrames = 0;
    int vidIdx = -1, audIdx = -1, strCount = 0, lastStrh = 0;
    uint16_t audioTag = 0;
};

static void aviParseRange(File &f, uint32_t pos, uint32_t end, int depth, AviInfo &info,
                          char* title, size_t tl, char* artist, size_t al, char* album, size_t abl) {
    if (depth > 4) return;
    while (pos + 8 <= end) {
        if (!f.seek(pos)) return;
        uint32_t id = rd32(f), size = rd32(f);
        uint32_t payload = pos + 8, next = payload + size + (size & 1);
        if (next <= pos) return;
        if (id == FCC("LIST")) {
            uint32_t type = rd32(f);
            if (type == FCC("movi")) { if (!info.moviStart) { info.moviStart = payload + 4; info.moviEnd = payload + size; } }
            else if (type == FCC("hdrl") || type == FCC("strl") || type == FCC("INFO"))
                aviParseRange(f, payload + 4, payload + size, depth + 1, info, title, tl, artist, al, album, abl);
        } else if (id == FCC("avih") && size >= 20) {
            uint8_t b[20]; if (f.read(b, 20) == 20) info.totalFrames = le32(b + 16);
        } else if (id == FCC("strh") && size >= 28) {
            uint8_t b[28];
            if (f.read(b, 28) == 28) {
                uint32_t type = le32(b);
                info.lastStrh = type;
                if (type == FCC("vids")) { info.vidIdx = info.strCount; info.vScale = le32(b + 20); info.vRate = le32(b + 24); }
                else if (type == FCC("auds")) info.audIdx = info.strCount;
            }
            info.strCount++;
        } else if (id == FCC("strf") && info.lastStrh == FCC("auds") && size >= 2) {
            uint8_t b[2]; if (f.read(b, 2) == 2) info.audioTag = b[0] | (b[1] << 8);
            info.lastStrh = 0;
        } else if (id == FCC("INAM") || id == FCC("IART") || id == FCC("IPRD")) {
            static uint8_t b[TITLE_MAX];
            size_t n = size < sizeof(b) ? size : sizeof(b);
            n = f.read(b, n);
            char* dst = (id == FCC("INAM")) ? title : (id == FCC("IART")) ? artist : album;
            size_t dl = (id == FCC("INAM")) ? tl : (id == FCC("IART")) ? al : abl;
            if (dst && dl) copyBounded(b, n, dst, dl);
        }
        pos = next;
    }
}

static bool aviParse(const char* path, AviInfo &info, char* title, size_t tl, char* artist, size_t al, char* album, size_t abl) {
    if (title) title[0] = '\0';
    if (artist) artist[0] = '\0';
    if (album) album[0] = '\0';
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    uint8_t h[12];
    bool ok = f.read(h, 12) == 12 && le32(h) == FCC("RIFF") && le32(h + 8) == FCC("AVI ");
    if (ok) aviParseRange(f, 12, f.size(), 0, info, title, tl, artist, al, album, abl);
    f.close();
    if (info.vScale == 0) info.vScale = 1;
    if (info.vRate == 0) info.vRate = 20;
    return ok && info.moviStart && info.vidIdx >= 0 && info.audIdx >= 0;
}

static void readTags(const char* path, char* title, size_t tl, char* artist, size_t al, char* album, size_t abl) {
    AviInfo info;
    aviParse(path, info, title, tl, artist, al, album, abl);
}

static void sanitizeField(char* s) {
    for (char* p = s; *p; p++) if (*p == '\t' || *p == '\n' || *p == '\r') *p = ' ';
    trimRight(s);
}

static void drawPaneFrames();
static void drawAll();
static bool fullscreen = false;
static void drawScanProgress(int tracks, const char* status) {
    if (!screenVisible()) return;
    auto &d = M5Cardputer.Display;
    d.fillRect(NC_X, NC_Y, NC_W, NC_H, COL_BG);
    d.setTextColor(COL_ACCENT, COL_BG);
    d.setCursor(NC_X, NC_Y);
    d.print(status);
    d.setTextColor(COL_TEXT, COL_BG);
    d.setCursor(NC_X, NC_Y + ROW_H);
    d.printf("%d tracks", tracks);
}

static File scanOut;
static int  scanTracks = 0, scanSkipped = 0;
static unsigned long scanLastDraw = 0;

static char scanPathBuf[MAX_DEPTH][MY_PATH_MAX];
static char scanNameBuf[MY_PATH_MAX];

static void scanDir(const char* path, int level) {
    if (level + 1 >= MAX_DEPTH) return;
    char* full = scanPathBuf[level + 1];
    {
        File dir = SD.open(path);
        if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }
        File e = dir.openNextFile();
        while (e) {
            strncpy(scanNameBuf, baseName(e.name()), MY_PATH_MAX - 1);
            scanNameBuf[MY_PATH_MAX - 1] = '\0';
            bool isDir = e.isDirectory();
            e.close();
            const char* nm = scanNameBuf;
            if (nm[0] != '.' && !isDir && isAudioFile(nm)) {
                joinPath(full, MY_PATH_MAX, path, nm);
                if (scanTracks < INDEX_MAX && strlen(full) < MY_PATH_MAX - 1) {
                    static char title[TITLE_MAX], artist[TITLE_MAX], album[TITLE_MAX];
                    readTags(full, title, sizeof(title), artist, sizeof(artist), album, sizeof(album));
                    sanitizeField(title); sanitizeField(artist); sanitizeField(album);
                    if (!title[0]) stripExt(nm, title, sizeof(title));
                    uint32_t off = scanOut.position();
                    scanOut.print(full); scanOut.print('\t'); scanOut.print(title); scanOut.print('\t'); scanOut.print(artist); scanOut.print('\t'); scanOut.print(album); scanOut.print('\n');
                    idxHash[scanTracks] = fnv1a(full);
                    idxOffset[scanTracks] = off;
                    scanTracks++;
                } else scanSkipped++;
                if (millis() - scanLastDraw > 150) { scanLastDraw = millis(); drawScanProgress(scanTracks, "Scanning"); }
            }
            e = dir.openNextFile();
        }
        dir.close();
    }
    for (int k = 0; ; k++) {
        File dir = SD.open(path);
        if (!dir) return;
        scanNameBuf[0] = '\0';
        int seen = 0;
        File e = dir.openNextFile();
        while (e) {
            const char* nm = baseName(e.name());
            if (e.isDirectory() && nm[0] != '.') {
                if (seen == k) { strncpy(scanNameBuf, nm, MY_PATH_MAX - 1); scanNameBuf[MY_PATH_MAX - 1] = '\0'; e.close(); break; }
                seen++;
            }
            e.close();
            e = dir.openNextFile();
        }
        dir.close();
        if (!scanNameBuf[0]) return;
        joinPath(full, MY_PATH_MAX, path, scanNameBuf);
        if (strlen(full) < MY_PATH_MAX - 2) scanDir(full, level + 1);
    }
}

static void stopPlayback();
static void runScan() {
    if (playState != STOPPED) { stopPlayback(); playState = STOPPED; nowPlaying[0] = '\0'; npTitle[0] = npLine2[0] = '\0'; }
    if (fullscreen) { fullscreen = false; if (screenVisible()) drawAll(); }
    closeIndex();
    SD.mkdir(CFG_DIR);
    SD.remove(IDX_TXT); SD.remove(IDX_BIN);
    scanOut = SD.open(IDX_TXT, FILE_WRITE);
    scanTracks = 0; scanSkipped = 0; scanLastDraw = 0;
    drawScanProgress(0, "Scanning");
    if (scanOut) {
        scanDir(cfgMusicDir, 0);
        scanOut.close();
        idxCount = scanTracks;
        sortIndex();
        File b = SD.open(IDX_BIN, FILE_WRITE);
        if (b) {
            uint32_t n = (uint32_t)idxCount;
            b.write((const uint8_t*)"EMIX", 4);
            b.write((const uint8_t*)&n, 4);
            for (int i = 0; i < idxCount; i++) { b.write((const uint8_t*)&idxHash[i], 4); b.write((const uint8_t*)&idxOffset[i], 4); }
            b.close();
        }
        drawScanProgress(scanTracks, scanSkipped ? "Done (full)" : "Done");
    } else {
        drawScanProgress(0, "Scan failed");
    }
    loadIndex();
    delay(700);
    needsFullRedraw = true;
}

static void buildLine2() {
    if (npArtist[0] && npAlbum[0]) snprintf(npLine2, sizeof(npLine2), "%s | %s", npArtist, npAlbum);
    else if (npArtist[0]) snprintf(npLine2, sizeof(npLine2), "%s", npArtist);
    else if (npAlbum[0]) snprintf(npLine2, sizeof(npLine2), "%s", npAlbum);
    else npLine2[0] = '\0';
}

static SemaphoreHandle_t sdMutex = nullptr;
static volatile uint32_t statAudioWaitCur = 0;
static void sdLock()   { if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY); }
static void sdUnlock() { if (sdMutex) xSemaphoreGive(sdMutex); }

static volatile bool readerActive = false;
static const uint32_t AUDIO_RING = 8192, VIDEO_RING = 65536, AUDIO_LOW = 1024, AUDIO_PREFILL = 2048;
static SemaphoreHandle_t ringMutex = nullptr;
static void ringLock()   { if (ringMutex) xSemaphoreTake(ringMutex, portMAX_DELAY); }
static void ringUnlock() { if (ringMutex) xSemaphoreGive(ringMutex); }
static uint8_t audioRingBuf[AUDIO_RING];
static uint8_t videoRingBuf[VIDEO_RING];
struct ByteRing { uint8_t* buf; uint32_t mask; volatile uint32_t head, tail; };
static ByteRing audioRing = { audioRingBuf, AUDIO_RING - 1, 0, 0 };
static ByteRing videoRing = { videoRingBuf, VIDEO_RING - 1, 0, 0 };
static inline uint32_t rUsed(ByteRing& r) { return r.head - r.tail; }
static inline uint32_t rFree(ByteRing& r) { return (r.mask + 1) - (r.head - r.tail); }
static void rReset(ByteRing& r) { r.tail = r.head = 0; }
static void ringGetAt(ByteRing& r, uint32_t pos, uint8_t* dst, uint32_t n) {
    uint32_t off = pos & r.mask, first = (r.mask + 1) - off; if (first > n) first = n;
    memcpy(dst, r.buf + off, first);
    if (n > first) memcpy(dst + first, r.buf, n - first);
}
static void ringPutAt(ByteRing& r, uint32_t pos, const uint8_t* src, uint32_t n) {
    uint32_t off = pos & r.mask, first = (r.mask + 1) - off; if (first > n) first = n;
    memcpy(r.buf + off, src, first);
    if (n > first) memcpy(r.buf, src + first, n - first);
}
static uint32_t ringFileAt(ByteRing& r, uint32_t pos, File& f, uint32_t n) {
    uint32_t off = pos & r.mask, first = (r.mask + 1) - off; if (first > n) first = n;
    uint32_t got = 0;
    int a = f.read(r.buf + off, first); if (a > 0) got += (uint32_t)a;
    if (a == (int)first && n > first) { int b = f.read(r.buf, n - first); if (b > 0) got += (uint32_t)b; }
    return got;
}

static uint32_t vidFcc = 0, audFcc = 0;
static void makeFccs(const AviInfo &info) {
    char v[5], a[5];
    snprintf(v, sizeof(v), "%02ddc", info.vidIdx);
    snprintf(a, sizeof(a), "%02dwb", info.audIdx);
    vidFcc = FCC(v); audFcc = FCC(a);
}

static uint32_t aviResync(File &f, uint32_t from, uint32_t end) {
    static uint8_t blk[2048 + 4];
    uint32_t pos = from;
    while (pos + 8 < end) {
        if (!f.seek(pos)) break;
        int n = f.read(blk, sizeof(blk));
        if (n < 8) break;
        for (int i = 0; i + 8 <= n; i++) {
            uint32_t id = le32(blk + i);
            if (id == vidFcc || id == audFcc) {
                uint32_t sz = le32(blk + i + 4);
                if (sz < 1000000 && pos + i + 8 + sz <= end) return pos + i;
            }
        }
        pos += n - 4;
    }
    return end;
}

class AudioFileSourceRing : public AudioFileSource {
public:
    uint32_t read(void* data, uint32_t len) override {
        uint8_t* p = (uint8_t*)data; uint32_t got = 0, waited = 0; int guard = 0;
        while (got < len) {
            ringLock();
            uint32_t avail = rUsed(audioRing);
            if (avail) {
                uint32_t n = len - got; if (n > avail) n = avail;
                ringGetAt(audioRing, audioRing.tail, p + got, n);
                audioRing.tail += n; got += n;
            }
            ringUnlock();
            if (avail) continue;
            if (got > 0 || !readerActive) break;
            uint32_t t0 = millis();
            vTaskDelay(1);
            waited += millis() - t0;
            if (++guard > 3000) break;
        }
        if (waited > statAudioWaitCur) statAudioWaitCur = waited;
        return got;
    }
    bool seek(int32_t, int) override { return false; }
    bool close() override { return true; }
    bool isOpen() override { return true; }
    uint32_t getSize() override { return 0x7FFFFFFF; }
    uint32_t getPos() override { return 0; }
};

static const uint32_t VBUF_SIZE = 40960;
static uint8_t  vbuf[VBUF_SIZE];
static File     vfile;
static uint32_t vMoviStart = 0, vMoviEnd = 0, vTotalFrames = 0, vFrameDurUs = 50000;
static volatile bool videoActive = false, videoIdle = true;
static volatile bool readerIdle = true;
static volatile bool videoSeekReq = false;
static volatile int  frameStepReq = 0;
static volatile int  paintOneReq = 0;
static void drawPauseGlyph();
static volatile uint32_t videoSeekPos = 0;
static volatile uint32_t readerPos = 0;
static uint32_t frameIdx = 0, frameBase = 0, samplesBase = 0, readerFrameIdx = 0;
static int64_t  timeBaseMs = 0;
static bool videoOn = true, halfRate = false, debugOn = false, muted = false, repeatOne = false;
static int  savedVolume = 50;
static unsigned long infoUntil = 0;
static volatile uint32_t statDrawn = 0, statDropped = 0, statDecodeMs = 0, statFps = 0, statAudioWaitMs = 0;
static unsigned long statWindow = 0;

static int64_t audioClockMs() {
    uint32_t r = out ? out->rate() : 0;
    if (r == 0) r = 44100;
    return timeBaseMs + (int64_t)(samplesOut - samplesBase) * 1000 / r;
}
static int64_t frameDueMs(uint32_t idx) { return timeBaseMs + ((int64_t)idx - (int64_t)frameBase) * vFrameDurUs / 1000 + cfgAvOffsetMs; }

static int progressPercent() {
    if (vMoviEnd <= vMoviStart) return 0;
    uint64_t span = vMoviEnd - vMoviStart;
    uint32_t denom = vTotalFrames ? vTotalFrames : 1;
    uint32_t cur = vMoviStart + (uint32_t)(span * frameIdx / denom);
    if (cur > vMoviEnd) cur = vMoviEnd;
    return (int)((uint64_t)(cur - vMoviStart) * 100 / span);
}

static void drawOverlays() {
    dispLock();
    auto &d = M5Cardputer.Display;
    d.setFont(&fonts::Font0);
    int y = SCR_H - 10;
    char line[160];
    if (millis() < infoUntil) {
        snprintf(line, sizeof(line), "%s  %d%%", npTitle, progressPercent());
        d.fillRect(0, y - 1, SCR_W, 10, TFT_BLACK);
        d.setTextColor(TFT_WHITE);
        d.setCursor(2, y); d.print(line);
        y -= 10;
    }
    if (debugOn) {
        snprintf(line, sizeof(line), "fps %lu drop %lu dec %lums aw %lums vq %lu%s%s",
                 (unsigned long)statFps, (unsigned long)statDropped, (unsigned long)statDecodeMs,
                 (unsigned long)statAudioWaitMs, (unsigned long)rUsed(videoRing),
                 halfRate ? " half" : "", videoOn ? "" : " novid");
        d.fillRect(0, y - 1, SCR_W, 10, TFT_BLACK);
        d.setTextColor(TFT_WHITE);
        d.setCursor(2, y); d.print(line);
    }
    d.setFont(FONT);
    dispUnlock();
}

static volatile bool audioRunning = false, audioTaskIdle = true;
static void audioTask(void*) {
    for (;;) {
        if (audioRunning && decoder && decoder->isRunning() && playState == PLAYING) {
            audioTaskIdle = false;
            if (!decoder->loop()) audioRunning = false;
        } else {
            audioTaskIdle = true;
            vTaskDelay(1);
        }
    }
}

static bool pendHave = false;
static uint32_t pendId = 0, pendSz = 0, pendPayload = 0;

static void doSeekLocked() {
    sdLock();
    uint32_t p = aviResync(vfile, videoSeekPos, vMoviEnd);
    vfile.seek(p);
    sdUnlock();
    ringLock();
    rReset(audioRing); rReset(videoRing);
    ringUnlock();
    pendHave = false;
    uint32_t est = (vMoviEnd > vMoviStart && p > vMoviStart) ? (uint32_t)((uint64_t)vTotalFrames * (p - vMoviStart) / (vMoviEnd - vMoviStart)) : 0;
    frameIdx = est; frameBase = est; readerFrameIdx = est;
    samplesBase = samplesOut;
    timeBaseMs = (int64_t)est * vFrameDurUs / 1000;
    readerPos = p;
}

static void skipPending() {
    sdLock();
    vfile.seek(pendPayload + pendSz + (pendSz & 1));
    sdUnlock();
    pendHave = false;
}

static void readerTask(void*) {
    uint32_t yieldAt = 0;
    for (;;) {
        if (!readerActive) { readerIdle = true; vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        readerIdle = false;
        if (millis() - yieldAt >= 20) { vTaskDelay(1); yieldAt = millis(); }
        if (videoSeekReq) { doSeekLocked(); videoSeekReq = false; continue; }
        if (!pendHave) {
            sdLock();
            uint32_t hpos = vfile.position();
            uint8_t h[8];
            if (hpos + 8 > vMoviEnd || vfile.read(h, 8) != 8) { sdUnlock(); readerActive = false; continue; }
            uint32_t id = le32(h), sz = le32(h + 4);
            if (id == FCC("LIST")) { vfile.seek(hpos + 12); sdUnlock(); continue; }
            sdUnlock();
            pendId = id; pendSz = sz; pendPayload = hpos + 8; pendHave = true;
        }
        uint32_t sz = pendSz;
        if (pendId == audFcc) {
            if (sz > AUDIO_RING) { skipPending(); continue; }
            if (rFree(audioRing) < sz) { vTaskDelay(2); continue; }
            sdLock();
            uint32_t base = audioRing.head;
            ringFileAt(audioRing, base, vfile, sz);
            vfile.seek(pendPayload + sz + (sz & 1));
            sdUnlock();
            audioRing.head = base + sz;
            pendHave = false;
        } else if (pendId == vidFcc) {
            if (sz == 0 || sz > VBUF_SIZE || sz + 8 > VIDEO_RING) { skipPending(); readerFrameIdx++; continue; }
            if (rFree(videoRing) < sz + 8) {
                if (rUsed(audioRing) < AUDIO_LOW) { skipPending(); readerFrameIdx++; statDropped++; continue; }
                vTaskDelay(2); continue;
            }
            uint32_t hdr[2] = { sz, readerFrameIdx };
            sdLock();
            uint32_t base = videoRing.head;
            ringPutAt(videoRing, base, (const uint8_t*)hdr, 8);
            ringFileAt(videoRing, base + 8, vfile, sz);
            vfile.seek(pendPayload + sz + (sz & 1));
            sdUnlock();
            videoRing.head = base + 8 + sz;
            readerFrameIdx++;
            pendHave = false;
        } else {
            skipPending();
        }
    }
}

static bool videoPeekIdx(uint32_t &idx) {
    ringLock();
    bool ok = rUsed(videoRing) >= 8;
    if (ok) { uint32_t hdr[2]; ringGetAt(videoRing, videoRing.tail, (uint8_t*)hdr, 8); idx = hdr[1]; }
    ringUnlock();
    return ok;
}

static bool videoPop(uint32_t &outSz, uint32_t &outIdx) {
    ringLock();
    if (rUsed(videoRing) < 8) { ringUnlock(); return false; }
    uint32_t hdr[2];
    ringGetAt(videoRing, videoRing.tail, (uint8_t*)hdr, 8);
    uint32_t len = hdr[0];
    if (len > VBUF_SIZE || rUsed(videoRing) < 8 + len) { ringUnlock(); return false; }
    ringGetAt(videoRing, videoRing.tail + 8, vbuf, len);
    videoRing.tail += 8 + len;
    ringUnlock();
    outSz = len; outIdx = hdr[1];
    return true;
}

static bool drawFrame(uint32_t sz, bool paused) {
    dispLock();
    bool drew = videoDrawEnabled;
    if (drew) {
        uint32_t t0 = millis();
        M5Cardputer.Display.drawJpg(vbuf, sz, 0, 0);
        statDecodeMs = millis() - t0;
        if (paused) drawPauseGlyph();
        if (debugOn || millis() < infoUntil) drawOverlays();
    }
    dispUnlock();
    return drew;
}

static void videoTask(void*) {
    for (;;) {
        if (!videoActive) { videoIdle = true; vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        videoIdle = false;
        if (videoSeekReq) { vTaskDelay(2); continue; }
        if (playState == PAUSED) {
            if (paintOneReq) {
                uint32_t sz = 0, idx = 0;
                if (videoPop(sz, idx)) {
                    paintOneReq = 0;
                    frameIdx = idx + 1;
                    if (sz) drawFrame(sz, true);
                } else vTaskDelay(2);
            } else vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        uint32_t idx = 0;
        if (!videoPeekIdx(idx)) {
            if (!readerActive) videoActive = false;
            else vTaskDelay(1);
            continue;
        }
        int64_t now = audioClockMs(), due = frameDueMs(idx);
        if (now < due) { vTaskDelay(1); continue; }
        uint32_t sz = 0;
        if (!videoPop(sz, idx)) { vTaskDelay(1); continue; }
        frameIdx = idx + 1;
        bool late = now > due + (int64_t)vFrameDurUs / 1000;
        bool skipHalf = halfRate && (idx & 1);
        if (!late && !skipHalf && videoDrawEnabled && sz > 0) { if (drawFrame(sz, false)) statDrawn++; }
        else if (late && videoDrawEnabled) statDropped++;
        unsigned long nowMs = millis();
        if (nowMs - statWindow >= 1000) {
            statWindow = nowMs;
            statFps = statDrawn; statDrawn = 0;
            statAudioWaitMs = statAudioWaitCur; statAudioWaitCur = 0;
        }
        vTaskDelay(1);
    }
}

static void videoStop() {
    videoActive = false;
    readerActive = false;
    for (int i = 0; i < 200 && (!videoIdle || !readerIdle); i++) delay(2);
    if (vfile) vfile.close();
}

static void stopPlayback() {
    audioRunning = false;
    for (int i = 0; i < 100 && !audioTaskIdle; i++) delay(2);
    videoStop();
    if (decoder) { if (decoder->isRunning()) decoder->stop(); delete decoder; decoder = nullptr; }
    if (file) { file->close(); delete file; file = nullptr; }
    M5Cardputer.Speaker.stop();
}

static void marqueesOff();
static void updateVideoDraw() { videoDrawEnabled = screenVisible() && fullscreen && videoOn; }
static void drawPauseGlyph();
static void stepFrame(int dir);
static void enterFullscreen() {
    dispLock();
    fullscreen = true;
    if (screenVisible()) {
        marqueesOff();
        M5Cardputer.Display.fillScreen(TFT_BLACK);
        if (playState == PAUSED) { drawPauseGlyph(); if (debugOn || millis() < infoUntil) drawOverlays(); }
    }
    updateVideoDraw();
    dispUnlock();
}
static void leaveFullscreen() {
    if (!fullscreen) return;
    dispLock();
    fullscreen = false;
    updateVideoDraw();
    dispUnlock();
    needsFullRedraw = true;
}
static void exitFullscreen() { leaveFullscreen(); }

static String trimToWidth(LovyanGFX &d, const char* text, int maxW);
static void playQueuePos(int pos) {
    if (queueCount == 0) return;
    if (pos < 0) pos = queueCount - 1;
    if (pos >= queueCount) pos = 0;
    queuePos = pos;

    const char* full = queueName(pos);

    stopPlayback();
    AviInfo info;
    bool ok = aviParse(full, info, curTitle, sizeof(curTitle), curArtist, sizeof(curArtist), curAlbum, sizeof(curAlbum));
    Serial.printf("open %s ok=%d movi=%lu..%lu fps=%lu/%lu frames=%lu audio=0x%x\n", full, ok ? 1 : 0,
                  (unsigned long)info.moviStart, (unsigned long)info.moviEnd, (unsigned long)info.vRate, (unsigned long)info.vScale,
                  (unsigned long)info.totalFrames, info.audioTag);
    const char* err = "";
    if (!ok) err = "not a frost avi";
    if (ok && info.audioTag != 0x55) { ok = false; err = "audio not mp3"; }
    if (ok) { makeFccs(info); vfile = SD.open(full, FILE_READ); if (!vfile) { ok = false; err = "open failed"; } }
    if (ok) {
        strncpy(nowPlaying, baseName(full), sizeof(nowPlaying) - 1);
        nowPlaying[sizeof(nowPlaying) - 1] = '\0';
        npTitle[0] = npArtist[0] = npAlbum[0] = '\0';
        indexLookup(full, npTitle, sizeof(npTitle), npArtist, sizeof(npArtist), npAlbum, sizeof(npAlbum));
        if (!npTitle[0]) {
            if (curTitle[0]) takeField(npTitle, sizeof(npTitle), curTitle);
            else stripExt(nowPlaying, npTitle, sizeof(npTitle));
        }
        if (!npArtist[0]) takeField(npArtist, sizeof(npArtist), curArtist);
        if (!npAlbum[0])  takeField(npAlbum,  sizeof(npAlbum),  curAlbum);
        buildLine2();

        vMoviStart = info.moviStart; vMoviEnd = info.moviEnd; vTotalFrames = info.totalFrames;
        vFrameDurUs = (uint32_t)((uint64_t)1000000 * info.vScale / info.vRate);
        vfile.seek(vMoviStart);
        ringLock(); rReset(audioRing); rReset(videoRing); ringUnlock();
        pendHave = false;
        frameIdx = 0; frameBase = 0; readerFrameIdx = 0; samplesBase = samplesOut; timeBaseMs = 0;
        videoSeekReq = false; frameStepReq = 0; paintOneReq = 0;
        statDropped = 0; statDrawn = 0;

        readerActive = true;
        uint32_t t0 = millis();
        while (rUsed(audioRing) < AUDIO_PREFILL && millis() - t0 < 1500) delay(5);

        file = new AudioFileSourceRing();
        decoder = new AudioGeneratorMP3(mp3Space, sizeof(mp3Space));
        ok = decoder->begin(file, out);
        Serial.printf("begin=%d heap=%lu\n", ok ? 1 : 0, (unsigned long)ESP.getFreeHeap());
        if (!ok) err = "no memory";
        if (ok) {
            playState = PLAYING;
            audioRunning = true;
            enterFullscreen();
            videoActive = true;
        }
    }
    if (!ok) {
        stopPlayback(); playState = STOPPED; nowPlaying[0] = '\0'; npTitle[0] = npLine2[0] = '\0';
        exitFullscreen();
        if (screenVisible()) {
            auto &d = M5Cardputer.Display;
            d.setFont(FONT); d.setTextColor(COL_ACCENT, COL_BG);
            d.fillRect(LC_X, LC_Y + LC_H - ROW_H, LC_W, ROW_H, COL_BG);
            d.setCursor(LC_X, LC_Y + LC_H - ROW_H + 1);
            char msg[40];
            snprintf(msg, sizeof(msg), "x %s", err);
            d.print(trimToWidth(d, msg, LC_W));
        }
    }
    redrawNowPlaying = true; redrawQueue = true;
}

static int shuffleOrder[QUEUE_MAX];
static int shufflePos = 0;
static void buildShuffle(int firstQueueIdx) {
    for (int i = 0; i < queueCount; i++) shuffleOrder[i] = i;
    for (int i = queueCount - 1; i > 0; i--) {
        int j = esp_random() % (i + 1);
        int t = shuffleOrder[i]; shuffleOrder[i] = shuffleOrder[j]; shuffleOrder[j] = t;
    }
    for (int i = 0; i < queueCount; i++) {
        if (shuffleOrder[i] == firstQueueIdx) { int t = shuffleOrder[0]; shuffleOrder[0] = shuffleOrder[i]; shuffleOrder[i] = t; break; }
    }
    shufflePos = 0;
}
static bool hasNextTrack() { return cfgShuffle ? (shufflePos + 1 < queueCount) : (queuePos + 1 < queueCount); }
static void nextTrack() {
    if (!cfgShuffle) { playQueuePos(queuePos + 1); return; }
    if (queueCount == 0) return;
    shufflePos = (shufflePos + 1) % queueCount;
    playQueuePos(shuffleOrder[shufflePos]);
}
static void prevTrack() {
    if (!cfgShuffle) { playQueuePos(queuePos - 1); return; }
    if (queueCount == 0) return;
    shufflePos = (shufflePos - 1 + queueCount) % queueCount;
    playQueuePos(shuffleOrder[shufflePos]);
}
static void toggleShuffle() {
    cfgShuffle = !cfgShuffle;
    if (cfgShuffle && queueCount) buildShuffle(queuePos);
    saveConfig();
    redrawNowPlaying = true;
}

static uint32_t playbackByte() {
    if (vMoviEnd <= vMoviStart) return vMoviStart;
    uint32_t denom = vTotalFrames ? vTotalFrames : 1;
    return vMoviStart + (uint32_t)((uint64_t)(vMoviEnd - vMoviStart) * frameIdx / denom);
}

static const int SEEK_STEP_PCT = 2;
static void seekBy(int pct) {
    if (playState == STOPPED || vMoviEnd <= vMoviStart) return;
    int64_t span = vMoviEnd - vMoviStart;
    int64_t pos = (int64_t)playbackByte() + span * pct / 100;
    if (pos < (int64_t)vMoviStart) pos = vMoviStart;
    if (pos > (int64_t)vMoviEnd - 65536) pos = (int64_t)vMoviEnd - 65536;
    videoSeekPos = (uint32_t)pos;
    videoSeekReq = true;
    if (playState == PAUSED) paintOneReq = 1;
    infoUntil = millis() + 2000;
}

static void seekToPct(int pct) {
    if (playState == STOPPED || vMoviEnd <= vMoviStart) return;
    int64_t pos = (int64_t)vMoviStart + (int64_t)(vMoviEnd - vMoviStart) * pct / 100;
    if (pos > (int64_t)vMoviEnd - 65536) pos = (int64_t)vMoviEnd - 65536;
    if (pos < (int64_t)vMoviStart) pos = vMoviStart;
    videoSeekPos = (uint32_t)pos;
    videoSeekReq = true;
    if (playState == PAUSED) paintOneReq = 1;
    infoUntil = millis() + 2000;
}

static void stepFrame(int dir) {
    if (playState != PAUSED || !fullscreen) return;
    if (dir > 0) { paintOneReq = 1; }
    else {
        uint32_t avg = (vTotalFrames && vMoviEnd > vMoviStart) ? (vMoviEnd - vMoviStart) / vTotalFrames : 8192;
        uint32_t cur = playbackByte();
        uint32_t target = (cur > vMoviStart + avg * 2) ? cur - avg * 2 : vMoviStart;
        videoSeekPos = target; videoSeekReq = true; paintOneReq = 1;
    }
    infoUntil = millis() + 2000;
}

static void drawPauseGlyph() {
    dispLock();
    if (screenVisible() && fullscreen) {
        auto &d = M5Cardputer.Display;
        int x = SCR_W - 22, y = SCR_H - 22;
        d.fillRoundRect(x, y, 18, 18, 3, TFT_BLACK);
        d.fillRect(x + 4, y + 4, 3, 10, TFT_WHITE);
        d.fillRect(x + 11, y + 4, 3, 10, TFT_WHITE);
    }
    dispUnlock();
}

static void togglePause() {
    if (playState == PLAYING) { playState = PAUSED; audioRunning = false; drawPauseGlyph(); }
    else if (playState == PAUSED) { playState = PLAYING; if (decoder && decoder->isRunning()) audioRunning = true; }
    redrawNowPlaying = true;
}

static void toggleMute() {
    if (!muted) { savedVolume = volume; muted = true; M5Cardputer.Speaker.setVolume(0); }
    else { muted = false; M5Cardputer.Speaker.setVolume(volume); }
    redrawNowPlaying = true;
}

static void adjustAvOffset(int d) {
    cfgAvOffsetMs += d;
    if (cfgAvOffsetMs < -1000) cfgAvOffsetMs = -1000;
    if (cfgAvOffsetMs > 1000) cfgAvOffsetMs = 1000;
    saveConfig();
    infoUntil = millis() + 2000;
}

static void showInfo() {
    if (!fullscreen) return;
    infoUntil = millis() + 2000;
    dispLock();
    if (fullscreen && screenVisible() && (playState == PAUSED || !videoDrawEnabled)) drawOverlays();
    dispUnlock();
}

static void toggleDebug() {
    dispLock();
    debugOn = !debugOn;
    if (fullscreen && screenVisible()) {
        if (!debugOn) { M5Cardputer.Display.fillRect(0, SCR_H - 22, SCR_W, 22, TFT_BLACK); if (playState == PAUSED) drawPauseGlyph(); }
        else if (playState == PAUSED) drawOverlays();
    }
    dispUnlock();
}

static void toggleVideo() {
    dispLock();
    videoOn = !videoOn;
    updateVideoDraw();
    if (fullscreen && screenVisible() && !videoOn) { M5Cardputer.Display.fillScreen(TFT_BLACK); if (playState == PAUSED) drawPauseGlyph(); }
    dispUnlock();
}

static void stopToUi() {
    stopPlayback();
    playState = STOPPED;
    nowPlaying[0] = '\0';
    npTitle[0] = npLine2[0] = '\0';
    exitFullscreen();
    redrawNowPlaying = true; redrawQueue = true;
}

static void enterFolder(int viewIdx) {
    if (depth >= MAX_DEPTH - 1) return;
    cursorStack[depth] = cursor; scrollStack[depth] = scroll;
    const char* nm = nameAt(viewIdx);
    char next[MY_PATH_MAX];
    joinPath(next, sizeof(next), currentPath, nm);
    if (strlen(next) >= MY_PATH_MAX - 2) return;
    strcpy(currentPath, next);
    depth++;
    loadDir();
    redrawBrowser = true;
}
static void goBack() {
    if (depth == 0) return;
    char* s = strrchr(currentPath, '/');
    if (s == currentPath) currentPath[1] = '\0';
    else if (s) *s = '\0';
    depth--;
    loadDir();
    cursor = cursorStack[depth]; scroll = scrollStack[depth];
    if (cursor >= entryCount) cursor = entryCount ? entryCount - 1 : 0;
    redrawBrowser = true;
}

static const int QCACHE = 8;
static int  qcIdx[QCACHE];
static char qcTitle[QCACHE][TITLE_MAX];
static int  qcNext = 0;
static void queueCacheReset() { for (int i = 0; i < QCACHE; i++) qcIdx[i] = -1; qcNext = 0; }
static const char* queueTitle(int i) {
    for (int k = 0; k < QCACHE; k++) if (qcIdx[k] == i) return qcTitle[k];
    int slot = qcNext; qcNext = (qcNext + 1) % QCACHE;
    displayTitleFor(queueName(i), qcTitle[slot], TITLE_MAX);
    qcIdx[slot] = i;
    return qcTitle[slot];
}

static void playPath(const char* full) {
    char folder[MY_PATH_MAX];
    strncpy(folder, full, sizeof(folder) - 1); folder[sizeof(folder) - 1] = '\0';
    char* sl = strrchr(folder, '/');
    if (sl == folder) folder[1] = '\0'; else if (sl) *sl = '\0';
    buildQueue(folder);
    queueCacheReset();
    int startPos = 0;
    for (int i = 0; i < queueCount; i++) {
        if (strcmp(queueName(i), full) == 0) { startPos = i; break; }
    }
    if (cfgShuffle) buildShuffle(startPos);
    playQueuePos(startPos);
}

static int queueInsert(const char* full, int at) {
    int len = strlen(full);
    if (queueCount >= QUEUE_MAX || (queuePoolUsed + len + 1) >= QNAME_POOL) return -1;
    bool wasEmpty = (queueCount == 0);
    uint16_t poolOff = queuePoolUsed;
    memcpy(&queuePool[queuePoolUsed], full, len + 1);
    queuePoolUsed += len + 1;
    int newIdx;
    if (!cfgShuffle) {
        if (at < 0 || at > queueCount) at = queueCount;
        for (int i = queueCount; i > at; i--) queueOffset[i] = queueOffset[i - 1];
        queueOffset[at] = poolOff;
        queueCount++;
        if (!wasEmpty && at <= queuePos) queuePos++;
        newIdx = at;
    } else {
        queueOffset[queueCount] = poolOff;
        newIdx = queueCount;
        queueCount++;
        if (wasEmpty) buildShuffle(0);
        else {
            int lo = shufflePos + 1, hi = newIdx;
            int sat = (at < 0) ? lo + (int)(esp_random() % (hi - lo + 1)) : at;
            if (sat < lo) sat = lo; if (sat > hi) sat = hi;
            for (int i = newIdx; i > sat; i--) shuffleOrder[i] = shuffleOrder[i - 1];
            shuffleOrder[sat] = newIdx;
        }
    }
    queueCacheReset();
    redrawQueue = true;
    return newIdx;
}

static int nextSlot(int k) { return cfgShuffle ? shufflePos + 1 + k : queuePos + 1 + k; }

static void startIfStopped(int idx) {
    if (idx < 0 || playState != STOPPED) return;
    if (cfgShuffle) { for (int i = 0; i < queueCount; i++) if (shuffleOrder[i] == idx) { shufflePos = i; break; } }
    playQueuePos(idx);
}

static void queueAppend(const char* full, bool next) {
    int idx = queueInsert(full, next ? nextSlot(0) : -1);
    startIfStopped(idx);
}

static const int FQ_MAX = 256, FQ_POOL = 8192;
static char     fqPool[FQ_POOL];
static uint16_t fqOff[FQ_MAX];
static void queueAppendFolder(const char* folder, bool next) {
    int n = 0, used = 0;
    sdLock();
    DIR* dir = openSdDir(folder);
    if (!dir) { sdUnlock(); return; }
    struct dirent* de;
    while ((de = readdir(dir)) != nullptr && n < FQ_MAX) {
        const char* nm = de->d_name;
        int len = strlen(nm);
        if (nm[0] == '.' || direntIsDir(folder, de) || !isAudioFile(nm) || used + len + 1 >= FQ_POOL) continue;
        fqOff[n++] = used;
        memcpy(&fqPool[used], nm, len + 1);
        used += len + 1;
    }
    closedir(dir);
    sdUnlock();
    for (int i = 1; i < n; i++) {
        uint16_t key = fqOff[i]; int j = i - 1;
        while (j >= 0 && nameCmp(&fqPool[fqOff[j]], &fqPool[key]) > 0) { fqOff[j + 1] = fqOff[j]; j--; }
        fqOff[j + 1] = key;
    }
    int first = -1;
    char full[MY_PATH_MAX];
    for (int i = 0; i < n; i++) {
        joinPath(full, sizeof(full), folder, &fqPool[fqOff[i]]);
        int idx = queueInsert(full, next ? nextSlot(i) : -1);
        if (idx < 0) break;
        if (first < 0) first = idx;
    }
    startIfStopped(first);
}

static int listCount() { return entryCount; }
static const char* listName(int i) { return nameAt(i); }
static bool listIsDir(int i) { return isDirAt(i); }

static void openSelected() {
    if (entryCount == 0) return;
    if (isDirAt(cursor)) { enterFolder(cursor); return; }
    char full[MY_PATH_MAX];
    joinPath(full, sizeof(full), currentPath, nameAt(cursor));
    playPath(full);
}

static void enqueueSelected(bool next) {
    char full[MY_PATH_MAX];
    if (entryCount == 0) return;
    joinPath(full, sizeof(full), currentPath, nameAt(cursor));
    if (isDirAt(cursor)) queueAppendFolder(full, next);
    else queueAppend(full, next);
}

static void drawBrowserRow(int idx);
static void moveCursor(int delta) {
    int count = listCount();
    if (count == 0) return;
    int oldCursor = cursor, oldScroll = scroll;
    cursor += delta;
    if (cursor < 0) cursor = count - 1;
    if (cursor >= count) cursor = 0;
    if (cursor < scroll) scroll = cursor;
    if (cursor >= scroll + BROWSER_ROWS) scroll = cursor - BROWSER_ROWS + 1;
    if (scroll != oldScroll) redrawBrowser = true;
    else if (cursor != oldCursor && screenVisible()) { drawBrowserRow(oldCursor); drawBrowserRow(cursor); }
}

static void changeVolume(int d);
static void seekBy(int pct);

static void drawNowPlayingStatus();
static void changeVolume(int d) {
    volume += d;
    if (volume < 0) volume = 0;
    if (volume > 255) volume = 255;
    muted = false;
    M5Cardputer.Speaker.setVolume(volume);
    redrawNowPlaying = true;
}

static void changeBrightness(int d) {
    cfgBrightness += d;
    if (cfgBrightness < 0) cfgBrightness = 0;
    if (cfgBrightness > 255) cfgBrightness = 255;
    applyBacklight();
    saveConfig();
}

static String trimToWidth(LovyanGFX &d, const char* text, int maxW) {
    String s(text);
    while (s.length() > 0 && d.textWidth(s.c_str()) > maxW) {
        int n = s.length() - 1;
        while (n > 0 && ((uint8_t)s[n] & 0xC0) == 0x80) n--;
        s.remove(n);
    }
    return s;
}

static void drawPaneFrames() {
    auto &d = M5Cardputer.Display;
    d.fillScreen(COL_BG);
    const int pane[3][4] = { {LEFT_X, LEFT_Y, LEFT_W, LEFT_H}, {RIGHT_X, NP_Y, RIGHT_W, NP_H}, {RIGHT_X, Q_Y, RIGHT_W, Q_H} };
    for (int i = 0; i < 3; i++) {
        d.drawRect(pane[i][0], pane[i][1], pane[i][2], pane[i][3], COL_ACCENT);
        d.drawRect(pane[i][0] + 1, pane[i][1] + 1, pane[i][2] - 2, pane[i][3] - 2, COL_ACCENT);
    }
}

static const int MARQUEE_STEP = 1, MARQUEE_INTERVAL_MS = 35, MARQUEE_GAP = 28, MARQUEE_HOLD_MS = 900;
struct Marquee {
    bool active = false, scrolling = false, centered = false;
    int x = 0, y = 0, w = 0;
    uint16_t fg = 0, bg = 0;
    char text[2 * TITLE_MAX + 4] = "";
    int textW = 0, scrollX = 0;
    unsigned long holdUntil = 0, lastMove = 0;
    void (*after)() = nullptr;
};
static Marquee mqTitle, mqArtist, mqBrowser, mqQueue;
static const int MQ_MAX_W = 108;
static M5Canvas mqCanvas(&M5Cardputer.Display);

static void marqueeDraw(Marquee &m) {
    auto &d = M5Cardputer.Display;
    mqCanvas.fillSprite(m.bg);
    mqCanvas.setTextColor(m.fg);
    if (m.scrolling) {
        mqCanvas.setCursor(m.scrollX, 1); mqCanvas.print(m.text);
        mqCanvas.setCursor(m.scrollX + m.textW + MARQUEE_GAP, 1); mqCanvas.print(m.text);
    } else {
        mqCanvas.setCursor(m.centered ? (m.w - m.textW) / 2 : 0, 1); mqCanvas.print(m.text);
    }
    d.setClipRect(m.x, m.y, m.w, ROW_H);
    mqCanvas.pushSprite(m.x, m.y);
    d.clearClipRect();
    if (m.after) m.after();
}

static void marqueeSet(Marquee &m, const char* text, int x, int y, int w, uint16_t fg, uint16_t bg, bool centered, void (*after)()) {
    auto &d = M5Cardputer.Display;
    d.setFont(FONT);
    strncpy(m.text, text, sizeof(m.text) - 1); m.text[sizeof(m.text) - 1] = '\0';
    m.x = x; m.y = y; m.w = w; m.fg = fg; m.bg = bg; m.centered = centered; m.after = after;
    m.textW = d.textWidth(m.text);
    m.scrolling = m.textW > w;
    m.scrollX = 0;
    m.holdUntil = millis() + MARQUEE_HOLD_MS;
    m.lastMove = millis();
    m.active = true;
    marqueeDraw(m);
}

static void marqueeTickOne(Marquee &m, unsigned long now) {
    if (!m.active || !m.scrolling) return;
    if (now < m.holdUntil) return;
    if (now - m.lastMove < MARQUEE_INTERVAL_MS) return;
    m.lastMove = now;
    m.scrollX -= MARQUEE_STEP;
    if (m.scrollX <= -(m.textW + MARQUEE_GAP)) { m.scrollX = 0; m.holdUntil = now + MARQUEE_HOLD_MS; }
    marqueeDraw(m);
}

static void marqueesOff() { mqTitle.active = mqArtist.active = mqBrowser.active = mqQueue.active = false; }

static void marqueeTickAll() {
    unsigned long now = millis();
    marqueeTickOne(mqTitle, now);
    marqueeTickOne(mqArtist, now);
    marqueeTickOne(mqBrowser, now);
    marqueeTickOne(mqQueue, now);
}

static void drawBrowserOutline() {
    int row = cursor - scroll;
    if (row < 0 || row >= BROWSER_ROWS) return;
    M5Cardputer.Display.drawRoundRect(LC_X, LC_Y + row * ROW_H, LC_W, ROW_H, 3, COL_ACCENT);
}

static void drawBrowserRow(int idx) {
    auto &d = M5Cardputer.Display;
    int row = idx - scroll;
    if (row < 0 || row >= BROWSER_ROWS) return;
    int y = LC_Y + row * ROW_H;
    d.setFont(FONT);
    d.fillRect(LC_X, y, LC_W, ROW_H, COL_BG);
    if (idx >= listCount()) return;
    uint16_t fg = listIsDir(idx) ? COL_ACCENT : COL_TEXT;
    if (idx == cursor) {
        marqueeSet(mqBrowser, listName(idx), LC_X + 3, y, LC_W - 6, fg, COL_BG, false, drawBrowserOutline);
    } else {
        d.setTextColor(fg);
        d.setCursor(LC_X + 3, y + 1);
        d.print(trimToWidth(d, listName(idx), LC_W - 6));
    }
}

static void drawFolderHelp(bool missing) {
    auto &d = M5Cardputer.Display;
    d.setFont(&fonts::TomThumb);
    d.setTextDatum(textdatum_t::top_left);
    const int LH = 7, DX = 58;
    int y = LC_Y + 1;
    auto text = [&](const char* str, uint16_t c) { d.setTextColor(c); d.drawString(str, LC_X, y); y += LH; };
    char buf[64];
    if (missing) {
        text("folder not found:", COL_ACCENT);
        text(trimToWidth(d, cfgMusicDir, LC_W).c_str(), COL_TEXT);
        text("create it, or change", COL_TEXT);
        text("movie_dir in the file", COL_TEXT);
        text("below, then restart", COL_TEXT);
    } else {
        snprintf(buf, sizeof(buf), "no videos in %s", cfgMusicDir);
        text(trimToWidth(d, buf, LC_W).c_str(), COL_ACCENT);
        text("convert videos on a PC", COL_TEXT);
        text("with frost-convert.py,", COL_TEXT);
        text("copy the .avi files here,", COL_TEXT);
        text("restart, press Fn+S", COL_TEXT);
    }
    y += LH;
    text("settings: /.frost/config", COL_ACCENT);
    static const char* const keys[8] = { "movie_dir", "brightness", "screen_timeout", "accent", "background", "shuffle", "av_offset", "boot_animation" };
    static const char* const desc[8] = { "video folder", "0-255", "ms, 0=off", "hex color", "hex color", "0 or 1", "sync, ms", "0 or 1" };
    for (int i = 0; i < 8; i++) {
        d.setTextColor(COL_TEXT); d.drawString(keys[i], LC_X, y);
        d.setTextColor(COL_DIM);  d.drawString(desc[i], LC_X + DX, y);
        y += LH;
    }
    d.setFont(FONT);
}

static void drawBrowser() {
    auto &d = M5Cardputer.Display;
    d.setFont(FONT);
    d.fillRect(LC_X, LC_Y, LC_W, LC_H, COL_BG);
    mqBrowser.active = false;
    if (!rootOk || (entryCount == 0 && depth == 0)) {
        drawFolderHelp(!rootOk);
        return;
    }
    if (entryCount == 0) {
        d.setTextColor(COL_DIM);
        d.setCursor(LC_X, LC_Y + 1); d.print("(empty)");
        return;
    }
    for (int r = 0; r < BROWSER_ROWS; r++) drawBrowserRow(scroll + r);
}

static const int NP_LINE1_Y = NC_Y, NP_LINE2_Y = NC_Y + ROW_H, NP_STATUS_Y = NC_Y + 2 * ROW_H + 2;

static void drawNowPlayingStatus() {
    auto &d = M5Cardputer.Display;
    d.setFont(FONT);
    int y = NP_STATUS_Y;
    d.fillRect(NC_X, y, NC_W, NC_H - (y - NC_Y), COL_BG);
    if (playState == STOPPED) {
        d.setTextColor(COL_DIM);
        d.setCursor(NC_X, y + 1); d.print("stopped");
        return;
    }
    int gx = NC_X, gy = y + 1;
    if (playState == PLAYING) d.fillTriangle(gx, gy, gx, gy + 10, gx + 8, gy + 5, COL_ACCENT);
    else { d.fillRect(gx, gy, 3, 11, COL_ACCENT); d.fillRect(gx + 5, gy, 3, 11, COL_ACCENT); }
    d.setTextColor(COL_TEXT);
    d.setCursor(NC_X + 14, y + 1);
    d.printf("%d%%", progressPercent());
    char vol[32];
    snprintf(vol, sizeof(vol), "%s%s%s%d", cfgShuffle ? "shuf " : "", repeatOne ? "rpt " : "", muted ? "mute " : "vol ", muted ? 0 : volume * 100 / 255);
    d.setTextColor(COL_DIM);
    d.setCursor(NC_X + NC_W - d.textWidth(vol), y + 1);
    d.print(vol);
}

static void drawNowPlaying() {
    auto &d = M5Cardputer.Display;
    d.setFont(FONT);
    d.fillRect(NC_X, NC_Y, NC_W, NC_H, COL_BG);
    if (playState == STOPPED) {
        mqTitle.active = mqArtist.active = false;
        d.setTextColor(COL_DIM);
        d.setCursor(NC_X + (NC_W - d.textWidth("frost")) / 2, NP_LINE1_Y + 1); d.print("frost");
        drawNowPlayingStatus();
        return;
    }
    marqueeSet(mqTitle,  npTitle, NC_X, NP_LINE1_Y, NC_W, COL_TEXT, COL_BG, true, nullptr);
    marqueeSet(mqArtist, npLine2, NC_X, NP_LINE2_Y, NC_W, COL_DIM,  COL_BG, true, nullptr);
    drawNowPlayingStatus();
}

static void drawQueue() {
    auto &d = M5Cardputer.Display;
    d.setFont(FONT);
    d.fillRect(QC_X, QC_Y, QC_W, QC_H, COL_BG);
    mqQueue.active = false;
    if (queueCount == 0) {
        d.setTextColor(COL_DIM);
        d.setCursor(QC_X, QC_Y + 1); d.print("queue empty");
        return;
    }
    int start = queuePos - QUEUE_ROWS / 2;
    if (start > queueCount - QUEUE_ROWS) start = queueCount - QUEUE_ROWS;
    if (start < 0) start = 0;
    for (int r = 0; r < QUEUE_ROWS; r++) {
        int i = start + r;
        if (i >= queueCount) break;
        int y = QC_Y + r * ROW_H;
        if (i == queuePos) {
            d.fillRoundRect(QC_X, y, QC_W, ROW_H, 3, COL_ACCENT);
            marqueeSet(mqQueue, queueTitle(i), QC_X + 3, y, QC_W - 6, COL_BG, COL_ACCENT, false, nullptr);
        } else {
            d.setTextColor(COL_TEXT);
            d.setCursor(QC_X + 3, y + 1);
            d.print(trimToWidth(d, queueTitle(i), QC_W - 6));
        }
    }
}

static void drawAll() {
    drawPaneFrames();
    drawBrowser();
    drawNowPlaying();
    drawQueue();
}

struct FlakePx { int8_t x, y; uint16_t t; };

static uint32_t fg_s = 1;
static uint32_t fg_u32() { fg_s ^= fg_s << 13; fg_s ^= fg_s >> 17; fg_s ^= fg_s << 5; return fg_s; }
static double fg_f() { return fg_u32() / 4294967296.0; }
static double fg_rng(double a, double b) { return a + (b - a) * fg_f(); }
static int fg_ri(int a, int b) { return a + (int)(fg_u32() % (uint32_t)(b - a + 1)); }
static int fg_rnd(double v) { double r = floor(fabs(v) + 0.5 + 1e-6); return v > 0 ? (int)r : (v < 0 ? -(int)r : 0); }

static const int FG_G = 71, FG_O = 35;
static const uint16_t FG_NONE = 0xFFFF;
static uint16_t* fg_cur = nullptr;
static void fg_put(int x, int y, double d) {
    int ix = x + FG_O, iy = y + FG_O;
    if (ix < 0 || iy < 0 || ix >= FG_G || iy >= FG_G) return;
    long q = (long)(d * 64 + 0.5);
    if (q > 0xFFFE) q = 0xFFFE;
    uint16_t &c = fg_cur[iy * FG_G + ix];
    if ((uint16_t)q < c) c = (uint16_t)q;
}
static void fg_line(int x0, int y0, int x1, int y1, double base) {
    int sx0 = x0, sy0 = y0;
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        fg_put(x0, y0, base + hypot((double)(x0 - sx0), (double)(y0 - sy0)));
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static int flakeGen(uint32_t seed, FlakePx* out, int maxOut, uint16_t* scratch) {
    fg_s = seed ? seed : 1;
    const double D2R = M_PI / 180.0, R = 26.0, A = 60.0 * D2R;
    double segs[48][5];
    int ns = 0;
    auto addSeg = [&](double x0, double y0, double x1, double y1, double b) {
        if (ns < 48) { segs[ns][0] = x0; segs[ns][1] = y0; segs[ns][2] = x1; segs[ns][3] = y1; segs[ns][4] = b; ns++; }
    };
    addSeg(0, 0, 0, R, 0);
    int c = fg_ri(3, 6);
    bool hexfill = fg_f() < 0.35;
    bool inner = fg_f() < 0.5;
    int n = fg_ri(2, 4);
    double us[4];
    int nu = 0, tries = 0;
    while (nu < n && tries < 50) {
        tries++;
        double u = fg_rng(0.28, 0.86) * R;
        bool ok = true;
        for (int i = 0; i < nu; i++) if (fabs(u - us[i]) < 5.5) ok = false;
        if (ok && u > c + 3) us[nu++] = u;
    }
    for (int i = 1; i < nu; i++) { double k = us[i]; int j = i - 1; while (j >= 0 && us[j] > k) { us[j + 1] = us[j]; j--; } us[j + 1] = k; }
    for (int i = 0; i < nu; i++) {
        double u = us[i], room = R - u;
        double L = fmax(2.5, fmin(fmin(fg_rng(0.45, 0.95) * room, 0.42 * R), 11.0));
        if (i == 0) L = fmax(L, fmin(0.36 * R, room * 0.9));
        addSeg(0, u, sin(A) * L, u + cos(A) * L, u);
        if (L >= 7 && fg_f() < 0.5) {
            double t = L * 0.55, px = sin(A) * t, py = u + cos(A) * t;
            double tl = fg_rng(1.6, fmin(3.2, L - t + 1.2));
            addSeg(px, py, px, py + tl, u + t);
        }
    }
    int tip = fg_ri(0, 2);
    if (tip == 1) addSeg(0, R - 2.5, sin(A) * 2.5, R - 2.5 + cos(A) * 2.5, R - 2.5);
    else if (tip == 2) {
        double h = 2.2, P[6][2];
        for (int k = 0; k < 6; k++) { double a = (90 + 60 * k) * D2R; P[k][0] = h * cos(a); P[k][1] = R + h * sin(a); }
        for (int k = 0; k < 6; k++) addSeg(P[k][0], P[k][1], P[(k + 1) % 6][0], P[(k + 1) % 6][1], R - h);
    }
    double hx[2]; bool hf[2]; int nh = 0;
    hx[nh] = c; hf[nh] = hexfill; nh++;
    if (inner && !hexfill && c >= 4) { hx[nh] = fmax(1.5, c - 2.2); hf[nh] = false; nh++; }
    double armOff[6], armMs[6];
    for (int k = 0; k < 6; k++) { armOff[k] = fg_f() * 500; armMs[k] = 32 + fg_f() * 20; }

    uint16_t* pd = scratch;
    uint16_t* hd = scratch + FG_G * FG_G;
    for (int i = 0; i < FG_G * FG_G; i++) { pd[i] = FG_NONE; hd[i] = FG_NONE; }

    fg_cur = pd;
    for (int k = 0; k < 6; k++) {
        double a = (90 + 60 * k) * D2R, ca = cos(a), sa = sin(a);
        for (int i = 0; i < ns; i++) {
            for (int m = 1; m >= -1; m -= 2) {
                double p0 = m * segs[i][0], u0 = segs[i][1], p1 = m * segs[i][2], u1 = segs[i][3];
                double X0 = u0 * ca + p0 * sin(a), Y0 = -(u0 * sa - p0 * cos(a));
                double X1 = u1 * ca + p1 * sin(a), Y1 = -(u1 * sa - p1 * cos(a));
                fg_line(fg_rnd(X0), fg_rnd(Y0), fg_rnd(X1), fg_rnd(Y1), segs[i][4]);
            }
        }
    }
    fg_cur = hd;
    for (int h = 0; h < nh; h++) {
        double rad = hx[h], PX[6], PY[6];
        for (int k = 0; k < 6; k++) { double a = (90 + 60 * k) * D2R; PX[k] = rad * cos(a); PY[k] = -rad * sin(a); }
        for (int k = 0; k < 6; k++) {
            int x0 = fg_rnd(PX[k]), y0 = fg_rnd(PY[k]);
            fg_line(x0, y0, fg_rnd(PX[(k + 1) % 6]), fg_rnd(PY[(k + 1) % 6]), hypot((double)x0, (double)y0));
        }
        if (hf[h]) {
            int r = fg_rnd(rad);
            for (int y = -r; y <= r; y++)
                for (int x = -r; x <= r; x++)
                    if (fabs((double)y) <= rad * 0.866 && fabs((double)x) + fabs((double)y) * 0.577 <= rad * 0.95)
                        fg_put(x, y, hypot((double)x, (double)y));
        }
    }

    int cnt = 0;
    for (int y = -FG_O; y <= 0; y++) {
        for (int x = 0; x <= FG_O; x++) {
            int idx = (y + FG_O) * FG_G + (x + FG_O);
            uint16_t qa = pd[idx], qh = hd[idx];
            if (qa == FG_NONE && qh == FG_NONE) continue;
            double da = qa / 64.0, dh = qh / 64.0;
            for (int sxm = 1; sxm >= -1; sxm -= 2) {
                for (int sym = 1; sym >= -1; sym -= 2) {
                    if ((sxm < 0 && x == 0) || (sym < 0 && y == 0)) continue;
                    int X = sxm * x, Y = sym * y;
                    double phi = atan2((double)-Y, (double)X) / D2R;
                    int arm = (((int)floor((phi - 90) / 60 + 0.5)) % 6 + 6) % 6;
                    double t = 1e30;
                    if (qa != FG_NONE) t = fmin(t, 60 + armOff[arm] + da * armMs[arm]);
                    if (qh != FG_NONE) t = fmin(t, 60 + dh * 42);
                    if (cnt < maxOut) { out[cnt].x = (int8_t)X; out[cnt].y = (int8_t)Y; out[cnt].t = (uint16_t)ceil(t); cnt++; }
                }
            }
        }
    }
    return cnt;
}
static const int FLAKEGEN_END = 0;

static const char* const SPL_FROST[5][9] = {
    { "..###", ".#...", ".#...", ".#...", "####.", ".#...", ".#...", ".#...", ".#..." },
    { ".....", ".....", ".....", ".....", "#.##.", "##..#", "#....", "#....", "#...." },
    { ".....", ".....", ".....", ".....", ".###.", "#...#", "#...#", "#...#", ".###." },
    { ".....", ".....", ".....", ".....", ".####", "#....", ".###.", "....#", "####." },
    { ".....", ".....", ".#...", ".#...", "####.", ".#...", ".#...", ".#..#", "..##." },
};
struct SplSubGlyph { char ch; const char* rows[8]; };
static const SplSubGlyph SPL_SUB[11] = {
    { 'v', { "...", "...", "#.#", "#.#", "#.#", ".#.", "...", "..." } },
    { 'i', { "#", ".", "#", "#", "#", "#", ".", "." } },
    { 'd', { "..#", "..#", ".##", "#.#", "#.#", ".##", "...", "..." } },
    { 'e', { "...", "...", ".#.", "###", "#..", ".##", "...", "..." } },
    { 'o', { "...", "...", ".#.", "#.#", "#.#", ".#.", "...", "..." } },
    { ' ', { "..", "..", "..", "..", "..", "..", "..", ".." } },
    { 'p', { "...", "...", "##.", "#.#", "#.#", "##.", "#..", "#.." } },
    { 'l', { "#", "#", "#", "#", "#", "#", ".", "." } },
    { 'a', { "...", "...", "##.", ".##", "#.#", ".##", "...", "..." } },
    { 'y', { "...", "...", "#.#", "#.#", "#.#", ".##", "..#", "##." } },
    { 'r', { "...", "...", "#.#", "##.", "#..", "#..", "...", "..." } },
};
static const char* const SPL_SUB_TEXT = "video player";
static const int8_t SPL_SUB_X[12] = { 0, 5, 8, 14, 19, 25, 29, 35, 38, 44, 49, 55 };

static const SplSubGlyph* splSubFind(char ch) {
    for (int i = 0; i < 11; i++) if (SPL_SUB[i].ch == ch) return &SPL_SUB[i];
    return nullptr;
}
static float splClamp(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
static uint16_t splFade(float a) { return rgb565From24(blend24(cfgBackgroundRGB, cfgAccentRGB, (int)(a * 255 + 0.5f))); }

static void bootSplash() {
    const int CX0 = 120, CX1 = 86, CY = 67, TX = 125, TY = 53, SY = 76;
    const int OX = 52, OY = 32, SW = 136, SH = 72, STRIP = 8;
    auto &d = M5Cardputer.Display;
    static_assert(sizeof(FlakePx) == 4, "FlakePx layout");
    static_assert(800 * sizeof(FlakePx) + 2 * FG_G * FG_G * sizeof(uint16_t) <= sizeof(mp3Space), "splash arena");
    static_assert(800 * sizeof(FlakePx) + 136 * 8 * 2 <= sizeof(mp3Space), "splash strip");
    Serial.printf("boot heap free=%u largest=%u\n", (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    FlakePx* px = (FlakePx*)mp3Space;
    uint8_t* after = mp3Space + 800 * sizeof(FlakePx);
    int n = flakeGen(esp_random(), px, 800, (uint16_t*)after);
    if (n == 0) return;
    M5Canvas cv(&d);
    cv.setColorDepth(16);
    cv.setBuffer(after, SW, STRIP, 16);
    float gEnd = 0;
    for (int i = 0; i < n; i++) if (px[i].t > gEnd) gEnd = (float)px[i].t;
    const float slideT0 = gEnd + 250, textT0 = slideT0 + 300, subT0 = textT0 + 500;
    const float endT = subT0 + 11 * 40 + 120 + 800;
    d.fillScreen(COL_BG);
    uint32_t start = millis();
    for (;;) {
        float t = (float)(millis() - start);
        if (t > endT) t = endT;
        int cx = CX0;
        if (t >= slideT0) {
            float u = splClamp((t - slideT0) / 600.0f), e = u * u * (3 - 2 * u);
            cx = (int)floorf(CX0 + (CX1 - CX0) * e + 0.5f);
        }
        for (int sy0 = 0; sy0 < SH; sy0 += STRIP) {
            const int oy = OY + sy0;
            cv.fillSprite(COL_BG);
            for (int i = 0; i < n; i++)
                if (t >= px[i].t) cv.drawPixel(cx + px[i].x - OX, CY + px[i].y - oy, COL_ACCENT);
            for (int i = 0; i < 5; i++) {
                float a = splClamp((t - (textT0 + i * 110)) / 140.0f);
                if (a <= 0) continue;
                uint16_t col = splFade(a);
                for (int r = 0; r < 9; r++)
                    for (int c = 0; c < 5; c++)
                        if (SPL_FROST[i][r][c] == '#') cv.fillRect(TX + (i * 6 + c) * 2 - OX, TY + r * 2 - oy, 2, 2, col);
            }
            for (int i = 0; i < 12; i++) {
                float a = splClamp((t - (subT0 + i * 40)) / 120.0f);
                if (a <= 0) continue;
                const SplSubGlyph* g = splSubFind(SPL_SUB_TEXT[i]);
                if (!g) continue;
                uint16_t col = splFade(a);
                for (int r = 0; r < 8; r++)
                    for (int c = 0; g->rows[r][c]; c++)
                        if (g->rows[r][c] == '#') cv.drawPixel(TX + SPL_SUB_X[i] + c - OX, SY + r - oy, col);
            }
            cv.pushSprite(OX, oy);
        }
        if (t >= endT) break;
        delay(6);
    }
    cv.deleteSprite();
}

void setup() {
    Serial.begin(115200);
    delay(1500);

    auto cfg = M5.config();
    cfg.external_speaker.hat_spk = true;
    M5Cardputer.begin(cfg, true);
    delay(100);

    auto spk_cfg = M5Cardputer.Speaker.config();
    spk_cfg.sample_rate      = 44100;
    spk_cfg.task_pinned_core = PRO_CPU_NUM;
    spk_cfg.dma_buf_count    = 16;
    spk_cfg.dma_buf_len      = 512;
    spk_cfg.task_priority    = 3;
    M5Cardputer.Speaker.config(spk_cfg);
    M5Cardputer.Speaker.begin();
    M5Cardputer.Speaker.setVolume(volume);

    auto &d = M5Cardputer.Display;
    d.setRotation(1);
    d.setFont(FONT);
    d.setTextWrap(false);
    mqCanvas.setColorDepth(16);
    mqCanvas.createSprite(MQ_MAX_W, ROW_H);
    mqCanvas.setFont(FONT);
    mqCanvas.setTextWrap(false);
    applyTheme();
    d.fillScreen(COL_BG);

    dispMutex = xSemaphoreCreateRecursiveMutex();
    out = new AudioOutputM5Speaker(&M5Cardputer.Speaker, 0);
    out->begin();
    sdMutex = xSemaphoreCreateMutex();
    ringMutex = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, configMAX_PRIORITIES - 2, nullptr, 1);
    xTaskCreatePinnedToCore(readerTask, "reader", 8192, nullptr, 3, nullptr, 0);
    xTaskCreatePinnedToCore(videoTask, "video", 8192, nullptr, 2, nullptr, 0);

    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    bool ok = SD.begin(SD_CS, SPI, 25000000, "/sd", 8);
    if (!ok) ok = SD.begin(SD_CS, SPI, 4000000, "/sd", 8);
    if (!ok) {
        d.setBrightness(cfgBrightness);
        d.setTextColor(TFT_RED, COL_BG);
        d.setCursor(4, 4); d.print("SD init FAILED");
        Serial.println("SD init failed");
        return;
    }
    Serial.println("SD ok");

    loadConfig();
    applyTheme();
    loadIndex();
    queueCacheReset();

    strncpy(currentPath, cfgMusicDir, MY_PATH_MAX - 1);
    currentPath[MY_PATH_MAX - 1] = '\0';
    depth = 0;
    rootOk = loadDir();

    displayOn = true; screenIsOff = false;
    applyBacklight();
    lastInputTime = millis();

    if (cfgBootAnim) bootSplash();
    lastInputTime = millis();

    drawAll();
    needsFullRedraw = false;
}

void loop() {
    M5Cardputer.update();

    if (playState == PLAYING && decoder && !audioRunning) {
        if (repeatOne) playQueuePos(queuePos);
        else if (hasNextTrack()) nextTrack();
        else stopToUi();
    }

    if (M5Cardputer.BtnA.wasPressed()) {
        lastInputTime = millis();
        if (screenVisible()) displayOn = false;
        else {
            displayOn = true; screenIsOff = false;
            if (fullscreen) {
                dispLock();
                applyBacklight();
                M5Cardputer.Display.fillScreen(TFT_BLACK);
                if (playState == PAUSED) drawPauseGlyph();
                dispUnlock();
            } else needsFullRedraw = true;
        }
        applyBacklight();
    }

    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        lastInputTime = millis();
        auto ks = M5Cardputer.Keyboard.keysState();
        bool fn = ks.fn;
        if (ks.space && !fn) togglePause();
        if (ks.enter && !fullscreen && playState != STOPPED) enterFullscreen();
        for (char c : ks.word) {
            if (c == ' ') continue;
            if (fn) {
                if      (c == KEY_VOLUP)                        changeBrightness(+16);
                else if (c == KEY_VOLDN_A || c == KEY_VOLDN_B)  changeBrightness(-16);
                else if (c == KEY_SCAN_A || c == KEY_SCAN_B)    runScan();
                else if (c == KEY_RIGHT)                        seekBy(+SEEK_STEP_PCT);
                else if (c == KEY_LEFT)                         seekBy(-SEEK_STEP_PCT);
                else if (c == KEY_NEXT)                         adjustAvOffset(+20);
                else if (c == KEY_PREV)                         adjustAvOffset(-20);
                else if (c == KEY_ENQUEUE && !fullscreen)       enqueueSelected(true);
                continue;
            }
            if (c >= '0' && c <= '9') { seekToPct((c - '0') * 10); continue; }
            if      (c == KEY_NEXT)  nextTrack();
            else if (c == KEY_PREV)  prevTrack();
            else if (c == KEY_SHUFFLE) toggleShuffle();
            else if (c == KEY_VOLUP) changeVolume(+15);
            else if (c == KEY_VOLDN_A || c == KEY_VOLDN_B) changeVolume(-15);
            else if (c == 's' || c == 'S') stopToUi();
            else if (c == 'v' || c == 'V') toggleVideo();
            else if (c == 'h' || c == 'H') halfRate = !halfRate;
            else if (c == 'd' || c == 'D') toggleDebug();
            else if (c == 'i' || c == 'I') showInfo();
            else if (c == 'm' || c == 'M') toggleMute();
            else if (c == 'r' || c == 'R') { repeatOne = !repeatOne; redrawNowPlaying = true; }
            else if (c == 'n' || c == 'N') stepFrame(+1);
            else if (c == 'b' || c == 'B') stepFrame(-1);
            else if (fullscreen) {
                if (c == KEY_LEFT) leaveFullscreen();
            } else {
                if      (c == KEY_UP)    moveCursor(-1);
                else if (c == KEY_DOWN)  moveCursor(+1);
                else if (c == KEY_RIGHT) openSelected();
                else if (c == KEY_LEFT)  goBack();
                else if (c == KEY_ENQUEUE) enqueueSelected(false);
            }
        }
    }

    if (displayOn && !screenIsOff && playState != PLAYING && cfgScreenTimeoutMs != 0 && millis() - lastInputTime >= cfgScreenTimeoutMs) {
        screenIsOff = true;
        applyBacklight();
    }

    dispLock();
    if (screenVisible() && !fullscreen) {
        if (needsFullRedraw) {
            drawAll();
            needsFullRedraw = false;
            redrawBrowser = redrawNowPlaying = redrawQueue = false;
        }
        if (redrawBrowser)    { drawBrowser();    redrawBrowser = false; }
        if (redrawNowPlaying) { drawNowPlaying(); redrawNowPlaying = false; }
        if (redrawQueue)      { drawQueue();      redrawQueue = false; }

        marqueeTickAll();
        static unsigned long lastProgressDraw = 0;
        if (playState != STOPPED && millis() - lastProgressDraw >= 500) {
            lastProgressDraw = millis();
            drawNowPlayingStatus();
        }
    }
    dispUnlock();
}

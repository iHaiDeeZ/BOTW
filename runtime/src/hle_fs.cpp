// nn::fs at the API level, on host files. The game imports these by name (main), so no fsp-srv IPC is involved:
//   rom:   -> private/extract/romfs (the RomFS with the update applied), or the RomFS pack when there is no such folder:
//            romfs.pak (every file, 16-byte aligned) + romfs.idx ("<offset>\t<size>\t<path>" per file), made by the
//            PS4 port's scripts/make-romfs-pack.py so the console holds two files instead of 304,207 (Tears of the
//            Kingdom).
//   save:  -> private/user/save/<mount>
//   cache: -> private/user/cache<index>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include <nx/runtime.h>

#include "platform.h"

namespace fs = std::filesystem;

namespace nx {

namespace {

// NX_TRACE_FS=1: every file the game opens (and the ones it looks for and does not find).
bool trace_fs() {
    static const bool on = g_trace || std::getenv("NX_TRACE_FS") != nullptr;
    return on;
}

constexpr uint32_t kOk = 0;
constexpr uint32_t kPathNotFound = 0x202;       // fs 1
constexpr uint32_t kPathAlreadyExists = 0x402;  // fs 2
constexpr uint32_t kTargetNotFound = 0x2EE202;  // fs 6001-ish: generic "not mounted"

std::mutex g_fs;
std::map<std::string, fs::path> g_mounts;
// Open files: shared so a read in progress survives a close from another thread. Reads run outside g_fs (which
// only guards the tables): one global lock made every streaming thread (Bink animations, FMOD banks) wait behind
// any big read, felt as hitches.
struct File {
    FILE* f;
    std::string path;
    uint64_t base = 0;   // RomFS pack: where the file starts in romfs.pak
    int64_t size = -1;   // RomFS pack: the file's size (-1: a host file of its own)
    bool owns = true;    // false: the shared romfs.pak
    // Streamed audio (.bwav in the pack) is read ahead: the game reads its streams 48 KB at a time, several streams
    // (music, ambience, a cutscene's dialogue) and the world's data from all over romfs.pak at once, and on the PS4's
    // hard disk each small read waited ~90 ms (up to 155) for its seek, long enough for the audio engine to drop a
    // voice in the middle of a line. One read of kReadAhead serves the next ones from memory. Under ahead_lock.
    // Only once the file is being streamed (a read starting where the last one ended), within a budget for all files
    // (kReadAheadBudget), and given back at the stream's end or the file's close: v00.36 read ahead for every .bwav
    // opened, and the loading screen froze (the game opens thousands of sound files while loading; the host heap has
    // ~130 MB on the PS4).
    bool stream = false;
    std::mutex ahead_lock;
    std::vector<uint8_t> ahead;
    int64_t ahead_at = -1;  // the window's offset in the file
    int64_t last_end = -1;  // where the last read ended (a read starting there: streaming)
    File(FILE* file, std::string p) : f(file), path(std::move(p)) {}
    File(FILE* file, std::string p, uint64_t b, int64_t n) : f(file), path(std::move(p)), base(b), size(n), owns(false) {
        static const bool on = [] {
            const char* e = std::getenv("NX_STREAM_READ_AHEAD");
            return !(e && e[0] == '0');
        }();
        stream = on && path.size() > 5 && path.compare(path.size() - 5, 5, ".bwav") == 0;
    }
    File(const File&) = delete;  // owns the FILE: one instance only
    File& operator=(const File&) = delete;
    ~File();
};
std::map<uint64_t, std::shared_ptr<File>> g_files;

// Files the runtime adds or replaces (mods): key = path under the mount point, '/' separated, lower case.
std::map<std::string, std::string> g_virtual;

std::string virtual_key(const fs::path& p) {
    auto g = p.lexically_normal().generic_u8string();
    std::string s(g.begin(), g.end());
    for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// The virtual file whose key the resolved path ends with, or nullptr.
const std::string* find_virtual(const fs::path& resolved) {
    if (g_virtual.empty() || resolved.empty()) return nullptr;
    std::string k = virtual_key(resolved);
    for (auto& [key, data] : g_virtual)
        if (k.size() >= key.size() + 1 && k.compare(k.size() - key.size(), key.size(), key) == 0 &&
            k[k.size() - key.size() - 1] == '/')
            return &data;
    return nullptr;
}
std::string utf8(const fs::path& p);

// ---- the RomFS pack ----------------------------------------------------------------------------------------
// Paths under it resolve below kPackRoot (resolve() keeps working on fs::path); pack_key() turns them back.
const fs::path kPackRoot = fs::u8path("/\x01romfs-pack");

struct PackEntry {
    uint64_t offset;
    int64_t size;  // -1: a directory
};
struct Pack {
    FILE* f = nullptr;
    std::unordered_map<std::string, PackEntry> entries;                 // "Shader/x.bfsha.zs", "Shader" (dirs)
    std::unordered_map<std::string, std::vector<std::string>> children;  // dir ("" = root) -> names
};
Pack* g_pack = nullptr;

// The pack in the content root, loaded once; nullptr when the RomFS is a folder (or there is no pack).
Pack* load_pack() {
    static bool tried = false;
    if (tried) return g_pack;
    tried = true;
    fs::path root = fs::u8path(plat::content_root());
    std::error_code ec;
    if (fs::is_directory(root / "extract" / "romfs", ec)) return nullptr;
    std::ifstream idx(root / "romfs.idx", std::ios::binary);
    FILE* f = plat::fopen_utf8(utf8(root / "romfs.pak"), "rb");
    if (!idx || !f) {
        if (f) std::fclose(f);
        return nullptr;
    }
    auto* pack = new Pack;
    pack->f = f;
    pack->entries[""] = {0, -1};
    std::string line;
    size_t files = 0;
    while (std::getline(idx, line)) {
        size_t t1 = line.find('\t'), t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        std::string path = line.substr(t2 + 1);
        if (!path.empty() && path.back() == '\r') path.pop_back();
        pack->entries[path] = {std::strtoull(line.c_str(), nullptr, 10), int64_t(std::strtoll(line.c_str() + t1 + 1, nullptr, 10))};
        files++;
        // its folders, up to the root
        std::string child = path;
        for (;;) {
            size_t slash = child.rfind('/');
            std::string parent = slash == std::string::npos ? std::string() : child.substr(0, slash);
            auto& kids = pack->children[parent];
            std::string name = child.substr(slash == std::string::npos ? 0 : slash + 1);
            bool new_dir = !parent.empty() && pack->entries.find(parent) == pack->entries.end();
            if (child == path || new_dir || std::find(kids.begin(), kids.end(), name) == kids.end()) kids.push_back(name);
            if (parent.empty() || !new_dir) break;
            pack->entries[parent] = {0, -1};
            child = parent;
        }
    }
    log("fs: RomFS pack: %zu files, %zu folders (%s)", files, pack->children.size(), utf8(root / "romfs.pak").c_str());
    g_pack = pack;
    return pack;
}

// The pack-relative key of a resolved path ("" = the root), or false when the path is not in the pack.
bool pack_key(const fs::path& p, std::string& key) {
    if (!g_pack || p.empty()) return false;
    auto rel = p.lexically_normal().lexically_relative(kPackRoot);
    if (rel.empty()) return false;
    auto g = rel.generic_u8string();
    key.assign(g.begin(), g.end());
    if (key == ".") key.clear();
    if (key.rfind("..", 0) == 0) return false;
    while (!key.empty() && key.back() == '/') key.pop_back();
    return true;
}

const PackEntry* pack_find(const fs::path& p) {
    std::string key;
    if (!pack_key(p, key)) return nullptr;
    auto it = g_pack->entries.find(key);
    return it == g_pack->entries.end() ? nullptr : &it->second;
}

struct DirEntry {
    std::string name;
    bool dir;
    int64_t size;
};
struct Dir {
    std::vector<DirEntry> entries;
    size_t pos = 0;
};
std::map<uint64_t, Dir> g_dirs;
uint64_t g_next = 1;

fs::path data_root() { return fs::u8path(plat::data_root()); }

std::string utf8(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string cstr(uint64_t p) { return p ? std::string(reinterpret_cast<const char*>(p)) : std::string(); }

// "mount:/a/b" -> host path; empty when the mount is unknown.
fs::path resolve(const std::string& guest) {
    auto colon = guest.find(':');
    if (colon == std::string::npos) return {};
    auto it = g_mounts.find(guest.substr(0, colon));
    if (it == g_mounts.end()) return {};
    std::string rest = guest.substr(colon + 1);
    while (!rest.empty() && (rest[0] == '/' || rest[0] == '\\')) rest.erase(0, 1);
    return it->second / fs::u8path(rest);
}

void mount(const std::string& name, const fs::path& p) {
    if (p != kPackRoot) fs::create_directories(p);
    g_mounts[name] = p;
    log("fs: mounted %s: -> %s", name.c_str(), p.string().c_str());
}

// ---- mounts ---------------------------------------------------------------------------------------------------

void QueryMountRomCacheSize(CPU& c) {
    st<uint64_t>(c.x[0], 0x1000);
    c.x[0] = kOk;
}

void MountRom(CPU& c) {
    std::lock_guard lk(g_fs);
    mount(cstr(c.x[0]), load_pack() ? kPackRoot : fs::u8path(plat::content_root()) / "extract" / "romfs");
    c.x[0] = kOk;
}

void EnsureSaveData(CPU& c) { c.x[0] = kOk; }

void MountSaveData(CPU& c) {
    std::lock_guard lk(g_fs);
    std::string name = cstr(c.x[0]);
    mount(name, data_root() / "user" / "save");
    c.x[0] = kOk;
}

void Unmount(CPU& c) {
    std::lock_guard lk(g_fs);
    g_mounts.erase(cstr(c.x[0]));
    c.x[0] = kOk;
}

void Commit(CPU& c) { c.x[0] = kOk; }
void EnableGlobalFileDataCache(CPU& c) { c.x[0] = kOk; }
void DisableGlobalFileDataCache(CPU& c) { c.x[0] = kOk; }

// ---- files ----------------------------------------------------------------------------------------------------

void CreateFile(CPU& c) {
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[0]));
    int64_t size = int64_t(c.x[1]);
    if (p.empty() || !fs::exists(p.parent_path())) {
        c.x[0] = kPathNotFound;
        return;
    }
    if (fs::exists(p)) {
        c.x[0] = kPathAlreadyExists;
        return;
    }
    FILE* f = plat::fopen_utf8(utf8(p), "wb");
    if (f) {
        if (size > 0) {
            plat::fseek64(f, size - 1, SEEK_SET);
            fputc(0, f);
        }
        fclose(f);
    }
    c.x[0] = f ? kOk : kPathNotFound;
}

void DeleteFile(CPU& c) {
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[0]));
    std::error_code ec;
    c.x[0] = !p.empty() && fs::remove(p, ec) ? kOk : kPathNotFound;
}

void CreateDirectory(CPU& c) {
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[0]));
    if (p.empty()) {
        c.x[0] = kPathNotFound;
        return;
    }
    if (fs::exists(p)) {
        c.x[0] = kPathAlreadyExists;
        return;
    }
    std::error_code ec;
    c.x[0] = fs::create_directory(p, ec) ? kOk : kPathNotFound;
}

void GetEntryType(CPU& c) {  // (DirectoryEntryType* out, path)
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[1]));
    if (find_virtual(p)) {
        st<uint32_t>(c.x[0], 1);
        c.x[0] = kOk;
        return;
    }
    std::string key;
    if (pack_key(p, key)) {
        const PackEntry* e = pack_find(p);
        if (!e) {
            c.x[0] = kPathNotFound;
            return;
        }
        st<uint32_t>(c.x[0], e->size < 0 ? 0 : 1);
        c.x[0] = kOk;
        return;
    }
    std::error_code ec;
    if (p.empty() || !fs::exists(p, ec)) {
        c.x[0] = kPathNotFound;
        return;
    }
    st<uint32_t>(c.x[0], fs::is_directory(p) ? 0 : 1);
    c.x[0] = kOk;
}

void OpenFile(CPU& c) {  // (FileHandle* out, path, int mode: 1 read, 2 write, 4 append)
    std::lock_guard lk(g_fs);
    std::string guest = cstr(c.x[1]);
    fs::path p = resolve(guest);
    int mode = int(c.x[2]);
    if (const std::string* v = find_virtual(p)) {  // served from memory, through a temporary file
        FILE* f = std::tmpfile();
        if (!f) {
            c.x[0] = kPathNotFound;
            return;
        }
        std::fwrite(v->data(), 1, v->size(), f);
        std::fflush(f);
        uint64_t h = g_next++;
        g_files[h] = std::make_shared<File>(f, guest);
        st<uint64_t>(c.x[0], h);
        if (trace_fs()) log("fs: OpenFile(%s) -> virtual, %zu bytes", guest.c_str(), v->size());
        c.x[0] = kOk;
        return;
    }
    std::string key;
    if (pack_key(p, key)) {
        const PackEntry* e = pack_find(p);
        if (!e || e->size < 0) {
            if (trace_fs()) log("fs: OpenFile(%s) not found", guest.c_str());
            c.x[0] = kPathNotFound;
            return;
        }
        uint64_t h = g_next++;
        g_files[h] = std::make_shared<File>(g_pack->f, guest, e->offset, e->size);
        st<uint64_t>(c.x[0], h);
        if (trace_fs()) log("fs: OpenFile(%s, %d) -> %llu (pack)", guest.c_str(), mode, (unsigned long long)h);
        c.x[0] = kOk;
        return;
    }
    std::error_code ec;
    if (p.empty() || !fs::is_regular_file(p, ec)) {
        if (trace_fs()) log("fs: OpenFile(%s) not found", guest.c_str());
        c.x[0] = kPathNotFound;
        return;
    }
    FILE* f = plat::fopen_utf8(utf8(p), (mode & 2) ? "r+b" : "rb");
    if (!f) {
        c.x[0] = kPathNotFound;
        return;
    }
    uint64_t h = g_next++;
    g_files[h] = std::make_shared<File>(f, guest);
    st<uint64_t>(c.x[0], h);
    if (trace_fs()) log("fs: OpenFile(%s, %d) -> %llu", guest.c_str(), mode, (unsigned long long)h);
    c.x[0] = kOk;
}

std::shared_ptr<File> open_file(uint64_t h) {
    std::lock_guard lk(g_fs);
    auto it = g_files.find(h);
    return it == g_files.end() ? nullptr : it->second;
}

void CloseFile(CPU& c) {
    std::lock_guard lk(g_fs);
    g_files.erase(c.x[0]);
}

void GetFileSize(CPU& c) {  // (s64* out, FileHandle)
    auto file = open_file(c.x[1]);
    if (!file) {
        c.x[0] = kTargetNotFound;
        return;
    }
    if (file->size >= 0) {
        st<int64_t>(c.x[0], file->size);
        c.x[0] = kOk;
        return;
    }
    std::lock_guard lk(g_fs);  // the FILE position is shared with writers
    FILE* f = file->f;
    int64_t cur = plat::ftell64(f);
    plat::fseek64(f, 0, SEEK_END);
    st<int64_t>(c.x[0], plat::ftell64(f));
    if (g_trace) log("fs: size %s = %lld", file->path.c_str(), (long long)ld<int64_t>(c.x[0]));
    plat::fseek64(f, cur, SEEK_SET);
    c.x[0] = kOk;
}

constexpr uint64_t kReadAhead = 512 << 10;        // NX_STREAM_READ_AHEAD=0: off
constexpr uint64_t kReadAheadBudget = 16ull << 20;  // all files' windows together
std::atomic<uint64_t> g_ahead_bytes{0};

void drop_ahead(File& file) {  // under ahead_lock
    g_ahead_bytes -= file.ahead.capacity();
    std::vector<uint8_t>().swap(file.ahead);
    file.ahead_at = -1;
}

File::~File() {
    if (!ahead.empty() || ahead.capacity()) g_ahead_bytes -= ahead.capacity();
    if (owns) std::fclose(f);
}

// `size` bytes at `off` (file-relative) of a streamed file through its read-ahead window. False: read directly.
bool read_ahead(File& file, int64_t off, uint64_t buf, uint64_t size, uint64_t& n) {
    if (!file.stream || size == 0 || size > kReadAhead / 2) return false;
    std::lock_guard lk(file.ahead_lock);
    const int64_t end = off + int64_t(size);
    const bool sequential = file.last_end == off;
    file.last_end = end;
    if (file.ahead_at < 0 || off < file.ahead_at || end > file.ahead_at + int64_t(file.ahead.size())) {
        if (!sequential) {  // not streaming (yet): read directly, and keep no window
            if (file.ahead.capacity()) drop_ahead(file);
            return false;
        }
        const uint64_t want = std::min<uint64_t>(kReadAhead, uint64_t(file.size - off));
        if (want <= size) return false;  // nothing left to read ahead
        if (want > file.ahead.capacity()) {
            const uint64_t grow = want - file.ahead.capacity();
            if (g_ahead_bytes.load() + grow > kReadAheadBudget) return false;  // over budget: read directly
            g_ahead_bytes += grow;
            file.ahead.reserve(want);
        }
        file.ahead.resize(want);
        uint64_t got = plat::read_at(file.f, int64_t(file.base) + off, file.ahead.data(), want);
        file.ahead.resize(got);
        file.ahead_at = off;
        if (got < size) {
            drop_ahead(file);
            return false;
        }
    }
    std::memcpy(reinterpret_cast<void*>(buf), file.ahead.data() + (off - file.ahead_at), size);
    n = size;
    if (end >= file.size) drop_ahead(file);  // the stream's end: the window goes back to the budget
    return true;
}

uint64_t read_at(File& file, int64_t off, uint64_t buf, uint64_t size) {
    uint64_t t0 = plat::monotonic_ns();
    if (file.size >= 0) {  // a window of the RomFS pack: never past the file's end
        if (off < 0 || off >= file.size) return 0;
        size = std::min<uint64_t>(size, uint64_t(file.size - off));
        if (uint64_t n = 0; read_ahead(file, off, buf, size, n)) {
            if (uint64_t ms = (plat::monotonic_ns() - t0) / 1'000'000; ms >= 30)
                log("fs: slow read %s: %llu bytes at %lld took %llu ms (read ahead)", file.path.c_str(),
                    (unsigned long long)size, (long long)off, (unsigned long long)ms);
            return n;
        }
        off += int64_t(file.base);
    }
    uint64_t n = plat::read_at(file.f, off, reinterpret_cast<void*>(buf), size);
    uint64_t ms = (plat::monotonic_ns() - t0) / 1'000'000;
    if (g_trace) log("fs: read %s at %lld size %llu -> %llu", file.path.c_str(), (long long)off, (unsigned long long)size,
                     (unsigned long long)n);
    if (trace_fs() && file.path.find(".bwav") != std::string::npos) {  // streamed audio: is it being consumed?
        static std::mutex m;
        static std::map<std::string, uint64_t> reads;
        std::lock_guard lk(m);
        uint64_t k = ++reads[file.path];
        if (k <= 3 || k % 50 == 0) log("fs: stream read #%llu %s at %lld", (unsigned long long)k, file.path.c_str(), (long long)off);
    }
    if (ms >= 30)
        log("fs: slow read %s: %llu bytes at %lld took %llu ms", file.path.c_str(), (unsigned long long)size,
            (long long)off, (unsigned long long)ms);
    return n;
}

void ReadFileOut(CPU& c) {  // (size_t* out, FileHandle, s64 off, void* buf, size_t size)
    auto file = open_file(c.x[1]);
    if (!file) {
        c.x[0] = kTargetNotFound;
        return;
    }
    st<uint64_t>(c.x[0], read_at(*file, int64_t(c.x[2]), c.x[3], c.x[4]));
    c.x[0] = kOk;
}

void ReadFile(CPU& c) {  // (FileHandle, s64 off, void* buf, size_t size)
    auto file = open_file(c.x[0]);
    if (!file) {
        c.x[0] = kTargetNotFound;
        return;
    }
    read_at(*file, int64_t(c.x[1]), c.x[2], c.x[3]);
    c.x[0] = kOk;
}

void WriteFile(CPU& c) {  // (FileHandle, s64 off, const void* buf, size_t size, const WriteOption&)
    auto file = open_file(c.x[0]);
    if (!file) {
        c.x[0] = kTargetNotFound;
        return;
    }
    std::lock_guard lk(g_fs);
    plat::fseek64(file->f, int64_t(c.x[1]), SEEK_SET);
    fwrite(reinterpret_cast<const void*>(c.x[2]), 1, c.x[3], file->f);
    fflush(file->f);  // positional reads bypass the FILE buffer (saves only: rare and small)
    c.x[0] = kOk;
}

void FlushFile(CPU& c) {
    if (auto file = open_file(c.x[0])) {
        std::lock_guard lk(g_fs);
        fflush(file->f);
    }
    c.x[0] = kOk;
}

// ---- directories ----------------------------------------------------------------------------------------------

void OpenDirectory(CPU& c) {  // (DirectoryHandle* out, path, int mode: 1 dirs, 2 files)
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[1]));
    int mode = int(c.x[2]);
    Dir d;
    std::string key;
    if (pack_key(p, key)) {
        const PackEntry* e = pack_find(p);
        if (!e || e->size >= 0) {
            c.x[0] = kPathNotFound;
            return;
        }
        for (const std::string& name : g_pack->children[key]) {
            const PackEntry& k = g_pack->entries[key.empty() ? name : key + "/" + name];
            bool dir = k.size < 0;
            if ((dir && (mode & 1)) || (!dir && (mode & 2))) d.entries.push_back({name, dir, dir ? 0 : k.size});
        }
    } else {
        std::error_code ec;
        if (p.empty() || !fs::is_directory(p, ec)) {
            c.x[0] = kPathNotFound;
            return;
        }
        for (auto& e : fs::directory_iterator(p, ec)) {
            bool dir = e.is_directory();
            auto u8 = e.path().filename().u8string();
            if ((dir && (mode & 1)) || (!dir && (mode & 2)))
                d.entries.push_back({std::string(u8.begin(), u8.end()), dir, dir ? 0 : int64_t(e.file_size())});
        }
    }
    uint64_t h = g_next++;
    g_dirs[h] = std::move(d);
    st<uint64_t>(c.x[0], h);
    c.x[0] = kOk;
}

void ReadDirectory(CPU& c) {  // (s64* out_count, DirectoryEntry* entries, DirectoryHandle, s64 max)
    std::lock_guard lk(g_fs);
    auto it = g_dirs.find(c.x[2]);
    if (it == g_dirs.end()) {
        c.x[0] = kTargetNotFound;
        return;
    }
    Dir& d = it->second;
    int64_t n = 0, max = int64_t(c.x[3]);
    for (; n < max && d.pos < d.entries.size(); n++, d.pos++) {
        const auto& e = d.entries[d.pos];
        uint64_t out = c.x[1] + uint64_t(n) * 0x310;
        std::memset(reinterpret_cast<void*>(out), 0, 0x310);
        std::memcpy(reinterpret_cast<void*>(out), e.name.c_str(), std::min<size_t>(e.name.size(), 0x300));
        st<int8_t>(out + 0x304, e.dir ? 0 : 1);
        st<int64_t>(out + 0x308, e.size);
    }
    st<int64_t>(c.x[0], n);
    c.x[0] = kOk;
}

void GetDirectoryEntryCount(CPU& c) {  // (s64* out, DirectoryHandle)
    std::lock_guard lk(g_fs);
    auto it = g_dirs.find(c.x[1]);
    st<int64_t>(c.x[0], it == g_dirs.end() ? 0 : int64_t(it->second.entries.size()));
    c.x[0] = kOk;
}

void CloseDirectory(CPU& c) {
    std::lock_guard lk(g_fs);
    g_dirs.erase(c.x[0]);
}

// ---- cache storage --------------------------------------------------------------------------------------------

void OpenCacheStorageList(CPU& c) {
    st<uint64_t>(c.x[0], 1);
    c.x[0] = kOk;
}
void ReadCacheStorageList(CPU& c) {  // (int* out, CacheStorageInfo*, handle, int)
    st<int32_t>(c.x[0], 0);
    c.x[0] = kOk;
}
void CloseCacheStorageList(CPU& c) {}
void CreateCacheStorage(CPU& c) { c.x[0] = kOk; }
void DeleteCacheStorage(CPU& c) { c.x[0] = kOk; }
void MountCacheStorage(CPU& c) {  // (name, int index)
    std::lock_guard lk(g_fs);
    mount(cstr(c.x[0]), data_root() / "user" / ("cache" + std::to_string(int(c.x[1]))));
    c.x[0] = kOk;
}

// ---- SDK 16 additions (Super Mario Bros. Wonder) ---------------------------------------------------------------
// MountRom(name) without a cache buffer, and ReadFile(size_t* out, FileHandle, s64, void*, size_t, const ReadOption&)
// are the older calls with one argument less / more: the same functions serve them.

void IsSaveDataExisting(CPU& c) { c.x[0] = 1; }  // (const account::Uid&) -> bool: EnsureSaveData made it

void SetFileSize(CPU& c) {  // (FileHandle, s64 size)
    auto file = open_file(c.x[0]);
    if (!file) {
        c.x[0] = kTargetNotFound;
        return;
    }
    std::lock_guard lk(g_fs);
    std::fflush(file->f);
#if defined(_WIN32)
    bool ok = _chsize_s(_fileno(file->f), int64_t(c.x[1])) == 0;
#else
    bool ok = ftruncate(fileno(file->f), off_t(c.x[1])) == 0;
#endif
    c.x[0] = ok ? kOk : kPathNotFound;
}

void DeleteDirectoryRecursively(CPU& c) {
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[0]));
    std::error_code ec;
    if (p.empty() || !fs::is_directory(p, ec)) {
        c.x[0] = kPathNotFound;
        return;
    }
    fs::remove_all(p, ec);
    c.x[0] = ec ? kPathNotFound : kOk;
}

}  // namespace

NX_HLE("_ZN2nn2fs22QueryMountRomCacheSizeEPm", QueryMountRomCacheSize);
NX_HLE("_ZN2nn2fs8MountRomEPKcPvm", MountRom);
NX_HLE("_ZN2nn2fs14EnsureSaveDataERKNS_7account3UidE", EnsureSaveData);
NX_HLE("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE", MountSaveData);
NX_HLE("_ZN2nn2fs7UnmountEPKc", Unmount);
NX_HLE("_ZN2nn2fs10DeleteFileEPKc", DeleteFile);
NX_HLE("_ZN2nn2fs10CreateFileEPKcl", CreateFile);
NX_HLE("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci", OpenFile);
NX_HLE("_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE", GetFileSize);
NX_HLE("_ZN2nn2fs15CreateDirectoryEPKc", CreateDirectory);
NX_HLE("_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc", GetEntryType);
NX_HLE("_ZN2nn2fs9CloseFileENS0_10FileHandleE", CloseFile);
NX_HLE("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm", ReadFileOut);
NX_HLE("_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm", ReadFile);
NX_HLE("_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", WriteFile);
NX_HLE("_ZN2nn2fs9FlushFileENS0_10FileHandleE", FlushFile);
NX_HLE("_ZN2nn2fs25EnableGlobalFileDataCacheEPvm", EnableGlobalFileDataCache);
NX_HLE("_ZN2nn2fs26DisableGlobalFileDataCacheEv", DisableGlobalFileDataCache);
NX_HLE("_ZN2nn2fs6CommitEPKc", Commit);
NX_HLE("_ZN2nn2fs14CommitSaveDataEPKc", Commit);  // SDK 7 name (Breath of the Wild)
NX_HLE("_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci", OpenDirectory);
NX_HLE("_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl", ReadDirectory);
NX_HLE("_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE", CloseDirectory);
NX_HLE("_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE", GetDirectoryEntryCount);
NX_HLE("_ZN2nn2fs20OpenCacheStorageListEPNS0_22CacheStorageListHandleE", OpenCacheStorageList);
NX_HLE("_ZN2nn2fs20ReadCacheStorageListEPiPNS0_16CacheStorageInfoENS0_22CacheStorageListHandleEi", ReadCacheStorageList);
NX_HLE("_ZN2nn2fs21CloseCacheStorageListENS0_22CacheStorageListHandleE", CloseCacheStorageList);
NX_HLE("_ZN2nn2fs18CreateCacheStorageEill", CreateCacheStorage);
NX_HLE("_ZN2nn2fs18DeleteCacheStorageEi", DeleteCacheStorage);
NX_HLE("_ZN2nn2fs17MountCacheStorageEPKci", MountCacheStorage);
NX_HLE("_ZN2nn2fs8MountRomEPKc", MountRom);
NX_HLE("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE", ReadFileOut);
NX_HLE("_ZN2nn2fs18IsSaveDataExistingERKNS_7account3UidE", IsSaveDataExisting);
NX_HLE("_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl", SetFileSize);
NX_HLE("_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc", DeleteDirectoryRecursively);

void add_virtual_file(const std::string& path, std::string data) {
    std::lock_guard lk(g_fs);
    g_virtual[virtual_key(fs::u8path(path))] = std::move(data);
}

std::string read_game_file(const std::string& relative_to_romfs) {
    if (Pack* pack = load_pack()) {
        auto it = pack->entries.find(relative_to_romfs);
        if (it == pack->entries.end() || it->second.size < 0) return {};
        std::string s(size_t(it->second.size), '\0');
        s.resize(plat::read_at(pack->f, int64_t(it->second.offset), s.data(), s.size()));
        return s;
    }
    FILE* f = plat::fopen_utf8(utf8(fs::u8path(plat::content_root()) / "extract" / "romfs" / fs::u8path(relative_to_romfs)), "rb");
    if (!f) return {};
    std::string s;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}

}  // namespace nx

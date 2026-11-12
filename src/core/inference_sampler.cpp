// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/inference_parse.hpp"
#include "core/weight_stores.hpp"
#include "core/sampler.hpp"
#include "core/server_probe.hpp"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Finds processes running generative workloads and splits their VRAM into weights,
// KV cache and compute. Evidence comes from loaded runtime libraries, weight files
// mapped into the process, its launch flags, the runtime's own load log, and the
// local server API when it has one. Everything the OS reports is authoritative;
// the rest is labelled with how it was obtained.

namespace ring0 {
namespace {

using namespace ring0::inference;

constexpr std::uint64_t kMiB = 1024ull * 1024;
constexpr std::uint64_t kGiB = 1024 * kMiB;
constexpr std::uint64_t kCandidateVram = 256 * kMiB;
constexpr std::uint64_t kSpillWarn = 256 * kMiB;
constexpr std::uint64_t kRescanMinMs = 5'000;
constexpr std::uint64_t kRescanMaxMs = 120'000;
constexpr double        kScanBudgetMs = 150;
constexpr std::uint32_t kDefaultCtx = 4096;

struct Handle {
    HANDLE h = nullptr;
    explicit Handle(HANDLE v) : h(v == INVALID_HANDLE_VALUE ? nullptr : v) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { if (h) CloseHandle(h); }
};

std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

bool starts_with(const std::wstring& s, std::wstring_view p) { return s.rfind(p, 0) == 0; }

// Image names that are inference hosts whatever they have loaded.
bool name_hint(const std::wstring& img) {
    static constexpr std::array<std::wstring_view, 10> prefixes = {
        L"llama-", L"ollama", L"lm studio", L"lms.exe", L"koboldcpp", L"jan.exe",
        L"gpt4all", L"llamafile", L"sd.exe", L"whisper-"};
    for (auto p : prefixes)
        if (starts_with(img, p)) return true;
    return img.find(L"vllm") != std::wstring::npos;
}

// Runtime libraries by module base name. Bare CUDA does not mark a process: games and
// encoders load it too. The libraries below exist to run networks.
struct RuntimeSig { std::wstring_view needle; std::wstring_view label; };
constexpr std::array<RuntimeSig, 13> kRuntimes = {{
    {L"ggml", L"ggml"}, {L"llama.dll", L"llama.cpp"}, {L"whisper.dll", L"whisper.cpp"},
    {L"torch_cuda", L"pytorch"}, {L"torch_cpu", L"pytorch"}, {L"onnxruntime", L"onnxruntime"},
    {L"directml", L"directml"}, {L"nvinfer", L"tensorrt"}, {L"cudnn", L"cudnn"},
    {L"openvino", L"openvino"}, {L"amdhip64", L"hip"}, {L"mlx", L"mlx"}, {L"ctranslate2", L"ctranslate2"},
}};

bool is_weight_ext(const std::wstring& ext, bool& ambiguous) {
    static const std::unordered_set<std::wstring> clear = {L".gguf", L".ggml", L".safetensors", L".onnx", L".engine", L".plan", L".tflite"};
    static const std::unordered_set<std::wstring> vague = {L".bin", L".pt", L".pth", L".ckpt"};
    ambiguous = vague.contains(ext);
    return ambiguous || clear.contains(ext);
}

// Opens through any symlink: Hugging Face snapshots are links into blobs/, and the
// attributes of the link itself report zero bytes.
struct ResolvedFile {
    std::wstring  path;          // final path, so a snapshot link and its blob compare equal
    std::uint64_t size = 0;
    bool          gguf = false;
    bool          safetensors = false;
};

std::optional<ResolvedFile> resolve_file(const std::wstring& path) {
    Handle f(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, 0, nullptr));
    if (!f.h) return std::nullopt;
    ResolvedFile r;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(f.h, &sz) || sz.QuadPart <= 0) return std::nullopt;
    r.size = static_cast<std::uint64_t>(sz.QuadPart);
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetFinalPathNameByHandleW(f.h, buf, static_cast<DWORD>(std::size(buf)), FILE_NAME_NORMALIZED);
    r.path = (n && n < std::size(buf)) ? std::wstring(buf, n) : path;
    if (r.path.rfind(L"\\\\?\\UNC\\", 0) == 0) r.path = L"\\" + r.path.substr(7);
    else if (r.path.rfind(L"\\\\?\\", 0) == 0) r.path = r.path.substr(4);
    // Content-addressed stores drop the extension, so the header says what the file is.
    unsigned char head[9] = {};
    DWORD got = 0;
    if (ReadFile(f.h, head, sizeof head, &got, nullptr) && got == sizeof head) {
        r.gguf = std::memcmp(head, "GGUF", 4) == 0;
        r.safetensors = head[8] == '{' && head[7] == 0 && head[6] == 0;
    }
    return r;
}

std::uint64_t file_size(const std::wstring& path) {
    auto r = resolve_file(path);
    return r ? r->size : 0;
}

// Buffered random-access reader for GGUF headers: the vocabulary is hundreds of
// thousands of tiny reads, which would be as many syscalls unbuffered.
class FileWindow {
public:
    explicit FileWindow(const std::wstring& path)
        : f_(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr)) {}
    bool ok() const { return f_.h != nullptr; }

    bool read(std::uint64_t off, void* dst, std::size_t len) {
        auto* out = static_cast<unsigned char*>(dst);
        while (len) {
            if (off < base_ || off >= base_ + buf_.size()) {
                if (!fill(off)) return false;
                if (buf_.empty()) return false;
            }
            const auto at = static_cast<std::size_t>(off - base_);
            const auto n = std::min(len, buf_.size() - at);
            std::memcpy(out, buf_.data() + at, n);
            out += n; off += n; len -= n;
        }
        return true;
    }

private:
    bool fill(std::uint64_t off) {
        // Header parsing is a forward walk; cap the total so a corrupt length cannot drag in gigabytes.
        if ((reads_ += 1) > 256) return false;
        LARGE_INTEGER li; li.QuadPart = static_cast<LONGLONG>(off);
        if (!SetFilePointerEx(f_.h, li, nullptr, FILE_BEGIN)) return false;
        buf_.resize(1 << 20);
        DWORD got = 0;
        if (!ReadFile(f_.h, buf_.data(), static_cast<DWORD>(buf_.size()), &got, nullptr)) return false;
        buf_.resize(got);
        base_ = off;
        return true;
    }

    Handle f_;
    std::vector<unsigned char> buf_;
    std::uint64_t base_ = 0;
    int reads_ = 0;
};

// Follows a growing log from where it left off. Starts near the end on first sight:
// the load lines that matter belong to the most recent load.
class LogTail {
public:
    LoadTally tally;

    void poll(const std::wstring& path) {
        Handle f(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, 0, nullptr));
        if (!f.h) return;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(f.h, &size)) return;
        const auto sz = static_cast<std::uint64_t>(size.QuadPart);
        if (sz < offset_) { offset_ = 0; tally = {}; partial_.clear(); }   // rotated
        if (offset_ == 0 && sz > 8 * kMiB) offset_ = sz - 8 * kMiB;
        if (sz == offset_) return;
        LARGE_INTEGER li; li.QuadPart = static_cast<LONGLONG>(offset_);
        if (!SetFilePointerEx(f.h, li, nullptr, FILE_BEGIN)) return;
        std::string chunk(static_cast<std::size_t>(std::min<std::uint64_t>(sz - offset_, 8 * kMiB)), '\0');
        DWORD got = 0;
        if (!ReadFile(f.h, chunk.data(), static_cast<DWORD>(chunk.size()), &got, nullptr)) return;
        chunk.resize(got);
        offset_ += got;
        partial_ += chunk;
        std::size_t start = 0;
        for (std::size_t nl = partial_.find('\n'); nl != std::string::npos; nl = partial_.find('\n', start)) {
            tally.feed(std::string_view(partial_).substr(start, nl - start));
            start = nl + 1;
        }
        partial_.erase(0, start);
        if (partial_.size() > 64 * 1024) partial_.clear();
    }

private:
    std::uint64_t offset_ = 0;
    std::string   partial_;
};

struct ProcScan {
    std::uint64_t create_time = 0;
    std::uint64_t last_ms = 0;
    std::uint64_t last_vram = 0;
    bool          scanned = false;
    std::vector<std::wstring> runtimes;
    std::vector<WeightFile>   mapped;
};

struct CachedGguf {
    std::uint64_t size = 0;
    std::optional<GgufInfo> info;
};

class InferenceSampler final : public Sampler {
public:
    void sample(Snapshot& out, const Snapshot*, double) override {
        const std::uint64_t now = out.tick_ms;
        const double budget_start = static_cast<double>(GetTickCount64());
        refresh_devices(now);

        bool integrated = !out.gpus.empty();
        for (const auto& g : out.gpus)
            if (g.dedicated_total > kGiB) integrated = false;

        std::vector<ProbeTarget> probe_targets;
        const auto probed = probe_.results();
        std::unordered_set<std::uint32_t> alive;

        // Ollama runs one runner per loaded network; its log cannot say which runner a
        // line belongs to, so it is only trusted when one runner holds VRAM.
        int ollama_runners = 0;
        for (const auto& p : out.processes)
            if (p.gpu_dedicated >= kCandidateVram && starts_with(lower(p.image_name), L"ollama") &&
                p.command_line.find(L"runner") != std::wstring::npos)
                ++ollama_runners;
        if (ollama_runners == 1 && now - ollama_log_ms_ >= 2000) {
            ollama_log_ms_ = now;
            if (auto dir = ollama_log_path(); !dir.empty()) ollama_log_.poll(dir);
        }

        for (const auto& p : out.processes) {
            if (p.pid <= 4) continue;
            const auto img = lower(p.image_name);
            const bool hinted = name_hint(img);
            if (!hinted && p.gpu_dedicated < kCandidateVram) continue;
            alive.insert(p.pid);

            auto& sc = scans_[p.pid];
            if (sc.create_time != p.create_time) sc = ProcScan{.create_time = p.create_time};
            const auto delta = p.gpu_dedicated > sc.last_vram ? p.gpu_dedicated - sc.last_vram : sc.last_vram - p.gpu_dedicated;
            const bool due = !sc.scanned || now - sc.last_ms >= kRescanMaxMs ||
                             (now - sc.last_ms >= kRescanMinMs && delta > std::max<std::uint64_t>(64 * kMiB, p.gpu_dedicated / 10));
            if (due && static_cast<double>(GetTickCount64()) - budget_start < kScanBudgetMs) {
                scan(p.pid, sc);
                sc.last_ms = now;
                sc.last_vram = p.gpu_dedicated;
                sc.scanned = true;
            }

            const auto flags = parse_launch_flags(split_command_line(p.command_line));
            if (!hinted && sc.runtimes.empty() && sc.mapped.empty() && flags.weight_paths.empty()) continue;

            InferenceProc ip;
            ip.pid = p.pid;
            ip.image_name = p.image_name;
            ip.vram = p.gpu_dedicated;
            ip.shared = p.gpu_shared;
            ip.working_set = p.working_set;
            for (const auto& r : sc.runtimes) ip.runtimes += (ip.runtimes.empty() ? L"" : L", ") + r;

            ip.files = sc.mapped;
            for (const auto& w : flags.weight_paths) {
                const auto rf = resolve_file(w);
                if (!rf) continue;
                const auto lw = lower(rf->path);
                if (std::any_of(ip.files.begin(), ip.files.end(), [&](const WeightFile& f) { return lower(f.path) == lw; }))
                    continue;
                WeightFile f;
                f.path = rf->path;
                f.bytes = rf->size;
                f.evidence = L"launch arg";
                // Name from the path as given: a snapshot link carries the repository, the blob only a hash.
                f.name = stores_.friendly_name(w);
                ip.files.push_back(std::move(f));
            }
            for (auto& f : ip.files)
                if (f.name.empty()) f.name = stores_.friendly_name(f.path);

            if (ServerProbe::wants_probe(img)) {
                probe_targets.push_back({p.pid, img, flags.api_key});
                if (auto it = probed.find(p.pid); it != probed.end()) {
                    ip.tokens_per_s = it->second.tokens_per_s;
                    ip.busy_slots = it->second.busy_slots;
                    for (const auto& n : it->second.loaded) {
                        // llama-server reports its weights by the path it was given.
                        const auto rf = (n.find(L'\\') != std::wstring::npos || n.find(L'/') != std::wstring::npos)
                                            ? resolve_file(n) : std::nullopt;
                        const auto key = lower(rf ? rf->path : n);
                        if (std::any_of(ip.files.begin(), ip.files.end(), [&](const WeightFile& f) {
                                return lower(f.path) == key || lower(f.name) == key; }))
                            continue;
                        WeightFile f;
                        f.name = rf ? stores_.friendly_name(n) : n;
                        f.evidence = L"reported";
                        ip.files.push_back(std::move(f));
                    }
                }
            }

            // GGUF headers drive both the kind and the estimates.
            std::vector<const GgufInfo*> ggufs;
            // read_gguf rejects anything without the magic, so extensionless blobs are just tried.
            for (const auto& f : ip.files) {
                if (f.path.empty()) continue;
                const auto ext = lower(std::filesystem::path(f.path).extension().wstring());
                if (ext == L".gguf" || ext.empty())
                    if (const auto* g = gguf(f.path)) ggufs.push_back(g);
            }

            ip.kind = classify(img, p.command_line, flags, sc, ggufs);
            build_segments(ip, img, p.command_line, flags, ggufs, integrated, ollama_runners);
            out.inference.push_back(std::move(ip));
        }

        std::erase_if(scans_, [&](const auto& kv) { return !alive.contains(kv.first); });
        probe_.set_targets(std::move(probe_targets));
        std::sort(out.inference.begin(), out.inference.end(),
                  [](const InferenceProc& a, const InferenceProc& b) { return a.vram > b.vram; });
    }

private:
    static std::wstring classify(const std::wstring& img, const std::wstring& cmd, const LaunchFlags& flags,
                                 const ProcScan& sc, const std::vector<const GgufInfo*>& ggufs) {
        if (flags.diffusion) return L"diffusion";
        const auto lc = lower(cmd);
        for (auto k : {L"diffusers", L"comfyui", L"stable-diffusion", L"stable_diffusion", L"invokeai", L"sd-scripts", L"fooocus"})
            if (lc.find(k) != std::wstring::npos) return L"diffusion";
        for (const auto* g : ggufs) {
            const auto k = g->kind();
            if (k != L"adapter" && k != L"vision") return k;
        }
        auto has = [&](std::wstring_view r) { return std::find(sc.runtimes.begin(), sc.runtimes.end(), r) != sc.runtimes.end(); };
        if (has(L"whisper.cpp") || starts_with(img, L"whisper")) return L"speech";
        if (has(L"llama.cpp") || has(L"ggml") || name_hint(img) || lc.find(L"vllm") != std::wstring::npos) return L"text";
        return L"runtime";
    }

    void build_segments(InferenceProc& ip, const std::wstring& img, const std::wstring& cmd, const LaunchFlags& flags,
                        const std::vector<const GgufInfo*>& ggufs, bool integrated, int ollama_runners) {
        Part weights, kv, compute;

        // Exact: the runtime's own account of what it put on the GPU.
        const LoadTally* tally = nullptr;
        if (starts_with(img, L"ollama") && cmd.find(L"runner") != std::wstring::npos && ollama_runners == 1 &&
            ip.vram >= kCandidateVram)
            tally = &ollama_log_.tally;
        else if (!flags.log_file.empty()) {
            auto& t = llama_logs_[lower(flags.log_file)];
            t.poll(flags.log_file);
            tally = &t.tally;
        }
        if (tally) {
            auto take = [&](const char* k, Part& p) {
                if (auto it = tally->bytes.find(k); it != tally->bytes.end()) p = {it->second, 0, true};
            };
            take("weights", weights);
            take("kv cache", kv);
            take("compute", compute);
        }

        // Estimated: weights from the GGUF tensor table at the offload depth, KV from the
        // per-token footprint at the configured context.
        if (!weights.known) {
            std::uint64_t w = 0;
            for (const auto* g : ggufs) w += g->gpu_weight_bytes(flags.n_gpu_layers);
            if (!w && ggufs.empty()) {
                for (const auto& f : ip.files)
                    if (!f.path.empty()) w += f.bytes;
            }
            if (w) weights = {w, 2, true};
        }
        if (!kv.known) {
            for (const auto* g : ggufs) {
                if (!g->kv_elems_per_token) continue;
                const std::uint32_t ctx = flags.ctx ? flags.ctx : kDefaultCtx;
                const double per_elem = (kv_type_bytes(flags.cache_type_k) + kv_type_bytes(flags.cache_type_v)) / 2.0;
                kv = {static_cast<std::uint64_t>(static_cast<double>(g->kv_elems_per_token) * ctx * per_elem), 2, true};
                break;
            }
        }

        if (ip.vram) {
            const auto b = build_breakdown(ip.vram, weights, kv, compute);
            for (const auto& s : b.segments)
                ip.segments.push_back({s.label, s.bytes, static_cast<Provenance>(s.source)});
            if (b.weights_exceed_vram)
                ip.warnings.push_back(L"weights exceed the VRAM held; part of the network runs from system memory");
        }
        if (!integrated && ip.shared > kSpillWarn)
            ip.warnings.push_back(std::to_wstring(ip.shared / kMiB) + L" MiB spilled into shared system memory");
    }

    const GgufInfo* gguf(const std::wstring& path) {
        const auto sz = file_size(path);
        auto& c = gguf_[lower(path)];
        if (c.size != sz) {
            c.size = sz;
            c.info.reset();
            FileWindow w(path);
            if (w.ok() && sz)
                c.info = read_gguf([&](std::uint64_t off, void* dst, std::size_t len) { return w.read(off, dst, len); }, sz);
        }
        return c.info ? &*c.info : nullptr;
    }

    void scan(std::uint32_t pid, ProcScan& sc) {
        sc.runtimes.clear();
        sc.mapped.clear();
        Handle h(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid));
        if (!h.h) return;

        std::vector<HMODULE> mods(1024);
        DWORD needed = 0;
        if (EnumProcessModulesEx(h.h, mods.data(), static_cast<DWORD>(mods.size() * sizeof(HMODULE)), &needed, LIST_MODULES_ALL)) {
            mods.resize(std::min<std::size_t>(mods.size(), needed / sizeof(HMODULE)));
            wchar_t name[MAX_PATH];
            for (HMODULE m : mods) {
                if (!GetModuleBaseNameW(h.h, m, name, MAX_PATH)) continue;
                const auto n = lower(name);
                for (const auto& r : kRuntimes) {
                    if (n.find(r.needle) == std::wstring::npos) continue;
                    if (std::find(sc.runtimes.begin(), sc.runtimes.end(), r.label) == sc.runtimes.end())
                        sc.runtimes.emplace_back(r.label);
                }
            }
        }
        scan_mapped(h.h, sc);
    }

    // Weights loaded by mmap show up as mapped views of the file. Region sizes give the
    // mapped extent; a sparse page sample gives how much of it is resident.
    void scan_mapped(HANDLE h, ProcScan& sc) {
        struct Acc { std::uint64_t mapped = 0; double resident = 0; };
        std::unordered_map<std::wstring, Acc> files;
        MEMORY_BASIC_INFORMATION mbi{};
        std::uintptr_t addr = 0;
        wchar_t dev[MAX_PATH * 2];
        for (int guard = 0; guard < 200'000 && VirtualQueryEx(h, reinterpret_cast<void*>(addr), &mbi, sizeof mbi); ++guard) {
            const auto next = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            if (next <= addr) break;
            addr = next;
            if (mbi.Type != MEM_MAPPED || mbi.State != MEM_COMMIT || mbi.RegionSize < 1 * kMiB) continue;
            if (!GetMappedFileNameW(h, mbi.BaseAddress, dev, static_cast<DWORD>(std::size(dev)))) continue;
            const auto path = dos_path(dev);
            if (path.empty()) continue;
            bool vague = false;
            const auto lp = lower(path);
            const auto ext = lower(std::filesystem::path(path).extension().wstring());
            // Ollama and Hugging Face store weights as extensionless content-addressed blobs.
            const bool blob = ext.empty() && lp.find(L"blobs") != std::wstring::npos;
            if (!blob && !is_weight_ext(ext, vague)) continue;
            auto& a = files[path];
            a.mapped += mbi.RegionSize;
            a.resident += resident_fraction(h, mbi) * static_cast<double>(mbi.RegionSize);
        }
        for (auto& [path, a] : files) {
            bool vague = false;
            const auto ext = lower(std::filesystem::path(path).extension().wstring());
            is_weight_ext(ext, vague);
            const auto rf = resolve_file(path);
            if (ext.empty() && !(rf && (rf->gguf || rf->safetensors))) continue;
            const auto sz = rf ? rf->size : 0;
            const auto biggest = std::max(sz, a.mapped);
            // .bin and .pt double as caches and indexes; demand real size before calling them weights.
            if (biggest < (vague ? 64 * kMiB : 4 * kMiB)) continue;
            WeightFile f;
            f.path = rf ? rf->path : path;
            f.bytes = sz ? sz : a.mapped;
            f.resident = static_cast<std::uint64_t>(a.resident);
            f.evidence = L"mapped";
            sc.mapped.push_back(std::move(f));
        }
    }

    static double resident_fraction(HANDLE h, const MEMORY_BASIC_INFORMATION& mbi) {
        constexpr std::size_t kSamples = 16;
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        const std::size_t pages = mbi.RegionSize / si.dwPageSize;
        if (!pages) return 0;
        const std::size_t n = std::min(pages, kSamples);
        std::array<PSAPI_WORKING_SET_EX_INFORMATION, kSamples> q{};
        for (std::size_t i = 0; i < n; ++i)
            q[i].VirtualAddress = static_cast<char*>(mbi.BaseAddress) + (pages * i / n) * si.dwPageSize;
        if (!QueryWorkingSetEx(h, q.data(), static_cast<DWORD>(n * sizeof q[0]))) return 0;
        std::size_t valid = 0;
        for (std::size_t i = 0; i < n; ++i) valid += q[i].VirtualAttributes.Valid ? 1 : 0;
        return static_cast<double>(valid) / static_cast<double>(n);
    }

    void refresh_devices(std::uint64_t now) {
        if (devices_ms_ && now - devices_ms_ < 60'000) return;
        devices_ms_ = now;
        devices_.clear();
        wchar_t target[MAX_PATH];
        for (wchar_t d = L'A'; d <= L'Z'; ++d) {
            const wchar_t drive[3] = {d, L':', 0};
            if (QueryDosDeviceW(drive, target, MAX_PATH)) devices_.emplace_back(target, drive);
        }
    }

    std::wstring dos_path(const std::wstring& dev) const {
        for (const auto& [nt, drive] : devices_) {
            if (dev.size() > nt.size() && dev.compare(0, nt.size(), nt) == 0 && dev[nt.size()] == L'\\')
                return drive + dev.substr(nt.size());
        }
        return {};
    }

    static std::wstring ollama_log_path() {
        wchar_t buf[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
        if (!n || n >= MAX_PATH) return {};
        return std::wstring(buf, n) + L"\\Ollama\\server.log";
    }

    std::unordered_map<std::uint32_t, ProcScan> scans_;
    std::unordered_map<std::wstring, CachedGguf> gguf_;
    std::unordered_map<std::wstring, LogTail> llama_logs_;
    std::vector<std::pair<std::wstring, std::wstring>> devices_;   // NT device -> "C:"
    std::uint64_t devices_ms_ = 0;
    LogTail       ollama_log_;
    std::uint64_t ollama_log_ms_ = 0;
    WeightStores  stores_;
    ServerProbe   probe_;
};

} // namespace

std::unique_ptr<Sampler> make_inference_sampler() { return std::make_unique<InferenceSampler>(); }

} // namespace ring0

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Readers for the inference breakdown that touch no Win32: GGUF headers, launch
// flags, runtime load logs, and the arithmetic that splits a process's VRAM. Kept
// apart from the sampler so they run against synthetic input in tests.

namespace ring0::inference {

// ---------------------------------------------------------------- GGUF

// Reads `len` bytes at `off` into `dst`; false past the end. The header of a big
// file carries the whole vocabulary, so it is pulled on demand rather than slurped.
using ByteReader = std::function<bool(std::uint64_t off, void* dst, std::size_t len)>;

struct GgufInfo {
    std::string   arch;
    std::string   general_type;            // "model", "adapter", ...
    std::uint32_t block_count = 0;
    bool          causal = true;
    std::uint64_t kv_elems_per_token = 0;  // K+V elements per token across layers; times type bytes = cache
    std::uint64_t data_bytes = 0;          // sum of every tensor
    std::uint64_t shared_bytes = 0;        // tensors outside blk.N (embeddings, output head)
    std::uint64_t token_embd_bytes = 0;    // the input embedding table, which llama.cpp keeps in host memory
    std::vector<std::uint64_t> block_bytes;

    // llama.cpp offloads the last n layers, then the output head once every block is on the GPU.
    std::uint64_t gpu_weight_bytes(int n_gpu_layers) const {
        if (block_bytes.empty()) return data_bytes;
        const int n = static_cast<int>(block_bytes.size());
        if (n_gpu_layers < 0 || n_gpu_layers > n) return data_bytes - token_embd_bytes;
        std::uint64_t sum = 0;
        for (int i = n - n_gpu_layers; i < n; ++i) sum += block_bytes[static_cast<std::size_t>(i)];
        return sum;
    }

    std::wstring kind() const {
        if (general_type == "adapter") return L"adapter";
        if (arch == "whisper") return L"speech";
        if (!causal || arch.find("bert") != std::string::npos) return L"embedding";
        if (arch == "clip") return L"vision";
        return L"text";
    }
};

namespace detail {

struct Cursor {
    const ByteReader& rd;
    std::uint64_t     pos = 0;
    bool              ok = true;

    template <class T>
    T get() {
        T v{};
        if (ok && !rd(pos, &v, sizeof v)) ok = false;
        pos += sizeof v;
        return v;
    }
    std::string str() {
        const auto n = get<std::uint64_t>();
        if (!ok || n > (1u << 20)) { ok = false; return {}; }
        std::string s(static_cast<std::size_t>(n), '\0');
        if (n && !rd(pos, s.data(), s.size())) ok = false;
        pos += n;
        return s;
    }
};

enum : std::uint32_t {
    kU8, kI8, kU16, kI16, kU32, kI32, kF32, kBool, kString, kArray, kU64, kI64, kF64
};

inline std::size_t scalar_size(std::uint32_t t) {
    switch (t) {
    case kU8: case kI8: case kBool: return 1;
    case kU16: case kI16: return 2;
    case kU32: case kI32: case kF32: return 4;
    case kU64: case kI64: case kF64: return 8;
    default: return 0;
    }
}

struct Value {
    std::optional<std::uint64_t> num;
    std::optional<std::string>   text;
    std::vector<std::uint64_t>   nums;   // integer arrays (per-layer head counts)
};

inline std::uint64_t read_int(Cursor& c, std::uint32_t t) {
    switch (t) {
    case kU8:  return c.get<std::uint8_t>();
    case kI8:  return static_cast<std::uint64_t>(std::max<std::int8_t>(0, c.get<std::int8_t>()));
    case kBool: return c.get<std::uint8_t>() ? 1 : 0;
    case kU16: return c.get<std::uint16_t>();
    case kI16: return static_cast<std::uint64_t>(std::max<std::int16_t>(0, c.get<std::int16_t>()));
    case kU32: return c.get<std::uint32_t>();
    case kI32: return static_cast<std::uint64_t>(std::max(0, c.get<std::int32_t>()));
    case kU64: return c.get<std::uint64_t>();
    case kI64: return static_cast<std::uint64_t>(std::max<std::int64_t>(0, c.get<std::int64_t>()));
    case kF32: c.get<float>(); return 0;
    case kF64: c.get<double>(); return 0;
    default: c.ok = false; return 0;
    }
}

inline Value read_value(Cursor& c, std::uint32_t t, bool keep_array) {
    Value v;
    if (t == kString) { v.text = c.str(); return v; }
    if (t == kArray) {
        const auto et = c.get<std::uint32_t>();
        const auto n = c.get<std::uint64_t>();
        if (!c.ok) return v;
        if (et == kString) {
            for (std::uint64_t i = 0; i < n && c.ok; ++i) {
                const auto len = c.get<std::uint64_t>();
                c.pos += len;
            }
        } else if (const auto sz = scalar_size(et); sz) {
            if (keep_array && n <= 4096) {
                for (std::uint64_t i = 0; i < n && c.ok; ++i) v.nums.push_back(read_int(c, et));
            } else {
                c.pos += sz * n;
            }
        } else {
            c.ok = false;   // nested arrays do not occur in practice
        }
        return v;
    }
    v.num = read_int(c, t);
    return v;
}

inline std::optional<std::uint32_t> block_index(std::string_view name) {
    if (name.substr(0, 4) != "blk.") return std::nullopt;
    std::uint32_t n = 0;
    std::size_t i = 4;
    if (i >= name.size() || name[i] < '0' || name[i] > '9') return std::nullopt;
    while (i < name.size() && name[i] >= '0' && name[i] <= '9') n = n * 10 + static_cast<std::uint32_t>(name[i++] - '0');
    return n;
}

} // namespace detail

inline std::optional<GgufInfo> read_gguf(const ByteReader& rd, std::uint64_t file_size) {
    using namespace detail;
    Cursor c{rd};
    char magic[4];
    if (!rd(0, magic, 4) || std::memcmp(magic, "GGUF", 4) != 0) return std::nullopt;
    c.pos = 4;
    const auto version = c.get<std::uint32_t>();
    if (version < 2) return std::nullopt;
    const auto n_tensors = c.get<std::uint64_t>();
    const auto n_kv = c.get<std::uint64_t>();
    if (!c.ok || n_tensors > 100000 || n_kv > 100000) return std::nullopt;

    std::map<std::string, Value> kv;
    for (std::uint64_t i = 0; i < n_kv && c.ok; ++i) {
        auto key = c.str();
        const auto t = c.get<std::uint32_t>();
        // Only per-layer integer arrays are worth keeping; the vocabulary is skipped by length.
        const bool keep = key.find("head_count") != std::string::npos;
        auto v = read_value(c, t, keep);
        if (c.ok) kv.emplace(std::move(key), std::move(v));
    }
    if (!c.ok) return std::nullopt;

    struct Tensor { std::uint64_t offset; std::optional<std::uint32_t> block; bool embd; };
    std::vector<Tensor> tensors;
    tensors.reserve(static_cast<std::size_t>(n_tensors));
    for (std::uint64_t i = 0; i < n_tensors && c.ok; ++i) {
        auto name = c.str();
        const auto dims = c.get<std::uint32_t>();
        if (dims > 8) { c.ok = false; break; }
        c.pos += 8ull * dims;
        c.get<std::uint32_t>();                 // ggml type
        const auto off = c.get<std::uint64_t>();
        tensors.push_back({off, block_index(name), name == "token_embd.weight"});
    }
    if (!c.ok) return std::nullopt;

    GgufInfo g;
    auto text = [&](const std::string& k) -> std::string {
        auto it = kv.find(k);
        return it != kv.end() && it->second.text ? *it->second.text : std::string{};
    };
    auto num = [&](const std::string& k) -> std::optional<std::uint64_t> {
        auto it = kv.find(k);
        if (it == kv.end()) return std::nullopt;
        return it->second.num;
    };
    g.arch = text("general.architecture");
    g.general_type = text("general.type");
    const std::string a = g.arch + ".";
    g.block_count = static_cast<std::uint32_t>(num(a + "block_count").value_or(0));
    if (auto causal = num(a + "attention.causal")) g.causal = *causal != 0;

    std::uint64_t alignment = num("general.alignment").value_or(32);
    if (alignment == 0) alignment = 32;
    const std::uint64_t data_start = (c.pos + alignment - 1) / alignment * alignment;

    // Tensor sizes fall out of the offset table: each runs to the next one's start.
    std::vector<std::size_t> order(tensors.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t x, std::size_t y) { return tensors[x].offset < tensors[y].offset; });
    const std::uint64_t data_len = file_size > data_start ? file_size - data_start : 0;
    if (g.block_count) g.block_bytes.assign(g.block_count, 0);
    for (std::size_t k = 0; k < order.size(); ++k) {
        const auto& t = tensors[order[k]];
        const std::uint64_t end = k + 1 < order.size() ? tensors[order[k + 1]].offset : data_len;
        const std::uint64_t sz = end > t.offset ? end - t.offset : 0;
        g.data_bytes += sz;
        if (t.block && *t.block < g.block_bytes.size()) g.block_bytes[*t.block] += sz;
        else g.shared_bytes += sz;
        if (t.embd) g.token_embd_bytes += sz;
    }

    if (g.causal && g.block_count) {
        const auto heads = num(a + "attention.head_count").value_or(0);
        const auto embd = num(a + "embedding_length").value_or(0);
        const auto head_dim = heads ? embd / heads : 0;
        const auto k_len = num(a + "attention.key_length").value_or(head_dim);
        const auto v_len = num(a + "attention.value_length").value_or(head_dim);
        std::uint64_t kv_heads_total = 0;
        if (auto it = kv.find(a + "attention.head_count_kv"); it != kv.end()) {
            if (!it->second.nums.empty())
                for (auto h : it->second.nums) kv_heads_total += h;
            else if (it->second.num)
                kv_heads_total = *it->second.num * g.block_count;
        } else {
            kv_heads_total = heads * g.block_count;
        }
        g.kv_elems_per_token = kv_heads_total * (k_len + v_len);
    }
    return g;
}

// Bytes per element of a KV cache type as llama.cpp names them (-ctk / -ctv).
inline double kv_type_bytes(std::wstring_view t) {
    if (t.empty() || t == L"f16" || t == L"bf16") return 2.0;
    if (t == L"f32") return 4.0;
    if (t == L"q8_0") return 34.0 / 32.0;
    if (t == L"q4_0") return 18.0 / 32.0;
    if (t == L"q4_1") return 20.0 / 32.0;
    if (t == L"q5_0") return 22.0 / 32.0;
    if (t == L"q5_1") return 24.0 / 32.0;
    if (t == L"iq4_nl") return 18.0 / 32.0;
    return 2.0;
}

// ---------------------------------------------------------------- command line

// CommandLineToArgvW rules, minus the shell32 dependency so the parser is testable.
inline std::vector<std::wstring> split_command_line(std::wstring_view s) {
    std::vector<std::wstring> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == L' ' || s[i] == L'\t')) ++i;
        if (i >= s.size()) break;
        std::wstring arg;
        bool quoted = false;
        while (i < s.size() && (quoted || (s[i] != L' ' && s[i] != L'\t'))) {
            if (s[i] == L'\\') {
                std::size_t n = 0;
                while (i < s.size() && s[i] == L'\\') { ++n; ++i; }
                if (i < s.size() && s[i] == L'"') {
                    arg.append(n / 2, L'\\');
                    if (n % 2) { arg.push_back(L'"'); ++i; }
                } else {
                    arg.append(n, L'\\');
                }
                continue;
            }
            if (s[i] == L'"') { quoted = !quoted; ++i; continue; }
            arg.push_back(s[i++]);
        }
        out.push_back(std::move(arg));
    }
    return out;
}

struct LaunchFlags {
    std::vector<std::wstring> weight_paths;   // -m, --model, --mmproj, -md, --lora, --diffusion-model, --vae
    bool          diffusion = false;
    int           n_gpu_layers = -1;          // -1: not given
    std::uint32_t ctx = 0;                    // 0: not given
    std::wstring  cache_type_k, cache_type_v;
    std::wstring  api_key;
    std::wstring  log_file;
};

inline LaunchFlags parse_launch_flags(const std::vector<std::wstring>& argv) {
    LaunchFlags f;
    auto to_int = [](const std::wstring& s) -> long long {
        try { return std::stoll(s); } catch (...) { return -1; }
    };
    for (std::size_t i = 1; i < argv.size(); ++i) {
        std::wstring flag = argv[i];
        std::wstring val;
        bool inline_val = false;
        if (auto eq = flag.find(L'='); eq != std::wstring::npos && flag.size() > 1 && flag[0] == L'-') {
            val = flag.substr(eq + 1);
            flag.resize(eq);
            inline_val = true;
        }
        auto take = [&]() -> std::wstring {
            if (inline_val) return val;
            if (i + 1 < argv.size()) return argv[++i];
            return {};
        };
        if (flag == L"-m" || flag == L"--model" || flag == L"--mmproj" || flag == L"-md" ||
            flag == L"--model-draft" || flag == L"--lora" || flag == L"--vae" || flag == L"--llm" ||
            flag == L"--clip_l" || flag == L"--t5xxl") {
            if (auto v = take(); !v.empty()) f.weight_paths.push_back(v);
        } else if (flag == L"--diffusion-model") {
            f.diffusion = true;
            if (auto v = take(); !v.empty()) f.weight_paths.push_back(v);
        } else if (flag == L"-ngl" || flag == L"--n-gpu-layers" || flag == L"--gpu-layers") {
            const auto n = to_int(take());
            if (n >= 0) f.n_gpu_layers = static_cast<int>(std::min<long long>(n, 100000));
        } else if (flag == L"-c" || flag == L"--ctx-size" || flag == L"--ctx_size") {
            const auto n = to_int(take());
            if (n > 0) f.ctx = static_cast<std::uint32_t>(std::min<long long>(n, 1ll << 24));
        } else if (flag == L"-ctk" || flag == L"--cache-type-k") {
            f.cache_type_k = take();
        } else if (flag == L"-ctv" || flag == L"--cache-type-v") {
            f.cache_type_v = take();
        } else if (flag == L"--api-key") {
            f.api_key = take();
        } else if (flag == L"--log-file") {
            f.log_file = take();
        }
    }
    return f;
}

// ---------------------------------------------------------------- load logs

struct LogBuffer {
    std::string   label;      // "weights", "kv cache", "compute"
    std::string   device;     // "CUDA0", "Vulkan0", "CPU", ...
    std::uint64_t bytes = 0;
};

inline bool device_is_host(std::string_view d) {
    return d.empty() || d.substr(0, 3) == "CPU" || d.find("_Host") != std::string_view::npos;
}

inline std::optional<std::uint64_t> parse_size(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '"')) s.remove_prefix(1);
    std::size_t i = 0;
    double v = 0;
    bool any = false;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i++] - '0'); any = true; }
    if (i < s.size() && s[i] == '.') {
        double scale = 0.1;
        for (++i; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) { v += (s[i] - '0') * scale; scale /= 10; any = true; }
    }
    if (!any) return std::nullopt;
    while (i < s.size() && s[i] == ' ') ++i;
    auto unit = s.substr(i, 3);
    double mul = 1;
    if (unit.substr(0, 3) == "GiB" || unit.substr(0, 2) == "GB") mul = 1024.0 * 1024 * 1024;
    else if (unit.substr(0, 3) == "MiB" || unit.substr(0, 2) == "MB") mul = 1024.0 * 1024;
    else if (unit.substr(0, 3) == "KiB" || unit.substr(0, 2) == "KB") mul = 1024.0;
    else if (unit.empty() || unit[0] != 'B') return std::nullopt;
    return static_cast<std::uint64_t>(v * mul);
}

// llama.cpp: "load_tensors:   CUDA0 model buffer size =  4403.49 MiB"
//            "llama_kv_cache: CUDA0 KV buffer size =   512.00 MiB"
//            "llama_context:  CUDA0 compute buffer size =   300.01 MiB"
// Ollama:    msg="model weights" device=CUDA0 size="4.3 GiB"
inline std::optional<LogBuffer> parse_log_line(std::string_view line) {
    LogBuffer b;
    if (auto m = line.find("msg=\""); m != std::string_view::npos) {
        auto rest = line.substr(m + 5);
        auto q = rest.find('"');
        if (q == std::string_view::npos) return std::nullopt;
        auto msg = rest.substr(0, q);
        if (msg == "model weights") b.label = "weights";
        else if (msg == "kv cache") b.label = "kv cache";
        else if (msg == "compute graph") b.label = "compute";
        else return std::nullopt;
        auto d = line.find("device=");
        auto s = line.find("size=");
        if (d == std::string_view::npos || s == std::string_view::npos) return std::nullopt;
        auto dv = line.substr(d + 7);
        dv = dv.substr(0, dv.find(' '));
        b.device = std::string(dv);
        auto sz = parse_size(line.substr(s + 5));
        if (!sz) return std::nullopt;
        b.bytes = *sz;
        return b;
    }
    auto bs = line.find(" buffer size");
    if (bs == std::string_view::npos) return std::nullopt;
    auto head = line.substr(0, bs);
    auto sp = head.rfind(' ');
    if (sp == std::string_view::npos) return std::nullopt;
    auto what = head.substr(sp + 1);
    if (what == "model") b.label = "weights";
    else if (what == "KV") b.label = "kv cache";
    else if (what == "compute") b.label = "compute";
    else return std::nullopt;
    head = head.substr(0, sp);
    while (!head.empty() && head.back() == ' ') head.remove_suffix(1);
    auto sp2 = head.rfind(' ');
    b.device = std::string(sp2 == std::string_view::npos ? head : head.substr(sp2 + 1));
    auto eq = line.find('=', bs);
    if (eq == std::string_view::npos) return std::nullopt;
    auto sz = parse_size(line.substr(eq + 1));
    if (!sz) return std::nullopt;
    b.bytes = *sz;
    return b;
}

// Folds a log into the latest load's GPU-side totals per label. A new "weights"
// line after kv/compute were seen means a reload, which starts the tally over.
struct LoadTally {
    std::map<std::string, std::uint64_t> bytes;
    bool started_kv = false;

    void feed(std::string_view line) {
        auto b = parse_log_line(line);
        if (!b || device_is_host(b->device)) return;
        if (b->label == "weights" && started_kv) { bytes.clear(); started_kv = false; }
        if (b->label != "weights") started_kv = true;
        bytes[b->label] += b->bytes;
    }
};

// ---------------------------------------------------------------- breakdown

struct Part {
    std::uint64_t bytes = 0;
    int           source = 2;   // matches Provenance: 0 exact, 1 reported, 2 estimated
    bool          known = false;
};

struct Breakdown {
    struct Seg { std::wstring label; std::uint64_t bytes; int source; };
    std::vector<Seg> segments;
    bool weights_exceed_vram = false;
};

// The OS total is authoritative; parts that do not fit are clipped in order and the
// remainder is whatever the runtime holds beyond them.
inline Breakdown build_breakdown(std::uint64_t vram, Part weights, Part kv, Part compute) {
    Breakdown out;
    std::uint64_t left = vram;
    auto add = [&](const wchar_t* label, const Part& p) {
        if (!p.known || p.bytes == 0) return;
        const auto take = std::min(p.bytes, left);
        left -= take;
        if (take) out.segments.push_back({label, take, p.source});
    };
    out.weights_exceed_vram = weights.known && weights.bytes > vram + vram / 20;
    add(L"weights", weights);
    add(L"kv cache", kv);
    add(L"compute", compute);
    // The remainder is only as trustworthy as the least trustworthy part it was carved by.
    int worst = 0;
    for (const auto* p : {&weights, &kv, &compute})
        if (p->known) worst = std::max(worst, p->source);
    if (!compute.known) worst = 2;
    if (left) out.segments.push_back({compute.known ? L"other" : L"compute + runtime", left, worst});
    return out;
}

} // namespace ring0::inference

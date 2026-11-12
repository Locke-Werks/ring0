// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/inference_parse.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace ring0::inference;

static int failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

namespace {

struct Gguf {
    std::vector<unsigned char> b;
    template <class T> void put(T v) { const auto* p = reinterpret_cast<const unsigned char*>(&v); b.insert(b.end(), p, p + sizeof v); }
    void str(const std::string& s) { put<std::uint64_t>(s.size()); b.insert(b.end(), s.begin(), s.end()); }
    void kv_u32(const std::string& k, std::uint32_t v) { str(k); put<std::uint32_t>(4); put(v); }
    void kv_str(const std::string& k, const std::string& v) { str(k); put<std::uint32_t>(8); str(v); }
    void kv_bool(const std::string& k, bool v) { str(k); put<std::uint32_t>(7); put<std::uint8_t>(v ? 1 : 0); }
};

// Two blocks of 1000 bytes each, a 500-byte embedding table, and a vocabulary array
// that has to be skipped by length.
std::vector<unsigned char> make_gguf(bool causal) {
    Gguf g;
    g.b.insert(g.b.end(), {'G', 'G', 'U', 'F'});
    g.put<std::uint32_t>(3);
    g.put<std::uint64_t>(5);     // tensors
    g.put<std::uint64_t>(8);     // kv
    g.kv_str("general.architecture", "llama");
    g.kv_u32("llama.block_count", 2);
    g.kv_u32("llama.attention.head_count", 8);
    g.kv_u32("llama.attention.head_count_kv", 2);
    g.kv_u32("llama.embedding_length", 512);
    g.kv_bool("llama.attention.causal", causal);
    g.kv_u32("general.alignment", 32);
    g.str("tokenizer.ggml.tokens");
    g.put<std::uint32_t>(9);
    g.put<std::uint32_t>(8);
    g.put<std::uint64_t>(3);
    g.str("a"); g.str("bb"); g.str("ccc");

    auto tensor = [&](const std::string& name, std::uint64_t off) {
        g.str(name);
        g.put<std::uint32_t>(1);
        g.put<std::uint64_t>(10);
        g.put<std::uint32_t>(0);
        g.put<std::uint64_t>(off);
    };
    tensor("token_embd.weight", 0);
    tensor("blk.0.attn_q.weight", 500);
    tensor("blk.0.ffn.weight", 1000);
    tensor("blk.1.attn_q.weight", 1500);
    tensor("blk.1.ffn.weight", 2000);
    while (g.b.size() % 32) g.b.push_back(0);
    g.b.resize(g.b.size() + 2500, 0);
    return g.b;
}

std::optional<GgufInfo> parse(const std::vector<unsigned char>& f) {
    return read_gguf([&](std::uint64_t off, void* dst, std::size_t len) {
        if (off + len > f.size()) return false;
        std::memcpy(dst, f.data() + off, len);
        return true;
    }, f.size());
}

} // namespace

int main() {
    {
        auto g = parse(make_gguf(true));
        CHECK(g.has_value());
        if (g) {
            CHECK(g->arch == "llama");
            CHECK(g->block_count == 2);
            CHECK(g->data_bytes == 2500);
            CHECK(g->shared_bytes == 500);
            CHECK(g->block_bytes.size() == 2 && g->block_bytes[0] == 1000 && g->block_bytes[1] == 1000);
            CHECK(g->gpu_weight_bytes(1) == 1000);
            // Full offload leaves the input embedding table in host memory.
            CHECK(g->token_embd_bytes == 500);
            CHECK(g->gpu_weight_bytes(-1) == 2000);
            CHECK(g->gpu_weight_bytes(99) == 2000);
            // 2 kv heads x 2 layers x (64 + 64) head dim.
            CHECK(g->kv_elems_per_token == 2u * 2u * 128u);
            CHECK(g->kind() == L"text");
        }
        auto e = parse(make_gguf(false));
        CHECK(e && e->kind() == L"embedding" && e->kv_elems_per_token == 0);

        auto bad = make_gguf(true);
        bad[0] = 'X';
        CHECK(!parse(bad));
        auto cut = make_gguf(true);
        cut.resize(60);
        CHECK(!parse(cut));
    }

    {
        auto argv = split_command_line(LR"("C:\llama\llama-server.exe" -m "D:\w\q 4.gguf" -ngl 33 --ctx-size=8192 -ctk q8_0 --api-key s3cret --lora a.gguf)");
        CHECK(argv.size() == 12);
        CHECK(argv[2] == L"D:\\w\\q 4.gguf");
        auto f = parse_launch_flags(argv);
        CHECK(f.weight_paths.size() == 2);
        CHECK(f.n_gpu_layers == 33);
        CHECK(f.ctx == 8192);
        CHECK(f.cache_type_k == L"q8_0");
        CHECK(f.api_key == L"s3cret");
        CHECK(!f.diffusion);
        auto d = parse_launch_flags(split_command_line(L"sd.exe --diffusion-model flux.gguf --vae ae.safetensors"));
        CHECK(d.diffusion && d.weight_paths.size() == 2);
        const auto quoted = split_command_line(LR"(a "b\"c" d\\e)");
        CHECK(quoted.size() == 3);
        CHECK(quoted.size() == 3 && quoted[1] == std::wstring(1, L'b') + L'"' + L'c');
        CHECK(kv_type_bytes(L"q8_0") > 1.0 && kv_type_bytes(L"q8_0") < 1.1);
    }

    {
        auto a = parse_log_line("load_tensors:        CUDA0 model buffer size =  4403.50 MiB");
        CHECK(a && a->label == "weights" && a->device == "CUDA0");
        CHECK(a && a->bytes == static_cast<std::uint64_t>(4403.5 * 1048576));
        auto b = parse_log_line("llama_kv_cache_unified:      CUDA0 KV buffer size =   512.00 MiB");
        CHECK(b && b->label == "kv cache");
        auto c = parse_log_line("time=2026-11-11T10:00:00 level=INFO source=device.go msg=\"compute graph\" device=CUDA0 size=\"1.5 GiB\"");
        CHECK(c && c->label == "compute" && c->bytes == 1610612736ull);
        auto h = parse_log_line("load_tensors:   CPU_Mapped model buffer size =   281.81 MiB");
        CHECK(h && device_is_host(h->device));
        CHECK(!parse_log_line("llama_context: n_ctx = 4096"));
        CHECK(!parse_log_line("msg=\"model weights\" device=CUDA0 size=\"lots\""));

        LoadTally t;
        t.feed("load_tensors: CUDA0 model buffer size = 100.00 MiB");
        t.feed("load_tensors: CPU_Mapped model buffer size = 50.00 MiB");
        t.feed("llama_kv_cache: CUDA0 KV buffer size = 10.00 MiB");
        CHECK(t.bytes["weights"] == 100ull * 1048576 && t.bytes["kv cache"] == 10ull * 1048576);
        // A second load replaces the first instead of adding to it.
        t.feed("load_tensors: CUDA0 model buffer size = 200.00 MiB");
        CHECK(t.bytes["weights"] == 200ull * 1048576 && t.bytes.count("kv cache") == 0);
    }

    {
        auto b = build_breakdown(1000, {600, 0, true}, {100, 2, true}, {});
        CHECK(b.segments.size() == 3);
        CHECK(b.segments[0].bytes == 600 && b.segments[0].source == 0);
        CHECK(b.segments[2].label == L"compute + runtime" && b.segments[2].bytes == 300 && b.segments[2].source == 2);
        CHECK(!b.weights_exceed_vram);

        auto over = build_breakdown(1000, {2000, 2, true}, {100, 2, true}, {});
        CHECK(over.weights_exceed_vram);
        CHECK(over.segments.size() == 1 && over.segments[0].bytes == 1000);

        auto exact = build_breakdown(1000, {500, 0, true}, {200, 0, true}, {200, 0, true});
        CHECK(exact.segments.back().label == L"other" && exact.segments.back().bytes == 100 && exact.segments.back().source == 0);

        auto none = build_breakdown(1000, {}, {}, {});
        CHECK(none.segments.size() == 1 && none.segments[0].bytes == 1000);
    }

    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}

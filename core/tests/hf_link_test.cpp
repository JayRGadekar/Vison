#include "vison/hf_link.h"
#include <cstdlib>
#include <iostream>

using namespace vison;

// assert() is compiled out in Release, so checks must not rely on it.
#define CHECK(c) do { if (!(c)) { std::cerr << "FAILED line " << __LINE__ << ": " #c << std::endl; std::exit(1); } } while (0)

int main() {
    auto a = parse_hf_link("https://huggingface.co/org/repo");
    CHECK(a.ok && a.repo == "org/repo" && a.file.empty() && a.revision == "main");

    auto b = parse_hf_link("hf.co/org/repo/resolve/main/dir/My%20File.gguf?download=true");
    CHECK(b.ok && b.file == "dir/My File.gguf");
    CHECK(hf_resolve_url(b, b.file) == "https://huggingface.co/org/repo/resolve/main/dir/My%20File.gguf");

    auto t = parse_hf_link("https://huggingface.co/org/repo/tree/dev");
    CHECK(t.ok && t.revision == "dev" && t.file.empty());

    CHECK(!parse_hf_link("https://example.com/a/b").ok);
    CHECK(!parse_hf_link("https://huggingface.co/datasets/a/b").ok);
    CHECK(!parse_hf_link("https://huggingface.co/org/repo/blob/main/../x").ok);
    CHECK(!parse_hf_link("justonepart").ok);
    CHECK(parse_hf_link("org/repo").ok);

    CHECK(detect_family("city96/FLUX.1-dev-gguf", "x.gguf").id == "flux");
    CHECK(detect_family("x/FLUX.1-Kontext-dev-gguf", "x.gguf").id.empty());
    CHECK(detect_family("QuantStack/Wan2.2-TI2V-5B-GGUF", "a.gguf").id == "wan-2.2-5b");
    CHECK(detect_family("QuantStack/Wan2.2-T2V-A14B-GGUF", "a.gguf").id.empty());
    CHECK(detect_family("a/b", "Wan2.1_t2v_1.3B.gguf").id == "wan-2.1");
    CHECK(detect_family("Abiray/LTX-2.5-Distilled-GGUF", "q.gguf").id == "ltx-2.5");
    CHECK(detect_family("a/ltx-2.3", "q.gguf").id.empty());
    CHECK(detect_family("Acly/Real-ESRGAN-GGUF", "RealESRGAN-x4plus_anime-6B-F16.gguf").id == "esrgan");
    CHECK(detect_family("a/4x-upscalers", "4x-UltraSharp.gguf").id == "esrgan");
    CHECK(detect_family("a/Real-ESRGAN", "RealESRGAN_x4plus.pth").id.empty());
    CHECK(detect_family("a/Real-ESRGAN", "RealESRGAN_x4plus.safetensors").id.empty());
    CHECK(detect_family("someone/random-llm", "model.safetensors").id.empty());

    CHECK(!is_main_model_candidate("vae/x.safetensors"));
    CHECK(!is_main_model_candidate("readme.md"));
    CHECK(is_main_model_candidate("a/b.gguf"));
    CHECK(safe_local_name("org/repo", "dir/a b.gguf") == "hf_org_repo_a_b.gguf");

    std::cout << "all ok" << std::endl;
    return 0;
}

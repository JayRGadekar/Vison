#include "vison/hf_link.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace vison {
namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

bool has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string percent_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) &&
            std::isxdigit((unsigned char)s[i + 2])) {
            out += (char)std::stoi(s.substr(i + 1, 2), nullptr, 16);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

bool valid_repo_part(const std::string& p) {
    if (p.empty() || p == "." || p == "..") return false;
    for (char c : p) {
        if (!(std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.')) return false;
    }
    return true;
}

const std::vector<HfFamily>& families() {
    static const std::vector<HfFamily> f = {
        {"ltx-2.5",      "LTX 2.5",             "lightricks/ltx-2.5-distilled",    "video"},
        {"minimax-h3",   "MiniMax H3",          "minimaxai/minimax-h3-fl2va",      "video"},
        {"hunyuan-video","HunyuanVideo 1.5",    "tencent/hunyuanvideo-1.5-t2v",    "video"},
        {"wan-2.2-5b",   "Wan 2.2 TI2V 5B",     "wan-ai/wan2.2-ti2v-5b",           "video"},
        {"wan-2.1",      "Wan 2.1",             "wan-ai/wan2.1-t2v-1.3b",          "video"},
        {"flux",         "FLUX.1",              "black-forest-labs/flux1-schnell", "image"},
        {"z-image",      "Z-Image",             "tongyi-milm/z-image-turbo",       "image"},
        {"qwen-image",   "Qwen-Image",          "qwen/qwen-image",                 "image"},
        {"sdxl",         "Stable Diffusion XL", "stabilityai/sdxl-turbo",          "image"},
        // Upscalers are registered for both image and video (same weights, run
        // per frame for video), so the family task is "upscale".
        {"esrgan",       "ESRGAN upscaler (GGUF)", "esrgan-4x-remacri",            "upscale"},
    };
    return f;
}

} // namespace

HfFamily family_by_id(const std::string& id) {
    for (const auto& f : families()) if (f.id == id) return f;
    return {};
}

HfLink parse_hf_link(const std::string& input) {
    HfLink out;
    std::string s = input;
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    size_t b = 0;
    while (b < s.size() && std::isspace((unsigned char)s[b])) ++b;
    s = s.substr(b);

    auto cut = s.find_first_of("?#");
    if (cut != std::string::npos) s = s.substr(0, cut);

    auto strip_prefix = [&](const char* p) {
        std::string pre(p);
        if (lower(s.substr(0, pre.size())) == pre) { s = s.substr(pre.size()); return true; }
        return false;
    };
    const bool had_scheme = strip_prefix("https://") || strip_prefix("http://");
    const bool had_host = strip_prefix("www.huggingface.co/") || strip_prefix("huggingface.co/") ||
                          strip_prefix("hf.co/");
    if (had_scheme && !had_host) {
        out.error = "Only huggingface.co links are supported.";
        return out;
    }

    auto parts = split(s, '/');
    if (!parts.empty() && parts[0] == "models") parts.erase(parts.begin());
    if (parts.size() < 2) { out.error = "Expected a link like huggingface.co/org/model."; return out; }
    static const char* reserved[] = {"datasets", "spaces", "collections", "docs", "papers"};
    for (const char* r : reserved) {
        if (parts[0] == r) { out.error = "That link is not a model repository."; return out; }
    }
    if (!valid_repo_part(parts[0]) || !valid_repo_part(parts[1])) {
        out.error = "Could not read a repository name from that link.";
        return out;
    }
    out.repo = parts[0] + "/" + parts[1];
    out.revision = "main";

    if (parts.size() >= 4 && (parts[2] == "blob" || parts[2] == "resolve")) {
        out.revision = percent_decode(parts[3]);
        std::string file;
        for (size_t i = 4; i < parts.size(); ++i) file += (file.empty() ? "" : "/") + percent_decode(parts[i]);
        if (file.find("..") != std::string::npos) { out.error = "Invalid file path."; return out; }
        out.file = file;
    } else if (parts.size() >= 4 && parts[2] == "tree") {
        out.revision = percent_decode(parts[3]);
    } else if (parts.size() > 2 && parts[2] != "tree") {
        out.error = "Could not read a file or repository from that link.";
        return out;
    }
    out.ok = true;
    return out;
}

std::string hf_resolve_url(const HfLink& link, const std::string& file) {
    auto enc = [](const std::string& seg) {
        static const char* hex = "0123456789ABCDEF";
        std::string o;
        for (unsigned char c : seg) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
            else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
        }
        return o;
    };
    std::string path;
    for (const auto& seg : split(file, '/')) path += "/" + enc(seg);
    return "https://huggingface.co/" + link.repo + "/resolve/" + enc(link.revision) + path;
}

HfFamily detect_family(const std::string& repo, const std::string& file) {
    const std::string r = lower(repo), f = lower(file), both = r + " " + f;

    // Variants the pinned engine has no pipeline for. Matching them to a
    // neighbouring family would download many GB and then fail to load, so
    // they are left unmatched on purpose.
    if (has(both, "kontext") || has(both, "flux.2") || has(both, "flux2") || has(both, "flux-2")) return {};
    if (has(both, "a14b") || (has(both, "i2v") && !has(both, "ti2v")) || has(both, "vace")) return {};
    if (has(both, "qwen") && has(both, "edit")) return {};
    if (has(both, "lora") || has(both, "controlnet") || has(both, "inpaint")) return {};

    // Upscalers. The pinned vision.cpp loads ESRGAN-family networks from GGUF
    // only, so a .pth/.safetensors upscaler is left unmatched rather than
    // downloaded and then failing to load.
    if (has(both, "esrgan") || has(both, "realesr") || has(both, "upscal") ||
        (has(both, "gguf") && (has(both, "4x") || has(both, "x4") || has(both, "2x") || has(both, "x2")) &&
         (has(both, "remacri") || has(both, "ultrasharp") || has(both, "nmkd") || has(both, "foolhardy")))) {
        if (f.size() > 5 && f.compare(f.size() - 5, 5, ".gguf") == 0) return family_by_id("esrgan");
        return {};
    }

    if (has(both, "ltx") && (has(both, "2.5") || has(both, "2_5") || has(both, "ltx25"))) return family_by_id("ltx-2.5");
    if (has(both, "minimax") && has(both, "h3")) return family_by_id("minimax-h3");
    if (has(both, "hunyuan") && has(both, "video")) return family_by_id("hunyuan-video");
    if (has(both, "wan2") || has(both, "wan_2") || has(both, "wan-2") || has(r, "wan-ai")) {
        if (has(both, "ti2v") || (has(both, "2.2") && has(both, "5b"))) return family_by_id("wan-2.2-5b");
        if (has(both, "2.1") || has(both, "2_1")) return family_by_id("wan-2.1");
        return {};
    }
    if (has(both, "z-image") || has(both, "z_image") || has(both, "zimage")) return family_by_id("z-image");
    if (has(both, "qwen") && has(both, "image")) return family_by_id("qwen-image");
    if (has(both, "flux")) return family_by_id("flux");
    if (has(both, "sdxl") || has(both, "sd_xl") || has(both, "stable-diffusion-xl")) return family_by_id("sdxl");
    return {};
}

const HfFamily* hf_families(int* count) {
    if (count) *count = (int)families().size();
    return families().data();
}

bool is_main_model_candidate(const std::string& path) {
    const std::string p = lower(path);
    const bool ext_ok = (p.size() > 5 && p.compare(p.size() - 5, 5, ".gguf") == 0) ||
                        (p.size() > 12 && p.compare(p.size() - 12, 12, ".safetensors") == 0);
    if (!ext_ok) return false;
    static const char* companions[] = {"vae", "text_encoder", "clip", "t5", "tokenizer", "lora",
                                       "controlnet", "mmproj", "audio", "embedding",
                                       "connector", "encoder"};
    for (const char* c : companions) if (has(p, c)) return false;
    return true;
}

std::string safe_local_name(const std::string& repo, const std::string& file) {
    std::string base = file;
    auto slash = base.find_last_of('/');
    if (slash != std::string::npos) base = base.substr(slash + 1);
    std::string s = "hf_" + repo + "_" + base;
    for (char& c : s) {
        if (!(std::isalnum((unsigned char)c) || c == '-' || c == '.')) c = '_';
    }
    return s;
}

} // namespace vison

#pragma once
#include <string>

namespace vison {

// What a pasted Hugging Face link points at. `file` is empty for a bare repo
// link, in which case the caller lists the repo and lets the user choose.
struct HfLink {
    bool ok = false;
    std::string error;
    std::string repo;       // "org/name"
    std::string revision;   // "main" unless the link names another
    std::string file;       // path inside the repo, or empty
};

// Accepts https://huggingface.co/org/repo, .../tree/<rev>[/dir],
// .../blob/<rev>/<file>, .../resolve/<rev>/<file> (query strings ignored),
// hf.co/..., and a bare "org/repo".
HfLink parse_hf_link(const std::string& input);

// Direct download URL for a file in a repo. Path segments are percent-encoded.
std::string hf_resolve_url(const HfLink& link, const std::string& file);

// The model families Vison can run, keyed to the registry entry whose
// companion files (text encoder, VAE...) a custom model of that family reuses.
struct HfFamily {
    std::string id;           // e.g. "flux"
    std::string name;         // for the UI
    std::string template_id;  // registry id to copy companions and defaults from
    std::string task;         // "image" or "video"
};

// Best-effort guess from repo and file names alone. Returns an empty id when
// nothing matches; never guesses between two plausible families.
HfFamily detect_family(const std::string& repo, const std::string& file);

// Every family, for a manual override in the UI.
const HfFamily* hf_families(int* count);

// Looks a family up by its id; empty id when unknown.
HfFamily family_by_id(const std::string& id);

// True for files worth offering as a main model: .gguf / .safetensors that are
// not obviously a text encoder, VAE, LoRA or other companion.
bool is_main_model_candidate(const std::string& path);

// Filesystem-safe local filename derived from a repo and file path.
std::string safe_local_name(const std::string& repo, const std::string& file);

} // namespace vison

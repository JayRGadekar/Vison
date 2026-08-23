#pragma once

#include "visp/image.h"
#include "stb_image_write.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <string>

namespace vison::pipelines {

// Writes an image in the container the caller asked for.
//
// visp::image_save() always emits PNG bytes no matter what the filename says,
// so requesting "jpg" used to produce a .jpg holding a PNG. It still renders -
// every viewer sniffs content rather than trusting the extension - but it is
// not the file that was asked for, and none of the size saving that picking
// JPEG implies actually happened.
//
// Both encoders are called directly here rather than via visp. That is not
// only for JPEG: stbi_write_png_compression_level is a global living in
// whichever copy of stb does the writing, and visp's copy is compiled into
// visioncpp.dll, so setting it from here would never reach visp's writer.
// Owning both paths keeps the `compression` setting effective.
//
// `compression` is 0 for "pipeline default", otherwise interpreted per
// container: JPEG takes a 1-100 quality, PNG a 1-9 zlib effort (PNG is
// lossless either way - the level trades encode time against file size).
//
// Formats with no encoder fall back to PNG *and take the .png extension with
// them*, so the name never lies about the bytes. Returns the path actually
// written, which may therefore differ from out_path.
inline std::string save_image(const visp::image_view& img,
                              const std::string& out_path,
                              const std::string& format,
                              int compression = 0) {
    std::string fmt;
    for (char c : format) fmt.push_back((char)std::tolower((unsigned char)c));

    std::error_code ec;
    auto parent = std::filesystem::path(out_path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);

    const int channels      = visp::n_channels(img);
    const int packed_stride = img.extent[0] * channels;

    // stbi_write_jpg has no stride parameter, so it can only be handed tightly
    // packed rows. Everything visp allocates is packed; anything else falls
    // back to the PNG path, which does take a stride.
    const bool wants_jpeg     = (fmt == "jpg" || fmt == "jpeg");
    const bool jpeg_possible  = wants_jpeg && img.stride == packed_stride;

    if (jpeg_possible) {
        // 92 is the usual "indistinguishable from source" point for
        // photographic output; below ~85 diffusion textures visibly block up.
        const int quality = (compression > 0) ? std::clamp(compression, 1, 100) : 92;
        if (stbi_write_jpg(out_path.c_str(), img.extent[0], img.extent[1], channels,
                           img.data, quality) != 0) {
            return out_path;
        }
        std::cerr << "[Vison] JPEG encode failed for " << out_path
                  << "; falling back to PNG" << std::endl;
    } else if (wants_jpeg) {
        std::cerr << "[Vison] Cannot JPEG-encode a non-packed image; writing PNG instead"
                  << std::endl;
    } else if (!fmt.empty() && fmt != "png") {
        std::cerr << "[Vison] No encoder for '" << fmt << "'; writing PNG instead" << std::endl;
    }

    // PNG, and the landing spot for every fallback above. Rewrite the extension
    // so the file is named for what it actually contains.
    std::filesystem::path png_path(out_path);
    png_path.replace_extension(".png");

    const int previous_level = stbi_write_png_compression_level;
    if (compression > 0) {
        stbi_write_png_compression_level = std::clamp(compression, 1, 9);
    }
    const int wrote = stbi_write_png(png_path.string().c_str(), img.extent[0], img.extent[1],
                                     channels, img.data, img.stride);
    stbi_write_png_compression_level = previous_level;

    if (wrote == 0) {
        std::cerr << "[Vison] Failed to write " << png_path.string() << std::endl;
    }
    return png_path.string();
}

}  // namespace vison::pipelines

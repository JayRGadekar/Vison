#pragma once
#include <memory>
#include "vison/vison.h"

namespace vison::pipelines {
    std::unique_ptr<Pipeline> create_image_upscale_pipeline();
}

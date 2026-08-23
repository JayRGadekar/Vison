#include "vison/vison.h"

namespace vison::pipelines {
std::unique_ptr<Pipeline> create_image_upscale_pipeline();
std::unique_ptr<Pipeline> create_image_gen_pipeline();
std::unique_ptr<Pipeline> create_video_gen_pipeline();
std::unique_ptr<Pipeline> create_video_upscale_pipeline();
}

namespace vison {

std::unique_ptr<Pipeline> create_pipeline(TaskType type) {
    switch (type) {
        case TaskType::IMAGE_UPSCALING:
            return pipelines::create_image_upscale_pipeline();
        case TaskType::IMAGE_GENERATION:
            return pipelines::create_image_gen_pipeline();
        case TaskType::VIDEO_GENERATION:
            return pipelines::create_video_gen_pipeline();
        case TaskType::VIDEO_UPSCALING:
            return pipelines::create_video_upscale_pipeline();
        default:
            return nullptr;
    }
}

} // namespace vison

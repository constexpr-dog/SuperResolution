//
// Created by Armando Herrera on 1/19/21.
//

#include "Model.h"
#include "utils.h"

#include <glog/logging.h>
#include <algorithm>
#include <deque>


namespace idx = torch::indexing;

Model::Model(const std::filesystem::path &model_path, int64_t upscale, int64_t output_size) : device("cpu") {
    try {
        module = torch::jit::load(model_path.string());
    } catch (const c10::Error &e) {
        LOG(FATAL) << "error loading module\n" << e.msg();
    }
    module.eval();

    if (torch::hasCUDA()) {
        device = torch::Device(torch::kCUDA);
        module.to(device);
    }

    input_dim = output_size / upscale;
    output_dim = output_size;
    scale = upscale;
}

std::vector<at::Tensor> Model::run(const std::vector<at::Tensor> &input) {
    std::vector<at::Tensor> output_tensors;
    for (const at::Tensor &tensor : input) {
        at::Tensor i_tensor = tensor.to(torch::Device(device));
        at::Tensor o_tensor = module.forward({i_tensor}).toTensor().to(torch::Device("cpu"));
        output_tensors.push_back(o_tensor);
    }
    return output_tensors;
}

std::vector<at::Tensor> Model::preprocess(const at::Tensor &input) const {
    int64_t width = input.size(0), height = input.size(1);
    std::vector<at::Tensor> output;
    for (int64_t i = 0; i < width; i += input_dim) {
        for (int64_t j = 0; j < height; j += input_dim) {
            at::Tensor block = input.index(
                    {
                            idx::Slice(i, std::min(i + static_cast<int64_t>(input_dim), width)),
                            idx::Slice(j, std::min(j + static_cast<int64_t>(input_dim), height))
                    });
            block = block.permute({2, 0, 1}).unsqueeze(0).to(torch::kFloat32).div(255);
            output.push_back(block);
        }
    }
    return output;
}

at::Tensor Model::postprocess(const std::vector<at::Tensor> &input, const cv::Size &output_size) const {
    at::Tensor unblocked = torch::zeros({output_size.width, output_size.height, 3});
    auto input_itr = input.cbegin();
    for (int64_t i = 0; i < output_size.width; i += output_dim) {
        for (int64_t j = 0; j < output_size.height; j += output_dim) {
            unblocked.index_put_(
                    {
                        idx::Slice(i, std::min(i + static_cast<int64_t>(output_dim), static_cast<int64_t>(output_size.width))),
                        idx::Slice(j, std::min(j + static_cast<int64_t>(output_dim), static_cast<int64_t>(output_size.height)))
                        }, (*input_itr).squeeze(0).permute({1, 2, 0}));
            input_itr++;
        }
    }
    unblocked = torch::clamp((unblocked * 255) + 0.5, 0, 255).to(torch::kUInt8);
    return unblocked;
}

cv::Mat Model::run(const cv::Mat &input) {
    int64_t width = input.rows, height = input.cols;
    at::Tensor input_t = torch::from_blob(input.data, {width, height, 3});
    std::vector<at::Tensor> blocked_input = preprocess(input_t);
    std::vector<at::Tensor> blocked_output = run(blocked_input);
    at::Tensor output_t = postprocess(blocked_output, cv::Size(width * scale, height * scale));
    auto *output_ptr = output_t.data_ptr<uint8_t>();
    return cv::Mat(cv::Size(output_t.size(2), output_t.size(3)), CV_8UC3, output_ptr);
}

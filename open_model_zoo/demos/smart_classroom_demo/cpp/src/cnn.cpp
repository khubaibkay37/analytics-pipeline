// Copyright (C) 2018-2019 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "cnn.hpp"

#include <string>
#include <vector>
#include <algorithm>

#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include <inference_engine.hpp>

CnnDLSDKBase::CnnDLSDKBase(const Config& config) : config_(config) {}

void CnnDLSDKBase::Load() {
    auto cnnNetwork = config_.ie.ReadNetwork(config_.path_to_model);


    const int currentBatchSize = cnnNetwork.getBatchSize();
    if (currentBatchSize != config_.max_batch_size)
        cnnNetwork.setBatchSize(config_.max_batch_size);

    InferenceEngine::InputsDataMap in = cnnNetwork.getInputsInfo();
    if (in.size() != 1) {
        throw std::runtime_error("Network should have only one input");
    }
    in.begin()->second->setPrecision(InferenceEngine::Precision::U8);
    in.begin()->second->setLayout(InferenceEngine::Layout::NCHW);
    input_blob_name_ = in.begin()->first;

    InferenceEngine::OutputsDataMap out = cnnNetwork.getOutputsInfo();
    for (auto&& item : out) {
        item.second->setPrecision(InferenceEngine::Precision::FP32);
        output_blobs_names_.push_back(item.first);
    }

    try {
        executable_network_ = config_.ie.LoadNetwork(cnnNetwork, config_.deviceName);
    } catch (const InferenceEngine::Exception&) {  // in case the model does not work with dynamic batch
        cnnNetwork.setBatchSize(1);
        executable_network_ = config_.ie.LoadNetwork(cnnNetwork, config_.deviceName,
            {{InferenceEngine::PluginConfigParams::KEY_DYN_BATCH_ENABLED, InferenceEngine::PluginConfigParams::NO}});
    }
    logExecNetworkInfo(executable_network_, config_.path_to_model, config_.deviceName, config_.model_type);
    infer_request_ = executable_network_.CreateInferRequest();
}

void CnnDLSDKBase::InferBatch(
        const std::vector<cv::Mat>& frames,
        const std::function<void(const InferenceEngine::BlobMap&, size_t)>& fetch_results) const {
    InferenceEngine::Blob::Ptr input = infer_request_.GetBlob(input_blob_name_);
    const size_t batch_size = input->getTensorDesc().getDims()[0];

    size_t num_imgs = frames.size();
    for (size_t batch_i = 0; batch_i < num_imgs; batch_i += batch_size) {
        const size_t current_batch_size = std::min(batch_size, num_imgs - batch_i);
        for (size_t b = 0; b < current_batch_size; b++) {
            matToBlob(frames[batch_i + b], input, b);
        }

        if (config_.max_batch_size != 1)
            infer_request_.SetBatch(current_batch_size);
        infer_request_.Infer();

        InferenceEngine::BlobMap blobs;
        for (const auto& name : output_blobs_names_)  {
            blobs[name] = infer_request_.GetBlob(name);
        }
        fetch_results(blobs, current_batch_size);
    }
}

void CnnDLSDKBase::Infer(const cv::Mat& frame,
                         const std::function<void(const InferenceEngine::BlobMap&, size_t)>& fetch_results) const {
    InferBatch({frame}, fetch_results);
}

VectorCNN::VectorCNN(const Config& config)
        : CnnDLSDKBase(config) {
    Load();
    if (output_blobs_names_.size() != 1) {
        throw std::runtime_error("Demo supports topologies only with 1 output");
    }
}

void VectorCNN::Compute(const cv::Mat& frame,
                                     cv::Mat* vector, cv::Size outp_shape) const {
    std::vector<cv::Mat> output;
    Compute({frame}, &output, outp_shape);
    *vector = output[0];
}

void VectorCNN::Compute(const std::vector<cv::Mat>& images, std::vector<cv::Mat>* vectors,
                                     cv::Size outp_shape) const {
    if (images.empty()) {
        return;
    }
    vectors->clear();
    auto results_fetcher = [vectors, outp_shape](const InferenceEngine::BlobMap& outputs, size_t batch_size) {
        for (auto&& item : outputs) {
            InferenceEngine::Blob::Ptr blob = item.second;
            if (blob == nullptr) {
                throw std::runtime_error("VectorCNN::Compute() Invalid blob '" + item.first + "'");
            }
            InferenceEngine::SizeVector ie_output_dims = blob->getTensorDesc().getDims();
            std::vector<int> blob_sizes(ie_output_dims.size(), 0);
            for (size_t i = 0; i < blob_sizes.size(); ++i) {
                blob_sizes[i] = ie_output_dims[i];
            }
            InferenceEngine::LockedMemory<const void> blobMapped =
                InferenceEngine::as<InferenceEngine::MemoryBlob>(blob)->rmap();
            cv::Mat out_blob(blob_sizes, CV_32F, blobMapped.as<float*>());
            for (size_t b = 0; b < batch_size; b++) {
                cv::Mat blob_wrapper(out_blob.size[1], 1, CV_32F,
                                     reinterpret_cast<void*>((out_blob.ptr<float>(0) + b * out_blob.size[1])));
                vectors->emplace_back();
                if (outp_shape != cv::Size())
                    blob_wrapper = blob_wrapper.reshape(1, {outp_shape.height, outp_shape.width});
                blob_wrapper.copyTo(vectors->back());
            }
        }
    };
    InferBatch(images, results_fetcher);
}

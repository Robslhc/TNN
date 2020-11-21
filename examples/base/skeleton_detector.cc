// Tencent is pleased to support the open source community by making TNN available.
//
// Copyright (C) 2020 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include "skeleton_detector.h"
#include <sys/time.h>
#include <cmath>
#include <fstream>
#include <cstring>

namespace TNN_NS {

Status SkeletonDetector::Init(std::shared_ptr<TNNSDKOption> option_i) {
    Status status = TNN_OK;
    auto option = dynamic_cast<SkeletonDetectorOption *>(option_i.get());
    RETURN_VALUE_ON_NEQ(!option, false,
                        Status(TNNERR_PARAM_ERR, "TNNSDKOption is invalid"));
    
    status = TNNSDKSample::Init(option_i);
    RETURN_ON_NEQ(status, TNN_OK);
    
    auto input_dims = GetInputShape();
    option->input_height = input_dims[2];
    option->input_width  = input_dims[3];

    return status;
}

std::shared_ptr<Mat> SkeletonDetector::ProcessSDKInputMat(std::shared_ptr<Mat> input_mat, std::string name) {
    this->orig_input_height = input_mat->GetHeight();
    this->orig_input_width  = input_mat->GetWidth();
    return TNNSDKSample::ResizeToInputShape(input_mat, name);
}

MatConvertParam SkeletonDetector::GetConvertParamForInput(std::string tag) {
    MatConvertParam input_convert_param;
    /*
     from ultra skeleton_api, for final 22, the model requires BGRA input
    input_convert_param.scale = {1.0 / 256, 1.0 / 256, 1.0 / 256, 0.0};
    input_convert_param.bias  = {-0.5, -0.5, -0.5, 0.0};
    */
    // for reduce model, rgb
    input_convert_param.scale = {0.01712475,  0.017507  ,  0.01742919, 0.0};
    input_convert_param.bias  = {-2.11790393,  -2.03571429,  -1.80444444, 0.0};
    
    return input_convert_param;
}

std::shared_ptr<TNNSDKOutput> SkeletonDetector::CreateSDKOutput() {
    return std::make_shared<SkeletonDetectorOutput>();
}

Status SkeletonDetector::ProcessSDKOutput(std::shared_ptr<TNNSDKOutput> output_) {
    Status status = TNN_OK;
    auto option = dynamic_cast<SkeletonDetectorOption *>(option_.get());
    RETURN_VALUE_ON_NEQ(!option, false,
                           Status(TNNERR_PARAM_ERR, "TNNSDKOption is invalid"));
    auto output = dynamic_cast<SkeletonDetectorOutput *>(output_.get());
    RETURN_VALUE_ON_NEQ(!output, false,
    Status(TNNERR_PARAM_ERR, "TNNSDKOutput is invalid"));
    
    auto heatmap = output->GetMat("heatmap");
    RETURN_VALUE_ON_NEQ(!heatmap, false,
                           Status(TNNERR_PARAM_ERR, "heatmap mat is nil"));
    
    std::vector<SkeletonInfo> keypoints;
    //decode keypoints
    //GenerateSkeleton(keypoints, heatmap, option->input_width, option->input_height, option->min_threshold);
    //output->keypoint_list = keypoints;
    
    return status;
}

static void GetGaussianKernel(int length, float sigma, std::vector<float>& kernels) {
    kernels.resize(length);
    if (sigma <= 0)
        return;
    if (length %2 != 1)
        return;
    
    const double sd_minus_0_125 = -0.125;
    double scale_2x = sd_minus_0_125 / (sigma * sigma);
    
    int length_half = (length - 1) / 2;
    std::vector<double> values(length_half +  1);
    double sum = 0;
    for(int i=0, x=1-length; i<length_half; ++i, x+=2) {
        double t = exp(static_cast<double>(x*x) * scale_2x);
        values[i] = t;
        sum += t;
    }
    sum *= 2;
    sum += 1;
    
    double mul1 = static_cast<double>(1.0) / sum;
    for(int i=0; i<length_half; ++i) {
        double t = values[i] * mul1;
        kernels[i] = t;
        kernels[length - 1 - i] = t;
    }
    kernels[length_half] = 1 * mul1;
}

static int GetBorderLocation(int x, int len, TNNBorderType border_type) {
    int p = x;
    if (x >=0 && x < len)
        ;
    else if ( border_type == TNNBorderReplicate)
        p = x < 0? 0 : len-1;
    else if( border_type == TNNBorderReflect || border_type == TNNBorderReflect101 ) {
        int delta = border_type == TNNBorderReflect101;
        if( len == 1 )
            return 0;
        do {
            if( p < 0 )
                p = -p - 1 + delta;
            else
                p = len - 1 - (p - len) - delta;
        } while(p < 0 || p >= len);
    }
    else if( border_type == TNNBorderWrap ) {
        if( p < 0 )
            p -= ((p-len+1)/len)*len;
        if( p >= len )
            p %= len;
    }
    else if( border_type == TNNBorderConstant )
        p = -1;
    
    return p;
}

// apply filter on the Mat
static TNN_NS::Status ApplyFilter(TNN_NS::Mat& src, TNN_NS::Mat& dst, std::vector<float> filter, TNNBorderType border_type, int direction) {
    const int kernel_size = filter.size();
    const int anchor = (kernel_size - 1) / 2;
    if (direction !=0 && direction != 1) {
        // error, invlaid direction
        return TNN_NS::Status(TNN_NS::TNNERR_PARAM_ERR, "invalid direction.");;
    }
    
    // only for nchw float mat
    auto src_mat_type = src.GetMatType();
    auto dst_mat_type = dst.GetMatType();
    if (src_mat_type != dst_mat_type || src_mat_type != TNN_NS::NCHW_FLOAT)
        return TNN_NS::Status(TNN_NS::TNNERR_PARAM_ERR, "invalid mat type.");
    
    // only for cpu mat
    auto src_dev_type = src.GetDeviceType();
    auto dst_dev_type = dst.GetDeviceType();
    if (src_dev_type != dst_dev_type || (src_dev_type != TNN_NS::DEVICE_ARM && src_dev_type != TNN_NS::DEVICE_NAIVE))
        return TNN_NS::Status(TNN_NS::TNNERR_PARAM_ERR, "invalid mat device type.");
    
    int width   = dst.GetWidth();
    int height  = dst.GetHeight();
    int channel = dst.GetChannel();
    
    const float* src_ptr = static_cast<float *>(src.GetData());
    float* dst_ptr = static_cast<float *>(dst.GetData());
    for(int c=0; c<channel; ++c) {
        const float* src_channel_ptr = src_ptr + c * width * height;
        float* dst_channel_ptr = dst_ptr + c * width * height;
        for(int h=0; h<height; ++h) {
            for(int w=0; w<width; ++w) {
                float val = 0;
                for(int k=0; k<kernel_size; ++k) {
                    int src_h = h;
                    int src_w = w;
                    if (direction == 0) {
                        src_w = (k - anchor) + w;
                    } else if (direction == 1) {
                        src_h = (k - anchor) + h;
                    }
                    src_h = GetBorderLocation(src_h, height, TNNBorderReflect101);
                    src_w = GetBorderLocation(src_w, width,  TNNBorderReflect101);
                    // TODO: use border_val instead of 0
                    float src_val = (src_h < 0 || src_w < 0) ? 0 : src_channel_ptr[src_h * width + src_w];
                    val += filter[k] * src_val;
                }
                dst_channel_ptr[h * width + w] = val;
            }
        }
    }
    return TNN_OK;
}


TNN_NS::Status SkeletonDetector::GaussianBlur(std::shared_ptr<TNN_NS::Mat>src, std::shared_ptr<TNN_NS::Mat> dst,
                  int kernel_h, int kernel_w,
                  float sigma_x, float sigma_y) {
    std::vector<float> weights_x;
    std::vector<float> weights_y;
    GetGaussianKernel(kernel_w, sigma_x, weights_x);
    if (kernel_h == kernel_w && sigma_x == sigma_y)
        weights_y = weights_x;
    else
        GetGaussianKernel(kernel_h, sigma_y, weights_y);
    
    Mat tmp(src->GetDeviceType(), src->GetMatType(), src->GetDims());
    // apply row filter
    auto status = ApplyFilter(*(src.get()), tmp, weights_x, TNNBorderReflect101, 0);
    if (status != TNN_OK)
        return status;
    // apply column filter
    status = ApplyFilter(tmp, *(dst.get()), weights_y, TNNBorderReflect101, 1);
    return status;
}

void SkeletonDetector::GenerateSkeleton(std::vector<SkeletonInfo> &skeleton, std::shared_ptr<TNN_NS::Mat> heatmap,
                                        int image_w, int image_h, float threshold) {
    const int heatmap_channels = heatmap->GetChannel();
    const int heatmap_height   = heatmap->GetHeight();
    const int heatmap_width    = heatmap->GetWidth();
    
    //float scale = static_cast<float>(this->orig_input_width) / static_cast<float>(image_w) / 2.0f;
    // gaussian blur kernel setting
    const int kernel_size = 5;
    const float sigma = 3.0f;
    
    TNN_NS::DimsVector dim_heatmap = {1, heatmap_channels, heatmap_height, heatmap_width};
    TNN_NS::DimsVector dim_resized = {1, heatmap_channels, this->orig_input_height, this->orig_input_width};
    auto heatmap_resized = std::make_shared<TNN_NS::Mat>(TNN_NS::DEVICE_ARM, TNN_NS::NCHW_FLOAT, dim_resized);
    auto heatmap_blur = std::make_shared<TNN_NS::Mat>(TNN_NS::DEVICE_ARM, TNN_NS::NCHW_FLOAT, dim_resized);

    Resize(heatmap, heatmap_resized, TNNInterpCubic);
    GaussianBlur(heatmap_resized, heatmap_blur, kernel_size, kernel_size, sigma, sigma);
    float* heatmap_data = static_cast<float *>(heatmap_blur->GetData());

    for(int c=0; c<heatmap_channels; ++c) {
        float* blurred_data_ptr = heatmap_data + c * heatmap_height * heatmap_width;
        // locate the max value inside a channel
        float max_pos_h = -1;
        float max_pos_w = -1;
        float max_val = -FLT_MAX;
        for(int h=0; h<dim_resized[2]; ++h) {
            for(int w=0; w<dim_resized[3]; ++w) {
                auto val = blurred_data_ptr[h * dim_resized[3] + w];
                if ( val > max_val) {
                    max_val = val;
                    max_pos_h = h;
                    max_pos_w = w;
                }
            }
        }
        
        if (max_val < threshold)
            continue;
        SkeletonInfo info;
        info.score = max_val;
        info.image_width  = this->orig_input_width;
        info.image_height = this->orig_input_height;
        info.key_points.emplace_back(max_pos_h, max_pos_w);
        
        skeleton.push_back(std::move(info));
    }
    
}

}


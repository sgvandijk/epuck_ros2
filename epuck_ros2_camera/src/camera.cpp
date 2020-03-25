// Copyright 2020 Cyberbotics
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <chrono>
#include <memory>
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/camera_info.hpp>
#include <sensor_msgs/msg/compressed_image.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <camera_info_manager/camera_info_manager.h>

extern "C" {
#include "epuck_ros2_camera/pipuck_image.h"
#include "epuck_ros2_camera/pipuck_mmal.h"
#include "epuck_ros2_camera/pipuck_ov7670.h"
#include "epuck_ros2_camera/pipuck_v4l2.h"
}

#define IMAGE_RATIO (4.0 / 3.0)

class CameraPublisher : public rclcpp::Node {
public:
  CameraPublisher() : Node("epuck_ros2_camera"), mIsV4l2Initialized(false), mIsJpegInitialized(false), mIsRgbInitialized(false) {
    // Add parameters
    int quality = declare_parameter<int>("quality", 8);
    const int framerate = declare_parameter<int>("framerate", 10);
    const int width = declare_parameter<int>("width", 640);
    std::string cameraInfoUrl =
      declare_parameter<std::string>("camera_info_url", "package://epuck_ros2_camera/camera_info/camera.yaml");
    std::string cameraName = declare_parameter<std::string>("camera_name", "camera");

    // MMAL JPEG
    pipuck_mmal_create(&mPipuckMmalJpeg);
    strcpy(mPipuckMmalJpeg.component, "vc.ril.image_encode");
    mPipuckMmalJpeg.output.data = mImageBuffer;
    mPipuckMmalJpeg.output.quality = quality;
    mPipuckMmalJpeg.output.encoding = MMAL_ENCODING_JPEG;

    // MMAL RGB
    pipuck_mmal_create(&mPipuckMmalRgb);
    strcpy(mPipuckMmalRgb.component, "vc.ril.isp");
    mPipuckMmalRgb.output.data = mImageBuffer;
    mPipuckMmalRgb.output.width = width;
    mPipuckMmalRgb.output.height = width / IMAGE_RATIO;
    mPipuckMmalRgb.output.encoding = MMAL_ENCODING_RGB24;

    // Load intrisic camera parameters
    camera_info_manager::CameraInfoManager cameraInfoManager(this, cameraName, cameraInfoUrl);
    if (!cameraInfoManager.loadCameraInfo(cameraInfoUrl))
      RCLCPP_INFO(this->get_logger(), "Calibration file %s is missing", cameraInfoUrl.c_str());
    else {
      mCameraInfoMsg = cameraInfoManager.getCameraInfo();
      RCLCPP_INFO(this->get_logger(), "Calibration calibrated from file %s", cameraInfoUrl.c_str());
    }

    // Configure the camera
    pipuck_ov7670_init();

    // Prepare ROS topics
    mCallbackHandler =
      this->add_on_set_parameters_callback(std::bind(&CameraPublisher::onParamChangeCallback, this, std::placeholders::_1));
    mPublisherCompressed = this->create_publisher<sensor_msgs::msg::CompressedImage>("image_raw/compressed", 1);
    mPublisherRaw = this->create_publisher<sensor_msgs::msg::Image>("image_raw", 1);
    mTimer =
      this->create_wall_timer(std::chrono::milliseconds(1000 / framerate), std::bind(&CameraPublisher::timerCallback, this));
    mPublisherCameraInfo = this->create_publisher<sensor_msgs::msg::CameraInfo>("camera_info", 1);
    RCLCPP_INFO(this->get_logger(), "E-puck2 camera is ready");
  }

  ~CameraPublisher() {
    deinitV4l2();
    deinitRgb();
    deinitJpeg();
  }

private:
  rcl_interfaces::msg::SetParametersResult onParamChangeCallback(std::vector<rclcpp::Parameter> parameters) {
    auto result = rcl_interfaces::msg::SetParametersResult();
    result.successful = true;

    for (auto parameter : parameters) {
      if (parameter.get_name() == "quality") {
        deinitJpeg();
        mPipuckMmalJpeg.output.quality = parameter.as_int();
        initJpeg();
      } else if (parameter.get_name() == "framerate") {
        mTimer->cancel();
        mTimer = this->create_wall_timer(std::chrono::milliseconds(1000 / parameter.as_int()),
                                         std::bind(&CameraPublisher::timerCallback, this));
      } else if (parameter.get_name() == "width") {
        int width = parameter.as_int();
        int height = parameter.as_int() / IMAGE_RATIO;

        deinitRgb();
        mPipuckMmalRgb.output.width = width;
        mPipuckMmalRgb.output.height = height;
        initRgb();
        RCLCPP_INFO(this->get_logger(), "New RGB resolution is %dx%d", width, height);
      }

      RCLCPP_INFO(this->get_logger(), "Parameter '%s' has changed to %s", parameter.get_name().c_str(),
                  parameter.value_to_string().c_str());
    }

    return result;
  }

  void timerCallback() {
    auto stamp = now();

    // Initialize V4L2 if needed
    if (mPublisherCompressed->get_subscription_count() > 0 || mPublisherRaw->get_subscription_count()) {
      // Capture an image
      initV4l2();
      pipuck_v4l2_capture(&(mPipuckMmalRgb.input));

      // Publish camera info
      mCameraInfoMsg.header.stamp = stamp;
      mPublisherCameraInfo->publish(mCameraInfoMsg);
    } else {
      deinitV4l2();
      return;
    }

    // Publish RAW RGB image if needed
    if (mPublisherRaw->get_subscription_count() > 0) {
      initRgb();

      pipuck_mmal_convert(&mPipuckMmalRgb);
      auto message = sensor_msgs::msg::Image();
      message.encoding = "rgb8";
      message.width = mPipuckMmalRgb.output.width;
      message.height = mPipuckMmalRgb.output.height;
      message.step = mPipuckMmalRgb.output.width * 3;
      message.is_bigendian = false;
      message.header.stamp = stamp;
      message.header.frame_id = "pipuck_image_raw";
      message.data.assign(mPipuckMmalRgb.output.data, mPipuckMmalRgb.output.data + mPipuckMmalRgb.output.size);
      mPublisherRaw->publish(message);
    } else
      deinitRgb();

    // Publish JPEG compressed image if needed
    if (mPublisherCompressed->get_subscription_count() > 0) {
      initJpeg();

      // It's the same data for both topics
      mPipuckMmalJpeg.input.data = mPipuckMmalRgb.input.data;
      mPipuckMmalJpeg.input.size = mPipuckMmalRgb.input.size;

      pipuck_mmal_convert(&mPipuckMmalJpeg);
      auto message = sensor_msgs::msg::CompressedImage();
      message.format = "jpeg";
      message.header.stamp = stamp;
      message.header.frame_id = "pipuck_image_compressed";
      message.data.assign(mPipuckMmalJpeg.output.data, mPipuckMmalJpeg.output.data + mPipuckMmalJpeg.output.size);
      mPublisherCompressed->publish(message);
    } else
      deinitJpeg();
  }

  void initV4l2() {
    if (!mIsV4l2Initialized) {
      pipuck_v4l2_init();
      mIsV4l2Initialized = true;
      RCLCPP_INFO(this->get_logger(), "V4L2 component initialized");
    }
  }

  void deinitV4l2() {
    if (mIsV4l2Initialized) {
      pipuck_v4l2_deinit();
      mIsV4l2Initialized = false;
      RCLCPP_INFO(this->get_logger(), "V4L2 component deinitialized");
      deinitJpeg();
      deinitRgb();
    }
  }

  void deinitJpeg() {
    if (mIsJpegInitialized) {
      pipuck_mmal_deinit(&mPipuckMmalJpeg);
      mIsJpegInitialized = false;
      RCLCPP_INFO(this->get_logger(), "MMAL JPEG component deinitialized");
    }
  }

  void initJpeg() {
    if (!mIsJpegInitialized) {
      pipuck_mmal_init(&mPipuckMmalJpeg);
      mIsJpegInitialized = true;
      RCLCPP_INFO(this->get_logger(), "MMAL JPEG component initialized");
    }
  }

  void deinitRgb() {
    if (mIsRgbInitialized) {
      pipuck_mmal_deinit(&mPipuckMmalRgb);
      mRgbInitialized = false;
      RCLCPP_INFO(this->get_logger(), "MMAL RGB component deinitialized");
    }
  }

  void initRgb() {
    if (!mIsRgbInitialized) {
      pipuck_mmal_init(&mPipuckMmalRgb);
      mIsRgbInitialized = true;
      RCLCPP_INFO(this->get_logger(), "MMAL RGB component initialized");
    }
  }

  rclcpp::TimerBase::SharedPtr mTimer;
  rclcpp::Publisher<sensor_msgs::msg::CameraInfo>::SharedPtr mPublisherCameraInfo;
  rclcpp::Publisher<sensor_msgs::msg::CompressedImage>::SharedPtr mPublisherCompressed;
  rclcpp::Publisher<sensor_msgs::msg::Image>::SharedPtr mPublisherRaw;
  OnSetParametersCallbackHandle::SharedPtr mCallbackHandler;
  char mImageBuffer[900 * 1024];
  bool mIsV4l2Initialized;
  bool mIsJpegInitialized;
  bool mIsRgbInitialized;
  pipuck_mmal_t mPipuckMmalJpeg;
  pipuck_mmal_t mPipuckMmalRgb;
  sensor_msgs::msg::CameraInfo mCameraInfoMsg;
};

int main(int argc, char *argv[]) {
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<CameraPublisher>());
  rclcpp::shutdown();
  return 0;
}

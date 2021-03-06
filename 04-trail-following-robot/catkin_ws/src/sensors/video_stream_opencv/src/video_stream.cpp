/*
 * Software License Agreement (Modified BSD License)
 *
 *  Copyright (c) 2016, PAL Robotics, S.L.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of PAL Robotics, S.L. nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 * @author Sammy Pfeiffer
 */

#include <ros/ros.h>
#include <nodelet/nodelet.h>
#include <dynamic_reconfigure/server.h>
#include <image_transport/image_transport.h>
#include <camera_info_manager/camera_info_manager.h>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <cv_bridge/cv_bridge.h>
#include <sstream>
#include <stdexcept>
#include <boost/filesystem.hpp>
#include <boost/assign/list_of.hpp>
#include <boost/thread/thread.hpp>
#include <queue>
#include <mutex>
#include <video_stream_opencv/VideoStreamConfig.h>

namespace fs = boost::filesystem;

namespace video_stream_opencv {

class VideoStreamNodelet: public nodelet::Nodelet {
protected:
boost::shared_ptr<ros::NodeHandle> nh, pnh;
image_transport::CameraPublisher pub;
boost::shared_ptr<dynamic_reconfigure::Server<VideoStreamConfig> > dyn_srv;
std::mutex q_mutex, s_mutex;
std::queue<cv::Mat> framesQueue;
cv::Mat frame;
boost::shared_ptr<cv::VideoCapture> cap;
std::string video_stream_provider;
std::string video_stream_provider_type;
std::string camera_name;
std::string camera_info_url;
std::string frame_id;
double set_camera_fps;
double fps;
int max_queue_size;
bool loop_videofile;
int subscriber_num;
int width_target;
int height_target;
bool flip_horizontal;
bool flip_vertical;
bool capture_thread_running;
bool reopen_on_read_failure;
boost::thread capture_thread;
ros::Timer publish_timer;
sensor_msgs::CameraInfo cam_info_msg;

// Based on the ros tutorial on transforming opencv images to Image messages

virtual sensor_msgs::CameraInfo get_default_camera_info_from_image(sensor_msgs::ImagePtr img){
    sensor_msgs::CameraInfo cam_info_msg;
    cam_info_msg.header.frame_id = img->header.frame_id;
    // Fill image size
    cam_info_msg.height = img->height;
    cam_info_msg.width = img->width;
    NODELET_INFO_STREAM("The image width is: " << img->width);
    NODELET_INFO_STREAM("The image height is: " << img->height);
    // Add the most common distortion model as sensor_msgs/CameraInfo says
    cam_info_msg.distortion_model = "plumb_bob";
    // Don't let distorsion matrix be empty
    cam_info_msg.D.resize(5, 0.0);
    // Give a reasonable default intrinsic camera matrix
    cam_info_msg.K = boost::assign::list_of(1.0) (0.0) (img->width/2.0)
            (0.0) (1.0) (img->height/2.0)
            (0.0) (0.0) (1.0);
    // Give a reasonable default rectification matrix
    cam_info_msg.R = boost::assign::list_of (1.0) (0.0) (0.0)
            (0.0) (1.0) (0.0)
            (0.0) (0.0) (1.0);
    // Give a reasonable default projection matrix
    cam_info_msg.P = boost::assign::list_of (1.0) (0.0) (img->width/2.0) (0.0)
            (0.0) (1.0) (img->height/2.0) (0.0)
            (0.0) (0.0) (1.0) (0.0);
    return cam_info_msg;
}


virtual void do_capture() {
    NODELET_DEBUG("Capture thread started");
    cv::Mat frame;
    ros::Rate camera_fps_rate(set_camera_fps);

    int frame_counter = 0;
    // Read frames as fast as possible
    capture_thread_running = true;
    while (nh->ok() && capture_thread_running && subscriber_num > 0) {
        if (!cap->isOpened()) {
          NODELET_WARN("Waiting for device...");
          cv::waitKey(100);
          continue;
        }
        if (!cap->read(frame)) {
          NODELET_ERROR("Could not capture frame");
          if (reopen_on_read_failure) {
            NODELET_WARN("trying to reopen the device");
            unsubscribe();
            subscribe();
          }
        }

        frame_counter++;
        if (video_stream_provider_type == "videofile")
        {
            camera_fps_rate.sleep();
        }
        if (video_stream_provider_type == "videofile" &&
            frame_counter == cap->get(CV_CAP_PROP_FRAME_COUNT)) 
        {
            if (loop_videofile)
            {
                cap->open(video_stream_provider);
                frame_counter = 0;
            }
            else {
              NODELET_INFO("Reached the end of frames");
              break;
            }
        }

        if(!frame.empty()) {
            std::lock_guard<std::mutex> g(q_mutex);
            // accumulate only until max_queue_size
            if (framesQueue.size() < max_queue_size) {
                framesQueue.push(frame.clone());
            }
            // once reached, drop the oldest frame
            else {
                framesQueue.pop();
                framesQueue.push(frame.clone());
            }
        }
    }
    NODELET_DEBUG("Capture thread finished");
}

virtual void do_publish(const ros::TimerEvent& event) {
    bool is_new_image = false;
    sensor_msgs::ImagePtr msg;
    std_msgs::Header header;
    header.frame_id = frame_id;
    {
        std::lock_guard<std::mutex> g(q_mutex);
        if (!framesQueue.empty()){
            frame = framesQueue.front();
            framesQueue.pop();
            is_new_image = true;
        }
    }

    // Check if grabbed frame is actually filled with some content
    if(!frame.empty()) {
        // From http://docs.opencv.org/modules/core/doc/operations_on_arrays.html#void flip(InputArray src, OutputArray dst, int flipCode)
        // FLIP_HORIZONTAL == 1, FLIP_VERTICAL == 0 or FLIP_BOTH == -1
        // Flip the image if necessary
        if (is_new_image){
          if (flip_horizontal && flip_vertical)
            cv::flip(frame, frame, -1);
          else if (flip_horizontal)
            cv::flip(frame, frame, 1);
          else if (flip_vertical)
            cv::flip(frame, frame, 0);
        }
        msg = cv_bridge::CvImage(header, "bgr8", frame).toImageMsg();
        // Create a default camera info if we didn't get a stored one on initialization
        if (cam_info_msg.distortion_model == ""){
            NODELET_WARN_STREAM("No calibration file given, publishing a reasonable default camera info.");
            cam_info_msg = get_default_camera_info_from_image(msg);
            // cam_info_manager.setCameraInfo(cam_info_msg);
        }
        // The timestamps are in sync thanks to this publisher
        pub.publish(*msg, cam_info_msg, ros::Time::now());
    }
}

std::string gstreamer_pipeline (int capture_width, int capture_height, int display_width, int display_height, int framerate, int flip_method) {
    return "nvarguscamerasrc ! video/x-raw(memory:NVMM), width=(int)" + std::to_string(capture_width) + ", height=(int)" +
           std::to_string(capture_height) + ", format=(string)NV12, framerate=(fraction)" + std::to_string(framerate) +
           "/1 ! nvvidconv flip-method=" + std::to_string(flip_method) + " ! video/x-raw, width=(int)" + std::to_string(display_width) + ", height=(int)" +
           std::to_string(display_height) + ", format=(string)BGRx ! videoconvert ! video/x-raw, format=(string)BGR ! appsink";
}

virtual void subscribe() {
  int capture_width = 1920 ;
  int capture_height = 1080 ;
  int display_width = 640 ;
  int display_height = 480 ;
  int framerate = 30 ;
  int flip_method = 0 ;

  std::string pipeline = gstreamer_pipeline(capture_width,
                                              capture_height,
                                              display_width,
                                              display_height,
                                              framerate,
                                              flip_method);
  std::cout << "Using pipeline: \n\t" << pipeline << "\n";


  ROS_DEBUG("Subscribe");
  cap.reset(new cv::VideoCapture);
  try {
    int device_num = std::stoi(video_stream_provider);
    NODELET_INFO_STREAM("Opening VideoCapture with provider: /dev/video" << device_num);
    cap->open(pipeline, cv::CAP_GSTREAMER);
  } catch (std::invalid_argument &ex) {
    NODELET_INFO_STREAM("Opening VideoCapture with provider: " << video_stream_provider);
    cap->open(video_stream_provider);
    if (!cap->isOpened()) {
      NODELET_FATAL_STREAM("Invalid 'video_stream_provider': " << video_stream_provider);
      return;
    }
  }
  NODELET_INFO_STREAM("Video stream provider type detected: " << video_stream_provider_type);

  double reported_camera_fps;
  // OpenCV 2.4 returns -1 (instead of a 0 as the spec says) and prompts an error
  // HIGHGUI ERROR: V4L2: Unable to get property <unknown property string>(5) - Invalid argument
  reported_camera_fps = cap->get(CV_CAP_PROP_FPS);
  if (reported_camera_fps > 0.0)
    NODELET_INFO_STREAM("Camera reports FPS: " << reported_camera_fps);
  else
    NODELET_INFO_STREAM("Backend can't provide camera FPS information");
    
  // 20190826 samliu
  //cap->set(CV_CAP_PROP_FPS, set_camera_fps);
  if(!cap->isOpened()){
    NODELET_ERROR_STREAM("Could not open the stream.");
    return;
  }
  // 20190826 samliu
  /*if (width_target != 0 && height_target != 0){
    cap->set(CV_CAP_PROP_FRAME_WIDTH, width_target);
    cap->set(CV_CAP_PROP_FRAME_HEIGHT, height_target);
  }*/

  try {
    capture_thread = boost::thread(
      boost::bind(&VideoStreamNodelet::do_capture, this));
    publish_timer = nh->createTimer(
      ros::Duration(1.0 / fps), &VideoStreamNodelet::do_publish, this);
  } catch (std::exception& e) {
    NODELET_ERROR_STREAM("Failed to start capture thread: " << e.what());
  }
}

virtual void unsubscribe() {
  ROS_DEBUG("Unsubscribe");
  publish_timer.stop();
  capture_thread_running = false;
  capture_thread.join();
  cap.reset();
}

virtual void connectionCallbackImpl() {
  std::lock_guard<std::mutex> lock(s_mutex);
  if (subscriber_num == 0) {
    subscriber_num++;
    subscribe();
  }
}

virtual void disconnectionCallbackImpl() {
  std::lock_guard<std::mutex> lock(s_mutex);
  bool always_subscribe = false;
  pnh->getParamCached("always_subscribe", always_subscribe);
  if (video_stream_provider == "videofile" || always_subscribe) {
    return;
  }

  subscriber_num--;
  if (subscriber_num == 0) {
    unsubscribe();
  }
}

virtual void connectionCallback(const image_transport::SingleSubscriberPublisher&) {
  connectionCallbackImpl();
}

virtual void infoConnectionCallback(const ros::SingleSubscriberPublisher&) {
  connectionCallbackImpl();
}

virtual void disconnectionCallback(const image_transport::SingleSubscriberPublisher&) {
  disconnectionCallbackImpl();
}

virtual void infoDisconnectionCallback(const ros::SingleSubscriberPublisher&) {
  disconnectionCallbackImpl();
}

virtual void configCallback(VideoStreamConfig& config, uint32_t level) {
    NODELET_DEBUG("configCallback");
    bool need_resubscribe = false;

    if (camera_name != config.camera_name ||
        camera_info_url != config.camera_info_url ||
        frame_id != config.frame_id) {
      camera_name = config.camera_name;
      camera_info_url = config.camera_info_url;
      frame_id = config.frame_id;
      NODELET_INFO_STREAM("Camera name: " << camera_name);
      NODELET_INFO_STREAM("Provided camera_info_url: '" << camera_info_url << "'");
      NODELET_INFO_STREAM("Publishing with frame_id: " << frame_id);
      camera_info_manager::CameraInfoManager cam_info_manager(*nh, camera_name, camera_info_url);
      // Get the saved camera info if any
      cam_info_msg = cam_info_manager.getCameraInfo();
      cam_info_msg.header.frame_id = frame_id;
    }

    if (set_camera_fps != config.set_camera_fps ||
        fps != config.fps) {
      if (config.fps > config.set_camera_fps) {
        NODELET_WARN_STREAM("Asked to publish at 'fps' (" << config.fps
                            << ") which is higher than the 'set_camera_fps' (" << config.set_camera_fps <<
                            "), we can't publish faster than the camera provides images.");
        config.fps = config.set_camera_fps;
      }
      set_camera_fps = config.set_camera_fps;
      fps = config.fps;
      NODELET_INFO_STREAM("Setting camera FPS to: " << set_camera_fps);
      NODELET_INFO_STREAM("Throttling to fps: " << fps);
      need_resubscribe = true;
    }

    if (max_queue_size != config.buffer_queue_size) {
      max_queue_size = config.buffer_queue_size;
      NODELET_INFO_STREAM("Setting buffer size for capturing frames to: " << max_queue_size);
    }

    if (flip_horizontal != config.flip_horizontal ||
        flip_vertical != config.flip_vertical) {
      flip_horizontal = config.flip_horizontal;
      flip_vertical = config.flip_vertical;
      NODELET_INFO_STREAM("Flip horizontal image is: " << ((flip_horizontal)?"true":"false"));
      NODELET_INFO_STREAM("Flip vertical image is: " << ((flip_vertical)?"true":"false"));
    }

    if (width_target != config.width ||
        height_target != config.height) {
      width_target = config.width;
      height_target = config.height;
      if (width_target != 0 && height_target != 0) {
        NODELET_INFO_STREAM("Forced image width is: " << width_target);
        NODELET_INFO_STREAM("Forced image height is: " << height_target);
      }
      need_resubscribe = true;
    }
    // 20190826 samliu
    /*if (cap && cap->isOpened()) {
      cap->set(CV_CAP_PROP_BRIGHTNESS, config.brightness);
      cap->set(CV_CAP_PROP_CONTRAST, config.contrast);
      cap->set(CV_CAP_PROP_HUE, config.hue);
      cap->set(CV_CAP_PROP_SATURATION, config.saturation);
      if (config.auto_exposure) {
        cap->set(CV_CAP_PROP_AUTO_EXPOSURE, 0.75);
        config.exposure = 0.5;
      } else {
        cap->set(CV_CAP_PROP_AUTO_EXPOSURE, 0.25);
        cap->set(CV_CAP_PROP_EXPOSURE, config.exposure);
      }
    }*/

    loop_videofile = config.loop_videofile;
    reopen_on_read_failure = config.reopen_on_read_failure;

    if (subscriber_num > 0 && need_resubscribe) {
      unsubscribe();
      subscribe();
    }
}

virtual void onInit() {
    nh.reset(new ros::NodeHandle(getNodeHandle()));
    pnh.reset(new ros::NodeHandle(getPrivateNodeHandle()));
    subscriber_num = 0;

    // provider can be an url (e.g.: rtsp://10.0.0.1:554) or a number of device, (e.g.: 0 would be /dev/video0)
    pnh->param<std::string>("video_stream_provider", video_stream_provider, "0");
    // check file type
    try {
      int device_num = std::stoi(video_stream_provider);
      video_stream_provider_type ="videodevice";
    } catch (std::invalid_argument &ex) {
      if (video_stream_provider.find("http://") != std::string::npos ||
          video_stream_provider.find("https://") != std::string::npos){
        video_stream_provider_type = "http_stream";
      }
      else if(video_stream_provider.find("rtsp://") != std::string::npos){
        video_stream_provider_type = "rtsp_stream";
      }
      else{
        fs::file_type file_type = fs::status(fs::path(video_stream_provider)).type();
        switch (file_type) {
        case fs::file_type::character_file:
        case fs::file_type::block_file:
          video_stream_provider_type = "videodevice";
          break;
        case fs::file_type::regular_file:
          video_stream_provider_type = "videofile";
          break;
        default:
          video_stream_provider_type = "unknown";
        }
      }
    }

    // set parameters from dynamic reconfigure server
    dyn_srv = boost::make_shared<dynamic_reconfigure::Server<VideoStreamConfig> >(*pnh);
    auto f = boost::bind(&VideoStreamNodelet::configCallback, this, _1, _2);
    dyn_srv->setCallback(f);

    subscriber_num = 0;
    image_transport::SubscriberStatusCallback connect_cb =
      boost::bind(&VideoStreamNodelet::connectionCallback, this, _1);
    ros::SubscriberStatusCallback info_connect_cb =
      boost::bind(&VideoStreamNodelet::infoConnectionCallback, this, _1);
    image_transport::SubscriberStatusCallback disconnect_cb =
      boost::bind(&VideoStreamNodelet::disconnectionCallback, this, _1);
    ros::SubscriberStatusCallback info_disconnect_cb =
      boost::bind(&VideoStreamNodelet::infoDisconnectionCallback, this, _1);
    pub = image_transport::ImageTransport(*nh).advertiseCamera(
      "image_raw", 1,
      connect_cb, connect_cb,
      info_connect_cb, info_connect_cb,
      ros::VoidPtr(), false);
}

virtual ~VideoStreamNodelet() {
  if (subscriber_num > 0)
    subscriber_num = 0;
    unsubscribe();
}
};
} // namespace

#include <pluginlib/class_list_macros.h>
PLUGINLIB_EXPORT_CLASS(video_stream_opencv::VideoStreamNodelet, nodelet::Nodelet)

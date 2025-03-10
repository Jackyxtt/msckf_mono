/*
 * COPYRIGHT AND PERMISSION NOTICE
 * Penn Software MSCKF_VIO
 * Copyright (C) 2017 The Trustees of the University of Pennsylvania
 * All rights reserved.
 */

#include <iostream>
#include <algorithm>
#include <set>
#include <eigen3/Eigen/Dense>

#include <sensor_msgs/image_encodings.h>
#include <random_numbers/random_numbers.h>

#include <msckf_vio/CameraMeasurement.h>
#include <msckf_vio/TrackingInfo.h>
#include <msckf_vio/image_processor.h>
#include <msckf_vio/utils.h>

using namespace std;
using namespace cv;
using namespace Eigen;

namespace msckf_vio {
ImageProcessor::ImageProcessor(ros::NodeHandle& n) :
  nh(n),
  is_first_img(true),
  //img_transport(n),
  stereo_sub(10),
  // GridFeatures定义：
  //  typedef std::map<int, std::vector<FeatureMetaData> > GridFeatures;
  
  // prev_features_ptr定义
  // boost::shared_ptr<GridFeatures> prev_features_ptr;
  // boost::shared_ptr<GridFeatures> curr_features_ptr;
  prev_features_ptr(new GridFeatures()),
  curr_features_ptr(new GridFeatures()) {
  return;
}

ImageProcessor::~ImageProcessor() {
  destroyAllWindows();
  //ROS_INFO("Feature lifetime statistics:");
  //featureLifetimeStatistics();
  return;
}

/**
 * @brief ROS的参数服务器中读取相关参数
 *
 * 配置文件位于/config/
 * 读取相机模型的类型、相机内参以及相机与imu之间的外参
 * 读取图像的分辨率（长宽）
 */
bool ImageProcessor::loadParameters() {
  // Camera calibration parameters
  nh.param<string>("cam0/distortion_model",
      cam0_distortion_model, string("radtan"));
  nh.param<string>("cam1/distortion_model",
      cam1_distortion_model, string("radtan"));

  vector<int> cam0_resolution_temp(2);
  nh.getParam("cam0/resolution", cam0_resolution_temp);
  cam0_resolution[0] = cam0_resolution_temp[0];
  cam0_resolution[1] = cam0_resolution_temp[1];

  vector<int> cam1_resolution_temp(2);
  nh.getParam("cam1/resolution", cam1_resolution_temp);
  cam1_resolution[0] = cam1_resolution_temp[0];
  cam1_resolution[1] = cam1_resolution_temp[1];

  vector<double> cam0_intrinsics_temp(4);
  nh.getParam("cam0/intrinsics", cam0_intrinsics_temp);
  cam0_intrinsics[0] = cam0_intrinsics_temp[0];
  cam0_intrinsics[1] = cam0_intrinsics_temp[1];
  cam0_intrinsics[2] = cam0_intrinsics_temp[2];
  cam0_intrinsics[3] = cam0_intrinsics_temp[3];

  vector<double> cam1_intrinsics_temp(4);
  nh.getParam("cam1/intrinsics", cam1_intrinsics_temp);
  cam1_intrinsics[0] = cam1_intrinsics_temp[0];
  cam1_intrinsics[1] = cam1_intrinsics_temp[1];
  cam1_intrinsics[2] = cam1_intrinsics_temp[2];
  cam1_intrinsics[3] = cam1_intrinsics_temp[3];

  vector<double> cam0_distortion_coeffs_temp(4);
  nh.getParam("cam0/distortion_coeffs",
      cam0_distortion_coeffs_temp);
  cam0_distortion_coeffs[0] = cam0_distortion_coeffs_temp[0];
  cam0_distortion_coeffs[1] = cam0_distortion_coeffs_temp[1];
  cam0_distortion_coeffs[2] = cam0_distortion_coeffs_temp[2];
  cam0_distortion_coeffs[3] = cam0_distortion_coeffs_temp[3];

  vector<double> cam1_distortion_coeffs_temp(4);
  nh.getParam("cam1/distortion_coeffs",
      cam1_distortion_coeffs_temp);
  cam1_distortion_coeffs[0] = cam1_distortion_coeffs_temp[0];
  cam1_distortion_coeffs[1] = cam1_distortion_coeffs_temp[1];
  cam1_distortion_coeffs[2] = cam1_distortion_coeffs_temp[2];
  cam1_distortion_coeffs[3] = cam1_distortion_coeffs_temp[3];

  // getTransformCV的作用是讲kalibr标定结果的格式转换为opencv格式
  // 得到imu、cam0和cam1之间的外参数
  cv::Mat     T_imu_cam0 = utils::getTransformCV(nh, "cam0/T_cam_imu");
  cv::Matx33d R_imu_cam0(T_imu_cam0(cv::Rect(0,0,3,3)));
  cv::Vec3d   t_imu_cam0 = T_imu_cam0(cv::Rect(3,0,1,3));
  R_cam0_imu = R_imu_cam0.t();
  t_cam0_imu = -R_imu_cam0.t() * t_imu_cam0;

  cv::Mat T_cam0_cam1 = utils::getTransformCV(nh, "cam1/T_cn_cnm1");
  cv::Mat T_imu_cam1 = T_cam0_cam1 * T_imu_cam0;
  cv::Matx33d R_imu_cam1(T_imu_cam1(cv::Rect(0,0,3,3)));
  cv::Vec3d   t_imu_cam1 = T_imu_cam1(cv::Rect(3,0,1,3));
  R_cam1_imu = R_imu_cam1.t();
  t_cam1_imu = -R_imu_cam1.t() * t_imu_cam1;

  // Processor parameters
  nh.param<int>("grid_row", processor_config.grid_row, 4);
  nh.param<int>("grid_col", processor_config.grid_col, 4);
  nh.param<int>("grid_min_feature_num",
      processor_config.grid_min_feature_num, 2);
  nh.param<int>("grid_max_feature_num",
      processor_config.grid_max_feature_num, 4);
  nh.param<int>("pyramid_levels",
      processor_config.pyramid_levels, 3);
  nh.param<int>("patch_size",
      processor_config.patch_size, 31);
  nh.param<int>("fast_threshold",
      processor_config.fast_threshold, 20);
  nh.param<int>("max_iteration",
      processor_config.max_iteration, 30);
  nh.param<double>("track_precision",
      processor_config.track_precision, 0.01);
  nh.param<double>("ransac_threshold",
      processor_config.ransac_threshold, 3);
  nh.param<double>("stereo_threshold",
      processor_config.stereo_threshold, 3);

  ROS_INFO("===========================================");
  ROS_INFO("cam0_resolution: %d, %d",
      cam0_resolution[0], cam0_resolution[1]);
  ROS_INFO("cam0_intrinscs: %f, %f, %f, %f",
      cam0_intrinsics[0], cam0_intrinsics[1],
      cam0_intrinsics[2], cam0_intrinsics[3]);
  ROS_INFO("cam0_distortion_model: %s",
      cam0_distortion_model.c_str());
  ROS_INFO("cam0_distortion_coefficients: %f, %f, %f, %f",
      cam0_distortion_coeffs[0], cam0_distortion_coeffs[1],
      cam0_distortion_coeffs[2], cam0_distortion_coeffs[3]);

  ROS_INFO("cam1_resolution: %d, %d",
      cam1_resolution[0], cam1_resolution[1]);
  ROS_INFO("cam1_intrinscs: %f, %f, %f, %f",
      cam1_intrinsics[0], cam1_intrinsics[1],
      cam1_intrinsics[2], cam1_intrinsics[3]);
  ROS_INFO("cam1_distortion_model: %s",
      cam1_distortion_model.c_str());
  ROS_INFO("cam1_distortion_coefficients: %f, %f, %f, %f",
      cam1_distortion_coeffs[0], cam1_distortion_coeffs[1],
      cam1_distortion_coeffs[2], cam1_distortion_coeffs[3]);

  cout << R_imu_cam0 << endl;
  cout << t_imu_cam0.t() << endl;

  ROS_INFO("grid_row: %d",
      processor_config.grid_row);
  ROS_INFO("grid_col: %d",
      processor_config.grid_col);
  ROS_INFO("grid_min_feature_num: %d",
      processor_config.grid_min_feature_num);
  ROS_INFO("grid_max_feature_num: %d",
      processor_config.grid_max_feature_num);
  ROS_INFO("pyramid_levels: %d",
      processor_config.pyramid_levels);
  ROS_INFO("patch_size: %d",
      processor_config.patch_size);
  ROS_INFO("fast_threshold: %d",
      processor_config.fast_threshold);
  ROS_INFO("max_iteration: %d",
      processor_config.max_iteration);
  ROS_INFO("track_precision: %f",
      processor_config.track_precision);
  ROS_INFO("ransac_threshold: %f",
      processor_config.ransac_threshold);
  ROS_INFO("stereo_threshold: %f",
      processor_config.stereo_threshold);
  ROS_INFO("===========================================");
  return true;
}

/**
 * @brief 视觉前端初始化，从ROS的参数服务器中读取相关参数以及创建ros发布和订阅的主题
 *
 * 载入参数服务器中的相关参数
 * 创建ros发布和订阅的主题
 */
    bool ImageProcessor::initialize() {
      if (!loadParameters()) return false;
      ROS_INFO("Finish loading ROS parameters...");

      // Create feature detector.
      detector_ptr = FastFeatureDetector::create(
              processor_config.fast_threshold);

      if (!createRosIO()) return false;
      ROS_INFO("Finish creating ROS IO...");

      return true;
    }

/**
 * @brief 创建ros发布和订阅的主题
 *
 * 发布节点：相机的特征量测和跟踪信息
 * 订阅节点：两个相机的图像以及imu
 */
bool ImageProcessor::createRosIO() {
  feature_pub = nh.advertise<CameraMeasurement>(
      "features", 3);
  tracking_info_pub = nh.advertise<TrackingInfo>(
      "tracking_info", 1);
  image_transport::ImageTransport it(nh);
  debug_stereo_pub = it.advertise("debug_stereo_image", 1);

  cam0_img_sub.subscribe(nh, "cam0_image", 10);
  cam1_img_sub.subscribe(nh, "cam1_image", 10);
  stereo_sub.connectInput(cam0_img_sub, cam1_img_sub);
  stereo_sub.registerCallback(&ImageProcessor::stereoCallback, this);
  imu_sub = nh.subscribe("imu", 50,
      &ImageProcessor::imuCallback, this);

  return true;
}

/**
 * @brief 将imu的消息类型保存在缓冲中
 *
 */
void ImageProcessor::stereoCallback(
    const sensor_msgs::ImageConstPtr& cam0_img,
    const sensor_msgs::ImageConstPtr& cam1_img) {

  cout << "==================================" << endl;
        cout << "get image here" << endl;

  // Get the current image.
  // 两个图像消息类型指针
  cam0_curr_img_ptr = cv_bridge::toCvShare(cam0_img,
      sensor_msgs::image_encodings::MONO8);
  cam1_curr_img_ptr = cv_bridge::toCvShare(cam1_img,
      sensor_msgs::image_encodings::MONO8);

  // Build the image pyramids once since they're used at multiple places
  createImagePyramids();

  // Detect features in the first frame.
  if (is_first_img) {
    // 第一帧图像用于初始化：提取匹配的特征点
    ros::Time start_time = ros::Time::now();
    initializeFirstFrame();
      std::cout << "detect first image" << std::endl;
    //ROS_INFO("Detection time: %f",
    //    (ros::Time::now()-start_time).toSec());
    is_first_img = false;

    // Draw results.
    // 将提取到的关键点和图像发布，用于rviz的显示
    start_time = ros::Time::now();
    drawFeaturesStereo();
    //ROS_INFO("Draw features: %f",
    //    (ros::Time::now()-start_time).toSec());
  }
  else {
    // Track the feature in the previous image.
      // 非第一帧关键帧，需要
    ros::Time start_time = ros::Time::now();
    trackFeatures();
    //ROS_INFO("Tracking time: %f",
    //    (ros::Time::now()-start_time).toSec());

    // Add new features into the current image.
    start_time = ros::Time::now();
    addNewFeatures();
    //ROS_INFO("Addition time: %f",
    //    (ros::Time::now()-start_time).toSec());

    // Add new features into the current image.
    start_time = ros::Time::now();
    pruneGridFeatures();
    //ROS_INFO("Prune grid features: %f",
    //    (ros::Time::now()-start_time).toSec());

    // Draw results.
    start_time = ros::Time::now();
    drawFeaturesStereo();
    //ROS_INFO("Draw features: %f",
    //    (ros::Time::now()-start_time).toSec());
  }

  //ros::Time start_time = ros::Time::now();
  //updateFeatureLifetime();
  //ROS_INFO("Statistics: %f",
  //    (ros::Time::now()-start_time).toSec());

  // Publish features in the current image.
  ros::Time start_time = ros::Time::now();
  publish();
  //ROS_INFO("Publishing: %f",
  //    (ros::Time::now()-start_time).toSec());

  // Update the previous image and previous features.
  // 下一时刻的上一时刻相关信息即为当前时刻的信息
  cam0_prev_img_ptr = cam0_curr_img_ptr;
  prev_features_ptr = curr_features_ptr;
  std::swap(prev_cam0_pyramid_, curr_cam0_pyramid_);

  // Initialize the current features to empty vectors.
  // 将当前时刻的特征点向量中的信息清零
  curr_features_ptr.reset(new GridFeatures());
  for (int code = 0; code <
      processor_config.grid_row*processor_config.grid_col; ++code) {
    (*curr_features_ptr)[code] = vector<FeatureMetaData>(0);
  }

  return;
}

/**
 * @brief 将imu的消息类型保存在缓冲中
 *
 */
void ImageProcessor::imuCallback(
    const sensor_msgs::ImuConstPtr& msg) {
  // Wait for the first image to be set.
  // 第一帧图像设置后再对imu做保存
  if (is_first_img) return;
  // 保存imu的消息类型
  imu_msg_buffer.push_back(*msg);
  return;
}

/**
 * @brief 创建图像金字塔
 *
 * 调用了OpenCV的函数buildOpticalFlowPyramid构建图像金字塔
 */
void ImageProcessor::createImagePyramids() {
  const Mat& curr_cam0_img = cam0_curr_img_ptr->image;

  // OpenCV的函数
  // Constructs the image pyramid which can be passed to calcOpticalFlowPyrLK.
  buildOpticalFlowPyramid(
      curr_cam0_img, curr_cam0_pyramid_,
      Size(processor_config.patch_size, processor_config.patch_size),
      processor_config.pyramid_levels, true, BORDER_REFLECT_101,
      BORDER_CONSTANT, false);

  const Mat& curr_cam1_img = cam1_curr_img_ptr->image;
  buildOpticalFlowPyramid(
      curr_cam1_img, curr_cam1_pyramid_,
      Size(processor_config.patch_size, processor_config.patch_size),
      processor_config.pyramid_levels, true, BORDER_REFLECT_101,
      BORDER_CONSTANT, false);
}

/**
 * @brief 第一帧图像初始化
 * 提取fast关键点，用光流进行跟踪匹配关键点对（klt）
 * 对图像画格子，对格子内提取一定数量的特征点
 *
 */
void ImageProcessor::initializeFirstFrame() {
  // Size of each grid.
  const Mat& img = cam0_curr_img_ptr->image;
  static int grid_height = img.rows / processor_config.grid_row;
  static int grid_width = img.cols / processor_config.grid_col;

  // Detect new features on the frist image.
  // 提取FAST关键点
  vector<KeyPoint> new_features(0);
  detector_ptr->detect(img, new_features);

  // Find the stereo matched points for the newly
  // detected features.
  // FAST关键点位于图像的像素坐标位置
  vector<cv::Point2f> cam0_points(new_features.size());
  for (int i = 0; i < new_features.size(); ++i)
    cam0_points[i] = new_features[i].pt;

  // 光流跟踪匹配两帧的关键点
  // 用外参计算E剔除明显不可能的点
  vector<cv::Point2f> cam1_points(0);
  vector<unsigned char> inlier_markers(0);
  stereoMatch(cam0_points, cam1_points, inlier_markers);

  // 保存符合要求的内点以及响应强度
  vector<cv::Point2f> cam0_inliers(0);
  vector<cv::Point2f> cam1_inliers(0);
  vector<float> response_inliers(0);
  for (int i = 0; i < inlier_markers.size(); ++i) {
    if (inlier_markers[i] == 0) continue;
    cam0_inliers.push_back(cam0_points[i]);
    cam1_inliers.push_back(cam1_points[i]);
    response_inliers.push_back(new_features[i].response);//像素强度
  }

  // Group the features into grids
  // 图像画格子，将特征点分配到各个格子中
  // GridFeatures为map<int,std::vector<FeatureMetaData>>
  GridFeatures grid_new_features;
  for (int code = 0; code <
      processor_config.grid_row*processor_config.grid_col; ++code)
      grid_new_features[code] = vector<FeatureMetaData>(0);

  for (int i = 0; i < cam0_inliers.size(); ++i) {
    const cv::Point2f& cam0_point = cam0_inliers[i];
    const cv::Point2f& cam1_point = cam1_inliers[i];
    const float& response = response_inliers[i];

    // 按照特征点位置与格子大小的关系，分别分配到每个格子中
    int row = static_cast<int>(cam0_point.y / grid_height);
    int col = static_cast<int>(cam0_point.x / grid_width);
    int code = row*processor_config.grid_col + col;

    // 格子中的特征点信息保存
    FeatureMetaData new_feature;
    new_feature.response = response;
    new_feature.cam0_point = cam0_point;
    new_feature.cam1_point = cam1_point;
    grid_new_features[code].push_back(new_feature);
  }

  // Sort the new features in each grid based on its response.
  // 按照特征响应对格子中的所有特征点进行排序
  for (auto& item : grid_new_features)
    std::sort(item.second.begin(), item.second.end(),
        &ImageProcessor::featureCompareByResponse);

  // Collect new features within each grid with high response.
  // 按照预设的阈值对每个格子保留特征点
  for (int code = 0; code <
      processor_config.grid_row*processor_config.grid_col; ++code) {
    // 将当前的关键点保存到curr_features_ptr
    vector<FeatureMetaData>& features_this_grid = (*curr_features_ptr)[code];
    // ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ 
    // | | | | | | | | | |  将新提取到的特征点new_features_this_grid保存到curr_features_ptr
    vector<FeatureMetaData>& new_features_this_grid = grid_new_features[code];//new_features_this_grid是grid_new_features的引用，已经根据特征强度排好了序

    // 按照响应值从大到小取指定的特征点
    for (int k = 0; k < processor_config.grid_min_feature_num &&
        k < new_features_this_grid.size(); ++k) {
      features_this_grid.push_back(new_features_this_grid[k]);
      features_this_grid.back().id = next_feature_id++;
      features_this_grid.back().lifetime = 1;
    }
  }

  return;
}

/**
 * @brief 根据单应性原理：已知一个平面的关键点可以得到另一个平面的关键点
 * @param input_pts：上一时刻的第一个相机对应的关键点
 * @param R_p_c: 利用imu数据计算得到的前后两个时刻图像帧的旋转初值
 * @param intrinsics:相机内参
 * @return compensated_pts:根据上一帧图像中的关键点位置预测得到当前帧的关键点位置
 *
 */
void ImageProcessor::predictFeatureTracking(
    const vector<cv::Point2f>& input_pts,
    const cv::Matx33f& R_p_c,//R_cur_last
    const cv::Vec4d& intrinsics,
    vector<cv::Point2f>& compensated_pts) {

  // Return directly if there are no input features.
  if (input_pts.size() == 0) {
    compensated_pts.clear();
    return;
  }
  compensated_pts.resize(input_pts.size());

  // Intrinsic matrix.
  // 相机内参矩阵K
  cv::Matx33f K(
      intrinsics[0], 0.0, intrinsics[2],
      0.0, intrinsics[1], intrinsics[3],
      0.0, 0.0, 1.0);

  // 单应性矩阵的计算，公式推到(忽略了平移t):
  // x1 = K * X1_c; x2 = K * X2_c
  // x2_c = H * x1_c; X1_C = R_1_2 * X2_c
  // x1 = K * R_1_2 * K^inv * x2 --> H = K * R_1_2 * K^inv
  // TUDO:这里应该还有平移向量，预测一个值所以可以去掉？
  cv::Matx33f H = K * R_p_c * K.inv();

  for (int i = 0; i < input_pts.size(); ++i) {
    cv::Vec3f p1(input_pts[i].x, input_pts[i].y, 1.0f);
    // 两个平面中的匹配点满足: x2 = H * x1
    // 归一化后的齐次坐标即另一个平面上匹配点的图像坐标
    cv::Vec3f p2 = H * p1;
    compensated_pts[i].x = p2[0] / p2[2];
    compensated_pts[i].y = p2[1] / p2[2];
  }

  return;
}

/**
 * @brief 第一帧图像初始化
 * 提取fast关键点，用光流进行跟踪匹配关键点对（klt）
 * 对图像画格子，对格子内提取一定数量的特征点
 *
 */
void ImageProcessor::trackFeatures() {
  // Size of each grid.
  // 长宽方向的格子的数量
  static int grid_height =
    cam0_curr_img_ptr->image.rows / processor_config.grid_row;
  static int grid_width =
    cam0_curr_img_ptr->image.cols / processor_config.grid_col;

  // Compute a rough relative rotation which takes a vector
  // from the previous frame to the current frame.
  // 根据imu的信息对前后时刻的图像的旋转计算得到一个初值
  Matx33f cam0_R_p_c;
  Matx33f cam1_R_p_c;
  integrateImuData(cam0_R_p_c, cam1_R_p_c);//R_previous_cur 上一帧到当前帧

  // Organize the features in the previous image.
  // 获取前一时刻的双目图像特征的信息
  vector<FeatureIDType> prev_ids(0);
  vector<int> prev_lifetime(0);
  vector<Point2f> prev_cam0_points(0);
  vector<Point2f> prev_cam1_points(0);

  for (const auto& item : *prev_features_ptr) {
    for (const auto& prev_feature : item.second) {
      prev_ids.push_back(prev_feature.id);
      prev_lifetime.push_back(prev_feature.lifetime);
      prev_cam0_points.push_back(prev_feature.cam0_point);
      prev_cam1_points.push_back(prev_feature.cam1_point);
    }
  }

  // Number of the features before tracking.
  // 获取前一时刻跟踪匹配成功的关键点对数量
  before_tracking = prev_cam0_points.size();

  // Abort tracking if there is no features in
  // the previous frame.
  if (prev_ids.size() == 0) return;

  // Track features using LK optical flow method.
  vector<Point2f> curr_cam0_points(0);
  vector<unsigned char> track_inliers(0);

  // 根据imu计算得到的旋转值以及单应性原理来预测当前帧的关键点位置
  // 得到curr_cam0_points，归一化平面坐标
  predictFeatureTracking(prev_cam0_points,
      cam0_R_p_c, cam0_intrinsics, curr_cam0_points);

  // LK光流对上一时刻的关键点位置做跟踪匹配
  calcOpticalFlowPyrLK(
      prev_cam0_pyramid_, curr_cam0_pyramid_,
      prev_cam0_points, curr_cam0_points,
      track_inliers, noArray(),
      Size(processor_config.patch_size, processor_config.patch_size),
      processor_config.pyramid_levels,
      TermCriteria(TermCriteria::COUNT+TermCriteria::EPS,
        processor_config.max_iteration,
        processor_config.track_precision),
      cv::OPTFLOW_USE_INITIAL_FLOW);//使用了OPTFLOW_USE_INITIAL_FLOW标志位，需要提供下一帧的特征点位置初值，curr_cam0_points既是input也是output

  // Mark those tracked points out of the image region
  // as untracked.
  for (int i = 0; i < curr_cam0_points.size(); ++i) {
    if (track_inliers[i] == 0) continue;
    if (curr_cam0_points[i].y < 0 ||
        curr_cam0_points[i].y > cam0_curr_img_ptr->image.rows-1 ||
        curr_cam0_points[i].x < 0 ||
        curr_cam0_points[i].x > cam0_curr_img_ptr->image.cols-1)
      track_inliers[i] = 0;
  }

  // Collect the tracked points.
  vector<FeatureIDType> prev_tracked_ids(0);
  vector<int> prev_tracked_lifetime(0);
  vector<Point2f> prev_tracked_cam0_points(0);
  vector<Point2f> prev_tracked_cam1_points(0);
  vector<Point2f> curr_tracked_cam0_points(0);

  // 移除所有track_inliers值为0的关键点
  removeUnmarkedElements(
      prev_ids, track_inliers, prev_tracked_ids);
  removeUnmarkedElements(
      prev_lifetime, track_inliers, prev_tracked_lifetime);
  removeUnmarkedElements(
      prev_cam0_points, track_inliers, prev_tracked_cam0_points);
  removeUnmarkedElements(
      prev_cam1_points, track_inliers, prev_tracked_cam1_points);
  removeUnmarkedElements(
      curr_cam0_points, track_inliers, curr_tracked_cam0_points);

  // Number of features left after tracking.
  // 最终所有跟踪到内点对的数量
  after_tracking = curr_tracked_cam0_points.size();


  // Outlier removal involves three steps, which forms a close
  // loop between the previous and current frames of cam0 (left)
  // and cam1 (right). Assuming the stereo matching between the
  // previous cam0 and cam1 images are correct, the three steps are:
  //
  // prev frames cam0 ----------> cam1
  //              |                |
  //              |ransac          |ransac
  //              |   stereo match |
  // curr frames cam0 ----------> cam1
  //
  // 1) Stereo matching between current images of cam0 and cam1.
  // 2) RANSAC between previous and current images of cam0.
  // 3) RANSAC between previous and current images of cam1.
  //
  // For Step 3, tracking between the images is no longer needed.
  // The stereo matching results are directly used in the RANSAC.

  // Step 1: stereo matching.
  // 第一步： 对当前时刻的双目进行匹配
  vector<Point2f> curr_cam1_points(0);
  vector<unsigned char> match_inliers(0);
  stereoMatch(curr_tracked_cam0_points, curr_cam1_points, match_inliers);

  vector<FeatureIDType> prev_matched_ids(0);//typedef long long int FeatureIDType;
  vector<int> prev_matched_lifetime(0);
  vector<Point2f> prev_matched_cam0_points(0);
  vector<Point2f> prev_matched_cam1_points(0);
  vector<Point2f> curr_matched_cam0_points(0);
  vector<Point2f> curr_matched_cam1_points(0);

  removeUnmarkedElements(
      prev_tracked_ids, match_inliers, prev_matched_ids);
  removeUnmarkedElements(
      prev_tracked_lifetime, match_inliers, prev_matched_lifetime);
  removeUnmarkedElements(
      prev_tracked_cam0_points, match_inliers, prev_matched_cam0_points);
  removeUnmarkedElements(
      prev_tracked_cam1_points, match_inliers, prev_matched_cam1_points);
  removeUnmarkedElements(
      curr_tracked_cam0_points, match_inliers, curr_matched_cam0_points);
  removeUnmarkedElements(
      curr_cam1_points, match_inliers, curr_matched_cam1_points);

  // Number of features left after stereo matching.
  // 当前时刻两个相机匹配得到的关键点对的内点数量
  after_matching = curr_matched_cam0_points.size();

  // Step 2 and 3: RANSAC on temporal image pairs of cam0 and cam1.
  // 步骤2： 对同一个相机的不同时刻做RANSAC剔除外点
  vector<int> cam0_ransac_inliers(0);
  twoPointRansac(prev_matched_cam0_points, curr_matched_cam0_points,
      cam0_R_p_c, cam0_intrinsics, cam0_distortion_model,
      cam0_distortion_coeffs, processor_config.ransac_threshold,
      0.99, cam0_ransac_inliers);

  vector<int> cam1_ransac_inliers(0);
  twoPointRansac(prev_matched_cam1_points, curr_matched_cam1_points,
      cam1_R_p_c, cam1_intrinsics, cam1_distortion_model,
      cam1_distortion_coeffs, processor_config.ransac_threshold,
      0.99, cam1_ransac_inliers);

  // Number of features after ransac.
  after_ransac = 0;

  for (int i = 0; i < cam0_ransac_inliers.size(); ++i) {
    if (cam0_ransac_inliers[i] == 0 ||
        cam1_ransac_inliers[i] == 0) continue;
    int row = static_cast<int>(
        curr_matched_cam0_points[i].y / grid_height);
    int col = static_cast<int>(
        curr_matched_cam0_points[i].x / grid_width);
    int code = row*processor_config.grid_col + col;
    (*curr_features_ptr)[code].push_back(FeatureMetaData());

    FeatureMetaData& grid_new_feature = (*curr_features_ptr)[code].back();
    grid_new_feature.id = prev_matched_ids[i];
    grid_new_feature.lifetime = ++prev_matched_lifetime[i];
    grid_new_feature.cam0_point = curr_matched_cam0_points[i];
    grid_new_feature.cam1_point = curr_matched_cam1_points[i];

    ++after_ransac;
  }

  // Compute the tracking rate.
  int prev_feature_num = 0;
  for (const auto& item : *prev_features_ptr)
    prev_feature_num += item.second.size();

  int curr_feature_num = 0;
  for (const auto& item : *curr_features_ptr)
    curr_feature_num += item.second.size();

  // 以0.5Hz频率发布
  ROS_INFO_THROTTLE(0.5,
      "\033[0;32m candidates: %d; track: %d; match: %d; ransac: %d/%d=%f\033[0m",
      before_tracking, after_tracking, after_matching,
      curr_feature_num, prev_feature_num,
      static_cast<double>(curr_feature_num)/
      (static_cast<double>(prev_feature_num)+1e-5));
  //printf(
  //    "\033[0;32m candidates: %d; raw track: %d; stereo match: %d; ransac: %d/%d=%f\033[0m\n",
  //    before_tracking, after_tracking, after_matching,
  //    curr_feature_num, prev_feature_num,
  //    static_cast<double>(curr_feature_num)/
  //    (static_cast<double>(prev_feature_num)+1e-5));

  return;
}

/**
 * @brief 对两帧图像对做特征匹配，对极几何约束剔除外点
 * @param cam0_points：第一帧图像帧的关键点位置
 * @return cam1_points:第二帧图像中的关键点位置
 * @return inlier_markers:匹配成功返回1，否则为0
 *
 */
void ImageProcessor::stereoMatch(
    const vector<cv::Point2f>& cam0_points,
    vector<cv::Point2f>& cam1_points,
    vector<unsigned char>& inlier_markers) {

  if (cam0_points.size() == 0) return;

  // 对第二帧图像中的特征点位置初始化
  if(cam1_points.size() == 0) {
    // Initialize cam1_points by projecting cam0_points to cam1 using the
    // rotation from stereo extrinsics
    const cv::Matx33d R_cam0_cam1 = R_cam1_imu.t() * R_cam0_imu;
    vector<cv::Point2f> cam0_points_undistorted;

    // 第一个摄像头图像中的关键点位置矫正
    undistortPoints(cam0_points, cam0_intrinsics, cam0_distortion_model,
                    cam0_distortion_coeffs, cam0_points_undistorted,
                    R_cam0_cam1);
//      ROS_INFO_STREAM("Before undistorted: cam0_points[0] = "
//                              << cam0_points[0].x << " " << cam0_points[0].y);
//      ROS_INFO_STREAM("After undistorted: cam0_points[0] = "
//                              << cam0_points_undistorted[0].x << " " << cam0_points_undistorted[0].y);
    // 第二个摄像头中的关键点位置
    cam1_points = distortPoints(cam0_points_undistorted, cam1_intrinsics,
                                cam1_distortion_model, cam1_distortion_coeffs);
  }

  // Track features using LK optical flow method.
  // 采用LK光流跟踪关键点
  // 输入两个相机图像对应的金字塔以及第一个相机图像对应的关键点cam0_points
  // 输出光流跟踪到的第二个相机图像对应的关键点cam1_points
  // inlier_markers表示cam0_points中的点是否有对应的点
  calcOpticalFlowPyrLK(curr_cam0_pyramid_, curr_cam1_pyramid_,
      cam0_points, cam1_points,
      inlier_markers, noArray(),
      Size(processor_config.patch_size, processor_config.patch_size),
      processor_config.pyramid_levels,
      TermCriteria(TermCriteria::COUNT+TermCriteria::EPS,
                   processor_config.max_iteration,
                   processor_config.track_precision),
      cv::OPTFLOW_USE_INITIAL_FLOW);

  // Mark those tracked points out of the image region
  // as untracked.
  // 光流跟踪得到的点超过图像区域就标志为未跟踪的点
  for (int i = 0; i < cam1_points.size(); ++i) {
    if (inlier_markers[i] == 0) continue;
    if (cam1_points[i].y < 0 ||
        cam1_points[i].y > cam1_curr_img_ptr->image.rows-1 ||
        cam1_points[i].x < 0 ||
        cam1_points[i].x > cam1_curr_img_ptr->image.cols-1)
      inlier_markers[i] = 0;
  }

  // Compute the relative rotation between the cam0
  // frame and cam1 frame.
  const cv::Matx33d R_cam0_cam1 = R_cam1_imu.t() * R_cam0_imu;
  const cv::Vec3d t_cam0_cam1 = R_cam1_imu.t() * (t_cam0_imu-t_cam1_imu);
  // Compute the essential matrix.
  // 本质矩阵的计算公式：[t]x * R（见多视图几何）
  const cv::Matx33d t_cam0_cam1_hat(
      0.0, -t_cam0_cam1[2], t_cam0_cam1[1],
      t_cam0_cam1[2], 0.0, -t_cam0_cam1[0],
      -t_cam0_cam1[1], t_cam0_cam1[0], 0.0);
  const cv::Matx33d E = t_cam0_cam1_hat * R_cam0_cam1;

  // Further remove outliers based on the known
  // essential matrix.
  // 所有的匹配点应满足对极几何约束，不满足该条件就剔除

  // 图像点先去畸变
  vector<cv::Point2f> cam0_points_undistorted(0);
  vector<cv::Point2f> cam1_points_undistorted(0);
  undistortPoints(
      cam0_points, cam0_intrinsics, cam0_distortion_model,
      cam0_distortion_coeffs, cam0_points_undistorted);
  undistortPoints(
      cam1_points, cam1_intrinsics, cam1_distortion_model,
      cam1_distortion_coeffs, cam1_points_undistorted);

//  ROS_INFO_STREAM("undistorted: cam0_points[0] = "
//                  << cam0_points_undistorted[0].x << " " << cam0_points_undistorted[0].y);

  // 将两个相机的fx和fy取平均: f_a = (fx_0+fy_0+fx_1+fy_1)/4.0
  // norm_pixel_unit = 1 / f_a
  double norm_pixel_unit = 4.0 / (
      cam0_intrinsics[0]+cam0_intrinsics[1]+
      cam1_intrinsics[0]+cam1_intrinsics[1]);

  // 剔除明显不符合对极几何的点
  for (int i = 0; i < cam0_points_undistorted.size(); ++i) {
    if (inlier_markers[i] == 0) continue;
    // 齐次坐标
    cv::Vec3d pt0(cam0_points_undistorted[i].x,
        cam0_points_undistorted[i].y, 1.0);
    cv::Vec3d pt1(cam1_points_undistorted[i].x,
        cam1_points_undistorted[i].y, 1.0);
    // 根据本质矩阵得到极线
    // 极线计算公式：l' = F*x = (a,b,c)^t
    cv::Vec3d epipolar_line = E * pt0;
    // 第二个相机中的匹配点到极线的距离(l' = ax+by+c)
    double error = fabs((pt1.t() * epipolar_line)[0]) / sqrt(
        epipolar_line[0]*epipolar_line[0]+
        epipolar_line[1]*epipolar_line[1]);
    // 距离小于阈值就认为是外点，剔除
    if (error > processor_config.stereo_threshold*norm_pixel_unit)
      inlier_markers[i] = 0;
  }

  return;
}

void ImageProcessor::addNewFeatures() {
  const Mat& curr_img = cam0_curr_img_ptr->image;

  // Size of each grid.
  static int grid_height =
    cam0_curr_img_ptr->image.rows / processor_config.grid_row;
  static int grid_width =
    cam0_curr_img_ptr->image.cols / processor_config.grid_col;

  // Create a mask to avoid redetecting existing features.
  Mat mask(curr_img.rows, curr_img.cols, CV_8U, Scalar(1));

  for (const auto& features : *curr_features_ptr) {
    for (const auto& feature : features.second) {
      const int y = static_cast<int>(feature.cam0_point.y);
      const int x = static_cast<int>(feature.cam0_point.x);

      int up_lim = y-2, bottom_lim = y+3,
          left_lim = x-2, right_lim = x+3;
      if (up_lim < 0) up_lim = 0;
      if (bottom_lim > curr_img.rows) bottom_lim = curr_img.rows;
      if (left_lim < 0) left_lim = 0;
      if (right_lim > curr_img.cols) right_lim = curr_img.cols;

      Range row_range(up_lim, bottom_lim);
      Range col_range(left_lim, right_lim);
      mask(row_range, col_range) = 0;
    }
  }

  // Detect new features.
  vector<KeyPoint> new_features(0);
  detector_ptr->detect(curr_img, new_features, mask);

  // Collect the new detected features based on the grid.
  // Select the ones with top response within each grid afterwards.
  vector<vector<KeyPoint> > new_feature_sieve(
      processor_config.grid_row*processor_config.grid_col);

  //遍历新提取到的特征点，放入所在网格vector    
  for (const auto& feature : new_features) {
    int row = static_cast<int>(feature.pt.y / grid_height);
    int col = static_cast<int>(feature.pt.x / grid_width);
    new_feature_sieve[
      row*processor_config.grid_col+col].push_back(feature);
  }

  new_features.clear();

  // 遍历网格，对网格内的特征进行按照特征相应排序
  for (auto& item : new_feature_sieve) {
    if (item.size() > processor_config.grid_max_feature_num) {

      std::sort(item.begin(), item.end(),
          &ImageProcessor::keyPointCompareByResponse);
      
      // 删掉每个网格中超过提取数量阈值的点
      item.erase(
          item.begin()+processor_config.grid_max_feature_num, item.end());
    }
    new_features.insert(new_features.end(), item.begin(), item.end());
  }

  int detected_new_features = new_features.size();

  // Find the stereo matched points for the newly
  // detected features.
  // 对新提取到的特征进行左右目匹配
  vector<cv::Point2f> cam0_points(new_features.size());
  for (int i = 0; i < new_features.size(); ++i)
    cam0_points[i] = new_features[i].pt;

  vector<cv::Point2f> cam1_points(0);
  vector<unsigned char> inlier_markers(0);
  stereoMatch(cam0_points, cam1_points, inlier_markers);

  vector<cv::Point2f> cam0_inliers(0);
  vector<cv::Point2f> cam1_inliers(0);
  vector<float> response_inliers(0);
  for (int i = 0; i < inlier_markers.size(); ++i) {
    if (inlier_markers[i] == 0) continue;
    cam0_inliers.push_back(cam0_points[i]);
    cam1_inliers.push_back(cam1_points[i]);
    response_inliers.push_back(new_features[i].response);
  }

  int matched_new_features = cam0_inliers.size();

  if (matched_new_features < 5 &&
      static_cast<double>(matched_new_features)/
      static_cast<double>(detected_new_features) < 0.1)
    ROS_WARN("Images at [%f] seems unsynced...",
        cam0_curr_img_ptr->header.stamp.toSec());

  // 将新提取的特征放入网格
  // Group the features into grids
  GridFeatures grid_new_features;
  for (int code = 0; code <
      processor_config.grid_row*processor_config.grid_col; ++code)
      grid_new_features[code] = vector<FeatureMetaData>(0);

  for (int i = 0; i < cam0_inliers.size(); ++i) {
    const cv::Point2f& cam0_point = cam0_inliers[i];
    const cv::Point2f& cam1_point = cam1_inliers[i];
    const float& response = response_inliers[i];

    int row = static_cast<int>(cam0_point.y / grid_height);
    int col = static_cast<int>(cam0_point.x / grid_width);
    int code = row*processor_config.grid_col + col;

    FeatureMetaData new_feature;
    new_feature.response = response;
    new_feature.cam0_point = cam0_point;
    new_feature.cam1_point = cam1_point;
    grid_new_features[code].push_back(new_feature);
  }

  // Sort the new features in each grid based on its response.
  for (auto& item : grid_new_features)
    std::sort(item.second.begin(), item.second.end(),
        &ImageProcessor::featureCompareByResponse);

  int new_added_feature_num = 0;
  // Collect new features within each grid with high response.
  for (int code = 0; code <
      processor_config.grid_row*processor_config.grid_col; ++code) {
    
    // features_this_grid是当前特征的引用
    vector<FeatureMetaData>& features_this_grid = (*curr_features_ptr)[code];

    // new_features_this_grid是当前网格新特征的引用
    vector<FeatureMetaData>& new_features_this_grid = grid_new_features[code];

    // 当前网格满了则进行下一个
    if (features_this_grid.size() >=
        processor_config.grid_min_feature_num) continue;

    // 查看当前网格还有多少空位
    int vacancy_num = processor_config.grid_min_feature_num -
      features_this_grid.size();
    for (int k = 0;
        k < vacancy_num && k < new_features_this_grid.size(); ++k) {
      features_this_grid.push_back(new_features_this_grid[k]);
      features_this_grid.back().id = next_feature_id++;
      features_this_grid.back().lifetime = 1;

      ++new_added_feature_num;
    }
  }

  //printf("\033[0;33m detected: %d; matched: %d; new added feature: %d\033[0m\n",
  //    detected_new_features, matched_new_features, new_added_feature_num);

  return;
}

void ImageProcessor::pruneGridFeatures() {
  for (auto& item : *curr_features_ptr) {
    auto& grid_features = item.second;
    // Continue if the number of features in this grid does
    // not exceed the upper bound.
    if (grid_features.size() <=
        processor_config.grid_max_feature_num) continue;
    std::sort(grid_features.begin(), grid_features.end(),
        &ImageProcessor::featureCompareByLifetime);
    grid_features.erase(grid_features.begin()+
        processor_config.grid_max_feature_num,
        grid_features.end());
  }
  return;
}

/**
 * @brief 计算原图像帧关键点对应的矫正位置
 * @param pts_in：原图像帧的关键点位置
 * @param intrinsics:内参矩阵
 * @param distortion_model：相机模型
 * @param distortion_coeffs:畸变系数
 * @return pts_out:畸变矫正后的关键点位置
 * @param rectification_matrix:矫正矩阵，即两个相机之间的外参数
 * @param new_intrinsics:新的内参矩阵,默认值
 *
 */
// 返回校正后的归一化点，[u，v，1]的前两维
void ImageProcessor::undistortPoints(
    const vector<cv::Point2f>& pts_in,
    const cv::Vec4d& intrinsics,
    const string& distortion_model,
    const cv::Vec4d& distortion_coeffs,
    vector<cv::Point2f>& pts_out,
    const cv::Matx33d &rectification_matrix,
    const cv::Vec4d &new_intrinsics) {

  if (pts_in.size() == 0) return;

  const cv::Matx33d K(
      intrinsics[0], 0.0, intrinsics[2],
      0.0, intrinsics[1], intrinsics[3],
      0.0, 0.0, 1.0);

  const cv::Matx33d K_new(
      new_intrinsics[0], 0.0, new_intrinsics[2],
      0.0, new_intrinsics[1], new_intrinsics[3],
      0.0, 0.0, 1.0);

  // 畸变模型选择，一般为radtan
  // 将原图像中的关键点转换为未畸变的关键点
  // 畸变矫正后的点是归一化(相机坐标系下)的坐标，详见opencv的函数说明
  if (distortion_model == "radtan") {
    cv::undistortPoints(pts_in, pts_out, K, distortion_coeffs,
                        rectification_matrix, K_new);
  } else if (distortion_model == "equidistant") {
    cv::fisheye::undistortPoints(pts_in, pts_out, K, distortion_coeffs,
                                 rectification_matrix, K_new);
  } else {
    ROS_WARN_ONCE("The model %s is unrecognized, use radtan instead...",
                  distortion_model.c_str());
    cv::undistortPoints(pts_in, pts_out, K, distortion_coeffs,
                        rectification_matrix, K_new);
  }

  return;
}

/**
 * @brief 计算原图像帧关键点对应的矫正位置
 * @param pts_in：图像帧中已校正的关键点位置
 * @param intrinsics:内参矩阵
 * @param distortion_model：相机模型
 * @param distortion_coeffs:畸变系数
 * @return pts_out:投影得到的图像点位置
 *
 */
// 根据校正后的归一化坐标求像素坐标
vector<cv::Point2f> ImageProcessor::distortPoints(
    const vector<cv::Point2f>& pts_in,
    const cv::Vec4d& intrinsics,
    const string& distortion_model,
    const cv::Vec4d& distortion_coeffs) {

  const cv::Matx33d K(intrinsics[0], 0.0, intrinsics[2],
                      0.0, intrinsics[1], intrinsics[3],
                      0.0, 0.0, 1.0);

  vector<cv::Point2f> pts_out;
  if (distortion_model == "radtan") {
    // 针孔模型
    vector<cv::Point3f> homogenous_pts;
    // 将图像坐标（u,v）转换为齐次坐标(u,v,1)
    cv::convertPointsToHomogeneous(pts_in, homogenous_pts);
    // ?
    cv::projectPoints(homogenous_pts, cv::Vec3d::zeros(), cv::Vec3d::zeros(), K,
                      distortion_coeffs, pts_out);
  } else if (distortion_model == "equidistant") {
    cv::fisheye::distortPoints(pts_in, pts_out, K, distortion_coeffs);
  } else {
    ROS_WARN_ONCE("The model %s is unrecognized, using radtan instead...",
                  distortion_model.c_str());
    vector<cv::Point3f> homogenous_pts;
    cv::convertPointsToHomogeneous(pts_in, homogenous_pts);
    cv::projectPoints(homogenous_pts, cv::Vec3d::zeros(), cv::Vec3d::zeros(), K,
                      distortion_coeffs, pts_out);
  }

  return pts_out;
}

/**
 * @brief 计算原图像帧关键点对应的矫正位置
 * @param cam0_R_p_c：图像帧中已校正的关键点位置
 * @param cam1_R_p_c:内参矩阵
 *
 */
void ImageProcessor::integrateImuData(
    Matx33f& cam0_R_p_c, Matx33f& cam1_R_p_c) {
  // Find the start and the end limit within the imu msg buffer.
  // 找到上一时刻和当前时刻的imu对应的时间戳
  auto begin_iter = imu_msg_buffer.begin();
  while (begin_iter != imu_msg_buffer.end()) {
    if ((begin_iter->header.stamp-
          cam0_prev_img_ptr->header.stamp).toSec() < -0.01)
      ++begin_iter;
    else
      break;
  }

  auto end_iter = begin_iter;
  while (end_iter != imu_msg_buffer.end()) {
    if ((end_iter->header.stamp-
          cam0_curr_img_ptr->header.stamp).toSec() < 0.005)
      ++end_iter;
    else
      break;
  }

  // Compute the mean angular velocity in the IMU frame.
  // 计算imu系下的平均角速度
  Vec3f mean_ang_vel(0.0, 0.0, 0.0);
  for (auto iter = begin_iter; iter < end_iter; ++iter)
    mean_ang_vel += Vec3f(iter->angular_velocity.x,
        iter->angular_velocity.y, iter->angular_velocity.z);

  if (end_iter-begin_iter > 0)
    mean_ang_vel *= 1.0f / (end_iter-begin_iter);

  // Transform the mean angular velocity from the IMU
  // frame to the cam0 and cam1 frames.
  // 将平均角速度从imu系转换到图像坐标系
  // t()表示转置
  Vec3f cam0_mean_ang_vel = R_cam0_imu.t() * mean_ang_vel;
  Vec3f cam1_mean_ang_vel = R_cam1_imu.t() * mean_ang_vel;

  // Compute the relative rotation.
  // 通过imu来计算前后两个时刻两帧图像之间的旋转
  double dtime = (cam0_curr_img_ptr->header.stamp-
      cam0_prev_img_ptr->header.stamp).toSec();
  Rodrigues(cam0_mean_ang_vel*dtime, cam0_R_p_c);
  Rodrigues(cam1_mean_ang_vel*dtime, cam1_R_p_c);
  cam0_R_p_c = cam0_R_p_c.t();
  cam1_R_p_c = cam1_R_p_c.t();

  // Delete the useless and used imu messages.
  // 清除已使用过的imu信息
  imu_msg_buffer.erase(imu_msg_buffer.begin(), end_iter);
  return;
}

/**
 * @brief 归一化关键点的坐标，计算得到尺度因子
 * @param pts1：上一时刻的关键点位置
 * @param pts2:当前时刻跟踪匹配到的关键点位置
 * @return scaling_factor：尺度因子
 *
 */
void ImageProcessor::rescalePoints(
    vector<Point2f>& pts1, vector<Point2f>& pts2,
    float& scaling_factor) {

  scaling_factor = 0.0f;

  // 将所有关键点的模长相加
  for (int i = 0; i < pts1.size(); ++i) {
    scaling_factor += sqrt(pts1[i].dot(pts1[i]));
    scaling_factor += sqrt(pts2[i].dot(pts2[i]));
  }

  // 为了采用乘法，这里其实采用的计算方式是倒数
  scaling_factor = (pts1.size()+pts2.size()) /
    scaling_factor * sqrt(2.0f);

  // 关键点的归一化处理
  // pts1 = pts1/（sum(sqrt(pts.dot(pts)))/(pts.size*sqrt(2)）
  for (int i = 0; i < pts1.size(); ++i) {
    pts1[i] *= scaling_factor;
    pts2[i] *= scaling_factor;
  }

  return;
}

/**
 * @brief 计算原图像帧关键点对应的矫正位置
 * @param pts1：上一时刻的关键点位置
 * @param pts2:当前时刻跟踪匹配到的关键点位置
 * @param R_p_c:根据imu信息计算得到的两个时刻相机的相对旋转信息
 * @param distortion_model,intrinsics：相机内参和畸变模型
 * @param inlier_error：内点可接受的阈值（关键点距离差）
 * @param success_probability：成功的概率
 * @return inlier_markers：内点标志位
 *
 */
void ImageProcessor::twoPointRansac(
    const vector<Point2f>& pts1, const vector<Point2f>& pts2,
    const cv::Matx33f& R_p_c, const cv::Vec4d& intrinsics,
    const std::string& distortion_model,
    const cv::Vec4d& distortion_coeffs,
    const double& inlier_error,
    const double& success_probability,
    vector<int>& inlier_markers) {

  // Check the size of input point size.
  if (pts1.size() != pts2.size())
    ROS_ERROR("Sets of different size (%lu and %lu) are used...",
        pts1.size(), pts2.size());

  // 平均焦距 f_a = (fx+fy)/2
  // norm_pixel_unit = 1 / f_a 表示一个像素点的归一化坐标值偏差
  double norm_pixel_unit = 2.0 / (intrinsics[0]+intrinsics[1]);
  int iter_num = static_cast<int>(
      ceil(log(1-success_probability) / log(1-0.7*0.7)));

  // Initially, mark all points as inliers.
  // 对所有的关键点赋予一个判断是否为内点的标志位
  // 初始化的inlier_markers都置为1
  inlier_markers.clear();
  inlier_markers.resize(pts1.size(), 1);

  // Undistort all the points.
  // 对前后时刻所有的关键点进行去畸变操作
  vector<Point2f> pts1_undistorted(pts1.size());
  vector<Point2f> pts2_undistorted(pts2.size());
  undistortPoints(
      pts1, intrinsics, distortion_model,
      distortion_coeffs, pts1_undistorted);
  undistortPoints(
      pts2, intrinsics, distortion_model,
      distortion_coeffs, pts2_undistorted);


  // Compenstate the points in the previous image with
  // the relative rotation.
  // 乘上帧间的旋转使上一时刻与当前时刻的关键点之间只有平移量
  for (auto& pt : pts1_undistorted) {
    Vec3f pt_h(pt.x, pt.y, 1.0f);
    //Vec3f pt_hc = dR * pt_h;
    Vec3f pt_hc = R_p_c * pt_h;
    pt.x = pt_hc[0];
    pt.y = pt_hc[1];
  }



  /*
  以下代码的操作流程：
  对所有关键点进行归一化处理：pts1 × scale， scale = 点的个数 / （所有点模长之和 × sqrt（2））
  对norm_pixel_unit也要进行归一化
  以50像素作为评判外点的阈值： (pts1_un - pts2_un) × scale > 50（像素） × 1/f × scale 
  */  

  // Normalize the points to gain numerical stability.
  // 归一化关键点（去除模长）从而来提高数值稳定性
  float scaling_factor = 0.0f;
  rescalePoints(pts1_undistorted, pts2_undistorted, scaling_factor);
  norm_pixel_unit *= scaling_factor;

  // Compute the difference between previous and current points,
  // which will be used frequently later.
  // 计算前后两帧匹配的关键点的差值
  vector<Point2d> pts_diff(pts1_undistorted.size());
  for (int i = 0; i < pts1_undistorted.size(); ++i)
    pts_diff[i] = pts1_undistorted[i] - pts2_undistorted[i];

  // Mark the point pairs with large difference directly.
  // BTW, the mean distance of the rest of the point pairs
  // are computed.
  // 计算关键点差值的中间值，将差值大于阈值的点对视为外点剔除
  double mean_pt_distance = 0.0;
  int raw_inlier_cntr = 0;
  for (int i = 0; i < pts_diff.size(); ++i) {
    double distance = sqrt(pts_diff[i].dot(pts_diff[i]));
    // 25 pixel distance is a pretty large tolerance for normal motion.
    // However, to be used with aggressive motion, this tolerance should
    // be increased significantly to match the usage.
    // 阈值设为50个像素差
    if (distance > 50.0*norm_pixel_unit) {
      inlier_markers[i] = 0;
    } else {
      mean_pt_distance += distance;
      ++raw_inlier_cntr;
    }
  }
  mean_pt_distance /= raw_inlier_cntr;

  // If the current number of inliers is less than 3, just mark
  // all input as outliers. This case can happen with fast
  // rotation where very few features are tracked.
  if (raw_inlier_cntr < 3) {
    for (auto& marker : inlier_markers) marker = 0;
    return;
  }

  // Before doing 2-point RANSAC, we have to check if the motion
  // is degenerated, meaning that there is no translation between
  // the frames, in which case, the model of the RANSAC does not
  // work. If so, the distance between the matched points will
  // be almost 0.
  // 检查运动是否退化，退化则表示帧间的平移量几乎为0
  // 平移量为0则匹配点对之间的距离几乎为0
  // 平均的差值小于1个像素则认为退化，只需简单比较设置的阈值来判断外点
  //if (mean_pt_distance < inlier_error*norm_pixel_unit) {
  if (mean_pt_distance < norm_pixel_unit) {
    ROS_WARN_THROTTLE(1.0, "Degenerated motion...");
    for (int i = 0; i < pts_diff.size(); ++i) {
      if (inlier_markers[i] == 0) continue;
      // 关键点之间的距离大于阈值时将其视为外点
      if (sqrt(pts_diff[i].dot(pts_diff[i])) >
          inlier_error*norm_pixel_unit)
        inlier_markers[i] = 0;
    }
    return;
  }

  // In the case of general motion, the RANSAC model can be applied.
  // The three column corresponds to tx, ty, and tz respectively.
  //
  MatrixXd coeff_t(pts_diff.size(), 3);
  for (int i = 0; i < pts_diff.size(); ++i) {
    coeff_t(i, 0) = pts_diff[i].y;
    coeff_t(i, 1) = -pts_diff[i].x;
    coeff_t(i, 2) = pts1_undistorted[i].x*pts2_undistorted[i].y -
      pts1_undistorted[i].y*pts2_undistorted[i].x;
  }

  // 找到被认为是内点的匹配关键点对的索引
  vector<int> raw_inlier_idx;
  for (int i = 0; i < inlier_markers.size(); ++i) {
    if (inlier_markers[i] != 0)
      // 将内点位置索引保存
      raw_inlier_idx.push_back(i);
  }

  //
  vector<int> best_inlier_set;
  double best_error = 1e10;
  // ros包，随机数的生成
  random_numbers::RandomNumberGenerator random_gen;

  // 执行两点RANSAC
  for (int iter_idx = 0; iter_idx < iter_num; ++iter_idx) {
    // Randomly select two point pairs.
    // Although this is a weird way of selecting two pairs, but it
    // is able to efficiently avoid selecting repetitive pairs.
    
    // 随机选择两个点对pair_idx1和pair_idx2（索引）

    // 先从保存内点索引的raw_inlier_idx中随机产生一个位置索引
    int pair_idx1 = raw_inlier_idx[random_gen.uniformInteger(
        0, raw_inlier_idx.size()-1)];

    // 随机产生一个索引差值
    int idx_diff = random_gen.uniformInteger(
        1, raw_inlier_idx.size()-1);

    // 产生第二个索引位置
    // 如果索引pair_idx1加上索引差超过最大值，则减去索引最大值
    int pair_idx2 = pair_idx1+idx_diff < raw_inlier_idx.size() ?
      pair_idx1+idx_diff : pair_idx1+idx_diff-raw_inlier_idx.size();

    // Construct the model;
    //
    Vector2d coeff_tx(coeff_t(pair_idx1, 0), coeff_t(pair_idx2, 0));
    Vector2d coeff_ty(coeff_t(pair_idx1, 1), coeff_t(pair_idx2, 1));
    Vector2d coeff_tz(coeff_t(pair_idx1, 2), coeff_t(pair_idx2, 2));
    vector<double> coeff_l1_norm(3);

    // 求一范数（模长）
    coeff_l1_norm[0] = coeff_tx.lpNorm<1>();
    coeff_l1_norm[1] = coeff_ty.lpNorm<1>();
    coeff_l1_norm[2] = coeff_tz.lpNorm<1>();

    //找到最小的一范数
    int base_indicator = min_element(coeff_l1_norm.begin(),
        coeff_l1_norm.end())-coeff_l1_norm.begin();

    Vector3d model(0.0, 0.0, 0.0);
    //最小的一范数是tx方向的，则用y，z构造
    if (base_indicator == 0) {
      Matrix2d A;
      A << coeff_ty, coeff_tz;
      Vector2d solution = A.inverse() * (-coeff_tx);
      model(0) = 1.0;
      model(1) = solution(0);
      model(2) = solution(1);
    //最小的一范数是ty方向的，则用x，z构造
    } else if (base_indicator ==1) {
      Matrix2d A;
      A << coeff_tx, coeff_tz;
      Vector2d solution = A.inverse() * (-coeff_ty);
      model(0) = solution(0);
      model(1) = 1.0;
      model(2) = solution(1);
    //最小的一范数是tz方向的，则用x，y构造
    } else {
      Matrix2d A;
      A << coeff_tx, coeff_ty;
      Vector2d solution = A.inverse() * (-coeff_tz);
      model(0) = solution(0);
      model(1) = solution(1);
      model(2) = 1.0;
    }

    // Find all the inliers among point pairs.
    // 每一行[y1-y2, -(x1-x2), x1y2 - x2y2]与model相乘
    VectorXd error = coeff_t * model;

    vector<int> inlier_set;
    for (int i = 0; i < error.rows(); ++i) {
      if (inlier_markers[i] == 0) continue;
      if (std::abs(error(i)) < inlier_error*norm_pixel_unit)
        inlier_set.push_back(i);
    }

    // If the number of inliers is small, the current
    // model is probably wrong.
    if (inlier_set.size() < 0.2*pts1_undistorted.size())
      continue;

    // Refit the model using all of the possible inliers.
    VectorXd coeff_tx_better(inlier_set.size());
    VectorXd coeff_ty_better(inlier_set.size());
    VectorXd coeff_tz_better(inlier_set.size());
    for (int i = 0; i < inlier_set.size(); ++i) {
      coeff_tx_better(i) = coeff_t(inlier_set[i], 0);
      coeff_ty_better(i) = coeff_t(inlier_set[i], 1);
      coeff_tz_better(i) = coeff_t(inlier_set[i], 2);
    }

    Vector3d model_better(0.0, 0.0, 0.0);
    if (base_indicator == 0) {
      MatrixXd A(inlier_set.size(), 2);
      A << coeff_ty_better, coeff_tz_better;
      Vector2d solution =
          (A.transpose() * A).inverse() * A.transpose() * (-coeff_tx_better);
      model_better(0) = 1.0;
      model_better(1) = solution(0);
      model_better(2) = solution(1);
    } else if (base_indicator ==1) {
      MatrixXd A(inlier_set.size(), 2);
      A << coeff_tx_better, coeff_tz_better;
      Vector2d solution =
          (A.transpose() * A).inverse() * A.transpose() * (-coeff_ty_better);
      model_better(0) = solution(0);
      model_better(1) = 1.0;
      model_better(2) = solution(1);
    } else {
      MatrixXd A(inlier_set.size(), 2);
      A << coeff_tx_better, coeff_ty_better;
      Vector2d solution =
          (A.transpose() * A).inverse() * A.transpose() * (-coeff_tz_better);
      model_better(0) = solution(0);
      model_better(1) = solution(1);
      model_better(2) = 1.0;
    }

    // Compute the error and upate the best model if possible.
    VectorXd new_error = coeff_t * model_better;

    double this_error = 0.0;
    for (const auto& inlier_idx : inlier_set)
      this_error += std::abs(new_error(inlier_idx));
    this_error /= inlier_set.size();

    if (inlier_set.size() > best_inlier_set.size()) {
      best_error = this_error;
      best_inlier_set = inlier_set;
    }
  }

  // Fill in the markers.
  inlier_markers.clear();
  inlier_markers.resize(pts1.size(), 0);
  for (const auto& inlier_idx : best_inlier_set)
    inlier_markers[inlier_idx] = 1;

  //printf("inlier ratio: %lu/%lu\n",
  //    best_inlier_set.size(), inlier_markers.size());

  return;
}

/**
 * @brief 用于显示双目图像和特征点，并发布消息
 *
 */
void ImageProcessor::publish() {

  // Publish features.
  CameraMeasurementPtr feature_msg_ptr(new CameraMeasurement);//在msg中定义了
  feature_msg_ptr->header.stamp = cam0_curr_img_ptr->header.stamp;

  vector<FeatureIDType> curr_ids(0);
  vector<Point2f> curr_cam0_points(0);
  vector<Point2f> curr_cam1_points(0);

  // 对当前图像中的特征点进行读取、位置矫正
  for (const auto& grid_features : (*curr_features_ptr)) {
    for (const auto& feature : grid_features.second) {
      curr_ids.push_back(feature.id);
      curr_cam0_points.push_back(feature.cam0_point);
      curr_cam1_points.push_back(feature.cam1_point);
    }
  }

  vector<Point2f> curr_cam0_points_undistorted(0);
  vector<Point2f> curr_cam1_points_undistorted(0);

  undistortPoints(
      curr_cam0_points, cam0_intrinsics, cam0_distortion_model,
      cam0_distortion_coeffs, curr_cam0_points_undistorted);
  undistortPoints(
      curr_cam1_points, cam1_intrinsics, cam1_distortion_model,
      cam1_distortion_coeffs, curr_cam1_points_undistorted);

  // 特征消息包含特征的位置和id
  for (int i = 0; i < curr_ids.size(); ++i) {
    feature_msg_ptr->features.push_back(FeatureMeasurement());
    feature_msg_ptr->features[i].id = curr_ids[i];
    feature_msg_ptr->features[i].u0 = curr_cam0_points_undistorted[i].x;
    feature_msg_ptr->features[i].v0 = curr_cam0_points_undistorted[i].y;
    feature_msg_ptr->features[i].u1 = curr_cam1_points_undistorted[i].x;
    feature_msg_ptr->features[i].v1 = curr_cam1_points_undistorted[i].y;
  }

  // topic名字为features
  feature_pub.publish(feature_msg_ptr);

  // Publish tracking info.
  // topic名字为tracking_info
      // 包含的信息为图像的头、
  TrackingInfoPtr tracking_info_msg_ptr(new TrackingInfo());
  tracking_info_msg_ptr->header.stamp = cam0_curr_img_ptr->header.stamp;
  tracking_info_msg_ptr->before_tracking = before_tracking;
  tracking_info_msg_ptr->after_tracking = after_tracking;
  tracking_info_msg_ptr->after_matching = after_matching;
  tracking_info_msg_ptr->after_ransac = after_ransac;
  tracking_info_pub.publish(tracking_info_msg_ptr);

  return;
}

void ImageProcessor::drawFeaturesMono() {
  // Colors for different features.
  Scalar tracked(0, 255, 0);
  Scalar new_feature(0, 255, 255);

  static int grid_height =
    cam0_curr_img_ptr->image.rows / processor_config.grid_row;
  static int grid_width =
    cam0_curr_img_ptr->image.cols / processor_config.grid_col;

  // Create an output image.
  int img_height = cam0_curr_img_ptr->image.rows;
  int img_width = cam0_curr_img_ptr->image.cols;
  Mat out_img(img_height, img_width, CV_8UC3);
  cvtColor(cam0_curr_img_ptr->image, out_img, CV_GRAY2RGB);

  // Draw grids on the image.
  for (int i = 1; i < processor_config.grid_row; ++i) {
    Point pt1(0, i*grid_height);
    Point pt2(img_width, i*grid_height);
    line(out_img, pt1, pt2, Scalar(255, 0, 0));
  }
  for (int i = 1; i < processor_config.grid_col; ++i) {
    Point pt1(i*grid_width, 0);
    Point pt2(i*grid_width, img_height);
    line(out_img, pt1, pt2, Scalar(255, 0, 0));
  }

  // Collect features ids in the previous frame.
  vector<FeatureIDType> prev_ids(0);
  for (const auto& grid_features : *prev_features_ptr)
    for (const auto& feature : grid_features.second)
      prev_ids.push_back(feature.id);

  // Collect feature points in the previous frame.
  map<FeatureIDType, Point2f> prev_points;
  for (const auto& grid_features : *prev_features_ptr)
    for (const auto& feature : grid_features.second)
      prev_points[feature.id] = feature.cam0_point;

  // Collect feature points in the current frame.
  map<FeatureIDType, Point2f> curr_points;
  for (const auto& grid_features : *curr_features_ptr)
    for (const auto& feature : grid_features.second)
      curr_points[feature.id] = feature.cam0_point;

  // Draw tracked features.
  for (const auto& id : prev_ids) {
    if (prev_points.find(id) != prev_points.end() &&
        curr_points.find(id) != curr_points.end()) {
      cv::Point2f prev_pt = prev_points[id];
      cv::Point2f curr_pt = curr_points[id];
      circle(out_img, curr_pt, 3, tracked);
      line(out_img, prev_pt, curr_pt, tracked, 1);

      prev_points.erase(id);
      curr_points.erase(id);
    }
  }

  // Draw new features.
  for (const auto& new_curr_point : curr_points) {
    cv::Point2f pt = new_curr_point.second;
    circle(out_img, pt, 3, new_feature, -1);
  }

  imshow("Feature", out_img);
  waitKey(5);
}

/**
 * @brief 用于显示双目图像和特征点，并发布消息
 *
 */
void ImageProcessor::drawFeaturesStereo() {

  // 有订阅的节点
  if(debug_stereo_pub.getNumSubscribers() > 0) {
    // Colors for different features.
    // 不同特征点用不同颜色显示
    Scalar tracked(0, 255, 0);//绿色
    Scalar new_feature(0, 255, 255);//黄色

    static int grid_height =
            cam0_curr_img_ptr->image.rows / processor_config.grid_row;
    static int grid_width =
            cam0_curr_img_ptr->image.cols / processor_config.grid_col;

    // Create an output image.
    // 输出图像out_img，两个图像合并为一个图像
    int img_height = cam0_curr_img_ptr->image.rows;
    int img_width = cam0_curr_img_ptr->image.cols;
    Mat out_img(img_height, img_width * 2, CV_8UC3);
    cvtColor(cam0_curr_img_ptr->image,
             out_img.colRange(0, img_width), CV_GRAY2RGB);
    cvtColor(cam1_curr_img_ptr->image,
             out_img.colRange(img_width, img_width * 2), CV_GRAY2RGB);

    // Draw grids on the image.
    // 在图像上画格子的线
    for (int i = 1; i < processor_config.grid_row; ++i) {
      Point pt1(0, i * grid_height);
      Point pt2(img_width * 2, i * grid_height);
      line(out_img, pt1, pt2, Scalar(255, 0, 0));
    }
    for (int i = 1; i < processor_config.grid_col; ++i) {
      Point pt1(i * grid_width, 0);
      Point pt2(i * grid_width, img_height);
      line(out_img, pt1, pt2, Scalar(255, 0, 0));
    }
    for (int i = 1; i < processor_config.grid_col; ++i) {
      Point pt1(i * grid_width + img_width, 0);
      Point pt2(i * grid_width + img_width, img_height);
      line(out_img, pt1, pt2, Scalar(255, 0, 0));
    }

    // Collect features ids in the previous frame.
    // 将上一时刻的特征点的id保存（第一帧图像没有）
    vector<FeatureIDType> prev_ids(0);
    for (const auto &grid_features : *prev_features_ptr)
      for (const auto &feature : grid_features.second)
        prev_ids.push_back(feature.id);

    // Collect feature points in the previous frame.
    // 将上一时刻的特征点位置保存
    map<FeatureIDType, Point2f> prev_cam0_points;
    map<FeatureIDType, Point2f> prev_cam1_points;
    for (const auto &grid_features : *prev_features_ptr)
      for (const auto &feature : grid_features.second) {
        prev_cam0_points[feature.id] = feature.cam0_point;
        prev_cam1_points[feature.id] = feature.cam1_point;
      }

    // Collect feature points in the current frame.
    // 当前时刻的关键点
    map<FeatureIDType, Point2f> curr_cam0_points;
    map<FeatureIDType, Point2f> curr_cam1_points;
    for (const auto &grid_features : *curr_features_ptr)
      for (const auto &feature : grid_features.second) {
        curr_cam0_points[feature.id] = feature.cam0_point;
        curr_cam1_points[feature.id] = feature.cam1_point;
      }

    // Draw tracked features.
    // 画出跟踪的特征点
    for (const auto &id : prev_ids) {
      if (prev_cam0_points.find(id) != prev_cam0_points.end() &&
          curr_cam0_points.find(id) != curr_cam0_points.end()) {
        cv::Point2f prev_pt0 = prev_cam0_points[id];
        cv::Point2f prev_pt1 = prev_cam1_points[id] + Point2f(img_width, 0.0);
        cv::Point2f curr_pt0 = curr_cam0_points[id];
        cv::Point2f curr_pt1 = curr_cam1_points[id] + Point2f(img_width, 0.0);

        circle(out_img, curr_pt0, 3, tracked, -1);
        circle(out_img, curr_pt1, 3, tracked, -1);
        line(out_img, prev_pt0, curr_pt0, tracked, 1);
        line(out_img, prev_pt1, curr_pt1, tracked, 1);

        prev_cam0_points.erase(id);
        prev_cam1_points.erase(id);
        curr_cam0_points.erase(id);
        curr_cam1_points.erase(id);
      }
    }

    // Draw new features.
    // 画出当前帧提取的特征点
    for (const auto &new_cam0_point : curr_cam0_points) {
      cv::Point2f pt0 = new_cam0_point.second;
      cv::Point2f pt1 = curr_cam1_points[new_cam0_point.first] +
                        Point2f(img_width, 0.0);

      circle(out_img, pt0, 3, new_feature, -1);
      circle(out_img, pt1, 3, new_feature, -1);
    }

    // 将用于显示的图像消息发布
    cv_bridge::CvImage debug_image(cam0_curr_img_ptr->header, "bgr8", out_img);
    debug_stereo_pub.publish(debug_image.toImageMsg());
  }
//    imshow("Feature", out_img);
//    waitKey(5);
  return;
}

void ImageProcessor::updateFeatureLifetime() {
  for (int code = 0; code <
      processor_config.grid_row*processor_config.grid_col; ++code) {
    vector<FeatureMetaData>& features = (*curr_features_ptr)[code];
    for (const auto& feature : features) {
      if (feature_lifetime.find(feature.id) == feature_lifetime.end())
        feature_lifetime[feature.id] = 1;
      else
        ++feature_lifetime[feature.id];
    }
  }

  return;
}

void ImageProcessor::featureLifetimeStatistics() {

  map<int, int> lifetime_statistics;
  for (const auto& data : feature_lifetime) {
    if (lifetime_statistics.find(data.second) ==
        lifetime_statistics.end())
      lifetime_statistics[data.second] = 1;
    else
      ++lifetime_statistics[data.second];
  }

  for (const auto& data : lifetime_statistics)
    cout << data.first << " : " << data.second << endl;

  return;
}

} // end namespace msckf_vio

#include "lidar_motion_detection/motion_detector.h"

#include <math.h>

#include <future>
#include <string>
#include <unordered_map>
#include <vector>

#include <minkindr_conversions/kindr_tf.h>
#include <pcl/segmentation/extract_clusters.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl_ros/impl/transforms.hpp>
#include <pcl_ros/point_cloud.h>
#include <ros/ros.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>
#include <tf2_ros/buffer.h>
#include <tf2_ros/transform_listener.h>
#include <visualization_msgs/Marker.h>

namespace motion_detection {

using Timer = voxblox::timing::Timer;

void MotionDetector::Config::checkParams() const {
  checkParamCond(!global_frame_name.empty(),
                 "'global_frame_name' may not be empty.");
  checkParamGE(num_threads, 1, "num_threads");
  checkParamGT(transform_timeout, 0.f, "transform_timeout");
}

void MotionDetector::Config::setupParamsAndPrinting() {
  setupParam("global_frame_name", &global_frame_name);
  setupParam("sensor_frame_name", &sensor_frame_name);
  setupParam("evaluate", &evaluate);
  setupParam("visualize", &visualize);
  setupParam("num_threads", &num_threads);
  setupParam("occ_counter_to_reset", &occ_counter_to_reset);
  setupParam("transform_timeout", &transform_timeout, "s");
}

MotionDetector::MotionDetector(const ros::NodeHandle& nh,
                               const ros::NodeHandle& nh_private)
    : config_(
          config_utilities::getConfigFromRos<MotionDetector::Config>(nh_private)
              .checkValid()),
      nh_(nh),
      nh_private_(nh_private) {
  LOG(INFO) << "\n" << config_.toString();

  // Setup the voxblox mapper. Overwrite dependent config parts.
  ros::NodeHandle nh_voxblox(nh_private, "voxblox");
  nh_voxblox.setParam("world_frame", config_.global_frame_name);
  tsdf_server_ = std::make_shared<voxblox::TsdfServer>(nh_voxblox, nh_voxblox);
  tsdf_layer_.reset(tsdf_server_->getTsdfMapPtr()->getTsdfLayerPtr());

  // Setup processing.
  preprocessing_ = std::make_shared<Preprocessing>(
      config_utilities::getConfigFromRos<Preprocessing::Config>(
          ros::NodeHandle(nh_private_, "preprocessing")));
  motion_vis_ = std::make_shared<MotionVisualizer>(
      nh_private, &point_classifications_, &current_clusters_,
      tsdf_server_->getTsdfMapPtr());
  ever_free_integrator_ = std::make_shared<EverFreeIntegrator>(
      nh_private, tsdf_server_->getTsdfMapPtr(), sensor_origin);
  gt_handler_ = std::make_shared<GroundTruthHandler>(nh, nh_private);
  evaluator_ = std::make_shared<Evaluator>(nh_private, &point_classifications_,
                                           gt_handler_.get());

  clustering_ =
      std::make_shared<Clustering>(nh_private_, tsdf_server_->getTsdfMapPtr(),
                                   &point_classifications_, &current_clusters_);

  // Cache frequently used constants.
  voxels_per_side_ = tsdf_layer_->voxels_per_side();
  voxels_per_block_ = voxels_per_side_ * voxels_per_side_ * voxels_per_side_;

  // Advertise and subscribe to topics.
  setupRos();
}

void MotionDetector::setupRos() {
  lidar_pcl_sub_ =
      nh_.subscribe("pointcloud", 1, &MotionDetector::pointcloudCallback, this);

  pointcloud_without_detections_pub_ =
      nh_private_.advertise<sensor_msgs::PointCloud2>(
          "pointcloud_without_detections", 1, true);
}

void MotionDetector::pointcloudCallback(
    const sensor_msgs::PointCloud2::Ptr& msg) {
  frame_counter_++;
  Timer detection_timer("motion_detection");

  // Lookup cloud transform T_M_S of sensor (S) to map (M).
  // If different sensor frame is required, update the message.
  Timer tf_lookup_timer("motion_detection/tf_lookup");
  const std::string sensor_frame_name = config_.sensor_frame_name.empty()
                                            ? msg->header.frame_id
                                            : config_.sensor_frame_name;

  std::cout << "Got msg." << std::endl;

  tf::StampedTransform T_M_S;
  if (!lookupTransform(config_.global_frame_name, sensor_frame_name,
                       msg->header.stamp.toSec(), T_M_S)) {
    // Getting transform failed, need to skip.
    return;
  }
  tf_lookup_timer.Stop();

  std::cout << "Found transform." << std::endl;

  // Preprocessing.
  Timer preprocessing_timer("motion_detection/preprocessing");
  CloudInfo cloud_info;
  Cloud cloud;
  preprocessing_->processPointcloud(msg, T_M_S, cloud, cloud_info);
  preprocessing_timer.Stop();

  std::cout << "Processed pointcloud of " << cloud.size() << " points."
            << std::endl;

  point_classifications_ = cloud_info;

  // Voxel-Point-map setup.
  // Defintion of the voxel2point-map: For parallelization we store
  // voxel2point-map as the vector blockwise_voxel2point_map, where each entry
  // of the vector corresponds to an updated block and each entry is a hash map
  // mapping voxelindices to the set of points falling into the voxel. The hash
  // map block2index_hash maps any updated blockindex to its corresponding index
  // in blockwise_voxel_map. The vector occupied_ever_free_voxel_indices stores
  // all currently occupied voxels.
  voxblox::AnyIndexHashMapType<int>::type block2index_hash;
  std::vector<voxblox::HierarchicalIndexIntMap> blockwise_voxel2point_map;
  std::vector<voxblox::VoxelKey> occupied_ever_free_voxel_indices;

  Timer setup_timer("motion_detection/setup");
  setUpVoxel2PointMap(block2index_hash, blockwise_voxel2point_map,
                      occupied_ever_free_voxel_indices, cloud);
  setup_timer.Stop();

  // Clustering.
  Timer clustering_timer("motion_detection/clustering");
  clusteringStep(&block2index_hash, &blockwise_voxel2point_map,
                 occupied_ever_free_voxel_indices, cloud);
  clustering_timer.Stop();

  Timer update_ever_free("motion_detection/update_ever_free");
  everFreeIntegrationStep(cloud);
  update_ever_free.Stop();

  if (config_.evaluate) {
    Timer eval_timer("motion_detection/evaluation");
    ROS_INFO_STREAM(msg->header.stamp.toNSec());
    std::uint64_t tstamp = msg->header.stamp.toNSec();
    evalStep(cloud, tstamp);
    eval_timer.Stop();
  }

  if (config_.visualize) {
    Timer vis_timer("motion_detection/visualizations");
    visualizationStep(msg, cloud);
    vis_timer.Stop();
  }

  int i = 0;
  for (const auto& point : cloud) {
    if (point_classifications_.points.at(i).cluster_level_dynamic) {
      point_classifications_.points.at(i).filtered_out = true;
    }
    i += 1;
  }
  // postprocessPointcloud(msg, &cloud, sensor_origin);

  // Integrate the pointcloud into the voxblox TSDF map.
  Timer tsdf_integration_timer("motion_detection/tsdf_integration");
  voxblox::Transformation T_G_C;
  tf::transformTFToKindr(T_M_S, &T_G_C);
  tsdf_server_->processPointCloudMessageAndInsert(msg, T_G_C, false);
  tsdf_integration_timer.Stop();

  detection_timer.Stop();
}

bool MotionDetector::lookupTransform(const std::string& target_frame,
                                     const std::string& source_frame,
                                     double timestamp,
                                     tf::StampedTransform& result) const {
  ros::Time timestamp_ros(timestamp);

  // Wait for the transform to arrive if required.
  if (!tf_listener_.waitForTransform(
          target_frame, source_frame, timestamp_ros,
          ros::Duration(config_.transform_timeout))) {
    LOG(WARNING) << "Could not get sensor transform within "
                 << config_.transform_timeout << "s time, Skipping pointcloud.";
    return false;
  }

  // Lookup the transform.
  try {
    tf_listener_.lookupTransform(target_frame, source_frame, timestamp_ros,
                                 result);
  } catch (const tf::TransformException& e) {
    LOG(WARNING) << "Could not get sensor transform, skipping pointcloud. "
                 << e.what();
    return false;
  }
  return true;
}

void MotionDetector::postprocessPointcloud(
    const sensor_msgs::PointCloud2::Ptr& msg, Cloud* processed_pcl,
    pcl::PointXYZ& sensor_origin) {
  processed_pcl->clear();
  processed_pcl->header.frame_id = msg->header.frame_id;

  pcl::fromROSMsg(*msg, *processed_pcl);
  // point_classifications_ptr_->points =
  // std::vector<PointInfo>(static_cast<int>(processed_pcl->size()));

  int i = 0;
  for (const auto& point : *processed_pcl) {
    if (point_classifications_.points.at(i).cluster_level_dynamic) {
      point_classifications_.points.at(i).filtered_out = true;
    }
    i += 1;
  }
  pcl_ros::transformPointCloud(config_.global_frame_name, *processed_pcl,
                               *processed_pcl, tf_listener_);
}

void MotionDetector::everFreeIntegrationStep(const Cloud& lidar_points) {
  std::string ever_free_method;
  voxblox::TsdfVoxel* tsdf_voxel;
  voxblox::Block<voxblox::TsdfVoxel>::Ptr tsdf_block;
  voxblox::GlobalIndex global_voxel_index;
  voxblox::VoxelIndex voxel_idx;

  // Recovers the tsdf-updated blocks
  voxblox::BlockIndexList updated_blocks;
  tsdf_layer_->getAllUpdatedBlocks(voxblox::Update::kEsdf, &updated_blocks);

  if (updated_blocks.empty()) {
    ROS_INFO("no updated blocks");
    return;
  }

  Timer RemoveEverFree_timer("motion_detection/RemoveEverFree");

  // Updates Occupancy counter and calls RemoveEverFree if warranted
  const float voxel_size = tsdf_layer_->voxel_size();
  for (auto& block_index : updated_blocks) {
    tsdf_block = tsdf_layer_->getBlockPtrByIndex(block_index);
    for (size_t linear_index = 0; linear_index < voxels_per_block_;
         ++linear_index) {
      tsdf_voxel = &tsdf_block->getVoxelByLinearIndex(linear_index);
      voxel_idx = tsdf_block->computeVoxelIndexFromLinearIndex(linear_index);

      // Updating the Occupancy Counter
      if (tsdf_voxel->distance < 3 * voxel_size / 2 ||
          tsdf_voxel->curr_occupied == frame_counter_) {
        ever_free_integrator_->updateOccupancyCounter(tsdf_voxel,
                                                      frame_counter_);
        tsdf_voxel->last_static = frame_counter_;
      }

      if (tsdf_voxel->curr_occupied < frame_counter_ - 2) {
        tsdf_voxel->moving = false;
      }

      // Call to Remove EverFree if warranted
      if (tsdf_voxel->occ_counter == config_.occ_counter_to_reset) {
        ever_free_integrator_->RemoveEverFree(block_index, voxel_idx,
                                              frame_counter_);
      }
    }
  }

  RemoveEverFree_timer.Stop();

  // Labels tsdf-updated voxels as EverFree that satisfy the criteria. Performed
  // blockwise in parallel.
  std::vector<voxblox::BlockIndex> indices;
  indices.resize(updated_blocks.size());

  for (size_t i = 0; i < indices.size(); ++i) {
    indices[i] = updated_blocks[i];
  }

  IndexGetter<voxblox::BlockIndex> index_getter(indices);
  std::vector<std::future<void>> threads;

  for (int i = 0; i < config_.num_threads; ++i) {
    threads.emplace_back(std::async(std::launch::async, [&]() {
      voxblox::BlockIndex index;
      while (index_getter.getNextIndex(&index)) {
        ever_free_integrator_->MakeEverFree(index, frame_counter_);
      }
    }));
  }

  for (auto& thread : threads) {
    thread.get();
  }
}

// helper function of setUpVoxel2PointMap: builds hash map mapping blocks to set
// of points that fall into block
void MotionDetector::buildBlock2PointMap(
    voxblox::Layer<voxblox::TsdfVoxel>* layer_ptr, const Cloud& all_points,
    voxblox::HierarchicalIndexIntMap& block2points_map) {
  int i = 0;
  voxblox::Point coord;

  for (auto& point : all_points) {
    coord.x() = point.x;
    coord.y() = point.y;
    coord.z() = point.z;

    const voxblox::BlockIndex& blockindex =
        layer_ptr->computeBlockIndexFromCoordinates(coord);

    block2points_map[blockindex].push_back(i);

    i += 1;
  }
}

// helper function of setUpVoxel2PointMap: builds the Voxel2PointMap blockwise
void MotionDetector::BlockwiseBuildVoxel2PointMap(
    const voxblox::BlockIndex blockindex,
    voxblox::HierarchicalIndexIntMap& block2points_map,
    voxblox::HierarchicalIndexIntMap* voxel_map, const Cloud& all_points) {
  const voxblox::AlignedVector<size_t>& pointsInBlock =
      block2points_map[blockindex];
  voxblox::Point coord;

  if (!tsdf_layer_->hasBlock(blockindex)) {
    return;
  }

  voxblox::Block<voxblox::TsdfVoxel>::Ptr tsdf_block =
      tsdf_layer_->getBlockPtrByIndex(blockindex);

  for (int k = 0; k < pointsInBlock.size(); k++) {
    int i = pointsInBlock[k];

    pcl::PointXYZ point = all_points[i];

    coord.x() = point.x;
    coord.y() = point.y;
    coord.z() = point.z;

    voxblox::VoxelIndex voxel_index =
        tsdf_block->computeVoxelIndexFromCoordinates(coord);

    voxblox::TsdfVoxel* tsdf_voxel =
        &tsdf_block->getVoxelByVoxelIndex(voxel_index);

    if (tsdf_voxel == nullptr) {
      continue;
    }

    (*voxel_map)[voxel_index].push_back(i);

    // EverFree detection flag
    if (tsdf_voxel->ever_free) {
      point_classifications_.points.at(i).EverFree_level_dynamic = true;
    }
  }
}

void MotionDetector::setUpVoxel2PointMap(
    voxblox::AnyIndexHashMapType<int>::type& block2index_hash,
    std::vector<voxblox::HierarchicalIndexIntMap>& blockwise_voxel2point_map,
    std::vector<voxblox::VoxelKey>& occupied_ever_free_voxel_indices,
    const Cloud& all_points) {
  voxblox::HierarchicalIndexIntMap block2points_map;

  // ROS_ERROR_STREAM("a");

  // Identifies for any LiDAR point the block it falls in and constructs the
  // hash-map block2points_map mapping each block to the LiDAR points that fall
  // into the block
  buildBlock2PointMap(tsdf_layer_.get(), all_points, block2points_map);

  // ROS_ERROR_STREAM("b");

  std::vector<voxblox::BlockIndex> indices;
  indices.resize(block2points_map.size());
  int i = 0;

  // ROS_ERROR_STREAM("c");

  // Assigns all updated blocks an index in the vector voxel2point-map
  for (auto it : block2points_map) {
    indices[i] = it.first;
    block2index_hash[it.first] = i;
    i += 1;
  }

  // ROS_ERROR_STREAM("d");

  // Builds the voxel2point-map in parallel blockwise
  IndexGetter<voxblox::BlockIndex> index_getter(indices);
  std::vector<std::future<void>> threads;
  (blockwise_voxel2point_map).resize(i);

  // ROS_ERROR_STREAM("f");

  for (int i = 0; i < config_.num_threads; ++i) {
    threads.emplace_back(std::async(std::launch::async, [&]() {
      voxblox::BlockIndex index;
      while (index_getter.getNextIndex(&index)) {
        this->BlockwiseBuildVoxel2PointMap(
            index, block2points_map,
            &(blockwise_voxel2point_map[block2index_hash[index]]), all_points);
      }
    }));
  }

  // ROS_ERROR_STREAM("f");

  for (auto& thread : threads) {
    thread.get();
  }

  // ROS_ERROR_STREAM("g");

  // Updates blockwise the voxel status of the currently occupied voxels
  voxblox::Block<voxblox::TsdfVoxel>::Ptr tsdf_block;
  for (auto it : block2points_map) {
    for (const auto& [voxel_index, pointsInVoxel] :
         (blockwise_voxel2point_map)[block2index_hash[it.first]]) {
      tsdf_block = tsdf_layer_->getBlockPtrByIndex(it.first);

      if (tsdf_block == nullptr) {
        continue;
      }

      if (!(tsdf_block->isValidVoxelIndex(voxel_index))) {
        continue;
      }

      voxblox::TsdfVoxel* tsdf_voxel =
          &tsdf_block->getVoxelByVoxelIndex(voxel_index);

      if (tsdf_voxel == nullptr) {
        continue;
      }

      tsdf_voxel->curr_occupied = frame_counter_;

      // this voxel attribute is used in the voxel clustering method: it
      // signalizes that a currently occupied voxel has not yet been clustered
      tsdf_voxel->clustering_processed = false;

      // the set of occupied_ever_free_voxel_indices allows for fast access of
      // the seed voxels in the voxel clustering
      if (tsdf_voxel->ever_free) {
        occupied_ever_free_voxel_indices.push_back(
            std::make_pair(it.first, voxel_index));
      }
    }
  }
}

void MotionDetector::clusteringStep(
    voxblox::AnyIndexHashMapType<int>::type* block2index_hash,
    std::vector<voxblox::HierarchicalIndexIntMap>* blockwise_voxel2point_map,
    std::vector<voxblox::VoxelKey> occupied_ever_free_voxel_indices,
    const Cloud& all_points) {
  std::vector<pcl::PointIndices> cluster_ind;

  std::vector<std::vector<voxblox::VoxelKey>> voxel_cluster_ind;
  clustering_->VoxelClustering(occupied_ever_free_voxel_indices, frame_counter_,
                               &voxel_cluster_ind);
  clustering_->InducePointClusters(block2index_hash, blockwise_voxel2point_map,
                                   all_points, &voxel_cluster_ind,
                                   &cluster_ind);

  clustering_->applyClusterLevelFilters();
  clustering_->setClusterLevelDynamicFlagOfallPoints();
}

void MotionDetector::evalStep(const Cloud& cloud, const std::uint64_t& tstamp) {
  if (evaluator_->checkGTAvailability(tstamp)) {
    evaluator_->evaluateFrame(cloud, tstamp);
  }
}

void MotionDetector::visualizationStep(const sensor_msgs::PointCloud2::Ptr& msg,
                                       const Cloud& lidar_points) {
  motion_vis_->setAllCloudsToVisualize(lidar_points);
  motion_vis_->publishAll();
}

}  // namespace motion_detection
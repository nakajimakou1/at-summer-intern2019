#include <string.h>
#include <iostream>
#include <ros/ros.h>
#include <ros/console.h>
#include <sensor_msgs/PointCloud2.h>
#include <sensor_msgs/LaserScan.h>
#include <sensor_msgs/Range.h>
#include <pcl_ros/point_cloud.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/filters/conditional_removal.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl_ros/transforms.h> 
#include <laser_geometry/laser_geometry.h>
#include <tf/transform_listener.h>
#include <tf/message_filter.h>
#include <message_filters/subscriber.h>


using namespace std;

typedef pcl::PointCloud<pcl::PointXYZ> PointCloudXYZ;

class LaserObstacleDetection{
    public:
    ros::NodeHandle nh_;
    laser_geometry::LaserProjection projector_;
    ros::Publisher pub_cloud_;
    ros::Publisher pub_range_;
    tf::TransformListener listener_;
    ros::Subscriber sub_laser_;
    sensor_msgs::Range danger_r;

    LaserObstacleDetection(ros::NodeHandle n): nh_(n){
        // ROS subscriber
        sub_laser_ = nh_.subscribe<sensor_msgs::LaserScan>("scan", 1, &LaserObstacleDetection::laserscan_cb, this);
        // ROS publisher
        pub_cloud_ = nh_.advertise<sensor_msgs::PointCloud2> ("cloud", 1);
        pub_range_ = nh_.advertise<sensor_msgs::Range> ("danger_field", 1);
        
        danger_r.header.frame_id = "laser_frame";
        danger_r.radiation_type = sensor_msgs::Range::INFRARED;
        danger_r.field_of_view = 3.1415926;
        danger_r.min_range = 0.1;
        danger_r.max_range = 1.5;
        
    }

    void laserscan_cb(const sensor_msgs::LaserScan::ConstPtr& scan_in){
        sensor_msgs::PointCloud2 cloud_in;
        PointCloudXYZ::Ptr cloud(new PointCloudXYZ);
        PointCloudXYZ::Ptr cloud_filtered(new PointCloudXYZ);
        // projector_.transformLaserScanToPointCloud("base_link", *scan_in, cloud_in, listener_);
        projector_.projectLaser (*scan_in, cloud_in);
        pcl::fromROSMsg(cloud_in, *cloud);

        pcl::ConditionAnd<pcl::PointXYZ>::Ptr range_cond (new pcl::ConditionAnd<pcl::PointXYZ> ());
        range_cond->addComparison (pcl::FieldComparison<pcl::PointXYZ>::ConstPtr (new
            pcl::FieldComparison<pcl::PointXYZ> ("x", pcl::ComparisonOps::GT, -1.5)));
        range_cond->addComparison (pcl::FieldComparison<pcl::PointXYZ>::ConstPtr (new
            pcl::FieldComparison<pcl::PointXYZ> ("x", pcl::ComparisonOps::LT, 0.0)));
        range_cond->addComparison (pcl::FieldComparison<pcl::PointXYZ>::ConstPtr (new
            pcl::FieldComparison<pcl::PointXYZ> ("y", pcl::ComparisonOps::GT, -0.8)));
        range_cond->addComparison (pcl::FieldComparison<pcl::PointXYZ>::ConstPtr (new
            pcl::FieldComparison<pcl::PointXYZ> ("y", pcl::ComparisonOps::LT, 0.8)));
        // build the filter
        pcl::ConditionalRemoval<pcl::PointXYZ> condrem;
        condrem.setCondition (range_cond);
        condrem.setInputCloud (cloud);
        condrem.setKeepOrganized(true);
        // apply filter
        condrem.filter (*cloud_filtered);
            
        tf::TransformListener listener;
        tf::StampedTransform tf1;
        try{
            listener.waitForTransform("base_link", "laser_frame", ros::Time(0), ros::Duration(10.0) );
            listener.lookupTransform("base_link", "laser_frame", ros::Time(0), tf1);
        }
        catch (tf::TransformException ex){
            ROS_ERROR("%s",ex.what());
            ros::Duration(1.0).sleep();
            return;
        }
        
        pcl_ros::transformPointCloud (*cloud_filtered, *cloud_filtered, tf1);
        
        // Publish the data
        pub_cloud_.publish(*cloud_filtered);
        danger_r.header.stamp = ros::Time::now();
        danger_r.range = 1;
        pub_range_.publish(danger_r);
    }
};


int main(int argc, char** argv)
{
    ros::init(argc, argv, "sub_pcl");
    ros::NodeHandle nh;
    LaserObstacleDetection obj(nh);
    ros::spin();
    return 0;
}
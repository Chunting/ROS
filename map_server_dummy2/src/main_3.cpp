#include <ros/ros.h>
#include "ros/console.h"
#include <ros/time.h>
#include <move_base_msgs/MoveBaseAction.h>
#include <actionlib/client/simple_action_client.h>
#include <nav_msgs/Odometry.h>
#include "nav_msgs/GetMap.h"
#include "nav_msgs/MapMetaData.h"
#include <geometry_msgs/PoseArray.h>
#include <geometry_msgs/PoseWithCovarianceStamped.h>
#include <geometry_msgs/Twist.h>
#include "map_server/image_loader.h"
#include "yaml-cpp/yaml.h"
#include <gazebo/ModelStates.h>
#include <gazebo_msgs/ModelState.h>
#include <tf/transform_datatypes.h>
#include <SDL/SDL_image.h>
#include <robot_sim_nlp/JPArray.h>
#include <std_msgs/String.h>

#include <stdio.h>
#include <stdlib.h>
#include <libgen.h>
#include <fstream>
#include <iostream>
#include <cmath>
#define stage_1 1
#define stage_2 2
#define stage_3 3
#define ROTATE 0
#define MOVING 1 
#define RE_ROTATE 2
using namespace std;

double start_time;
double duration;
bool start = false, time_start = false;
bool care_for_ori = false;
int goal_number;
double goal_x, goal_y;
int stage_number, state_;

double rv_ori_x, rv_ori_y, rv_ori_z, rv_ori_w;
double x_map;
double y_map;
double yaw_val;
double res = 0.044875;
int image_height = 300;
bool is_map_1_ = true;
double end_yaw;

actionlib_msgs::GoalID goal_id_;
move_base_msgs::MoveBaseGoal goal;
geometry_msgs::Twist cmd;

geometry_msgs::PoseWithCovarianceStamped map_pose;
nav_msgs::MapMetaData meta_data_message_;
nav_msgs::GetMap::Response map_resp_;
ros::Publisher map_pose_pub_;
ros::Publisher map_pub_;
ros::Publisher metadata_pub_;
ros::Publisher cancel_goal_;
ros::Publisher vel_pub_;
ros::Publisher end_pub_;

void sendMap();
void cancelGoal();

typedef actionlib::SimpleActionClient<move_base_msgs::MoveBaseAction> MoveBaseClient;

void mapCallBack(const robot_sim_nlp::JPArray::ConstPtr& msg){
  int index = 0;

  for(unsigned int y=0; y<map_resp_.map.info.height; y++){
	for(unsigned int x=0; x<map_resp_.map.info.width; x++){
		map_resp_.map.data[index] = msg->data[index];
		index++;					
	}				
  }
  map_resp_.map.info.map_load_time = ros::Time::now();
  map_resp_.map.header.stamp = ros::Time::now();
  ROS_INFO("Read a %d X %d map @ %.3lf m/cell",
           map_resp_.map.info.width,
           map_resp_.map.info.height,
           map_resp_.map.info.resolution);

  // Latched publisher for data
  map_pub_.publish( map_resp_.map );

}

void goalCallBack(const geometry_msgs::Point::ConstPtr& msg){
  MoveBaseClient ac("move_base", true);
  //wait for the action server to come up
  while(!ac.waitForServer(ros::Duration(5.0))){
    ROS_INFO("Waiting for the move_base action server to come up");
  }
  //we'll send a goal to the robot to move 1 meter forward
  goal.target_pose.header.frame_id = "map";
  goal.target_pose.header.stamp = ros::Time::now();
  goal.target_pose.pose.position.x = msg->x*res;
  goal.target_pose.pose.position.y = (image_height-msg->y) * res;
  if (msg->z > -4){
    end_yaw = msg->z;
    goal.target_pose.pose.orientation = tf::createQuaternionMsgFromYaw(msg->z);
    care_for_ori = true;
    stage_number = stage_2;
    start = true;
  }
  else{
    goal.target_pose.pose.orientation.w = 1;
    care_for_ori = false;
    ROS_INFO("Sending goal");
    ac.sendGoal(goal);
  }
  return;

}

void goalCallBack1(const move_base_msgs::MoveBaseActionGoal::ConstPtr& msg){
  ROS_INFO("Got goal");
  goal_id_ = msg->goal_id;
  stage_number = stage_1;
  start = true;
}

void map_statesCallBack(const geometry_msgs::PoseWithCovarianceStamped::ConstPtr& msg){
  x_map = msg->pose.pose.position.x;
  y_map = msg->pose.pose.position.y;
  rv_ori_x = msg->pose.pose.orientation.x;
  rv_ori_y = msg->pose.pose.orientation.y;
	rv_ori_z = msg->pose.pose.orientation.z;
	rv_ori_w = msg->pose.pose.orientation.w;
	tf::Quaternion q;
  double roll, pitch;
  tf::quaternionMsgToTF(msg->pose.pose.orientation, q);
  tf::Matrix3x3(q).getRPY(roll, pitch, yaw_val);
  if (!start)
    return;
  double distance;
  double destination_yaw = atan2((goal.target_pose.pose.position.y-y_map),(goal.target_pose.pose.position.x-x_map));
  cmd.linear.x = cmd.linear.y = cmd.angular.z = 0;
  if (stage_number == stage_1){
    if (pow((x_map - goal.target_pose.pose.position.x), 2) + pow((y_map - goal.target_pose.pose.position.y), 2) < pow (0.3, 2) ){
      cancelGoal();
      stage_number = stage_2;
    }
  }
  else if (stage_number == stage_2){
    switch(state_){
	    case ROTATE:
	      ROS_INFO("rotating");
	      if (!time_start){
	        start_time = ros::Time::now().sec + ros::Time::now().nsec/1000.0;
	        time_start = true;
	        duration = (destination_yaw - yaw_val) / 0.2;
	        if (destination_yaw > yaw_val){
			      cmd.angular.z = 0.2;
		      }
		      else {
			      cmd.angular.z = -0.2;
		      }
	        return;
	      }
	      else{
		      if (ros::Time::now().sec + ros::Time::now().nsec/1000.0 > duration + start_time){
			      state_ = MOVING;
			      time_start = false;
			      ROS_INFO("Go to moving");
			    }
		    }
		    break;
	    case MOVING:
	      ROS_INFO("moving");
	      distance = sqrt(pow(goal.target_pose.pose.position.x - x_map, 2)+ pow(goal.target_pose.pose.position.y - y_map, 2));
		    if (distance < 0.1){
		      if (care_for_ori){
		        state_ = RE_ROTATE;
			      ROS_INFO("Go to rerotate");
			    }
			    else{
			      state_ = ROTATE;
			      stage_number = stage_3;
			      start = false;
			      ROS_INFO("finfished");
			      std_msgs::String str;
            std::stringstream ss;
            ss << "goal_reached";
            str.data = ss.str();
            end_pub_.publish(std_msgs::String(str));
			    }
        }
		    else{
				    cmd.linear.x = 0.1 + 0.4 * distance;
				    if (destination_yaw > yaw_val + 0.01){
					    cmd.angular.z = destination_yaw - yaw_val;
				    }
				    else if (destination_yaw - yaw_val < -0.01){
					    cmd.angular.z = destination_yaw - yaw_val;
				    }
		    }
		    break;
		  case RE_ROTATE:
	      ROS_INFO("re_rotating");
		    if ( end_yaw - yaw_val < 0.01 && end_yaw - yaw_val > -0.01){
		      state_ = ROTATE;
			    stage_number = stage_3;
			    start = false;
			    ROS_INFO("finfished");
			    std_msgs::String str;
          std::stringstream ss;
          ss << "goal_reached";
          str.data = ss.str();
          end_pub_.publish(std_msgs::String(str));
        }
		    else{
				    if (end_yaw > yaw_val + 0.01){
					    cmd.angular.z = 0.15 + 0.5 * (end_yaw - yaw_val);
				    }
				    else if (end_yaw - yaw_val < -0.01){
					    cmd.angular.z = -0.15 + 0.5 * (end_yaw - yaw_val);
				    }
		    }
		    break;
    }
    vel_pub_.publish(cmd);
  }
}

int main(int argc, char** argv)
{
  ros::init(argc, argv, "constant_goal_publisher");
  ros::NodeHandle n;
  ros::Subscriber goal_sub_;
  ros::Subscriber map_sub_;
  ros::Subscriber map_pose_sub_;
  ros::Subscriber world_pose_sub_;
  
  map_sub_ = n.subscribe("sim_costmap", 1, mapCallBack);
  goal_sub_ = n.subscribe("sim_goal", 1, goalCallBack);
  world_pose_sub_ = n.subscribe("/move_base/goal", 1, goalCallBack1);
  map_pose_sub_ = n.subscribe("amcl_pose", 1, map_statesCallBack);
  
  map_pose_pub_ = n.advertise<geometry_msgs::PoseWithCovarianceStamped>("initialpose", 1);
  metadata_pub_ = n.advertise<nav_msgs::MapMetaData>("mymap_metadata", 1, true);
  map_pub_ = n.advertise<nav_msgs::OccupancyGrid>("mymap", 1, true);
  cancel_goal_ = n.advertise<actionlib_msgs::GoalID>("/move_base/cancel", 1, true);
  vel_pub_ = n.advertise<geometry_msgs::Twist>("base_controller/command", 1);
  end_pub_ = n.advertise<std_msgs::String>("goal_request", 1, true);
  stage_number = stage_1;
  start = false;
  is_map_1_ = true;
  state_ = ROTATE;
  sendMap();
  
  ros::spin();
  return 0;
}

void sendMap(){
  std::string mapfname;
  double origin[3];
  int negate;
  double occ_th, free_th;
  
  if (is_map_1_)
    mapfname = "/home/jfasola/fuerte_workspace/sandbox/map_server_dummy2/aptemptyhousemap_1.pgm";
  else
    mapfname = "/home/jfasola/fuerte_workspace/sandbox/map_server_dummy2/apthousemap_1.pgm";
  std::string frame_id;
  ros::NodeHandle private_nh("~");
  private_nh.param("frame_id", frame_id, std::string("map"));
  private_nh.param("negate", negate, 0);
  private_nh.param("occupied_thresh", occ_th, 0.65);
  private_nh.param("free_thresh", free_th, 0.196);
  origin[0] = origin[1] = origin[2] = 0.0;

  ROS_INFO("Loading map from image \"%s\"", mapfname.c_str());
  map_server::loadMapFromFile(&map_resp_,mapfname.c_str(),res,negate,occ_th,free_th, origin);
  map_resp_.map.info.map_load_time = ros::Time::now();
  map_resp_.map.header.frame_id = frame_id;
  map_resp_.map.header.stamp = ros::Time::now();
  ROS_INFO("Read a %d X %d map @ %.3lf m/cell",
           map_resp_.map.info.width,
           map_resp_.map.info.height,
           map_resp_.map.info.resolution);
  
  // Latched publisher for data
  map_pub_.publish( map_resp_.map );
}

void cancelGoal(){
  ROS_INFO("cancel goal");
  cancel_goal_.publish(goal_id_);
}


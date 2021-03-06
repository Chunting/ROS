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
#include <tf/transform_listener.h>
#include <SDL/SDL_image.h>
#include <robot_sim_nlp/JPArray.h>
#include <std_msgs/String.h>

#include <stdio.h>
#include <stdlib.h>
#include <libgen.h>
#include <fstream>
#include <iostream>
#include <cmath>
#define stage_1 1 //waiting for move base
#define stage_2 2 //command the robot to move
#define stage_3 3 //waiting
#define stage_4 4 //back off
#define ROTATE 0
#define MOVING 1 
#define RE_ROTATE 2
using namespace std;

bool pick = true;
bool start = false;
bool care_for_ori = false;
int stage_number, state_;

double roll, pitch, yaw_odom, yaw_tf, destination_yaw;
double x_map;
double y_map;
double yaw_val;
double res = 0.05;
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
ros::Publisher tuck_pub_;

tf::Quaternion q;

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
  //we'll send a goal to the destination_yawrobot to move 1 meter forward
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

void backCallBack(const std_msgs::String::ConstPtr& msg){
  stage_number = stage_4;
  start = true;
  if (msg->data.compare("pick_back_off") == 0)
    pick = true;
  else
    pick = false;
}

void map_statesCallBack(const geometry_msgs::PoseWithCovarianceStamped::ConstPtr& msg){
  map_pose = *msg;
}

int main(int argc, char** argv)
{
  ros::init(argc, argv, "constant_goal_publisher");
  ros::NodeHandle n;
  ros::Subscriber goal_sub_;
  ros::Subscriber map_sub_;
  ros::Subscriber map_pose_sub_;
  ros::Subscriber world_pose_sub_;
  ros::Subscriber back_sub_;
  map_sub_ = n.subscribe("sim_costmap", 1, mapCallBack);
  goal_sub_ = n.subscribe("sim_goal", 1, goalCallBack);
  world_pose_sub_ = n.subscribe("/move_base/goal", 1, goalCallBack1);
  map_pose_sub_ = n.subscribe("robot_pose_ekf/odom_combined", 1, map_statesCallBack);
  back_sub_ = n.subscribe("back_request", 1, backCallBack);
    
  map_pose_pub_ = n.advertise<geometry_msgs::PoseWithCovarianceStamped>("initialpose", 1);
  metadata_pub_ = n.advertise<nav_msgs::MapMetaData>("mymap_metadata", 1, true);
  map_pub_ = n.advertise<nav_msgs::OccupancyGrid>("mymap", 1, true);
  cancel_goal_ = n.advertise<actionlib_msgs::GoalID>("/move_base/cancel", 1, true);
  vel_pub_ = n.advertise<geometry_msgs::Twist>("base_controller/command", 1);
  end_pub_ = n.advertise<std_msgs::String>("goal_request", 1, true);
  tuck_pub_ = n.advertise<std_msgs::String>("pick_place", 1, true);
  
  stage_number = stage_1;
  start = false;
  is_map_1_ = true;
  state_ = ROTATE;
  sendMap();
  tf::TransformListener listener_;
  listener_.waitForTransform("map", "odom_combined", ros::Time(0), ros::Duration(1.0));
  while(ros::ok()){
    ros::spinOnce();
    
    if (!start)
      continue;
    tf::StampedTransform transform;
    listener_.lookupTransform("map", "odom_combined", ros::Time(0), transform);
	  x_map = map_pose.pose.pose.position.x + transform.getOrigin().x();
	  y_map = map_pose.pose.pose.position.y + transform.getOrigin().y();

	  tf::quaternionMsgToTF(map_pose.pose.pose.orientation, q);
    tf::Matrix3x3(q).getRPY(roll, pitch, yaw_odom);
	  yaw_tf = tf::getYaw(transform.getRotation());
	  yaw_val = yaw_tf + yaw_odom;
    destination_yaw = atan2((goal.target_pose.pose.position.y-y_map),(goal.target_pose.pose.position.x-x_map));
    if (yaw_val > 3.1415926)
      yaw_val -= 3.1415926;
    else if (yaw_val < - 3.1415926)
      yaw_val += 3.1415926;
    cmd.linear.x = cmd.linear.y = cmd.angular.z = 0;
    printf("X: %f Y: %f Yaw: %f\n", x_map, y_map, yaw_val);
    double distance;
    if (stage_number == stage_1){
      if (pow((x_map - goal.target_pose.pose.position.x), 2) + pow((y_map - goal.target_pose.pose.position.y), 2) < pow (0.3, 2) ){
        cancelGoal();
        stage_number = stage_2;
      }
    }
    else if (stage_number == stage_2){
      switch(state_){
	      case ROTATE:
		    //  ROS_INFO("rotating");
		      if (destination_yaw - yaw_val < 0.15 && destination_yaw - yaw_val > -0.15){
			      state_ = MOVING;
			      ROS_INFO("Go to moving");
			    }
		      else
			      if (destination_yaw > yaw_val){
				      cmd.angular.z = 0.2 + 0.5 * (destination_yaw - yaw_val);
			      }
			      else {
				      cmd.angular.z = -0.2 + 0.5 * (destination_yaw - yaw_val);
			      }
		      break;
	      case MOVING:
	  //      ROS_INFO("moving");
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
				      if (destination_yaw > yaw_val + 0.15){
					      cmd.angular.z = destination_yaw - yaw_val;
				      }
				      else if (destination_yaw - yaw_val < -0.15){
					      cmd.angular.z = destination_yaw - yaw_val;
				      }
		      }
		      break;
		    case RE_ROTATE:
	  //      ROS_INFO("re_rotating");
		      if ( end_yaw - yaw_val < 0.15 && end_yaw - yaw_val > -0.15){
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
				      if (end_yaw > yaw_val + 0.15){
					      cmd.angular.z = 0.15 + 0.5 * (end_yaw - yaw_val);
				      }
				      else if (end_yaw - yaw_val < -0.15){
					      cmd.angular.z = -0.15 + 0.5 * (end_yaw - yaw_val);
				      }
		      }
		      break;
      }
      vel_pub_.publish(cmd);
    }
    else if (stage_number == stage_4){
      distance = sqrt(pow(goal.target_pose.pose.position.x - x_map, 2)+ pow(goal.target_pose.pose.position.y - y_map, 2));
      if (distance > 0.4){
        stage_number = stage_3;
        start = false;
        ROS_INFO("finfished");
        std_msgs::String str;
        std::stringstream ss;
        if (pick)
          ss << "close_pick";
        else
          ss << "close_drop";
        str.data = ss.str();
        tuck_pub_.publish(std_msgs::String(str));
      }
      else{
	      cmd.linear.x = -0.1 - 0.4 * distance;
      }
      vel_pub_.publish(cmd);
    }
  };
  
  return 0;
}

void sendMap(){
  std::string mapfname;
  double origin[3];
  int negate;
  double occ_th, free_th;
  
  mapfname = "/home/jfasola/fuerte_workspace/sandbox/map_server_dummy2/slammap_rotated2.bmp";
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


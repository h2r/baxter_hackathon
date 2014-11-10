// ROS
#include <ros/ros.h>

#include <boost/thread.hpp>
#include <boost/date_time.hpp>

// MoveIt!
#include <moveit/move_group_interface/move_group.h>

// Baxter Utilities
#include <baxter_control/baxter_utilities.h>
#include "geometry_msgs/PointStamped.h"
#include "geometry_msgs/Vector3Stamped.h"
#include "geometry_msgs/QuaternionStamped.h"
#include "geometry_msgs/TransformStamped.h"
#include "geometry_msgs/PoseStamped.h"
#include "geometry_msgs/PoseWithCovarianceStamped.h"
#include "std_msgs/String.h"
#include <tf/transform_listener.h>
#include <tf/transform_datatypes.h>
#include "object_recognition_msgs/RecognizedObjectArray.h"
#include "baxter_grasps_server/GraspService.h"
#include "ros/time.h"

#include "baxter_pick_and_place/object_scene_interface.h"
 
#include <baxter_core_msgs/EndEffectorState.h>
#include <baxter_core_msgs/EndEffectorCommand.h>
#include <baxter_core_msgs/DigitalIOState.h>

#include "ros/console.h"

#include <iostream>

#include <map>
#include <iterator>
#include <vector>
#include <string>
#include <utility> // header for pair
#include <math.h>

static const double MSG_PULSE_SEC = 0.01;
static const double WAIT_GRIPPER_CLOSE_SEC = 0.5;
static const double WAIT_STATE_MSG_SEC = 1; // max time to wait for the gripper state to refresh
static const double GRIPPER_MSG_RESEND = 5; 
static const ros::Duration WAIT_FOR_OBJECT_TO_APPEAR(30.0);
// XXX oberlin
//static const ros::Duration WAIT_FOR_NEUTRAL_TIMEOUT(10.0);
static const ros::Duration WAIT_FOR_NEUTRAL_TIMEOUT(60.0);
static const double JOINT_POSITION_TOLERANCE = 0.2;

class MarkerPickPlace
{
public:
	baxter_control::BaxterUtilities baxterUtil;

	tf::TransformListener transformer;
	ros::Publisher markersPub;
	ros::Publisher leftGripperPub;
	ros::Publisher statusPublisher;
	ros::Subscriber objectsSub;
	ros::Subscriber jointSubscriber;
	ros::Subscriber gripperSubscriber;
	ros::ServiceClient graspClient;
  int gripperSeq;
	ObjectSceneInterface interface;

	typedef std::map<std::string, geometry_msgs::PoseStamped> PoseMap;
	typedef std::map<std::string, double> JointMap;

	PoseMap objectPoses;
	JointMap jointLookup;
	std::string currentPickObject;

	bool isPicking;
	bool isPlacing;
	bool isGripperOpen;

	ros::Time clearSceneStart;
	bool clearingScene;
  JointMap neutralJoints;
	ros::Time moveToNeutralStart;
	bool isMovingToNeutral;
	boost::thread *pickingAndPlacingThread;
        bool first;

	move_group_interface::MoveGroup *moveGroup;

	tf::TransformListener tranformer;

	MarkerPickPlace()
	{
          gripperSeq = 0;
          first = true;
          this->pickingAndPlacingThread = NULL;
          this->isPicking = false;
          this->isPlacing = false;
          this->clearingScene = false;
          this->isMovingToNeutral = false;
          this->isGripperOpen = false;
          
          
          neutralJoints["left_e0"] = 0.0;
          neutralJoints["left_e1"] = 0.75;
          neutralJoints["left_s0"] = 0.0;
          neutralJoints["left_s1"] = -0.55;
          neutralJoints["left_w0"] = 0.0;
          neutralJoints["left_w1"] = 1.26;
          neutralJoints["left_w2"] = 0.0;
          
          
          
          ros::NodeHandle nh;
          moveGroup = new move_group_interface::MoveGroup("left_arm");
          //moveGroup->setEndEffectorLink("left_gripper");
          //moveGroup->setEndEffector("left_gripper");
          ROS_INFO("Planning Frame: %s", moveGroup->getPlanningFrame().c_str());
          ROS_INFO("End Effector Link: %s", moveGroup->getEndEffectorLink().c_str());
          ROS_INFO("End Effector: %s", moveGroup->getEndEffector().c_str());
          
          markersPub = nh.advertise<visualization_msgs::Marker>("/grasp_markers", 1000);
          leftGripperPub = nh.advertise<baxter_core_msgs::EndEffectorCommand>("/robot/end_effector/left_gripper/command",10);
          graspClient = nh.serviceClient<baxter_grasps_server::GraspService>("/grasp_service");
          statusPublisher = nh.advertise<std_msgs::String>("/pick_place_status", 1000);
          
          moveGroup->setPlanningTime(5.0);
          moveGroup->setSupportSurfaceName("table");
          moveGroup->setWorkspace(0.0, -0.2, -0.30, 0.9, 1.0, 2.0);
          moveGroup->setPlannerId("RRTConnectkConfigDefault");
          
          gripperSubscriber = nh.subscribe("/robot/end_effector/left_gripper/state", 1000, &MarkerPickPlace::gripperCB, this);
          objectsSub = nh.subscribe("/ar_objects", 1000, &MarkerPickPlace::markersCB, this);	
          jointSubscriber = nh.subscribe("/robot/joint_states", 1000, &MarkerPickPlace::jointsCB, this);	
	    
	}

	bool openGripper()
	{
          ROS_INFO("Opening left arm gripper");

          baxter_core_msgs::EndEffectorCommand command;
          command.command = baxter_core_msgs::EndEffectorCommand::CMD_GO;
          command.args = "{\"position\": 100.0}";
          command.id = 65538;
          command.sequence = gripperSeq++;
          leftGripperPub.publish(command);
          ros::Duration(2).sleep();
          
          ROS_INFO("Done opening");
          return true;
	}
  bool initializeGripper()
  {
    ROS_INFO("Initializing gripper");
    
    baxter_core_msgs::EndEffectorCommand command;
    command.command = baxter_core_msgs::EndEffectorCommand::CMD_CONFIGURE;
    command.args = "{\"holding_force\": 30.0, \"velocity\": 50.0, \"dead_zone\": 5.0, \"moving_force\": 40.0}";
    command.id = 65538;
    command.sender = "marker_pick_place";
    command.sequence = gripperSeq++;
    leftGripperPub.publish(command);
    ROS_INFO("Done initializing gripper.");
    
    command.args = "";
    command.command = baxter_core_msgs::EndEffectorCommand::CMD_CALIBRATE;
    command.id = 65538;
    command.sender = "marker_pick_place";
    command.sequence = gripperSeq++;    
    for (int i = 0; i < 50; i++) {
      leftGripperPub.publish(command);
      ros::spinOnce();
    }
    
// id: 65538
// command: calibrate
// args: ''
// sender: /rsdk_gripper_keyboard_calibrate
// sequence: 2


          return true;

  }

  
  bool closeGripper()
  {
    ROS_INFO("Closing left arm gripper");
    
    baxter_core_msgs::EndEffectorCommand command;
    command.command = baxter_core_msgs::EndEffectorCommand::CMD_GO;
    command.args = "{\"position\": 0.0}";
    command.id = 65538;
    command.sequence = gripperSeq++;
    // Send command several times to be safe
    ROS_INFO("publishing gripper");
    leftGripperPub.publish(command);
    ros::Duration(2).sleep();
    return true;
  }
  
  bool isPickingOrPlacing() 
  {
    return isPicking || isPlacing;
  }
  
  void moveToNeutral()
  {
    if (this->isPickingOrPlacing())
      {
        //ROS_INFO("moveToNeutral is picking or placing");
        return;
      }
    if (this->isInNeutral()) 
      {
        //ROS_INFO("is in neutral");
              return;
      }
    ROS_INFO("Moving to neutral.");
    this->isMovingToNeutral = true;
    this->moveToNeutralStart = ros::Time::now();
    moveGroup->setJointValueTarget(neutralJoints);
    
    
    ROS_INFO("Opening gripper");
    //this->openGripper();
    this->moveGroup->detachObject();
    this->moveGroup->asyncMove();
  }
  
  
  bool isInNeutral()
  {
    
    bool isInNeutral = true;
    JointMap::iterator it, endIt;
    for (it = neutralJoints.begin(), endIt = neutralJoints.end(); it != endIt; it++)
      {
        JointMap::iterator findIt = this->jointLookup.find(it->first);

        if (findIt != this->jointLookup.end())
          {
            double joint_error = std::abs(findIt->second - it->second);

            if (joint_error > JOINT_POSITION_TOLERANCE) 
              {
                return false;
              }
          } else {
            ROS_WARN_STREAM("Could not find joint " << it->first);
            return false;
        }
        
      }
    return true;
  }

	bool checkMoveToNeutral()
	{
		if (!this->isMovingToNeutral)
		{
			return true;
		}

		if (ros::Time::now() - this->moveToNeutralStart > WAIT_FOR_NEUTRAL_TIMEOUT)
		{
			ROS_INFO("Moving to neutral took too long. Stopping in hopes things are ok");
			this->moveGroup->stop();
			this->isMovingToNeutral = false;
			return true;
		}
		else
		{

			if (isInNeutral())
			{
				ROS_INFO("Arm is in the neutral position");
				this->isMovingToNeutral = false;
				ROS_INFO("returning");
				return true;
			}
			
		}
		return false;
	}

	void waitForMovingToNeutralToFinish()
	{
		ROS_INFO("wait Neutral");
				
		while(!this->checkMoveToNeutral())
		{
			ros::Duration(0.1).sleep();
		}
	}


	geometry_msgs::Point getRegion(std::string name)
	{
		geometry_msgs::Point point;
		if (name.compare("sink") == 0)
		{
			point.x = 0.6;
			point.y = 0.7;
			point.z = 0.3;
		}
		else if (name.compare("counter") == 0)
		{
			point.x = 0.6;
			point.y = -0.25;
			point.z = 0.3;
		}
		else if (name.compare("robot_counter") == 0)
		{
			point.x = 0.6;
			point.y = 0.0;
			point.z = 0.0;
		}

		return point;
	}

	geometry_msgs::Quaternion getRegionOrientation(std::string name) 
	{
		geometry_msgs::Quaternion orientation;
		if (name.compare("sink") == 0)
		{
			orientation.x = 0.0;
			orientation.y = 1.0;
			orientation.z = 0.0;
			orientation.w = 0.0;
		}
		else if (name.compare("counter") == 0)
		{
			orientation.x = 0.5;
			orientation.y = 0.5;
			orientation.z = -0.5;
			orientation.w = 0.5;
		}
		else if (name.compare("robot_counter") == 0)
		{
			orientation.x = 0.0;
			orientation.y = 1.0;
			orientation.z = 0.0;
			orientation.w = 0.0;
		}
		return orientation;
	}

	geometry_msgs::PoseStamped getPoseStampedFromPoseWithCovariance(geometry_msgs::PoseWithCovarianceStamped pose){
			geometry_msgs::PoseStamped poseStamped;
			poseStamped.header = pose.header;
			poseStamped.pose.position = pose.pose.pose.position;
			poseStamped.pose.orientation = pose.pose.pose.orientation;
			return poseStamped;
	}

	geometry_msgs::Vector3Stamped getDirectionFromPose(geometry_msgs::PoseStamped graspPose, geometry_msgs::Vector3Stamped direction)
	{
		//ROS_INFO_STREAM("Grasp pose " << graspPose);
		tf::Transform graspPoseTransform = getTransformFromPose(graspPose.pose);
		//ROS_INFO_STREAM("Direction " << direction);
		
		tf::Vector3 tfVector;
		tf::vector3MsgToTF(direction.vector, tfVector);
		
		//tfVector = graspPoseTransform * tfVector;

		tfVector = graspPoseTransform.getBasis() * tfVector;

		//ROS_INFO_STREAM("Transformed vector " << tfVector);

		geometry_msgs::Vector3Stamped newDirection;
		newDirection.header = graspPose.header;
		tf::vector3TFToMsg(tfVector, newDirection.vector);
		//ROS_INFO_STREAM("New direction " << newDirection);
		return newDirection;
	}
	tf::Transform getTransformFromPose(geometry_msgs::Pose pose)
	{
		tf::Pose tfPose;
		tf::poseMsgToTF(pose, tfPose);
		tf::Transform transform(tfPose.getRotation(), tfPose.getOrigin());
		return transform;
	}

	geometry_msgs::PoseStamped getPoseFromTransform(tf::Transform transform, std_msgs::Header header)
	{

		geometry_msgs::PoseStamped pose;
		pose.header = header;

		tf::Pose tfPose(transform.getRotation(), transform.getOrigin());
		tf::poseTFToMsg(tfPose, pose.pose);

		return pose;
	}

	geometry_msgs::PoseStamped getGraspPoseRelativeToStampedPose(geometry_msgs::PoseStamped graspPose, geometry_msgs::PoseStamped objectPose)
	{
          visualization_msgs::Marker graspPoseMarker = createGraspMarker(graspPose, "grasp");
          graspPoseMarker.color.g = 0;
          graspPoseMarker.color.b = 0;
          //markersPub.publish(graspPoseMarker);

          visualization_msgs::Marker objectPoseMarker = createGraspMarker(objectPose, "object");
          objectPoseMarker.color.r = 0;
          objectPoseMarker.color.b = 0;
          markersPub.publish(objectPoseMarker);

          
          std::string *error;
          //ROS_INFO_STREAM("graspPose\n" << graspPose);
          //ROS_INFO_STREAM("pose\n" << pose);
          objectPose.header.stamp = ros::Time(0);
          if (objectPose.header.frame_id.compare("base") != 0 || objectPose.header.frame_id.compare("/base") != 0)
            {
              geometry_msgs::PoseStamped newPose;
              transformer.transformPose("base", objectPose, newPose);
              objectPose = newPose;
              ROS_INFO_STREAM("Transformed object pose\n" << objectPose);
            }
          
          tf::Transform graspPoseTransform = getTransformFromPose(graspPose.pose);
          tf::Transform poseTransform = getTransformFromPose(objectPose.pose);
          tf::Transform totalTransform = poseTransform * graspPoseTransform;
          
          geometry_msgs::PoseStamped finalPose = getPoseFromTransform(totalTransform, objectPose.header);
          //finalPose.pose.position.x = objectPose.pose.position.x;
          //finalPose.pose.position.y = objectPose.pose.position.y;
          // tf::Quaternion q;
          // tf::quaternionMsgToTF(objectPose.pose.orientation, q);
          // double pi = -3.1415926535;
          // q.setEuler(-pi, 0, 0);
          // tf::quaternionTFToMsg(q, finalPose.pose.orientation);


          visualization_msgs::Marker finalPoseMarker = createGraspMarker(finalPose, "final");
          finalPoseMarker.color.g = 1;
          finalPoseMarker.color.b = 0;
          markersPub.publish(finalPoseMarker);

          return finalPose;
	}

	std::vector<moveit_msgs::Grasp> setGraspsAtPose(geometry_msgs::PoseStamped pose,std::vector<moveit_msgs::Grasp> grasps)
	{
		ros::Time when;
		std::string *error;
		transformer.getLatestCommonTime("base", "camera_link", when, error);
		std::vector<moveit_msgs::Grasp> newGrasps;
		for (int i = 0; i < grasps.size(); i++)
		{
			moveit_msgs::Grasp grasp = grasps[i];
			grasp.id = i;
			grasp.pre_grasp_posture.header.stamp = when;
			grasp.grasp_posture.header.stamp = when;

			grasp.grasp_pose = getGraspPoseRelativeToStampedPose(grasp.grasp_pose, pose);
			grasp.pre_grasp_approach.direction = getDirectionFromPose(grasp.grasp_pose, grasp.pre_grasp_approach.direction);
			grasp.grasp_quality = 1.0;
			newGrasps.push_back(grasp);
		}
		return newGrasps;
	}

	bool pick(std::string objectName, geometry_msgs::PoseStamped objectPose)
	{
          ROS_INFO("Picking");

          this->interface.setStaticObjects();

          geometry_msgs::PoseStamped worldObjectPose;
          objectPose.header.stamp = ros::Time(0);

          transformer.transformPose("base", objectPose, worldObjectPose);
          objectPose = worldObjectPose;
          ROS_INFO_STREAM("Object\n" << objectPose);

          this->openGripper();
          moveGroup->detachObject();			
		
          baxter_grasps_server::GraspService graspRequest;
          graspRequest.request.name = objectName;
          
          if (!graspClient.call(graspRequest) || !graspRequest.response.success)
            {
              ROS_ERROR_STREAM("No grasps were found for the object " << objectName);
              this->moveToNeutral();
              return false;
            }
          
          moveGroup->allowReplanning(false);
          moveGroup->setPlanningTime(10.0);
          moveGroup->setStartStateToCurrentState();
          moveGroup->setSupportSurfaceName("table");
	  
          std::vector<moveit_msgs::Grasp> grasps = setGraspsAtPose(objectPose, graspRequest.response.grasps);
          ROS_INFO_STREAM("Grasps: " << grasps.size());
          //publishMarkers(grasps, objectName);
          //return false;
          moveGroup->setWorkspace(0.0, -0.2, -0.30, 0.9, 1.0, 2.0);
          
          bool result;
          //bool result = moveGroup->pick(objectName, grasps);
          ROS_INFO("End Effector Link: %s", moveGroup->getEndEffectorLink().c_str());	
          ROS_INFO("Pose Reference Frame: %s", moveGroup->getPoseReferenceFrame().c_str());
          ROS_INFO_STREAM("Grasp pose: " << grasps[0].grasp_pose);
          moveGroup->setPoseReferenceFrame("/base");
          result = moveGroup->setPoseTarget(grasps[0].grasp_pose);
          visualization_msgs::Marker p = createGraspMarker(grasps[0].grasp_pose, "moveit", 15); 
          p.color.r = 0.5;
          p.color.g = 0.5;
          p.color.b = 0.5;
          p.scale.x = 0.2;
          markersPub.publish(p);

          ROS_INFO_STREAM("Transformed pose\n" << grasps[0].grasp_pose);
          if (! result) {
            ROS_ERROR_STREAM("Couldn't set pose to pregrasp.");
          }
          ROS_INFO("Moving to pregrasp");
          this->moveGroup->move();
          //ros::Duration(1).sleep();
          //moveGroup->setStartStateToCurrentState();
          //this->moveGroup->move();

          this->interface.removeAllObjects();
          ros::spinOnce();
          
          moveGroup->setStartStateToCurrentState();
          result = moveGroup->setPoseTarget(grasps[1].grasp_pose);
          if (! result) {
            ROS_ERROR_STREAM("Couldn't set pose to grasp.");
          }
          ROS_INFO("Moving to grasp");
          result = this->moveGroup->move();
          if (! result) {
            ROS_ERROR("Couldn't move to grasp");
          }
          ros::Duration(1).sleep();
          ROS_INFO("Closing gripper");
          this->closeGripper();
          
          moveGroup->setStartStateToCurrentState();
          
          geometry_msgs::PoseStamped uppose = grasps[1].grasp_pose;
          uppose.pose.position.z += 0.3;
          result = moveGroup->setPoseTarget(uppose);
          
          this->interface.setStaticObjects();
          
          if (! result) {
            ROS_ERROR("Couldn't move to grasp");
          }
          
          
          
          ROS_INFO_STREAM("Pick result: " << result);
          return result;
	}

	bool place(std::string objectName, std::vector<geometry_msgs::PoseStamped> placePoses)
	{
          ROS_INFO("Placing");
          bool result;
          this->moveGroup->setStartStateToCurrentState();
          
	  
          moveit::planning_interface::MoveGroup::Plan plan;
          for (int i = 0; i < placePoses.size(); i++)
            {
              ROS_INFO_STREAM("Trying pose " << placePoses[i]);
              this->moveGroup->setPoseTarget(placePoses[i]);
              
              if (this->moveGroup->plan(plan))
                {
                  break;
                }
            }
          result = this->moveGroup->execute(plan);
          isPlacing = false;
          
          moveGroup->detachObject(objectName);			
          this->openGripper();
          //bool result = moveGroup->place(objectName, placePoses);
          
          ROS_INFO("Allowing scene to repopulate");
          this->interface.clearFrozenObjects();
          
          return result;
	}

	std::vector<geometry_msgs::PoseStamped> getValidPlacePoses(geometry_msgs::Point placePoint, geometry_msgs::Quaternion orientation)
	{
		tf::Quaternion startQuat, rotationQuat, outQuat;
		tf::quaternionMsgToTF(orientation, startQuat);
		rotationQuat.setRPY(0,0,0);

		std::vector<geometry_msgs::PoseStamped> placePoses;
		placePoses.resize(36);
		for (int i = 0; i < 36; i++)
		{	
			placePoses[i].header.frame_id = "base";
			placePoses[i].pose.position = placePoint;
			rotationQuat.setRPY(0.0, 0.0, i * 2.0 * M_PI / 36.0);
			tf::quaternionTFToMsg(startQuat * rotationQuat, placePoses[i].pose.orientation );
		}	

		return placePoses;
	}


	void pickAndPlace(std::string objectName)
	{
          std::string name;
          std::getline(std::cin, name);
          ros::spinOnce();
          geometry_msgs::Point placePoint;
          placePoint.x = 0.67;
          placePoint.y = 0.79;
          placePoint.z = -0.02;
          geometry_msgs::Quaternion orientation;
          orientation.x = 0.0;
          orientation.y = 1.0;
          orientation.z = 0.0;
          orientation.w = 0.0;
          geometry_msgs::PoseStamped objectPose = this->objectPoses[objectName];	
          
          ROS_INFO_STREAM("Starting pick and place routine " << objectName);
          
          isPicking = true;
          
          moveGroup->setSupportSurfaceName("table");
          this->interface.freezeAllObjects();
          
          ROS_INFO_STREAM("Finding a valid place pose");
          std::vector<geometry_msgs::PoseStamped> placePoses = getValidPlacePoses(placePoint, orientation);
          
          ROS_INFO_STREAM("Attempting to pick up object " + objectName);
          bool pickSuccess = pick(objectName, objectPose);
          
          ROS_INFO_STREAM("Pick success: " << pickSuccess);
          isPlacing = pickSuccess;
          isPicking = false;
          
          if (!this->isGripperOpen)
            {
              ROS_INFO("Gripper appears to have missed object");
              pickSuccess = false;
            }
          
          
          if (!pickSuccess)
            {
              ROS_ERROR_STREAM("Object pick up failed");
              isPicking = false;
              isPlacing = false;
              
              this->moveToNeutral();
              ros::Duration(2).sleep();
              this->openGripper();
              return;
            }
          
          ROS_INFO("Pick up success. Now placing");
          bool place_result = false;
          place_result = this->place(objectName, placePoses);
          
          if (place_result)
            {
              ROS_INFO("Place success. Preparing for next pickup");
            }
          
          isPicking = false;
          this->moveToNeutral();
	}

	void executeAction()
	{
		std::string objectName = this->currentPickObject;
                
		this->pickingAndPlacingThread = 
			new boost::thread(boost::bind(&MarkerPickPlace::pickAndPlace, this, objectName));
	}

	bool checkScene()
	{
		std::string objectName = this->currentPickObject;
		
		PoseMap::iterator findIt = objectPoses.find(objectName);
		if (findIt == objectPoses.end() && ros::Time::now() - this->clearSceneStart < WAIT_FOR_OBJECT_TO_APPEAR)
		{
			return false;
		}
		if (findIt != objectPoses.end()) 
		{
			ROS_INFO("Found object, stopping execution");
			this->moveGroup->stop();
			this->clearingScene = false;
			return true;
		}
		if (ros::Time::now() - this->clearSceneStart < WAIT_FOR_OBJECT_TO_APPEAR)
		{
			ROS_INFO("Object not found. Abandoning hope");
			this->moveGroup->stop();
			this->clearingScene = false;
			return false;
		}
	}

	void markersCB(const object_recognition_msgs::RecognizedObjectArray::ConstPtr &msg)
	{

          if (first) {
            ROS_INFO("Calling moveToNeutral");
            this->moveToNeutral();
            first = false;
          }
                
          //ROS_INFO_STREAM("Detected " << msg->objects.size() << " objects.");
          objectPoses.clear();	
          for (int i = 0; i < msg->objects.size(); i++)
            {
              geometry_msgs::PoseStamped pose = getPoseStampedFromPoseWithCovariance(msg->objects[i].pose);
              objectPoses[msg->objects[i].type.key] = pose;
            }
          
          
          //ROS_INFO_STREAM(msg);
          
          //this->interface.updateObjects(msg);
          
          //ROS_INFO_STREAM("markers cb: " << this->isMovingToNeutral);
          if (this->isMovingToNeutral)
            {
              return;
            }
          
          if (msg->objects.size() == 0) 
            {
              //ROS_INFO("No objects detected");
              this->moveToNeutral();
              return;
            } 
          
          if (this->pickingAndPlacingThread == NULL || this->pickingAndPlacingThread->timed_join(boost::posix_time::milliseconds(1.0)))
            {
              ROS_INFO("Beginning new pick place thread");
              int rand_index = rand() % msg->objects.size();
              
              std::string objectName = msg->objects[rand_index].type.key;
              this->currentPickObject = objectName;
              this->executeAction();	
            }
	}

	void gripperCB(const baxter_core_msgs::EndEffectorState::ConstPtr &msg)
	{
          this->isGripperOpen = msg->position > 4.0;
	}

	void jointsCB(const sensor_msgs::JointState::ConstPtr &msg)
	{
		for (int i = 0; i < msg->name.size(); i++)
		{
			this->jointLookup[msg->name[i]] = msg->position[i];
		}
		this->checkMoveToNeutral();
	}



  visualization_msgs::Marker createGraspMarker(moveit_msgs::Grasp grasp, std::string markerName, double lifetime = 10.0) 
  {
    return createGraspMarker(grasp.grasp_pose, markerName, lifetime);
  }
  visualization_msgs::Marker createGraspMarker(geometry_msgs::PoseStamped pose, std::string markerName, double lifetime = 10.0)
  {

    // z -90,
    //tf::Transform transform(tfPose.getRotation(), tfPose.getOrigin());
    tf::Quaternion zAxis = tf::createQuaternionFromRPY(0, -3.14159/2.0, 0); 
    tf::Quaternion graspQuat;
    tf::quaternionMsgToTF(pose.pose.orientation, graspQuat);
    tf::Quaternion result = graspQuat * zAxis;
    geometry_msgs::PoseStamped marker_pose = pose;
    
    tf::quaternionTFToMsg(result, marker_pose.pose.orientation);
    visualization_msgs::Marker marker;
    marker.type = 0;
    marker.header = marker_pose.header; //grasp.grasp_pose.header;
    marker.header.stamp = ros::Time::now();
    // This may not be the correct pose
    marker.pose = marker_pose.pose;
    marker.ns = markerName;
    marker.lifetime = ros::Duration(lifetime);
    marker.action = 0;
    marker.color.r = 1;
    marker.color.g = 1;
    marker.color.b = 1;
    marker.color.a = 1;
    marker.scale.x = 0.1;
    marker.scale.y = 0.03;
    marker.scale.z = 0.03;	
    
    return marker;
  }

  void publishMarkers(std::vector<moveit_msgs::Grasp> grasps, std::string object_name)
  {
    for (int i = 0; i < grasps.size(); i++)
      {
        visualization_msgs::Marker marker = createGraspMarker(grasps[i], object_name);
        marker.id = i;
        markersPub.publish(marker);
      }
  }
};


int main(int argc, char**argv) 
{
  ros::init(argc, argv, "baxter_action_server");
  MarkerPickPlace pickPlace;
  pickPlace.initializeGripper();
  ros::spin();
  return 0;	
}

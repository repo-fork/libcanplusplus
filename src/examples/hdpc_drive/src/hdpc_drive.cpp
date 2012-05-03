
#include <math.h>

#include <ros/ros.h>
#include <hdpc_drive/SetControlMode.h>
#include <geometry_msgs/Twist.h>
#include <hdpc_drive/DirectDrive.h>
#include <hdpc_drive/Status.h>

#include <hdpc_com/HDPCConst.h>
#include <hdpc_com/ChangeStateMachine.h>
#include <hdpc_com/Commands.h>
#include <hdpc_com/Readings.h>
#include <hdpc_com/HDPCStateMachineEnums.h>
#include <hdpc_com/HDPCGeometry.h>

using namespace hdpc_drive;
using namespace hdpc_com;

class HDPCDrive {
    protected:
        hdpc_com::HDPCGeometry geom;

        // Transition value between rotation on the spot and ackermann
        double elevation_boundary_rad;
        double max_rotation_speed_rad_per_s;
        double max_linear_speed_m_per_s;
    protected:
        ros::Publisher command_pub;
        ros::Subscriber reading_sub;
        ros::ServiceClient state_machine_client;


        ros::ServiceServer control_mode_serv;
        ros::Subscriber direct_command_sub;
        ros::Subscriber ackermann_command_sub;
        ros::Publisher status_pub;

        unsigned int control_mode;
        static const unsigned int WATCHDOG_INIT = 100;
        unsigned int watchdog;
        hdpc_com::Readings motors;
        hdpc_com::Commands commands;

        void stop_rover() {
            double max_wheel_offset = 0;
            for (unsigned int i=6;i<10;i++) {
                max_wheel_offset = std::max(fabs(motors.position[i]),max_wheel_offset);
            }

            if (max_wheel_offset > 5e-2) {
                // ROS_INFO("STOPPING: Max wheel offset: %02f",max_wheel_offset);
                commands.header.stamp = ros::Time::now();
                for (unsigned int i=0;i<6;i++) {
                    commands.isActive[i] = false;
                    commands.velocity[i] = 0.0;
                }
                for (unsigned int i=6;i<10;i++) {
                    commands.isActive[i] = true;
                    commands.position[i] = 0.0;
                }
            } else if (control_mode != HDPCConst::MODE_STOPPED) {
                hdpc_com::ChangeStateMachine change_state;
                change_state.request.event = EVENT_STOP;
                state_machine_client.call(change_state);

                control_mode = HDPCConst::MODE_STOPPED;
                if (watchdog == 0) {
                    ROS_INFO("HDPC Drive: Rover entered STOPPED mode on watchdog");
                } else {
                    ROS_INFO("HDPC Drive: Rover entered STOPPED mode");
                }
            }
        }

        void init_rotation() {
            double desired[10];
            desired[HDPCConst::STEERING_FRONT_LEFT] = remainder(-atan2(geom.rover_center_to_front,geom.rover_width/2.),M_PI);
            desired[HDPCConst::STEERING_FRONT_RIGHT] = remainder(-atan2(geom.rover_center_to_front,-geom.rover_width/2.),M_PI);
            desired[HDPCConst::STEERING_REAR_LEFT] = remainder(-atan2(-geom.rover_center_to_rear,geom.rover_width/2.),M_PI);
            desired[HDPCConst::STEERING_REAR_RIGHT] = remainder(-atan2(-geom.rover_center_to_rear,-geom.rover_width/2.),M_PI);

            double max_wheel_offset = 0;
            for (unsigned int i=6;i<10;i++) {
                max_wheel_offset = std::max(fabs(remainder(motors.position[i]-desired[i],M_PI)),max_wheel_offset);
            }

            if (max_wheel_offset > 5e-2) {
                // ROS_INFO("INIT ROT: Max wheel offset: %02f",max_wheel_offset);
                commands.header.stamp = ros::Time::now();
                for (unsigned int i=0;i<6;i++) {
                    commands.isActive[i] = false;
                    commands.velocity[i] = 0.0;
                }
                for (unsigned int i=6;i<10;i++) {
                    commands.isActive[i] = true;
                    commands.position[i] = desired[i];
                }
            } else if (control_mode != HDPCConst::MODE_ROTATION) {
                control_mode = HDPCConst::MODE_ROTATION;
                ROS_INFO("HDPC Drive: Rover entered ROTATION mode");
            }
        }

        static inline double SQR(double x) {return x*x;}

        void vel_and_steering(float v_c, float omega_c, unsigned int i_vel, signed int i_steer, float x_w, float y_w) {
            if ((fabs(omega_c) < 1e-2) && (fabs(v_c) < 1e-2)) {
                commands.velocity[i_vel] = 0.0;
                commands.isActive[i_vel] = false;
            } else {
                double phi_w = atan2(x_w*omega_c,(v_c - y_w*omega_c));
                commands.isActive[i_vel] = true;
                commands.velocity[i_vel] = sqrt(SQR(x_w * omega_c) + SQR(v_c - y_w * omega_c)) / geom.rover_wheel_radius;
                if (fabs(phi_w) > M_PI/2) {
                    // If the wheel had to turn more than 90 degrees, then
                    // the other side is used, and the velocity must be
                    // negated
                    phi_w = remainder(phi_w,M_PI);
                    commands.velocity[i_vel] *= -1;
                }
                if (i_steer>=0) {
                    commands.isActive[i_steer] = true;
                    commands.position[i_steer] = phi_w;
                }
            }
        }

        void drive_rover(float velocity, float omega) {
            commands.header.stamp = ros::Time::now();
            if (omega > max_rotation_speed_rad_per_s) {
                omega = max_rotation_speed_rad_per_s;
            }
            if (omega <-max_rotation_speed_rad_per_s) {
                omega = -max_rotation_speed_rad_per_s;
            }
            if (velocity > max_linear_speed_m_per_s) {
                velocity = max_linear_speed_m_per_s;
            }
            if (velocity <-max_linear_speed_m_per_s) {
                velocity = -max_linear_speed_m_per_s;
            }
            double r = velocity / omega;
            switch (control_mode) {
                case HDPCConst::MODE_INIT:
                case HDPCConst::MODE_DIRECT_DRIVE:
                case HDPCConst::MODE_INIT_ROTATION:
                    ROS_WARN("Ignored drive command in current mode");
                    return;
                    break;
                case HDPCConst::MODE_ACKERMANN:
                    if (fabs(velocity) > 1e-2) {
                        if ((r >= 0) && (r <= (geom.rover_width + geom.rover_wheel_width)/2)) {
                            ROS_WARN("Clipped rotation speed in ACKERMANN mode");
                            omega = 2*velocity / (geom.rover_width + geom.rover_wheel_width);
                        }
                        if ((r < 0) && (r >= -(geom.rover_width + geom.rover_wheel_width)/2)) {
                            ROS_WARN("Clipped rotation speed in ACKERMANN mode");
                            omega = -2*velocity / (geom.rover_width + geom.rover_wheel_width);
                        }
                    }
                    break;
                case HDPCConst::MODE_ROTATION:
                    if (fabs(omega) > 1e-2) {

                        if ((r >= 0) && (r >= (geom.rover_width - geom.rover_wheel_width)/2)) {
                            ROS_WARN("Clipped velocity speed in ROTATION mode");
                            velocity = omega * (geom.rover_width - geom.rover_wheel_width)/2;
                        }
                        if ((r < 0) && (r <= -(geom.rover_width - geom.rover_wheel_width)/2)) {
                            ROS_WARN("Clipped velocity speed in ROTATION mode");
                            velocity = -omega * (geom.rover_width - geom.rover_wheel_width)/2;
                        }
                    }
                    break;
                default:
                    return;
            }
            double max_t_steering = 0, delta_steering[10];
            unsigned int i;
            i = HDPCConst::STEERING_FRONT_LEFT;
            vel_and_steering(velocity, omega, HDPCConst::DRIVE_FRONT_LEFT, i, geom.rover_center_to_front, geom.rover_width/2.);
            delta_steering[i] = commands.position[i] - motors.position[i];
            max_t_steering = std::max(max_t_steering,fabs(delta_steering[i])/geom.rover_max_steering_velocity);

            i = HDPCConst::STEERING_FRONT_RIGHT;
            vel_and_steering(velocity, omega, HDPCConst::DRIVE_FRONT_RIGHT, i, geom.rover_center_to_front, -geom.rover_width/2.);
            delta_steering[i] = commands.position[i] - motors.position[i];
            max_t_steering = std::max(max_t_steering,fabs(delta_steering[i])/geom.rover_max_steering_velocity);

            vel_and_steering(velocity, omega, HDPCConst::DRIVE_MIDDLE_LEFT, -1, 0, geom.rover_width/2.);
            vel_and_steering(velocity, omega, HDPCConst::DRIVE_MIDDLE_RIGHT, -1, 0, -geom.rover_width/2.);

            i = HDPCConst::STEERING_REAR_LEFT;
            vel_and_steering(velocity, omega, HDPCConst::DRIVE_REAR_LEFT, i, -geom.rover_center_to_rear, geom.rover_width/2.);
            delta_steering[i] = commands.position[i] - motors.position[i];
            max_t_steering = std::max(max_t_steering,fabs(delta_steering[i])/geom.rover_max_steering_velocity);
            
            i = HDPCConst::STEERING_REAR_RIGHT;
            vel_and_steering(velocity, omega, HDPCConst::DRIVE_REAR_RIGHT, i, -geom.rover_center_to_rear, -geom.rover_width/2.);
            delta_steering[i] = commands.position[i] - motors.position[i];
            max_t_steering = std::max(max_t_steering,fabs(delta_steering[i])/geom.rover_max_steering_velocity);

            commands.velocity[HDPCConst::STEERING_FRONT_LEFT] = delta_steering[HDPCConst::STEERING_FRONT_LEFT]/max_t_steering;
            commands.velocity[HDPCConst::STEERING_FRONT_RIGHT] = delta_steering[HDPCConst::STEERING_FRONT_RIGHT]/max_t_steering;
            commands.velocity[HDPCConst::STEERING_REAR_LEFT] = delta_steering[HDPCConst::STEERING_REAR_LEFT]/max_t_steering;
            commands.velocity[HDPCConst::STEERING_REAR_RIGHT] = delta_steering[HDPCConst::STEERING_REAR_RIGHT]/max_t_steering;
            
        }

        bool set_mode(SetControlMode::Request  &req, SetControlMode::Response &res )
        {
            hdpc_com::ChangeStateMachine change_state;
            change_state.request.event = EVENT_READ_STATE;
            if (!state_machine_client.call(change_state)) {
                ROS_WARN("Change state machine: request state failed");
                return false;
            }
            switch (change_state.response.state) {
                case SM_INIT:
                case SM_STOP:
                    if (req.request_mode == HDPCConst::MODE_STOPPED) {
                        break;
                    }
                    change_state.request.event = EVENT_START;
                    if (!state_machine_client.call(change_state)) {
                        ROS_WARN("Change state machine failed");
                        res.result = false;
                        res.result_mode = control_mode;
                        return false;
                    }
                    break;
                case SM_DRIVE:
                    // Normal. Nothing to do. Stop rover will stop the rover
                    // eventually if required
                    break;
                case SM_FAULT:
                default:
                    ROS_WARN("State machine is in faulty or unknown state. Trying calling RESET");
                    return false;
            }
            // If we reach this state we're good. Now we can have the logic 
            // of the transition
            res.result = true;
            if (control_mode != req.request_mode) {
                switch (req.request_mode) {
                    case HDPCConst::MODE_INIT_ROTATION:
                    case HDPCConst::MODE_ROTATION:
                        if (control_mode != HDPCConst::MODE_STOPPED) {
                            ROS_WARN("Cannot switch to ROTATION from any other mode than STOPPED");
                            res.result = false;
                        } else {
                            control_mode = HDPCConst::MODE_INIT_ROTATION;
                            ROS_INFO("Entering Init Rotation mode");
                        }
                        break;
                    case HDPCConst::MODE_ACKERMANN:
                        if (control_mode != HDPCConst::MODE_STOPPED) {
                            ROS_WARN("Cannot switch to ACKERMANN or ROTATION from any other mode than STOPPED");
                            res.result = false;
                        } else {
                            control_mode = req.request_mode;
                            ROS_INFO("Entering Ackermann mode");
                        }
                        break;
                    case HDPCConst::MODE_DIRECT_DRIVE:
                        control_mode = req.request_mode;
                        ROS_INFO("Entering Direct Drive mode");
                        break;
                    case HDPCConst::MODE_INIT:
                    case HDPCConst::MODE_STOPPED:
                        control_mode = HDPCConst::MODE_INIT;
                        ROS_INFO("Entering INIT mode");
                        break;
                    default:
                        break;
                }
            } else {
                ROS_INFO("Ignoring transition request to the current mode");
            }
            watchdog = WATCHDOG_INIT;
            res.result_mode = control_mode;
            ROS_INFO("Rover set mode successful: %d",control_mode);
            return true;
        }

        void readingsCallback(const hdpc_com::Readings::ConstPtr & msg) {
            Status sts;
            sts.header.frame_id = "rover";
            sts.header.stamp = ros::Time::now();
            sts.control_mode = control_mode;
            motors = sts.motors = *msg;
            status_pub.publish(sts);
        }

        void directDriveCallback(const DirectDrive::ConstPtr& msg)
        {
            if (control_mode != HDPCConst::MODE_DIRECT_DRIVE) {
                ROS_WARN("Ignored direct drive command while not in DIRECT_DRIVE mode");
                return;
            }
            watchdog = WATCHDOG_INIT;
            commands.header.stamp = ros::Time::now();
            for (unsigned int i=0;i<10;i++) {
                commands.isActive[i] = true;
            }
            commands.velocity[HDPCConst::DRIVE_FRONT_LEFT] = msg->velocities_rad_per_sec[DirectDrive::WHEEL_FRONT_LEFT];
            commands.velocity[HDPCConst::DRIVE_FRONT_RIGHT] = msg->velocities_rad_per_sec[DirectDrive::WHEEL_FRONT_RIGHT];
            commands.velocity[HDPCConst::DRIVE_MIDDLE_LEFT] = msg->velocities_rad_per_sec[DirectDrive::WHEEL_MIDDLE_LEFT];
            commands.velocity[HDPCConst::DRIVE_MIDDLE_RIGHT] = msg->velocities_rad_per_sec[DirectDrive::WHEEL_MIDDLE_RIGHT];
            commands.velocity[HDPCConst::DRIVE_REAR_LEFT] = msg->velocities_rad_per_sec[DirectDrive::WHEEL_REAR_LEFT];
            commands.velocity[HDPCConst::DRIVE_REAR_RIGHT] = msg->velocities_rad_per_sec[DirectDrive::WHEEL_REAR_RIGHT];
            commands.velocity[HDPCConst::STEERING_FRONT_LEFT] = 0.0;
            commands.velocity[HDPCConst::STEERING_FRONT_RIGHT] = 0.0;
            commands.velocity[HDPCConst::STEERING_REAR_LEFT] = 0.0;
            commands.velocity[HDPCConst::STEERING_REAR_RIGHT] = 0.0;
            commands.position[HDPCConst::STEERING_FRONT_LEFT] = msg->steering_rad[DirectDrive::WHEEL_FRONT_LEFT];
            commands.position[HDPCConst::STEERING_FRONT_RIGHT] = msg->steering_rad[DirectDrive::WHEEL_FRONT_RIGHT]; 
            commands.position[HDPCConst::STEERING_REAR_LEFT] = msg->steering_rad[DirectDrive::WHEEL_REAR_LEFT]; 
            commands.position[HDPCConst::STEERING_REAR_RIGHT] = msg->steering_rad[DirectDrive::WHEEL_REAR_RIGHT]; 
        }

        void ackermannCallback(const geometry_msgs::Twist::ConstPtr& msg)
        {
            if ((control_mode != HDPCConst::MODE_ACKERMANN) && 
                    (control_mode != HDPCConst::MODE_ROTATION)) {
                ROS_WARN("Ignored Ackermann command while not in Ackermann/Rotation mode");
                return;
            }
            watchdog = WATCHDOG_INIT;
            drive_rover(msg->linear.x, msg->angular.z);
        }

    public:
        HDPCDrive(ros::NodeHandle & nh) : geom(nh) {
            command_pub = nh.advertise<hdpc_com::Commands>("/hdpc_com/commands",1);
            reading_sub = nh.subscribe("/hdpc_com/readings",1,&HDPCDrive::readingsCallback,this);
            state_machine_client = nh.serviceClient<hdpc_com::ChangeStateMachine>("/hdpc_com/changeState");


            control_mode_serv = nh.advertiseService("set_control_mode",&HDPCDrive::set_mode,this);
            direct_command_sub = nh.subscribe("direct",1,&HDPCDrive::directDriveCallback,this);
            ackermann_command_sub = nh.subscribe("ackermann",1,&HDPCDrive::ackermannCallback,this);
            status_pub = nh.advertise<Status>("status",1);

            // TODO: change default value to something that makes sense
            nh.param("max_rotation_speed_rad_per_s",max_rotation_speed_rad_per_s,1.0);
            nh.param("max_linear_speed_m_per_s",max_linear_speed_m_per_s,0.9);

            elevation_boundary_rad = atan(geom.rover_width/2);

            watchdog = 0;
            control_mode = HDPCConst::MODE_INIT;
            commands.header.stamp = ros::Time::now();
            for (unsigned int i=0;i<10;i++) {
                commands.isActive[i] = false;
                commands.velocity[i] = 0;
                commands.position[i] = 0.0;
            }
        }

        bool wait_for_services() {
            state_machine_client.waitForExistence();
            ROS_INFO("State machine service is ready, waiting for init");
            hdpc_com::ChangeStateMachine change_state;
            ros::Rate rate(1);
            do {
                rate.sleep();
                change_state.request.event = EVENT_READ_STATE;
                if (!state_machine_client.call(change_state)) {
                    ROS_WARN("Change state machine: request state failed");
                    return false;
                }
            } while (ros::ok() && (change_state.response.state != SM_STOP));
            ROS_INFO("HDPC Drive: ROVER is ready");

            change_state.request.event = EVENT_START;
            if (!state_machine_client.call(change_state)) {
                ROS_WARN("Change state machine: starting failed");
                return false;
            }

            watchdog = 0;
            control_mode = HDPCConst::MODE_INIT;
            commands.header.stamp = ros::Time::now();
            for (unsigned int i=0;i<10;i++) {
                commands.isActive[i] = false;
                commands.velocity[i] = 0;
                commands.position[i] = 0.0;
            }
            
            return true;
        }
        
        void main_loop() {
            ros::Rate rate(50);
            while (ros::ok()) {
                ros::spinOnce();
                switch (control_mode) {
                    case HDPCConst::MODE_INIT:
                        stop_rover();
                        command_pub.publish(commands);
                        break;
                    case HDPCConst::MODE_INIT_ROTATION:
                        init_rotation();
                        command_pub.publish(commands);
                        break;
                    case HDPCConst::MODE_ROTATION:
                    case HDPCConst::MODE_ACKERMANN:
                    case HDPCConst::MODE_DIRECT_DRIVE:
                        if (watchdog == 0) {
                            control_mode = HDPCConst::MODE_INIT;
                        } else {
                            watchdog -= 1;
                            command_pub.publish(commands);
                        }
                        break;
                    case HDPCConst::MODE_STOPPED:
                    default:
                        break;
                }
                rate.sleep();
            }
        }


};


int main(int argc, char *argv[]) {
    ros::init(argc,argv,"hdpc_drive");
    ros::NodeHandle nh("~");

    HDPCDrive driver(nh);

    driver.wait_for_services();

    driver.main_loop();

    return 0;
}


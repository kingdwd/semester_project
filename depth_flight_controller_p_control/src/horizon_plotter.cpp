#include "horizon_plotter.h"

namespace depth_flight_controller
{
    HorizonPlotter::HorizonPlotter()
            : it_(nh_)
    {
        expanded_image_sub_ = it_.subscribe("/hummingbird/vi_sensor/camera_depth/depth/expanded", 1, &HorizonPlotter::expandedImageCallback, this);

        state_estimate_sub_ = nh_.subscribe("/hummingbird/state_estimate", 1, &HorizonPlotter::stateEstimateCallback, this);

        cmd_vel_base_frame_pub_ =  nh_.advertise<geometry_msgs::TwistStamped>("/hummingbird/base_frame_cmd_vel", 1);
        horizon_points_pub_ =  nh_.advertise<depth_flight_controller_msgs::HorizonPoints>("/hummingbird/horizon_points", 1);

        rotate_left_ = false;
        rotate_right_ = false;
        close_to_wall_ = false;

        horizonPoints = HorizonPlotter::generate3DPoints();
        K = (cv::Mat_<double>(3,3)<<151.8076510090423, 0.0, 80.5, 0.0, 151.8076510090423, 60.5, 0.0, 0.0, 1.0);
        T = (cv::Mat_<double>(3,1) <<  0, 0, 0);
        distCoeffs = (cv::Mat_<double>(4,1) <<  0, 0, 0, 0);
    }


    HorizonPlotter::~HorizonPlotter()
    {

    }


    void HorizonPlotter::toEulerAngle(const Eigen::Quaterniond& q, double& roll, double& pitch, double& yaw)
    {
        // roll (x-axis rotation)
        double sinr = +2.0 * (q.w() * q.x() + q.y() * q.z());
        double cosr = +1.0 - 2.0 * (q.x() * q.x() + q.y() * q.y());
        roll = atan2(sinr, cosr);

        // pitch (y-axis rotation)
        double sinp = +2.0 * (q.w() * q.y() - q.z() * q.x());
        if (fabs(sinp) >= 1)
            pitch = copysign(M_PI / 2, sinp); // use 90 degrees if out of range
        else
            pitch = asin(sinp);

        // yaw (z-axis rotation)
        double siny = +2.0 * (q.w() * q.z() + q.x() * q.y());
        double cosy = +1.0 - 2.0 * (q.y() * q.y() + q.z() * q.z());
        yaw = atan2(siny, cosy);
    }


    Eigen::Matrix3d HorizonPlotter::tiltCalculator(const QuadState &state_estimate)
    {
        Eigen::Quaterniond q = state_estimate.orientation;

        HorizonPlotter::toEulerAngle(q, roll_, pitch_, yaw_);

        Eigen::Vector3d euler_angles(roll_, pitch_, 0);

        Eigen::Matrix3d state_estimate_rot_mat = eulerAnglesZYXToRotationMatrix(euler_angles);

        double a1 = sqrt(pow(state_estimate_rot_mat(0,0),2)+pow(state_estimate_rot_mat(0,1),2)+pow(state_estimate_rot_mat(0,2),2));
        double a2 = sqrt(pow(state_estimate_rot_mat(0,0),2)+pow(state_estimate_rot_mat(1,0),2)+pow(state_estimate_rot_mat(2,0),2));

        return state_estimate_rot_mat;
    }

    void HorizonPlotter::expandedImageCallback(const sensor_msgs::ImageConstPtr& msg)
    {
        ros::Time start = ros::Time::now();

        cv_bridge::CvImagePtr cv_ptr_expanded;

        try
        {
            cv_ptr_expanded = cv_bridge::toCvCopy(msg);

        }
        catch (cv_bridge::Exception& e)
        {
            ROS_ERROR("cv_bridge exception: %s", e.what());
            return;
        }

        depth_expanded_img_ = cv_ptr_expanded->image;

        HorizonPlotter::buildHorizon();

        HorizonPlotter::horizonAnalyze();

        HorizonPlotter::cmdVelPlotter(max_depth_, max_depth_pos_, horizon_center_, state_estimate_);

        HorizonPlotter::plotHorizonPoints();
    }


    void HorizonPlotter::buildHorizon()
    {
        // Calculate edge points of line
        Eigen::Matrix3d rvec_state_estimate = HorizonPlotter::tiltCalculator(state_estimate_);

        cv::Mat rvec = (cv::Mat_<double>(3,3) << rvec_state_estimate(0,0), rvec_state_estimate(0,1), rvec_state_estimate(0,2),
        rvec_state_estimate(1,0), rvec_state_estimate(1,1), rvec_state_estimate(1,2),
        rvec_state_estimate(2,0), rvec_state_estimate(2,1), rvec_state_estimate(2,2));

        cv::Rodrigues(rvec, rvecR);

        cv::projectPoints( cv::Mat(horizonPoints), rvecR, T, K,
        distCoeffs, projectedHorizonPoints);

        cv::Point pt1 = cv::Point(projectedHorizonPoints[0].x,projectedHorizonPoints[0].y);
        cv::Point pt2 = cv::Point(projectedHorizonPoints[1].x,projectedHorizonPoints[1].y);
        horizon_center_ = cv::Point(projectedHorizonPoints[2].x,projectedHorizonPoints[2].y);

        HorizonPlotter::fullLine(pt1, pt2);
    }

// Find depth values without convolution
/*
    void HorizonPlotter::horizonAnalyze()
    {
        cv::LineIterator it(depth_expanded_img_, pt1_, pt2_, 8);
        cv::LineIterator it2 = it;
        std::vector<float> buf(it.count);
        free_space_points.erase(free_space_points.begin(),free_space_points.end());

        // Reset values
        max_depth_ = 0;
        min_depth_ = 5;
        min_depth_ib_ = 5;
        min_dist_center_max_ = 180;
        min_dist_center_min_ = 180;
        bool in_free_space = false;


        // Iterate over line
        for(int i = 0; i < it2.count; i++, ++it2)
        {
            float expanded_depth = depth_expanded_img_.at<float>(it2.pos());
            std::cout << "depth: " << expanded_depth << std::endl;

            // Creat position point
            int x_pos = it2.pos().x;
            int y_pos = it2.pos().y;
            cv::Point pos(x_pos,y_pos);

            // Calc dist to center (e.g. needed change of orientation)
            float dist_center = euclideanDist(pos, horizon_center_);

            if (expanded_depth < min_depth_)
            {
                min_depth_ = expanded_depth;
                min_depth_pos_ = pos;
            }

            if (expanded_depth < 4.75)
            {
                in_free_space = false;
            }

            if (expanded_depth > max_depth_ || (expanded_depth == max_depth_ && dist_center < min_dist_center_max_) || in_free_space == true)
            {
                if (in_free_space == false)
                {
                    free_space_points.erase(free_space_points.begin(),free_space_points.end());
                }

                if (expanded_depth >= 4.75)
                {
                    in_free_space = true;
                    free_space_points.push_back(pos);
                }

                min_dist_center_max_ = dist_center;
                max_depth_ = expanded_depth;
                max_depth_pos_ = pos;
                //std::cout << "max_depth: " << max_depth_ << std::endl;
            }
        }

        int max_left_of_horizon = leftOfSecArg(max_depth_pos_, horizon_center_); // -1 := true ; 0 := is horizon ; 1 := false

        if (max_left_of_horizon == 0)
        {
            min_depth_ib_ = max_depth_;
            min_depth_ib_pos_ = max_depth_pos_;
        } else
        {
            // Iterate over line
            for(int i = 0; i < it2.count; i++, ++it2)
            {
                float expanded_depth = depth_expanded_img_.at<float>(it2.pos());

                // Creat position point
                int x_pos = it2.pos().x;
                int y_pos = it2.pos().y;
                cv::Point pos(x_pos,y_pos);

                // Calc dist to center (e.g. needed change of orientation)
                float dist_center = euclideanDist(pos, horizon_center_);
                int pos_left_of_horizon = leftOfSecArg(pos, horizon_center_);
                int pos_right_of_max = leftOfSecArg(max_depth_pos_, pos);

                if ((pos_right_of_max == -1 && pos_left_of_horizon == -1) || (pos_right_of_max == -1 && pos_left_of_horizon == 0) || (pos_right_of_max == 1 && pos_left_of_horizon == 0) || (pos_right_of_max == 1 && pos_left_of_horizon == 1))
                {
                    if (expanded_depth < min_depth_ib_ || (expanded_depth == min_depth_ib_ && dist_center < min_dist_center_min_))
                    {
                        min_dist_center_min_ = dist_center;
                        min_depth_ib_ = expanded_depth;
                        min_depth_ib_pos_ = pos;
                    }
                }
            }
        }

        int length_free_space = free_space_points.size();
        int take_pos = int(length_free_space/2);
        if (max_depth_ >= 4.75)
        {
            max_depth_pos_ = free_space_points[take_pos];
        }
    } */



    void HorizonPlotter::horizonAnalyze()
    {
        cv::LineIterator it(depth_expanded_img_, pt1_, pt2_, 8);
        cv::LineIterator it2 = it;
        std::vector<float> buf(it.count);
        free_space_points.erase(free_space_points.begin(),free_space_points.end());

        left_edge_pos_ = pt1_;
        right_edge_pos_ = cv::Point(pt2_.x,pt2_.y);

        depth_edge_left_ = depth_expanded_img_.at<float>(left_edge_pos_);
        depth_edge_right_ = depth_expanded_img_.at<float>(right_edge_pos_);
        depth_center_ = depth_expanded_img_.at<float>(horizon_center_);

        // Reset values
        max_depth_ = 0;
        min_depth_left_ = 5.0;
        min_depth_right_ = 5.0;
        min_depth_ib_ = 5.0;
        min_dist_center_max_ = 180;
        max_dist_center_ib_min_ = 180;
        max_dist_center_left_min_ = 180;
        max_dist_center_right_min_ = 180;
        bool in_free_space = false;
        float conv_depth_vec[kernel_size];
        float conv_depth;
        bool iterate = false;
        int iteration = 0;
        conv_center_point_.erase(conv_center_point_.begin(), conv_center_point_.end());

        // Iterate over line
        for(int i = 0; i < it2.count; i++, ++it2)
        {
            float expanded_depth = depth_expanded_img_.at<float>(it2.pos());

            // Creat position point
            int x_pos = it2.pos().x;
            int y_pos = it2.pos().y;
            cv::Point current_pos(x_pos,y_pos);
            cv::Point kernel_pos;
            float kernel_depth;

            if (i == 0)
            {
                for(int j = 0; j < (kernel_size); ++j)
                {
                    conv_depth_vec[j] = expanded_depth;
                    conv_center_point_.push_back(current_pos);
                }
            }

            for(int j = 0; j < (kernel_size-1); ++j)
            {
                conv_depth_vec[j] = conv_depth_vec[j+1];
            }

            conv_depth_vec[(kernel_size-1)] = expanded_depth;
            conv_center_point_.erase(conv_center_point_.begin());
            conv_center_point_.push_back(current_pos);

            if (i >= (kernel_middle-1))
            {
                conv_depth = 0;
                for (int j = 0; j < (kernel_size); ++j)
                {
                    conv_depth = conv_depth + conv_depth_vec[j];
                }
                kernel_pos = conv_center_point_[kernel_middle-1];
                kernel_depth = conv_depth/kernel_size;
            }

            if (i >= (kernel_middle-1))
            {
                do
                {
                    // Calc dist to center (e.g. needed change of orientation)
                    float kernel_dist_center = euclideanDist(kernel_pos, horizon_center_);
                    float current_dist_center = euclideanDist(current_pos, horizon_center_);

                    int side = leftOfSecArg(current_pos, horizon_center_);

                    if (side == -1 && (expanded_depth < min_depth_left_ || (expanded_depth == min_depth_left_ && current_dist_center > max_dist_center_left_min_)))
                    {
                        max_dist_center_left_min_ = current_dist_center;
                        min_depth_left_ = expanded_depth;
                        min_depth_left_pos_ = current_pos;

                    } else if (side == +1 && (expanded_depth < min_depth_right_ || (expanded_depth == min_depth_right_ && current_dist_center > max_dist_center_right_min_)))
                    {
                        max_dist_center_right_min_ = current_dist_center;
                        min_depth_right_ = expanded_depth;
                        min_depth_right_pos_ = current_pos;
                    }


                    if (kernel_depth < 4.5) {
                        in_free_space = false;
                    }

                    if (kernel_depth > max_depth_ || (kernel_depth == max_depth_ && kernel_dist_center < min_dist_center_max_) || in_free_space == true)
                    {
                        if (in_free_space == false) {
                            free_space_points.erase(free_space_points.begin(), free_space_points.end());
                        }

                        if (kernel_depth >= 4.5) {
                            in_free_space = true;
                            free_space_points.push_back(kernel_pos);
                        }
                        min_dist_center_max_ = kernel_dist_center;
                        max_depth_ = kernel_depth;
                        max_depth_pos_ = kernel_pos;
                    }

                    if (i == it2.count-1)
                    {
                        ++iteration;
                        //std::cout << "end iteration: " << iteration << std::endl;
                        iterate = true;
                        if (iteration == kernel_middle) iterate = false;
                        for(int j = 0; j < (kernel_size-1); ++j)
                        {
                            conv_depth_vec[j] = conv_depth_vec[j+1];
                        }

                        conv_depth_vec[kernel_size-1] = conv_depth_vec[kernel_size-2];
                        conv_center_point_.erase(conv_center_point_.begin());
                        conv_center_point_.push_back(conv_center_point_.back());

                        conv_depth = 0;
                        for (int j = 0; j < (kernel_size); ++j)
                        {
                            conv_depth = conv_depth + conv_depth_vec[j];
                        }
                        kernel_pos = conv_center_point_[kernel_size/2+1];
                        kernel_depth = conv_depth/kernel_size;
                    }
                } while(iterate == true);
            }
        }


        int max_left_of_horizon = leftOfSecArg(max_depth_pos_, horizon_center_); // -1 := true ; 0 := is horizon ; 1 := false

        if (max_left_of_horizon == 0)
        {
            min_depth_ib_ = max_depth_;
            min_depth_ib_pos_ = max_depth_pos_;
        } else
        {
            // Iterate over line
            for(int i = 0; i < it2.count; i++, ++it2)
            {
                float expanded_depth = depth_expanded_img_.at<float>(it2.pos());

                // Creat position point
                int x_pos = it2.pos().x;
                int y_pos = it2.pos().y;
                cv::Point pos(x_pos,y_pos);

                // Calc dist to center (e.g. needed change of orientation)
                float dist_center = euclideanDist(pos, horizon_center_);
                int pos_left_of_horizon = leftOfSecArg(pos, horizon_center_);
                int pos_right_of_max = leftOfSecArg(max_depth_pos_, pos);

                if ((pos_right_of_max == -1 && pos_left_of_horizon == -1) || (pos_right_of_max == -1 && pos_left_of_horizon == 0) || (pos_right_of_max == 1 && pos_left_of_horizon == 0) || (pos_right_of_max == 1 && pos_left_of_horizon == 1) || (pos_right_of_max == 0 && pos_left_of_horizon == 0))
                {
                    if (expanded_depth < min_depth_ib_ || (expanded_depth == min_depth_ib_ && dist_center > max_dist_center_ib_min_))
                    {
                        max_dist_center_ib_min_ = dist_center;
                        min_depth_ib_ = expanded_depth;
                        min_depth_ib_pos_ = pos;
                    }
                }
            }
        }

        if (max_depth_ >= 4.5)
        {
            int length_free_space = free_space_points.size();
            int take_pos = int(depth_edge_right_/(depth_edge_left_+ depth_edge_right_)*length_free_space);
            max_depth_pos_ = free_space_points[take_pos];
        }

        if (max_depth_ > 1.0)
        {
            target_ = max_depth_pos_;
            obstacle_ = min_depth_ib_pos_;
            obstacle_depth_ = min_depth_ib_;
        } else if (depth_edge_left_ >= depth_edge_right_)
        {
            target_ = left_edge_pos_;
            obstacle_ = min_depth_left_pos_;
            obstacle_depth_ = min_depth_left_;
        } else
        {
            target_ = right_edge_pos_;
            obstacle_ = min_depth_right_pos_;
            obstacle_depth_ = min_depth_right_;
        }
    }

    void HorizonPlotter::plotHorizonPoints()
    {
        Eigen::Vector3d eig_left(pt1_.x,pt1_.y,0);
        Eigen::Vector3d eig_right(pt2_.x-1,pt2_.y,0);

        Eigen::Vector3d eig_center(horizon_center_.x,horizon_center_.y,0);
        Eigen::Vector3d eig_max_depth(max_depth_pos_.x, max_depth_pos_.y,0);
        Eigen::Vector3d eig_min_depth_left(min_depth_left_pos_.x, min_depth_left_pos_.y,0);
        Eigen::Vector3d eig_min_depth_right(min_depth_right_pos_.x, min_depth_right_pos_.y,0);
        Eigen::Vector3d eig_min_depth_ib(min_depth_ib_pos_.x, min_depth_ib_pos_.y,0);

        hps.pt_left = eigenToGeometry(eig_left);
        hps.pt_right = eigenToGeometry(eig_right);
        hps.pt_center = eigenToGeometry(eig_center);
        hps.pt_max_depth = eigenToGeometry(eig_max_depth);
        hps.pt_min_depth_left = eigenToGeometry(eig_min_depth_left);
        hps.pt_min_depth_right = eigenToGeometry(eig_min_depth_right);
        hps.pt_min_depth_ib = eigenToGeometry(eig_min_depth_ib);

        horizon_points_pub_.publish(hps);
    }

    int HorizonPlotter::leftOfSecArg(cv::Point& p, cv::Point& q)
    {
        if (p.x < q.x)
        {
            return -1;
        } else if (p.x > q.x)
        {
            return 1;
        } else
        {
            return 0;
        }
    }


    float HorizonPlotter::euclideanDist(cv::Point& p, cv::Point& q)
    {
        cv::Point diff = p - q;
        return cv::sqrt(diff.x*diff.x + diff.y*diff.y);
    }


    float HorizonPlotter::euclideanDistSign(cv::Point& p, cv::Point& q)
    {
        cv::Point diff = p - q;
        return copysign(cv::sqrt(diff.x*diff.x + diff.y*diff.y),(p.x-q.x));
    }


    double HorizonPlotter::Slope(int x0, int y0, int x1, int y1)
    {
        return (double)(y1-y0)/(x1-x0);
    }


    void HorizonPlotter::fullLine(cv::Point a, cv::Point b)
    {
        double slope = Slope(a.x, a.y, b.x, b.y);

        pt1_ = cv::Point(0,0), pt2_= cv::Point(depth_expanded_img_.cols-1,depth_expanded_img_.rows-1);

        pt1_.y = -(a.x - pt1_.x) * slope + a.y;
        pt2_.y = -(b.x - pt2_.x) * slope + b.y;
    }

    void HorizonPlotter::stateEstimateCallback(const quad_msgs::QuadStateEstimate::ConstPtr &msg) {
        state_estimate_ = QuadState(*msg);
    }

// Alternative option for drone velocity control
/*
    void HorizonPlotter::cmdVelPlotter()
    {
        acc_ = 0.5;

        geometry_msgs::TwistStamped msg_cmd;
        msg_cmd.header.stamp = ros::Time::now();

        float atan_input = euclideanDistSign(target_, horizon_center_)/151.81;

        float e_alpha_target = atan(atan_input);

        float e_alpha_obstacle = atan(euclideanDist(obstacle_, horizon_center_)/151.81);

        float ang_vel = -1 * acc_*e_alpha_target;

        int lin_input = acc_*int((obstacle_depth_ - e_alpha_obstacle)*10);
        float lin_vel = acc_*float( std::min(20 , std::max(0,lin_input) ) )/10;

        std::cout << "e_alpha_target: " << e_alpha_target << std::endl;
        std::cout << "ang: " << ang_vel << "; lin: " << lin_vel << std::endl;

        msg_cmd.twist.angular.z = ang_vel;
        msg_cmd.twist.linear.x = lin_vel;

        cmd_vel_base_frame_pub_.publish(msg_cmd);
    }
*/

    void HorizonPlotter::cmdVelPlotter(const int &max_depth, const cv::Point &max_depth_pos, cv::Point &horizon_center_pos, const QuadState &state_estimate)
    {
        int max_depth_pos_x = max_depth_pos.x;
        int horizon_center_pos_x = horizon_center_pos.x;
        int dist_center_x =  max_depth_pos_x - horizon_center_pos_x;
        geometry_msgs::TwistStamped msg_cmd;
        msg_cmd.header.stamp = ros::Time::now();

        acc_ = 2; // Acceleration



        if (depth_edge_left_ < 0.5 && rotate_left_ == false && rotate_right_ == false )
        {
            msg_cmd.twist.linear.x = acc_*double(std::min(5, int(max_depth*2)))/10;
            msg_cmd.twist.angular.z = -0.3;
        } else if (depth_edge_right_ < 0.5 && rotate_left_ == false && rotate_right_ == false )
        {
            msg_cmd.twist.linear.x = acc_*double(std::min(5, int(max_depth*2)))/10;
            msg_cmd.twist.angular.z = 0.3;
        } else
        {
            if (rotate_left_ == false && rotate_right_ == false)
            {
                if (depth_center_<= 0.6) {
                    if (state_estimate.bodyrates[2] < 0)
                    {
                        rotate_right_ = true;
                        rotate_left_ = false;
                    } else
                    {
                        rotate_left_ = true;
                        rotate_right_ = false;
                    }
                } else
                {
                    double lin_vel = acc_*double(std::min(5, int(depth_center_*2)))/10;
                    double ang_vel = -acc_*double(std::min(5, dist_center_x/30))/10;
                    msg_cmd.twist.linear.x = lin_vel;
                    msg_cmd.twist.angular.z = ang_vel;
                }
            } else if (rotate_left_ = true)
            {
                if (depth_center_ > 0.8) //&& max_depth_loc_x < 30
                {
                    rotate_left_ = false;
                    rotate_right_ = false;
                } else
                {
                    msg_cmd.twist.linear.x = 0.0;
                    msg_cmd.twist.angular.z = acc_*0.3;
                }

            } else if (rotate_right_ = true)
            {
                if (depth_center_ > 0.8) //&& max_depth_loc_x > 130
                {
                    rotate_right_ = false;
                    rotate_right_ = false;
                } else
                {
                    msg_cmd.twist.linear.x = 0.0;
                    msg_cmd.twist.angular.z = -acc_*0.3;
                }
            }
        }

        cmd_vel_base_frame_pub_.publish(msg_cmd);
    }


    std::vector<cv::Point3f> HorizonPlotter::generate3DPoints()
    {
        std::vector<cv::Point3f> points;

        double x,y,z;

        x=10;y=0;z=100;
        points.push_back(cv::Point3d(x,y,z));

        x=-10;y=0;z=100;
        points.push_back(cv::Point3d(x,y,z));

        x=0;y=0;z=100;
        points.push_back(cv::Point3d(x,y,z));

        return points;
    }
}

int main(int argc, char** argv)
{
    ros::init(argc, argv, "horizon_plotter");

    depth_flight_controller::HorizonPlotter hp;
    ros::spin();

    return 0;
}

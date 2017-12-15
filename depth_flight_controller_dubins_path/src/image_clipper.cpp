//
// Created by nilsiism on 13.11.17.
//

#include "image_clipper.h"


namespace depth_flight_controller {

    ImageClipper::ImageClipper()
            : it_(nh_)
    {
        image_sub_ = it_.subscribe("/hummingbird/vi_sensor/camera_depth/depth/disparity", 1,
                                   &ImageClipper::imageCallback, this);
        image_pub_ = it_.advertise("/hummingbird/vi_sensor/camera_depth/depth/clipped", 1);
    }


    ImageClipper::~ImageClipper()
    {
    }

    void ImageClipper::imageCallback(const sensor_msgs::ImageConstPtr& msg)
    {
        cv_bridge::CvImagePtr cv_ptr_clipped;

        try
        {
            cv_ptr_clipped = cv_bridge::toCvCopy(msg);
        }
        catch (cv_bridge::Exception& e)
        {
            ROS_ERROR("cv_bridge exception: %s", e.what());
            return;
        }

        depth_float_img_clipped_ = cv_ptr_clipped->image;
        cv::Mat mask = cv::Mat(depth_float_img_clipped_ != depth_float_img_clipped_);
        depth_float_img_clipped_.setTo(4.99, mask);

        //cv::Mat mask2 = depth_float_img_original == 0;
        //cv::Mat mask3 = depth_float_img_original > 4;

        //std::cout << depth_float_img_clipped_.at<float>(10,10) << std::endl;

        //cv::GaussianBlur(depth_float_img_clipped_, depth_float_img_clipped_, cv::Size( 3, 3), 0, 0 );

        image_pub_.publish(cv_ptr_clipped->toImageMsg());
    }
}

int main(int argc, char** argv)
{
    ros::init(argc, argv, "image_clipper");

    depth_flight_controller::ImageClipper ic;

    ros::spin();

    return 0;
}




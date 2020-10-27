#include "ceres/ceres.h"
#include "ceres/types.h"
#include <camodocal/calib/HandEyeCalibration.h>
#include <eigen3/Eigen/Geometry>
#include <opencv2/core/eigen.hpp>
#include <ros/ros.h>
#include <termios.h>
#include <tf/transform_listener.h>
#include <tf_conversions/tf_eigen.h>

typedef std::vector<Eigen::Vector3d, Eigen::aligned_allocator<Eigen::Vector3d>>
    eigenVector;

typedef std::vector<Eigen::Affine3d, Eigen::aligned_allocator<Eigen::Affine3d>>
    EigenAffineVector;

///////////////////////////////////////////////////////
// DEFINING GLOBAL VARIABLES

std::string cameraTFname, ARTagTFname;
std::string EETFname, baseTFname;
tf::TransformListener *listener;
Eigen::Affine3d firstEEInverse, firstCamInverse;
bool firstTransform = true;
eigenVector tvecsArm, rvecsArm, tvecsFiducial, rvecsFiducial;

EigenAffineVector baseToTip, cameraToTag;

template <typename Input>
Eigen::Vector3d eigenRotToEigenVector3dAngleAxis(Input eigenQuat)
{
    Eigen::AngleAxisd ax3d(eigenQuat);
    return ax3d.angle() * ax3d.axis();
}

/// @return 0 on success, otherwise error code
int writeTransformPairsToFile(const EigenAffineVector &t1,
                              const EigenAffineVector &t2,
                              std::string filename)
{
    std::cerr << "Writing pairs to \"" << filename << "\"...\n";
    cv::FileStorage fs(filename, cv::FileStorage::WRITE);
    int frameCount = t1.size();
    fs << "frameCount" << frameCount;
    auto t1_it = t1.begin();
    auto t2_it = t2.begin();

    if (fs.isOpened())
    {

        for (int i = 0; i < t1.size(); ++i, ++t1_it, ++t2_it)
        {
            cv::Mat_<double> t1cv = cv::Mat_<double>::ones(4, 4);
            cv::eigen2cv(t1_it->matrix(), t1cv);
            cv::Mat_<double> t2cv = cv::Mat_<double>::ones(4, 4);
            cv::eigen2cv(t2_it->matrix(), t2cv);

            std::stringstream ss1;
            ss1 << "T1_" << i;
            fs << ss1.str() << t1cv;

            std::stringstream ss2;
            ss2 << "T2_" << i;
            fs << ss2.str() << t2cv;
        }
        fs.release();
    }
    else
    {
        std::cerr << "failed to open output file " << filename << "\n";
        return 1;
    }
    return 0;
}

/// @return 0 on success, otherwise error code
int readTransformPairsFromFile(std::string filename, EigenAffineVector &t1,
                               EigenAffineVector &t2)
{
    cv::FileStorage fs(filename, cv::FileStorage::READ);
    int frameCount;

    fs["frameCount"] >> frameCount;
    auto t1_it = std::back_inserter(t1);
    auto t2_it = std::back_inserter(t2);

    if (fs.isOpened())
    {

        for (int i = 0; i < frameCount; ++i, ++t1_it, ++t2_it)
        {
            // read in frame one
            {
                cv::Mat_<double> t1cv = cv::Mat_<double>::ones(4, 4);
                std::stringstream ss1;
                ss1 << "T1_" << i;
                fs[ss1.str()] >> t1cv;
                Eigen::Affine3d t1e;
                cv::cv2eigen(t1cv, t1e.matrix());
                *t1_it = t1e;
            }

            // read in frame two
            {
                cv::Mat_<double> t2cv = cv::Mat_<double>::ones(4, 4);
                std::stringstream ss2;
                ss2 << "T2_" << i;
                fs[ss2.str()] >> t2cv;
                Eigen::Affine3d t2e;
                cv::cv2eigen(t2cv, t2e.matrix());
                *t2_it = t2e;
            }
        }
        fs.release();
    }
    else
    {
        std::cerr << "failed to open input file " << filename << "\n";
        return 1;
    }
    return 0;
}

void reportCalibration(const std::string &EETFname,
                       const std::string &cameraTFname,
                       Eigen::Affine3d &resultAffine)
{
    std::cout << "\e[1;33m\n\n";
    std::cerr << "Result from " << EETFname << " to " << cameraTFname << ":\e[0m" << std::endl;
    std::cerr << resultAffine.matrix() << "\n\n";

    std::cerr << "\e[1;33m";
    std::cerr << "Translation (x,y,z) : "
              << resultAffine.translation().transpose() << std::endl;
    Eigen::Quaternion<double> quaternionResult(resultAffine.rotation());
    {
      std::stringstream ss;
      ss << quaternionResult.x() << " "
         << quaternionResult.y() << " "
         << quaternionResult.z() << " "
         << quaternionResult.w();
      std::cerr << "Rotation q(x,y,z,w): " << ss.str() << std::endl;
    }
    {
      std::stringstream ss;
      const auto rpy = resultAffine.rotation().eulerAngles(2, 1, 0);
      ss << rpy[2] << " "
         << rpy[1] << " "
         << rpy[0];
      std::cerr << "Rotation (roll,pitch,yaw): " << ss.str() << std::endl;
    }
    std::cerr << "\e[0m";

    std::cout << "\e[1;34m"
              << "Now you can publish tf in: [ Translation, Rotation] "
              << EETFname << " " << cameraTFname << "\e[0m"
              << "\n\n";

    Eigen::Transform<double, 3, Eigen::Affine> resultAffineInv = resultAffine.inverse();
    std::cerr << "Inverted translation (x,y,z) : "
              << resultAffineInv.translation().transpose() << std::endl;
    quaternionResult = Eigen::Quaternion<double>(resultAffineInv.rotation());
    std::cerr << "Inverted rotation (x,y,z,w): "
              << quaternionResult.x() << " "
              << quaternionResult.y() << " "
              << quaternionResult.z() << " "
              << quaternionResult.w() << std::endl;
    return;
}

Eigen::Affine3d estimateHandEye(const EigenAffineVector &baseToTip,
                                const EigenAffineVector &camToTag,
                                ceres::Solver::Summary &summary)
{

    auto t1_it = baseToTip.begin();
    auto t2_it = camToTag.begin();

    Eigen::Affine3d firstEEInverse, firstCamInverse;
    eigenVector tvecsArm, rvecsArm, tvecsFiducial, rvecsFiducial;

    bool firstTransform = true;

    for (int i = 0; i < baseToTip.size(); ++i, ++t1_it, ++t2_it)
    {
        auto &eigenEE = *t1_it;
        auto &eigenCam = *t2_it;
        if (firstTransform)
        {
            firstEEInverse = eigenEE.inverse();
            firstCamInverse = eigenCam.inverse();
            ROS_INFO("Adding first transformation.");
            firstTransform = false;
        }
        else
        {
            Eigen::Affine3d robotTipinFirstTipBase = firstEEInverse * eigenEE;
            Eigen::Affine3d fiducialInFirstFiducialBase =
                firstCamInverse * eigenCam;

            rvecsArm.push_back(eigenRotToEigenVector3dAngleAxis(
                robotTipinFirstTipBase.rotation()));
            tvecsArm.push_back(robotTipinFirstTipBase.translation());

            rvecsFiducial.push_back(eigenRotToEigenVector3dAngleAxis(
                fiducialInFirstFiducialBase.rotation()));
            tvecsFiducial.push_back(fiducialInFirstFiducialBase.translation());
            ROS_INFO("Hand Eye Calibration Transform Pair Added");

            Eigen::Vector4d r_tmp = robotTipinFirstTipBase.matrix().col(3);
            r_tmp[3] = 0;
            Eigen::Vector4d c_tmp = fiducialInFirstFiducialBase.matrix().col(3);
            c_tmp[3] = 0;

            std::cerr
                << "L2Norm EE: "
                << robotTipinFirstTipBase.matrix().block(0, 3, 3, 1).norm()
                << " vs Cam:"
                << fiducialInFirstFiducialBase.matrix().block(0, 3, 3, 1).norm()
                << std::endl;
        }
        std::cerr << "EE transform: \n"
                  << eigenEE.matrix() << std::endl;
        std::cerr << "Cam transform: \n"
                  << eigenCam.matrix() << std::endl;
    }

    camodocal::HandEyeCalibration calib;
    Eigen::Matrix4d result;
    calib.estimateHandEyeScrew(rvecsArm, tvecsArm, rvecsFiducial, tvecsFiducial,
                               result, summary, false);

    Eigen::Transform<double, 3, Eigen::Affine> resultAffine(result);
    reportCalibration(EETFname, cameraTFname, resultAffine);
    return resultAffine;
}

void writeCalibration(const Eigen::Affine3d &resultAffine,
                      const std::string &filename,
                      ceres::Solver::Summary &summary)
{
    cv::FileStorage fs(filename, cv::FileStorage::WRITE);

    std::cerr << "FULL CONVERGENCE REPORT \""
              << "\"...\n";
    std::cout << summary.BriefReport() << "\n";
    std::cout << summary.termination_type << "\n";
    std::cerr << "Writing calibration to \"" << filename << "\"...\n";
    if (fs.isOpened())
    {

        // in tf format (x,y,z,qx,qy,qz,qw)
        cv::Mat Mat_tfpose_1_7(1,7,CV_64F);  // row:1 coloum:7
        Eigen::Quaternion<double> resultQuat(resultAffine.rotation());
        Mat_tfpose_1_7.at<double>(0,0) = resultAffine.translation().x();
        Mat_tfpose_1_7.at<double>(0,1) = resultAffine.translation().y();
        Mat_tfpose_1_7.at<double>(0,2) = resultAffine.translation().z();
        Mat_tfpose_1_7.at<double>(0,3) = resultQuat.x();
        Mat_tfpose_1_7.at<double>(0,4) = resultQuat.y();
        Mat_tfpose_1_7.at<double>(0,5) = resultQuat.z();
        Mat_tfpose_1_7.at<double>(0,6) = resultQuat.w();
        fs << "handToEyeTF" << Mat_tfpose_1_7;
        
        // in transformation matrix 4x4 format 
        cv::Mat_<double> t1cv = cv::Mat_<double>::ones(4, 4);
        cv::eigen2cv(resultAffine.matrix(), t1cv);
        fs << "handToEyeTransform" << t1cv;

        fs << "initial_cost" << summary.initial_cost;
        fs << "final_cost" << summary.final_cost;
        fs << "change_cost" << summary.initial_cost - summary.final_cost;
        fs << "termination_type" << summary.termination_type;
        fs << "num_successful_iteration" << summary.num_successful_steps;
        fs << "num_unsuccessful_iteration" << summary.num_unsuccessful_steps;
        fs << "num_iteration"
           << summary.num_unsuccessful_steps + summary.num_successful_steps;
        fs.release();
    }
}

// function getch is from
// http://answers.ros.org/question/63491/keyboard-key-pressed/
int getch()
{
    static struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt); // save old settings
    newt = oldt;
    newt.c_lflag &= ~(ICANON);               // disable buffering
    tcsetattr(STDIN_FILENO, TCSANOW, &newt); // apply new settings

    int c = getchar(); // read character (non-blocking)

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt); // restore old settings
    return c;
}

void addFrame()
{
    ros::Time now = ros::Time::now();
    tf::StampedTransform EETransform, CamTransform;
    bool hasEE = true, hasCam = true;

    if (listener->waitForTransform(ARTagTFname, cameraTFname, now,
                                   ros::Duration(10)))
        listener->lookupTransform(ARTagTFname, cameraTFname, now, CamTransform);
    else
    {
        hasCam = false;
        ROS_WARN("Fail to Cam TF transform between %s to %s",
                 ARTagTFname.c_str(), cameraTFname.c_str());
    }

    if (listener->waitForTransform(baseTFname, EETFname, now, ros::Duration(10)))
        listener->lookupTransform(baseTFname, EETFname, now, EETransform);
    else
    {
        hasEE = false;
        ROS_WARN("Fail to EE TF transform between %s to %s", baseTFname.c_str(),
                 EETFname.c_str());
    }

    if (hasEE and hasCam)
    {

        Eigen::Affine3d eigenEE, eigenCam;
        tf::transformTFToEigen(EETransform, eigenEE);
        tf::transformTFToEigen(CamTransform, eigenCam);

        baseToTip.push_back(eigenEE);
        cameraToTag.push_back(eigenCam);

        if (firstTransform)
        {
            std::cerr << "\e[1;34m"
                      << "Adding First Transform"
                      << "\e[0m"
                      << "\n";
            firstEEInverse = eigenEE.inverse();
            firstCamInverse = eigenCam.inverse();
            ROS_INFO("Adding first transformation.");
            firstTransform = false;
        }
        else
        {
            std::cerr << "\e[1;34m"
                      << "Adding Transform #:" << rvecsArm.size() + 1 << "\e[0m"
                      << "\n";
            Eigen::Affine3d robotTipinFirstTipBase = firstEEInverse * eigenEE;
            Eigen::Affine3d fiducialInFirstFiducialBase =
                firstCamInverse * eigenCam;

            rvecsArm.push_back(eigenRotToEigenVector3dAngleAxis(
                robotTipinFirstTipBase.rotation()));
            tvecsArm.push_back(robotTipinFirstTipBase.translation());

            rvecsFiducial.push_back(eigenRotToEigenVector3dAngleAxis(
                fiducialInFirstFiducialBase.rotation()));
            tvecsFiducial.push_back(fiducialInFirstFiducialBase.translation());
            ROS_INFO("Hand Eye Calibration Transform Pair Added");

            std::cerr << "EE Relative transform: \n"
                      << robotTipinFirstTipBase.matrix() << std::endl;
            std::cerr << "Cam Relative transform: \n"
                      << fiducialInFirstFiducialBase.matrix() << std::endl;
            std::cerr << "EE pos: (" << EETransform.getOrigin().getX() << ", "
                      << EETransform.getOrigin().getY() << ", "
                      << EETransform.getOrigin().getZ() << ")\n";
            std::cerr << "EE rot: ("
                      << EETransform.getRotation().getAxis().getX() << ", "
                      << EETransform.getRotation().getAxis().getY() << ", "
                      << EETransform.getRotation().getAxis().getZ() << ", "
                      << EETransform.getRotation().getW() << ")\n";
            Eigen::Vector4d r_tmp = robotTipinFirstTipBase.matrix().col(3);
            r_tmp[3] = 0;
            Eigen::Vector4d c_tmp = fiducialInFirstFiducialBase.matrix().col(3);
            c_tmp[3] = 0;

            std::cerr
                << "L2Norm EE: "
                << robotTipinFirstTipBase.matrix().block(0, 3, 3, 1).norm()
                << " vs Cam:"
                << fiducialInFirstFiducialBase.matrix().block(0, 3, 3, 1).norm()
                << std::endl;
        }
        std::cerr << "EE transform: \n"
                  << eigenEE.matrix() << std::endl;
        std::cerr << "Cam transform: \n"
                  << eigenCam.matrix() << std::endl;
    }
    else
    {
        ROS_WARN("Fail to get one/both of needed TF transform");
    }
}

int main(int argc, char **argv)
{
    ros::init(argc, argv, "handeye_calib_camodocal");
    ros::NodeHandle nh("~");
    std::string transformPairsRecordFile;
    std::string transformPairsLoadFile;
    std::string calibratedTransformFile;
    bool loadTransformsFromFile = false;

    // getting TF names
    nh.param("cameraTF", cameraTFname, std::string("/camera_2_link"));
    nh.param("ARTagTF", ARTagTFname, std::string("/camera_2/ar_marker_0"));
    nh.param("EETF", EETFname, std::string("/ee_fixed_link"));
    nh.param("baseTF", baseTFname, std::string("/base_link"));
    nh.param("load_transforms_from_file", loadTransformsFromFile, false);
    nh.param("transform_pairs_record_filename", transformPairsRecordFile,
             std::string("TransformPairsInput.yml"));
    nh.param("transform_pairs_load_filename", transformPairsLoadFile,
             std::string("TransformPairsOutput.yml"));
    nh.param("output_calibrated_transform_filename", calibratedTransformFile,
             std::string("CalibratedTransform.yml"));

    std::cerr << "Calibrated output file: " << calibratedTransformFile << "\n";

    if (loadTransformsFromFile)
    {
        std::cerr << "Transform pairs loading file: " << transformPairsLoadFile
                  << "\n";
        EigenAffineVector t1, t2;
        readTransformPairsFromFile(transformPairsLoadFile, t1, t2);
        ceres::Solver::Summary summary;
        auto result = estimateHandEye(t1, t2, summary);
        writeCalibration(result, calibratedTransformFile, summary);
        return 0;
    }

    std::cerr << "Transform pairs recording to file: "
              << transformPairsRecordFile << "\n";

    ros::Rate r(10); // 10 hz
    listener = new (tf::TransformListener);

    ros::Duration(1.0).sleep(); // cache the TF transforms
    int key = 0;
    ROS_INFO("\e[1;35m Press s to add the current frame transformation to the cache.\e[0m");
    ROS_INFO("\e[1;34m Press d to delete last frame transformation.\e[0m");
    ROS_INFO("\e[1;33m Press q to calibrate frame transformation and exit the application.\e[0m");

    while (ros::ok())
    {
        key = getch();
        if ((key == 's') || (key == 'S'))
        {
            addFrame();
            writeTransformPairsToFile(baseToTip, cameraToTag,
                                      transformPairsRecordFile);
        }
        else if ((key == 'd') || (key == 'D'))
        {
            ROS_INFO("Deleted last frame transformation. Number of Current "
                     "Transformations: %u",
                     (unsigned int)rvecsArm.size());
            rvecsArm.pop_back();
            tvecsArm.pop_back();
            rvecsFiducial.pop_back();
            tvecsFiducial.pop_back();
            baseToTip.pop_back();
            cameraToTag.pop_back();
        }
        else if ((key == 'q') || (key == 'Q'))
        {
            if (rvecsArm.size() < 5)
            {
                ROS_WARN("Number of calibration transform pairs < 5.");
                ROS_INFO("Node Quit");
            }
            ROS_INFO("Calculating Calibration...");
            camodocal::HandEyeCalibration calib;
            Eigen::Matrix4d result;
            ceres::Solver::Summary summary;

            calib.estimateHandEyeScrew(rvecsArm, tvecsArm, rvecsFiducial,
                                       tvecsFiducial, result, summary,
                                       false);

            Eigen::Transform<double, 3, Eigen::Affine> resultAffine(result);
            reportCalibration(EETFname, cameraTFname, resultAffine);
            writeCalibration(resultAffine, calibratedTransformFile, summary);

            break;
        }
        else
        {
            std::cerr << key << " pressed.\n";
        }
        r.sleep();
    }

    ros::shutdown();
    return (0);
}

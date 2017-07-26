#include "AutoBuildMap.h"
#include <opencv2/core/eigen.hpp>
#include <pcl/filters/statistical_outlier_removal.h>
#include <pcl/filters/voxel_grid.h>
#include <boost/math/constants/constants.hpp>
#include <cmath>
#include <chrono>
#include <thread>
#include <opencv2/core/eigen.hpp>
#include <pangolin/pangolin.h>
namespace ORB_SLAM2
{
AutoBuildMap::AutoBuildMap(System *pSystem,
                           std::shared_ptr<Tracking> pTracking,
                           std::shared_ptr<MapDrawer> pMapDrawer,
                           cv::FileStorage &fSettings)
    :
    mpSystem(pSystem),
    mpTracker(pTracking),
    mpMapDrawer(pMapDrawer),
    mfHeightUpperBound(1.0),
    mfHeightLowerBound(-0.2),
    mbAutoDone(false),
    mbXYThetaPlan(false)
{
    mvBounds = std::vector<float>(4, 0.0);
    std::string planner = static_cast<std::string> (fSettings["AutoBuildMapPathPlanner"]);
    mLineWidth = fSettings["Viewer.KeyFrameLineWidth"];
    mPointSize = fSettings["Viewer.PointSize"];

    mfcGridSize = fSettings["GridSize"];
    mLineWidth *= 4.0;
    mPointSize *= 6.0;
    int xythetaPlane = fSettings["XYThetaPlan"];
    if (xythetaPlane != 0) {
        mbXYThetaPlan = true;
    }

    if (mbXYThetaPlan) {
        mfRobotHalfLength = fSettings["RobotHalfLength"];
        mfRobotHalfWidth = fSettings["RobotHalfWidth"];
        SetupRobot();
        mptPathPlanningXYTheta = std::make_shared<PathPlanning2DXYTheta>(planner,
                                                                         mPointSize,
                                                                         mLineWidth,
                                                                         mfcGridSize,
                                                                         mpTracker->msDataFolder,
                                                                         mfRobotHalfLength,
                                                                         mfRobotHalfWidth,
                                                                         mfHeightUpperBound,
                                                                         mfHeightLowerBound);
    }
    else {
        mfObstacleWidth = fSettings["RobotRadius"];
        mptPathPlanning = std::make_shared<PathPlanning2D>(planner,
                                                           mPointSize,
                                                           mLineWidth,
                                                           mfObstacleWidth,
                                                           mfcGridSize,
                                                           mpTracker->msDataFolder,
                                                           mfHeightUpperBound,
                                                           mfHeightLowerBound);
    }

    mpSolution = std::make_shared<std::vector<std::vector<float> > >();
}

void AutoBuildMap::Run()
{
    while (mpTracker->mState != Tracking::OK) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::cout << "Start building map automatically" << std::endl;
    sleep(2);  //wait for some time to accumulate some keyframes
    PlanPath();
    mbAutoDone = false;
    while (!mbAutoDone) {
        if (NeedReplanPath()) {
            int count = 0;
            while (!PlanPath()) {
                count++;
                if (count >= 4) {
                    EnsureAllAreasChecked();
                    if (!GoToStartPosition()) {
                        std::cout << "cannot go back to starting position" << std::endl;
                    }
                    SaveMap();
                    mbAutoDone = true;
                    break;
                }
            }
//            if(!PlanPath())
//            {
//                SaveMap();
//                break;
//            }
        }
    }
    std::cout << "\x1B[36m" << "Stop building map automatically" << "\x1B[0m" << std::endl;

}

void AutoBuildMap::EnsureAllAreasChecked()
{
    std::cout << "\x1B[33m" << "Sweeping over blank areas ..." << "\x1B[0m" << std::endl;
    float fCameraCenterX = 0;
    float fCameraCenterZ = 0;
    {
        unique_lock<mutex> lock(mMutexFrame);
        if (mCameraCenter.total() != 3) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return;
        }
        float *it = mCameraCenter.ptr<float>(0);
        fCameraCenterX = it[0];
        fCameraCenterZ = it[2];
    }
    std::vector<float> start = {fCameraCenterX, fCameraCenterZ};
    std::vector<float> target;
    int count = 0;
    int closeToTargetNumSameTarget = 0;

    while (true) {
        {
            unique_lock<mutex> lock(mMutexPath);
            mpSolution->clear();
        }
        std::cout << "\x1B[32m" << "calculating new target ..." << "\x1B[0m" << std::endl;
        mpMapDrawer->CalPointCloud();
        bool bGetNewTarget;
        if (mbXYThetaPlan) {
            bGetNewTarget = mptPathPlanningXYTheta->UnvisitedAreasToGo(target, mpMapDrawer->mCloud);
        }
        else {
            bGetNewTarget = mptPathPlanning->UnvisitedAreasToGo(target, mpMapDrawer->mCloud);
        }
        if (bGetNewTarget) {
            std::cout << "\x1B[32m" << " new target is: " << target[0] << "  " << target[1]
                      << "\x1B[0m" << std::endl;
            if (PlanPath(target)) {
                while (true) {
                    if (CloseToTarget(target, 0.5)) {
                        break;
                    }
                    if (NeedReplanPath()) {
                        if (!PlanPath(target)) {
                            break;
                        }
                    }
                }
            }
            if (mbXYThetaPlan) {
                mptPathPlanningXYTheta->reset(false);
            }
            else {
                mptPathPlanning->reset(false);
            }
            std::cout << "\x1B[32m" << " target[ " << target[0] << "  " << target[1] <<
                      " ] finished ..." << "\x1B[0m" << std::endl;
        }
        else {
            count++;
            if (count > 2) {
                break;
            }
        }
    }
    std::cout << "\x1B[34m" << "All areas have been checked ..." << "\x1B[0m" << std::endl;
}

bool AutoBuildMap::CloseToTarget(std::vector<float> &target, float disThresh)
{
    float fCameraCenterX = 0;
    float fCameraCenterZ = 0;
    {
        unique_lock<mutex> lock(mMutexFrame);
        if (mCameraCenter.total() != 3) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return false;
        }
        float *it = mCameraCenter.ptr<float>(0);
        fCameraCenterX = it[0];
        fCameraCenterZ = it[2];
    }
    float fToTargetX = target[0] - fCameraCenterX;
    float fToTargetZ = target[1] - fCameraCenterZ;
    float fToTargetDistanceSquared = pow(fToTargetX, 2.0) + pow(fToTargetZ, 2.0);
    if (fToTargetDistanceSquared < pow(disThresh, 2.0)) // if the camera is close to the target, replan
    {
        std::cout << "robot is close to target " << std::endl;
        if (mbXYThetaPlan) {
            mptPathPlanningXYTheta->reset(false);
        }
        else {
            mptPathPlanning->reset(false);
        }

        return true;
    }
    else {
        return false;
    }
}

bool AutoBuildMap::GoToStartPosition()
{
    std::cout << "\x1B[32m" << "Planning a path to go back to starting position ... " <<
              "\x1B[0m" << std::endl;
    mObstacles.clear();
    mpMapDrawer->CalPointCloud();
    float fCameraCenterX = 0;
    float fCameraCenterZ = 0;
    {
        unique_lock<mutex> lock(mMutexFrame);
        if (mCameraCenter.total() != 3) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return false;
        }
        float *it = mCameraCenter.ptr<float>(0);
        fCameraCenterX = it[0];
        fCameraCenterZ = it[2];
    }
    std::vector<float> start = {fCameraCenterX, fCameraCenterZ};
    std::vector<float> target = {0, 0};
    bool bFindPath;
    if (mbXYThetaPlan) {
        bFindPath = mptPathPlanningXYTheta->PlanPath(start, target, mpMapDrawer->mCloud);
    }
    else {
        bFindPath = mptPathPlanning->PlanPath(start, target, mpMapDrawer->mCloud);

    }
    if (bFindPath) {
        if (mbXYThetaPlan) {
            UpdateSolution(mptPathPlanningXYTheta->mpSolution);
        }
        else {
            UpdateSolution(mptPathPlanning->mpSolution);
        }
        while (!CloseToTarget(target, 0.7)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (NeedReplanPath()) {
                if (!PlanPath(target)) {
                    break;
                }
            }
        }
        {
            unique_lock<mutex> lock(mMutexPath);
            mpSolution->clear();
            // tell the robot to perform a zero-radius turning
            std::vector<float> rotatingCommand = {-1, -1};
            mpSolution->push_back(rotatingCommand);
        }
        std::chrono::time_point<std::chrono::system_clock> start;
        std::chrono::time_point<std::chrono::system_clock> end;
        start = std::chrono::system_clock::now();
        bool noLoopDetected = false;
        double maxWaitTime = 20;
        // wait for global BA
        while (!mpSystem->mpLoopCloser->mbRunningGBA) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            end = std::chrono::system_clock::now();
            std::chrono::duration<double> elapsed_seconds = end - start;
            if (elapsed_seconds.count() >= maxWaitTime) {
                noLoopDetected = true;
                std::cout << "No loop detected in " << maxWaitTime << " s ..." << std::endl;
                break;
            }
        }
        {
            unique_lock<mutex> lock(mMutexPath);
            mpSolution->clear();
            // tell the robot to stop
            std::vector<float> stoppingCommand = {-2, -2};
            mpSolution->push_back(stoppingCommand);
        }
        if (!noLoopDetected) {
            while (!mpSystem->mpLoopCloser->mbFinishedGBA) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }
    return bFindPath;
}

void AutoBuildMap::SaveMap()
{
    std::cout << "Find a closed map, saving the map ...";
    mpSystem->SaveMap();
}

bool AutoBuildMap::PlanPath()
{
    std::cout << "Planning a new path ... " << std::endl;
    mpMapDrawer->CalPointCloud();
    float fCameraCenterX = 0;
    float fCameraCenterZ = 0;
    std::vector<float> start;
    {
        unique_lock<mutex> lock(mMutexFrame);
        if (mCameraCenter.total() != 3) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return false;
        }
        float *it = mCameraCenter.ptr<float>(0);
        fCameraCenterX = it[0];
        fCameraCenterZ = it[2];
        start.push_back(fCameraCenterX);
        start.push_back(fCameraCenterZ);
        if (mbXYThetaPlan)
        {
            cv::Mat Rwc = mTwc.rowRange(0, 3).colRange(0, 3);
            Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic> ERwc;
            cv::cv2eigen(Rwc, ERwc);
            Eigen::Matrix3f rwc = ERwc;
            Eigen::Quaternionf tQua;
            tQua = rwc;
            float yaw = 2 * atan2(tQua.y(), tQua.w());
            start.push_back(yaw);
        }

    }
    bool bFindPath;
    if (mbXYThetaPlan) {
        mptPathPlanningXYTheta->reset(false);
        bFindPath = mptPathPlanningXYTheta->PlanPath(start, mpMapDrawer->mCloud);
        if (bFindPath) {
            UpdateSolution(mptPathPlanningXYTheta->mpSolution);
        }
    }
    else {
        mptPathPlanning->reset(false);
        bFindPath = mptPathPlanning->PlanPath(start, mpMapDrawer->mCloud);
        if (bFindPath) {
            UpdateSolution(mptPathPlanning->mpSolution);
        }
    }

    return bFindPath;
}

bool AutoBuildMap::PlanPath(std::vector<float> &target)
{
    std::cout << "Planning a new path ... " << std::endl;
//    mpMapDrawer->CalPointCloud();
    float fCameraCenterX = 0;
    float fCameraCenterZ = 0;
    std::vector<float> start;
    {
        unique_lock<mutex> lock(mMutexFrame);
        if (mCameraCenter.total() != 3) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return false;
        }
        float *it = mCameraCenter.ptr<float>(0);
        fCameraCenterX = it[0];
        fCameraCenterZ = it[2];
        start.push_back(fCameraCenterX);
        start.push_back(fCameraCenterZ);
        if (mbXYThetaPlan)
        {
            cv::Mat Rwc = mTwc.rowRange(0, 3).colRange(0, 3);
            Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic> ERwc;
            cv::cv2eigen(Rwc, ERwc);
            Eigen::Matrix3f rwc = ERwc;
            Eigen::Quaternionf tQua;
            tQua = rwc;
            float yaw = 2 * atan2(tQua.y(), tQua.w());
            start.push_back(yaw);
        }

    }
    bool bFindPath;
    if (mbXYThetaPlan) {
        mptPathPlanningXYTheta->reset(false);
        bFindPath = mptPathPlanningXYTheta->PlanPath(start, target, mpMapDrawer->mCloud);
        if (bFindPath) {
            UpdateSolution(mptPathPlanningXYTheta->mpSolution);
        }
    }
    else {
        mptPathPlanning->reset(false);
        bFindPath = mptPathPlanning->PlanPath(start, target, mpMapDrawer->mCloud);
        if (bFindPath) {
            UpdateSolution(mptPathPlanning->mpSolution);
        }
    }
    return bFindPath;
}

void AutoBuildMap::Show2DMap()
{
    std::vector<std::vector<int> > mObstaclesCopy;
    {
        unique_lock<mutex> lock(mMutexObstacle);
        mObstaclesCopy = mObstacles;
    }
    glBegin(GL_POINTS);
    glColor3f(1.0, 0.0, 0.0);
    cv::Mat Twc;
    {
        unique_lock<mutex> lockTwc(mMutexFrame);
        if (!mTwc.empty()) {
            Twc = mTwc.clone();
        }
    }

    for (int i = 0; i < mObstaclesCopy.size(); i++) {
        for (int j = 0; j < mObstaclesCopy[0].size(); j++) {
            if (mObstaclesCopy[i][j] == 1) {
                std::vector<float> tmpPoint;
                std::vector<int> tmpSrc = {j, i};
                GridToWorld(tmpSrc, tmpPoint);
                glVertex3f(tmpPoint[0], 5, tmpPoint[1]);
            }
        }
    }
    glEnd();
    if (mbXYThetaPlan) {
        mptPathPlanningXYTheta->ShowPlannedPath();
    }
    else {
        mptPathPlanning->ShowPlannedPath();
    }

}

void AutoBuildMap::UpdateSolution(std::shared_ptr<std::vector<std::vector<float> > > &pSolution)
{
    unique_lock<mutex> lock(mMutexPath);
    mpSolution = pSolution;
}

bool AutoBuildMap::NeedReplanPath()
{
    float fCameraCenterX = 0;
    float fCameraCenterZ = 0;

    cv::Mat depthIm;
    cv::Mat Twc;
    {
        unique_lock<mutex> lock(mMutexFrame);
        if (mCameraCenter.total() != 3) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return false;
        }
        float *it = mCameraCenter.ptr<float>(0);
        fCameraCenterX = it[0];
        fCameraCenterZ = it[2];

        if (mCurrentFrameImDepth.empty() || mTwc.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return false;
        }
        else {
            depthIm = mCurrentFrameImDepth.clone();
            Twc = mTwc.clone();
        }
    }
    std::vector<float> vStart;
    if (mpSolution && mpSolution->size() > 0) {
        vStart = (*mpSolution)[0];
    }
    std::vector<float> vTargetW;
    if (mbXYThetaPlan) {
        vTargetW = mptPathPlanningXYTheta->GetTargetW();
    }
    else {
        vTargetW = mptPathPlanning->GetTargetW();
    }

    float fToTargetX = vTargetW[0] - fCameraCenterX;
    float fToTargetZ = vTargetW[1] - fCameraCenterZ;
    float fToTargetDistanceSquared = pow(fToTargetX, 2.0) + pow(fToTargetZ, 2.0);
    if (fToTargetDistanceSquared < 0.25) // if the camera is close to the target, replan
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::cout << "close to target " << std::endl;
        return true;
    }
    pcl::PointCloud<pcl::PointXYZ>::Ptr pCurrentFrameCloud(new pcl::PointCloud<pcl::PointXYZ>());
    pcl::PointXYZ point;
    int imgStep = 3;
    Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic> ETwc;
    cv::cv2eigen(Twc, ETwc);
    for (int r = 0; r < depthIm.rows; r += imgStep) {
        const float *itD = depthIm.ptr<float>(r);
        const float y = mpTracker->mpMap->mLookupY.at<float>(0, r);
        const float *itX = mpTracker->mpMap->mLookupX.ptr<float>();
        for (size_t c = 0; c < (size_t) depthIm.cols;
             c += imgStep, itD += imgStep, itX += imgStep) {
            float depthValue = *itD;
            if (depthValue > 0.2 && depthValue < 3.0) {
                float zc = depthValue;
                float xc = *itX * depthValue;
                float yc = y * depthValue;
                Eigen::Vector4f X3Dc(xc, yc, zc, 1);
                Eigen::Vector4f X3Dw = ETwc * X3Dc;
                xc = X3Dw[0];
                yc = X3Dw[1];  // y axis is facing downward!!
                zc = X3Dw[2];
                if (yc >= -mfHeightUpperBound && yc <= -mfHeightLowerBound) {
                    point.x = xc;
                    point.y = yc;
                    point.z = zc;
                    pCurrentFrameCloud->points.push_back(point);
                }
            }
        }
    }
    if (pCurrentFrameCloud->points.size() <= 0) {
        return false;
    }
//    cout << "Current Frame Original Cloud Size:" << pCurrentFrameCloud->points.size() << endl;
    std::vector<int> indices;
    pcl::removeNaNFromPointCloud(*pCurrentFrameCloud, *pCurrentFrameCloud, indices);
//    cout << "Current Frame Cloud Size after NaN Removal:" << pCurrentFrameCloud->points.size() << endl;
//    pcl::PointCloud<pcl::PointXYZ>::Ptr cloudRandomFilter(new pcl::PointCloud<pcl::PointXYZ>);
    pcl::PointCloud<pcl::PointXYZ>::Ptr cloudIQRFilter(new pcl::PointCloud<pcl::PointXYZ>);
    pcl::PointCloud<pcl::PointXYZ>::Ptr cloudVoxel(new pcl::PointCloud<pcl::PointXYZ>);

    // voxel filter
    pcl::VoxelGrid<pcl::PointXYZ> voxelFilter;
    voxelFilter.setLeafSize(0.03f, 0.03f, 0.03f);       // resolution
    voxelFilter.setInputCloud(pCurrentFrameCloud);
    voxelFilter.filter(*cloudVoxel);
//    cout << "Current Frame Cloud Size After Voxel Filter:" << cloudVoxel->points.size() << endl;

    if (cloudVoxel->points.size() > 0) {
        std::vector<std::vector<float> > vXYZCoords(3, std::vector<float>());
        std::vector<std::vector<float> > vIQRBounds(3, std::vector<float>(2, 0));
        for (auto &p : cloudVoxel->points) {
            vXYZCoords[0].push_back(p.x);
            vXYZCoords[1].push_back(p.y);
            vXYZCoords[2].push_back(p.z);
        }
        for (int i = 0; i < vXYZCoords.size(); i++) {
            std::vector<float> &vCoords = vXYZCoords[i];
            auto const Q1 = vCoords.size() / 4;
            auto const Q3 = vCoords.size() * 3 / 4;
            std::nth_element(vCoords.begin(), vCoords.begin() + Q1, vCoords.end());
            std::nth_element(vCoords.begin() + Q1 + 1, vCoords.begin() + Q3, vCoords.end());
            float q25 = *(vCoords.begin() + Q1);
            float q75 = *(vCoords.begin() + Q3);
            float iqr = q75 - q25;
            float lowerBound = q25 - iqr * 1.5;
            float upperBound = q75 + iqr * 1.5;
            vIQRBounds[i][0] = lowerBound;
            vIQRBounds[i][1] = upperBound;
        }
        cloudIQRFilter->points.clear();
        for (auto &p : cloudVoxel->points) {
            if (p.x >= vIQRBounds[0][0] && p.x <= vIQRBounds[0][1] &&
                p.y >= vIQRBounds[1][0] && p.y <= vIQRBounds[1][1] &&
                p.z >= vIQRBounds[2][0] && p.z <= vIQRBounds[2][1]) {
                cloudIQRFilter->points.push_back(p);
            }
        }
//    cout << "Current Frame Cloud Size After IQR Filter:" << cloudIQRFilter->points.size() << endl;
    }
    else {
        cloudIQRFilter = cloudVoxel;
    }


    // statistical removal
    pcl::StatisticalOutlierRemoval<pcl::PointXYZ> sor;
    sor.setMeanK(15);
    sor.setStddevMulThresh(1.0);
    sor.setInputCloud(cloudIQRFilter);
    sor.filter(*pCurrentFrameCloud);
//    cout << "Current Frame Cloud Size After Statistical Filter:" << pCurrentFrameCloud->points.size() << endl;

    Get2DBounds(pCurrentFrameCloud, vStart);
    CalGridSize();
    {
        unique_lock<mutex> lock(mMutexObstacle);
        mObstacles = std::vector<std::vector<int> >(mnSizeY, std::vector<int>(mnSizeX, 0));
        double radius = mbXYThetaPlan ? std::min(mfRobotHalfLength, mfRobotHalfWidth) / mfcGridSize : mfObstacleWidth;
        double radiusSquared = pow(radius, 2.0);
        for (auto p : pCurrentFrameCloud->points) {
            int nTmpX = static_cast<int>((p.x - mvBounds[0]) / mfcGridSize);
            int nTmpY = static_cast<int>((p.z - mvBounds[2]) / mfcGridSize);
            for (int i = nTmpX - radius; i <= nTmpX + radius; i++) {
                for (int j = nTmpY - radius; j <= nTmpY + radius; j++) {
                    if (i >= 0 && i < mnSizeX && j >= 0 && j < mnSizeY) {
                        if (pow(i - nTmpX, 2.0) + pow(j - nTmpY, 2.0) <= radiusSquared) {
                            mObstacles[j][i] = 1;
                        }

                    }
                }
            }
        }

    }
    int obstacleCount = 0;
    int obstacleCountThreshold = 1;
    for (std::vector<std::vector<float> >::iterator it = mpSolution->begin();
         it != mpSolution->end(); it++) {
        std::vector<int> solutionInGrid;
        std::vector<float> solutionInWorld = {(*it)[0], (*it)[1]};
        WorldToGrid(solutionInWorld, solutionInGrid);
        if (solutionInGrid[0] >= 0 && solutionInGrid[0] < mObstacles[0].size() &&
            solutionInGrid[1] >= 0 && solutionInGrid[1] < mObstacles.size()) {
            if (mbXYThetaPlan)
            {
                if (!IsStateValid(solutionInGrid[0], solutionInGrid[1], (*it)[2]));
                {
                    obstacleCount++;
                    if (obstacleCount >= obstacleCountThreshold)
                    {
                        std::cout << "possible collision detected ..." << std::endl;
                        return true;
                    }

                }
            }
            else
            {
                if (mObstacles[solutionInGrid[1]][solutionInGrid[0]] == 1) {
                    obstacleCount++;
                    if (obstacleCount > obstacleCountThreshold)
                    {
                        std::cout << "possible collision detected ..." << std::endl;
                        return true;
                    }
                }
            }
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return false;
}

void AutoBuildMap::GetPath(std::vector<std::vector<float> > &path)
{
    unique_lock<mutex> lock(mMutexPath);
    if (mpSolution && mpSolution->size() > 0) {
        path = *mpSolution;
    }
}

void AutoBuildMap::SetCurrentFrame(Frame &m)
{
    unique_lock<mutex> lock(mMutexFrame);
    mCurrentFrameImDepth = m.mImDepth.clone();
    mCameraCenter = m.GetCameraCenter();
    mTwc = m.GetPoseInverse();
}

void AutoBuildMap::ShortenSolution()
{
    // find the closest point along the solution path to the
    // camera's current position, and delete all the points
    // ahead of this point
    float fCameraCenterX = 0;
    float fCameraCenterZ = 0;
    {
        unique_lock<mutex> lockCC(mMutexFrame);
        if (mCameraCenter.total() != 3) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return;
        }
        float *it = mCameraCenter.ptr<float>(0);
        fCameraCenterX = it[0];
        fCameraCenterZ = it[2];
    }
    std::vector<std::vector<float> >::iterator closestSolution;
    float minDistanceSquared = std::numeric_limits<float>::max();
    for (std::vector<std::vector<float> >::iterator it = mpSolution->begin();
         it != mpSolution->end(); it++) {
        float deltaX = (*it)[0] - fCameraCenterX;
        float deltaZ = (*it)[1] - fCameraCenterZ;
        float distanceSquared = pow(deltaX, 2.0) + pow(deltaZ, 2.0);
        if (distanceSquared > 25) {
            break;
        }
        if (minDistanceSquared > distanceSquared) {
            minDistanceSquared = distanceSquared;
            closestSolution = it;
        }
    }
    mpSolution->erase(mpSolution->begin(), closestSolution);
}

void AutoBuildMap::CalGridSize()
{
    mnSizeX = static_cast<int> ((mvBounds[1] - mvBounds[0]) / mfcGridSize);
    mnSizeY = static_cast<int> ((mvBounds[3] - mvBounds[2]) / mfcGridSize);
}

void AutoBuildMap::Get2DBounds(pcl::PointCloud<pcl::PointXYZ>::Ptr &cloud, std::vector<float> &vStart)
{
    mvBounds[0] = std::numeric_limits<float>::max();
    mvBounds[1] = std::numeric_limits<float>::lowest();
    mvBounds[2] = std::numeric_limits<float>::max();
    mvBounds[3] = std::numeric_limits<float>::lowest();
    for (auto p: cloud->points) {
        if (p.x < mvBounds[0]) {
            mvBounds[0] = p.x;
        }
        if (p.x > mvBounds[1]) {
            mvBounds[1] = p.x;
        }
        if (p.z < mvBounds[2]) {
            mvBounds[2] = p.z;
        }
        if (p.z > mvBounds[3]) {
            mvBounds[3] = p.z;
        }
    }
    if (mpSolution->size() > 0 && vStart.size() == 2) {
        mvBounds[0] = std::min(mvBounds[0], vStart[0]);
        mvBounds[1] = std::max(mvBounds[1], vStart[0]);
        mvBounds[2] = std::min(mvBounds[2], vStart[1]);
        mvBounds[3] = std::max(mvBounds[3], vStart[1]);
    }
    float margin = mfObstacleWidth * mfcGridSize + 0.2;
    mvBounds[0] -= margin;
    mvBounds[1] += margin;
    mvBounds[2] -= margin;
    mvBounds[3] += margin;
}

void AutoBuildMap::WorldToGrid(std::vector<float> &input, std::vector<int> &output)
{
    output.resize(2, 0);
    output[0] = static_cast<int> ((input[0] - mvBounds[0]) / mfcGridSize);
    output[1] = static_cast<int> ((input[1] - mvBounds[2]) / mfcGridSize);
}

void AutoBuildMap::GridToWorld(std::vector<int> &input, std::vector<float> &output)
{
    output.resize(2, 0);
    output[0] = input[0] * mfcGridSize + mvBounds[0];
    output[1] = input[1] * mfcGridSize + mvBounds[2];
}

bool AutoBuildMap::IsStateValid(int x, int y, double yaw)
{
    if (yaw > boost::math::constants::pi<double>() || yaw <= -boost::math::constants::pi<double>())
    {
        return false;
    }
    Eigen::Vector2i center;
    center << x , y;
    std::vector<std::vector<std::vector<int> > > triangles;
    Eigen::Matrix<double, 2, 4> robotCorners;
    std::vector<int> bounds;
    GetTriangles(center, yaw, triangles, robotCorners);
    GetRobotBounds(robotCorners, bounds);
    bool bCollision = RobotCollideObstacle(bounds, triangles);
    return !bCollision;
}

bool AutoBuildMap::RobotCollideObstacle(std::vector<int> &bounds,
                                        std::vector<std::vector<std::vector<int> > > &triangles)
{
    int obstacleCount = 0;
    int obstacleThresh = 5;
    for (int i = bounds[0]; i < bounds[1]; i++)
    {
        for (int j = bounds[2]; j < bounds[3]; j++)
        {
            if (mObstacles[j][i] == 1)
            {
                Eigen::Vector2i point;
                point << i, j;
                for (int k = 0; k < triangles.size(); k++)
                {
                    if (PointInTriangle(point, triangles[k]))
                    {
                        obstacleCount++;
                        if (obstacleCount >= obstacleThresh)
                        {
                            return true;
                        }
                        break;
                    }
                }
            }
        }
    }
    return false;
}

void AutoBuildMap::SetupRobot()
{
    Eigen::Matrix<float, 2, 4> robotCornersUnMoved;
    robotCornersUnMoved << mfRobotHalfWidth, -mfRobotHalfWidth, -mfRobotHalfWidth, mfRobotHalfWidth,
        mfRobotHalfLength, mfRobotHalfLength, -mfRobotHalfLength, -mfRobotHalfLength;
    robotCornersUnMoved /= mfcGridSize;
    mRobotCornersUnMoved = robotCornersUnMoved.cast<int>();
}

void AutoBuildMap::GetTriangles(Eigen::Vector2i &center,
                                double angle,
                                std::vector<std::vector<std::vector<int> > > &triangles,
                                Eigen::Matrix<double, 2, 4> &robotCorners)
{
    triangles.clear();
    Eigen::Rotation2D<double> rotMat(angle);
    robotCorners = rotMat.toRotationMatrix() * mRobotCornersUnMoved.cast<double>();
    robotCorners.colwise() += center.cast<double>();
    std::vector<std::vector<int> > triangle(3, std::vector<int>(2, 0));
    triangle[0][0] = robotCorners(0, 0);
    triangle[0][1] = robotCorners(1, 0);
    triangle[1][0] = robotCorners(0, 1);
    triangle[1][1] = robotCorners(1, 1);
    triangle[2][0] = robotCorners(0, 2);
    triangle[2][1] = robotCorners(1, 2);
    triangles.push_back(triangle);
    triangle[1][0] = robotCorners(0, 3);
    triangle[1][1] = robotCorners(1, 3);
    triangles.push_back(triangle);
}

void AutoBuildMap::GetRobotBounds(Eigen::Matrix<double, 2, 4> &robotCorners,
                                  std::vector<int> &bounds)
{
    bounds.resize(4, 0);
    Eigen::Vector2d minCoord = robotCorners.rowwise().minCoeff();
    Eigen::Vector2d maxCoord = robotCorners.rowwise().maxCoeff();
    bounds[0] = std::max(0, static_cast<int>(minCoord(0)));
    bounds[2] = std::max(0, static_cast<int>(minCoord(1)));
    bounds[1] = std::min(static_cast<int>(mObstacles[0].size() - 1), static_cast<int>(maxCoord(0)));
    bounds[3] = std::min(static_cast<int>(mObstacles.size() - 1), static_cast<int>(maxCoord(1)));
}


bool AutoBuildMap::PointInTriangle(Eigen::Vector2i &point, std::vector<std::vector<int> > triangle)
{
    // http://totologic.blogspot.com/2014/01/accurate-point-in-triangle-test.html
    // as we are dealing with int type values, we are not considering the accurate checking process

    int x = point(0);
    int y = point(1);
    int x1 = triangle[0][0];
    int y1 = triangle[0][1];
    int x2 = triangle[1][0];
    int y2 = triangle[1][1];
    int x3 = triangle[2][0];
    int y3 = triangle[2][1];

    double denominator = static_cast<double>((y2 - y3)*(x1 - x3) + (x3 - x2)*(y1 - y3));
    double a = ((y2 - y3)*(x - x3) + (x3 - x2)*(y - y3)) / denominator;
    double b = ((y3 - y1)*(x - x3) + (x1 - x3)*(y - y3)) / denominator;
    double c = 1 - a - b;
    return a >= 0 && a <= 1 && 0 <= b && b <= 1 && 0 <= c && c <= 1;
}

}
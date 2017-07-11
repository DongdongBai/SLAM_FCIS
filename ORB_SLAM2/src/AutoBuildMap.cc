#include "AutoBuildMap.h"
#include <opencv2/core/eigen.hpp>
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
                           cv::FileStorage &fSettings):
    mpSystem(pSystem),
    mpTracker(pTracking),
    mpMapDrawer(pMapDrawer),
    mfcLeafSize(0.025),
    mfObstacleWidth(5.0),
    mfHeightUpperBound(0.6),
    mfHeightLowerBound(-0.6)
{
    mvBounds = std::vector<float> (4, 0.0);
    std::string planner = static_cast<std::string> (fSettings["AutoBuildMapPathPlanner"]);
    mLineWidth = fSettings["Viewer.KeyFrameLineWidth"];
    mPointSize = fSettings["Viewer.PointSize"];
    mLineWidth *= 4.0;
    mPointSize *= 6.0;
    mptPathPlanning = std::make_shared<PathPlanning2D> (planner,
                                                        mPointSize,
                                                        mLineWidth,
                                                        mfObstacleWidth,
                                                        mfcLeafSize);
}

void AutoBuildMap::Run()
{
    while (mpTracker->mState != Tracking::OK)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::cout << "Start building map automatically" << std::endl;
    sleep(2);  //wait for some time to accumulate some keyframes
    PlanPath();
    bool bAutoDone = false;
    while(!bAutoDone)
    {
        if(NeedReplanPath())
        {
            int count = 0;
            while (!PlanPath())
            {
                count++;
                if (count >= 2)
                {
                    if (GoToStartPosition())
                    {
                        std::cout << "going back to starting position" << std::endl;
                    }
                    else
                    {
                        std::cout << "cannot go back to starting position" << std::endl;
                    }
                    SaveMap();
                    bAutoDone = true;
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
    std::cout << "Stop building map automatically" << std::endl;

}

bool AutoBuildMap::GoToStartPosition()
{
    std::cout << "Planning a path to go back to starting position ... " << std::endl;
    mpMapDrawer->CalPointCloud();
    float fCameraCenterX = 0;
    float fCameraCenterZ = 0;
    {
        unique_lock<mutex> lock(mMutexFrame);
        if (mCameraCenter.total() != 3)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return false;
        }
        float *it =  mCameraCenter.ptr<float>(0);
        fCameraCenterX = it[0];
        fCameraCenterZ = it[2];
    }
    std::vector<float> start = {fCameraCenterX, fCameraCenterZ};
    std::vector<float> target = {0, 0};
    bool bFindPath = mptPathPlanning->PlanPath(start, target, mpMapDrawer->mCloud);
    if (bFindPath)
    {
        UpdateSolution(mptPathPlanning->mpSolution);
        // wait for global BA
        while(!mpSystem->mpLoopCloser->mbRunningGBA)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        };
        while(!mpSystem->mpLoopCloser->mbFinishedGBA)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        };
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
    {
        unique_lock<mutex> lock(mMutexFrame);
        if (mCameraCenter.total() != 3)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return false;
        }
        float *it =  mCameraCenter.ptr<float>(0);
        fCameraCenterX = it[0];
        fCameraCenterZ = it[2];
    }
    std::vector<float> start = {fCameraCenterX, fCameraCenterZ};
    bool bFindPath = mptPathPlanning->PlanPath(start, mpMapDrawer->mCloud);
    if (bFindPath)
    {
        UpdateSolution(mptPathPlanning->mpSolution);
    }
    return bFindPath;
}

void AutoBuildMap::Show2DMap()
{
    std::vector<std::vector<float> > obstacles;
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
        if (!mTwc.empty())
        {
            Twc = mTwc.clone();
        }
    }

    for (int i = 0; i < mObstaclesCopy.size(); i++)
    {
        for (int j = 0; j < mObstaclesCopy[0].size(); j++)
        {
            if (mObstaclesCopy[i][j] == 1)
            {
                std::vector<float> tmpPoint;
                std::vector<int> tmpSrc = {j, i};
                GridToWorld(tmpSrc, tmpPoint);
                glVertex3f(tmpPoint[0], 5, tmpPoint[1]);
            }
        }
    }
    glEnd();
    mptPathPlanning->ShowPlannedPath();

}

void AutoBuildMap::UpdateSolution(std::shared_ptr<std::vector<std::vector<float> > > &pSolution)
{
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
        if (mCameraCenter.total() != 3)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return false;
        }
        float *it =  mCameraCenter.ptr<float>(0);
        fCameraCenterX = it[0];
        fCameraCenterZ = it[2];

        if (mCurrentFrameImDepth.empty() || mTwc.empty())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return false;
        }
        else
        {
            depthIm = mCurrentFrameImDepth.clone();
            Twc = mTwc.clone();
        }
    }
    std::vector<float> vStart = (*mpSolution)[0];
    std::vector<float> vTargetW = mptPathPlanning->GetTargetW();
    float fToTargetX = vTargetW[0] - fCameraCenterX;
    float fToTargetZ = vTargetW[1] - fCameraCenterZ;
    float fToTargetDistanceSquared = pow(fToTargetX, 2.0) + pow(fToTargetZ, 2.0);
    if (fToTargetDistanceSquared < 0.36) // if the camera is close to the target, replan
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::cout << "close to target " << std::endl;
        return true;
    }
    pcl::PointCloud<pcl::PointXYZ>::Ptr pCurrentFrameCloud(new pcl::PointCloud<pcl::PointXYZ>());
    pcl::PointXYZ point;
    int imgStep = 2;
    Eigen::Matrix<float,Eigen::Dynamic,Eigen::Dynamic> ETwc;
    cv::cv2eigen(Twc,ETwc);
    for (int r = 0; r < depthIm.rows; r += imgStep) {
        const float *itD = depthIm.ptr<float>(r);
        const float y = mpTracker->mpMap->mLookupY.at<float>(0, r);
        const float *itX = mpTracker->mpMap->mLookupX.ptr<float>();
        for (size_t c = 0; c < (size_t) depthIm.cols;
             c += imgStep, itD += imgStep, itX += imgStep) {
            float depthValue = *itD;
            if (depthValue > 0.1 && depthValue < 3.0) {
                float zc = depthValue;
                float xc = *itX * depthValue;
                float yc = y * depthValue;
                Eigen::Vector4f X3Dc(xc, yc, zc, 1);
                Eigen::Vector4f X3Dw = ETwc * X3Dc;
                xc = X3Dw[0];
                yc = X3Dw[1];  // y axis is facing downward!!
                zc = X3Dw[2];
                if (yc <= mfHeightUpperBound && yc >= mfHeightLowerBound)
                {
                    point.x = xc;
                    point.y = yc;
                    point.z = zc;
                    pCurrentFrameCloud->points.push_back(point);
                }
            }
        }
    }

    Get2DBounds(pCurrentFrameCloud, vStart);
    CalGridSize();
    {
        unique_lock<mutex> lock(mMutexObstacle);
        mObstacles = std::vector<std::vector<int> > (mnSizeY, std::vector<int> (mnSizeX, 0));
        double radiusSquared = pow(mfObstacleWidth, 2.0);
        for (auto p : pCurrentFrameCloud->points) {
            int nTmpX = static_cast<int>((p.x - mvBounds[0]) / mfcLeafSize);
            int nTmpY = static_cast<int>((p.z - mvBounds[2]) / mfcLeafSize);
            for (int i = nTmpX - mfObstacleWidth; i <= nTmpX + mfObstacleWidth; i++)
            {
                for (int j = nTmpY - mfObstacleWidth; j <= nTmpY + mfObstacleWidth; j++)
                {
                    if (i >= 0 && i < mnSizeX && j >= 0 && j < mnSizeY)
                    {
                        if (pow(i - nTmpX, 2.0) + pow(j - nTmpY, 2.0) <= radiusSquared)
                        {
                            mObstacles[j][i] = 1;
                        }

                    }
                }
            }
        }
    }

    for (std::vector<std::vector<float> >::iterator it = mpSolution->begin();
        it != mpSolution->end(); it++)
    {
        std::vector<int> solutionInGrid;
        std::vector<float> solutionInWorld = {(*it)[0], (*it)[1]};
        WorldToGrid(solutionInWorld, solutionInGrid);
        if (solutionInGrid[0] >= 0 && solutionInGrid[0] < mObstacles[0].size() &&
            solutionInGrid[1] >= 0 && solutionInGrid[1] < mObstacles.size())
        {
            if (mObstacles[solutionInGrid[1]][solutionInGrid[0]] == 1)
            {
                return true;
            }
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return false;
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
        if (mCameraCenter.total() != 3)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return;
        }
        float *it =  mCameraCenter.ptr<float>(0);
        fCameraCenterX = it[0];
        fCameraCenterZ = it[2];
    }
    std::vector<std::vector<float> >::iterator closestSolution;
    float minDistanceSquared = std::numeric_limits<float>::max();
    for (std::vector<std::vector<float> >::iterator it = mpSolution->begin();
         it != mpSolution->end(); it++)
    {
        float deltaX = (*it)[0] - fCameraCenterX;
        float deltaZ = (*it)[1] - fCameraCenterZ;
        float distanceSquared = pow(deltaX, 2.0) + pow(deltaZ, 2.0);
        if (distanceSquared > 25)
        {
            break;
        }
        if (minDistanceSquared > distanceSquared)
        {
            minDistanceSquared = distanceSquared;
            closestSolution = it;
        }
    }
    mpSolution->erase(mpSolution->begin(), closestSolution);
}

void AutoBuildMap::CalGridSize()
{
    mnSizeX = static_cast<int> ((mvBounds[1] - mvBounds[0]) / mfcLeafSize);
    mnSizeY = static_cast<int> ((mvBounds[3] - mvBounds[2]) / mfcLeafSize);
}

void AutoBuildMap::Get2DBounds(pcl::PointCloud<pcl::PointXYZ>::Ptr &cloud, std::vector<float> &vStart)
{
    mvBounds[0] = std::numeric_limits<float>::max();
    mvBounds[1] = std::numeric_limits<float>::lowest();
    mvBounds[2] = std::numeric_limits<float>::max();
    mvBounds[3] = std::numeric_limits<float>::lowest();
    for (auto p: cloud->points)
    {
        if (p.x < mvBounds[0])
        {
            mvBounds[0] = p.x;
        }
        if (p.x > mvBounds[1])
        {
            mvBounds[1] = p.x;
        }
        if (p.z < mvBounds[2])
        {
            mvBounds[2] = p.z;
        }
        if (p.z > mvBounds[3])
        {
            mvBounds[3] = p.z;
        }
    }
    if (mpSolution->size() > 0)
    {
        mvBounds[0] = std::min(mvBounds[0], vStart[0]) - 0.05;
        mvBounds[1] = std::max(mvBounds[1], vStart[0]) + 0.05;
        mvBounds[2] = std::min(mvBounds[2], vStart[1]) - 0.05;
        mvBounds[3] = std::max(mvBounds[3], vStart[1]) + 0.05;
    }
}

void AutoBuildMap::WorldToGrid(std::vector<float> &input, std::vector<int> &output)
{
    output.resize(2, 0);
    output[0] = static_cast<int> ((input[0] - mvBounds[0]) / mfcLeafSize);
    output[1] = static_cast<int> ((input[1] - mvBounds[2]) / mfcLeafSize);
}

void AutoBuildMap::GridToWorld(std::vector<int> &input, std::vector<float> &output)
{
    output.resize(2, 0);
    output[0] = input[0] * mfcLeafSize + mvBounds[0];
    output[1] = input[1] * mfcLeafSize + mvBounds[2];
}

}
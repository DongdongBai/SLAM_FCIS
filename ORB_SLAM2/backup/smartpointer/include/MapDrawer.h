/**
* This file is part of ORB-SLAM2.
*
* Copyright (C) 2014-2016 Raúl Mur-Artal <raulmur at unizar dot es> (University of Zaragoza)
* For more information see <https://github.com/raulmur/ORB_SLAM2>
*
* ORB-SLAM2 is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* ORB-SLAM2 is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with ORB-SLAM2. If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef MAPDRAWER_H
#define MAPDRAWER_H

#include "Map.h"
#include "MapPoint.h"
#include "KeyFrame.h"
#include <pangolin/pangolin.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <mutex>
#include <thread>
#include <memory>
namespace ORB_SLAM2
{

class MapDrawer
{
public:
    MapDrawer(std::shared_ptr<Map> pMap, const string &strSettingPath);

    std::shared_ptr<Map> mpMap;
    bool mbCalPointCloud;
    std::unique_ptr<std::thread> mpThreadOctomap;
    pcl::PointCloud<pcl::PointXYZ>::Ptr mCloud;

    void GeneratePointCloud(const vector<std::shared_ptr<KeyFrame> > &vpKFs, pcl::PointCloud<pcl::PointXYZ>::Ptr cloud,
                            int begin, int step);

    void FilterPointCloud(pcl::PointCloud<pcl::PointXYZ>::Ptr cloud, pcl::PointCloud<pcl::PointXYZ>::Ptr output);

    void DrawPointCloud();

    void DrawMapPoints();

    void DrawKeyFrames(const bool bDrawKF, const bool bDrawGraph);

    void DrawCurrentCamera(pangolin::OpenGlMatrix &Twc);

    void SetCurrentCameraPose(const cv::Mat &Tcw);

    void GetCurrentOpenGLCameraMatrix(pangolin::OpenGlMatrix &M);

    void BuildOctomap(pcl::PointCloud<pcl::PointXYZ>::Ptr cloud);

private:

    float mKeyFrameSize;
    float mKeyFrameLineWidth;
    float mGraphLineWidth;
    float mPointSize;
    float mCameraSize;
    float mCameraLineWidth;

    cv::Mat mCameraPose;

    std::mutex mMutexCamera;
    std::mutex mMutexCloud;
    std::mutex mMutexMCloud;
};

} //namespace ORB_SLAM

#endif // MAPDRAWER_H

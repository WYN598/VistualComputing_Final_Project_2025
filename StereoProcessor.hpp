#pragma once
#include <opencv2/opencv.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/calib3d.hpp>
#include <vector>
#include <glm/glm.hpp>
#include <iostream>
#include <fstream>
#include <string>

// 3D 顶点结构
struct Vertex {
    glm::vec3 position;
    glm::vec3 color;
};

// 算法参数结构体
struct StereoParams {
    int numDisparities = 64;
    int blockSize = 9;
    int minDisparity = 0;
    // 新增高级参数
    int uniquenessRatio = 10;    // 唯一性检测 (5-15)
    int speckleWindowSize = 100; // 斑点过滤窗口 (50-200)
    int speckleRange = 32;       // 斑点深度差异阈值 (1-32)
};

class StereoProcessor {
public:
    std::vector<Vertex> pointCloud;
    cv::Mat disparityVis;
    cv::Mat rectLeft, rectRight;
    bool isRectified = false;

    // 核心处理函数
    bool process(const std::string& pathL, const std::string& pathR, const StereoParams& params)
    {
        try {
            // 1. 读取与预处理
            cv::Mat imgL = cv::imread(pathL);
            cv::Mat imgR = cv::imread(pathR);
            if (imgL.empty() || imgR.empty()) return false;

            // 降采样限制 (提升大图处理速度)
            float scale = 1.0f;
            if (imgL.cols > 1200) {
                scale = 1200.0f / imgL.cols;
                cv::resize(imgL, imgL, cv::Size(), scale, scale);
                cv::resize(imgR, imgR, cv::Size(), scale, scale);
            }

            // 2. 自动校正 (如果未校正或图片变了)
            // 简单起见，每次重新计算，保证参数实时性
            if (!computeRectification(imgL, imgR)) return false;

            // 3. SGBM 参数配置
            int realNumDisp = (params.numDisparities / 16) * 16;
            if (realNumDisp < 16) realNumDisp = 16;
            int realBlockSize = params.blockSize | 1; // 确保奇数
            if (realBlockSize < 5) realBlockSize = 5;

            // 创建 SGBM 对象 (包含高级抗噪)
            cv::Ptr<cv::StereoSGBM> sgbm = cv::StereoSGBM::create(
                params.minDisparity, realNumDisp, realBlockSize,
                8 * 3 * realBlockSize * realBlockSize,
                32 * 3 * realBlockSize * realBlockSize,
                1, 63,
                params.uniquenessRatio, // Uniqueness
                params.speckleWindowSize, // Speckle Window
                params.speckleRange,      // Speckle Range
                cv::StereoSGBM::MODE_SGBM_3WAY
            );

            cv::Mat disp16;
            sgbm->compute(rectLeft, rectRight, disp16);

            // 4. 可视化生成
            cv::Mat disp8;
            disp16.convertTo(disp8, CV_8U, 255.0 / (realNumDisp * 16.0));
            cv::applyColorMap(disp8, disparityVis, cv::COLORMAP_INFERNO); // 更专业的 Inferno 配色

            // 5. 重投影到 3D
            double W = rectLeft.cols;
            double H = rectLeft.rows;
            cv::Mat Q = cv::Mat::eye(4, 4, CV_64F);
            Q.at<double>(0, 3) = -W / 2.0;
            Q.at<double>(1, 3) = -H / 2.0;
            Q.at<double>(2, 3) = 0.8 * W;
            Q.at<double>(3, 2) = -1.0 / W;

            cv::Mat points3D;
            cv::reprojectImageTo3D(disp16, points3D, Q, true);

            // 6. 生成点云 (带有效性过滤)
            pointCloud.clear();
            // 预分配内存防止频繁扩容
            pointCloud.reserve(W * H / 4);

            int step = 1; // 全分辨率采样
            for (int y = 0; y < H; y += step) {
                for (int x = 0; x < W; x += step) {
                    cv::Vec3f p = points3D.at<cv::Vec3f>(y, x);
                    cv::Vec3b c = rectLeft.at<cv::Vec3b>(y, x);

                    // 深度有效性过滤 + 黑色背景过滤
                    if (std::isfinite(p[2]) && p[2] > 0 && p[2] < 8000 && (c[0] + c[1] + c[2] > 20)) {
                        Vertex v;
                        v.position = glm::vec3(p[0], -p[1], -p[2]);
                        v.color = glm::vec3(c[2] / 255.0f, c[1] / 255.0f, c[0] / 255.0f);
                        pointCloud.push_back(v);
                    }
                }
            }
            return true;
        }
        catch (...) { return false; }
    }

    // 导出为 PLY 文件 (MeshLab/Blender 通用格式)
    bool saveToPLY(const std::string& filename) {
        if (pointCloud.empty()) return false;
        std::ofstream out(filename);
        if (!out.is_open()) return false;

        // PLY Header
        out << "ply\n";
        out << "format ascii 1.0\n";
        out << "element vertex " << pointCloud.size() << "\n";
        out << "property float x\nproperty float y\nproperty float z\n";
        out << "property uchar red\nproperty uchar green\nproperty uchar blue\n";
        out << "end_header\n";

        // Data
        for (const auto& v : pointCloud) {
            out << v.position.x << " " << v.position.y << " " << v.position.z << " "
                << (int)(v.color.r * 255) << " " << (int)(v.color.g * 255) << " " << (int)(v.color.b * 255) << "\n";
        }
        out.close();
        return true;
    }

private:
    bool computeRectification(const cv::Mat& imgL, const cv::Mat& imgR) {
        // 使用 KNN + Ratio Test 保证专业数据集的鲁棒性
        std::vector<cv::KeyPoint> kp1, kp2;
        cv::Mat desc1, desc2;
        cv::Ptr<cv::SIFT> sift = cv::SIFT::create();
        sift->detectAndCompute(imgL, cv::noArray(), kp1, desc1);
        sift->detectAndCompute(imgR, cv::noArray(), kp2, desc2);

        if (desc1.empty() || desc2.empty()) return false;

        cv::FlannBasedMatcher matcher;
        std::vector<std::vector<cv::DMatch>> knn_matches;
        matcher.knnMatch(desc1, desc2, knn_matches, 2);

        std::vector<cv::Point2f> pts1, pts2;
        const float ratio_thresh = 0.75f;
        for (size_t i = 0; i < knn_matches.size(); i++) {
            if (knn_matches[i][0].distance < ratio_thresh * knn_matches[i][1].distance) {
                pts1.push_back(kp1[knn_matches[i][0].queryIdx].pt);
                pts2.push_back(kp2[knn_matches[i][0].trainIdx].pt);
            }
        }
        if (pts1.size() < 15) return false;

        cv::Mat mask;
        cv::Mat F = cv::findFundamentalMat(pts1, pts2, cv::FM_RANSAC, 3.0, 0.99, mask);
        if (F.empty()) return false;

        std::vector<cv::Point2f> good1, good2;
        for (int i = 0; i < mask.rows; i++) {
            if (mask.at<uchar>(i)) {
                good1.push_back(pts1[i]);
                good2.push_back(pts2[i]);
            }
        }
        if (good1.size() < 10) return false;

        cv::Mat H1, H2;
        cv::stereoRectifyUncalibrated(good1, good2, F, imgL.size(), H1, H2, 5.0);
        cv::warpPerspective(imgL, rectLeft, H1, imgL.size());
        cv::warpPerspective(imgR, rectRight, H2, imgR.size());
        return true;
    }
};
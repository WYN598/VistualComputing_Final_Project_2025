#pragma once
#include <opencv2/opencv.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/calib3d.hpp>
#include <vector>
#include <glm/glm.hpp>
#include <iostream>
#include <fstream>
#include <string>
#include <sstream>

// 3D 顶点
struct Vertex {
    glm::vec3 position;
    glm::vec3 color;
};

// 算法参数
struct StereoParams {
    int numDisparities = 64;
    int blockSize = 9;
    int minDisparity = 0;
    int uniquenessRatio = 10;
    int speckleWindowSize = 100;
    int speckleRange = 32;
    float processScale = 0.5f; // 0.5=Fast, 1.0=High Quality
};

// 简单的日志回调接口
typedef void (*LogCallback)(const char* fmt, ...);

class StereoProcessor {
public:
    std::vector<Vertex> pointCloud;
    cv::Mat disparityVis;
    cv::Mat rectLeft, rectRight;

    // 外部注入日志函数
    LogCallback logger = nullptr;

    void setLogger(LogCallback cb) { logger = cb; }

    void log(const char* fmt, ...) {
        if (logger) {
            char buf[1024];
            va_list args;
            va_start(args, fmt);
            vsnprintf(buf, IM_ARRAYSIZE(buf), fmt, args);
            buf[IM_ARRAYSIZE(buf) - 1] = 0;
            va_end(args);
            logger("%s", buf);
        }
    }

    bool process(const std::string& pathL, const std::string& pathR, const StereoParams& params)
    {
        try {
            log("[Vision] Loading images...");
            cv::Mat imgL = cv::imread(pathL);
            cv::Mat imgR = cv::imread(pathR);
            if (imgL.empty() || imgR.empty()) {
                log("[Error] Failed to load images.");
                return false;
            }

            // 根据用户设定的 Scale 进行缩放
            if (params.processScale < 0.99f) {
                cv::resize(imgL, imgL, cv::Size(), params.processScale, params.processScale);
                cv::resize(imgR, imgR, cv::Size(), params.processScale, params.processScale);
                log("[Vision] Resized input to %.0f%% for performance.", params.processScale * 100.0f);
            }

            // 自动校正
            log("[Vision] Computing Rectification (SIFT + RANSAC)...");
            if (!computeRectification(imgL, imgR)) {
                log("[Error] Rectification failed. Features not found.");
                return false;
            }

            // SGBM 参数规整
            int realNumDisp = (params.numDisparities / 16) * 16;
            if (realNumDisp < 16) realNumDisp = 16;
            int realBlockSize = params.blockSize | 1;

            log("[Vision] Running SGBM (Disp: %d, Block: %d)...", realNumDisp, realBlockSize);

            cv::Ptr<cv::StereoSGBM> sgbm = cv::StereoSGBM::create(
                params.minDisparity, realNumDisp, realBlockSize,
                8 * 3 * realBlockSize * realBlockSize,
                32 * 3 * realBlockSize * realBlockSize,
                1, 63,
                params.uniquenessRatio,
                params.speckleWindowSize,
                params.speckleRange,
                cv::StereoSGBM::MODE_SGBM_3WAY
            );

            cv::Mat disp16;
            sgbm->compute(rectLeft, rectRight, disp16);

            // 可视化
            cv::Mat disp8;
            disp16.convertTo(disp8, CV_8U, 255.0 / (realNumDisp * 16.0));
            cv::applyColorMap(disp8, disparityVis, cv::COLORMAP_INFERNO);

            // 重投影
            log("[Vision] Reprojecting to 3D...");
            double W = rectLeft.cols;
            double H = rectLeft.rows;
            cv::Mat Q = cv::Mat::eye(4, 4, CV_64F);
            Q.at<double>(0, 3) = -W / 2.0;
            Q.at<double>(1, 3) = -H / 2.0;
            Q.at<double>(2, 3) = 0.8 * W;
            Q.at<double>(3, 2) = -1.0 / W;

            cv::Mat points3D;
            cv::reprojectImageTo3D(disp16, points3D, Q, true);

            // 生成点云
            pointCloud.clear();
            pointCloud.reserve(W * H);

            int step = 1;
            for (int y = 0; y < H; y += step) {
                for (int x = 0; x < W; x += step) {
                    cv::Vec3f p = points3D.at<cv::Vec3f>(y, x);
                    cv::Vec3b c = rectLeft.at<cv::Vec3b>(y, x);

                    // 深度过滤
                    if (std::isfinite(p[2]) && p[2] > 0 && p[2] < 8000 && (c[0] + c[1] + c[2] > 20)) {
                        Vertex v;
                        v.position = glm::vec3(p[0], -p[1], -p[2]);
                        v.color = glm::vec3(c[2] / 255.0f, c[1] / 255.0f, c[0] / 255.0f);
                        pointCloud.push_back(v);
                    }
                }
            }
            log("[Success] Generated %d points.", (int)pointCloud.size());
            return true;
        }
        catch (std::exception& e) {
            log("[Exception] %s", e.what());
            return false;
        }
    }

    bool saveToPLY(const std::string& filename) {
        if (pointCloud.empty()) return false;
        std::ofstream out(filename);
        if (!out.is_open()) return false;
        out << "ply\nformat ascii 1.0\nelement vertex " << pointCloud.size() << "\n";
        out << "property float x\nproperty float y\nproperty float z\n";
        out << "property uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n";
        for (const auto& v : pointCloud) {
            out << v.position.x << " " << v.position.y << " " << v.position.z << " "
                << (int)(v.color.r * 255) << " " << (int)(v.color.g * 255) << " " << (int)(v.color.b * 255) << "\n";
        }
        return true;
    }

private:
    bool computeRectification(const cv::Mat& imgL, const cv::Mat& imgR) {
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
        log("[Vision] Features matched: %d", (int)pts1.size());
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
        log("[Vision] RANSAC Inliers: %d", (int)good1.size());
        if (good1.size() < 10) return false;

        cv::Mat H1, H2;
        cv::stereoRectifyUncalibrated(good1, good2, F, imgL.size(), H1, H2, 5.0);
        cv::warpPerspective(imgL, rectLeft, H1, imgL.size());
        cv::warpPerspective(imgR, rectRight, H2, imgR.size());
        return true;
    }
};
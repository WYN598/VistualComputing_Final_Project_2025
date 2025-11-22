#pragma once
#include <opencv2/opencv.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/calib3d.hpp>
// [WLS必选] 引入 ximgproc 模块，通常在 opencv_contrib 中
// 如果报错 "No such file"，请确保安装了 opencv-contrib-python (Python) 或 opencv_contrib (C++)
#include <opencv2/ximgproc.hpp> 

#include <vector>
#include <glm/glm.hpp>
#include <iostream>
#include <fstream>
#include <string>
#include <sstream>

// 3D Vertex
struct Vertex {
    glm::vec3 position;
    glm::vec3 color;
};

// Algorithm & Calibration Parameters
struct StereoParams {
    int numDisparities = 64;
    int blockSize = 9;
    int minDisparity = 0;
    int uniquenessRatio = 10;
    int speckleWindowSize = 100;
    int speckleRange = 32;
    float processScale = 0.5f;

    // --- [New] WLS Filter Toggle ---
    bool useWLS = true;          // 用户开关：是否启用 WLS 滤波
    double wlsLambda = 8000.0;   // 平滑系数 (经验值 8000)
    double wlsSigma = 1.5;       // 颜色敏感度 (经验值 1.0-2.0)

    // --- Calibration Flags ---
    bool useCalibration = false;
    float focalLength = 4000.0f;
    float principalX = 0.0f;
    float principalY = 0.0f;
    float baseline = 174.0f;
};

typedef void (*LogCallback)(const char* fmt, ...);

class StereoProcessor {
public:
    std::vector<Vertex> pointCloud;
    cv::Mat disparityVis;
    cv::Mat rectLeft, rectRight;

    LogCallback logger = nullptr;

    void setLogger(LogCallback cb) { logger = cb; }

    void log(const char* fmt, ...) {
        if (logger) {
            char buf[1024];
            va_list args;
            va_start(args, fmt);
            vsnprintf(buf, 1024, fmt, args);
            buf[1023] = 0;
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

            // Resize
            if (params.processScale < 0.99f) {
                cv::resize(imgL, imgL, cv::Size(), params.processScale, params.processScale);
                cv::resize(imgR, imgR, cv::Size(), params.processScale, params.processScale);
                log("[Vision] Resized input to %.0f%%.", params.processScale * 100.0f);
            }

            // Calibration / Rectification Check
            if (params.useCalibration) {
                log("[Vision] Mode: MANUAL (Skipping auto-rectification).");
                rectLeft = imgL.clone();
                rectRight = imgR.clone();
            }
            else {
                log("[Vision] Mode: AUTO (Running SIFT rectification).");
                if (!computeRectification(imgL, imgR)) {
                    log("[Error] Rectification failed.");
                    return false;
                }
            }

            // === SGBM Setup ===
            int realNumDisp = (params.numDisparities / 16) * 16;
            if (realNumDisp < 16) realNumDisp = 16;
            int realBlockSize = params.blockSize | 1;

            // 1. 创建左匹配器 (Left Matcher)
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

            // =========================================================
            // [New] WLS Filtering Logic
            // =========================================================
            if (params.useWLS) {
                log("[Vision] Running WLS Filter (Lambda=%.0f)...", params.wlsLambda);

                // 2. 创建右匹配器 (Right Matcher) - 用于一致性检查
                // 这一步需要 opencv_contrib 模块
                cv::Ptr<cv::StereoMatcher> right_matcher = cv::ximgproc::createRightMatcher(sgbm);

                // 3. 计算左右视差图
                cv::Mat leftDisp, rightDisp;
                sgbm->compute(rectLeft, rectRight, leftDisp);
                right_matcher->compute(rectRight, rectLeft, rightDisp);

                // 4. 创建并应用 WLS 滤波器
                cv::Ptr<cv::ximgproc::DisparityWLSFilter> wls_filter = cv::ximgproc::createDisparityWLSFilter(sgbm);
                wls_filter->setLambda(params.wlsLambda);
                wls_filter->setSigmaColor(params.wlsSigma);

                // 5. 滤波 (结果存入 disp16)
                wls_filter->filter(leftDisp, rectLeft, disp16, rightDisp);
            }
            else {
                // 标准模式：只计算左视差
                sgbm->compute(rectLeft, rectRight, disp16);
            }

            // === Visualization (Fixed Contrast) ===
            cv::Mat disp8, dispAdjusted;
            cv::subtract(disp16, cv::Scalar(params.minDisparity * 16), dispAdjusted);
            dispAdjusted.convertTo(disp8, CV_8U, 255.0 / (realNumDisp * 16.0));
            cv::applyColorMap(disp8, disparityVis, cv::COLORMAP_INFERNO);

            // === Disparity to Float ===
            cv::Mat dispFloat;
            disp16.convertTo(dispFloat, CV_32F, 1.0 / 16.0);

            // === Q Matrix Construction (Fixed Z+) ===
            double W = rectLeft.cols;
            double H = rectLeft.rows;
            cv::Mat Q = cv::Mat::eye(4, 4, CV_64F);

            if (params.useCalibration) {
                float scale = (params.processScale < 0.99f) ? params.processScale : 1.0f;
                double f = params.focalLength * scale;
                double cx = params.principalX * scale;
                double cy = params.principalY * scale;
                double B = params.baseline;

                if (std::abs(cx) < 1e-5) cx = W / 2.0;
                if (std::abs(cy) < 1e-5) cy = H / 2.0;

                Q.at<double>(0, 3) = -cx;
                Q.at<double>(1, 3) = -cy;
                Q.at<double>(2, 3) = f;
                Q.at<double>(3, 2) = 1.0 / B;  // Positive Z
                Q.at<double>(3, 3) = 0.0;      // Linear Depth

                log("[Vision] Real Q Matrix: f=%.1f, B=%.1f", f, B);
            }
            else {
                double f_guess = 0.8 * W;
                Q.at<double>(0, 3) = -W / 2.0; Q.at<double>(1, 3) = -H / 2.0;
                Q.at<double>(2, 3) = f_guess; Q.at<double>(3, 2) = -1.0 / W;
            }

            // Reprojection
            cv::Mat points3D;
            cv::reprojectImageTo3D(dispFloat, points3D, Q, true);

            pointCloud.clear();
            pointCloud.reserve(W * H);

            float maxDepth = params.useCalibration ? 50000.0f : 10000.0f;

            int step = 1;
            for (int y = 0; y < H; y += step) {
                for (int x = 0; x < W; x += step) {
                    float d = dispFloat.at<float>(y, x);

                    // Filter: strictly remove invalid points
                    if (d < ((float)params.minDisparity - 0.5f)) continue;

                    cv::Vec3f p = points3D.at<cv::Vec3f>(y, x);
                    cv::Vec3b c = rectLeft.at<cv::Vec3b>(y, x);

                    if (std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2])) {
                        if (p[2] > 10.0f && p[2] < maxDepth) {
                            Vertex v;
                            v.position = glm::vec3(p[0], -p[1], -p[2]);
                            v.color = glm::vec3(c[2] / 255.0f, c[1] / 255.0f, c[0] / 255.0f);
                            pointCloud.push_back(v);
                        }
                    }
                }
            }
            log("[Success] Generated %d points (WLS: %s).", (int)pointCloud.size(), params.useWLS ? "ON" : "OFF");
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
        // (代码保持不变，与之前一致)
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
        for (size_t i = 0; i < knn_matches.size(); i++) {
            if (knn_matches[i][0].distance < 0.75f * knn_matches[i][1].distance) {
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
            if (mask.at<uchar>(i)) { good1.push_back(pts1[i]); good2.push_back(pts2[i]); }
        }
        if (good1.size() < 10) return false;
        cv::Mat H1, H2;
        cv::stereoRectifyUncalibrated(good1, good2, F, imgL.size(), H1, H2, 5.0);
        cv::warpPerspective(imgL, rectLeft, H1, imgL.size());
        cv::warpPerspective(imgR, rectRight, H2, imgR.size());
        return true;
    }
};
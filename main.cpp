#include <Windows.h>
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <future>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "StereoProcessor.hpp"
#include "GLRenderer.hpp"

// --- 全局状态 ---
struct AppState {
    std::string pathL, pathR;
    StereoParams params;
    bool isProcessing = false;
    bool processSuccess = false;
    std::string statusMsg = "Ready.";
    int viewMode = 0;
};

std::string OpenFileDialog(bool save = false) {
    OPENFILENAMEA ofn; char szFile[260] = { 0 }; ZeroMemory(&ofn, sizeof(ofn)); ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = NULL; ofn.lpstrFile = szFile; ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = save ? "PLY Point Cloud\0*.ply\0" : "Image Files\0*.jpg;*.png;*.bmp;*.jpeg\0All\0*.*\0";
    ofn.nFilterIndex = 1; ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST; if (save) ofn.Flags = OFN_OVERWRITEPROMPT;
    if ((save ? GetSaveFileNameA(&ofn) : GetOpenFileNameA(&ofn)) == TRUE) return std::string(ofn.lpstrFile);
    return "";
}

void SetupProfessionalStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;
    style.WindowRounding = 6.0f; style.FrameRounding = 4.0f;
    colors[ImGuiCol_WindowBg] = ImVec4(0.11f, 0.12f, 0.15f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.20f, 0.22f, 0.27f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.18f, 0.35f, 0.58f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.42f, 0.68f, 1.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.16f, 0.19f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.14f, 0.18f, 1.00f);
}

int main(int argc, char** argv) {
    GLRenderer renderer;
    if (!renderer.init()) return -1;

    IMGUI_CHECKVERSION(); ImGui::CreateContext(); ImGuiIO& io = ImGui::GetIO(); (void)io;
    SetupProfessionalStyle();
    ImGui_ImplGlfw_InitForOpenGL(renderer.window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    StereoProcessor processor;
    AppState state;
    std::future<bool> futureResult;

    while (!glfwWindowShouldClose(renderer.window)) {
        glfwPollEvents();

        // ==========================================================
        // 交互逻辑核心升级：左键旋转 / 右键平移 / 滚轮缩放
        // ==========================================================
        // 关键修复：在 NewFrame 之后处理输入，确保 io.WantCaptureMouse 状态正确
        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();

        if (!io.WantCaptureMouse) {
            double x, y; glfwGetCursorPos(renderer.window, &x, &y);

            // 1. 滚轮缩放
            if (io.MouseWheel != 0.0f) renderer.camera.processZoom((float)io.MouseWheel);

            // 2. 左键旋转 (Rotate)
            if (glfwGetMouseButton(renderer.window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
                if (!renderer.isRotateDragging) {
                    renderer.isRotateDragging = true; renderer.lastX = x; renderer.lastY = y;
                }
                else {
                    renderer.camera.processRotate((float)(x - renderer.lastX), (float)(y - renderer.lastY));
                    renderer.lastX = x; renderer.lastY = y;
                }
            }
            else {
                renderer.isRotateDragging = false;
            }

            // 3. 右键平移 (Pan) - 抓手工具
            if (glfwGetMouseButton(renderer.window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
                if (!renderer.isPanDragging) {
                    renderer.isPanDragging = true; renderer.lastX = x; renderer.lastY = y;
                }
                else {
                    renderer.camera.processPan((float)(x - renderer.lastX), (float)(y - renderer.lastY));
                    renderer.lastX = x; renderer.lastY = y;
                }
            }
            else {
                renderer.isPanDragging = false;
            }

            if (!renderer.isRotateDragging && !renderer.isPanDragging) {
                // Idle
            }
            else if (renderer.isRotateDragging && renderer.isPanDragging) {
                // 防止冲突
            }
        }

        int dispW, dispH; glfwGetFramebufferSize(renderer.window, &dispW, &dispH);

        // 异步结果
        if (state.isProcessing && futureResult.valid()) {
            if (futureResult.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                state.processSuccess = futureResult.get(); state.isProcessing = false;
                if (state.processSuccess) {
                    renderer.uploadGeometry(processor.pointCloud); renderer.uploadTexture(processor.disparityVis);
                    state.statusMsg = "Success! Points: " + std::to_string(processor.pointCloud.size()); state.viewMode = 0;
                }
                else state.statusMsg = "Failed.";
            }
        }

        // 模态加载窗
        if (state.isProcessing) ImGui::OpenPopup("Processing");
        if (ImGui::BeginPopupModal("Processing", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
            ImGui::Text("Generating 3D Geometry..."); ImGui::Separator(); ImGui::TextColored(ImVec4(1, 1, 0, 1), "Please Wait...");
            if (!state.isProcessing) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // 图层1: 视差
        if (state.viewMode == 1 && renderer.disparityTexture != 0) {
            ImGui::SetNextWindowPos(ImVec2(350, 0)); ImGui::SetNextWindowSize(ImVec2((float)dispW - 350, (float)dispH));
            ImGui::Begin("Disp", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBringToFrontOnFocus);
            ImGui::Image((ImTextureID)(intptr_t)renderer.disparityTexture, ImGui::GetContentRegionAvail()); ImGui::End();
        }

        // 图层2: 侧边栏
        ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(350, (float)dispH));
        ImGui::Begin("Sidebar", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
        ImGui::TextColored(ImVec4(0.4f, 0.6f, 1.0f, 1.0f), "DATA SOURCE"); ImGui::Separator(); ImGui::Spacing();

        // 【修复点】使用不同的文字 "Left OK" 和 "Right OK"，避免 ID 冲突
        if (ImGui::Button(state.pathL.empty() ? "Left" : "Left OK", ImVec2(160, 80))) { std::string p = OpenFileDialog(); if (!p.empty()) state.pathL = p; } ImGui::SameLine();
        if (ImGui::Button(state.pathR.empty() ? "Right" : "Right OK", ImVec2(160, 80))) { std::string p = OpenFileDialog(); if (!p.empty()) state.pathR = p; }

        ImGui::Spacing(); ImGui::TextColored(ImVec4(0.8f, 0.4f, 0.8f, 1.0f), "PARAMS"); ImGui::Separator();
        ImGui::Text("Disparities"); ImGui::SliderInt("##num", &state.params.numDisparities, 16, 256); state.params.numDisparities = (state.params.numDisparities / 16) * 16;
        ImGui::Text("Block Size"); ImGui::SliderInt("##blk", &state.params.blockSize, 5, 31); if (state.params.blockSize % 2 == 0) state.params.blockSize++;
        ImGui::Text("Min Disp"); ImGui::SliderInt("##min", &state.params.minDisparity, -20, 20);
        ImGui::Text("Noise Filter"); ImGui::SliderInt("##uniq", &state.params.uniquenessRatio, 0, 20);
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
        if (ImGui::Button("BUILD 3D", ImVec2(330, 50)) && !state.isProcessing) {
            if (state.pathL.empty() || state.pathR.empty()) state.statusMsg = "Err: Images";
            else { state.isProcessing = true; state.statusMsg = "Processing..."; futureResult = std::async(std::launch::async, [&]() {return processor.process(state.pathL, state.pathR, state.params);}); }
        }
        if (ImGui::Button("EXPORT", ImVec2(330, 30))) {
            if (processor.pointCloud.empty()) state.statusMsg = "Err: Empty";
            else { std::string p = OpenFileDialog(true); if (!p.empty()) { if (p.find(".ply") == std::string::npos)p += ".ply"; processor.saveToPLY(p); state.statusMsg = "Saved."; } }
        }
        ImGui::Spacing(); ImGui::Separator(); ImGui::TextWrapped("%s", state.statusMsg.c_str());
        ImGui::End();

        // 图层3: Mode
        ImGui::SetNextWindowPos(ImVec2((float)dispW - 220, 20)); ImGui::SetNextWindowSize(ImVec2(200, 0));
        ImGui::Begin("Mode", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground);
        bool is3D = (state.viewMode == 0); if (is3D) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1));
        if (ImGui::Button("3D View", ImVec2(90, 30))) state.viewMode = 0; if (is3D) ImGui::PopStyleColor(); ImGui::SameLine();
        bool isDisp = (state.viewMode == 1); if (isDisp) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1));
        if (ImGui::Button("Disparity", ImVec2(90, 30))) state.viewMode = 1; if (isDisp) ImGui::PopStyleColor();
        ImGui::End();

        // 图层4: Controls Tips
        ImGui::SetNextWindowPos(ImVec2(370, (float)dispH - 130));
        ImGui::SetNextWindowSize(ImVec2(200, 110));
        ImGui::Begin("Tips", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar);
        ImGui::Text("Controls:"); ImGui::Separator();
        ImGui::BulletText("Left Click: Rotate");
        ImGui::BulletText("Right Click: Pan");
        ImGui::BulletText("Scroll: Zoom");
        ImGui::End();

        // Render
        glViewport(0, 0, dispW, dispH); glClearColor(0.08f, 0.08f, 0.10f, 1.0f); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (state.viewMode == 0) renderer.render3D(processor.pointCloud.size());
        ImGui::Render(); ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(renderer.window);
    }

    ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplGlfw_Shutdown(); ImGui::DestroyContext(); glfwTerminate();
    return 0;
}
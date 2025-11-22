#include <Windows.h>
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <future>
#include <vector>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "StereoProcessor.hpp"
#include "GLRenderer.hpp"

// --- 日志系统 ---
struct AppLog {
    ImGuiTextBuffer Buf;
    ImVector<int> LineOffsets;
    bool AutoScroll = true;

    void Clear() { Buf.clear(); LineOffsets.clear(); LineOffsets.push_back(0); }
    void AddLog(const char* fmt, ...) IM_FMTARGS(2) {
        int old_size = Buf.size();
        va_list args; va_start(args, fmt); Buf.appendfv(fmt, args); va_end(args);
        for (int new_size = Buf.size(); old_size < new_size; old_size++)
            if (Buf[old_size] == '\n') LineOffsets.push_back(old_size + 1);
    }
    void DrawContent() {
        if (ImGui::Button("Clear")) Clear();
        ImGui::SameLine(); ImGui::Checkbox("Auto-scroll", &AutoScroll);
        ImGui::Separator();
        ImGui::BeginChild("scrolling", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(Buf.begin());
        if (AutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }
};

AppLog g_Log;
void LogCallbackFunc(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    char buf[1024]; vsnprintf(buf, 1024, fmt, args); va_end(args);
    g_Log.AddLog("%s\n", buf);
}

struct AppState {
    std::string pathL, pathR;
    StereoParams params;
    bool isProcessing = false;
    bool processSuccess = false;
    int viewMode = 0;
    float sidebarWidth = 380.0f; // 动态记录侧边栏宽度
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
    style.WindowRounding = 4.0f; style.FrameRounding = 3.0f;
    colors[ImGuiCol_WindowBg] = ImVec4(0.11f, 0.12f, 0.15f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.20f, 0.22f, 0.27f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.18f, 0.35f, 0.58f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.42f, 0.68f, 1.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.16f, 0.19f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.15f, 0.16f, 0.19f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.20f, 0.22f, 0.27f, 1.00f);
}

// --- 辅助组件：帮助提示标记 ---
// 在文本旁边显示一个 (?)，鼠标悬停时弹出解释
void HelpMarker(const char* desc) {
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// 辅助函数：带自动换行的 Bullet Text
void BulletTextWrapped(const char* text) {
    ImGui::Bullet();
    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + ImGui::GetContentRegionAvail().x);
    ImGui::Text(text);
    ImGui::PopTextWrapPos();
}

int main(int argc, char** argv) {
    GLRenderer renderer;
    if (!renderer.init()) return -1;

    IMGUI_CHECKVERSION(); ImGui::CreateContext(); ImGuiIO& io = ImGui::GetIO(); (void)io;
    SetupProfessionalStyle();

    ImFont* font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\segoeui.ttf", 18.0f);
    if (!font) io.Fonts->AddFontDefault();

    ImGui_ImplGlfw_InitForOpenGL(renderer.window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    StereoProcessor processor;
    processor.setLogger(LogCallbackFunc);
    AppState state;
    std::future<bool> futureResult;

    float logHeight = 250.0f;

    g_Log.AddLog("[System] Ready. Load images to start.\n");

    while (!glfwWindowShouldClose(renderer.window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();

        // 交互处理
        if (!io.WantCaptureMouse) {
            double x, y; glfwGetCursorPos(renderer.window, &x, &y);
            if (io.MouseWheel != 0.0f) renderer.camera.processZoom((float)io.MouseWheel);
            if (glfwGetMouseButton(renderer.window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
                if (!renderer.isRotateDragging) { renderer.isRotateDragging = true; renderer.lastX = x; renderer.lastY = y; }
                else { renderer.camera.processRotate((float)(x - renderer.lastX), (float)(y - renderer.lastY)); renderer.lastX = x; renderer.lastY = y; }
            }
            else renderer.isRotateDragging = false;

            if (glfwGetMouseButton(renderer.window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
                if (!renderer.isPanDragging) { renderer.isPanDragging = true; renderer.lastX = x; renderer.lastY = y; }
                else { renderer.camera.processPan((float)(x - renderer.lastX), (float)(y - renderer.lastY)); renderer.lastX = x; renderer.lastY = y; }
            }
            else renderer.isPanDragging = false;
        }

        int dispW, dispH; glfwGetFramebufferSize(renderer.window, &dispW, &dispH);

        // 异步结果
        if (state.isProcessing && futureResult.valid()) {
            if (futureResult.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                state.processSuccess = futureResult.get(); state.isProcessing = false;
                if (state.processSuccess) {
                    renderer.uploadGeometry(processor.pointCloud); renderer.uploadTexture(processor.disparityVis);
                    state.viewMode = 0;
                }
            }
        }

        // 模态窗口
        if (state.isProcessing) ImGui::OpenPopup("Processing");
        if (ImGui::BeginPopupModal("Processing", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
            ImGui::Text("Processing Pipeline Running..."); ImGui::Separator();
            ImGui::TextColored(ImVec4(1, 1, 0, 1), "Please Wait...");
            if (!state.isProcessing) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // ----------------------------------------------------------
        // 1. 左侧控制面板 (Sidebar)
        // ----------------------------------------------------------
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(state.sidebarWidth, (float)dispH), ImGuiCond_FirstUseEver);
        ImGui::Begin("Controls", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

        state.sidebarWidth = ImGui::GetWindowWidth(); // 记录实时宽度

        // === 用户指南 ===
        if (ImGui::CollapsingHeader("USER GUIDE & HELP", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
            ImGui::TextWrapped("How to Navigate:");
            BulletTextWrapped("Rotate: Left Click + Drag");
            BulletTextWrapped("Pan: Right Click + Drag");
            BulletTextWrapped("Zoom: Scroll Wheel");
            ImGui::Spacing();
            ImGui::TextWrapped("Workflow:");
            BulletTextWrapped("1. Load Left/Right images.");
            BulletTextWrapped("2. Adjust params (optional).");
            BulletTextWrapped("3. Click RUN RECONSTRUCTION.");
            ImGui::PopStyleColor();
            ImGui::Separator();
        }

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.4f, 0.6f, 1.0f, 1.0f), "1. DATA SOURCE"); ImGui::Separator(); ImGui::Spacing();
        if (ImGui::Button(state.pathL.empty() ? "Left Img" : "Left OK", ImVec2(120, 80))) { std::string p = OpenFileDialog(); if (!p.empty()) { state.pathL = p; g_Log.AddLog("Left: %s\n", p.c_str()); } } ImGui::SameLine();
        if (ImGui::Button(state.pathR.empty() ? "Right Img" : "Right OK", ImVec2(120, 80))) { std::string p = OpenFileDialog(); if (!p.empty()) { state.pathR = p; g_Log.AddLog("Right: %s\n", p.c_str()); } }

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.8f, 0.4f, 0.8f, 1.0f), "2. ALGORITHM SETTINGS"); ImGui::Separator();

        // === 参数区域：添加 HelpMarker ===

        ImGui::Text("Processing Scale"); ImGui::SameLine();
        HelpMarker("Downscale images before processing to speed up computation.\n\nFast (50%): 4x faster, good for tuning.\nHigh (100%): Best detail, slower.");
        if (ImGui::RadioButton("Fast (50%)", state.params.processScale == 0.5f)) state.params.processScale = 0.5f; ImGui::SameLine();
        if (ImGui::RadioButton("High (100%)", state.params.processScale == 1.0f)) state.params.processScale = 1.0f;

        ImGui::Text("Disparities"); ImGui::SameLine();
        HelpMarker("Max search range for matching points.\n\nIncrease this value if foreground objects (close to camera) are missing or cut off.");
        ImGui::SliderInt("##num", &state.params.numDisparities, 16, 256); state.params.numDisparities = (state.params.numDisparities / 16) * 16;

        ImGui::Text("Block Size"); ImGui::SameLine();
        HelpMarker("Size of the matching window.\n\nSmall (5-9): More fine details, sharper edges, but more noise.\nLarge (11+): Smoother surfaces, less noise, but blurred details.");
        ImGui::SliderInt("##blk", &state.params.blockSize, 5, 31); if (state.params.blockSize % 2 == 0) state.params.blockSize++;

        ImGui::Text("Min Disp"); ImGui::SameLine();
        HelpMarker("Minimum disparity shift.\n\nAdjust this if the 3D model seems too far away or if the background is cut off.");
        ImGui::SliderInt("##min", &state.params.minDisparity, -20, 20);

        ImGui::Text("Noise Filter"); ImGui::SameLine();
        HelpMarker("Uniqueness Ratio (Strictness).\n\nHigher (5-15): Strict filtering, removes 'ghost' points and flying noise.\nLower (0-5): Relaxed, fills more holes but adds noise.");
        ImGui::SliderInt("##uniq", &state.params.uniquenessRatio, 0, 20);

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), "3. VISUALIZATION"); ImGui::Separator();

        ImGui::Text("Point Size"); ImGui::SameLine();
        HelpMarker("Visual size of the rendered points.\n\nIncrease size for sparse models to make them look solid.\nDecrease size for dense models to see texture details.");
        ImGui::SliderFloat("##pt", &renderer.pointSize, 1.0f, 10.0f);

        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
        if (ImGui::Button("RUN RECONSTRUCTION", ImVec2(-1, 50)) && !state.isProcessing) {
            if (state.pathL.empty() || state.pathR.empty()) g_Log.AddLog("[Error] Missing images!\n");
            else { state.isProcessing = true; g_Log.AddLog("Starting reconstruction job...\n"); futureResult = std::async(std::launch::async, [&]() {return processor.process(state.pathL, state.pathR, state.params);}); }
        }
        if (ImGui::Button("EXPORT RESULT (.PLY)", ImVec2(-1, 30))) {
            if (processor.pointCloud.empty()) g_Log.AddLog("[Error] Nothing to export.\n");
            else { std::string p = OpenFileDialog(true); if (!p.empty()) { if (p.find(".ply") == std::string::npos)p += ".ply"; processor.saveToPLY(p); g_Log.AddLog("Exported to: %s\n", p.c_str()); } }
        }
        ImGui::End();

        // ----------------------------------------------------------
        // 2. 底部控制台 (Bottom Console) - 可拉伸
        // ----------------------------------------------------------
        if (logHeight < 50.0f) logHeight = 50.0f;
        if (logHeight > dispH * 0.6f) logHeight = dispH * 0.6f;

        float consoleY = (float)dispH - logHeight;
        float consoleW = (float)dispW - state.sidebarWidth;

        // Splitter
        ImGui::SetNextWindowPos(ImVec2(state.sidebarWidth, consoleY - 4.0f));
        ImGui::SetNextWindowSize(ImVec2(consoleW, 4.0f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
        ImGui::Begin("Splitter", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
        ImGui::PopStyleColor();
        ImGui::Button("##splitBtn", ImVec2(-1, -1));
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (ImGui::IsItemActive()) logHeight -= ImGui::GetIO().MouseDelta.y;
        ImGui::End();

        // Console
        ImGui::SetNextWindowPos(ImVec2(state.sidebarWidth, consoleY));
        ImGui::SetNextWindowSize(ImVec2(consoleW, logHeight));
        ImGui::Begin("Console Output", NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
        g_Log.DrawContent();
        ImGui::End();

        // ----------------------------------------------------------
        // 3. 悬浮组件: 视图切换
        // ----------------------------------------------------------
        ImGui::SetNextWindowPos(ImVec2((float)dispW - 220, 20));
        ImGui::SetNextWindowSize(ImVec2(200, 0));
        ImGui::Begin("ViewMode", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground);
        bool is3D = (state.viewMode == 0); if (is3D) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1));
        if (ImGui::Button("3D View", ImVec2(90, 30))) state.viewMode = 0; if (is3D) ImGui::PopStyleColor(); ImGui::SameLine();
        bool isDisp = (state.viewMode == 1); if (isDisp) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1));
        if (ImGui::Button("Disparity", ImVec2(90, 30))) state.viewMode = 1; if (isDisp) ImGui::PopStyleColor();
        ImGui::End();

        // ----------------------------------------------------------
        // 4. 渲染层
        // ----------------------------------------------------------
        glViewport(0, 0, dispW, dispH);
        glClearColor(0.11f, 0.12f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // 视差图背景 (随 Sidebar 宽度变化)
        if (state.viewMode == 1 && renderer.disparityTexture != 0) {
            float viewX = state.sidebarWidth;
            float viewW = (float)dispW - state.sidebarWidth;
            float viewH = (float)dispH - logHeight;

            ImGui::SetNextWindowPos(ImVec2(viewX, 0));
            ImGui::SetNextWindowSize(ImVec2(viewW, viewH));
            ImGui::Begin("DispImg", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus);
            ImGui::Image((ImTextureID)(intptr_t)renderer.disparityTexture, ImGui::GetContentRegionAvail());
            ImGui::End();
        }

        if (state.viewMode == 0) renderer.render3D(processor.pointCloud.size());

        ImGui::Render(); ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(renderer.window);
    }

    ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplGlfw_Shutdown(); ImGui::DestroyContext(); glfwTerminate();
    return 0;
}
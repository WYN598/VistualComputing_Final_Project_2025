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

// --- Log System ---
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

// Global Log Instance
AppLog g_Log;
void LogCallbackFunc(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    char buf[1024]; vsnprintf(buf, 1024, fmt, args); va_end(args);
    g_Log.AddLog("%s\n", buf);
}

// --- Global App State ---
struct AppState {
    std::string pathL, pathR;
    StereoParams params;
    bool isProcessing = false;
    bool processSuccess = false;
    int viewMode = 0; // 0: 3D View, 1: Disparity View
    float sidebarWidth = 380.0f;
};

// --- File Dialog Helper ---
std::string OpenFileDialog(bool save = false) {
    OPENFILENAMEA ofn; char szFile[260] = { 0 }; ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = NULL; ofn.lpstrFile = szFile; ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = save ? "PLY Point Cloud\0*.ply\0" : "Image Files\0*.jpg;*.png;*.bmp;*.jpeg\0All\0*.*\0";
    ofn.nFilterIndex = 1; ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
    if (save) ofn.Flags = OFN_OVERWRITEPROMPT;
    if ((save ? GetSaveFileNameA(&ofn) : GetOpenFileNameA(&ofn)) == TRUE) return std::string(ofn.lpstrFile);
    return "";
}

// --- UI Styling ---
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

// --- UI Helpers ---
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

void BulletTextWrapped(const char* text) {
    ImGui::Bullet();
    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + ImGui::GetContentRegionAvail().x);
    ImGui::Text(text);
    ImGui::PopTextWrapPos();
}

// ====================================================================================
// MAIN FUNCTION
// ====================================================================================
int main(int argc, char** argv) {
    // 1. Init OpenGL/GLFW
    GLRenderer renderer;
    if (!renderer.init()) return -1;

    // 2. Init ImGui
    IMGUI_CHECKVERSION(); ImGui::CreateContext(); ImGuiIO& io = ImGui::GetIO(); (void)io;
    SetupProfessionalStyle();

    // Load Fonts (Try Segoe UI, fallback to default)
    ImFont* font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\segoeui.ttf", 18.0f);
    if (!font) io.Fonts->AddFontDefault();

    ImGui_ImplGlfw_InitForOpenGL(renderer.window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // 3. Core Objects
    StereoProcessor processor;
    processor.setLogger(LogCallbackFunc);
    AppState state;
    std::future<bool> futureResult;

    float logHeight = 250.0f;

    g_Log.AddLog("[System] Ready. Load images to start.\n");

    // 4. Main Loop
    while (!glfwWindowShouldClose(renderer.window)) {
        glfwPollEvents();

        // Start ImGui Frame (Updates Inputs)
        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();

        // ---------------------------------------------------------
        // Interaction Logic (Input Handling)
        // ---------------------------------------------------------
        if (!io.WantCaptureMouse) {
            double x, y; glfwGetCursorPos(renderer.window, &x, &y);

            // Zoom (Scroll)
            if (io.MouseWheel != 0.0f) renderer.camera.processZoom((float)io.MouseWheel);

            // Rotate (Left Mouse)
            if (glfwGetMouseButton(renderer.window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
                if (!renderer.isRotateDragging) { renderer.isRotateDragging = true; renderer.lastX = x; renderer.lastY = y; }
                else { renderer.camera.processRotate((float)(x - renderer.lastX), (float)(y - renderer.lastY)); renderer.lastX = x; renderer.lastY = y; }
            }
            else renderer.isRotateDragging = false;

            // Pan (Right Mouse)
            if (glfwGetMouseButton(renderer.window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
                if (!renderer.isPanDragging) { renderer.isPanDragging = true; renderer.lastX = x; renderer.lastY = y; }
                else { renderer.camera.processPan((float)(x - renderer.lastX), (float)(y - renderer.lastY)); renderer.lastX = x; renderer.lastY = y; }
            }
            else renderer.isPanDragging = false;
        }

        int dispW, dispH; glfwGetFramebufferSize(renderer.window, &dispW, &dispH);

        // ---------------------------------------------------------
        // Async Task Check
        // ---------------------------------------------------------
        if (state.isProcessing && futureResult.valid()) {
            if (futureResult.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                state.processSuccess = futureResult.get();
                state.isProcessing = false;

                if (state.processSuccess) {
                    renderer.uploadGeometry(processor.pointCloud);
                    renderer.uploadTexture(processor.disparityVis);
                    state.viewMode = 0;

                    // Auto Focus
                    if (!processor.pointCloud.empty()) {
                        glm::vec3 centroid(0.0f);
                        for (const auto& v : processor.pointCloud) centroid += v.position;
                        centroid /= (float)processor.pointCloud.size();
                        renderer.camera.Target = centroid;
                        renderer.camera.Distance = std::abs(centroid.z) * 0.6f;
                        if (renderer.camera.Distance < 50.0f) renderer.camera.Distance = 500.0f;
                        g_Log.AddLog("[Camera] Auto-focused on object center.\n");
                    }
                }
            }
        }

        // ---------------------------------------------------------
        // Modal Loading Window
        // ---------------------------------------------------------
        if (state.isProcessing) ImGui::OpenPopup("Processing");
        if (ImGui::BeginPopupModal("Processing", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
            ImGui::Text("Processing Pipeline Running..."); ImGui::Separator();
            ImGui::TextColored(ImVec4(1, 1, 0, 1), "Please Wait...");
            if (!state.isProcessing) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // =========================================================
        // UI Layout
        // =========================================================

        // 1. Sidebar (Left)
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(state.sidebarWidth, (float)dispH), ImGuiCond_FirstUseEver);
        ImGui::Begin("Controls", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

        state.sidebarWidth = ImGui::GetWindowWidth();

        // User Guide
        if (ImGui::CollapsingHeader("USER GUIDE & HELP", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
            ImGui::TextWrapped("How to Navigate:");
            BulletTextWrapped("Rotate: Left Click + Drag");
            BulletTextWrapped("Pan: Right Click + Drag");
            BulletTextWrapped("Zoom: Scroll Wheel");
            ImGui::Spacing();
            ImGui::TextWrapped("Tips:");
            BulletTextWrapped("Artroom: Use WLS, small block size (5-7), min disp 90.");
            BulletTextWrapped("Skiboots: Min disp 57, block size 15+.");
            ImGui::PopStyleColor();
            ImGui::Separator();
        }

        // 1. Data Source
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.4f, 0.6f, 1.0f, 1.0f), "1. DATA SOURCE"); ImGui::Separator(); ImGui::Spacing();
        if (ImGui::Button(state.pathL.empty() ? "Left Img" : "Left OK", ImVec2(120, 80))) {
            std::string p = OpenFileDialog(); if (!p.empty()) { state.pathL = p; g_Log.AddLog("Left: %s\n", p.c_str()); }
        }
        ImGui::SameLine();
        if (ImGui::Button(state.pathR.empty() ? "Right Img" : "Right OK", ImVec2(120, 80))) {
            std::string p = OpenFileDialog(); if (!p.empty()) { state.pathR = p; g_Log.AddLog("Right: %s\n", p.c_str()); }
        }

        // 2. Camera Calibration
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), "2. CAMERA CALIBRATION"); ImGui::Separator();
        ImGui::Checkbox("Use Manual Calibration", &state.params.useCalibration);
        ImGui::SameLine(); HelpMarker("Check this if you have rectified images and known camera parameters (e.g., Middlebury).");

        if (state.params.useCalibration) {
            ImGui::BeginDisabled(false);
            ImGui::InputFloat("Focal Length (px)", &state.params.focalLength);
            ImGui::InputFloat("Baseline (mm)", &state.params.baseline);
            ImGui::InputFloat("Principal X", &state.params.principalX);
            ImGui::InputFloat("Principal Y", &state.params.principalY);
            ImGui::TextDisabled("(Set 0 for CX/CY to use center)");
            ImGui::EndDisabled();
        }
        else {
            ImGui::TextDisabled("[Auto Mode Active]");
            ImGui::TextWrapped("System will auto-rectify images and use fake parameters.");
        }

        // 3. Algorithm Settings
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.8f, 0.4f, 0.8f, 1.0f), "3. ALGORITHM SETTINGS"); ImGui::Separator();

        // [New] WLS Filter Toggle
        ImGui::Checkbox("Use WLS Filter (Pro)", &state.params.useWLS);
        ImGui::SameLine(); HelpMarker("Weighted Least Squares Filter.\nFill holes and smooth surfaces based on color edges.");

        if (state.params.useWLS) {
            ImGui::Indent();
            int lambda = (int)state.params.wlsLambda;
            if (ImGui::SliderInt("WLS Lambda", &lambda, 500, 12000)) state.params.wlsLambda = (double)lambda;

            float sigma = (float)state.params.wlsSigma;
            if (ImGui::SliderFloat("WLS Sigma", &sigma, 0.5f, 3.0f)) state.params.wlsSigma = (double)sigma;
            ImGui::Unindent();
        }

        ImGui::Text("Processing Scale");
        if (ImGui::RadioButton("Fast (50%)", state.params.processScale == 0.5f)) state.params.processScale = 0.5f; ImGui::SameLine();
        if (ImGui::RadioButton("High (100%)", state.params.processScale == 1.0f)) state.params.processScale = 1.0f;

        ImGui::Text("Disparities"); ImGui::SameLine(); HelpMarker("Max search range (must be > max_disp in calib.txt).");
        ImGui::SliderInt("##num", &state.params.numDisparities, 16, 400); state.params.numDisparities = (state.params.numDisparities / 16) * 16;

        ImGui::Text("Block Size"); ImGui::SameLine(); HelpMarker("Window size. Small=Detail(Artroom), Large=Smooth(Skiboots).");
        ImGui::SliderInt("##blk", &state.params.blockSize, 5, 31); if (state.params.blockSize % 2 == 0) state.params.blockSize++;

        ImGui::Text("Min Disp"); ImGui::SameLine(); HelpMarker("Minimum disparity (vmin in calib.txt).");
        // [FIX] Increased range from -20~20 to 0~300 to accommodate real datasets
        ImGui::SliderInt("##min", &state.params.minDisparity, 0, 300);

        ImGui::Text("Noise Filter"); ImGui::SameLine(); HelpMarker("Uniqueness Ratio. Lower = More points (noisier). Higher = Cleaner (less points).");
        ImGui::SliderInt("##uniq", &state.params.uniquenessRatio, 0, 20);

        // 4. Visualization
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), "4. VISUALIZATION"); ImGui::Separator();
        ImGui::Text("Point Size");
        ImGui::SliderFloat("##pt", &renderer.pointSize, 1.0f, 10.0f);

        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

        // Run Button
        if (ImGui::Button("RUN RECONSTRUCTION", ImVec2(-1, 50)) && !state.isProcessing) {
            if (state.pathL.empty() || state.pathR.empty()) g_Log.AddLog("[Error] Missing images!\n");
            else {
                state.isProcessing = true;
                g_Log.AddLog("Starting reconstruction job...\n");
                futureResult = std::async(std::launch::async,
                    [&processor, pathL = state.pathL, pathR = state.pathR, params = state.params]() {
                    return processor.process(pathL, pathR, params);
                }
                );
            }
        }
        // Export Button
        if (ImGui::Button("EXPORT RESULT (.PLY)", ImVec2(-1, 30))) {
            if (processor.pointCloud.empty()) g_Log.AddLog("[Error] Nothing to export.\n");
            else {
                std::string p = OpenFileDialog(true);
                if (!p.empty()) {
                    if (p.find(".ply") == std::string::npos) p += ".ply";
                    processor.saveToPLY(p);
                    g_Log.AddLog("Exported to: %s\n", p.c_str());
                }
            }
        }
        ImGui::End(); // End Sidebar

        // 2. Console (Bottom, Resizable)
        if (logHeight < 50.0f) logHeight = 50.0f;
        if (logHeight > dispH * 0.6f) logHeight = dispH * 0.6f;

        float consoleY = (float)dispH - logHeight;
        float consoleW = (float)dispW - state.sidebarWidth;

        ImGui::SetNextWindowPos(ImVec2(state.sidebarWidth, consoleY - 4.0f));
        ImGui::SetNextWindowSize(ImVec2(consoleW, 4.0f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
        ImGui::Begin("Splitter", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
        ImGui::PopStyleColor();
        ImGui::Button("##splitBtn", ImVec2(-1, -1));
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (ImGui::IsItemActive()) logHeight -= ImGui::GetIO().MouseDelta.y;
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(state.sidebarWidth, consoleY));
        ImGui::SetNextWindowSize(ImVec2(consoleW, logHeight));
        ImGui::Begin("Console Output", NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
        g_Log.DrawContent();
        ImGui::End();

        // 3. View Switcher (Floating)
        ImGui::SetNextWindowPos(ImVec2((float)dispW - 220, 20));
        ImGui::SetNextWindowSize(ImVec2(200, 0));
        ImGui::Begin("ViewMode", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground);

        bool is3D = (state.viewMode == 0);
        if (is3D) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1));
        if (ImGui::Button("3D View", ImVec2(90, 30))) state.viewMode = 0;
        if (is3D) ImGui::PopStyleColor();

        ImGui::SameLine();

        bool isDisp = (state.viewMode == 1);
        if (isDisp) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1));
        if (ImGui::Button("Disparity", ImVec2(90, 30))) state.viewMode = 1;
        if (isDisp) ImGui::PopStyleColor();

        ImGui::End();

        // ---------------------------------------------------------
        // Rendering
        // ---------------------------------------------------------
        glViewport(0, 0, dispW, dispH);
        glClearColor(0.11f, 0.12f, 0.15f, 1.0f); // Dark Background
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Layer 1: Disparity (Background)
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

        // Layer 2: 3D Scene
        if (state.viewMode == 0) renderer.render3D(processor.pointCloud.size());

        // Layer 3: UI Overlay
        ImGui::Render(); ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(renderer.window);
    }

    ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplGlfw_Shutdown(); ImGui::DestroyContext(); glfwTerminate();
    return 0;
}
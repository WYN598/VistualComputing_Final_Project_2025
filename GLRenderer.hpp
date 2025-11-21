#pragma once
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <opencv2/opencv.hpp>
#include "StereoProcessor.hpp"

// === Shaders ===
const char* vShaderSrc = R"(
    #version 330 core
    layout (location = 0) in vec3 aPos;
    layout (location = 1) in vec3 aColor;
    out vec3 vColor;
    uniform mat4 MVP;
    void main() { gl_Position = MVP * vec4(aPos, 1.0); vColor = aColor; }
)";
const char* fShaderSrc = R"(
    #version 330 core
    in vec3 vColor; out vec4 FragColor;
    void main() { FragColor = vec4(vColor, 1.0); }
)";

// === 专业轨道相机类 (Orbit Camera) ===
class Camera {
public:
    glm::vec3 Target = glm::vec3(0.0f); // 观察目标点
    float Distance = 1000.0f;           // 距离
    float Yaw = 0.0f;
    float Pitch = -30.0f;

    // 获取观察矩阵
    glm::mat4 getViewMatrix() {
        float radYaw = glm::radians(Yaw);
        float radPitch = glm::radians(Pitch);

        // 球坐标转笛卡尔坐标
        float camX = Target.x + Distance * sin(radYaw) * cos(radPitch);
        float camY = Target.y + Distance * sin(radPitch);
        float camZ = Target.z + Distance * cos(radYaw) * cos(radPitch);

        glm::vec3 pos(camX, camY, camZ);
        glm::vec3 up(0.0f, 1.0f, 0.0f);
        return glm::lookAt(pos, Target, up);
    }

    // [旋转] 左键拖动
    void processRotate(float dx, float dy) {
        Yaw += dx * 0.3f;
        Pitch -= dy * 0.3f;
        if (Pitch > 89.0f) Pitch = 89.0f;
        if (Pitch < -89.0f) Pitch = -89.0f;
    }

    // [缩放] 滚轮
    void processZoom(float dy) {
        Distance -= dy * 100.0f;
        if (Distance < 10.0f) Distance = 10.0f;
    }

    // [平移] 右键拖动 (新增)
    // 根据摄像机当前的朝向，计算出“屏幕平面”的右向量和上向量，实现抓手平移效果
    void processPan(float dx, float dy) {
        float speed = Distance * 0.001f; // 距离越远，移动越快，保持手感一致

        float radYaw = glm::radians(Yaw);
        float radPitch = glm::radians(Pitch);

        // 计算相机的前方向量 (Front)
        glm::vec3 front;
        front.x = sin(radYaw) * cos(radPitch);
        front.y = sin(radPitch);
        front.z = cos(radYaw) * cos(radPitch);
        front = glm::normalize(front);

        // 计算右向量 (Right) 和 上向量 (Real Up)
        glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
        glm::vec3 right = glm::normalize(glm::cross(front, worldUp));
        glm::vec3 up = glm::normalize(glm::cross(right, front));

        // 移动目标点 (反向移动实现“抓手”效果：鼠标往左，物体往左=相机往右)
        Target -= right * dx * speed;
        Target += up * dy * speed;
    }
};

class GLRenderer {
public:
    GLFWwindow* window;
    GLuint prog;
    GLuint VAO_P, VBO_P; // Point Cloud
    GLuint VAO_G, VBO_G; // Grid
    GLuint disparityTexture = 0;
    int width = 1600, height = 900;

    Camera camera;

    // 交互状态管理
    bool isRotateDragging = false;
    bool isPanDragging = false;
    double lastX = 0, lastY = 0;

    int gridVertexCount = 0;

    bool init() {
        if (!glfwInit()) return false;
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_SAMPLES, 4);

        window = glfwCreateWindow(width, height, "Pro Stereo Reconstruction", NULL, NULL);
        if (!window) return false;
        glfwMakeContextCurrent(window);
        if (glewInit() != GLEW_OK) return false;

        glEnable(GL_DEPTH_TEST);
        glEnable(GL_PROGRAM_POINT_SIZE);
        glEnable(GL_MULTISAMPLE);

        // Shader
        GLuint vs = glCreateShader(GL_VERTEX_SHADER); glShaderSource(vs, 1, &vShaderSrc, 0); glCompileShader(vs);
        GLuint fs = glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(fs, 1, &fShaderSrc, 0); glCompileShader(fs);
        prog = glCreateProgram(); glAttachShader(prog, vs); glAttachShader(prog, fs); glLinkProgram(prog);

        // Buffer
        glGenVertexArrays(1, &VAO_P); glGenBuffers(1, &VBO_P);

        initGridAxes();
        return true;
    }

    void initGridAxes() {
        std::vector<Vertex> lines;
        float size = 2000.0f; float step = 200.0f;
        glm::vec3 gray(0.25f); glm::vec3 dark(0.15f);

        for (float i = -size; i <= size; i += step) {
            glm::vec3 c = (abs(i) < 0.1f) ? glm::vec3(0) : ((int)i % (int)(step * 5) == 0 ? gray : dark);
            if (abs(i) < 0.1f) continue;
            lines.push_back({ glm::vec3(i, 0, -size), c }); lines.push_back({ glm::vec3(i, 0, size), c });
            lines.push_back({ glm::vec3(-size, 0, i), c }); lines.push_back({ glm::vec3(size, 0, i), c });
        }
        // Axes
        lines.push_back({ glm::vec3(-size, 0, 0), glm::vec3(0.8f, 0.2f, 0.2f) }); lines.push_back({ glm::vec3(size, 0, 0), glm::vec3(0.8f, 0.2f, 0.2f) });
        lines.push_back({ glm::vec3(0, 0, -size), glm::vec3(0.2f, 0.2f, 0.8f) }); lines.push_back({ glm::vec3(0, 0, size), glm::vec3(0.2f, 0.2f, 0.8f) });
        lines.push_back({ glm::vec3(0, -size, 0), glm::vec3(0.2f, 0.8f, 0.2f) }); lines.push_back({ glm::vec3(0, size, 0), glm::vec3(0.2f, 0.8f, 0.2f) });

        gridVertexCount = (int)lines.size();
        glGenVertexArrays(1, &VAO_G); glGenBuffers(1, &VBO_G);
        glBindVertexArray(VAO_G); glBindBuffer(GL_ARRAY_BUFFER, VBO_G);
        glBufferData(GL_ARRAY_BUFFER, lines.size() * sizeof(Vertex), lines.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), 0); glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, color)); glEnableVertexAttribArray(1);
    }

    void uploadGeometry(const std::vector<Vertex>& cloud) {
        if (cloud.empty()) return;
        glBindVertexArray(VAO_P); glBindBuffer(GL_ARRAY_BUFFER, VBO_P);
        glBufferData(GL_ARRAY_BUFFER, cloud.size() * sizeof(Vertex), cloud.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), 0); glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, color)); glEnableVertexAttribArray(1);
    }

    void uploadTexture(const cv::Mat& img) {
        if (img.empty()) return;
        if (disparityTexture == 0) glGenTextures(1, &disparityTexture);
        glBindTexture(GL_TEXTURE_2D, disparityTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        cv::Mat rgb; img.copyTo(rgb);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, rgb.cols, rgb.rows, 0, GL_BGR, GL_UNSIGNED_BYTE, rgb.data);
    }

    void render3D(size_t pointCount) {
        int dw, dh; glfwGetFramebufferSize(window, &dw, &dh); glViewport(0, 0, dw, dh);
        glm::mat4 proj = glm::perspective(glm::radians(45.0f), (float)dw / dh, 1.0f, 10000.0f);
        glm::mat4 view = camera.getViewMatrix();
        glm::mat4 MVP = proj * view;

        glUseProgram(prog);
        glUniformMatrix4fv(glGetUniformLocation(prog, "MVP"), 1, GL_FALSE, &MVP[0][0]);

        glBindVertexArray(VAO_G); glLineWidth(1.0f); glDrawArrays(GL_LINES, 0, gridVertexCount); // Grid
        if (pointCount > 0) { glBindVertexArray(VAO_P); glPointSize(2.0f); glDrawArrays(GL_POINTS, 0, (GLsizei)pointCount); } // Points
    }
};
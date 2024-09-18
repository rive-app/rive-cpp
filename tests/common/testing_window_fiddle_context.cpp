/*
 * Copyright 2022 Rive
 */

#include "testing_window.hpp"

#if defined(TESTING) || defined(RIVE_TOOLS_NO_GLFW)

std::unique_ptr<TestingWindow> TestingWindow::MakeFiddleContext(Backend,
                                                                Visibility,
                                                                const char*,
                                                                void* platformWindow)
{
    return nullptr;
}

#else

#include "fiddle_context.hpp"

#include <deque>

#define GLFW_INCLUDE_NONE
#define GLFW_NATIVE_INCLUDE_NONE
#include "GLFW/glfw3.h"
#include <GLFW/glfw3native.h>

using namespace rive;
using namespace rive::gpu;

static std::deque<char> s_keys;

static void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods)
{
    if (action == GLFW_PRESS)
    {
        if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z)
        {
            if (!(mods & GLFW_MOD_SHIFT))
            {
                key += 'a' - 'A';
            }
        }
        else if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9)
        {
            if (mods & GLFW_MOD_SHIFT)
            {
                switch (key)
                {
                        // clang-format off
                    case GLFW_KEY_1: key = '!'; break;
                    case GLFW_KEY_2: key = '@'; break;
                    case GLFW_KEY_3: key = '#'; break;
                    case GLFW_KEY_4: key = '$'; break;
                    case GLFW_KEY_5: key = '%'; break;
                    case GLFW_KEY_6: key = '^'; break;
                    case GLFW_KEY_7: key = '&'; break;
                    case GLFW_KEY_8: key = '*'; break;
                    case GLFW_KEY_9: key = '('; break;
                    case GLFW_KEY_0: key = ')'; break;
                        // clang-format on
                }
            }
        }
        if (key < 128)
        {
            s_keys.push_back(static_cast<char>(key));
            return;
        }
        switch (key)
        {
            case GLFW_KEY_ESCAPE:
                glfwSetWindowShouldClose(window, GLFW_TRUE);
                break;
            case GLFW_KEY_TAB:
                s_keys.push_back('\t');
                break;
            case GLFW_KEY_BACKSPACE:
                s_keys.push_back('\b');
                break;
        }
    }
}

class TestingWindowFiddleContext : public TestingWindow
{
public:
    TestingWindowFiddleContext(Backend backend,
                               Visibility visibility,
                               const char* gpuNameFilter,
                               void* platformWindow)
    {
        // Vulkan headless rendering doesn't need an OS window.
        // It's convenient to run Swiftshader on CI without GLFW.
        if (IsANGLE(backend))
        {
#ifdef __APPLE__
            glfwInitHint(GLFW_ANGLE_PLATFORM_TYPE, GLFW_ANGLE_PLATFORM_TYPE_METAL);
#elif defined(_WIN32)
            glfwInitHint(GLFW_ANGLE_PLATFORM_TYPE, GLFW_ANGLE_PLATFORM_TYPE_D3D11);
#else
            glfwInitHint(GLFW_ANGLE_PLATFORM_TYPE, GLFW_ANGLE_PLATFORM_TYPE_VULKAN);
#endif
        }
        if (!glfwInit())
        {
            fprintf(stderr, "Failed to initialize GLFW.\n");
            abort();
        }

        if (!IsGL(backend))
        {
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            glfwWindowHint(GLFW_SAMPLES, 0);
            glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
        }
        else if (IsANGLE(backend))
        {
            glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
            glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
        }
        else
        {
            glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
        }
        if (IsMSAA(backend))
        {
            m_msaaSampleCount = 4;
            glfwWindowHint(GLFW_SAMPLES, m_msaaSampleCount);
            glfwWindowHint(GLFW_STENCIL_BITS, 8);
            glfwWindowHint(GLFW_DEPTH_BITS, 24);
        }
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
        if (visibility == Visibility::headless)
        {
            glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        }

        GLFWmonitor* fullscreenMonitor = nullptr;
        if (visibility == Visibility::fullscreen)
        {
            glfwWindowHint(GLFW_CENTER_CURSOR, GLFW_FALSE);

            // For GLFW, the platformWindow is a fullscreen monitor index.
            intptr_t fullscreenMonitorIdx = reinterpret_cast<intptr_t>(platformWindow);

            int monitorCount;
            GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
            if (fullscreenMonitorIdx < 0 || fullscreenMonitorIdx >= monitorCount)
            {
                fprintf(stderr,
                        "Monitor index out of range. Requested %zi but %i are available.",
                        fullscreenMonitorIdx,
                        monitorCount);
                abort();
            }
            fullscreenMonitor = monitors[fullscreenMonitorIdx];
        }

        // GLFW doesn't appear to have a way to say "make the window as big as the
        // screen", but if we don't make it large enough here, the fullscreen window
        // will be quarter resultion instead of full.
        // NOTE: glfwGetMonitorPhysicalSize() returns bogus values.
        m_glfwWindow = glfwCreateWindow(9999, 9999, "Rive Renderer", fullscreenMonitor, nullptr);
        if (!m_glfwWindow)
        {
            glfwTerminate();
            fprintf(stderr, "Failed to create GLFW m_glfwWindow.\n");
            abort();
        }
        glfwMakeContextCurrent(m_glfwWindow);
        glfwSwapInterval(0);

        glfwSetKeyCallback(m_glfwWindow, key_callback);

        FiddleContextOptions fiddleOptions = {
            .retinaDisplay = visibility == Visibility::fullscreen,
            .synchronousShaderCompilations = true,
            .enableReadPixels = true,
            .disableRasterOrdering = IsAtomic(backend),
            .coreFeaturesOnly = IsCore(backend),
            .allowHeadlessRendering = visibility == Visibility::headless,
            .enableVulkanValidationLayers = true,
            .gpuNameFilter = gpuNameFilter,
        };

        switch (backend)
        {
            case Backend::coregraphics:
                break;
            case Backend::gl:
            case Backend::glatomic:
            case Backend::glmsaa:
            case Backend::angle:
            case Backend::anglemsaa:
                m_fiddleContext = FiddleContext::MakeGLPLS(fiddleOptions);
                break;
            case Backend::d3d:
            case Backend::d3datomic:
                m_fiddleContext = FiddleContext::MakeD3DPLS(fiddleOptions);
                break;
            case Backend::metal:
            case Backend::metalatomic:
                m_fiddleContext = FiddleContext::MakeMetalPLS(fiddleOptions);
                break;
            case Backend::vk:
            case Backend::vkcore:
            case Backend::moltenvk:
            case Backend::moltenvkcore:
            case Backend::swiftshader:
            case Backend::swiftshadercore:
                m_fiddleContext = FiddleContext::MakeVulkanPLS(fiddleOptions);
                break;
            case Backend::dawn:
                m_fiddleContext = FiddleContext::MakeDawnPLS(fiddleOptions);
                break;
        }

        if (m_fiddleContext == nullptr)
        {
            fprintf(stderr,
                    "error: unable to create FiddleContext for %s.\n",
                    BackendName(backend));
            abort();
        }
        // On Mac we need to call glfwSetWindowSize even though we created the
        // window with these same dimensions.
        int w, h;
        glfwGetFramebufferSize(m_glfwWindow, &w, &h);
        m_width = w;
        m_height = h;
        m_fiddleContext->onSizeChanged(m_glfwWindow, m_width, m_height, 0);
    }

    ~TestingWindowFiddleContext() override
    {
        m_fiddleContext.reset();
        if (m_glfwWindow != nullptr)
        {
            glfwPollEvents();
            glfwDestroyWindow(m_glfwWindow);
            glfwTerminate();
        }
    }

    rive::Factory* factory() override { return m_fiddleContext->factory(); }

    void resize(int width, int height) override
    {
        if (width != m_width || height != m_height)
        {
            glfwSetWindowSize(m_glfwWindow, width, height);
            m_fiddleContext->onSizeChanged(m_glfwWindow, width, height, 0);
            m_width = width;
            m_height = height;
        }
    }

    std::unique_ptr<rive::Renderer> beginFrame(uint32_t clearColor, bool doClear) override
    {
        rive::gpu::RenderContext::FrameDescriptor frameDescriptor = {
            .renderTargetWidth = static_cast<uint32_t>(m_width),
            .renderTargetHeight = static_cast<uint32_t>(m_height),
            .loadAction = doClear ? rive::gpu::LoadAction::clear
                                  : rive::gpu::LoadAction::preserveRenderTarget,
            .clearColor = clearColor,
            .msaaSampleCount = m_msaaSampleCount,
        };
        m_fiddleContext->begin(std::move(frameDescriptor));
        return m_fiddleContext->makeRenderer(m_width, m_height);
    }

    void endFrame(std::vector<uint8_t>* pixelData) override
    {
        m_fiddleContext->end(m_glfwWindow, pixelData);
        m_fiddleContext->tick();
        glfwPollEvents();
        glfwSwapBuffers(m_glfwWindow);
    }

    rive::gpu::RenderContext* renderContext() const override
    {
        return m_fiddleContext->renderContextOrNull();
    }

    rive::gpu::RenderTarget* renderTarget() const override
    {
        return m_fiddleContext->renderTargetOrNull();
    }

    void flushPLSContext() override { m_fiddleContext->flushPLSContext(); }

    bool peekKey(char& key) override
    {
        if (s_keys.empty())
        {
            return false;
        }
        key = s_keys.front();
        s_keys.pop_front();
        return true;
    }

    char getKey() override
    {
        char key;
        while (!peekKey(key))
        {
            glfwWaitEvents();
            if (glfwWindowShouldClose(m_glfwWindow))
            {
                printf("Window terminated by user.\n");
                exit(0);
            }
        }
        return key;
    }

    bool shouldQuit() const override { return glfwWindowShouldClose(m_glfwWindow); }

private:
    GLFWwindow* m_glfwWindow = nullptr;
    int m_msaaSampleCount = 0;
    std::unique_ptr<FiddleContext> m_fiddleContext;
};

std::unique_ptr<TestingWindow> TestingWindow::MakeFiddleContext(Backend backend,
                                                                Visibility visibility,
                                                                const char* gpuNameFilter,
                                                                void* platformWindow)
{
    return std::make_unique<TestingWindowFiddleContext>(backend,
                                                        visibility,
                                                        gpuNameFilter,
                                                        platformWindow);
}

#endif

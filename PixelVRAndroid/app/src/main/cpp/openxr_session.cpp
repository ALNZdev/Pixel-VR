
#include <jni.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <android/log.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace
{
    constexpr char kTag[] = "PixelVR-OpenXR";
    constexpr uint32_t kEyeCount = 2;

    void LogError(const char* message, XrResult result = XR_SUCCESS)
    {
        if (result == XR_SUCCESS)
            __android_log_print(ANDROID_LOG_ERROR, kTag, "%s", message);
        else
            __android_log_print(ANDROID_LOG_ERROR, kTag, "%s (OpenXR result %d)", message, result);
    }

    GLuint CompileShader(GLenum type, const char* source)
    {
        const GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);

        GLint compiled = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (compiled != GL_TRUE)
        {
            GLchar message[1024]{};
            glGetShaderInfoLog(shader, sizeof(message), nullptr, message);
            __android_log_print(ANDROID_LOG_ERROR, kTag, "Shader compile failed: %s", message);
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    }

    GLuint CreateBlitProgram()
    {
        static constexpr char vertexSource[] = R"(
            #version 300 es
            layout(location = 0) in vec2 aPosition;
            layout(location = 1) in vec2 aUv;
            out vec2 vUv;
            void main() {
                gl_Position = vec4(aPosition, 0.0, 1.0);
                vUv = aUv;
            }
        )";

        static constexpr char fragmentSource[] = R"(
            #version 300 es
            #extension GL_OES_EGL_image_external_essl3 : require
            precision highp float;
            uniform samplerExternalOES uSbs;
            uniform mat4 uTextureMatrix;
            uniform float uEye;
            uniform vec4 uSourceBounds;
            uniform vec4 uDestinationBounds;
            in vec2 vUv;
            out vec4 outColor;
            void main() {
                float rayX = mix(uDestinationBounds.x, uDestinationBounds.y, vUv.x);
                float rayY = mix(uDestinationBounds.w, uDestinationBounds.z, vUv.y);
                float sourceU = (rayX - uSourceBounds.x) / (uSourceBounds.y - uSourceBounds.x);
                float sourceV = (rayY - uSourceBounds.w) / (uSourceBounds.z - uSourceBounds.w);
                if (sourceU < 0.0 || sourceU > 1.0 || sourceV < 0.0 || sourceV > 1.0) {
                    outColor = vec4(0.0, 0.0, 0.0, 1.0);
                    return;
                }
                vec2 eyeUv = vec2((sourceU + uEye) * 0.5, sourceV);
                vec2 sourceUv = (uTextureMatrix * vec4(eyeUv, 0.0, 1.0)).xy;
                outColor = texture(uSbs, clamp(sourceUv, 0.0, 1.0));
            }
        )";

        const GLuint vertex = CompileShader(GL_VERTEX_SHADER, vertexSource);
        const GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, fragmentSource);
        if (vertex == 0 || fragment == 0)
        {
            if (vertex != 0) glDeleteShader(vertex);
            if (fragment != 0) glDeleteShader(fragment);
            return 0;
        }

        const GLuint program = glCreateProgram();
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);
        glLinkProgram(program);
        glDeleteShader(vertex);
        glDeleteShader(fragment);

        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE)
        {
            GLchar message[1024]{};
            glGetProgramInfoLog(program, sizeof(message), nullptr, message);
            __android_log_print(ANDROID_LOG_ERROR, kTag, "Shader link failed: %s", message);
            glDeleteProgram(program);
            return 0;
        }
        return program;
    }

    struct EyeSwapchain
    {
        XrSwapchain handle = XR_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<XrSwapchainImageOpenGLESKHR> images;
    };

    class OpenXrSession
    {
    public:
        bool Initialize(JNIEnv* env, jobject activity, EGLDisplay display, EGLConfig config, EGLContext context)
        {
            Shutdown(env);
            if (display == EGL_NO_DISPLAY || config == nullptr || context == EGL_NO_CONTEXT)
            {
                LogError("GLSurfaceView did not provide a current EGL display, config, and context");
                return false;
            }

            if (env->GetJavaVM(&javaVm_) != JNI_OK)
            {
                LogError("Could not get the Android Java VM");
                return false;
            }
            activity_ = env->NewGlobalRef(activity);
            if (activity_ == nullptr)
            {
                LogError("Could not keep the Android activity alive for OpenXR");
                javaVm_ = nullptr;
                return false;
            }

            PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
            XrResult result = xrGetInstanceProcAddr(
                XR_NULL_HANDLE,
                "xrInitializeLoaderKHR",
                reinterpret_cast<PFN_xrVoidFunction*>(&initializeLoader));
            if (XR_FAILED(result) || initializeLoader == nullptr)
            {
                LogError("OpenXR Android loader initialization function is unavailable", result);
                Shutdown(env);
                return false;
            }

            XrLoaderInitInfoAndroidKHR loaderInfo{ XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
            loaderInfo.applicationVM = javaVm_;
            loaderInfo.applicationContext = activity_;
            result = initializeLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loaderInfo));
            if (XR_FAILED(result))
            {
                LogError("OpenXR Android loader initialization failed", result);
                Shutdown(env);
                return false;
            }

            std::vector<const char*> extensions = {
                XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
                XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME
            };
            uint32_t extensionCount = 0;
            result = xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr);
            if (XR_FAILED(result))
            {
                LogError("Could not enumerate OpenXR instance extensions", result);
                Shutdown(env);
                return false;
            }
            std::vector<XrExtensionProperties> availableExtensions(extensionCount);
            for (auto& extension : availableExtensions)
                extension = { XR_TYPE_EXTENSION_PROPERTIES };
            result = xrEnumerateInstanceExtensionProperties(nullptr, extensionCount,
                &extensionCount, availableExtensions.data());
            if (XR_FAILED(result))
            {
                LogError("Could not read OpenXR instance extensions", result);
                Shutdown(env);
                return false;
            }
            refreshRateExtensionEnabled_ = std::any_of(availableExtensions.begin(),
                availableExtensions.end(), [](const XrExtensionProperties& extension) {
                    return std::strcmp(extension.extensionName,
                        XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME) == 0;
                });
            if (refreshRateExtensionEnabled_)
                extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);

            XrInstanceCreateInfoAndroidKHR androidCreateInfo{ XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
            androidCreateInfo.applicationVM = javaVm_;
            androidCreateInfo.applicationActivity = activity_;

            XrInstanceCreateInfo instanceInfo{ XR_TYPE_INSTANCE_CREATE_INFO };
            instanceInfo.next = &androidCreateInfo;
            instanceInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
            instanceInfo.enabledExtensionNames = extensions.data();
            instanceInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
            std::snprintf(instanceInfo.applicationInfo.applicationName,
                XR_MAX_APPLICATION_NAME_SIZE, "%s", "PixelVR Android");
            std::snprintf(instanceInfo.applicationInfo.engineName,
                XR_MAX_ENGINE_NAME_SIZE, "%s", "PixelVR");
            result = xrCreateInstance(&instanceInfo, &instance_);
            if (XR_FAILED(result))
            {
                LogError("xrCreateInstance failed; check that the Quest OpenXR runtime is available", result);
                Shutdown(env);
                return false;
            }

            XrSystemGetInfo systemInfo{ XR_TYPE_SYSTEM_GET_INFO };
            systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
            result = xrGetSystem(instance_, &systemInfo, &systemId_);
            if (XR_FAILED(result))
            {
                LogError("No OpenXR head-mounted display system was found", result);
                Shutdown(env);
                return false;
            }

            PFN_xrGetOpenGLESGraphicsRequirementsKHR getGraphicsRequirements = nullptr;
            result = xrGetInstanceProcAddr(instance_, "xrGetOpenGLESGraphicsRequirementsKHR",
                reinterpret_cast<PFN_xrVoidFunction*>(&getGraphicsRequirements));
            if (XR_FAILED(result) || getGraphicsRequirements == nullptr)
            {
                LogError("OpenXR GLES graphics requirements function is unavailable", result);
                Shutdown(env);
                return false;
            }

            XrGraphicsRequirementsOpenGLESKHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
            result = getGraphicsRequirements(instance_, systemId_, &graphicsRequirements);
            if (XR_FAILED(result))
            {
                LogError("Could not query OpenXR GLES requirements", result);
                Shutdown(env);
                return false;
            }

            EGLint clientVersion = 0;
            eglQueryContext(display, context, EGL_CONTEXT_CLIENT_VERSION, &clientVersion);
            if (clientVersion < static_cast<EGLint>(XR_VERSION_MAJOR(graphicsRequirements.minApiVersionSupported)))
            {
                LogError("GLSurfaceView EGL context is below the OpenXR runtime's minimum GLES version");
                Shutdown(env);
                return false;
            }

            XrGraphicsBindingOpenGLESAndroidKHR binding{ XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
            binding.display = display;
            binding.config = config;
            binding.context = context;

            XrSessionCreateInfo sessionInfo{ XR_TYPE_SESSION_CREATE_INFO };
            sessionInfo.next = &binding;
            sessionInfo.systemId = systemId_;
            result = xrCreateSession(instance_, &sessionInfo, &session_);
            if (XR_FAILED(result))
            {
                LogError("xrCreateSession failed", result);
                Shutdown(env);
                return false;
            }

            if (refreshRateExtensionEnabled_)
            {
                xrGetInstanceProcAddr(instance_, "xrEnumerateDisplayRefreshRatesFB",
                    reinterpret_cast<PFN_xrVoidFunction*>(&enumerateRefreshRates_));
                xrGetInstanceProcAddr(instance_, "xrRequestDisplayRefreshRateFB",
                    reinterpret_cast<PFN_xrVoidFunction*>(&requestRefreshRate_));
                if (enumerateRefreshRates_ != nullptr && requestRefreshRate_ != nullptr)
                {
                    uint32_t rateCount = 0;
                    if (XR_SUCCEEDED(enumerateRefreshRates_(session_, 0, &rateCount, nullptr)) && rateCount > 0)
                    {
                        supportedRefreshRates_.resize(rateCount);
                        if (XR_FAILED(enumerateRefreshRates_(session_, rateCount,
                            &rateCount, supportedRefreshRates_.data())))
                            supportedRefreshRates_.clear();
                    }
                }
            }

            XrReferenceSpaceCreateInfo spaceInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
            // Keep the streamed image fixed to the headset. PixelVR Driver supplies
            // the scene pose; Quest poses are used only for the OpenXR eye layer.
            spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
            spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
            result = xrCreateReferenceSpace(session_, &spaceInfo, &viewSpace_);
            if (XR_FAILED(result))
            {
                LogError("Could not create OpenXR local reference space", result);
                Shutdown(env);
                return false;
            }

            uint32_t viewCount = 0;
            result = xrEnumerateViewConfigurationViews(instance_, systemId_,
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
            if (XR_FAILED(result) || viewCount != kEyeCount)
            {
                LogError("Quest runtime did not report two primary stereo views", result);
                Shutdown(env);
                return false;
            }

            viewConfigurationViews_.assign(viewCount, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
            result = xrEnumerateViewConfigurationViews(instance_, systemId_,
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount,
                viewConfigurationViews_.data());
            if (XR_FAILED(result))
            {
                LogError("Could not query OpenXR eye dimensions", result);
                Shutdown(env);
                return false;
            }

            std::vector<int64_t> formats;
            uint32_t formatCount = 0;
            result = xrEnumerateSwapchainFormats(session_, 0, &formatCount, nullptr);
            if (XR_FAILED(result) || formatCount == 0)
            {
                LogError("OpenXR runtime reported no swapchain formats", result);
                Shutdown(env);
                return false;
            }
            formats.resize(formatCount);
            result = xrEnumerateSwapchainFormats(session_, formatCount, &formatCount, formats.data());
            if (XR_FAILED(result))
            {
                LogError("Could not enumerate OpenXR swapchain formats", result);
                Shutdown(env);
                return false;
            }

            const std::array<GLint, 3> preferredFormats = { GL_SRGB8_ALPHA8, GL_RGBA8, GL_RGB10_A2 };
            for (const GLint preferred : preferredFormats)
            {
                if (std::find(formats.begin(), formats.end(), preferred) != formats.end())
                {
                    swapchainFormat_ = preferred;
                    break;
                }
            }
            if (swapchainFormat_ == 0)
            {
                LogError("Runtime has no supported GLES color swapchain format");
                Shutdown(env);
                return false;
            }

            eyes_.resize(kEyeCount);
            for (uint32_t eye = 0; eye < kEyeCount; ++eye)
            {
                EyeSwapchain& target = eyes_[eye];
                const XrViewConfigurationView& view = viewConfigurationViews_[eye];
                target.width = view.recommendedImageRectWidth;
                target.height = view.recommendedImageRectHeight;

                XrSwapchainCreateInfo swapchainInfo{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
                swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
                swapchainInfo.format = swapchainFormat_;
                swapchainInfo.sampleCount = 1;
                swapchainInfo.width = target.width;
                swapchainInfo.height = target.height;
                swapchainInfo.faceCount = 1;
                swapchainInfo.arraySize = 1;
                swapchainInfo.mipCount = 1;
                result = xrCreateSwapchain(session_, &swapchainInfo, &target.handle);
                if (XR_FAILED(result))
                {
                    LogError("Could not create an OpenXR eye swapchain", result);
                    Shutdown(env);
                    return false;
                }

                uint32_t imageCount = 0;
                result = xrEnumerateSwapchainImages(target.handle, 0, &imageCount, nullptr);
                if (XR_FAILED(result) || imageCount == 0)
                {
                    LogError("Could not enumerate OpenXR eye swapchain images", result);
                    Shutdown(env);
                    return false;
                }
                target.images.resize(imageCount);
                for (auto& image : target.images)
                    image = { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR };
                result = xrEnumerateSwapchainImages(target.handle, imageCount, &imageCount,
                    reinterpret_cast<XrSwapchainImageBaseHeader*>(target.images.data()));
                if (XR_FAILED(result))
                {
                    LogError("Could not retrieve OpenXR eye textures", result);
                    Shutdown(env);
                    return false;
                }
            }

            blitProgram_ = CreateBlitProgram();
            if (blitProgram_ == 0)
            {
                LogError("Could not compile the SBS-to-eye OpenGL shader");
                Shutdown(env);
                return false;
            }
            samplerLocation_ = glGetUniformLocation(blitProgram_, "uSbs");
            matrixLocation_ = glGetUniformLocation(blitProgram_, "uTextureMatrix");
            eyeLocation_ = glGetUniformLocation(blitProgram_, "uEye");
            sourceBoundsLocation_ = glGetUniformLocation(blitProgram_, "uSourceBounds");
            destinationBoundsLocation_ = glGetUniformLocation(blitProgram_, "uDestinationBounds");
            glGenFramebuffers(1, &framebuffer_);
            static constexpr GLfloat quad[] = {
                -1.0f, -1.0f, 0.0f, 0.0f,
                 1.0f, -1.0f, 1.0f, 0.0f,
                -1.0f,  1.0f, 0.0f, 1.0f,
                 1.0f,  1.0f, 1.0f, 1.0f
            };
            glGenBuffers(1, &vertexBuffer_);
            glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer_);
            glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
            glBindBuffer(GL_ARRAY_BUFFER, 0);

            __android_log_print(ANDROID_LOG_INFO, kTag,
                "OpenXR initialized with two %ux%u eye swapchains",
                eyes_[0].width, eyes_[0].height);
            return true;
        }

        bool Render(GLuint sbsTexture, const GLfloat* textureMatrix,
            const GLfloat* sourceFovDegrees, uint32_t streamFrameRate)
        {
            if (instance_ == XR_NULL_HANDLE || session_ == XR_NULL_HANDLE)
                return false;

            PollEvents();
            if (!sessionRunning_)
                return false;

            RequestMatchingRefreshRate(streamFrameRate);

            XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
            XrFrameState frameState{ XR_TYPE_FRAME_STATE };
            XrResult result = xrWaitFrame(session_, &waitInfo, &frameState);
            if (XR_FAILED(result))
            {
                LogError("xrWaitFrame failed", result);
                return false;
            }

            XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
            result = xrBeginFrame(session_, &beginInfo);
            if (XR_FAILED(result))
            {
                LogError("xrBeginFrame failed", result);
                return false;
            }

            std::array<XrCompositionLayerProjectionView, kEyeCount> projectionViews{};
            bool rendered = false;
            if (frameState.shouldRender == XR_TRUE)
            {
                std::array<XrView, kEyeCount> locatedViews{};
                for (auto& view : locatedViews)
                    view = { XR_TYPE_VIEW };

                XrViewLocateInfo locateInfo{ XR_TYPE_VIEW_LOCATE_INFO };
                locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                locateInfo.displayTime = frameState.predictedDisplayTime;
                locateInfo.space = viewSpace_;
                XrViewState viewState{ XR_TYPE_VIEW_STATE };
                uint32_t locatedCount = 0;
                result = xrLocateViews(session_, &locateInfo, &viewState,
                    static_cast<uint32_t>(locatedViews.size()), &locatedCount, locatedViews.data());
                if (XR_SUCCEEDED(result) && locatedCount == kEyeCount &&
                    (viewState.viewStateFlags & (XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT)) ==
                        (XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT))
                {
                    rendered = true;
                    for (uint32_t eye = 0; eye < kEyeCount; ++eye)
                    {
                        if (!RenderEye(eye, sbsTexture, textureMatrix, sourceFovDegrees,
                            locatedViews[eye], projectionViews[eye]))
                        {
                            rendered = false;
                            break;
                        }
                    }
                }
            }

            const XrCompositionLayerProjection* projectionLayerPointer = nullptr;
            XrCompositionLayerProjection projectionLayer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
            if (rendered)
            {
                projectionLayer.space = viewSpace_;
                projectionLayer.viewCount = kEyeCount;
                projectionLayer.views = projectionViews.data();
                projectionLayerPointer = &projectionLayer;
            }

            XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
            endInfo.displayTime = frameState.predictedDisplayTime;
            endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            if (projectionLayerPointer != nullptr)
            {
                const XrCompositionLayerBaseHeader* layers[] = {
                    reinterpret_cast<const XrCompositionLayerBaseHeader*>(projectionLayerPointer)
                };
                endInfo.layerCount = 1;
                endInfo.layers = layers;
                result = xrEndFrame(session_, &endInfo);
            }
            else
            {
                result = xrEndFrame(session_, &endInfo);
            }
            if (XR_FAILED(result))
                LogError("xrEndFrame failed", result);
            return true;
        }

        void Shutdown(JNIEnv* env)
        {
            if (session_ != XR_NULL_HANDLE && sessionRunning_)
            {
                xrRequestExitSession(session_);
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                while (sessionRunning_ && std::chrono::steady_clock::now() < deadline)
                {
                    PollEvents();
                    if (sessionRunning_)
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }

            sessionRunning_ = false;
            if (framebuffer_ != 0)
                glDeleteFramebuffers(1, &framebuffer_);
            framebuffer_ = 0;
            if (vertexBuffer_ != 0)
                glDeleteBuffers(1, &vertexBuffer_);
            vertexBuffer_ = 0;
            if (blitProgram_ != 0)
                glDeleteProgram(blitProgram_);
            blitProgram_ = 0;
            samplerLocation_ = matrixLocation_ = eyeLocation_ = -1;
            sourceBoundsLocation_ = destinationBoundsLocation_ = -1;

            for (EyeSwapchain& eye : eyes_)
            {
                if (eye.handle != XR_NULL_HANDLE)
                    xrDestroySwapchain(eye.handle);
                eye = {};
            }
            eyes_.clear();
            viewConfigurationViews_.clear();

            if (viewSpace_ != XR_NULL_HANDLE)
                xrDestroySpace(viewSpace_);
            viewSpace_ = XR_NULL_HANDLE;
            if (session_ != XR_NULL_HANDLE)
                xrDestroySession(session_);
            session_ = XR_NULL_HANDLE;
            if (instance_ != XR_NULL_HANDLE)
                xrDestroyInstance(instance_);
            instance_ = XR_NULL_HANDLE;
            systemId_ = XR_NULL_SYSTEM_ID;
            swapchainFormat_ = 0;
            refreshRateExtensionEnabled_ = false;
            enumerateRefreshRates_ = nullptr;
            requestRefreshRate_ = nullptr;
            supportedRefreshRates_.clear();
            requestedRefreshRate_ = 0.0f;

            if (activity_ != nullptr)
            {
                env->DeleteGlobalRef(activity_);
                activity_ = nullptr;
            }
            javaVm_ = nullptr;
        }

    private:
        bool RenderEye(uint32_t eyeIndex, GLuint sbsTexture, const GLfloat* textureMatrix,
            const GLfloat* sourceFovDegrees,
            const XrView& locatedView, XrCompositionLayerProjectionView& projectionView)
        {
            EyeSwapchain& eye = eyes_[eyeIndex];
            XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            uint32_t imageIndex = 0;
            XrResult result = xrAcquireSwapchainImage(eye.handle, &acquireInfo, &imageIndex);
            if (XR_FAILED(result))
            {
                LogError("Could not acquire an OpenXR eye image", result);
                return false;
            }

            XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            waitInfo.timeout = XR_INFINITE_DURATION;
            result = xrWaitSwapchainImage(eye.handle, &waitInfo);
            if (XR_FAILED(result))
            {
                LogError("Could not wait for an OpenXR eye image", result);
                XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                xrReleaseSwapchainImage(eye.handle, &releaseInfo);
                return false;
            }

            const GLuint texture = eye.images[imageIndex].image;
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            {
                LogError("OpenXR eye framebuffer is incomplete");
                XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                xrReleaseSwapchainImage(eye.handle, &releaseInfo);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                return false;
            }

            glViewport(0, 0, static_cast<GLsizei>(eye.width), static_cast<GLsizei>(eye.height));
            glDisable(GL_BLEND);
            glDisable(GL_DEPTH_TEST);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            glUseProgram(blitProgram_);
            glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer_);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), nullptr);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                reinterpret_cast<const void*>(2 * sizeof(GLfloat)));
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, sbsTexture);
            glUniform1i(samplerLocation_, 0);
            glUniformMatrix4fv(matrixLocation_, 1, GL_FALSE, textureMatrix);
            glUniform1f(eyeLocation_, static_cast<GLfloat>(eyeIndex));
            constexpr GLfloat degreesToHalfRadians = 0.008726646259971648f;
            const GLfloat sourceBounds[4] = {
                -tanf(sourceFovDegrees[0] * degreesToHalfRadians),
                 tanf(sourceFovDegrees[1] * degreesToHalfRadians),
                 tanf(sourceFovDegrees[2] * degreesToHalfRadians),
                -tanf(sourceFovDegrees[3] * degreesToHalfRadians)
            };
            const GLfloat destinationBounds[4] = {
                tanf(locatedView.fov.angleLeft), tanf(locatedView.fov.angleRight),
                tanf(locatedView.fov.angleUp), tanf(locatedView.fov.angleDown)
            };
            glUniform4fv(sourceBoundsLocation_, 1, sourceBounds);
            glUniform4fv(destinationBoundsLocation_, 1, destinationBounds);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glDisableVertexAttribArray(0);
            glDisableVertexAttribArray(1);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
            glUseProgram(0);

            XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            result = xrReleaseSwapchainImage(eye.handle, &releaseInfo);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            if (XR_FAILED(result))
            {
                LogError("Could not release an OpenXR eye image", result);
                return false;
            }

            projectionView = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
            projectionView.pose = locatedView.pose;
            projectionView.fov = locatedView.fov;
            projectionView.subImage.swapchain = eye.handle;
            projectionView.subImage.imageRect.offset = { 0, 0 };
            projectionView.subImage.imageRect.extent = {
                static_cast<int32_t>(eye.width), static_cast<int32_t>(eye.height)
            };
            projectionView.subImage.imageArrayIndex = 0;
            return true;
        }

        void PollEvents()
        {
            if (instance_ == XR_NULL_HANDLE)
                return;

            XrEventDataBuffer event{ XR_TYPE_EVENT_DATA_BUFFER };
            while (xrPollEvent(instance_, &event) == XR_SUCCESS)
            {
                if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
                {
                    const auto* stateEvent = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
                    if (stateEvent->session != session_)
                    {
                        event = { XR_TYPE_EVENT_DATA_BUFFER };
                        continue;
                    }

                    switch (stateEvent->state)
                    {
                    case XR_SESSION_STATE_READY:
                    {
                        XrSessionBeginInfo beginInfo{ XR_TYPE_SESSION_BEGIN_INFO };
                        beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                        const XrResult result = xrBeginSession(session_, &beginInfo);
                        if (XR_SUCCEEDED(result)) sessionRunning_ = true;
                        else LogError("xrBeginSession failed", result);
                        break;
                    }
                    case XR_SESSION_STATE_STOPPING:
                        if (sessionRunning_)
                        {
                            const XrResult result = xrEndSession(session_);
                            if (XR_FAILED(result)) LogError("xrEndSession failed", result);
                        }
                        sessionRunning_ = false;
                        break;
                    case XR_SESSION_STATE_EXITING:
                    case XR_SESSION_STATE_LOSS_PENDING:
                        sessionRunning_ = false;
                        break;
                    default:
                        break;
                    }
                }
                event = { XR_TYPE_EVENT_DATA_BUFFER };
            }
        }

        void RequestMatchingRefreshRate(uint32_t streamFrameRate)
        {
            if (!refreshRateExtensionEnabled_ || requestRefreshRate_ == nullptr ||
                supportedRefreshRates_.empty() || streamFrameRate < 30 || streamFrameRate > 240)
                return;

            const auto closest = std::min_element(supportedRefreshRates_.begin(),
                supportedRefreshRates_.end(), [streamFrameRate](float a, float b) {
                    return std::fabs(a - static_cast<float>(streamFrameRate)) <
                        std::fabs(b - static_cast<float>(streamFrameRate));
                });
            if (closest == supportedRefreshRates_.end() || *closest == requestedRefreshRate_)
                return;

            const XrResult result = requestRefreshRate_(session_, *closest);
            if (XR_SUCCEEDED(result))
            {
                requestedRefreshRate_ = *closest;
                __android_log_print(ANDROID_LOG_INFO, kTag,
                    "Requested OpenXR display refresh rate %.2f Hz to match the stream", *closest);
            }
            else
            {
                LogError("Could not request a display refresh rate matching the stream", result);
            }
        }

        JavaVM* javaVm_ = nullptr;
        jobject activity_ = nullptr;
        XrInstance instance_ = XR_NULL_HANDLE;
        XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
        XrSession session_ = XR_NULL_HANDLE;
        XrSpace viewSpace_ = XR_NULL_HANDLE;
        bool sessionRunning_ = false;
        bool refreshRateExtensionEnabled_ = false;
        PFN_xrEnumerateDisplayRefreshRatesFB enumerateRefreshRates_ = nullptr;
        PFN_xrRequestDisplayRefreshRateFB requestRefreshRate_ = nullptr;
        std::vector<float> supportedRefreshRates_;
        float requestedRefreshRate_ = 0.0f;
        int64_t swapchainFormat_ = 0;
        std::vector<XrViewConfigurationView> viewConfigurationViews_;
        std::vector<EyeSwapchain> eyes_;
        GLuint blitProgram_ = 0;
        GLuint vertexBuffer_ = 0;
        GLint samplerLocation_ = -1;
        GLint matrixLocation_ = -1;
        GLint eyeLocation_ = -1;
        GLint sourceBoundsLocation_ = -1;
        GLint destinationBoundsLocation_ = -1;
        GLuint framebuffer_ = 0;
    };

    OpenXrSession g_session;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_pixelvr_OpenXrSession_initialize(JNIEnv* env, jobject, jobject activity,
    jlong display, jlong config, jlong context)
{
    return g_session.Initialize(env, activity,
        reinterpret_cast<EGLDisplay>(display),
        reinterpret_cast<EGLConfig>(config),
        reinterpret_cast<EGLContext>(context)) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_pixelvr_OpenXrSession_render(JNIEnv* env, jobject, jint textureId,
    jfloatArray matrix, jfloatArray sourceFov, jint streamFrameRate)
{
    if (matrix == nullptr || env->GetArrayLength(matrix) != 16 ||
        sourceFov == nullptr || env->GetArrayLength(sourceFov) != 4)
        return JNI_FALSE;
    jfloat values[16];
    jfloat fov[4];
    env->GetFloatArrayRegion(matrix, 0, 16, values);
    env->GetFloatArrayRegion(sourceFov, 0, 4, fov);
    if (env->ExceptionCheck())
        return JNI_FALSE;
    const uint32_t frameRate = streamFrameRate > 0
        ? static_cast<uint32_t>(streamFrameRate) : 0;
    return g_session.Render(static_cast<GLuint>(textureId), values, fov, frameRate)
        ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_pixelvr_OpenXrSession_shutdown(JNIEnv* env, jobject)
{
    g_session.Shutdown(env);
}

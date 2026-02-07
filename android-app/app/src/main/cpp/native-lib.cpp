#include <jni.h>
#include <string>
#include <android/log.h>
#include <dlfcn.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cstring>
#include <cstdarg>
#include <cstdio>

#define TAG "NovaNative"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

#define RETRO_ENVIRONMENT_SET_PIXEL_FORMAT 10
#define RETRO_ENVIRONMENT_GET_CAN_DUPE 3
#define RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY 9
#define RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY 31
#define RETRO_ENVIRONMENT_GET_LOG_INTERFACE 27
#define RETRO_ENVIRONMENT_SET_MEMORY_MAPS 36

#define RETRO_PIXEL_FORMAT_0RGB1555 0
#define RETRO_PIXEL_FORMAT_XRGB8888 1
#define RETRO_PIXEL_FORMAT_RGB565 2

// Log callback for cores that need it
enum retro_log_level {
   RETRO_LOG_DEBUG = 0,
   RETRO_LOG_INFO,
   RETRO_LOG_WARN,
   RETRO_LOG_ERROR,
   RETRO_LOG_DUMMY = INT_MAX
};

struct retro_log_callback {
   void (*log)(enum retro_log_level level, const char *fmt, ...);
};

static void LogCallback(enum retro_log_level level, const char *fmt, ...) {
    va_list va;
    va_start(va, fmt);
    char buf[4096];
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    
    switch(level) {
        case RETRO_LOG_DEBUG: LOGD("%s", buf); break;
        case RETRO_LOG_INFO: LOGI("%s", buf); break;
        case RETRO_LOG_WARN: LOGI("WARN: %s", buf); break;
        case RETRO_LOG_ERROR: LOGE("%s", buf); break;
        default: LOGD("%s", buf); break;
    }
}

#define RETRO_DEVICE_JOYPAD 1

struct retro_game_info {
    const char *path;
    const void *data;
    size_t size;
    const char *meta;
};

struct retro_system_info {
    const char *library_name;
    const char *library_version;
    const char *valid_extensions;
    bool need_fullpath;
    bool block_extract;
};

typedef void (*retro_init_t)(void);
typedef bool (*retro_load_game_t)(const struct retro_game_info *game);
typedef void (*retro_run_t)(void);
typedef void (*retro_deinit_t)(void);
typedef void (*retro_unload_game_t)(void);
typedef void (*retro_reset_t)(void);
typedef void (*retro_get_system_info_t)(struct retro_system_info *info);
typedef size_t (*retro_serialize_size_t)(void);
typedef bool (*retro_serialize_t)(void *data, size_t size);
typedef bool (*retro_unserialize_t)(const void *data, size_t size);

typedef bool (*retro_environment_t)(unsigned cmd, void *data);
typedef void (*retro_video_refresh_t)(const void *data, unsigned width, unsigned height, size_t pitch);
typedef void (*retro_audio_sample_t)(int16_t left, int16_t right);
typedef void (*retro_audio_sample_batch_t)(const int16_t *data, size_t frames);
typedef void (*retro_input_poll_t)(void);
typedef int16_t (*retro_input_state_t)(unsigned port, unsigned device, unsigned index, unsigned id);

typedef void (*retro_set_environment_t)(retro_environment_t);
typedef void (*retro_set_video_refresh_t)(retro_video_refresh_t);
typedef void (*retro_set_audio_sample_t)(retro_audio_sample_t);
typedef void (*retro_set_audio_sample_batch_t)(retro_audio_sample_batch_t);
typedef void (*retro_set_input_poll_t)(retro_input_poll_t);
typedef void (*retro_set_input_state_t)(retro_input_state_t);

// Global state
static JavaVM* g_vm = nullptr;
static jobject g_activity = nullptr;
static jmethodID g_writeAudioMethod = nullptr;
static void* g_coreHandle = nullptr;
static ANativeWindow* g_nativeWindow = nullptr;

static std::atomic<bool> g_isRunning{false};
static std::atomic<bool> g_isPaused{false};
static std::atomic<bool> g_fastForward{false};
static std::atomic<uint16_t> g_joypadBits{0};
static std::atomic<int> g_pixelFormat{RETRO_PIXEL_FORMAT_RGB565};
static std::atomic<int> g_sampleRate{44100};

static std::thread g_emuThread;
static std::mutex g_activityMutex;
static std::mutex g_windowMutex;

// Core functions
static retro_init_t core_init = nullptr;
static retro_load_game_t core_load_game = nullptr;
static retro_run_t core_run = nullptr;
static retro_deinit_t core_deinit = nullptr;
static retro_unload_game_t core_unload_game = nullptr;
static retro_reset_t core_reset = nullptr;
static retro_get_system_info_t core_get_system_info = nullptr;
static retro_serialize_size_t core_serialize_size = nullptr;
static retro_serialize_t core_serialize = nullptr;
static retro_unserialize_t core_unserialize = nullptr;

static retro_set_environment_t core_set_environment = nullptr;
static retro_set_video_refresh_t core_set_video_refresh = nullptr;
static retro_set_audio_sample_t core_set_audio_sample = nullptr;
static retro_set_audio_sample_batch_t core_set_audio_sample_batch = nullptr;
static retro_set_input_poll_t core_set_input_poll = nullptr;
static retro_set_input_state_t core_set_input_state = nullptr;

// Audio buffer - simple and effective
static std::mutex g_audioMutex;
static int16_t g_audioBuffer[8192];  // 8KB buffer
static int g_audioBufferPos = 0;
static std::atomic<int> g_audioBufferSize{4096}; // Dynamic buffer size

static std::string g_systemDir;
static std::string g_saveDir;

static JNIEnv* GetJNIEnv() {
    if (!g_vm) return nullptr;
    JNIEnv* env = nullptr;
    int status = g_vm->GetEnv((void**)&env, JNI_VERSION_1_6);
    if (status == JNI_EDETACHED) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != 0) return nullptr;
    } else if (status != JNI_OK) {
        return nullptr;
    }
    return env;
}

// Send audio to Java - BLOCKING
static void SendAudioToJava(JNIEnv* env, const int16_t* data, int size) {
    if (size <= 0 || !data) return;
    
    jobject activity = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_activityMutex);
        if (g_activity) activity = env->NewLocalRef(g_activity);
    }
    
    if (activity && g_writeAudioMethod) {
        jshortArray audioData = env->NewShortArray(size);
        if (audioData) {
            env->SetShortArrayRegion(audioData, 0, size, data);
            env->CallVoidMethod(activity, g_writeAudioMethod, audioData, size);
            env->DeleteLocalRef(audioData);
        }
        env->DeleteLocalRef(activity);
    }
    
    if (env->ExceptionCheck()) env->ExceptionClear();
}

static bool EnvironmentCallback(unsigned cmd, void *data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            *(bool*)data = true;
            return true;
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            g_pixelFormat.store(*(int*)data);
            return true;
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
            if (!g_systemDir.empty()) {
                *(const char**)data = g_systemDir.c_str();
                return true;
            }
            return false;
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            if (!g_saveDir.empty()) {
                *(const char**)data = g_saveDir.c_str();
                return true;
            }
            return false;
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
            if (data) {
                struct retro_log_callback* log_cb = (struct retro_log_callback*)data;
                log_cb->log = LogCallback;
                return true;
            }
            return false;
    }
    return false;
}

static void VideoRefreshCallback(const void *data, unsigned width, unsigned height, size_t pitch) {
    if (!data) return;
    
    ANativeWindow* window = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_windowMutex);
        window = g_nativeWindow;
    }
    
    if (!window) return;
    
    int format = g_pixelFormat.load();
    int32_t androidFormat = (format == RETRO_PIXEL_FORMAT_XRGB8888) 
                            ? WINDOW_FORMAT_RGBA_8888 
                            : WINDOW_FORMAT_RGB_565;
    
    ANativeWindow_setBuffersGeometry(window, width, height, androidFormat);
    
    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(window, &buffer, nullptr) != 0) return;
    
    if (format == RETRO_PIXEL_FORMAT_XRGB8888) {
        for (unsigned y = 0; y < height; y++) {
            const uint32_t* srcLine = (const uint32_t*)((const uint8_t*)data + y * pitch);
            uint32_t* dstLine = (uint32_t*)((uint8_t*)buffer.bits + y * buffer.stride * 4);
            for (unsigned x = 0; x < width; x++) {
                uint32_t pixel = srcLine[x];
                dstLine[x] = 0xFF000000 | ((pixel & 0xFF) << 16) | (pixel & 0xFF00) | ((pixel >> 16) & 0xFF);
            }
        }
    } else if (format == RETRO_PIXEL_FORMAT_0RGB1555) {
        for (unsigned y = 0; y < height; y++) {
            const uint16_t* srcLine = (const uint16_t*)((const uint8_t*)data + y * pitch);
            uint16_t* dstLine = (uint16_t*)((uint8_t*)buffer.bits + y * buffer.stride * 2);
            for (unsigned x = 0; x < width; x++) {
                uint16_t pixel = srcLine[x];
                dstLine[x] = (((pixel >> 10) & 0x1F) << 11) | (((pixel >> 5) & 0x1F) << 6) | (pixel & 0x1F);
            }
        }
    } else {
        for (unsigned y = 0; y < height; y++) {
            const uint8_t* srcLine = (const uint8_t*)data + y * pitch;
            uint8_t* dstLine = (uint8_t*)buffer.bits + y * buffer.stride * 2;
            memcpy(dstLine, srcLine, width * 2);
        }
    }
    
    ANativeWindow_unlockAndPost(window);
}

// Audio callback - accumulate samples, flush when we have enough
static void AudioSampleBatchCallback(const int16_t *data, size_t frames) {
    if (g_fastForward.load() || !data || frames == 0) return;
    
    int samples = frames * 2; // Stereo
    int bufferSize = g_audioBufferSize.load();
    
    for (int i = 0; i < samples; i++) {
        g_audioMutex.lock();
        g_audioBuffer[g_audioBufferPos++] = data[i];
        
        // Flush when buffer reaches threshold
        if (g_audioBufferPos >= bufferSize / 2) {
            int samplesToSend = g_audioBufferPos;
            g_audioBufferPos = 0;
            g_audioMutex.unlock();
            
            JNIEnv* env = GetJNIEnv();
            if (env) {
                SendAudioToJava(env, g_audioBuffer, samplesToSend);
            }
        } else {
            g_audioMutex.unlock();
        }
    }
}

static void AudioSampleCallback(int16_t left, int16_t right) {
    int16_t samples[2] = {left, right};
    AudioSampleBatchCallback(samples, 1);
}

static void InputPollCallback() {}

static int16_t InputStateCallback(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port == 0 && device == RETRO_DEVICE_JOYPAD && index == 0) {
        return (g_joypadBits.load() & (1 << id)) ? 1 : 0;
    }
    return 0;
}

// Game loop with proper audio sync
static void GameLoop() {
    LOGI("Game loop started");
    JNIEnv* env = GetJNIEnv();
    
    // 60 FPS = 16666.67 microseconds per frame
    const auto frameTime = std::chrono::microseconds(16667);
    auto nextFrameTime = std::chrono::steady_clock::now();
    
    while (g_isRunning.load()) {
        if (g_isPaused.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            nextFrameTime = std::chrono::steady_clock::now() + frameTime;
            continue;
        }
        
        // Run one frame
        if (core_run) core_run();
        
        // Flush any remaining audio
        g_audioMutex.lock();
        if (g_audioBufferPos > 0 && env) {
            int samplesToSend = g_audioBufferPos;
            g_audioBufferPos = 0;
            g_audioMutex.unlock();
            SendAudioToJava(env, g_audioBuffer, samplesToSend);
        } else {
            g_audioMutex.unlock();
        }
        
        if (!g_fastForward.load()) {
            // Wait until next frame time
            nextFrameTime += frameTime;
            std::this_thread::sleep_until(nextFrameTime);
        }
    }
    
    LOGI("Game loop ended");
}

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    g_vm = vm;
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL
Java_com_blinkchase_nova_MainActivity_updateNativeActivity(JNIEnv *env, jobject thiz) {
    std::lock_guard<std::mutex> lock(g_activityMutex);
    if (g_activity) env->DeleteGlobalRef(g_activity);
    g_activity = env->NewGlobalRef(thiz);
    jclass clazz = env->GetObjectClass(thiz);
    g_writeAudioMethod = env->GetMethodID(clazz, "writeAudio", "([SI)V");
}

JNIEXPORT void JNICALL
Java_com_blinkchase_nova_MainActivity_setSystemDirectories(JNIEnv *env, jobject thiz, jstring systemDir, jstring saveDir) {
    const char* sysPath = env->GetStringUTFChars(systemDir, 0);
    const char* savePath = env->GetStringUTFChars(saveDir, 0);
    g_systemDir = sysPath;
    g_saveDir = savePath;
    env->ReleaseStringUTFChars(systemDir, sysPath);
    env->ReleaseStringUTFChars(saveDir, savePath);
}

JNIEXPORT jstring JNICALL
Java_com_blinkchase_nova_MainActivity_loadCore(JNIEnv *env, jobject thiz, jstring corePath) {
    const char *path = env->GetStringUTFChars(corePath, 0);
    LOGI("Loading core: %s", path);
    
    if (g_coreHandle) {
        if (core_deinit) core_deinit();
        dlclose(g_coreHandle);
    }
    
    g_coreHandle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    env->ReleaseStringUTFChars(corePath, path);
    
    if (!g_coreHandle) {
        return env->NewStringUTF(dlerror());
    }
    
    core_init = (retro_init_t)dlsym(g_coreHandle, "retro_init");
    core_load_game = (retro_load_game_t)dlsym(g_coreHandle, "retro_load_game");
    core_run = (retro_run_t)dlsym(g_coreHandle, "retro_run");
    core_deinit = (retro_deinit_t)dlsym(g_coreHandle, "retro_deinit");
    core_unload_game = (retro_unload_game_t)dlsym(g_coreHandle, "retro_unload_game");
    core_reset = (retro_reset_t)dlsym(g_coreHandle, "retro_reset");
    core_get_system_info = (retro_get_system_info_t)dlsym(g_coreHandle, "retro_get_system_info");
    core_serialize_size = (retro_serialize_size_t)dlsym(g_coreHandle, "retro_serialize_size");
    core_serialize = (retro_serialize_t)dlsym(g_coreHandle, "retro_serialize");
    core_unserialize = (retro_unserialize_t)dlsym(g_coreHandle, "retro_unserialize");
    
    core_set_environment = (retro_set_environment_t)dlsym(g_coreHandle, "retro_set_environment");
    core_set_video_refresh = (retro_set_video_refresh_t)dlsym(g_coreHandle, "retro_set_video_refresh");
    core_set_audio_sample = (retro_set_audio_sample_t)dlsym(g_coreHandle, "retro_set_audio_sample");
    core_set_audio_sample_batch = (retro_set_audio_sample_batch_t)dlsym(g_coreHandle, "retro_set_audio_sample_batch");
    core_set_input_poll = (retro_set_input_poll_t)dlsym(g_coreHandle, "retro_set_input_poll");
    core_set_input_state = (retro_set_input_state_t)dlsym(g_coreHandle, "retro_set_input_state");
    
    if (!core_init || !core_load_game || !core_run) {
        dlclose(g_coreHandle);
        g_coreHandle = nullptr;
        return env->NewStringUTF("Core missing required functions");
    }
    
    if (core_set_environment) core_set_environment(EnvironmentCallback);
    if (core_set_video_refresh) core_set_video_refresh(VideoRefreshCallback);
    if (core_set_audio_sample) core_set_audio_sample(AudioSampleCallback);
    if (core_set_audio_sample_batch) core_set_audio_sample_batch(AudioSampleBatchCallback);
    if (core_set_input_poll) core_set_input_poll(InputPollCallback);
    if (core_set_input_state) core_set_input_state(InputStateCallback);
    
    core_init();
    return nullptr;
}

JNIEXPORT jboolean JNICALL
Java_com_blinkchase_nova_MainActivity_nativeLoadGame(JNIEnv *env, jobject thiz, jstring romPath) {
    if (!g_coreHandle || !core_load_game) return JNI_FALSE;
    
    if (g_isRunning.load()) {
        g_isRunning.store(false);
        if (g_emuThread.joinable()) g_emuThread.join();
        if (core_unload_game) core_unload_game();
    }
    
    // Clear audio buffer
    {
        std::lock_guard<std::mutex> lock(g_audioMutex);
        g_audioBufferPos = 0;
    }
    
    const char *path = env->GetStringUTFChars(romPath, 0);
    retro_game_info game = {0};
    game.path = path;
    bool success = core_load_game(&game);
    env->ReleaseStringUTFChars(romPath, path);
    
    if (!success) return JNI_FALSE;
    
    g_isRunning.store(true);
    g_emuThread = std::thread(GameLoop);
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_blinkchase_nova_MainActivity_nativePauseGame(JNIEnv *env, jobject thiz) {
    g_isPaused.store(true);
}

JNIEXPORT void JNICALL
Java_com_blinkchase_nova_MainActivity_nativeResumeGame(JNIEnv *env, jobject thiz) {
    g_isPaused.store(false);
}

JNIEXPORT void JNICALL
Java_com_blinkchase_nova_MainActivity_resetGame(JNIEnv *env, jobject thiz) {
    if (core_reset) core_reset();
}

JNIEXPORT void JNICALL
Java_com_blinkchase_nova_MainActivity_nativeQuitGame(JNIEnv *env, jobject thiz) {
    g_isRunning.store(false);
    if (g_emuThread.joinable()) g_emuThread.join();
    if (core_unload_game) core_unload_game();
    {
        std::lock_guard<std::mutex> lock(g_audioMutex);
        g_audioBufferPos = 0;
    }
}

JNIEXPORT void JNICALL
Java_com_blinkchase_nova_MainActivity_setSurface(JNIEnv *env, jobject thiz, jobject surface) {
    std::lock_guard<std::mutex> lock(g_windowMutex);
    if (g_nativeWindow) {
        ANativeWindow_release(g_nativeWindow);
        g_nativeWindow = nullptr;
    }
    if (surface) {
        g_nativeWindow = ANativeWindow_fromSurface(env, surface);
        if (g_nativeWindow) {
            ANativeWindow_setBuffersGeometry(g_nativeWindow, 0, 0, WINDOW_FORMAT_RGB_565);
        }
    }
}

JNIEXPORT void JNICALL
Java_com_blinkchase_nova_MainActivity_sendInput(JNIEnv *env, jobject thiz, jint buttonId, jint value) {
    uint16_t bits = g_joypadBits.load();
    if (value) bits |= (1 << buttonId);
    else bits &= ~(1 << buttonId);
    g_joypadBits.store(bits);
}

JNIEXPORT void JNICALL
Java_com_blinkchase_nova_MainActivity_setFastForward(JNIEnv *env, jobject thiz, jboolean enabled) {
    g_fastForward.store(enabled);
}

JNIEXPORT jboolean JNICALL
Java_com_blinkchase_nova_MainActivity_saveState(JNIEnv *env, jobject thiz, jstring filePath) {
    LOGI("saveState: core_serialize_size=%p, core_serialize=%p", 
         (void*)core_serialize_size, (void*)core_serialize);
    
    if (!core_serialize_size || !core_serialize) {
        LOGE("saveState: Core does not support serialization");
        return JNI_FALSE;
    }
    
    size_t size = core_serialize_size();
    LOGI("saveState: Required size = %zu bytes", size);
    
    if (size == 0) {
        LOGE("saveState: Core returned size 0 (no save state support)");
        return JNI_FALSE;
    }
    
    void* data = malloc(size);
    if (!data) {
        LOGE("saveState: Failed to allocate memory");
        return JNI_FALSE;
    }
    
    bool success = core_serialize(data, size);
    LOGI("saveState: core_serialize returned %d", success);
    
    if (success) {
        const char *path = env->GetStringUTFChars(filePath, 0);
        FILE* f = fopen(path, "wb");
        if (f) {
            size_t written = fwrite(data, 1, size, f);
            fclose(f);
            LOGI("saveState: Wrote %zu bytes to %s", written, path);
            success = (written == size);
        } else {
            LOGE("saveState: Failed to open file for writing: %s", path);
            success = false;
        }
        env->ReleaseStringUTFChars(filePath, path);
    }
    
    free(data);
    return success ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_blinkchase_nova_MainActivity_loadState(JNIEnv *env, jobject thiz, jstring filePath) {
    LOGI("loadState: core_unserialize=%p", (void*)core_unserialize);
    
    if (!core_unserialize) {
        LOGE("loadState: Core does not support unserialize");
        return JNI_FALSE;
    }
    
    const char *path = env->GetStringUTFChars(filePath, 0);
    FILE* f = fopen(path, "rb");
    env->ReleaseStringUTFChars(filePath, path);
    
    if (!f) {
        LOGE("loadState: File not found: %s", path);
        return JNI_FALSE;
    }
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    LOGI("loadState: File size = %ld bytes", size);
    
    void* data = malloc(size);
    if (!data) {
        LOGE("loadState: Failed to allocate memory");
        fclose(f);
        return JNI_FALSE;
    }
    
    size_t read = fread(data, 1, size, f);
    fclose(f);
    
    if (read != (size_t)size) {
        LOGE("loadState: Failed to read full file (read %zu of %ld)", read, size);
        free(data);
        return JNI_FALSE;
    }
    
    bool success = core_unserialize(data, size);
    LOGI("loadState: core_unserialize returned %d", success);
    
    free(data);
    return success ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_blinkchase_nova_MainActivity_setCheat(JNIEnv *env, jobject thiz, jint index, jboolean enabled, jstring code) {}

JNIEXPORT jint JNICALL
Java_com_blinkchase_nova_MainActivity_getNativeFps(JNIEnv *env, jobject thiz) {
    return 60;
}

}

/*
 * Copyright 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Wextra"

//#define LOG_NDEBUG 0
#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#undef LOG_TAG
#define LOG_TAG "RegionSamplingThread"

#include "RegionSamplingThread.h"

#include <compositionengine/Display.h>
#include <compositionengine/impl/OutputCompositionState.h>
#include <cutils/properties.h>
#include <ftl/future.h>
#include <gui/IRegionSamplingListener.h>
#include <gui/SyncScreenCaptureListener.h>
#include <ui/DisplayStatInfo.h>
#include <utils/Trace.h>

#include <string>

#include "DisplayDevice.h"
#include "DisplayRenderArea.h"
#include "Layer.h"
#include "Scheduler/VsyncController.h"
#include "SurfaceFlinger.h"

namespace android {
using namespace std::chrono_literals;

template <typename T>
struct SpHash {
    size_t operator()(const sp<T>& p) const { return std::hash<T*>()(p.get()); }
};

constexpr auto lumaSamplingStepTag = "LumaSamplingStep";
enum class samplingStep {
    noWorkNeeded,
    idleTimerWaiting,
    waitForQuietFrame,
    waitForZeroPhase,
    waitForSamplePhase,
    sample
};

constexpr auto timeForRegionSampling = 5000000ns;
constexpr auto maxRegionSamplingSkips = 10;
constexpr auto defaultRegionSamplingWorkDuration = 3ms;
constexpr auto defaultRegionSamplingPeriod = 100ms;
constexpr auto defaultRegionSamplingTimerTimeout = 100ms;
// TODO: (b/127403193) duration to string conversion could probably be constexpr
template <typename Rep, typename Per>
inline std::string toNsString(std::chrono::duration<Rep, Per> t) {
    return std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(t).count());
}

RegionSamplingThread::EnvironmentTimingTunables::EnvironmentTimingTunables() {
    char value[PROPERTY_VALUE_MAX] = {};

    property_get("debug.sf.region_sampling_duration_ns", value,
                 toNsString(defaultRegionSamplingWorkDuration).c_str());
    int const samplingDurationNsRaw = atoi(value);

    property_get("debug.sf.region_sampling_period_ns", value,
                 toNsString(defaultRegionSamplingPeriod).c_str());
    int const samplingPeriodNsRaw = atoi(value);

    property_get("debug.sf.region_sampling_timer_timeout_ns", value,
                 toNsString(defaultRegionSamplingTimerTimeout).c_str());
    int const samplingTimerTimeoutNsRaw = atoi(value);

    if ((samplingPeriodNsRaw < 0) || (samplingTimerTimeoutNsRaw < 0)) {
        ALOGW("User-specified sampling tuning options nonsensical. Using defaults");
        mSamplingDuration = defaultRegionSamplingWorkDuration;
        mSamplingPeriod = defaultRegionSamplingPeriod;
        mSamplingTimerTimeout = defaultRegionSamplingTimerTimeout;
    } else {
        mSamplingDuration = std::chrono::nanoseconds(samplingDurationNsRaw);
        mSamplingPeriod = std::chrono::nanoseconds(samplingPeriodNsRaw);
        mSamplingTimerTimeout = std::chrono::nanoseconds(samplingTimerTimeoutNsRaw);
    }
}

struct SamplingOffsetCallback : VSyncSource::Callback {
    SamplingOffsetCallback(RegionSamplingThread& samplingThread, Scheduler& scheduler,
                           std::chrono::nanoseconds targetSamplingWorkDuration)
          : mRegionSamplingThread(samplingThread),
            mTargetSamplingWorkDuration(targetSamplingWorkDuration),
            mVSyncSource(scheduler.makePrimaryDispSyncSource("SamplingThreadDispSyncListener", 0ns,
                                                             0ns,
                                                             /*traceVsync=*/false)) {
        mVSyncSource->setCallback(this);
    }

    ~SamplingOffsetCallback() { stopVsyncListener(); }

    SamplingOffsetCallback(const SamplingOffsetCallback&) = delete;
    SamplingOffsetCallback& operator=(const SamplingOffsetCallback&) = delete;

    void startVsyncListener() {
        std::lock_guard lock(mMutex);
        if (mVsyncListening) return;

        mPhaseIntervalSetting = Phase::ZERO;
        mVSyncSource->setVSyncEnabled(true);
        mVsyncListening = true;
    }

    void stopVsyncListener() {
        std::lock_guard lock(mMutex);
        stopVsyncListenerLocked();
    }

private:
    void stopVsyncListenerLocked() /*REQUIRES(mMutex)*/ {
        if (!mVsyncListening) return;

        mVSyncSource->setVSyncEnabled(false);
        mVsyncListening = false;
    }

    void onVSyncEvent(nsecs_t /*when*/, nsecs_t /*expectedVSyncTimestamp*/,
                      nsecs_t /*deadlineTimestamp*/) final {
        std::unique_lock<decltype(mMutex)> lock(mMutex);

        if (mPhaseIntervalSetting == Phase::ZERO) {
            ATRACE_INT(lumaSamplingStepTag, static_cast<int>(samplingStep::waitForSamplePhase));
            mPhaseIntervalSetting = Phase::SAMPLING;
            mVSyncSource->setDuration(mTargetSamplingWorkDuration, 0ns);
            return;
        }

        if (mPhaseIntervalSetting == Phase::SAMPLING) {
            mPhaseIntervalSetting = Phase::ZERO;
            mVSyncSource->setDuration(0ns, 0ns);
            stopVsyncListenerLocked();
            lock.unlock();
            mRegionSamplingThread.notifySamplingOffset();
            return;
        }
    }

    RegionSamplingThread& mRegionSamplingThread;
    const std::chrono::nanoseconds mTargetSamplingWorkDuration;
    mutable std::mutex mMutex;
    enum class Phase {
        ZERO,
        SAMPLING
    } mPhaseIntervalSetting /*GUARDED_BY(mMutex) macro doesnt work with unique_lock?*/
            = Phase::ZERO;
    bool mVsyncListening /*GUARDED_BY(mMutex)*/ = false;
    std::unique_ptr<VSyncSource> mVSyncSource;
};

RegionSamplingThread::RegionSamplingThread(SurfaceFlinger& flinger, Scheduler& scheduler,
                                           const TimingTunables& tunables)
      : mFlinger(flinger),
        mScheduler(scheduler),
        mTunables(tunables),
        mIdleTimer(
                "RegSampIdle",
                std::chrono::duration_cast<std::chrono::milliseconds>(
                        mTunables.mSamplingTimerTimeout),
                [] {}, [this] { checkForStaleLuma(); }),
        mPhaseCallback(std::make_unique<SamplingOffsetCallback>(*this, mScheduler,
                                                                tunables.mSamplingDuration)),
        lastSampleTime(0ns) {
    mThread = std::thread([this]() { threadMain(); });
    pthread_setname_np(mThread.native_handle(), "RegionSampling");
    mIdleTimer.start();
}

RegionSamplingThread::RegionSamplingThread(SurfaceFlinger& flinger, Scheduler& scheduler)
      : RegionSamplingThread(flinger, scheduler,
                             TimingTunables{defaultRegionSamplingWorkDuration,
                                            defaultRegionSamplingPeriod,
                                            defaultRegionSamplingTimerTimeout}) {}

RegionSamplingThread::~RegionSamplingThread() {
    mIdleTimer.stop();

    {
        std::lock_guard lock(mThreadControlMutex);
        mRunning = false;
        mCondition.notify_one();
    }

    if (mThread.joinable()) {
        mThread.join();
    }
}

void RegionSamplingThread::addListener(const Rect& samplingArea, const wp<Layer>& stopLayer,
                                       const sp<IRegionSamplingListener>& listener) {
    sp<IBinder> asBinder = IInterface::asBinder(listener);
    asBinder->linkToDeath(this);
    std::lock_guard lock(mSamplingMutex);
    mDescriptors.emplace(wp<IBinder>(asBinder), Descriptor{samplingArea, stopLayer, listener});
}

void RegionSamplingThread::removeListener(const sp<IRegionSamplingListener>& listener) {
    std::lock_guard lock(mSamplingMutex);
    mDescriptors.erase(wp<IBinder>(IInterface::asBinder(listener)));
}

void RegionSamplingThread::checkForStaleLuma() {
    std::lock_guard lock(mThreadControlMutex);

    if (mDiscardedFrames > 0) {
        ATRACE_INT(lumaSamplingStepTag, static_cast<int>(samplingStep::waitForZeroPhase));
        mDiscardedFrames = 0;
        mPhaseCallback->startVsyncListener();
    }
}

void RegionSamplingThread::notifyNewContent() {
    doSample();
}

void RegionSamplingThread::notifySamplingOffset() {
    doSample();
}

void RegionSamplingThread::doSample() {
    std::lock_guard lock(mThreadControlMutex);
    auto now = std::chrono::nanoseconds(systemTime(SYSTEM_TIME_MONOTONIC));
    if (lastSampleTime + mTunables.mSamplingPeriod > now) {
        ATRACE_INT(lumaSamplingStepTag, static_cast<int>(samplingStep::idleTimerWaiting));
        if (mDiscardedFrames == 0) mDiscardedFrames++;
        return;
    }
    if (mDiscardedFrames < maxRegionSamplingSkips) {
        // If there is relatively little time left for surfaceflinger
        // until the next vsync deadline, defer this sampling work
        // to a later frame, when hopefully there will be more time.
        const DisplayStatInfo stats = mScheduler.getDisplayStatInfo(systemTime());
        if (std::chrono::nanoseconds(stats.vsyncTime) - now < timeForRegionSampling) {
            ATRACE_INT(lumaSamplingStepTag, static_cast<int>(samplingStep::waitForQuietFrame));
            mDiscardedFrames++;
            return;
        }
    }

    ATRACE_INT(lumaSamplingStepTag, static_cast<int>(samplingStep::sample));

    mDiscardedFrames = 0;
    lastSampleTime = now;

    mIdleTimer.reset();
    mPhaseCallback->stopVsyncListener();

    mSampleRequested = true;
    mCondition.notify_one();
}

void RegionSamplingThread::binderDied(const wp<IBinder>& who) {
    std::lock_guard lock(mSamplingMutex);
    mDescriptors.erase(who);
}

float sampleArea(const uint32_t* data, int32_t width, int32_t height, int32_t stride,
                 uint32_t orientation, const Rect& sample_area) {
    if (!sample_area.isValid() || (sample_area.getWidth() > width) ||
        (sample_area.getHeight() > height)) {
        ALOGE("invalid sampling region requested");
        return 0.0f;
    }

    // (b/133849373) ROT_90 screencap images produced upside down
    auto area = sample_area;
    if (orientation & ui::Transform::ROT_90) {
        area.top = height - area.top;
        area.bottom = height - area.bottom;
        std::swap(area.top, area.bottom);

        area.left = width - area.left;
        area.right = width - area.right;
        std::swap(area.left, area.right);
    }

    const uint32_t pixelCount = (area.bottom - area.top) * (area.right - area.left);
    uint32_t accumulatedLuma = 0;

    // Calculates luma with approximation of Rec. 709 primaries
    for (int32_t row = area.top; row < area.bottom; ++row) {
        const uint32_t* rowBase = data + row * stride;
        for (int32_t column = area.left; column < area.right; ++column) {
            uint32_t pixel = rowBase[column];
            const uint32_t r = pixel & 0xFF;
            const uint32_t g = (pixel >> 8) & 0xFF;
            const uint32_t b = (pixel >> 16) & 0xFF;
            const uint32_t luma = (r * 7 + b * 2 + g * 23) >> 5;
            accumulatedLuma += luma;
        }
    }

    return accumulatedLuma / (255.0f * pixelCount);
}

std::vector<float> RegionSamplingThread::sampleBuffer(
        const sp<GraphicBuffer>& buffer, const Point& leftTop,
        const std::vector<RegionSamplingThread::Descriptor>& descriptors, uint32_t orientation) {
    void* data_raw = nullptr;
    buffer->lock(GRALLOC_USAGE_SW_READ_OFTEN, &data_raw);
    std::shared_ptr<uint32_t> data(reinterpret_cast<uint32_t*>(data_raw),
                                   [&buffer](auto) { buffer->unlock(); });
    if (!data) return {};

    const int32_t width = buffer->getWidth();
    const int32_t height = buffer->getHeight();
    const int32_t stride = buffer->getStride();
    std::vector<float> lumas(descriptors.size());
    std::transform(descriptors.begin(), descriptors.end(), lumas.begin(),
                   [&](auto const& descriptor) {
                       return sampleArea(data.get(), width, height, stride, orientation,
                                         descriptor.area - leftTop);
                   });
    return lumas;
}

void RegionSamplingThread::captureSample() {
    ATRACE_CALL();
    std::lock_guard lock(mSamplingMutex);

    if (mDescriptors.empty()) {
        return;
    }

    wp<const DisplayDevice> displayWeak;

    ui::LayerStack layerStack;
    ui::Transform::RotationFlags orientation;
    ui::Size displaySize;

    {
        // TODO(b/159112860): Don't keep sp<DisplayDevice> outside of SF main thread
        const sp<const DisplayDevice> display = mFlinger.getDefaultDisplayDevice();
        displayWeak = display;
        layerStack = display->getLayerStack();
        orientation = ui::Transform::toRotationFlags(display->getOrientation());
        displaySize = display->getSize();
    }

    std::vector<RegionSamplingThread::Descriptor> descriptors;
    Region sampleRegion;
    for (const auto& [listener, descriptor] : mDescriptors) {
        sampleRegion.orSelf(descriptor.area);
        descriptors.emplace_back(descriptor);
    }

    auto dx = 0;
    auto dy = 0;
    switch (orientation) {
        case ui::Transform::ROT_90:
            dx = displaySize.getWidth();
            break;
        case ui::Transform::ROT_180:
            dx = displaySize.getWidth();
            dy = displaySize.getHeight();
            break;
        case ui::Transform::ROT_270:
            dy = displaySize.getHeight();
            break;
        default:
            break;
    }

    ui::Transform t(orientation);
    auto screencapRegion = t.transform(sampleRegion);
    screencapRegion = screencapRegion.translate(dx, dy);

    const Rect sampledBounds = sampleRegion.bounds();

    SurfaceFlinger::RenderAreaFuture renderAreaFuture = ftl::defer([=] {
        return DisplayRenderArea::create(displayWeak, screencapRegion.bounds(),
                                         sampledBounds.getSize(), ui::Dataspace::V0_SRGB,
                                         orientation);
    });

    std::unordered_set<sp<IRegionSamplingListener>, SpHash<IRegionSamplingListener>> listeners;

    auto traverseLayers = [&](const LayerVector::Visitor& visitor) {
        bool stopLayerFound = false;
        auto filterVisitor = [&](Layer* layer) {
            // We don't want to capture any layers beyond the stop layer
            if (stopLayerFound) return;

            // Likewise if we just found a stop layer, set the flag and abort
            for (const auto& [area, stopLayer, listener] : descriptors) {
                if (layer == stopLayer.promote().get()) {
                    stopLayerFound = true;
                    return;
                }
            }

            // Compute the layer's position on the screen
            const Rect bounds = Rect(layer->getBounds());
            const ui::Transform transform = layer->getTransform();
            constexpr bool roundOutwards = true;
            Rect transformed = transform.transform(bounds, roundOutwards);

            // If this layer doesn't intersect with the larger sampledBounds, skip capturing it
            Rect ignore;
            if (!transformed.intersect(sampledBounds, &ignore)) return;

            // If the layer doesn't intersect a sampling area, skip capturing it
            bool intersectsAnyArea = false;
            for (const auto& [area, stopLayer, listener] : descriptors) {
                if (transformed.intersect(area, &ignore)) {
                    intersectsAnyArea = true;
                    listeners.insert(listener);
                }
            }
            if (!intersectsAnyArea) return;

            ALOGV("Traversing [%s] [%d, %d, %d, %d]", layer->getDebugName(), bounds.left,
                  bounds.top, bounds.right, bounds.bottom);
            visitor(layer);
        };
        mFlinger.traverseLayersInLayerStack(layerStack, CaptureArgs::UNSET_UID, filterVisitor);
    };

    std::shared_ptr<renderengine::ExternalTexture> buffer = nullptr;
    if (mCachedBuffer && mCachedBuffer->getBuffer()->getWidth() == sampledBounds.getWidth() &&
        mCachedBuffer->getBuffer()->getHeight() == sampledBounds.getHeight()) {
        buffer = mCachedBuffer;
    } else {
        const uint32_t usage =
                GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_TEXTURE;
        sp<GraphicBuffer> graphicBuffer =
                new GraphicBuffer(sampledBounds.getWidth(), sampledBounds.getHeight(),
                                  PIXEL_FORMAT_RGBA_8888, 1, usage, "RegionSamplingThread");
        const status_t bufferStatus = graphicBuffer->initCheck();
        LOG_ALWAYS_FATAL_IF(bufferStatus != OK, "captureSample: Buffer failed to allocate: %d",
                            bufferStatus);
        buffer = std::make_shared<
                renderengine::ExternalTexture>(graphicBuffer, mFlinger.getRenderEngine(),
                                               renderengine::ExternalTexture::Usage::WRITEABLE);
    }

    const sp<SyncScreenCaptureListener> captureListener = new SyncScreenCaptureListener();
    mFlinger.captureScreenCommon(std::move(renderAreaFuture), traverseLayers, buffer,
                                 true /* regionSampling */, false /* grayscale */, captureListener);
    ScreenCaptureResults captureResults = captureListener->waitForResults();

    std::vector<Descriptor> activeDescriptors;
    for (const auto& descriptor : descriptors) {
        if (listeners.count(descriptor.listener) != 0) {
            activeDescriptors.emplace_back(descriptor);
        }
    }

    ALOGV("Sampling %zu descriptors", activeDescriptors.size());
    std::vector<float> lumas = sampleBuffer(buffer->getBuffer(), sampledBounds.leftTop(),
                                            activeDescriptors, orientation);
    if (lumas.size() != activeDescriptors.size()) {
        ALOGW("collected %zu median luma values for %zu descriptors", lumas.size(),
              activeDescriptors.size());
        return;
    }

    for (size_t d = 0; d < activeDescriptors.size(); ++d) {
        activeDescriptors[d].listener->onSampleCollected(lumas[d]);
    }

    mCachedBuffer = buffer;
    ATRACE_INT(lumaSamplingStepTag, static_cast<int>(samplingStep::noWorkNeeded));
}

// NO_THREAD_SAFETY_ANALYSIS is because std::unique_lock presently lacks thread safety annotations.
void RegionSamplingThread::threadMain() NO_THREAD_SAFETY_ANALYSIS {
    std::unique_lock<std::mutex> lock(mThreadControlMutex);
    while (mRunning) {
        if (mSampleRequested) {
            mSampleRequested = false;
            lock.unlock();
            captureSample();
            lock.lock();
        }
        mCondition.wait(lock, [this]() REQUIRES(mThreadControlMutex) {
            return mSampleRequested || !mRunning;
        });
    }
}

} // namespace android

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic pop // ignored "-Wconversion -Wextra"

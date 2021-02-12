/*
 * Copyright 2020 The Android Open Source Project
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

#undef LOG_TAG
#define LOG_TAG "FrameTimeline"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include "FrameTimeline.h"
#include <android-base/stringprintf.h>
#include <utils/Log.h>
#include <utils/Trace.h>
#include <chrono>
#include <cinttypes>
#include <numeric>

namespace android::frametimeline {

using base::StringAppendF;
using FrameTimelineEvent = perfetto::protos::pbzero::FrameTimelineEvent;

void dumpTable(std::string& result, TimelineItem predictions, TimelineItem actuals,
               const std::string& indent, PredictionState predictionState, nsecs_t baseTime) {
    StringAppendF(&result, "%s", indent.c_str());
    StringAppendF(&result, "\t\t");
    StringAppendF(&result, "    Start time\t\t|");
    StringAppendF(&result, "    End time\t\t|");
    StringAppendF(&result, "    Present time\n");
    if (predictionState == PredictionState::Valid) {
        // Dump the Predictions only if they are valid
        StringAppendF(&result, "%s", indent.c_str());
        StringAppendF(&result, "Expected\t|");
        std::chrono::nanoseconds startTime(predictions.startTime - baseTime);
        std::chrono::nanoseconds endTime(predictions.endTime - baseTime);
        std::chrono::nanoseconds presentTime(predictions.presentTime - baseTime);
        StringAppendF(&result, "\t%10.2f\t|\t%10.2f\t|\t%10.2f\n",
                      std::chrono::duration<double, std::milli>(startTime).count(),
                      std::chrono::duration<double, std::milli>(endTime).count(),
                      std::chrono::duration<double, std::milli>(presentTime).count());
    }
    StringAppendF(&result, "%s", indent.c_str());
    StringAppendF(&result, "Actual  \t|");

    if (actuals.startTime == 0) {
        StringAppendF(&result, "\t\tN/A\t|");
    } else {
        std::chrono::nanoseconds startTime(std::max<nsecs_t>(0, actuals.startTime - baseTime));
        StringAppendF(&result, "\t%10.2f\t|",
                      std::chrono::duration<double, std::milli>(startTime).count());
    }
    if (actuals.endTime <= 0) {
        // Animation leashes can send the endTime as -1
        StringAppendF(&result, "\t\tN/A\t|");
    } else {
        std::chrono::nanoseconds endTime(actuals.endTime - baseTime);
        StringAppendF(&result, "\t%10.2f\t|",
                      std::chrono::duration<double, std::milli>(endTime).count());
    }
    if (actuals.presentTime == 0) {
        StringAppendF(&result, "\t\tN/A\n");
    } else {
        std::chrono::nanoseconds presentTime(std::max<nsecs_t>(0, actuals.presentTime - baseTime));
        StringAppendF(&result, "\t%10.2f\n",
                      std::chrono::duration<double, std::milli>(presentTime).count());
    }

    StringAppendF(&result, "%s", indent.c_str());
    StringAppendF(&result, "----------------------");
    StringAppendF(&result, "----------------------");
    StringAppendF(&result, "----------------------");
    StringAppendF(&result, "----------------------\n");
}

std::string toString(PredictionState predictionState) {
    switch (predictionState) {
        case PredictionState::Valid:
            return "Valid";
        case PredictionState::Expired:
            return "Expired";
        case PredictionState::None:
            return "None";
    }
}

std::string jankTypeBitmaskToString(int32_t jankType) {
    if (jankType == JankType::None) {
        return "None";
    }

    std::vector<std::string> janks;
    if (jankType & JankType::DisplayHAL) {
        janks.emplace_back("Display HAL");
        jankType &= ~JankType::DisplayHAL;
    }
    if (jankType & JankType::SurfaceFlingerCpuDeadlineMissed) {
        janks.emplace_back("SurfaceFlinger CPU Deadline Missed");
        jankType &= ~JankType::SurfaceFlingerCpuDeadlineMissed;
    }
    if (jankType & JankType::SurfaceFlingerGpuDeadlineMissed) {
        janks.emplace_back("SurfaceFlinger GPU Deadline Missed");
        jankType &= ~JankType::SurfaceFlingerGpuDeadlineMissed;
    }
    if (jankType & JankType::AppDeadlineMissed) {
        janks.emplace_back("App Deadline Missed");
        jankType &= ~JankType::AppDeadlineMissed;
    }
    if (jankType & JankType::PredictionError) {
        janks.emplace_back("Prediction Error");
        jankType &= ~JankType::PredictionError;
    }
    if (jankType & JankType::SurfaceFlingerScheduling) {
        janks.emplace_back("SurfaceFlinger Scheduling");
        jankType &= ~JankType::SurfaceFlingerScheduling;
    }
    if (jankType & JankType::BufferStuffing) {
        janks.emplace_back("Buffer Stuffing");
        jankType &= ~JankType::BufferStuffing;
    }
    if (jankType & JankType::Unknown) {
        janks.emplace_back("Unknown jank");
        jankType &= ~JankType::Unknown;
    }

    // jankType should be 0 if all types of jank were checked for.
    LOG_ALWAYS_FATAL_IF(jankType != 0, "Unrecognized jank type value 0x%x", jankType);
    return std::accumulate(janks.begin(), janks.end(), std::string(),
                           [](const std::string& l, const std::string& r) {
                               return l.empty() ? r : l + ", " + r;
                           });
}

std::string toString(FramePresentMetadata presentMetadata) {
    switch (presentMetadata) {
        case FramePresentMetadata::OnTimePresent:
            return "On Time Present";
        case FramePresentMetadata::LatePresent:
            return "Late Present";
        case FramePresentMetadata::EarlyPresent:
            return "Early Present";
        case FramePresentMetadata::UnknownPresent:
            return "Unknown Present";
    }
}

std::string toString(FrameReadyMetadata finishMetadata) {
    switch (finishMetadata) {
        case FrameReadyMetadata::OnTimeFinish:
            return "On Time Finish";
        case FrameReadyMetadata::LateFinish:
            return "Late Finish";
        case FrameReadyMetadata::UnknownFinish:
            return "Unknown Finish";
    }
}

std::string toString(FrameStartMetadata startMetadata) {
    switch (startMetadata) {
        case FrameStartMetadata::OnTimeStart:
            return "On Time Start";
        case FrameStartMetadata::LateStart:
            return "Late Start";
        case FrameStartMetadata::EarlyStart:
            return "Early Start";
        case FrameStartMetadata::UnknownStart:
            return "Unknown Start";
    }
}

std::string toString(SurfaceFrame::PresentState presentState) {
    using PresentState = SurfaceFrame::PresentState;
    switch (presentState) {
        case PresentState::Presented:
            return "Presented";
        case PresentState::Dropped:
            return "Dropped";
        case PresentState::Unknown:
            return "Unknown";
    }
}

FrameTimelineEvent::PresentType toProto(FramePresentMetadata presentMetadata) {
    switch (presentMetadata) {
        case FramePresentMetadata::EarlyPresent:
            return FrameTimelineEvent::PRESENT_EARLY;
        case FramePresentMetadata::LatePresent:
            return FrameTimelineEvent::PRESENT_LATE;
        case FramePresentMetadata::OnTimePresent:
            return FrameTimelineEvent::PRESENT_ON_TIME;
        case FramePresentMetadata::UnknownPresent:
            return FrameTimelineEvent::PRESENT_UNSPECIFIED;
    }
}

int32_t jankTypeBitmaskToProto(int32_t jankType) {
    if (jankType == JankType::None) {
        return FrameTimelineEvent::JANK_NONE;
    }

    int32_t protoJank = 0;
    if (jankType & JankType::DisplayHAL) {
        protoJank |= FrameTimelineEvent::JANK_DISPLAY_HAL;
        jankType &= ~JankType::DisplayHAL;
    }
    if (jankType & JankType::SurfaceFlingerCpuDeadlineMissed) {
        protoJank |= FrameTimelineEvent::JANK_SF_CPU_DEADLINE_MISSED;
        jankType &= ~JankType::SurfaceFlingerCpuDeadlineMissed;
    }
    if (jankType & JankType::SurfaceFlingerGpuDeadlineMissed) {
        protoJank |= FrameTimelineEvent::JANK_SF_GPU_DEADLINE_MISSED;
        jankType &= ~JankType::SurfaceFlingerGpuDeadlineMissed;
    }
    if (jankType & JankType::AppDeadlineMissed) {
        protoJank |= FrameTimelineEvent::JANK_APP_DEADLINE_MISSED;
        jankType &= ~JankType::AppDeadlineMissed;
    }
    if (jankType & JankType::PredictionError) {
        protoJank |= FrameTimelineEvent::JANK_PREDICTION_ERROR;
        jankType &= ~JankType::PredictionError;
    }
    if (jankType & JankType::SurfaceFlingerScheduling) {
        protoJank |= FrameTimelineEvent::JANK_SF_SCHEDULING;
        jankType &= ~JankType::SurfaceFlingerScheduling;
    }
    if (jankType & JankType::BufferStuffing) {
        protoJank |= FrameTimelineEvent::JANK_BUFFER_STUFFING;
        jankType &= ~JankType::BufferStuffing;
    }
    if (jankType & JankType::Unknown) {
        protoJank |= FrameTimelineEvent::JANK_UNKNOWN;
        jankType &= ~JankType::Unknown;
    }

    // jankType should be 0 if all types of jank were checked for.
    LOG_ALWAYS_FATAL_IF(jankType != 0, "Unrecognized jank type value 0x%x", jankType);
    return protoJank;
}

// Returns the smallest timestamp from the set of predictions and actuals.
nsecs_t getMinTime(PredictionState predictionState, TimelineItem predictions,
                   TimelineItem actuals) {
    nsecs_t minTime = std::numeric_limits<nsecs_t>::max();
    if (predictionState == PredictionState::Valid) {
        // Checking start time for predictions is enough because start time is always lesser than
        // endTime and presentTime.
        minTime = std::min(minTime, predictions.startTime);
    }

    // Need to check startTime, endTime and presentTime for actuals because some frames might not
    // have them set.
    if (actuals.startTime != 0) {
        minTime = std::min(minTime, actuals.startTime);
    }
    if (actuals.endTime != 0) {
        minTime = std::min(minTime, actuals.endTime);
    }
    if (actuals.presentTime != 0) {
        minTime = std::min(minTime, actuals.endTime);
    }
    return minTime;
}

int64_t TraceCookieCounter::getCookieForTracing() {
    return ++mTraceCookie;
}

SurfaceFrame::SurfaceFrame(const FrameTimelineInfo& frameTimelineInfo, pid_t ownerPid,
                           uid_t ownerUid, std::string layerName, std::string debugName,
                           PredictionState predictionState,
                           frametimeline::TimelineItem&& predictions,
                           std::shared_ptr<TimeStats> timeStats,
                           JankClassificationThresholds thresholds,
                           TraceCookieCounter* traceCookieCounter)
      : mToken(frameTimelineInfo.vsyncId),
        mInputEventId(frameTimelineInfo.inputEventId),
        mOwnerPid(ownerPid),
        mOwnerUid(ownerUid),
        mLayerName(std::move(layerName)),
        mDebugName(std::move(debugName)),
        mPresentState(PresentState::Unknown),
        mPredictionState(predictionState),
        mPredictions(predictions),
        mActuals({0, 0, 0}),
        mTimeStats(timeStats),
        mJankClassificationThresholds(thresholds),
        mTraceCookieCounter(*traceCookieCounter) {}

void SurfaceFrame::setActualStartTime(nsecs_t actualStartTime) {
    std::scoped_lock lock(mMutex);
    mActuals.startTime = actualStartTime;
}

void SurfaceFrame::setActualQueueTime(nsecs_t actualQueueTime) {
    std::scoped_lock lock(mMutex);
    mActualQueueTime = actualQueueTime;
}
void SurfaceFrame::setAcquireFenceTime(nsecs_t acquireFenceTime) {
    std::scoped_lock lock(mMutex);
    mActuals.endTime = std::max(acquireFenceTime, mActualQueueTime);
}

void SurfaceFrame::setPresentState(PresentState presentState, nsecs_t lastLatchTime) {
    std::scoped_lock lock(mMutex);
    LOG_ALWAYS_FATAL_IF(mPresentState != PresentState::Unknown,
                        "setPresentState called on a SurfaceFrame from Layer - %s, that has a "
                        "PresentState - %s set already.",
                        mDebugName.c_str(), toString(mPresentState).c_str());
    mPresentState = presentState;
    mLastLatchTime = lastLatchTime;
}

void SurfaceFrame::setRenderRate(Fps renderRate) {
    std::lock_guard<std::mutex> lock(mMutex);
    mRenderRate = renderRate;
}

std::optional<int32_t> SurfaceFrame::getJankType() const {
    std::scoped_lock lock(mMutex);
    if (mActuals.presentTime == 0) {
        return std::nullopt;
    }
    return mJankType;
}

nsecs_t SurfaceFrame::getBaseTime() const {
    std::scoped_lock lock(mMutex);
    return getMinTime(mPredictionState, mPredictions, mActuals);
}

TimelineItem SurfaceFrame::getActuals() const {
    std::scoped_lock lock(mMutex);
    return mActuals;
}

SurfaceFrame::PresentState SurfaceFrame::getPresentState() const {
    std::scoped_lock lock(mMutex);
    return mPresentState;
}

FramePresentMetadata SurfaceFrame::getFramePresentMetadata() const {
    std::scoped_lock lock(mMutex);
    return mFramePresentMetadata;
}

FrameReadyMetadata SurfaceFrame::getFrameReadyMetadata() const {
    std::scoped_lock lock(mMutex);
    return mFrameReadyMetadata;
}

void SurfaceFrame::dump(std::string& result, const std::string& indent, nsecs_t baseTime) const {
    std::scoped_lock lock(mMutex);
    StringAppendF(&result, "%s", indent.c_str());
    StringAppendF(&result, "Layer - %s", mDebugName.c_str());
    if (mJankType != JankType::None) {
        // Easily identify a janky Surface Frame in the dump
        StringAppendF(&result, " [*] ");
    }
    StringAppendF(&result, "\n");
    StringAppendF(&result, "%s", indent.c_str());
    StringAppendF(&result, "Token: %" PRId64 "\n", mToken);
    StringAppendF(&result, "%s", indent.c_str());
    StringAppendF(&result, "Owner Pid : %d\n", mOwnerPid);
    StringAppendF(&result, "%s", indent.c_str());
    StringAppendF(&result, "Scheduled rendering rate: %d fps\n",
                  mRenderRate ? mRenderRate->getIntValue() : 0);
    StringAppendF(&result, "%s", indent.c_str());
    StringAppendF(&result, "Present State : %s\n", toString(mPresentState).c_str());
    StringAppendF(&result, "%s", indent.c_str());
    StringAppendF(&result, "Prediction State : %s\n", toString(mPredictionState).c_str());
    StringAppendF(&result, "%s", indent.c_str());
    StringAppendF(&result, "Jank Type : %s\n", jankTypeBitmaskToString(mJankType).c_str());
    StringAppendF(&result, "%s", indent.c_str());
    StringAppendF(&result, "Present Metadata : %s\n", toString(mFramePresentMetadata).c_str());
    StringAppendF(&result, "%s", indent.c_str());
    StringAppendF(&result, "Finish Metadata: %s\n", toString(mFrameReadyMetadata).c_str());
    std::chrono::nanoseconds latchTime(
            std::max(static_cast<int64_t>(0), mLastLatchTime - baseTime));
    StringAppendF(&result, "%s", indent.c_str());
    StringAppendF(&result, "Last latch time: %10f\n",
                  std::chrono::duration<double, std::milli>(latchTime).count());
    if (mPredictionState == PredictionState::Valid) {
        nsecs_t presentDelta = mActuals.presentTime - mPredictions.presentTime;
        std::chrono::nanoseconds presentDeltaNs(std::abs(presentDelta));
        StringAppendF(&result, "%s", indent.c_str());
        StringAppendF(&result, "Present delta: %10f\n",
                      std::chrono::duration<double, std::milli>(presentDeltaNs).count());
    }
    dumpTable(result, mPredictions, mActuals, indent, mPredictionState, baseTime);
}

void SurfaceFrame::onPresent(nsecs_t presentTime, int32_t displayFrameJankType, Fps refreshRate,
                             nsecs_t displayDeadlineDelta, nsecs_t displayPresentDelta) {
    std::scoped_lock lock(mMutex);

    if (mPresentState != PresentState::Presented) {
        // No need to update dropped buffers
        return;
    }

    mActuals.presentTime = presentTime;
    // Jank Analysis for SurfaceFrame
    if (mPredictionState == PredictionState::None) {
        // Cannot do jank classification on frames that don't have a token.
        return;
    }
    if (mPredictionState == PredictionState::Expired) {
        // We do not know what happened here to classify this correctly. This could
        // potentially be AppDeadlineMissed but that's assuming no app will request frames
        // 120ms apart.
        mJankType = JankType::Unknown;
        mFramePresentMetadata = FramePresentMetadata::UnknownPresent;
        mFrameReadyMetadata = FrameReadyMetadata::UnknownFinish;
        const constexpr nsecs_t kAppDeadlineDelta = -1;
        mTimeStats->incrementJankyFrames({refreshRate, mRenderRate, mOwnerUid, mLayerName,
                                          mJankType, displayDeadlineDelta, displayPresentDelta,
                                          kAppDeadlineDelta});
        return;
    }

    const nsecs_t presentDelta = mActuals.presentTime - mPredictions.presentTime;
    const nsecs_t deadlineDelta = mActuals.endTime - mPredictions.endTime;
    const nsecs_t deltaToVsync = std::abs(presentDelta) % refreshRate.getPeriodNsecs();

    if (deadlineDelta > mJankClassificationThresholds.deadlineThreshold) {
        mFrameReadyMetadata = FrameReadyMetadata::LateFinish;
    } else {
        mFrameReadyMetadata = FrameReadyMetadata::OnTimeFinish;
    }

    if (std::abs(presentDelta) > mJankClassificationThresholds.presentThreshold) {
        mFramePresentMetadata = presentDelta > 0 ? FramePresentMetadata::LatePresent
                                                 : FramePresentMetadata::EarlyPresent;
    } else {
        mFramePresentMetadata = FramePresentMetadata::OnTimePresent;
    }

    if (mFramePresentMetadata == FramePresentMetadata::OnTimePresent) {
        // Frames presented on time are not janky.
        mJankType = JankType::None;
    } else if (mFramePresentMetadata == FramePresentMetadata::EarlyPresent) {
        if (mFrameReadyMetadata == FrameReadyMetadata::OnTimeFinish) {
            // Finish on time, Present early
            if (deltaToVsync < mJankClassificationThresholds.presentThreshold ||
                deltaToVsync >= refreshRate.getPeriodNsecs() -
                                mJankClassificationThresholds.presentThreshold) {
                // Delta factor of vsync
                mJankType = JankType::SurfaceFlingerScheduling;
            } else {
                // Delta not a factor of vsync
                mJankType = JankType::PredictionError;
            }
        } else if (mFrameReadyMetadata == FrameReadyMetadata::LateFinish) {
            // Finish late, Present early
            mJankType = JankType::Unknown;
        }
    } else {
        if (mLastLatchTime != 0 && mPredictions.endTime <= mLastLatchTime) {
            // Buffer Stuffing.
            mJankType |= JankType::BufferStuffing;
        }
        if (mFrameReadyMetadata == FrameReadyMetadata::OnTimeFinish) {
            // Finish on time, Present late
            if (displayFrameJankType != JankType::None) {
                // Propagate displayFrame's jank if it exists
                mJankType |= displayFrameJankType;
            } else {
                if (deltaToVsync < mJankClassificationThresholds.presentThreshold ||
                    deltaToVsync >= refreshRate.getPeriodNsecs() -
                                    mJankClassificationThresholds.presentThreshold) {
                    // Delta factor of vsync
                    mJankType |= JankType::SurfaceFlingerScheduling;
                } else {
                    // Delta not a factor of vsync
                    mJankType |= JankType::PredictionError;
                }
            }
        } else if (mFrameReadyMetadata == FrameReadyMetadata::LateFinish) {
            // Finish late, Present late
            if (displayFrameJankType == JankType::None) {
                // Display frame is not janky, so purely app's fault
                mJankType |= JankType::AppDeadlineMissed;
            } else {
                // Propagate DisplayFrame's jankType if it is janky
                mJankType |= displayFrameJankType;
            }
        }
    }
    mTimeStats->incrementJankyFrames({refreshRate, mRenderRate, mOwnerUid, mLayerName, mJankType,
                                      displayDeadlineDelta, displayPresentDelta, deadlineDelta});
}

/**
 * TODO(b/178637512): add inputEventId to the perfetto trace.
 */
void SurfaceFrame::trace(int64_t displayFrameToken) {
    using FrameTimelineDataSource = impl::FrameTimeline::FrameTimelineDataSource;

    int64_t expectedTimelineCookie = mTraceCookieCounter.getCookieForTracing();
    bool missingToken = false;
    // Expected timeline start
    FrameTimelineDataSource::Trace([&](FrameTimelineDataSource::TraceContext ctx) {
        std::scoped_lock lock(mMutex);
        if (mToken == FrameTimelineInfo::INVALID_VSYNC_ID) {
            ALOGD("Cannot trace SurfaceFrame - %s with invalid token", mLayerName.c_str());
            missingToken = true;
            return;
        } else if (displayFrameToken == FrameTimelineInfo::INVALID_VSYNC_ID) {
            ALOGD("Cannot trace SurfaceFrame  - %s with invalid displayFrameToken",
                  mLayerName.c_str());
            missingToken = true;
            return;
        }
        auto packet = ctx.NewTracePacket();
        packet->set_timestamp_clock_id(perfetto::protos::pbzero::BUILTIN_CLOCK_MONOTONIC);
        packet->set_timestamp(static_cast<uint64_t>(mPredictions.startTime));

        auto* event = packet->set_frame_timeline_event();
        auto* expectedSurfaceFrameStartEvent = event->set_expected_surface_frame_start();

        expectedSurfaceFrameStartEvent->set_cookie(expectedTimelineCookie);

        expectedSurfaceFrameStartEvent->set_token(mToken);
        expectedSurfaceFrameStartEvent->set_display_frame_token(displayFrameToken);

        expectedSurfaceFrameStartEvent->set_pid(mOwnerPid);
        expectedSurfaceFrameStartEvent->set_layer_name(mDebugName);
    });

    if (missingToken) {
        // If one packet can't be traced because of missing token, then no packets can be traced.
        // Exit early in this case.
        return;
    }

    // Expected timeline end
    FrameTimelineDataSource::Trace([&](FrameTimelineDataSource::TraceContext ctx) {
        std::scoped_lock lock(mMutex);
        auto packet = ctx.NewTracePacket();
        packet->set_timestamp_clock_id(perfetto::protos::pbzero::BUILTIN_CLOCK_MONOTONIC);
        packet->set_timestamp(static_cast<uint64_t>(mPredictions.endTime));

        auto* event = packet->set_frame_timeline_event();
        auto* expectedSurfaceFrameEndEvent = event->set_frame_end();

        expectedSurfaceFrameEndEvent->set_cookie(expectedTimelineCookie);
    });

    int64_t actualTimelineCookie = mTraceCookieCounter.getCookieForTracing();
    // Actual timeline start
    FrameTimelineDataSource::Trace([&](FrameTimelineDataSource::TraceContext ctx) {
        std::scoped_lock lock(mMutex);
        auto packet = ctx.NewTracePacket();
        packet->set_timestamp_clock_id(perfetto::protos::pbzero::BUILTIN_CLOCK_MONOTONIC);
        // Actual start time is not yet available, so use expected start instead
        packet->set_timestamp(static_cast<uint64_t>(mPredictions.startTime));

        auto* event = packet->set_frame_timeline_event();
        auto* actualSurfaceFrameStartEvent = event->set_actual_surface_frame_start();

        actualSurfaceFrameStartEvent->set_cookie(actualTimelineCookie);

        actualSurfaceFrameStartEvent->set_token(mToken);
        actualSurfaceFrameStartEvent->set_display_frame_token(displayFrameToken);

        actualSurfaceFrameStartEvent->set_pid(mOwnerPid);
        actualSurfaceFrameStartEvent->set_layer_name(mDebugName);

        if (mPresentState == PresentState::Dropped) {
            actualSurfaceFrameStartEvent->set_present_type(FrameTimelineEvent::PRESENT_DROPPED);
        } else if (mPresentState == PresentState::Unknown) {
            actualSurfaceFrameStartEvent->set_present_type(FrameTimelineEvent::PRESENT_UNSPECIFIED);
        } else {
            actualSurfaceFrameStartEvent->set_present_type(toProto(mFramePresentMetadata));
        }
        actualSurfaceFrameStartEvent->set_on_time_finish(mFrameReadyMetadata ==
                                                         FrameReadyMetadata::OnTimeFinish);
        actualSurfaceFrameStartEvent->set_gpu_composition(mGpuComposition);
        actualSurfaceFrameStartEvent->set_jank_type(jankTypeBitmaskToProto(mJankType));
    });
    // Actual timeline end
    FrameTimelineDataSource::Trace([&](FrameTimelineDataSource::TraceContext ctx) {
        std::scoped_lock lock(mMutex);
        auto packet = ctx.NewTracePacket();
        packet->set_timestamp_clock_id(perfetto::protos::pbzero::BUILTIN_CLOCK_MONOTONIC);
        packet->set_timestamp(static_cast<uint64_t>(mActuals.endTime));

        auto* event = packet->set_frame_timeline_event();
        auto* actualSurfaceFrameEndEvent = event->set_frame_end();

        actualSurfaceFrameEndEvent->set_cookie(actualTimelineCookie);
    });
}

namespace impl {

int64_t TokenManager::generateTokenForPredictions(TimelineItem&& predictions) {
    ATRACE_CALL();
    std::scoped_lock lock(mMutex);
    const int64_t assignedToken = mCurrentToken++;
    mPredictions[assignedToken] = {systemTime(), predictions};
    flushTokens(systemTime());
    return assignedToken;
}

std::optional<TimelineItem> TokenManager::getPredictionsForToken(int64_t token) const {
    std::scoped_lock lock(mMutex);
    auto predictionsIterator = mPredictions.find(token);
    if (predictionsIterator != mPredictions.end()) {
        return predictionsIterator->second.predictions;
    }
    return {};
}

void TokenManager::flushTokens(nsecs_t flushTime) {
    for (auto it = mPredictions.begin(); it != mPredictions.end();) {
        if (flushTime - it->second.timestamp >= kMaxRetentionTime) {
            it = mPredictions.erase(it);
        } else {
            // Tokens are ordered by time. If i'th token is within the retention time, then the
            // i+1'th token will also be within retention time.
            break;
        }
    }
}

FrameTimeline::FrameTimeline(std::shared_ptr<TimeStats> timeStats, pid_t surfaceFlingerPid,
                             JankClassificationThresholds thresholds)
      : mMaxDisplayFrames(kDefaultMaxDisplayFrames),
        mTimeStats(std::move(timeStats)),
        mSurfaceFlingerPid(surfaceFlingerPid),
        mJankClassificationThresholds(thresholds) {
    mCurrentDisplayFrame =
            std::make_shared<DisplayFrame>(mTimeStats, thresholds, &mTraceCookieCounter);
}

void FrameTimeline::onBootFinished() {
    perfetto::TracingInitArgs args;
    args.backends = perfetto::kSystemBackend;
    perfetto::Tracing::Initialize(args);
    registerDataSource();
}

void FrameTimeline::registerDataSource() {
    perfetto::DataSourceDescriptor dsd;
    dsd.set_name(kFrameTimelineDataSource);
    FrameTimelineDataSource::Register(dsd);
}

std::shared_ptr<SurfaceFrame> FrameTimeline::createSurfaceFrameForToken(
        const FrameTimelineInfo& frameTimelineInfo, pid_t ownerPid, uid_t ownerUid,
        std::string layerName, std::string debugName) {
    ATRACE_CALL();
    if (frameTimelineInfo.vsyncId == FrameTimelineInfo::INVALID_VSYNC_ID) {
        return std::make_shared<SurfaceFrame>(frameTimelineInfo, ownerPid, ownerUid,
                                              std::move(layerName), std::move(debugName),
                                              PredictionState::None, TimelineItem(), mTimeStats,
                                              mJankClassificationThresholds, &mTraceCookieCounter);
    }
    std::optional<TimelineItem> predictions =
            mTokenManager.getPredictionsForToken(frameTimelineInfo.vsyncId);
    if (predictions) {
        return std::make_shared<SurfaceFrame>(frameTimelineInfo, ownerPid, ownerUid,
                                              std::move(layerName), std::move(debugName),
                                              PredictionState::Valid, std::move(*predictions),
                                              mTimeStats, mJankClassificationThresholds,
                                              &mTraceCookieCounter);
    }
    return std::make_shared<SurfaceFrame>(frameTimelineInfo, ownerPid, ownerUid,
                                          std::move(layerName), std::move(debugName),
                                          PredictionState::Expired, TimelineItem(), mTimeStats,
                                          mJankClassificationThresholds, &mTraceCookieCounter);
}

FrameTimeline::DisplayFrame::DisplayFrame(std::shared_ptr<TimeStats> timeStats,
                                          JankClassificationThresholds thresholds,
                                          TraceCookieCounter* traceCookieCounter)
      : mSurfaceFlingerPredictions(TimelineItem()),
        mSurfaceFlingerActuals(TimelineItem()),
        mTimeStats(timeStats),
        mJankClassificationThresholds(thresholds),
        mTraceCookieCounter(*traceCookieCounter) {
    mSurfaceFrames.reserve(kNumSurfaceFramesInitial);
}

void FrameTimeline::addSurfaceFrame(std::shared_ptr<SurfaceFrame> surfaceFrame) {
    ATRACE_CALL();
    std::scoped_lock lock(mMutex);
    mCurrentDisplayFrame->addSurfaceFrame(surfaceFrame);
}

void FrameTimeline::setSfWakeUp(int64_t token, nsecs_t wakeUpTime, Fps refreshRate) {
    ATRACE_CALL();
    std::scoped_lock lock(mMutex);
    mCurrentDisplayFrame->onSfWakeUp(token, refreshRate,
                                     mTokenManager.getPredictionsForToken(token), wakeUpTime);
}

void FrameTimeline::setSfPresent(nsecs_t sfPresentTime,
                                 const std::shared_ptr<FenceTime>& presentFence) {
    ATRACE_CALL();
    std::scoped_lock lock(mMutex);
    mCurrentDisplayFrame->setActualEndTime(sfPresentTime);
    mPendingPresentFences.emplace_back(std::make_pair(presentFence, mCurrentDisplayFrame));
    flushPendingPresentFences();
    finalizeCurrentDisplayFrame();
}

void FrameTimeline::DisplayFrame::addSurfaceFrame(std::shared_ptr<SurfaceFrame> surfaceFrame) {
    mSurfaceFrames.push_back(surfaceFrame);
}

void FrameTimeline::DisplayFrame::onSfWakeUp(int64_t token, Fps refreshRate,
                                             std::optional<TimelineItem> predictions,
                                             nsecs_t wakeUpTime) {
    mToken = token;
    mRefreshRate = refreshRate;
    if (!predictions) {
        mPredictionState = PredictionState::Expired;
    } else {
        mPredictionState = PredictionState::Valid;
        mSurfaceFlingerPredictions = *predictions;
    }
    mSurfaceFlingerActuals.startTime = wakeUpTime;
}

void FrameTimeline::DisplayFrame::setPredictions(PredictionState predictionState,
                                                 TimelineItem predictions) {
    mPredictionState = predictionState;
    mSurfaceFlingerPredictions = predictions;
}

void FrameTimeline::DisplayFrame::setActualStartTime(nsecs_t actualStartTime) {
    mSurfaceFlingerActuals.startTime = actualStartTime;
}

void FrameTimeline::DisplayFrame::setActualEndTime(nsecs_t actualEndTime) {
    mSurfaceFlingerActuals.endTime = actualEndTime;
}

void FrameTimeline::DisplayFrame::onPresent(nsecs_t signalTime) {
    mSurfaceFlingerActuals.presentTime = signalTime;

    // Delta between the expected present and the actual present
    const nsecs_t presentDelta =
            mSurfaceFlingerActuals.presentTime - mSurfaceFlingerPredictions.presentTime;
    const nsecs_t deadlineDelta =
            mSurfaceFlingerActuals.endTime - mSurfaceFlingerPredictions.endTime;

    // How far off was the presentDelta when compared to the vsyncPeriod. Used in checking if there
    // was a prediction error or not.
    nsecs_t deltaToVsync = std::abs(presentDelta) % mRefreshRate.getPeriodNsecs();
    if (std::abs(presentDelta) > mJankClassificationThresholds.presentThreshold) {
        mFramePresentMetadata = presentDelta > 0 ? FramePresentMetadata::LatePresent
                                                 : FramePresentMetadata::EarlyPresent;
    } else {
        mFramePresentMetadata = FramePresentMetadata::OnTimePresent;
    }

    if (mSurfaceFlingerActuals.endTime - mSurfaceFlingerPredictions.endTime >
        mJankClassificationThresholds.deadlineThreshold) {
        mFrameReadyMetadata = FrameReadyMetadata::LateFinish;
    } else {
        mFrameReadyMetadata = FrameReadyMetadata::OnTimeFinish;
    }

    if (std::abs(mSurfaceFlingerActuals.startTime - mSurfaceFlingerPredictions.startTime) >
        mJankClassificationThresholds.startThreshold) {
        mFrameStartMetadata =
                mSurfaceFlingerActuals.startTime > mSurfaceFlingerPredictions.startTime
                ? FrameStartMetadata::LateStart
                : FrameStartMetadata::EarlyStart;
    }

    if (mFramePresentMetadata != FramePresentMetadata::OnTimePresent) {
        // Do jank classification only if present is not on time
        if (mFramePresentMetadata == FramePresentMetadata::EarlyPresent) {
            if (mFrameReadyMetadata == FrameReadyMetadata::OnTimeFinish) {
                // Finish on time, Present early
                if (deltaToVsync < mJankClassificationThresholds.presentThreshold ||
                    deltaToVsync >= (mRefreshRate.getPeriodNsecs() -
                                     mJankClassificationThresholds.presentThreshold)) {
                    // Delta is a factor of vsync if its within the presentTheshold on either side
                    // of the vsyncPeriod. Example: 0-2ms and 9-11ms are both within the threshold
                    // of the vsyncPeriod if the threshold was 2ms and the vsyncPeriod was 11ms.
                    mJankType = JankType::SurfaceFlingerScheduling;
                } else {
                    // Delta is not a factor of vsync,
                    mJankType = JankType::PredictionError;
                }
            } else if (mFrameReadyMetadata == FrameReadyMetadata::LateFinish) {
                // Finish late, Present early
                mJankType = JankType::SurfaceFlingerScheduling;
            } else {
                // Finish time unknown
                mJankType = JankType::Unknown;
            }
        } else if (mFramePresentMetadata == FramePresentMetadata::LatePresent) {
            if (mFrameReadyMetadata == FrameReadyMetadata::OnTimeFinish) {
                // Finish on time, Present late
                if (deltaToVsync < mJankClassificationThresholds.presentThreshold ||
                    deltaToVsync >= (mRefreshRate.getPeriodNsecs() -
                                     mJankClassificationThresholds.presentThreshold)) {
                    // Delta is a factor of vsync if its within the presentTheshold on either side
                    // of the vsyncPeriod. Example: 0-2ms and 9-11ms are both within the threshold
                    // of the vsyncPeriod if the threshold was 2ms and the vsyncPeriod was 11ms.
                    mJankType = JankType::DisplayHAL;
                } else {
                    // Delta is not a factor of vsync
                    mJankType = JankType::PredictionError;
                }
            } else if (mFrameReadyMetadata == FrameReadyMetadata::LateFinish) {
                // Finish late, Present late
                mJankType = JankType::SurfaceFlingerCpuDeadlineMissed;
            } else {
                // Finish time unknown
                mJankType = JankType::Unknown;
            }
        } else {
            // Present unknown
            mJankType = JankType::Unknown;
        }
    }
    for (auto& surfaceFrame : mSurfaceFrames) {
        surfaceFrame->onPresent(signalTime, mJankType, mRefreshRate, deadlineDelta, deltaToVsync);
    }
}

void FrameTimeline::DisplayFrame::trace(pid_t surfaceFlingerPid) const {
    int64_t expectedTimelineCookie = mTraceCookieCounter.getCookieForTracing();
    bool missingToken = false;
    // Expected timeline start
    FrameTimelineDataSource::Trace([&](FrameTimelineDataSource::TraceContext ctx) {
        auto packet = ctx.NewTracePacket();
        if (mToken == FrameTimelineInfo::INVALID_VSYNC_ID) {
            ALOGD("Cannot trace DisplayFrame with invalid token");
            missingToken = true;
            return;
        }
        packet->set_timestamp_clock_id(perfetto::protos::pbzero::BUILTIN_CLOCK_MONOTONIC);
        packet->set_timestamp(static_cast<uint64_t>(mSurfaceFlingerPredictions.startTime));

        auto* event = packet->set_frame_timeline_event();
        auto* expectedDisplayFrameStartEvent = event->set_expected_display_frame_start();

        expectedDisplayFrameStartEvent->set_cookie(expectedTimelineCookie);

        expectedDisplayFrameStartEvent->set_token(mToken);
        expectedDisplayFrameStartEvent->set_pid(surfaceFlingerPid);
    });

    if (missingToken) {
        // If one packet can't be traced because of missing token, then no packets can be traced.
        // Exit early in this case.
        return;
    }

    // Expected timeline end
    FrameTimelineDataSource::Trace([&](FrameTimelineDataSource::TraceContext ctx) {
        auto packet = ctx.NewTracePacket();
        packet->set_timestamp_clock_id(perfetto::protos::pbzero::BUILTIN_CLOCK_MONOTONIC);
        packet->set_timestamp(static_cast<uint64_t>(mSurfaceFlingerPredictions.endTime));

        auto* event = packet->set_frame_timeline_event();
        auto* expectedDisplayFrameEndEvent = event->set_frame_end();

        expectedDisplayFrameEndEvent->set_cookie(expectedTimelineCookie);
    });

    int64_t actualTimelineCookie = mTraceCookieCounter.getCookieForTracing();
    // Expected timeline start
    FrameTimelineDataSource::Trace([&](FrameTimelineDataSource::TraceContext ctx) {
        auto packet = ctx.NewTracePacket();
        packet->set_timestamp_clock_id(perfetto::protos::pbzero::BUILTIN_CLOCK_MONOTONIC);
        packet->set_timestamp(static_cast<uint64_t>(mSurfaceFlingerActuals.startTime));

        auto* event = packet->set_frame_timeline_event();
        auto* actualDisplayFrameStartEvent = event->set_actual_display_frame_start();

        actualDisplayFrameStartEvent->set_cookie(actualTimelineCookie);

        actualDisplayFrameStartEvent->set_token(mToken);
        actualDisplayFrameStartEvent->set_pid(surfaceFlingerPid);

        actualDisplayFrameStartEvent->set_present_type(toProto(mFramePresentMetadata));
        actualDisplayFrameStartEvent->set_on_time_finish(mFrameReadyMetadata ==
                                                         FrameReadyMetadata::OnTimeFinish);
        actualDisplayFrameStartEvent->set_gpu_composition(mGpuComposition);
        actualDisplayFrameStartEvent->set_jank_type(jankTypeBitmaskToProto(mJankType));
    });
    // Expected timeline end
    FrameTimelineDataSource::Trace([&](FrameTimelineDataSource::TraceContext ctx) {
        auto packet = ctx.NewTracePacket();
        packet->set_timestamp_clock_id(perfetto::protos::pbzero::BUILTIN_CLOCK_MONOTONIC);
        packet->set_timestamp(static_cast<uint64_t>(mSurfaceFlingerActuals.endTime));

        auto* event = packet->set_frame_timeline_event();
        auto* actualDisplayFrameEndEvent = event->set_frame_end();

        actualDisplayFrameEndEvent->set_cookie(actualTimelineCookie);
    });

    for (auto& surfaceFrame : mSurfaceFrames) {
        surfaceFrame->trace(mToken);
    }
}

void FrameTimeline::flushPendingPresentFences() {
    for (size_t i = 0; i < mPendingPresentFences.size(); i++) {
        const auto& pendingPresentFence = mPendingPresentFences[i];
        nsecs_t signalTime = Fence::SIGNAL_TIME_INVALID;
        if (pendingPresentFence.first && pendingPresentFence.first->isValid()) {
            signalTime = pendingPresentFence.first->getSignalTime();
            if (signalTime == Fence::SIGNAL_TIME_PENDING) {
                continue;
            }
        }
        if (signalTime != Fence::SIGNAL_TIME_INVALID) {
            auto& displayFrame = pendingPresentFence.second;
            displayFrame->onPresent(signalTime);
            displayFrame->trace(mSurfaceFlingerPid);
        }

        mPendingPresentFences.erase(mPendingPresentFences.begin() + static_cast<int>(i));
        --i;
    }
}

void FrameTimeline::finalizeCurrentDisplayFrame() {
    while (mDisplayFrames.size() >= mMaxDisplayFrames) {
        // We maintain only a fixed number of frames' data. Pop older frames
        mDisplayFrames.pop_front();
    }
    mDisplayFrames.push_back(mCurrentDisplayFrame);
    mCurrentDisplayFrame.reset();
    mCurrentDisplayFrame = std::make_shared<DisplayFrame>(mTimeStats, mJankClassificationThresholds,
                                                          &mTraceCookieCounter);
}

nsecs_t FrameTimeline::DisplayFrame::getBaseTime() const {
    nsecs_t baseTime =
            getMinTime(mPredictionState, mSurfaceFlingerPredictions, mSurfaceFlingerActuals);
    for (const auto& surfaceFrame : mSurfaceFrames) {
        nsecs_t surfaceFrameBaseTime = surfaceFrame->getBaseTime();
        if (surfaceFrameBaseTime != 0) {
            baseTime = std::min(baseTime, surfaceFrameBaseTime);
        }
    }
    return baseTime;
}

void FrameTimeline::DisplayFrame::dumpJank(std::string& result, nsecs_t baseTime,
                                           int displayFrameCount) const {
    if (mJankType == JankType::None) {
        // Check if any Surface Frame has been janky
        bool isJanky = false;
        for (const auto& surfaceFrame : mSurfaceFrames) {
            if (surfaceFrame->getJankType() != JankType::None) {
                isJanky = true;
                break;
            }
        }
        if (!isJanky) {
            return;
        }
    }
    StringAppendF(&result, "Display Frame %d", displayFrameCount);
    dump(result, baseTime);
}

void FrameTimeline::DisplayFrame::dumpAll(std::string& result, nsecs_t baseTime) const {
    dump(result, baseTime);
}

void FrameTimeline::DisplayFrame::dump(std::string& result, nsecs_t baseTime) const {
    if (mJankType != JankType::None) {
        // Easily identify a janky Display Frame in the dump
        StringAppendF(&result, " [*] ");
    }
    StringAppendF(&result, "\n");
    StringAppendF(&result, "Prediction State : %s\n", toString(mPredictionState).c_str());
    StringAppendF(&result, "Jank Type : %s\n", jankTypeBitmaskToString(mJankType).c_str());
    StringAppendF(&result, "Present Metadata : %s\n", toString(mFramePresentMetadata).c_str());
    StringAppendF(&result, "Finish Metadata: %s\n", toString(mFrameReadyMetadata).c_str());
    StringAppendF(&result, "Start Metadata: %s\n", toString(mFrameStartMetadata).c_str());
    std::chrono::nanoseconds vsyncPeriod(mRefreshRate.getPeriodNsecs());
    StringAppendF(&result, "Vsync Period: %10f\n",
                  std::chrono::duration<double, std::milli>(vsyncPeriod).count());
    nsecs_t presentDelta =
            mSurfaceFlingerActuals.presentTime - mSurfaceFlingerPredictions.presentTime;
    std::chrono::nanoseconds presentDeltaNs(std::abs(presentDelta));
    StringAppendF(&result, "Present delta: %10f\n",
                  std::chrono::duration<double, std::milli>(presentDeltaNs).count());
    std::chrono::nanoseconds deltaToVsync(std::abs(presentDelta) % mRefreshRate.getPeriodNsecs());
    StringAppendF(&result, "Present delta %% refreshrate: %10f\n",
                  std::chrono::duration<double, std::milli>(deltaToVsync).count());
    dumpTable(result, mSurfaceFlingerPredictions, mSurfaceFlingerActuals, "", mPredictionState,
              baseTime);
    StringAppendF(&result, "\n");
    std::string indent = "    "; // 4 spaces
    for (const auto& surfaceFrame : mSurfaceFrames) {
        surfaceFrame->dump(result, indent, baseTime);
    }
    StringAppendF(&result, "\n");
}

void FrameTimeline::dumpAll(std::string& result) {
    std::scoped_lock lock(mMutex);
    StringAppendF(&result, "Number of display frames : %d\n", (int)mDisplayFrames.size());
    nsecs_t baseTime = (mDisplayFrames.empty()) ? 0 : mDisplayFrames[0]->getBaseTime();
    for (size_t i = 0; i < mDisplayFrames.size(); i++) {
        StringAppendF(&result, "Display Frame %d", static_cast<int>(i));
        mDisplayFrames[i]->dumpAll(result, baseTime);
    }
}

void FrameTimeline::dumpJank(std::string& result) {
    std::scoped_lock lock(mMutex);
    nsecs_t baseTime = (mDisplayFrames.empty()) ? 0 : mDisplayFrames[0]->getBaseTime();
    for (size_t i = 0; i < mDisplayFrames.size(); i++) {
        mDisplayFrames[i]->dumpJank(result, baseTime, static_cast<int>(i));
    }
}

void FrameTimeline::parseArgs(const Vector<String16>& args, std::string& result) {
    ATRACE_CALL();
    std::unordered_map<std::string, bool> argsMap;
    for (size_t i = 0; i < args.size(); i++) {
        argsMap[std::string(String8(args[i]).c_str())] = true;
    }
    if (argsMap.count("-jank")) {
        dumpJank(result);
    }
    if (argsMap.count("-all")) {
        dumpAll(result);
    }
}

void FrameTimeline::setMaxDisplayFrames(uint32_t size) {
    std::scoped_lock lock(mMutex);

    // The size can either increase or decrease, clear everything, to be consistent
    mDisplayFrames.clear();
    mPendingPresentFences.clear();
    mMaxDisplayFrames = size;
}

void FrameTimeline::reset() {
    setMaxDisplayFrames(kDefaultMaxDisplayFrames);
}

} // namespace impl
} // namespace android::frametimeline

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

#pragma once

#include <utils/Timers.h>

#include <chrono>
#include <deque>

#include "LayerHistory.h"
#include "RefreshRateConfigs.h"
#include "Scheduler/Seamlessness.h"
#include "SchedulerUtils.h"

namespace android {

class Layer;

namespace scheduler {

using namespace std::chrono_literals;

// Maximum period between presents for a layer to be considered active.
constexpr std::chrono::nanoseconds MAX_ACTIVE_LAYER_PERIOD_NS = 1200ms;

// Earliest present time for a layer to be considered active.
constexpr nsecs_t getActiveLayerThreshold(nsecs_t now) {
    return now - MAX_ACTIVE_LAYER_PERIOD_NS.count();
}

// Stores history of present times and refresh rates for a layer.
class LayerInfo {
    using LayerUpdateType = LayerHistory::LayerUpdateType;

    // Layer is considered frequent if the earliest value in the window of most recent present times
    // is within a threshold. If a layer is infrequent, its average refresh rate is disregarded in
    // favor of a low refresh rate.
    static constexpr size_t kFrequentLayerWindowSize = 3;
    static constexpr Fps kMinFpsForFrequentLayer{10.0f};
    static constexpr auto kMaxPeriodForFrequentLayerNs =
            std::chrono::nanoseconds(kMinFpsForFrequentLayer.getPeriodNsecs()) + 1ms;

    friend class LayerHistoryTest;
    friend class LayerInfoTest;

public:
    // Holds information about the layer vote
    struct LayerVote {
        LayerHistory::LayerVoteType type = LayerHistory::LayerVoteType::Heuristic;
        Fps fps{0.0f};
        Seamlessness seamlessness = Seamlessness::Default;
    };

    static void setTraceEnabled(bool enabled) { sTraceEnabled = enabled; }

    static void setRefreshRateConfigs(const RefreshRateConfigs& refreshRateConfigs) {
        sRefreshRateConfigs = &refreshRateConfigs;
    }

    LayerInfo(const std::string& name, LayerHistory::LayerVoteType defaultVote);

    LayerInfo(const LayerInfo&) = delete;
    LayerInfo& operator=(const LayerInfo&) = delete;

    // Records the last requested present time. It also stores information about when
    // the layer was last updated. If the present time is farther in the future than the
    // updated time, the updated time is the present time.
    void setLastPresentTime(nsecs_t lastPresentTime, nsecs_t now, LayerUpdateType updateType,
                            bool pendingModeChange);

    // Sets an explicit layer vote. This usually comes directly from the application via
    // ANativeWindow_setFrameRate API
    void setLayerVote(LayerVote vote) { mLayerVote = vote; }

    // Sets the default layer vote. This will be the layer vote after calling to resetLayerVote().
    // This is used for layers that called to setLayerVote() and then removed the vote, so that the
    // layer can go back to whatever vote it had before the app voted for it.
    void setDefaultLayerVote(LayerHistory::LayerVoteType type) { mDefaultVote = type; }

    // Resets the layer vote to its default.
    void resetLayerVote() { mLayerVote = {mDefaultVote, Fps(0.0f), Seamlessness::Default}; }

    LayerVote getRefreshRateVote(nsecs_t now);

    // Return the last updated time. If the present time is farther in the future than the
    // updated time, the updated time is the present time.
    nsecs_t getLastUpdatedTime() const { return mLastUpdatedTime; }

    // Returns a C string for tracing a vote
    const char* getTraceTag(LayerHistory::LayerVoteType type) const;

    void onLayerInactive(nsecs_t now) {
        // Mark mFrameTimeValidSince to now to ignore all previous frame times.
        // We are not deleting the old frame to keep track of whether we should treat the first
        // buffer as Max as we don't know anything about this layer or Min as this layer is
        // posting infrequent updates.
        const auto timePoint = std::chrono::nanoseconds(now);
        mFrameTimeValidSince = std::chrono::time_point<std::chrono::steady_clock>(timePoint);
        mLastRefreshRate = {};
        mRefreshRateHistory.clear();
    }

    void clearHistory(nsecs_t now) {
        onLayerInactive(now);
        mFrameTimes.clear();
    }

private:
    // Used to store the layer timestamps
    struct FrameTimeData {
        nsecs_t presentTime; // desiredPresentTime, if provided
        nsecs_t queueTime;  // buffer queue time
        bool pendingModeChange;
    };

    // Holds information about the calculated and reported refresh rate
    struct RefreshRateHeuristicData {
        // Rate calculated on the layer
        Fps calculated{0.0f};
        // Last reported rate for LayerInfo::getRefreshRate()
        Fps reported{0.0f};
        // Whether the last reported rate for LayerInfo::getRefreshRate()
        // was due to animation or infrequent updates
        bool animatingOrInfrequent = false;
    };

    // Class to store past calculated refresh rate and determine whether
    // the refresh rate calculated is consistent with past values
    class RefreshRateHistory {
    public:
        static constexpr auto HISTORY_SIZE = 90;
        static constexpr std::chrono::nanoseconds HISTORY_DURATION = 2s;

        RefreshRateHistory(const std::string& name) : mName(name) {}

        // Clears History
        void clear();

        // Adds a new refresh rate and returns true if it is consistent
        bool add(Fps refreshRate, nsecs_t now);

    private:
        friend class LayerHistoryTest;

        // Holds the refresh rate when it was calculated
        struct RefreshRateData {
            Fps refreshRate{0.0f};
            nsecs_t timestamp = 0;

            bool operator<(const RefreshRateData& other) const {
                // We don't need comparison with margins since we are using
                // this to find the min and max refresh rates.
                return refreshRate.getValue() < other.refreshRate.getValue();
            }
        };

        // Holds tracing strings
        struct HeuristicTraceTagData {
            std::string min;
            std::string max;
            std::string consistent;
            std::string average;
        };

        bool isConsistent() const;
        HeuristicTraceTagData makeHeuristicTraceTagData() const;

        const std::string mName;
        mutable std::optional<HeuristicTraceTagData> mHeuristicTraceTagData;
        std::deque<RefreshRateData> mRefreshRates;
        static constexpr float MARGIN_CONSISTENT_FPS = 1.0;
    };

    bool isFrequent(nsecs_t now) const;
    bool isAnimating(nsecs_t now) const;
    bool hasEnoughDataForHeuristic() const;
    std::optional<Fps> calculateRefreshRateIfPossible(nsecs_t now);
    std::optional<nsecs_t> calculateAverageFrameTime() const;
    bool isFrameTimeValid(const FrameTimeData&) const;

    const std::string mName;

    // Used for sanitizing the heuristic data. If two frames are less than
    // this period apart from each other they'll be considered as duplicates.
    static constexpr nsecs_t kMinPeriodBetweenFrames = Fps(120.f).getPeriodNsecs();
    // Used for sanitizing the heuristic data. If two frames are more than
    // this period apart from each other, the interval between them won't be
    // taken into account when calculating average frame rate.
    static constexpr nsecs_t kMaxPeriodBetweenFrames = kMinFpsForFrequentLayer.getPeriodNsecs();
    LayerHistory::LayerVoteType mDefaultVote;

    LayerVote mLayerVote;

    nsecs_t mLastUpdatedTime = 0;

    nsecs_t mLastAnimationTime = 0;

    RefreshRateHeuristicData mLastRefreshRate;

    std::deque<FrameTimeData> mFrameTimes;
    std::chrono::time_point<std::chrono::steady_clock> mFrameTimeValidSince =
            std::chrono::steady_clock::now();
    static constexpr size_t HISTORY_SIZE = RefreshRateHistory::HISTORY_SIZE;
    static constexpr std::chrono::nanoseconds HISTORY_DURATION = 1s;

    RefreshRateHistory mRefreshRateHistory;

    mutable std::unordered_map<LayerHistory::LayerVoteType, std::string> mTraceTags;

    // Shared for all LayerInfo instances
    static const RefreshRateConfigs* sRefreshRateConfigs;
    static bool sTraceEnabled;
};

} // namespace scheduler
} // namespace android

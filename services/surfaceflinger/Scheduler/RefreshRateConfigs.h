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

#pragma once

#include <android-base/stringprintf.h>
#include <gui/DisplayEventReceiver.h>

#include <algorithm>
#include <numeric>
#include <optional>
#include <type_traits>

#include "DisplayHardware/DisplayMode.h"
#include "DisplayHardware/HWComposer.h"
#include "Fps.h"
#include "Scheduler/SchedulerUtils.h"
#include "Scheduler/Seamlessness.h"
#include "Scheduler/StrongTyping.h"

namespace android::scheduler {

using namespace std::chrono_literals;

enum class RefreshRateConfigEvent : unsigned { None = 0b0, Changed = 0b1 };

inline RefreshRateConfigEvent operator|(RefreshRateConfigEvent lhs, RefreshRateConfigEvent rhs) {
    using T = std::underlying_type_t<RefreshRateConfigEvent>;
    return static_cast<RefreshRateConfigEvent>(static_cast<T>(lhs) | static_cast<T>(rhs));
}

using FrameRateOverride = DisplayEventReceiver::Event::FrameRateOverride;

/**
 * This class is used to encapsulate configuration for refresh rates. It holds information
 * about available refresh rates on the device, and the mapping between the numbers and human
 * readable names.
 */
class RefreshRateConfigs {
public:
    // Margin used when matching refresh rates to the content desired ones.
    static constexpr nsecs_t MARGIN_FOR_PERIOD_CALCULATION =
        std::chrono::nanoseconds(800us).count();

    class RefreshRate {
    private:
        // Effectively making the constructor private while allowing
        // std::make_unique to create the object
        struct ConstructorTag {
            explicit ConstructorTag(int) {}
        };

    public:
        RefreshRate(DisplayModeId configId, DisplayModePtr config, Fps fps, ConstructorTag)
              : configId(configId), hwcConfig(config), fps(std::move(fps)) {}

        DisplayModeId getConfigId() const { return configId; }
        nsecs_t getVsyncPeriod() const { return hwcConfig->getVsyncPeriod(); }
        int32_t getConfigGroup() const { return hwcConfig->getConfigGroup(); }
        std::string getName() const { return to_string(fps); }
        Fps getFps() const { return fps; }

        // Checks whether the fps of this RefreshRate struct is within a given min and max refresh
        // rate passed in. Margin of error is applied to the boundaries for approximation.
        bool inPolicy(Fps minRefreshRate, Fps maxRefreshRate) const {
            return minRefreshRate.lessThanOrEqualWithMargin(fps) &&
                    fps.lessThanOrEqualWithMargin(maxRefreshRate);
        }

        bool operator!=(const RefreshRate& other) const {
            return configId != other.configId || hwcConfig != other.hwcConfig;
        }

        bool operator<(const RefreshRate& other) const {
            return getFps().getValue() < other.getFps().getValue();
        }

        bool operator==(const RefreshRate& other) const { return !(*this != other); }

        std::string toString() const;
        friend std::ostream& operator<<(std::ostream& os, const RefreshRate& refreshRate) {
            return os << refreshRate.toString();
        }

    private:
        friend RefreshRateConfigs;
        friend class RefreshRateConfigsTest;

        // This config ID corresponds to the position of the config in the vector that is stored
        // on the device.
        const DisplayModeId configId;
        // The config itself
        DisplayModePtr hwcConfig;
        // Refresh rate in frames per second
        const Fps fps{0.0f};
    };

    using AllRefreshRatesMapType =
            std::unordered_map<DisplayModeId, std::unique_ptr<const RefreshRate>>;

    struct FpsRange {
        Fps min{0.0f};
        Fps max{std::numeric_limits<float>::max()};

        bool operator==(const FpsRange& other) const {
            return min.equalsWithMargin(other.min) && max.equalsWithMargin(other.max);
        }

        bool operator!=(const FpsRange& other) const { return !(*this == other); }

        std::string toString() const {
            return base::StringPrintf("[%s %s]", to_string(min).c_str(), to_string(max).c_str());
        }
    };

    struct Policy {
    private:
        static constexpr int kAllowGroupSwitchingDefault = false;

    public:
        // The default config, used to ensure we only initiate display config switches within the
        // same config group as defaultConfigId's group.
        DisplayModeId defaultConfig;
        // Whether or not we switch config groups to get the best frame rate.
        bool allowGroupSwitching = kAllowGroupSwitchingDefault;
        // The primary refresh rate range represents display manager's general guidance on the
        // display configs we'll consider when switching refresh rates. Unless we get an explicit
        // signal from an app, we should stay within this range.
        FpsRange primaryRange;
        // The app request refresh rate range allows us to consider more display configs when
        // switching refresh rates. Although we should generally stay within the primary range,
        // specific considerations, such as layer frame rate settings specified via the
        // setFrameRate() api, may cause us to go outside the primary range. We never go outside the
        // app request range. The app request range will be greater than or equal to the primary
        // refresh rate range, never smaller.
        FpsRange appRequestRange;

        Policy() = default;

        Policy(DisplayModeId defaultConfig, const FpsRange& range)
              : Policy(defaultConfig, kAllowGroupSwitchingDefault, range, range) {}

        Policy(DisplayModeId defaultConfig, bool allowGroupSwitching, const FpsRange& range)
              : Policy(defaultConfig, allowGroupSwitching, range, range) {}

        Policy(DisplayModeId defaultConfig, const FpsRange& primaryRange,
               const FpsRange& appRequestRange)
              : Policy(defaultConfig, kAllowGroupSwitchingDefault, primaryRange, appRequestRange) {}

        Policy(DisplayModeId defaultConfig, bool allowGroupSwitching, const FpsRange& primaryRange,
               const FpsRange& appRequestRange)
              : defaultConfig(defaultConfig),
                allowGroupSwitching(allowGroupSwitching),
                primaryRange(primaryRange),
                appRequestRange(appRequestRange) {}

        bool operator==(const Policy& other) const {
            return defaultConfig == other.defaultConfig && primaryRange == other.primaryRange &&
                    appRequestRange == other.appRequestRange &&
                    allowGroupSwitching == other.allowGroupSwitching;
        }

        bool operator!=(const Policy& other) const { return !(*this == other); }
        std::string toString() const;
    };

    // Return code set*Policy() to indicate the current policy is unchanged.
    static constexpr int CURRENT_POLICY_UNCHANGED = 1;

    // We maintain the display manager policy and the override policy separately. The override
    // policy is used by CTS tests to get a consistent device state for testing. While the override
    // policy is set, it takes precedence over the display manager policy. Once the override policy
    // is cleared, we revert to using the display manager policy.

    // Sets the display manager policy to choose refresh rates. The return value will be:
    //   - A negative value if the policy is invalid or another error occurred.
    //   - NO_ERROR if the policy was successfully updated, and the current policy is different from
    //     what it was before the call.
    //   - CURRENT_POLICY_UNCHANGED if the policy was successfully updated, but the current policy
    //     is the same as it was before the call.
    status_t setDisplayManagerPolicy(const Policy& policy) EXCLUDES(mLock);
    // Sets the override policy. See setDisplayManagerPolicy() for the meaning of the return value.
    status_t setOverridePolicy(const std::optional<Policy>& policy) EXCLUDES(mLock);
    // Gets the current policy, which will be the override policy if active, and the display manager
    // policy otherwise.
    Policy getCurrentPolicy() const EXCLUDES(mLock);
    // Gets the display manager policy, regardless of whether an override policy is active.
    Policy getDisplayManagerPolicy() const EXCLUDES(mLock);

    // Returns true if config is allowed by the current policy.
    bool isConfigAllowed(DisplayModeId config) const EXCLUDES(mLock);

    // Describes the different options the layer voted for refresh rate
    enum class LayerVoteType {
        NoVote,          // Doesn't care about the refresh rate
        Min,             // Minimal refresh rate available
        Max,             // Maximal refresh rate available
        Heuristic,       // Specific refresh rate that was calculated by platform using a heuristic
        ExplicitDefault, // Specific refresh rate that was provided by the app with Default
                         // compatibility
        ExplicitExactOrMultiple, // Specific refresh rate that was provided by the app with
                                 // ExactOrMultiple compatibility
        ExplicitExact,           // Specific refresh rate that was provided by the app with
                                 // Exact compatibility

    };

    // Captures the layer requirements for a refresh rate. This will be used to determine the
    // display refresh rate.
    struct LayerRequirement {
        // Layer's name. Used for debugging purposes.
        std::string name;
        // Layer's owner uid
        uid_t ownerUid = static_cast<uid_t>(-1);
        // Layer vote type.
        LayerVoteType vote = LayerVoteType::NoVote;
        // Layer's desired refresh rate, if applicable.
        Fps desiredRefreshRate{0.0f};
        // If a seamless mode switch is required.
        Seamlessness seamlessness = Seamlessness::Default;
        // Layer's weight in the range of [0, 1]. The higher the weight the more impact this layer
        // would have on choosing the refresh rate.
        float weight = 0.0f;
        // Whether layer is in focus or not based on WindowManager's state
        bool focused = false;

        bool operator==(const LayerRequirement& other) const {
            return name == other.name && vote == other.vote &&
                    desiredRefreshRate.equalsWithMargin(other.desiredRefreshRate) &&
                    seamlessness == other.seamlessness && weight == other.weight &&
                    focused == other.focused;
        }

        bool operator!=(const LayerRequirement& other) const { return !(*this == other); }
    };

    // Global state describing signals that affect refresh rate choice.
    struct GlobalSignals {
        // Whether the user touched the screen recently. Used to apply touch boost.
        bool touch = false;
        // True if the system hasn't seen any buffers posted to layers recently.
        bool idle = false;
    };

    // Returns the refresh rate that fits best to the given layers.
    //   layers - The layer requirements to consider.
    //   globalSignals - global state of touch and idle
    //   outSignalsConsidered - An output param that tells the caller whether the refresh rate was
    //                          chosen based on touch boost and/or idle timer.
    RefreshRate getBestRefreshRate(const std::vector<LayerRequirement>& layers,
                                   const GlobalSignals& globalSignals,
                                   GlobalSignals* outSignalsConsidered = nullptr) const
            EXCLUDES(mLock);

    FpsRange getSupportedRefreshRateRange() const EXCLUDES(mLock) {
        std::lock_guard lock(mLock);
        return {mMinSupportedRefreshRate->getFps(), mMaxSupportedRefreshRate->getFps()};
    }

    std::optional<Fps> onKernelTimerChanged(std::optional<DisplayModeId> desiredActiveConfigId,
                                            bool timerExpired) const EXCLUDES(mLock);

    // Returns the highest refresh rate according to the current policy. May change at runtime. Only
    // uses the primary range, not the app request range.
    RefreshRate getMaxRefreshRateByPolicy() const EXCLUDES(mLock);

    // Returns the current refresh rate
    RefreshRate getCurrentRefreshRate() const EXCLUDES(mLock);

    // Returns the current refresh rate, if allowed. Otherwise the default that is allowed by
    // the policy.
    RefreshRate getCurrentRefreshRateByPolicy() const;

    // Returns the refresh rate that corresponds to a DisplayModeId. This may change at
    // runtime.
    // TODO(b/159590486) An invalid config id may be given here if the dipslay configs have changed.
    RefreshRate getRefreshRateFromConfigId(DisplayModeId configId) const EXCLUDES(mLock) {
        std::lock_guard lock(mLock);
        return *mRefreshRates.at(configId);
    };

    // Stores the current configId the device operates at
    void setCurrentConfigId(DisplayModeId configId) EXCLUDES(mLock);

    // Returns a string that represents the layer vote type
    static std::string layerVoteTypeString(LayerVoteType vote);

    // Returns a known frame rate that is the closest to frameRate
    Fps findClosestKnownFrameRate(Fps frameRate) const;

    RefreshRateConfigs(const DisplayModes& configs, DisplayModeId currentConfigId,
                       bool enableFrameRateOverride = false);

    void updateDisplayConfigs(const DisplayModes& configs, DisplayModeId currentConfig)
            EXCLUDES(mLock);

    // Returns whether switching configs (refresh rate or resolution) is possible.
    // TODO(b/158780872): Consider HAL support, and skip frame rate detection if the configs only
    // differ in resolution.
    bool canSwitch() const EXCLUDES(mLock) {
        std::lock_guard lock(mLock);
        return mRefreshRates.size() > 1;
    }

    // Class to enumerate options around toggling the kernel timer on and off. We have an option
    // for no change to avoid extra calls to kernel.
    enum class KernelIdleTimerAction {
        NoChange, // Do not change the idle timer.
        TurnOff,  // Turn off the idle timer.
        TurnOn    // Turn on the idle timer.
    };
    // Checks whether kernel idle timer should be active depending the policy decisions around
    // refresh rates.
    KernelIdleTimerAction getIdleTimerAction() const;

    bool supportsFrameRateOverride() const { return mSupportsFrameRateOverride; }

    // Returns a divider for the current refresh rate
    int getRefreshRateDivider(Fps frameRate) const EXCLUDES(mLock);

    using UidToFrameRateOverride = std::map<uid_t, Fps>;
    // Returns the frame rate override for each uid.
    //
    // @param layers list of visible layers
    // @param displayFrameRate the display frame rate
    // @param touch whether touch timer is active (i.e. user touched the screen recently)
    UidToFrameRateOverride getFrameRateOverrides(const std::vector<LayerRequirement>& layers,
                                                 Fps displayFrameRate, bool touch) const
            EXCLUDES(mLock);

    void dump(std::string& result) const EXCLUDES(mLock);

private:
    friend class RefreshRateConfigsTest;

    void constructAvailableRefreshRates() REQUIRES(mLock);

    void getSortedRefreshRateListLocked(
            const std::function<bool(const RefreshRate&)>& shouldAddRefreshRate,
            std::vector<const RefreshRate*>* outRefreshRates) REQUIRES(mLock);

    // Returns the refresh rate with the highest score in the collection specified from begin
    // to end. If there are more than one with the same highest refresh rate, the first one is
    // returned.
    template <typename Iter>
    const RefreshRate* getBestRefreshRate(Iter begin, Iter end) const;

    // Returns number of display frames and remainder when dividing the layer refresh period by
    // display refresh period.
    std::pair<nsecs_t, nsecs_t> getDisplayFrames(nsecs_t layerPeriod, nsecs_t displayPeriod) const;

    // Returns the lowest refresh rate according to the current policy. May change at runtime. Only
    // uses the primary range, not the app request range.
    const RefreshRate& getMinRefreshRateByPolicyLocked() const REQUIRES(mLock);

    // Returns the highest refresh rate according to the current policy. May change at runtime. Only
    // uses the primary range, not the app request range.
    const RefreshRate& getMaxRefreshRateByPolicyLocked() const REQUIRES(mLock);

    // Returns the current refresh rate, if allowed. Otherwise the default that is allowed by
    // the policy.
    const RefreshRate& getCurrentRefreshRateByPolicyLocked() const REQUIRES(mLock);

    const Policy* getCurrentPolicyLocked() const REQUIRES(mLock);
    bool isPolicyValidLocked(const Policy& policy) const REQUIRES(mLock);

    // Return the display refresh rate divider to match the layer
    // frame rate, or 0 if the display refresh rate is not a multiple of the
    // layer refresh rate.
    static int getFrameRateDivider(Fps displayFrameRate, Fps layerFrameRate);

    // calculates a score for a layer. Used to determine the display refresh rate
    // and the frame rate override for certains applications.
    float calculateLayerScoreLocked(const LayerRequirement&, const RefreshRate&,
                                    bool isSeamlessSwitch) const REQUIRES(mLock);

    // The list of refresh rates, indexed by display config ID. This may change after this
    // object is initialized.
    AllRefreshRatesMapType mRefreshRates GUARDED_BY(mLock);

    // The list of refresh rates in the primary range of the current policy, ordered by vsyncPeriod
    // (the first element is the lowest refresh rate).
    std::vector<const RefreshRate*> mPrimaryRefreshRates GUARDED_BY(mLock);

    // The list of refresh rates in the app request range of the current policy, ordered by
    // vsyncPeriod (the first element is the lowest refresh rate).
    std::vector<const RefreshRate*> mAppRequestRefreshRates GUARDED_BY(mLock);

    // The current config. This will change at runtime. This is set by SurfaceFlinger on
    // the main thread, and read by the Scheduler (and other objects) on other threads.
    const RefreshRate* mCurrentRefreshRate GUARDED_BY(mLock);

    // The policy values will change at runtime. They're set by SurfaceFlinger on the main thread,
    // and read by the Scheduler (and other objects) on other threads.
    Policy mDisplayManagerPolicy GUARDED_BY(mLock);
    std::optional<Policy> mOverridePolicy GUARDED_BY(mLock);

    // The min and max refresh rates supported by the device.
    // This may change at runtime.
    const RefreshRate* mMinSupportedRefreshRate GUARDED_BY(mLock);
    const RefreshRate* mMaxSupportedRefreshRate GUARDED_BY(mLock);

    mutable std::mutex mLock;

    // A sorted list of known frame rates that a Heuristic layer will choose
    // from based on the closest value.
    const std::vector<Fps> mKnownFrameRates;

    const bool mEnableFrameRateOverride;
    bool mSupportsFrameRateOverride;
};

} // namespace android::scheduler

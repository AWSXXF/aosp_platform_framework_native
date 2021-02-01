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

#include <SkCanvas.h>
#include <SkImage.h>
#include <SkRuntimeEffect.h>
#include <SkSurface.h>

using namespace std;

namespace android {
namespace renderengine {
namespace skia {

/**
 * This is an implementation of a Kawase blur, as described in here:
 * https://community.arm.com/cfs-file/__key/communityserver-blogs-components-weblogfiles/
 * 00-00-00-20-66/siggraph2015_2D00_mmg_2D00_marius_2D00_notes.pdf
 */
class BlurFilter {
public:
    // Downsample FBO to improve performance
    static constexpr float kInputScale = 0.25f;
    // Downsample scale factor used to improve performance
    static constexpr float kInverseInputScale = 1.0f / kInputScale;
    // Maximum number of render passes
    static constexpr uint32_t kMaxPasses = 4;
    // To avoid downscaling artifacts, we interpolate the blurred fbo with the full composited
    // image, up to this radius.
    static constexpr float kMaxCrossFadeRadius = 30.0f;

    explicit BlurFilter();
    virtual ~BlurFilter(){};

    // Execute blur, saving it to a texture
    sk_sp<SkImage> generate(SkCanvas* canvas, const sk_sp<SkSurface> input, const uint32_t radius,
                            SkRect rect) const;
    // Returns a matrix that should be applied to the blur shader
    SkMatrix getShaderMatrix() const;

private:
    sk_sp<SkRuntimeEffect> mBlurEffect;
};

} // namespace skia
} // namespace renderengine
} // namespace android

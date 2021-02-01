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

#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include "BlurFilter.h"
#include <SkCanvas.h>
#include <SkData.h>
#include <SkPaint.h>
#include <SkRuntimeEffect.h>
#include <SkSize.h>
#include <SkString.h>
#include <SkSurface.h>
#include <log/log.h>
#include <utils/Trace.h>

namespace android {
namespace renderengine {
namespace skia {

BlurFilter::BlurFilter() {
    SkString blurString(R"(
        in shader input;
        uniform float in_inverseScale;
        uniform float2 in_blurOffset;

        half4 main(float2 xy) {
            float2 scaled_xy = float2(xy.x * in_inverseScale, xy.y * in_inverseScale);

            half4 c = sample(input, scaled_xy);
            c += sample(input, scaled_xy + float2( in_blurOffset.x,  in_blurOffset.y));
            c += sample(input, scaled_xy + float2( in_blurOffset.x, -in_blurOffset.y));
            c += sample(input, scaled_xy + float2(-in_blurOffset.x,  in_blurOffset.y));
            c += sample(input, scaled_xy + float2(-in_blurOffset.x, -in_blurOffset.y));

            return half4(c.rgb * 0.2, 1.0);
        }
    )");

    auto [blurEffect, error] = SkRuntimeEffect::Make(blurString);
    if (!blurEffect) {
        LOG_ALWAYS_FATAL("RuntimeShader error: %s", error.c_str());
    }
    mBlurEffect = std::move(blurEffect);
}

sk_sp<SkImage> BlurFilter::generate(SkCanvas* canvas, const sk_sp<SkSurface> input,
                                    const uint32_t blurRadius, SkRect rect) const {
    // Kawase is an approximation of Gaussian, but it behaves differently from it.
    // A radius transformation is required for approximating them, and also to introduce
    // non-integer steps, necessary to smoothly interpolate large radii.
    float tmpRadius = (float)blurRadius / 6.0f;
    float numberOfPasses = std::min(kMaxPasses, (uint32_t)ceil(tmpRadius));
    float radiusByPasses = tmpRadius / (float)numberOfPasses;

    SkImageInfo scaledInfo = SkImageInfo::MakeN32Premul((float)rect.width() * kInputScale,
                                                        (float)rect.height() * kInputScale);

    const float stepX = radiusByPasses;
    const float stepY = radiusByPasses;

    // start by drawing and downscaling and doing the first blur pass
    SkSamplingOptions linear(SkFilterMode::kLinear, SkMipmapMode::kNone);
    SkRuntimeShaderBuilder blurBuilder(mBlurEffect);
    blurBuilder.child("input") =
            input->makeImageSnapshot(rect.round())
                    ->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, linear);
    blurBuilder.uniform("in_inverseScale") = kInverseInputScale;
    blurBuilder.uniform("in_blurOffset") =
            SkV2{stepX * kInverseInputScale, stepY * kInverseInputScale};

    sk_sp<SkImage> tmpBlur(
            blurBuilder.makeImage(canvas->recordingContext(), nullptr, scaledInfo, false));

    // And now we'll build our chain of scaled blur stages
    blurBuilder.uniform("in_inverseScale") = 1.0f;
    for (auto i = 1; i < numberOfPasses; i++) {
        const float stepScale = (float)i * kInputScale;
        blurBuilder.child("input") =
                tmpBlur->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, linear);
        blurBuilder.uniform("in_blurOffset") = SkV2{stepX * stepScale, stepY * stepScale};
        tmpBlur = blurBuilder.makeImage(canvas->recordingContext(), nullptr, scaledInfo, false);
    }

    return tmpBlur;
}

SkMatrix BlurFilter::getShaderMatrix() const {
    return SkMatrix::Scale(kInverseInputScale, kInverseInputScale);
}

} // namespace skia
} // namespace renderengine
} // namespace android

/*
 * Copyright 2021 The Android Open Source Project
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

#include <aidl/android/hardware/graphics/common/Dataspace.h>
#include <math/vec3.h>

#include <string>
#include <vector>

namespace android::tonemap {

// Describes a shader uniform
// The shader uniform is intended to be passed into a SkRuntimeShaderBuilder, i.e.:
//
// SkRuntimeShaderBuilder builder;
// builder.uniform(<uniform name>).set(<uniform value>.data(), <uniform value>.size());
struct ShaderUniform {
    // The name of the uniform, used for binding into a shader.
    // The shader must contain a uniform whose name matches this.
    std::string name;

    // The value for the uniform, which should be bound to the uniform identified by <name>
    std::vector<uint8_t> value;
};

// Describes metadata which may be used for constructing the shader uniforms.
// This metadata should not be used for manipulating the source code of the shader program directly,
// as otherwise caching by other system of these shaders may break.
struct Metadata {
    // The maximum luminance of the display in nits
    float displayMaxLuminance = 0.0;
    // The maximum luminance of the content in nits
    float contentMaxLuminance = 0.0;
};

class ToneMapper {
public:
    virtual ~ToneMapper() {}
    // Constructs a tonemap shader whose shader language is SkSL, which tonemaps from an
    // input whose dataspace is described by sourceDataspace, to an output whose dataspace
    // is described by destinationDataspace
    //
    // The returned shader string *must* contain a function with the following signature:
    // float libtonemap_LookupTonemapGain(vec3 linearRGB, vec3 xyz);
    //
    // The arguments are:
    // * linearRGB is the absolute nits of the RGB pixels in linear space
    // * xyz is linearRGB converted into XYZ
    //
    // libtonemap_LookupTonemapGain() returns a float representing the amount by which to scale the
    // absolute nits of the pixels. This function may be plugged into any existing SkSL shader, and
    // is expected to look something like this:
    //
    // vec3 rgb = ...;
    // // apply the EOTF based on the incoming dataspace to convert to linear nits.
    // vec3 linearRGB = applyEOTF(rgb);
    // // apply a RGB->XYZ matrix float3
    // vec3 xyz = toXYZ(linearRGB);
    // // Scale the luminance based on the content standard
    // vec3 absoluteRGB = ScaleLuminance(linearRGB);
    // vec3 absoluteXYZ = ScaleLuminance(xyz);
    // float gain = libtonemap_LookupTonemapGain(absoluteRGB, absoluteXYZ);
    // // Normalize the luminance back down to a [0, 1] range
    // xyz = NormalizeLuminance(absoluteXYZ * gain);
    // // apply a XYZ->RGB matrix and apply the output OETf.
    // vec3 finalColor = applyOETF(ToRGB(xyz));
    // ...
    //
    // Helper methods in this shader should be prefixed with "libtonemap_". Accordingly, libraries
    // which consume this shader must *not* contain any methods prefixed with "libtonemap_" to
    // guarantee that there are no conflicts in name resolution.
    virtual std::string generateTonemapGainShaderSkSL(
            aidl::android::hardware::graphics::common::Dataspace sourceDataspace,
            aidl::android::hardware::graphics::common::Dataspace destinationDataspace) = 0;

    // Constructs uniform descriptions that correspond to those that are generated for the tonemap
    // shader. Uniforms must be prefixed with "in_libtonemap_". Libraries which consume this shader
    // must not bind any new uniforms that begin with this prefix.
    //
    // Downstream shaders may assume the existence of the uniform in_libtonemap_displayMaxLuminance
    // and in_libtonemap_inputMaxLuminance, in order to assist with scaling and normalizing
    // luminance as described in the documentation for generateTonemapGainShaderSkSL(). That is,
    // shaders plugging in a tone-mapping shader returned by generateTonemapGainShaderSkSL() may
    // assume that there are predefined floats in_libtonemap_displayMaxLuminance and
    // in_libtonemap_inputMaxLuminance inside of the body of the tone-mapping shader.
    virtual std::vector<ShaderUniform> generateShaderSkSLUniforms(const Metadata& metadata) = 0;

    // CPU implementation of the tonemapping gain. This must match the GPU implementation returned
    // by generateTonemapGainShaderSKSL() above, with some epsilon difference to account for
    // differences in hardware precision.
    //
    // The gain is computed assuming an input described by sourceDataspace, tonemapped to an output
    // described by destinationDataspace. To compute the gain, the input colors are provided by
    // linearRGB, which is the RGB colors in linear space. The colors in XYZ space are also
    // provided. Metadata is also provided for helping to compute the tonemapping curve.
    virtual double lookupTonemapGain(
            aidl::android::hardware::graphics::common::Dataspace sourceDataspace,
            aidl::android::hardware::graphics::common::Dataspace destinationDataspace,
            vec3 linearRGB, vec3 xyz, const Metadata& metadata) = 0;
};

// Retrieves a tonemapper instance.
// This instance is globally constructed.
ToneMapper* getToneMapper();

} // namespace android::tonemap
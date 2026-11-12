// Runs the converted Demucs network on the GPU with Core ML (macOS only).
// See scripts/export_htdemucs_coreml.py for the model and DemucsPipeline.h for what
// surrounds it.
#include "DemucsPipeline.h"

#import <CoreML/CoreML.h>
#import <Foundation/Foundation.h>

#include <algorithm>

// MLMultiArray.dataPointer and compileModelAtURL: are deprecated in the newest SDKs but
// work on every macOS this app supports.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace stems
{
namespace
{
    // Input and output names the export script gives the network.
    NSString* const inputFreq = @"x";
    NSString* const inputTime = @"xt";
    NSString* const outputFreq = @"x_out";
    NSString* const outputTime = @"xt_out";

    NSURL* path (const juce::File& file)
    {
        return [NSURL fileURLWithPath: [NSString stringWithUTF8String: file.getFullPathName().toRawUTF8()]];
    }

    juce::String describe (NSError* error)
    {
        return error != nil ? juce::String::fromUTF8 ([[error localizedDescription] UTF8String]) : juce::String ("unknown error");
    }

    // A float32 array over memory we own (nothing is copied or freed by Core ML).
    MLMultiArray* wrap (const float* data, NSArray<NSNumber*>* shape, NSArray<NSNumber*>* strides, NSError** error)
    {
        return [[MLMultiArray alloc] initWithDataPointer: const_cast<float*> (data)
                                                   shape: shape
                                                dataType: MLMultiArrayDataTypeFloat32
                                                 strides: strides
                                             deallocator: nil
                                                   error: error];
    }

    // Copies an output into contiguous memory. Core ML may pad rows, so this goes by the
    // array's own strides. The shape is [1, channels, bins, columns], or with bins == 0,
    // [1, channels, columns].
    juce::String copyOut (MLMultiArray* array, float* dest, int channels, int bins, int columns)
    {
        if (array == nil || array.dataType != MLMultiArrayDataTypeFloat32)
            return "The network's output isn't float32 (run the export script again).";

        const auto dims = bins > 0 ? 4 : 3;
        const auto shape = array.shape;
        if ((int) shape.count != dims || [shape[1] intValue] != channels || [shape[(NSUInteger) dims - 1] intValue] != columns
            || (bins > 0 && [shape[2] intValue] != bins))
            return "The network's output has an unexpected shape (run the export script again).";

        const auto* base = static_cast<const float*> (array.dataPointer);
        const auto stride = [array] (int d) { return (size_t) [array.strides[(NSUInteger) d] integerValue]; };
        const auto channelStride = stride (1);
        const auto binStride = bins > 0 ? stride (2) : (size_t) 0;
        const auto columnStride = stride (dims - 1);

        for (int c = 0; c < channels; ++c)
            for (int f = 0; f < juce::jmax (1, bins); ++f)
            {
                const auto* src = base + (size_t) c * channelStride + (size_t) f * binStride;
                auto* out = dest + ((size_t) c * (size_t) juce::jmax (1, bins) + (size_t) f) * (size_t) columns;

                if (columnStride == 1)
                    std::copy_n (src, columns, out);
                else
                    for (int t = 0; t < columns; ++t)
                        out[t] = src[(size_t) t * columnStride];
            }

        return {};
    }

    // Compiles the .mlpackage next to itself (once, and again when the package changes).
    NSURL* getCompiledModel (const juce::File& package, juce::String& error)
    {
        const auto compiled = package.withFileExtension ("mlmodelc");
        if (compiled.isDirectory() && compiled.getLastModificationTime() >= package.getLastModificationTime())
            return path (compiled);

        NSError* nsError = nil;
        NSURL* temporary = [MLModel compileModelAtURL: path (package)
                                                error: &nsError];
        if (temporary == nil)
        {
            error = "Couldn't compile the Core ML model: " + describe (nsError);
            return nil;
        }

        compiled.deleteRecursively();
        NSURL* destination = path (compiled);
        if (! [[NSFileManager defaultManager] moveItemAtURL: temporary toURL: destination error: &nsError])
            return temporary; // can't keep it next to the package: use it where it is, this once

        return destination;
    }
}

bool GpuModel::isBuiltIn() { return true; }

Separator GpuModel::load (const juce::File& file, juce::String& error)
{
    @autoreleasepool
    {
        if (@available (macOS 13.0, *))
        {}
        else
        {
            error = "The GPU model needs macOS 13 or later.";
            return nullptr;
        }

        if (! file.isDirectory())
        {
            error = "The Core ML model isn't there: run scripts/export_htdemucs_coreml.py once.";
            return nullptr;
        }

        auto* compiled = getCompiledModel (file, error);
        if (compiled == nil)
            return nullptr;

        auto* configuration = [[MLModelConfiguration alloc] init];
        configuration.computeUnits = MLComputeUnitsCPUAndGPU; // the GPU; not the Neural Engine (it can't take this network's big tensors)

        NSError* nsError = nil;
        MLModel* model = [MLModel modelWithContentsOfURL: compiled configuration: configuration error: &nsError];
        if (model == nil)
        {
            error = "Couldn't load the Core ML model: " + describe (nsError);
            return nullptr;
        }

        auto* inputs = model.modelDescription.inputDescriptionsByName;
        auto* outputs = model.modelDescription.outputDescriptionsByName;
        if (inputs[inputFreq] == nil || inputs[inputTime] == nil || outputs[outputFreq] == nil || outputs[outputTime] == nil)
        {
            error = "The Core ML model doesn't look like the one the export script makes (wrong inputs or outputs).";
            return nullptr;
        }

        constexpr int L = CoreNetwork::segmentSamples, F = CoreNetwork::freqBins, T = CoreNetwork::frames;

        auto core = std::make_shared<CoreNetwork>();
        core->run = [model] (const float* x, const float* xt, float* xOut, float* xtOut) -> juce::String
        {
            @autoreleasepool
            {
                NSError* predictError = nil;
                auto* xArray = wrap (x, @[ @1, @(CoreNetwork::freqChannels), @(F), @(T) ],
                                     @[ @((NSInteger) CoreNetwork::freqChannels * F * T), @((NSInteger) F * T), @(T), @1 ], &predictError);
                auto* xtArray = wrap (xt, @[ @1, @(CoreNetwork::timeChannels), @(L) ],
                                      @[ @((NSInteger) CoreNetwork::timeChannels * L), @(L), @1 ], &predictError);
                if (xArray == nil || xtArray == nil)
                    return "Couldn't prepare the network's input: " + describe (predictError);

                auto* provider = [[MLDictionaryFeatureProvider alloc] initWithDictionary: @{ inputFreq: xArray, inputTime: xtArray }
                                                                                    error: &predictError];
                if (provider == nil)
                    return "Couldn't prepare the network's input: " + describe (predictError);

                id<MLFeatureProvider> result = [model predictionFromFeatures: provider error: &predictError];
                if (result == nil)
                    return "The GPU couldn't run the network: " + describe (predictError);

                constexpr int S = numStems;
                if (auto problem = copyOut ([result featureValueForName: outputFreq].multiArrayValue, xOut, S * CoreNetwork::freqChannels, F, T);
                    problem.isNotEmpty())
                    return problem;

                return copyOut ([result featureValueForName: outputTime].multiArrayValue, xtOut, S * CoreNetwork::timeChannels, 0, L);
            }
        };

        // Warm up: the first prediction builds the GPU's own version of the network, which
        // takes a while; do it here, where it counts as loading. It also proves the model runs.
        {
            std::vector<float> x ((size_t) CoreNetwork::freqChannels * F * T, 0.0f), xt ((size_t) CoreNetwork::timeChannels * L, 0.0f);
            std::vector<float> xOut ((size_t) numStems * CoreNetwork::freqChannels * F * T), xtOut ((size_t) numStems * CoreNetwork::timeChannels * L);

            if (const auto problem = core->run (x.data(), xt.data(), xOut.data(), xtOut.data()); problem.isNotEmpty())
            {
                error = problem;
                return nullptr;
            }
        }

        return makeSeparator (core);
    }
}
}

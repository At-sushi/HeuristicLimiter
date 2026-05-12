/*
  ==============================================================================

   This file is part of the JUCE library.
   Copyright (c) 2020 - Raw Material Software Limited

   JUCE is an open source library subject to commercial or open-source
   licensing.

   By using JUCE, you agree to the terms of both the JUCE 6 End-User License
   Agreement and JUCE Privacy Policy (both effective as of the 16th June 2020).

   End User License Agreement: www.juce.com/juce-6-licence
   Privacy Policy: www.juce.com/juce-privacy-policy



   Or: You may also use this code under the terms of the GPL v3 (see
   www.gnu.org/licenses).

   JUCE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL WARRANTIES, WHETHER
   EXPRESSED OR IMPLIED, INCLUDING MERCHANTABILITY AND FITNESS FOR PURPOSE, ARE
   DISCLAIMED.

  ==============================================================================
*/

#include <omp.h>
#include <JuceHeader.h>

namespace dsp_original
{

        /**
            A simple compressor with standard threshold, ratio, attack time and release time
            controls.

            @tags{DSP}
        */
        template <typename SampleType, typename InnerSampleType = double>
        class LookForwardingCompressor
        {
        public:
            //==============================================================================
            /** Constructor. */
            LookForwardingCompressor()
            {
                update();
            }

            //==============================================================================
            /** Sets the threshold in dB of the compressor.*/
            void setThreshold(SampleType newThreshold)
            {
                thresholddB = newThreshold;
                update();
            }

            /** Sets the ratio of the compressor (must be higher or equal to 1).*/
            void setRatio(SampleType newRatio)
            {
                jassert(newRatio >= static_cast<SampleType> (1.0));

                ratio = newRatio;
                update();
            }

            /** Sets the attack time in milliseconds of the compressor.*/
            void setAttack(SampleType newAttack)
            {
                attackTime = newAttack;
                update();
            }

            /** Sets the release time in milliseconds of the compressor.*/
            void setRelease(SampleType newRelease)
            {
                releaseTime = newRelease;
                update();
            }

            /** Sets the look-ahead time in milliseconds of the compressor.*/
            void setLookAheadTime(SampleType newLookAheadTime)
            {
                lookAheadTime = newLookAheadTime;
                update();
            }

            // set M/S procesing enabled/disenabled
            void setMSProcessingEnabled(bool newValue) {
                useMSProcessing = newValue;
                update();
            }

            //==============================================================================
            /** Initialises the processor. */
            void prepare(const juce::dsp::ProcessSpec& spec)
            {
                jassert(spec.sampleRate > 0);
                jassert(spec.numChannels > 0);

                sampleRate = spec.sampleRate;
                numChannels = spec.numChannels;

                envelopeFilter.prepare(spec);

                update();
                reset();
            }

            /** Resets the internal state variables of the processor. */
            void reset()
            {
                envelopeFilter.reset();
            }

            //==============================================================================
            /** Processes the input and output samples supplied in the processing context. */
            template <typename ProcessContext>
            void process(const ProcessContext& context) noexcept
            {
                const auto& inputBlock = context.getInputBlock();
                auto& outputBlock = context.getOutputBlock();
                const auto numChannels = outputBlock.getNumChannels();
                const auto numSamples = outputBlock.getNumSamples();

                jassert(inputBlock.getNumChannels() == numChannels);
                jassert(inputBlock.getNumSamples() == numSamples);

                if (context.isBypassed)
                {
                    outputBlock.copyFrom(inputBlock);
                    return;
                }

                //#pragma omp parallel for
                for (size_t channel = 0; channel < numChannels; ++channel)
                {
                    auto* inputSamples = inputBlock.getChannelPointer(channel);
                    auto* outputSamples = outputBlock.getChannelPointer(channel);

                    for (size_t i = 0; i < numSamples; ++i)
                        outputSamples[i] = processSample((int)channel, inputSamples[i]);
                }
            }

            /** Performs the processing operation on a single sample at a time. */
            SampleType processSample(int channel, SampleType inputValue)
            {
                // Ballistics filter with peak rectifier
                auto env = envelopeFilter.processSample(channel, inputValue);

                // VCA
                auto gain = (env < threshold) ? static_cast<SampleType>(1.0)
                    : std::pow(env * thresholdInverse, ratioInverse - static_cast<SampleType>(1.0));

				// Look-ahead delay
				delayBuffer[channel].push_back(inputValue); // Store the current input sample in the delay buffer

                // Output
                auto output = gain * delayBuffer[channel].front();
                delayBuffer[channel].pop_front();
                return output;
            }

        private:
            //==============================================================================
            void update()
            {
                threshold = juce::Decibels::decibelsToGain(thresholddB, static_cast<SampleType> (-200.0));
                thresholdInverse = static_cast<SampleType> (1.0) / threshold;
                ratioInverse = static_cast<SampleType> (1.0) / ratio;

                envelopeFilter.setAttackTime(attackTime);
                envelopeFilter.setReleaseTime(releaseTime);

                delayBuffer.resize(numChannels);
                for (auto& channelBuffer : delayBuffer) {
					channelBuffer.clear();
                    channelBuffer.resize(static_cast<int>(sampleRate * lookAheadTime / 1000.0), 0.0); // Look-ahead delay buffer
                }
            }

            // M/S処理
            //static pair<SampleType, SampleType> MSDecode(SampleType M, SampleType S) noexcept
            //{
            //    const auto L = M + S, R = M - S;

            //    return {L, R};
            //}

            //static pair<SampleType, SampleType> MSEncode(SampleType L, SampleType R) noexcept
            //{
            //    const auto [M, S] = MSDecode(L, R);

            //    return {M / 2, S / 2};
            //}

            //==============================================================================
            SampleType threshold, thresholdInverse, ratioInverse;
            juce::dsp::BallisticsFilter<InnerSampleType> envelopeFilter;
            std::vector<std::deque<SampleType>> delayBuffer;

            double sampleRate = 44100.0;
			juce::uint32 numChannels = 0;
            SampleType thresholddB = 0.0, ratio = 1.0, attackTime = 1.0, releaseTime = 100.0, lookAheadTime = 5.0;
            bool useMSProcessing = false;
        };

} // namespace juce

/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "LookForwardingCompressor.h"

//==============================================================================
/**
*/
class HeuristicLimiterAudioProcessor  : public juce::AudioProcessor
{
public:
    //==============================================================================
    HeuristicLimiterAudioProcessor();
    ~HeuristicLimiterAudioProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

   #ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
   #endif

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

private:
    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HeuristicLimiterAudioProcessor)
  
    // parameters
    juce::AudioParameterFloat *const gain,
                              *const threshold,
                              *const ratio;
  
    enum {
      compressorIndex,
      waveShaperIndex
    };

    constexpr static int OVERSAMPLE_FACTOR = 4, OVERSAMPLE_RATIO = 1 << OVERSAMPLE_FACTOR;
    constexpr static double LOOKAHEAD_TIME = 5.0;
  
    // filters
    juce::dsp::ProcessorChain<
        dsp_original::LookAheadCompressor<float>,
        juce::dsp::WaveShaper<float>
    > processorChain;
    juce::dsp::Oversampling<float> oversampling;

    // FFT用
    juce::dsp::FFT fft;
    std::vector<std::vector<float>> fftBuffer;
    
    // 一時コピー用バッファ
    juce::AudioBuffer<float> temporaryResultBuffer, temporaryResultBuffer2;

    // 誤差計測用の関数を返す
    template<bool is_release>
    auto getFuncCalculateDiff(
        const juce::dsp::ProcessContextNonReplacing<float>& simulate,
        int totalNumInputChannels,
        const decltype(fftBuffer)& buffer
    );
};

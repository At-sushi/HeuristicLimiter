/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#include <boost/math/tools/minima.hpp>
#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
HeuristicLimiterAudioProcessor::HeuristicLimiterAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       )
#endif
    : gain(new juce::AudioParameterFloat("GAIN", "Gain", 0.0f, 20.0f, 0.0f))
    , threshold(new juce::AudioParameterFloat("THRESHOLD", "Threshold", -50.0f, 0.0f, -0.3f))
    , ratio(new juce::AudioParameterFloat("RATIO", "Ratio", 1.0f, 20.0f, 4.0f))
    , oversampling(2, OVERSAMPLE_FACTOR, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple)
{
    for (auto i : {gain, threshold, ratio}) {
      addParameter(i);
    }
  
    // prepare DSPs
    processorChain.get<waveShaperIndex>().functionToUse = [](float x) {
        return std::tanh(x);
    };
}

HeuristicLimiterAudioProcessor::~HeuristicLimiterAudioProcessor()
{
}

//==============================================================================
const juce::String HeuristicLimiterAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool HeuristicLimiterAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool HeuristicLimiterAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool HeuristicLimiterAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double HeuristicLimiterAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int HeuristicLimiterAudioProcessor::getNumPrograms()
{
    return 1;   // NB: some hosts don't cope very well if you tell them there are 0 programs,
                // so this should be at least 1, even if you're not really implementing programs.
}

int HeuristicLimiterAudioProcessor::getCurrentProgram()
{
    return 0;
}

void HeuristicLimiterAudioProcessor::setCurrentProgram (int index)
{
}

const juce::String HeuristicLimiterAudioProcessor::getProgramName (int index)
{
    return {};
}

void HeuristicLimiterAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
}

//==============================================================================
void HeuristicLimiterAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // Use this method as the place to do any pre-playback
    // initialisation that you need..
    juce::dsp::ProcessSpec a;
    a.sampleRate = sampleRate;
    a.maximumBlockSize = samplesPerBlock;
    a.numChannels = 2;
  
    processorChain.reset();
    processorChain.prepare(a);

    // reset oversampler
    oversampling.reset();
    oversampling.initProcessing(samplesPerBlock);

    // adjust latency
    setLatencySamples(static_cast<int>(oversampling.getLatencyInSamples()));

    resultBuffer.setSize(2, samplesPerBlock);
}

void HeuristicLimiterAudioProcessor::releaseResources()
{
    // When playback stops, you can use this as an opportunity to free up any
    // spare memory, etc.
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool HeuristicLimiterAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else
    // This is the place where you check if the layout is supported.
    // In this template code we only support mono or stereo.
    // Some plugin hosts, such as certain GarageBand versions, will only
    // load plugins that support stereo bus layouts.
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // This checks if the input layout matches the output layout
   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
   #endif

    return true;
  #endif
}
#endif

// 誤差計測用の関数を返す
auto HeuristicLimiterAudioProcessor::getFuncCalculateDiff(bool is_release, const juce::dsp::ProcessContextNonReplacing<float>& simulate, int totalNumInputChannels, juce::AudioSampleBuffer& buffer)
{
    return [&, is_release](double param) -> double {
        auto temporaryProcessorChain = processorChain;

        // 仮のRelease値を試す
        if (is_release)
            temporaryProcessorChain.get<compressorIndex>().setRelease(static_cast<float>(param));
        else
            temporaryProcessorChain.get<compressorIndex>().setAttack(static_cast<float>(param));
        temporaryProcessorChain.process(simulate);

        double result = 0.0;

        // 誤差を計算
        for (int channel = 0; channel < totalNumInputChannels; ++channel)
        {
            auto* inBufferFrom = buffer.getReadPointer(channel);
            auto* inBufferTo = resultBuffer.getReadPointer(channel);

            for (auto samples = 0; samples < buffer.getNumSamples(); samples++)
                result += std::abs(*inBufferFrom++ - *inBufferTo++);
        }

        return result;
    };
}

void HeuristicLimiterAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    // applying parameters
    processorChain.get<gainIndex>().setGainDecibels(*gain);
    processorChain.get<compressorIndex>().setThreshold(*threshold);
    processorChain.get<compressorIndex>().setRatio(*ratio);

    juce::ScopedNoDenormals noDenormals;
    auto totalNumInputChannels  = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    // In case we have more outputs than inputs, this code clears any output
    // channels that didn't contain input data, (because these aren't
    // guaranteed to be empty - they may contain garbage).
    // This is here to avoid people getting screaming feedback
    // when they first compile a plugin, but obviously you don't need to keep
    // this code if your algorithm always overwrites all the output channels.
    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    // This is the place where you'd normally do the guts of your plugin's
    // audio processing...
    // Make sure to reset the state if your inner loop is processing
    // the samples and the outer loop is handling the channels.
    // Alternatively, you can process the samples with the channels
    // interleaved by keeping the same state.
    juce::dsp::AudioBlock<float> block(buffer);

    // コピー用のバッファを生成
    // TODO: resize on preparetoPlay
    resultBuffer.setSize(buffer.getNumChannels(), buffer.getNumSamples(), false, false, true);  // 暫定措置
    juce::dsp::AudioBlock<float> resultBlock(resultBuffer);
    juce::dsp::ProcessContextNonReplacing<float> simulate(block, resultBlock);


    // minimize differences
    const auto release = boost::math::tools::brent_find_minima(getFuncCalculateDiff(true, simulate, totalNumInputChannels, buffer), 0.0, 300.0, 24).first;
    processorChain.get<compressorIndex>().setRelease(static_cast<float>(release));

    const auto attack = boost::math::tools::brent_find_minima(getFuncCalculateDiff(false, simulate, totalNumInputChannels, buffer), 0.0, 30.0, 24).first;
    processorChain.get<compressorIndex>().setAttack(static_cast<float>(attack * OVERSAMPLE_RATIO));
    processorChain.get<compressorIndex>().setRelease(static_cast<float>(release * OVERSAMPLE_RATIO));

    // get oversampled buffer
    auto blockOver = oversampling.processSamplesUp(block);
    juce::dsp::ProcessContextReplacing<float> context(blockOver);

    // process
    processorChain.process(context);

    // downsample oversampled buffer
    oversampling.processSamplesDown(block);
}

//==============================================================================
bool HeuristicLimiterAudioProcessor::hasEditor() const
{
    return false; // (change this to false if you choose to not supply an editor)
}

juce::AudioProcessorEditor* HeuristicLimiterAudioProcessor::createEditor()
{
    return new HeuristicLimiterAudioProcessorEditor (*this);
}

//==============================================================================
void HeuristicLimiterAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // You should use this method to store your parameters in the memory block.
    // You could do that either as raw data, or use the XML or ValueTree classes
    // as intermediaries to make it easy to save and load complex data.
    juce::MemoryOutputStream stream(destData, true);

    for (auto i : { gain, threshold, ratio })
        stream.writeFloat(*i);
}

void HeuristicLimiterAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // You should use this method to restore your parameters from this memory block,
    // whose contents will have been created by the getStateInformation() call.
    juce::MemoryInputStream stream(data, static_cast<size_t>(sizeInBytes), false);

    for (auto i : { gain, threshold, ratio })
        *i = stream.readFloat();
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new HeuristicLimiterAudioProcessor();
}

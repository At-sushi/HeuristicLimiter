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
    , gain(new juce::AudioParameterFloat("GAIN", "Gain", -80.0f, 20.0f, 0.0f))
    , threshold(new juce::AudioParameterFloat("THRESHOLD", "Threshold", -50.0f, 0.0f, -0.3f))
    , ratio(new juce::AudioParameterFloat("RATIO", "Ratio", 1.0f, 30.0f, 10.0f))
#endif
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
  
    processorChain.prepare(a);
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
    juce::dsp::ProcessContextReplacing<float> context(block);

    // simulate and calculate difference
    static const auto calculateDiff = [&](double param) -> double {
        auto temporaryProcessorChain = processorChain;
        juce::AudioBuffer<float> resultBuffer(buffer.getNumChannels(), buffer.getNumSamples());
        juce::dsp::AudioBlock<float> resultBlock(resultBuffer);
        juce::dsp::ProcessContextNonReplacing<float> process(block, resultBlock);
      
        // 仮のRelease値を試す
        temporaryProcessorChain.get<compressorIndex>().setRelease(static_cast<float>(param));
        temporaryProcessorChain.process(process);
        
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
  
    // minimize differences
    const auto release = boost::math::tools::brent_find_minima(calculateDiff, 1.0, 100.0, 24).first;
    processorChain.get<compressorIndex>().setRelease(static_cast<float>(release));

    processorChain.process(context);
}

//==============================================================================
bool HeuristicLimiterAudioProcessor::hasEditor() const
{
    return true; // (change this to false if you choose to not supply an editor)
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
}

void HeuristicLimiterAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // You should use this method to restore your parameters from this memory block,
    // whose contents will have been created by the getStateInformation() call.
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new HeuristicLimiterAudioProcessor();
}

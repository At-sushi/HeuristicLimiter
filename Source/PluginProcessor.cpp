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
    , oversampling(1, OVERSAMPLE_FACTOR, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple)
    , fft(12)
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
    a.sampleRate = sampleRate * OVERSAMPLE_RATIO;
    a.maximumBlockSize = samplesPerBlock * OVERSAMPLE_RATIO;
    a.numChannels = getTotalNumOutputChannels();
  
    processorChain.reset();
    processorChain.prepare(a);

    // reset oversampler
    oversampling.reset();
    oversampling.numChannels = getTotalNumOutputChannels();
    oversampling.initProcessing(samplesPerBlock);

    // adjust latency
    setLatencySamples(static_cast<int>(oversampling.getLatencyInSamples()));
    
    // FFT・バッファ初期化
    const auto order = static_cast<int>(std::ceil(std::log2(samplesPerBlock)));
    fft = juce::dsp::FFT(order);
    fftBuffer = std::vector(getTotalNumInputChannels(), std::vector(fft.getSize() * 2, 0.0f));
    
    temporaryResultBuffer.setSize(getTotalNumOutputChannels(), fft.getSize() * 2);
	temporaryResultBuffer2.setSize(getTotalNumOutputChannels(), samplesPerBlock);
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
template <bool Is_release>
auto HeuristicLimiterAudioProcessor::getFuncCalculateDiff(
    const juce::dsp::ProcessContextNonReplacing<float>& simulate,
    int totalNumInputChannels,
    const decltype(fftBuffer)& buffer)
{
    return [&, totalNumInputChannels](double param) -> double {
		// FIXME: ここでのparamはRelease/Attack値を表すが、OVERSAMPLE_RATIOを考慮していないため、調整が必要
        auto temporaryProcessorChain = processorChain;

        //juce::dsp::ProcessSpec a;
        //a.sampleRate = this->getSampleRate();
        //a.maximumBlockSize = simulate.getInputBlock().getNumSamples();
        //a.numChannels = totalNumInputChannels;

        //temporaryProcessorChain.prepare(a);

        // 仮のRelease/Attack値を試す
        if constexpr (Is_release)
            temporaryProcessorChain.get<compressorIndex>().setRelease(static_cast<float>(param / OVERSAMPLE_RATIO));
        else
            temporaryProcessorChain.get<compressorIndex>().setAttack(static_cast<float>(param / OVERSAMPLE_RATIO));
        temporaryProcessorChain.process(simulate);

        std::atomic<double> result = 0.0;

        // 誤差を計算
		#pragma omp parallel for
        for (int channel = 0; channel < totalNumInputChannels; ++channel)
        {
            auto inBufferFrom = buffer[channel].begin();
            auto* inBufferTo = temporaryResultBuffer.getWritePointer(channel);

            // FFT（resultBufferを直接指定している点については暫定措置）
            std::fill_n(inBufferTo + simulate.getInputBlock().getNumSamples(),
                        fft.getSize() * 2 - simulate.getInputBlock().getNumSamples(),
                        0.0f);
            fft.performFrequencyOnlyForwardTransform(inBufferTo);
            
            for (auto samples = 0; samples < buffer[channel].size(); samples++) {
                result += std::fabs(std::log((1.0f + *inBufferFrom++) / (1.0f + *inBufferTo++)));
            }
        }

        return result;
    };
}

void HeuristicLimiterAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    // applying parameters
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

    // Gain audio before simulation
    buffer.applyGain(juce::Decibels::decibelsToGain(float{*gain}));    // 暫定
    
    // FFT処理
    for (int channel = 0; channel < totalNumInputChannels; ++channel)
    {
        std::copy_n(buffer.getReadPointer(channel), buffer.getNumSamples(), fftBuffer[channel].begin());
        std::fill(fftBuffer[channel].begin() + buffer.getNumSamples(), fftBuffer[channel].end(), 0.0f);
        
        fft.performFrequencyOnlyForwardTransform(fftBuffer[channel].data());
    }

    // This is the place where you'd normally do the guts of your plugin's
    // audio processing...
    // Make sure to reset the state if your inner loop is processing
    // the samples and the outer loop is handling the channels.
    // Alternatively, you can process the samples with the channels
    // interleaved by keeping the same state.
    juce::dsp::AudioBlock<float> block(buffer);

    // コピー用のバッファを生成
    juce::dsp::AudioBlock<float> resultBlock(temporaryResultBuffer2);
    juce::dsp::ProcessContextNonReplacing<float> simulate(block, resultBlock);

    // minimize differences
    const auto release = boost::math::tools::brent_find_minima(
        getFuncCalculateDiff<true>(simulate, totalNumInputChannels, fftBuffer),
        0.0,
        300.0,
        24
    ).first;
    processorChain.get<compressorIndex>().setRelease(static_cast<float>(release / OVERSAMPLE_RATIO));

    const auto attack = boost::math::tools::brent_find_minima(
        getFuncCalculateDiff<false>(simulate, totalNumInputChannels, fftBuffer),
        0.0,
        30.0,
        24
    ).first;
    processorChain.get<compressorIndex>().setAttack(static_cast<float>(attack));
    processorChain.get<compressorIndex>().setRelease(static_cast<float>(release));

    // get oversampled buffer
    auto blockOver = oversampling.processSamplesUp(block);
    juce::dsp::ProcessContextReplacing<float> context(blockOver);

    // process
    processorChain.process(context);

    // downsample oversampled buffer
    oversampling.processSamplesDown(block);
}

void HeuristicLimiterAudioProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    // 一回経由させる
    // get oversampled buffer
    juce::dsp::AudioBlock<float> block(buffer);
    auto blockOver = oversampling.processSamplesUp(block);
    juce::dsp::ProcessContextReplacing<float> context(blockOver);
    context.isBypassed = true;

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
    std::unique_ptr<juce::XmlElement> xml (new juce::XmlElement ("ParamHeuristicLimiter"));

    xml->setAttribute("gain", *gain);
    xml->setAttribute("threshold", *threshold);
    xml->setAttribute("ratio", *ratio);

    copyXmlToBinary(*xml, destData);
}

void HeuristicLimiterAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // You should use this method to restore your parameters from this memory block,
    // whose contents will have been created by the getStateInformation() call.
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary(data, sizeInBytes));

    if (xmlState.get() != nullptr && xmlState->hasTagName("ParamHeuristicLimiter")) {
        *gain = xmlState->getDoubleAttribute("gain", 0.0);
        *threshold = xmlState->getDoubleAttribute("threshold", -0.3);
        *ratio = xmlState->getDoubleAttribute("ratio", 4.0);
    }

}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new HeuristicLimiterAudioProcessor();
}

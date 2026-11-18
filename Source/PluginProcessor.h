#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

// One playback deck (A or B). Everything the audio thread touches is atomic
// or guarded by a try-lock, so loading a song never blocks audio.
struct Deck
{
    juce::SpinLock lock;
    std::shared_ptr<juce::AudioBuffer<float>> buf;

    std::atomic<double> srcRate { 44100.0 };   // sample rate of the loaded file
    std::atomic<double> pos { 0.0 };           // play position in source samples
    std::atomic<double> rate { 1.0 };          // 1.0 = normal speed (pitch follows tempo)
    std::atomic<float>  gain { 0.85f };
    std::atomic<bool>   playing { false };
    std::atomic<float>  peakL { 0.f }, peakR { 0.f };

    // used only by the interface thread
    std::mutex ioMutex;
    juce::MemoryBlock assembling, fileBytes;
    juce::String ext { "wav" }, name { "track" };
};

class MPKeyProcessor : public juce::AudioProcessor
{
public:
    MPKeyProcessor();
    ~MPKeyProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "MP Key"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

    // called from the interface
    bool finishLoad (int deckIndex);      // decodes Deck::assembling
    void stopAll();
    void clearGraveyard();

    Deck decks[2];

private:
    juce::AudioFormatManager formats;
    std::mutex graveMutex;
    std::vector<std::shared_ptr<juce::AudioBuffer<float>>> graveyard;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MPKeyProcessor)
};

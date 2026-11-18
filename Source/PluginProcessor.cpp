#include "PluginProcessor.h"
#include "PluginEditor.h"

MPKeyProcessor::MPKeyProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    formats.registerBasicFormats();   // WAV, AIFF, FLAC, OGG, MP3 (+ AAC/M4A via Windows Media)
}

void MPKeyProcessor::prepareToPlay (double, int) {}

bool MPKeyProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
        && l.getMainInputChannelSet()  == juce::AudioChannelSet::stereo();
}

static void raisePeak (std::atomic<float>& a, float v)
{
    float c = a.load();
    while (v > c && ! a.compare_exchange_weak (c, v)) {}
}

void MPKeyProcessor::processBlock (juce::AudioBuffer<float>& out, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int n  = out.getNumSamples();
    const int nc = out.getNumChannels();
    const double hostRate = getSampleRate() > 0 ? getSampleRate() : 44100.0;

    // Whatever arrives on the input passes through unchanged; the decks are added on top.
    for (auto& d : decks)
    {
        std::shared_ptr<juce::AudioBuffer<float>> b;
        {
            const juce::SpinLock::ScopedTryLockType tl (d.lock);
            if (! tl.isLocked()) continue;
            b = d.buf;
        }
        if (b == nullptr || ! d.playing.load()) continue;

        const int len = b->getNumSamples();
        const float* l = b->getReadPointer (0);
        const float* r = b->getReadPointer (b->getNumChannels() > 1 ? 1 : 0);
        double pos = d.pos.load();
        const double step = d.rate.load() * d.srcRate.load() / hostRate;
        const float g = d.gain.load();
        float pl = 0.f, pr = 0.f;

        float* o0 = out.getWritePointer (0);
        float* o1 = nc > 1 ? out.getWritePointer (1) : nullptr;

        for (int i = 0; i < n; ++i)
        {
            if (pos < 0.0) pos = 0.0;
            if (pos >= (double) (len - 1)) { d.playing = false; pos = 0.0; break; }

            const int i0 = (int) pos;
            const float f = (float) (pos - (double) i0);
            const float sl = (l[i0] + (l[i0 + 1] - l[i0]) * f) * g;
            const float sr = (r[i0] + (r[i0 + 1] - r[i0]) * f) * g;

            o0[i] += sl;
            if (o1 != nullptr) o1[i] += sr;
            pl = juce::jmax (pl, std::abs (sl));
            pr = juce::jmax (pr, std::abs (sr));
            pos += step;
        }

        d.pos = pos;
        raisePeak (d.peakL, pl);
        raisePeak (d.peakR, pr);
    }
}

bool MPKeyProcessor::finishLoad (int idx)
{
    auto& d = decks[idx];

    auto in = std::make_unique<juce::MemoryInputStream> (d.assembling.getData(), d.assembling.getSize(), true);
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (std::move (in)));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->lengthInSamples > 0x7fffffffLL / 2)
        return false;

    const int len = (int) reader->lengthInSamples;
    auto nb = std::make_shared<juce::AudioBuffer<float>> (2, len);
    nb->clear();
    reader->read (nb.get(), 0, len, 0, true, true);
    if (reader->numChannels < 2)
        nb->copyFrom (1, 0, *nb, 0, 0, len);

    d.playing = false;
    d.pos = 0.0;
    d.srcRate = reader->sampleRate;
    {
        const juce::SpinLock::ScopedLockType sl (d.lock);
        {
            std::lock_guard<std::mutex> g (graveMutex);
            if (d.buf != nullptr) graveyard.push_back (d.buf);   // freed later, off the audio thread
        }
        d.buf = nb;
    }
    d.fileBytes = d.assembling;
    d.assembling.setSize (0);
    return true;
}

void MPKeyProcessor::stopAll()
{
    for (auto& d : decks) { d.playing = false; d.pos = 0.0; }
}

void MPKeyProcessor::clearGraveyard()
{
    std::lock_guard<std::mutex> g (graveMutex);
    graveyard.clear();
}

juce::AudioProcessorEditor* MPKeyProcessor::createEditor() { return new MPKeyEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new MPKeyProcessor(); }

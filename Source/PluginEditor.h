#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginProcessor.h"

// Shows the MP Key web interface and answers its /api/... requests.
class MPKeyEditor : public juce::AudioProcessorEditor,
                    private juce::Timer
{
public:
    explicit MPKeyEditor (MPKeyProcessor&);
    ~MPKeyEditor() override;

    void resized() override;

private:
    using Resource = juce::WebBrowserComponent::Resource;

    std::optional<Resource> onRequest (const juce::String& url);
    std::optional<Resource> handleApi (const juce::String& op, const juce::StringPairArray& q);
    static Resource makeResource (const void* data, size_t size, const juce::String& mime);
    static Resource makeJson (const juce::String& text);

    void timerCallback() override { proc.clearGraveyard(); }

    MPKeyProcessor& proc;
    std::unique_ptr<juce::WebBrowserComponent> web;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MPKeyEditor)
};

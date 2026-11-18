#include "PluginEditor.h"
#include "MPKeyUI.h"

MPKeyEditor::MPKeyEditor (MPKeyProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    using Opt = juce::WebBrowserComponent::Options;

    // The sign-in, the song library and settings live here, so they survive restarts.
    auto dataDir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                       .getChildFile ("MPKey").getChildFile ("WebView");
    dataDir.createDirectory();

    web = std::make_unique<juce::WebBrowserComponent> (
        Opt{}
            .withBackend (Opt::Backend::webview2)
            .withWinWebView2Options (Opt::WinWebView2{}.withUserDataFolder (dataDir))
            .withKeepPageLoadedWhenBrowserIsHidden()
            .withResourceProvider ([this] (const juce::String& url) { return onRequest (url); }));

    addAndMakeVisible (*web);
    web->goToURL (juce::WebBrowserComponent::getResourceProviderRoot());

    setResizable (true, true);
    setResizeLimits (720, 620, 2000, 1500);
    setSize (1120, 860);
    startTimer (1000);
}

MPKeyEditor::~MPKeyEditor()
{
    proc.stopAll();   // closing the window stops playback
}

void MPKeyEditor::resized()
{
    if (web != nullptr) web->setBounds (getLocalBounds());
}

MPKeyEditor::Resource MPKeyEditor::makeResource (const void* data, size_t size, const juce::String& mime)
{
    std::vector<std::byte> v (size);
    if (size > 0) std::memcpy (v.data(), data, size);
    return Resource { std::move (v), mime };
}

MPKeyEditor::Resource MPKeyEditor::makeJson (const juce::String& text)
{
    auto utf8 = text.toStdString();
    return makeResource (utf8.data(), utf8.size(), "application/json");
}

static juce::StringPairArray parseQuery (const juce::String& q)
{
    juce::StringPairArray m;
    for (auto& kv : juce::StringArray::fromTokens (q, "&", ""))
        m.set (juce::URL::removeEscapeChars (kv.upToFirstOccurrenceOf ("=", false, false)),
               juce::URL::removeEscapeChars (kv.fromFirstOccurrenceOf ("=", false, false)));
    return m;
}

std::optional<MPKeyEditor::Resource> MPKeyEditor::onRequest (const juce::String& rawUrl)
{
    // The provider may hand us a full URL or just the path; keep only path and query.
    juce::String url = rawUrl;
    if (url.containsIgnoreCase ("://"))
        url = url.fromFirstOccurrenceOf ("://", false, false).fromFirstOccurrenceOf ("/", true, false);

    const auto path  = url.upToFirstOccurrenceOf ("?", false, false);
    const auto query = url.fromFirstOccurrenceOf ("?", false, false);

    if (path == "/" || path.isEmpty() || path == "/index.html")
        return makeResource (MPKeyUI::index_html, (size_t) MPKeyUI::index_htmlSize, "text/html");

    if (path.startsWith ("/api/"))
        return handleApi (path.substring (5), parseQuery (query));

    return std::nullopt;
}

std::optional<MPKeyEditor::Resource> MPKeyEditor::handleApi (const juce::String& op, const juce::StringPairArray& q)
{
    const int idx = q["deck"] == "B" ? 1 : 0;
    auto& d = proc.decks[idx];

    if (op == "state")
    {
        juce::String s;
        for (int i = 0; i < 2; ++i)
        {
            auto& k = proc.decks[i];
            const double sr  = k.srcRate.load();
            double pos = sr > 0 ? k.pos.load() / sr : 0.0;
            if (! std::isfinite (pos)) pos = 0.0;
            const float l = k.peakL.exchange (0.f), r = k.peakR.exchange (0.f);
            s << (i ? "," : "") << "\"" << (i ? "B" : "A") << "\":{\"pos\":" << juce::String (pos, 4)
              << ",\"pl\":" << (k.playing.load() ? 1 : 0)
              << ",\"lv\":[" << juce::String (l, 5) << "," << juce::String (r, 5) << "],\"t\":1}";
        }
        return makeJson ("{" + s + "}");
    }

    if (op == "play")
    {
        const double sr = d.srcRate.load();
        d.pos = juce::jmax (0.0, q["pos"].getDoubleValue()) * sr;
        d.rate = juce::jlimit (0.25, 4.0, q["rate"].getDoubleValue());
        d.playing = true;
        return makeJson ("{\"ok\":true}");
    }
    if (op == "pause") { d.playing = false; return makeJson ("{\"ok\":true}"); }
    if (op == "rate")  { d.rate = juce::jlimit (0.25, 4.0, q["v"].getDoubleValue()); return makeJson ("{\"ok\":true}"); }
    if (op == "gain")  { d.gain = (float) juce::jlimit (0.0, 1.5, q["v"].getDoubleValue()); return makeJson ("{\"ok\":true}"); }

    if (op == "load")
    {
        const int i = q["i"].getIntValue(), n = q["n"].getIntValue();
        std::lock_guard<std::mutex> g (d.ioMutex);

        if (i == 0) { d.playing = false; d.assembling.setSize (0); }

        auto b64 = q["d"].replaceCharacter ('-', '+').replaceCharacter ('_', '/');
        while (b64.length() % 4 != 0) b64 << "=";
        juce::MemoryOutputStream mo;
        if (! juce::Base64::convertFromBase64 (mo, b64))
            return makeJson ("{\"ok\":false}");
        d.assembling.append (mo.getData(), mo.getDataSize());

        if (i >= n - 1)
        {
            d.ext  = q["ext"].retainCharacters ("abcdefghijklmnopqrstuvwxyz0123456789");
            if (d.ext.isEmpty()) d.ext = "wav";
            d.name = q["name"].isNotEmpty() ? q["name"] : "track";
            const bool ok = proc.finishLoad (idx);
            return makeJson (ok ? "{\"ok\":true}" : "{\"ok\":false}");
        }
        return makeJson ("{\"ok\":true}");
    }

    if (op == "dragout")
    {
        juce::String path;
        {
            std::lock_guard<std::mutex> g (d.ioMutex);
            if (d.fileBytes.getSize() == 0) return makeJson ("{\"ok\":false}");

            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("MPKey");
            dir.createDirectory();
            auto f = dir.getChildFile (juce::File::createLegalFileName (d.name) + "." + d.ext);
            if (! f.replaceWithData (d.fileBytes.getData(), d.fileBytes.getSize()))
                return makeJson ("{\"ok\":false}");
            path = f.getFullPathName();
        }

        // Must run on the message thread while the mouse button is still held down.
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<juce::Component> (this), path]
        {
            if (safe != nullptr)
                juce::DragAndDropContainer::performExternalDragDropOfFiles ({ path }, false, safe.getComponent());
        });
        return makeJson ("{\"ok\":true}");
    }

    return makeJson ("{\"ok\":false}");
}

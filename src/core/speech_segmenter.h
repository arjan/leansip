#pragma once

#include <QtGlobal>

namespace leansip {

// Finds one utterance in a stream of per-frame levels (dBFS): it starts after `startMs` of
// frames above the threshold and ends after `silenceMs` below it, or at `maxMs`.
class SpeechSegmenter {
public:
    struct Params {
        double thresholdDbfs = -45;
        int frameMs = 20;
        int startMs = 100;
        int silenceMs = 1200;
        int maxMs = 30000;
        int prerollMs = 300; // kept before the detected start, so the first syllable survives
    };
    enum class State { Waiting, Speaking, Done };

    explicit SpeechSegmenter(Params params);

    State feed(double dbfs);
    State state() const { return m_state; }
    qint64 framesSeen() const { return m_frame; }
    // Frame indices [start, end) of the utterance; valid once speaking / done.
    qint64 startFrame() const { return m_start; }
    qint64 endFrame() const { return m_end; }
    // True when the utterance ended because it hit maxMs rather than a pause.
    bool truncated() const { return m_truncated; }

private:
    Params m_p;
    State m_state = State::Waiting;
    qint64 m_frame = 0;
    qint64 m_run = 0;      // consecutive loud frames while waiting, quiet frames while speaking
    qint64 m_start = 0;
    qint64 m_end = 0;
    bool m_truncated = false;
};

} // namespace leansip

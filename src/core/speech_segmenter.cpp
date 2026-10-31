#include "speech_segmenter.h"

#include <algorithm>

namespace leansip {

SpeechSegmenter::SpeechSegmenter(Params params)
    : m_p(params)
{
}

SpeechSegmenter::State SpeechSegmenter::feed(double dbfs)
{
    if (m_state == State::Done)
        return m_state;
    const bool loud = dbfs >= m_p.thresholdDbfs;
    const qint64 frame = m_frame++;
    const qint64 startFrames = std::max(1, m_p.startMs / m_p.frameMs);
    const qint64 silenceFrames = std::max(1, m_p.silenceMs / m_p.frameMs);
    const qint64 maxFrames = std::max(1, m_p.maxMs / m_p.frameMs);
    const qint64 prerollFrames = m_p.prerollMs / m_p.frameMs;

    if (m_state == State::Waiting) {
        m_run = loud ? m_run + 1 : 0;
        if (m_run >= startFrames) {
            m_state = State::Speaking;
            m_start = std::max<qint64>(0, frame + 1 - m_run - prerollFrames);
            m_run = 0;
        }
        return m_state;
    }
    m_run = loud ? 0 : m_run + 1;
    if (m_run >= silenceFrames) {
        m_state = State::Done;
        m_end = frame + 1 - m_run + std::min<qint64>(m_run, prerollFrames); // keep a short tail
    } else if (frame + 1 - m_start >= maxFrames) {
        m_state = State::Done;
        m_end = frame + 1;
        m_truncated = true;
    }
    return m_state;
}

} // namespace leansip

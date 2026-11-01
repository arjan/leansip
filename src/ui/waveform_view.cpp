#include "waveform_view.h"

#include "core/call_audio.h"

#include <QPainter>

#include <algorithm>
#include <cmath>

namespace leansip {

WaveformView::WaveformView(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents); // drags move the window as everywhere else
}

void WaveformView::setPoints(const QVector<float> &points)
{
    m_points = points;
    update();
}

void WaveformView::setColor(const QColor &color)
{
    m_color = color;
    update();
}

void WaveformView::setBars(int count, double gapRatio)
{
    m_bars = std::max(2, count);
    m_gapRatio = gapRatio;
    update();
}

void WaveformView::setWindowMs(int ms)
{
    m_windowMs = std::max(Waveform::kBucketMs * 2, ms);
    update();
}

double WaveformView::offsetFor(float level) const
{
    const double half = height() / 2.0;
    return std::clamp(static_cast<double>(level), 0.0, 1.0) * half;
}

void WaveformView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(m_color);

    // m_bars bars across the full width, mirrored about the centre: half of them on each side.
    // A bar's distance from the centre is the age of the audio it shows, the newest next to
    // the middle, windowMs() old at the edges; its height is the loudest point in its slice of
    // that time, centred on the middle line and scaled down linearly with the distance from
    // the centre, so the outermost bars are always 1 px. Silent bars make a flat 1 px line.
    const int perSide = std::max(1, m_bars / 2);
    const int bars = 2 * perSide;
    const double barWidth = width() / (bars + (bars - 1) * m_gapRatio);
    const double pitch = barWidth * (1.0 + m_gapRatio);
    const double middle = width() / 2.0;
    const double centre = height() / 2.0;
    const qsizetype shown = std::min<qsizetype>(m_points.size(), m_windowMs / Waveform::kBucketMs);
    const qsizetype newest = m_points.size() - 1;
    const double agesPerBar = static_cast<double>(shown) / perSide;
    for (int k = 0; k < perSide; ++k) {
        float level = 0;
        const auto from = static_cast<qsizetype>(k * agesPerBar);
        const auto to = std::max(from + 1, static_cast<qsizetype>((k + 1) * agesPerBar));
        for (qsizetype age = from; age < to && age <= newest; ++age)
            level = std::max(level, m_points[newest - age]);
        const double fade = perSide > 1 ? 1.0 - static_cast<double>(k) / (perSide - 1) : 1.0;
        const double h = std::max(1.0, 2 * offsetFor(level) * fade);
        const double radius = h > 1.0 ? std::min({kCornerRadius, barWidth / 2, h / 2}) : 0.0;
        const double offset = m_gapRatio * barWidth / 2 + k * pitch; // centre gap, then bars
        for (const double x : {middle + offset, middle - offset - barWidth}) {
            const QRectF bar(x, centre - h / 2, barWidth, h);
            if (radius > 0)
                p.drawRoundedRect(bar, radius, radius);
            else
                p.fillRect(bar, m_color);
        }
    }
}

} // namespace leansip

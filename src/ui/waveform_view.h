#pragma once

#include <QColor>
#include <QVector>
#include <QWidget>

namespace leansip {

// Scrolling audio activity: the newest points of a Waveform (levels 0..1, oldest first) as
// bars on fixed positions, mirrored about both centre lines and fading out towards the edges.
// The bars next to the centre show the newest audio, those further out ever older audio, up to
// windowMs(). Bars with height have slightly rounded corners; silence is a flat 1 px line.
class WaveformView : public QWidget {
    Q_OBJECT
public:
    explicit WaveformView(QWidget *parent = nullptr);

    void setPoints(const QVector<float> &points);
    QVector<float> points() const { return m_points; }

    void setColor(const QColor &color);
    // How many bars (even: half on each side) and the gap between them as a fraction of a
    // bar's width; the bars share the full width.
    void setBars(int count, double gapRatio);
    static constexpr double kCornerRadius = 2.0;
    // How much of the recent audio fills the width; points are kBucketMs apart.
    void setWindowMs(int ms);
    int windowMs() const { return m_windowMs; }

    // How far above (and below) the centre line a level is drawn (tests).
    double offsetFor(float level) const;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QVector<float> m_points;
    QColor m_color = Qt::black;
    int m_bars = 16;
    double m_gapRatio = 0.6;
    int m_windowMs = 1000;
};

} // namespace leansip

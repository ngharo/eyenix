#include "gazefilter.h"

#include <cmath>

namespace {

double alpha(double cutoff, double dt) {
    const double tau = 1.0 / (2.0 * M_PI * cutoff);
    return 1.0 / (1.0 + tau / dt);
}

double step(double raw, double dt, double &value, double &speed, double minCutoff, double beta) {
    const double speedAlpha = alpha(1.0, dt);
    speed += speedAlpha * ((raw - value) / dt - speed);
    value += alpha(minCutoff + beta * std::fabs(speed), dt) * (raw - value);
    return value;
}

}

GazeFilter::Point GazeFilter::filter(Point raw, double time) {
    const double dt = time - m_time;
    m_time = time;
    // After a blink or a look away the old position says nothing about the new one.
    if (!m_started || dt <= 0 || dt > maxGap) {
        m_started = true;
        m_value = raw;
        m_speed = {};
        return raw;
    }
    step(raw.x, dt, m_value.x, m_speed.x, minCutoff, beta);
    step(raw.y, dt, m_value.y, m_speed.y, minCutoff, beta);
    return m_value;
}

#pragma once

// Smooths the gaze point with a One Euro filter (Casiez, Roussel and Vogel,
// 2012): heavy smoothing while the eye rests, so the point doesn't jitter,
// and almost none during a fast move, so it doesn't lag behind.
class GazeFilter {
public:
    struct Point {
        double x = 0, y = 0;
    };

    // Coordinates are fractions of the screen; time is in seconds.
    Point filter(Point raw, double time);
    void reset() { m_started = false; }

    double minCutoff = 0.6; // Hz at rest: lower is steadier
    double beta = 4.0;      // how quickly speed opens the filter up
    double maxGap = 0.3;    // seconds without samples before starting over

private:
    bool m_started = false;
    double m_time = 0;
    Point m_value, m_speed;
};

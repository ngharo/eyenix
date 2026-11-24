#pragma once

#include <QByteArray>
#include <QMutex>
#include <QThread>
#include <atomic>
#include <deque>
#include <functional>

#include "device.h"
#include "eyenix/headpose.hpp"

Q_DECLARE_METATYPE(eyenix::GazeSample)
Q_DECLARE_METATYPE(eyenix::HeadPose)

// Owns the tracker on its own thread, because every call to it blocks. While
// the tracker is wanted it keeps one connection open, reconnects when the
// tracker goes away, streams gaze, and runs the commands posted to it between
// samples. While it is not wanted the tracker is released, so it can go idle
// and other programs can use it.
//
// Head tracking, when on, also runs here: camera frames never leave this thread.
class TrackerWorker : public QThread {
    Q_OBJECT

public:
    using Command = std::function<QByteArray(Device &)>;

    explicit TrackerWorker(DeviceFactory factory, int retryMs = 1000, QObject *parent = nullptr);
    ~TrackerWorker() override;

    // Queues a command and returns its id. Exactly one finished() follows for
    // each id, with an error if the command threw or the tracker was not there.
    int post(Command command);

    // Wanted from the start. Unwanted, the tracker is closed and idle() follows.
    void setWanted(bool wanted) { m_wanted = wanted; }
    // modelDirectory holds the face models; see eyenix::HeadTracker.
    void setHeadTracking(bool on, const QString &modelDirectory = {});
    // The screen head pose is measured against.
    void setHeadDisplayArea(const eyenix::DisplayArea &area);

signals:
    void connected(const QString &name);
    // Follows connected(): what the tracker says about itself.
    void described(const QString &serial, const DeviceDescriptions &descriptions);
    // The tracker was released because it is not wanted.
    void idle();
    // One per camera frame while head tracking is on. Not found: no face in view.
    void head(const eyenix::HeadPose &pose, bool found);
    void headTrackingFailed(const QString &reason);
    void disconnected(const QString &reason);
    void gaze(const eyenix::GazeSample &sample);
    void finished(int id, const QString &error, const QByteArray &result);

protected:
    void run() override;

private:
    struct Pending {
        int id;
        Command command;
    };

    bool takeNext(Pending &next);
    void failPending(const QString &error);

    DeviceFactory m_factory;
    int m_retryMs;
    std::atomic<bool> m_stop{false}, m_wanted{true}, m_head{false}, m_areaChanged{false};
    QString m_modelDirectory;       // guarded by m_mutex
    eyenix::DisplayArea m_headArea; // guarded by m_mutex
    QMutex m_mutex;
    std::deque<Pending> m_queue;
    int m_nextId = 0;
};

#include "trackerworker.h"

#include <QMutexLocker>

TrackerWorker::TrackerWorker(DeviceFactory factory, int retryMs, QObject *parent)
    : QThread(parent), m_factory(std::move(factory)), m_retryMs(retryMs) {
    qRegisterMetaType<eyenix::GazeSample>();
    qRegisterMetaType<eyenix::HeadPose>();
    qRegisterMetaType<DeviceDescriptions>();
}

void TrackerWorker::setHeadTracking(bool on, const QString &modelDirectory) {
    QMutexLocker lock(&m_mutex);
    m_modelDirectory = modelDirectory;
    m_head = on && !modelDirectory.isEmpty();
}

void TrackerWorker::setHeadDisplayArea(const eyenix::DisplayArea &area) {
    QMutexLocker lock(&m_mutex);
    m_headArea = area;
    m_areaChanged = true;
}

TrackerWorker::~TrackerWorker() {
    m_stop = true;
    wait();
}

int TrackerWorker::post(Command command) {
    QMutexLocker lock(&m_mutex);
    m_queue.push_back({++m_nextId, std::move(command)});
    return m_nextId;
}

bool TrackerWorker::takeNext(Pending &next) {
    QMutexLocker lock(&m_mutex);
    if (m_queue.empty())
        return false;
    next = std::move(m_queue.front());
    m_queue.pop_front();
    return true;
}

void TrackerWorker::failPending(const QString &error) {
    Pending pending;
    while (takeNext(pending))
        emit finished(pending.id, error, {});
}

void TrackerWorker::run() {
    while (!m_stop) {
        if (!m_wanted) {
            failPending(QStringLiteral("the tracker is not in use"));
            msleep(20);
            continue;
        }
        QString reason;
        try {
            auto device = m_factory();
            device->startGaze();
            emit connected(device->name());
            emit described(device->serial(), device->descriptions());

            std::unique_ptr<eyenix::HeadTracker> head;
            bool images = false;

            while (!m_stop && m_wanted) {
                Pending pending;
                while (!m_stop && takeNext(pending)) {
                    try {
                        emit finished(pending.id, {}, pending.command(*device));
                    } catch (const eyenix::UsbError &error) {
                        // The connection itself failed: report the command, then reconnect.
                        emit finished(pending.id, QString::fromUtf8(error.what()), {});
                        throw;
                    } catch (const std::exception &error) {
                        emit finished(pending.id, QString::fromUtf8(error.what()), {});
                    }
                }

                if (m_head != images) {
                    images = m_head;
                    if (images) {
                        QString models;
                        {
                            QMutexLocker lock(&m_mutex);
                            models = m_modelDirectory;
                        }
                        try {
                            head = std::make_unique<eyenix::HeadTracker>(models.toStdString());
                            m_areaChanged = true;
                            device->startImages();
                        } catch (const eyenix::UsbError &) {
                            throw;
                        } catch (const std::exception &error) {
                            // Bad models or a tracker without a camera stream: carry on with gaze alone.
                            head.reset();
                            m_head = images = false;
                            emit headTrackingFailed(QString::fromUtf8(error.what()));
                        }
                    } else {
                        device->stopImages();
                        head.reset();
                    }
                }

                const auto sample = device->readGaze(50);
                if (!sample)
                    continue;
                emit gaze(*sample);
                if (!head)
                    continue;
                if (m_areaChanged.exchange(false)) {
                    QMutexLocker lock(&m_mutex);
                    head->setDisplayArea(m_headArea);
                }
                head->onGaze(*sample);
                if (const auto frame = device->takeImage()) {
                    const auto pose = head->onImage(*frame);
                    emit this->head(pose.value_or(eyenix::HeadPose{}), pose.has_value());
                }
            }
            if (!m_stop && !m_wanted) {
                device.reset(); // closes the connection; the tracker goes idle
                emit idle();
                continue;
            }
        } catch (const std::exception &error) {
            reason = QString::fromUtf8(error.what());
        }
        if (m_stop)
            break;
        emit disconnected(reason);

        // Nothing can run without a tracker, so answer whatever is waiting
        // rather than letting it sit until one appears.
        for (int waited = 0; waited < m_retryMs && !m_stop; waited += 20) {
            failPending(reason);
            msleep(20);
        }
    }
    failPending(QStringLiteral("stopped"));
}

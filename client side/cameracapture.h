#ifndef CAMERACAPTURE_H
#define CAMERACAPTURE_H

#include <QObject>
#include <QCamera>
#include <QMediaCaptureSession>
#include <QVideoSink>
#include <QVideoFrame>
#include <QMediaDevices>

extern int CAMERA_WIDTH;
extern int CAMERA_HEIGHT;

class cameracapture : public QObject
{
    Q_OBJECT

public:
    explicit cameracapture(QObject * parent = nullptr);
    void start();
    void stop();
    void cameraSettings();

signals:
    void frameReady(const QVideoFrame &frame);

private:
    QCamera * camera;
    QMediaCaptureSession *capturesession;
    QVideoSink *videosink;
};

#endif // CAMERACAPTURE_H

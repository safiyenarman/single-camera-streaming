#include "cameracapture.h"
#include <QDebug>


cameracapture::cameracapture(QObject *parent) : QObject(parent)
{
    const QList<QCameraDevice> cameras = QMediaDevices::videoInputs();

    if (cameras.isEmpty()) {
        qDebug() << "No camera device is found!";
        return;
    }

    camera = new QCamera(this);
    capturesession = new QMediaCaptureSession(this);
    videosink = new QVideoSink(this);

    capturesession->setCamera(camera);
    capturesession->setVideoSink(videosink);

    // whenever videosink gets a new frame we will automatically fire our frameReady
    // member function and pass the frame.
    connect(videosink, &QVideoSink::videoFrameChanged, this, &cameracapture::frameReady);

    cameraSettings();
}

void cameracapture::start()
{
    camera->start();
    if (camera->focusMode() != QCamera::FocusModeAuto) {
        camera->setFocusMode(QCamera::FocusModeAuto);
    }

    if (camera->exposureMode() != QCamera::ExposureAuto) {
        camera->setExposureMode(QCamera::ExposureAuto);
    }
    qDebug() << "Camera is started!";
}

void cameracapture::stop()
{
    camera->stop();
    qDebug() << "Camera is stopped!";
}

void cameracapture::cameraSettings()
{
    // we are getting the camera device's full list of supported formats, where each
    // format will be combination of resolution and pixel format (whether it is JPEG or YUYV)
    // and frame rate. this list will come from hardware driver.
    const QList<QCameraFormat> formats = camera->cameraDevice().videoFormats();

    // DEBUG: Print EVERY format the camera supports
    // for (int i = 0; i < formats.size(); i++) {
    //     qDebug() << "Format" << i
    //              << "| Resolution:" << formats[i].resolution()
    //              << "| PixelFormat:" << formats[i].pixelFormat()
    //              << "| FPS:" << formats[i].minFrameRate() << "-" << formats[i].maxFrameRate();
    // }

    QCameraFormat targetformat;

    for (int i = 0; i < formats.size(); i++) {
        const QCameraFormat &format = formats[i];
        if (format.resolution() == QSize(CAMERA_WIDTH, CAMERA_HEIGHT) && format.pixelFormat() == QVideoFrameFormat::Format_Jpeg) {
            targetformat = format;
            break;
        }
    }
    // after we have all of the supported formats we are looping through every format looking
    // for match for both conditions which are our wanted CAMERA_WIDTH and CAMERA_HEIGHT and
    // JPEG pixel format which Qt can decode. if we found the matched format, we are applying it to camera.
    if (!targetformat.isNull()) {
        camera->setCameraFormat(targetformat);
    }

    if (targetformat.isNull()) {
        qDebug() << "Unsupported resolution, camera will not start.";

    }
}

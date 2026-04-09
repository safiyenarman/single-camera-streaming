#ifndef FRAMECONVERTER_H
#define FRAMECONVERTER_H

#include <QObject>
#include <QVideoFrame>
#include <QByteArray>

extern int BYTES_PER_UDP_PACKET;
extern int JPEG_QUALITY;

class frameconverter : public QObject
{
    Q_OBJECT

public:
    explicit frameconverter(QObject *parent = nullptr);

signals:
    // fires every frame, carrying JPEG-compressed data
    void frameConverted(const QByteArray &jpegData);

public slots:
    void processFrame(const QVideoFrame &frame);

private:
    QByteArray convertToJpeg(const QVideoFrame &frame);
};

#endif // FRAMECONVERTER_H

#include "frameconverter.h"
#include "cameracapture.h"
#include <QImage>
#include <QBuffer>
#include <QDebug>

int CAMERA_WIDTH = 1280;
int CAMERA_HEIGHT = 720;

// the problem: image freezing when I switch to color mode from greyscale mode - ip fragmentation
// solution: using JPEG compresser and setting the bytes per udp packet lower

// since the network has a maximum transmission unit we have a boundary for how many bytes we are sending in a single packet.
// when we set 1400 byte to be sent per packet, in rgb mode assuming our resolution is 360x240 we will have360 × 240 × 3 = 259,200 bytes
// when we are in greyscale mode it will be 360 × 240 × 1 = 86,400 bytes
// in color mode, one RGB frame needed 259,200 / 1400 = 186 packets. Every single one of those 186 packets had to arrive for the frame to be complete.
// with using JPEG compresser a 259,200-byte raw RGB frame down to roughly 8,000–15,000 bytes
// At BYTES_PER_UDP_PACKET = 1400, that is about 6–11 packets per frame instead of 186.
// The probability of all 8 packets arriving on WiFi is higher than all 186 arriving.

int BYTES_PER_UDP_PACKET = 1400;

// JPEG quality: 0–100 range
//Lower jpeg quality means smaller file so fewer packets over UDP which is more reliable.

int JPEG_QUALITY = 70;

frameconverter::frameconverter(QObject *parent) : QObject(parent) { }

void frameconverter::processFrame(const QVideoFrame &frame)
{
    if (!frame.isValid()) return;

    QByteArray jpegData = convertToJpeg(frame);

    if (jpegData.isEmpty()) return;

    //we are firing the frameConverted signal and passing the greyscale data as bytearray
    //cameraclient is listening to our frameConverted signal so it will take this data and start to package the image and send them over UDP.
    emit frameConverted(jpegData);
}

QByteArray frameconverter::convertToJpeg(const QVideoFrame &frame)
{
    // QVideoFrame is raw hardware buffer we are converting it to QImage to work with a proper pixel grid
    // Format_RGB888 means each pixel is stored as exactly 3 bytes - red, green, blue we need to guarantee
    // to make our format RGB888 because of our pixel math for converting to greyscale in server.
    QImage image = frame.toImage();

    if (image.isNull()) {
        return {};
    }

    // Scaling to the target resolution
    image = image.scaled(CAMERA_WIDTH, CAMERA_HEIGHT, Qt::IgnoreAspectRatio, Qt::FastTransformation);

    // JPEG-compress into a byte array in memory
    // normally image.save() writes files in the disk but we want to save it in memory because of these 2 reasons:
    // speed:each frame should be compressed and sent over UDP in small time writing it to disk means that operating system needs to find space in hard drive
    // write the file then we can reach the memory and then delete it so it will drop our frame rate whereas writing it to RAM is much more efficient
    // our aim: we never read the file so placing it to disk is again inefficient, we only sent it through UDP then they become useless.
    // with using QBuffer we wrap the QByteArray and make it behave like a file
    // so Qt writes the JPEG data into jpegData and in memory not in disk.

    QByteArray jpegData;
    QBuffer buffer(&jpegData);
    buffer.open(QIODevice::WriteOnly);

    // by using save() we need 3 arguments where to write in what format and in what quality.
    // Qt has a built-in JPEG encoder library (libjpeg) by using JPEG format we can zip the bytes into our buffer
    // thorugh JPEG encoder and compress bytes into our buffer
    // we are taking the pixel table and zip the jpeg and write the result to buffer (in jpegData)
    //after we do jpeg compression we will save about 250.000 bytes which means we shrinked the byte array about 20x times.
    if (!image.save(&buffer, "JPEG", JPEG_QUALITY)) {
        qDebug() << "JPEG compression failed!";
        return {};
    }

    return jpegData;
}

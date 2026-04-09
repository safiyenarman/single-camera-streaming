#include "serverwindow.h"
#include "ui_serverwindow.h"
#include <QNetworkDatagram>
#include <QDataStream>
#include <QPixmap>
#include <QVBoxLayout>
#include <QDebug>
#include <QTimer>
#include <cmath>

// default camera resolution client starts capturing at this size
// these get updated when user picks a different resolution from the combo box
// or when the client sends its saved settings on connect
int CAM_WIDTH  = 1280;
int CAM_HEIGHT = 720;

// TCP is the control channel(port) resolution changes, mode changes, settings sync
// UDP is the video channel JPEG frame packets arrive here
int TCP_PORT = 3456;
int UDP_PORT = 4567;

// when user picks index 0 from combo box it maps to resolutions[0] which is "1280x720"
QStringList resolutions = {"1280x720", "360x240", "640x480", "800x600", "960x720", "1024x576","1552x1552", "1920x1080"};

serverwindow::serverwindow(QWidget *parent)
    : QMainWindow(parent), ui(new Ui::serverwindow)
{
    ui->setupUi(this);
    setWindowTitle("IP CAMERA (Server Side)");

    // resolution overlay label is on  top of the video feed showing current resolution
    // we create it in code instead of Qt Designer because it floats over another widget
    // which layouts cant do. ui->videoLabel is its parent so it moves with the video area
    resolutionOverlayLabel = new QLabel("1280 x 720", ui->videoLabel);
    resolutionOverlayLabel->setStyleSheet(
        "color: #00ffff; font-size: 14px;"
        "font-family: 'Courier New'; letter-spacing: 1px;"
        "background-color: rgba(0, 255, 255, 0.03);"
        "border: 1px solid rgba(0, 255, 255, 0.2);"
        "padding-left: 5px; padding-right: 5px;");
    resolutionOverlayLabel->setFixedSize(150, 30);
    resolutionOverlayLabel->setAlignment(Qt::AlignCenter);
    resolutionOverlayLabel->move(8, 6);

    // when user changes the resolution
    connect(ui->resolutionComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),this, &serverwindow::onResolutionChanged);

    // when user clicks the grayscale/color button
    connect(ui->colorToggleButton, &QPushButton::clicked,this, &serverwindow::onColorToggleClicked);

    // creating the frame collector and connect to its -frame is ready- signal
    // framecollector assembles UDP packets into complete JPEG frames
    // when a frame is fully assembled it fires frameComplete and we decode and display it
    collector = new framecollector(this);
    connect(collector, &framecollector::frameComplete, this, &serverwindow::onFrameComplete);

    // TCP server listens for incoming client connections on TCP_PORT
    // when a client connects onNewTcpConnection fires and we save the client socket
    tcpServer = new QTcpServer(this);
    connect(tcpServer, &QTcpServer::newConnection, this, &serverwindow::onNewTcpConnection);
    if (!tcpServer->listen(QHostAddress::Any, TCP_PORT))
        qDebug() << "TCP failed to start!";

    // UDP socket receives video frame packets from the client
    // each packet has an 8-byte header + JPEG data packet where we formed it in framecollector.cpp
    udpSocket = new QUdpSocket(this);
    connect(udpSocket, &QUdpSocket::readyRead, this, &serverwindow::onUdpDataReceived);
    if (!udpSocket->bind(QHostAddress::Any, UDP_PORT))
        qDebug() << "UDP failed to bind!";

    // initializing motion detection buffer with zeros at the starting resolution
    // without this the first frame comparison would fail because previousFrame would be empty
    // we use MOTION_WIDTH x MOTION_HEIGHT (160x120) not CAM_WIDTH x CAM_HEIGHT
    // because motion detection always works on the tiny downscaled image regardless of camera resolution
    // we used this method to be able to not encounter freezing problem.
    //previousGrayFrame.fill(0, CAM_WIDTH * CAM_HEIGHT);
    previousGrayFrame.fill(0, MOTION_WIDTH * MOTION_HEIGHT);

    // we use statistics timer tbat updates the 4 statistics boxes every second.
    statsTimer = new QTimer(this);
    statsTimer->setInterval(1000);
    connect(statsTimer, &QTimer::timeout, this, [this]() {
        int fps = frameCount;
        int packetperframe = (frameCount > 0) ? (packetCount / frameCount) : 0;

        // Format bytes showing like this: XX.Xk for example 11.2K
        // we are using static_cast for safe conversion between variable types like converting from int to double.
        QString bytesText;
        if (frameCount > 0) {
            double avgBytes = static_cast<double>(totalBytes) / frameCount;
            if (avgBytes >= 1024.0)
                bytesText = QString::number(avgBytes / 1024.0, 'f', 1) + "K";
            else
                bytesText = QString::number(static_cast<int>(avgBytes));
        } else {
            bytesText = "0";
        }

        // update the stat labels on the UI
        ui->fpsLabel->setText(QString::number(fps));
        ui->packetsLabel->setText(QString::number(packetperframe));
        ui->bytesLabel->setText(bytesText);

        // save current values then reset for the next second
        lastFrames = frameCount;
        lastPackets = packetCount;
        frameCount = 0;
        packetCount = 0;
        totalBytes = 0;
    });

    // we use frame timeout timer here because if no complete frames arrive for 3 seconds we assume the client
    // lost connection so we need to reset the ui back to its waiting stage.
    uiTimer = new QTimer(this);
    uiTimer->setInterval(3000);
    uiTimer->setSingleShot(true);
    connect(uiTimer, &QTimer::timeout, this, [this]() {
        qDebug() << "No frames for 3 seconds client disconnected.";
        if (clientSocket && clientSocket->state() == QAbstractSocket::ConnectedState)
            clientSocket->disconnectFromHost();
    });
}

serverwindow::~serverwindow()
{
    delete ui;
}

// updates the connection status label on the ui
void serverwindow::updateConnectionStatus(bool connected)
{
    if (connected) {
        ui->connectionLabel->setText("● ONLINE");
        ui->connectionLabel->setStyleSheet(
            "background-color: rgba(0, 255, 100, 0.06);"
            "color: #00ff66;"
            "font-size: 14px; font-weight: bold;"
            "font-family: 'Courier New';"
            "letter-spacing: 1px;"
            "border: 1px solid rgba(0, 255, 100, 0.25);");
    } else {
        ui->connectionLabel->setText("● OFFLINE");
        ui->connectionLabel->setStyleSheet(
            "background-color: rgba(255, 50, 50, 0.06);"
            "color: #e24b4a;"
            "font-size: 14px; font-weight: bold;"
            "font-family: 'Courier New';"
            "letter-spacing: 1px;"
            "border: 1px solid rgba(255, 50, 50, 0.25);");
    }
}

// called when a client connects via TCP
// we save the client socket so we can send commands to it later (RES:, MODE:)
// start the stats timer and frame timeout timer
// and show the clients IP on the UI
void serverwindow::onNewTcpConnection()
{
    // accept the incoming connection and get the clients socket
    clientSocket = tcpServer->nextPendingConnection();

    // wire up signals readyRead fires when client sends TCP data, disconnected when it drops
    connect(clientSocket, &QTcpSocket::readyRead, this, &serverwindow::onTcpDataReceived);
    connect(clientSocket, &QTcpSocket::disconnected, this, &serverwindow::onClientDisconnected);

    updateConnectionStatus(true);
    ui->videoLabel->setText("");

    QString clientIp = clientSocket->peerAddress().toString().remove("::ffff:");
    ui->clientIpLabel->setText("CLIENT: " + clientIp);

    // reset all stats counters and start timers
    frameCount = packetCount = lastFrames = lastPackets = totalBytes = 0;
    statsTimer->start();
    uiTimer->start();

    qDebug() << "Client connected:" << clientIp;
}

// called when the client disconnects from TCP
void serverwindow::onClientDisconnected()
{
    updateConnectionStatus(false);
    ui->videoLabel->setText("WAITING FOR CAMERA FEED..");
    ui->clientIpLabel->setText("CLIENT → ---");

    statsTimer->stop();
    uiTimer->stop();
    frameCount = packetCount = lastFrames = lastPackets = totalBytes = 0;

    ui->fpsLabel->setText("0");
    ui->packetsLabel->setText("0");
    ui->bytesLabel->setText("0");
    ui->motionStatLabel->setText("—");
    ui->motionStatLabel->setStyleSheet(
        "color: #334455;"
        "font-size: 20px; font-weight: bold;"
        "font-family: 'Courier New';"
        "background-color: transparent;");

    qDebug() << "Client disconnected.";
}


// onTcpDataReceived() is called when TCP data arrives from the client.
// handles SETTINGS: messages that the client sends on connect
// containing its saved resolution and color mode so our UI can match
void serverwindow::onTcpDataReceived()
{
    QByteArray data = clientSocket->readAll();
    QString message = QString::fromUtf8(data);
    qDebug() << "TCP from client:" << message;

    //format: SETTINGS:640x480:COLOR
    if (message.startsWith("SETTINGS:")) {
        applyClientSettings(message);
    }
}

// onResolutionChanged() function is called when we pick a different resolution from the combo box
// sends RES: message to client over TCP so client changes its camera resolution
void serverwindow::onResolutionChanged(int index)
{
    if (!clientSocket || clientSocket->state() != QAbstractSocket::ConnectedState)
        return;

    // Must match the combo box order in serverwindow.ui exactly:
    //   Index 0 → 1280 x 720   (default)
    //   Index 1 → 360 x 240
    //   Index 2 → 640 x 480 .. ..
    QStringList displayRes  = {"1280 x 720","360 x 240", "640 x 480", "800 x 600","960 x 720", "1024 x 576", "1552 x 1552", "1920 x 1080"};

    QStringList parts = resolutions[index].split("x");
    int newWidth = parts[0].toInt();
    int newHeight = parts[1].toInt();

    if (newWidth == CAM_WIDTH && newHeight == CAM_HEIGHT) return;

    // flush any half collected frames from the old resolution
    // old size packets would corrupt new size frames
    collector->clearBuffers();

    CAM_WIDTH = newWidth;
    CAM_HEIGHT = newHeight;
    frameCount = packetCount = lastFrames = lastPackets = totalBytes = 0;
    previousGrayFrame.fill(0, CAM_WIDTH * CAM_HEIGHT);

    resolutionOverlayLabel->setText(displayRes[index]);

    // sennding the resolution change command to client via TCP
    // client will receive for examplxe "RES:640x480" and reconfigure its camera
    QString message = "RES:" + resolutions[index];
    clientSocket->write(message.toUtf8());
    qDebug() << "Resolution change sent:" << message;
}

// called when user clicks the grayscale/color button
// regardless of this setting. we just choose to display it as color or convert to grayscale
// also sends MODE: to the client so the client can save the preference in its JSON file
void serverwindow::onColorToggleClicked()
{
    if (!clientSocket ||
        clientSocket->state() != QAbstractSocket::ConnectedState)
        return;

    isColorMode = !isColorMode;


    if (isColorMode) {
        ui->colorToggleButton->setText("◈ COLOR");
        ui->colorToggleButton->setStyleSheet(
            "QPushButton {"
            "  background-color: rgba(0, 255, 100, 0.06);"
            "  color: #00ff66;"
            "  border: 1px solid rgba(0, 255, 100, 0.25);"
            "  font-size: 14px; font-weight: bold;"
            "  font-family:'Courier New';"
            "  letter-spacing: 2px;"
            "}"
            "QPushButton:hover { background-color: rgba(0, 255, 100, 0.1); }"
            "QPushButton:pressed { background-color: rgba(0, 255, 100, 0.15); }");
        clientSocket->write("MODE:COLOR");
        qDebug() << "Display switched to COLOR mode";
    }
    else {
        ui->colorToggleButton->setText("◈ GRAYSCALE");
        ui->colorToggleButton->setStyleSheet(
            "QPushButton {"
            "  background-color: #0d1220;"
            "  color: #00ffff;"
            "  border: 1px solid rgba(0, 255, 255, 0.2);"
            "  font-size: 14px; font-weight: bold;"
            "  font-family: 'Courier New';"
            "  letter-spacing: 2px;"
            "}"
            "QPushButton:hover { background-color: rgba(0, 255, 255, 0.05); }"
            "QPushButton:pressed { background-color: rgba(0, 255, 255, 0.1); }");
        clientSocket->write("MODE:GRAYSCALE");
        qDebug() << "Display switched to GRAYSCALE mode";
    }
}

// receives settings from the client when it connects and updates the server UI to match
// the client sends "SETTINGS:640x480:COLOR" format containing its saved resolution and color mode
// this way the server combo box and color toggle always reflect what the client is actually doing
// we block signals on the combo box so changing the index doesnt trigger onResolutionChanged
// which would send RES: back to the client creating an infinite loop
void serverwindow::applyClientSettings(const QString &message)
{
    // "SETTINGS:640x480:COLOR" → removing SETTINGS: so we left with 640x480:COLOR
    QString rest = message.mid(9);

    // splitting by : "640x480", "COLOR"
    QStringList parts = rest.split(":");

    // parse resolution:640x480 → width=640, height=480
    QString resolution = parts[0];
    QString mode = parts[1];
    QStringList resolutionParts = resolution.split("x");

    int newWidth = resolutionParts[0].toInt();
    int newHeight = resolutionParts[1].toInt();


    CAM_WIDTH = newWidth;
    CAM_HEIGHT = newHeight;

    previousGrayFrame.fill(0, MOTION_WIDTH * MOTION_HEIGHT);

    // update the combo box to match so find which index has this resolution
    // block signals so onResolutionChanged doesnt fire and send RES: back to client
    // without blockSignals: setCurrentIndex → onResolutionChanged → sends RES: → client restarts camera for no reason
    // with blockSignals: just updates the UI quietly
    int index = resolutions.indexOf(resolution);
    if (index >= 0) {
        ui->resolutionComboBox->blockSignals(true);
        ui->resolutionComboBox->setCurrentIndex(index);
        ui->resolutionComboBox->blockSignals(false);
    }

    resolutionOverlayLabel->setText(QString::number(newWidth) + " x " + QString::number(newHeight));

    bool clientColorMode = (mode == "COLOR");

    if (clientColorMode != isColorMode) {
        isColorMode = clientColorMode;

        if (isColorMode) {
            ui->colorToggleButton->setText("◈ COLOR");
            ui->colorToggleButton->setStyleSheet(
                "QPushButton {"
                "  background-color: rgba(0, 255, 100, 0.06);"
                "  color: #00ff66;"
                "  border: 1px solid rgba(0, 255, 100, 0.25);"
                "  font-size: 14px; font-weight: bold;"
                "  font-family:'Courier New';"
                "  letter-spacing: 2px;"
                "}"
                "QPushButton:hover { background-color: rgba(0, 255, 100, 0.1); }"
                "QPushButton:pressed { background-color: rgba(0, 255, 100, 0.15); }");
        }
        else {
            ui->colorToggleButton->setText("◈ GRAYSCALE");
            ui->colorToggleButton->setStyleSheet(
                "QPushButton {"
                "  background-color: #0d1220;"
                "  color: #00ffff;"
                "  border: 1px solid rgba(0, 255, 255, 0.2);"
                "  font-size: 14px; font-weight: bold;"
                "  font-family:'Courier New';"
                "  letter-spacing: 2px;"
                "}"
                "QPushButton:hover { background-color: rgba(0, 255, 255, 0.05); }"
                "QPushButton:pressed { background-color: rgba(0, 255, 255, 0.1); }");
        }
    }

    qDebug() << "Applied client settings:" << newWidth << "x" << newHeight << (isColorMode ? "COLOR" : "GRAYSCALE");
}


// onUdpDataReceived function is called whenever UDP packets arrive on the udp port
// each packet has 8 byte header (by client using QDataStream BigEndian)
// bytes 0-1: packet index (which packet this is)
// bytes 2-3: total packets (how many packets the frame will split into)
// bytes 4-7: frame id (which frame these packets belong to)
// after the 8 byte header the rest is the actual JPEG data packet

// why we use quint ??
// normal int can vary in size different computers it might be 4 bytes on one machine and different on another.
// in network programming this is a problem because if the client writes a number as 2 bytes but the server reads it as 4 bytes everything will break.
// quint16 = always exactly 2 bytes, unsigned, range 0-65000ish
// quint32 = always exactly 4 bytes, unsigned, range 0-4 billion
// we use these in the UDP packet header so client and server always read and write
// the exact same byte sizes in the exact same order so nothing will cause chaos.
void serverwindow::onUdpDataReceived()
{
    // process all pending datagrams in a loop
    // multiple packets can arrive between readyRead signals so we drain them all
    while (udpSocket->hasPendingDatagrams()) {
        QNetworkDatagram datagram = udpSocket->receiveDatagram();
        QByteArray packet = datagram.data();

        // skip any packet smaller than 8 bytes because it cant have a valid header, we used quint type for that.
        if (packet.size() < 8) continue;

        // read the 8-byte header using QDataStream with BigEndian byte order
        // must match exactly how the client wrote it which means same tool, same byte order
        QDataStream stream(packet);
        stream.setByteOrder(QDataStream::BigEndian);

        quint16 packetindex, totalpackets;
        quint32 frameid;
        stream >> packetindex >> totalpackets >> frameid;

        // everything after the 8-byte header is the actual JPEG data for this packet
        QByteArray packetdata = packet.mid(8);

        // hand it to framecollector to store and try to collect into a complete frame
        collector->addpacket(packetindex, totalpackets, frameid, packetdata);
        packetCount++;
    }
}

// onFrameComplete is called by framecollector when all packets of a frame have been assembled
// receives the complete JPEG byte array, decodes it into a QImage
// runs motion detection on a tiny grayscale version and displays the image on screen
void serverwindow::onFrameComplete(const QByteArray &jpegData)
{
    // restart the frame timeout timer frames are still arriving so client is alive
    // if frames stop for 3 seconds this timer fires and disconnects the client
    uiTimer->start();

    // decode the JPEG bytes back into a QImage
    // this is the reverse of what the client did with image.save(&buffer, "JPEG")
    QImage image;
    if (!image.loadFromData(jpegData, "JPEG")) {
        qDebug() << "JPEG decode failed, size:" << jpegData.size();
        return;
    }

    // convert to tiny grayscale for motion detection
    // motion detection always runs regardless of color/grayscale display mode
    // because we want to detect motion even when viewing in color
    //QByteArray grayFrame = imageToGrayscaleBytes(image);
    //bool motionNow = detectMotion(grayFrame);

    QByteArray grayFrame = imageToMotionGray(image);
    bool motionNow = detectMotion(grayFrame);

    if (motionNow) {
        qDebug() << "Motion detected!";
        ui->motionLabel->setText("⚠ MOTION DETECTED");
        ui->motionLabel->setStyleSheet(
            "background-color: rgba(255, 32, 64, 0.15);"
            "color: #ff2040;"
            "font-weight: bold; font-size: 16px;"
            "font-family: 'Courier New';"
            "letter-spacing: 2px;");

        ui->motionStatLabel->setText("⚠");
        ui->motionStatLabel->setStyleSheet(
            "color: #ff2040;"
            "font-size: 18px; font-weight: bold;"
            "font-family: 'Courier New';"
            "background-color: transparent;");
        ui->motionStatLabel->setAlignment(Qt::AlignCenter);

        // after 1second clear the motion alert automatically
        QTimer::singleShot(1000, this, [this]() {
            ui->motionLabel->setText("");
            ui->motionLabel->setStyleSheet(
                "background-color: transparent;"
                "color: #ff2040;"
                "font-weight: bold; font-size: 12px;"
                "font-family: 'Courier New';"
                "letter-spacing: 2px;");
            ui->motionStatLabel->setText("—");
            ui->motionStatLabel->setStyleSheet(
                "color: #334455;"
                "font-size: 20px; font-weight: bold;"
                "font-family: 'Courier New';"
                "background-color: transparent;");
        });
    }

    // save current grayscale frame for next frames motion comparison
    previousGrayFrame = grayFrame;

    /*
     * using this method for image display is problematic because
     * doing displayImage = image creates a copy. Which creates extra memomry.
     * Method below we used passes image directly to fromImage() so not using copy or extra memory.
     *
    QImage displayImage;
    if (isColorMode) {
        displayImage = image;
    } else {
        displayImage = image.convertToFormat(QImage::Format_Grayscale8);
    }
    QPixmap pixmap = QPixmap::fromImage(displayImage).scaled(ui->videoLabel->width(),ui->videoLabel->height(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    */



    QPixmap pixmap;
    // scale the image to fit the videoLabel widget while keeping aspect ratio
    // FastTransformation is used instead of SmoothTransformation because its much faster we dont want lagging in the camera.
    if (isColorMode) {
        pixmap = QPixmap::fromImage(image).scaled(ui->videoLabel->width(),ui->videoLabel->height(), Qt::KeepAspectRatio, Qt::FastTransformation);
    } else {
        // convertToFormat(Grayscale8) converts the color image to grayscale for display
        // this is a server-side display choice because JPEG coming from client is always color
        pixmap = QPixmap::fromImage(image.convertToFormat(QImage::Format_Grayscale8)).scaled(ui->videoLabel->width(),ui->videoLabel->height(), Qt::KeepAspectRatio, Qt::FastTransformation);
    }
    ui->videoLabel->setPixmap(pixmap);


    // update statistics counters these get read and reset every second by statsTimer
    frameCount++;
    totalBytes += jpegData.size();
}

/*
    problem: at 1920x1080 the image freezes. the reason is our motion detection function
    was converting the all full resolution image to grayscale every single frame.
    which means 1920 x 1080 = 2 millionish pixels. for each pixel we read 3 bytes (R,G,B)
    and write 1 byte (grayscale) into a brand new byte array. thats 2 million pixels
    being copied and processed 20-30 times per second. the CPU cant keep up
    so display freezes, and everything falls behind.
    at 360x240 this was fine because thats only about 85 thousand pixels which is about 25x less work.
    but at 1080p the workload explodes and the CPU spends more time on motion detection
    than it takes for the next frame to arrive.


    solution:we dont need full resolution to detect motion. a person walking across the room
    is just as visible in a tiny 160x120 image as in a 1920x1080 image.
    so before doing any grayscale conversion we shrink the image to 160x120 first
    using Qt built in method to scale. then we only loop through 160 x 120 = 19,200 pixels
    instead of 2 million so thats 100x less work.
    the motion detection quality is virtually the same because we are just asking
    "did something move?" not "what exact pixel changed?"

    before: 1920x1080 -> grayscale (2,073,600 pixels) -> compare 2M pixels -> CPU dies
    after: 1920x1080 -> shrink to 160x120 -> grayscale (19,200 pixels) -> compare 19K pixels -> olleyy

    below here there is the problematic function:

QByteArray serverwindow::imageToGrayscaleBytes(const QImage &image) {
    int w = image.width();
    int h = image.height();
    QByteArray gray;
    gray.reserve(w * h);
    for (int row = 0; row < h; row++) {
        const uchar *scanLine = image.constScanLine(row);
        // since format is RGB888 one row is looking like this [R, G, B, R, G, B, ...]
        for (int col = 0; col < w; col++) {
            uchar R = scanLine[col * 3];
            uchar G = scanLine[col * 3 + 1];
            uchar B = scanLine[col * 3 + 2];
            uchar g = static_cast<uchar>(0.299 * R + 0.587 * G + 0.114 * B);
            gray.append(static_cast<char>(g));
        }
    }
    return gray;
}
*/


// imageToMotionGray() function first shrinks the QImage to 160x120 it will be blurry a bit but enough for motion detection
// this way motion detection always processes only 19200 pixels regardless of camera resolution
// whether the camera is at 360x240 or 1920x1080 the workload is the same
QByteArray serverwindow::imageToMotionGray(const QImage &image)
{
    // shrink to tiny fixed size — Qt does this efficiently with its internal scaler
    // FastTransformation because we do this every frame and dont need high quality for motion
    QImage tiny = image.scaled(MOTION_WIDTH, MOTION_HEIGHT, Qt::IgnoreAspectRatio, Qt::FastTransformation);

    // we need to be sure again its RGB888 so our pixel math works
    // RGB888 means each pixel is exactly 3 bytes: Red, Green, Blue in that order
    if (tiny.format() != QImage::Format_RGB888)
        tiny = tiny.convertToFormat(QImage::Format_RGB888);

    QByteArray gray;
    gray.reserve(MOTION_WIDTH * MOTION_HEIGHT);

    // looping through the tiny image only 19200 pixels instead of like 2 million
    // for each pixel we read R, G, B and convert to single brightness value
    // using ITU-R BT.601 formula which weights green most because human eyes are most sensitive to green
    for (int row = 0; row < MOTION_HEIGHT; row++) {
        // constScanLine gives us a pointer to the beginning of this rows pixel data in memory
        const uchar *scanLine = tiny.constScanLine(row);
        for (int col = 0; col < MOTION_WIDTH; col++) {
            // since RGB888 stores pixels as [R,G,B,R,G,B,...] pixel N starts at byte N*3
            uchar R = scanLine[col * 3];
            uchar G = scanLine[col * 3 + 1];
            uchar B = scanLine[col * 3 + 2];
            uchar g = static_cast<uchar>(0.299 * R + 0.587 * G + 0.114 * B);
            gray.append(static_cast<char>(g));
        }
    }
    return gray;
}

// compares current grayscale frame against the previous frame pixel by pixel
// for each pixel calculating absolute brightness difference
// if a pixel changed by more than 25 brightness level (because our PER_PIXEL_BRIGHTNESS_DIFFERENCE is 25) out of 255 count it as changed
// so if more than 2% of all pixels changed we will count this as motion
bool serverwindow::detectMotion(const QByteArray &currentGray)
{
    // if sizes dont match skip this frame
    // next frame will have matching sizes and comparison will work normally
    if (previousGrayFrame.size() != currentGray.size()) {
        return false;
    }
    int changedPixels = 0;
    for (int i = 0; i < currentGray.size(); i++) {
        //QByteArray keeps the datas as char which is signed and ranged between -128 and +127 but we said our
        //brightness values are between 0-255 so we make a safe conversion from char to unsigned char.
        int diff = std::abs(static_cast<uchar>(currentGray[i]) - static_cast<uchar>(previousGrayFrame[i]));
        if (diff > PER_PIXEL_BRIGHTNESS_DIFFERENCE)
            changedPixels++;
    }

    // calculate what fraction of total pixels changed
    // if 2% or more pixels changed by more than 25 brightness levels we call it motion
    double ratio = static_cast<double>(changedPixels) / currentGray.size();
    return (ratio >= MOTION_RATIO);
}
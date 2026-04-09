#ifndef SERVERWINDOW_H
#define SERVERWINDOW_H

#include <QMainWindow>
#include <QUdpSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QLabel>
#include <QTimer>
#include "framecollector.h"

QT_BEGIN_NAMESPACE
namespace Ui { class serverwindow; }
QT_END_NAMESPACE

class serverwindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit serverwindow(QWidget *parent = nullptr);
    ~serverwindow();

private slots:
    void onNewTcpConnection();
    void onTcpDataReceived();
    void onUdpDataReceived();
    void onFrameComplete(const QByteArray &jpegData);
    void onClientDisconnected();
    void onResolutionChanged(int index);
    void onColorToggleClicked();

private:
    Ui::serverwindow *ui;
    QTcpServer *tcpServer;
    QTcpSocket *clientSocket = nullptr;
    QUdpSocket *udpSocket;
    QLabel *resolutionOverlayLabel;

    framecollector *collector;
    //QByteArray imageToGrayscaleBytes(const QImage &image);
    QByteArray imageToMotionGray(const QImage &image);
    QByteArray previousGrayFrame;
    QTimer *statsTimer;
    QTimer *uiTimer;

    void updateConnectionStatus(bool connected);
    void applyClientSettings(const QString &message);
    bool detectMotion(const QByteArray &currentGray);

    int frameCount = 0;
    int packetCount = 0;
    int lastPackets = 0;
    int lastFrames = 0;
    int totalBytes = 0;
    int displayCounter = 0;
    static const int MOTION_WIDTH = 160;
    static const int MOTION_HEIGHT = 120;
    static const int PER_PIXEL_BRIGHTNESS_DIFFERENCE = 25;
    static constexpr double MOTION_RATIO = 0.02;
    bool isColorMode = false;
};

#endif // SERVERWINDOW_H

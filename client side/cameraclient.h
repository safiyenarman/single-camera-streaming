#ifndef CAMERACLIENT_H
#define CAMERACLIENT_H

#include <QObject>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QTimer>
#include <QNetworkInformation>
#include <QJsonObject>
#include <QJsonDocument>
#include <QFile>
#include <QCoreApplication>
#include "cameracapture.h"
#include "frameconverter.h"

class cameraclient : public QObject {
    Q_OBJECT

public:
    explicit cameraclient(QObject * parent = nullptr);

private slots:
    void connected();
    void lostConnection();
    void attemptConnectionAgain();
    void socketError(QAbstractSocket::SocketError error);
    void sendFrameUdp(const QByteArray &framedata);
    void onTcpDataReceived();
    void onNetworkReachabilityChanged(QNetworkInformation::Reachability reachability);

private:
    QTcpSocket *tcpSocket;
    QUdpSocket *udpSocket;
    QTimer *retryTimer;
    cameracapture *camera;
    frameconverter *fconverter;    
    QString settingsFilePath;

    void saveSettings();
    void loadSettings();
    void sendSettingsToServer();
    void resetTcpSocket();

    bool isColorMode = false;
    bool hasNetwork = false;
    bool isServerClosed = false;
    bool isConnected = false;
};

#endif // CAMERACLIENT_H

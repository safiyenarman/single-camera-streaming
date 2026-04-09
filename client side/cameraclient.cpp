#include "cameraclient.h"
#include <QDebug>
#include <QDataStream>

// server address and ports defined once here so if we need to change them
// we only change in one place instead of hunting through the whole code
QString IP_ADDRESS = "172.20.10.11";
int TCP_PORT = 3456;
int UDP_PORT = 4567;

cameraclient::cameraclient(QObject *parent) : QObject(parent)
{
    // creating the two sockets we need:
    // tcpSocket for control messages (resolution changes, mode changes)
    // udpSocket for sending video frame packets to the server
    tcpSocket = new QTcpSocket(this);
    udpSocket = new QUdpSocket(this);

    // retry timer fires every 5 seconds to attempt reconnection to server
    // but only when we have network and server isnt responding
    // it doesnt start here it starts when network becomes available or connection drops
    retryTimer = new QTimer(this);
    retryTimer->setInterval(5000);
    connect(retryTimer, &QTimer::timeout, this, &cameraclient::attemptConnectionAgain);

    // creating the camera hardware controller and the JPEG compressor
    camera = new cameracapture(this);
    fconverter = new frameconverter(this);

    // wiring up TCP socket signals so we know when connection succeeds or fails or when daata arrives
    connect(tcpSocket, &QTcpSocket::connected, this, &cameraclient::connected);
    connect(tcpSocket, &QTcpSocket::disconnected, this, &cameraclient::lostConnection);
    connect(tcpSocket, &QTcpSocket::errorOccurred, this, &cameraclient::socketError);
    connect(tcpSocket, &QTcpSocket::readyRead, this, &cameraclient::onTcpDataReceived);

    // 1. camera produces a raw frame → frameconverter scales and JPEG compresses it
    // 2. frameconverter finishes compression → cameraclient splits into UDP packets and sends
    connect(camera, &cameracapture::frameReady, fconverter, &frameconverter::processFrame);
    connect(fconverter, &frameconverter::frameConverted, this, &cameraclient::sendFrameUdp);

    // when the app is about to close save the current settings to JSON file
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &cameraclient::saveSettings);

    // we are building the settings file and for its path we are placing it to next to the executable file.
    // applicationDirPath() will return the directory that contains application executable.
    settingsFilePath = QCoreApplication::applicationDirPath() + "/camera_settings.json";

    // we are loading  the last saved resolution and color mode from JSON
    // if no file exists it keeps the defaults
    loadSettings();
    camera->start();

    // PROBLEM:
    // when wifi drops the TCP socket enters a broken state but we had no way of knowing
    // the network was gone. the old code just blindly retried connectToHost every 10 seconds
    // on the same broken socket which silently failed every time. even when wifi came back
    // the socket was still broken so it could never reconnect. we were only relying on
    // the socket itself to tell us about connection problems but the socket cant tell us
    // "wifi is gone" → it was able to only say "connection failed" without knowing why.
    //
    // SOLUTION:
    // we use QNetworkInformation which talks directly to the operating system and tells us
    // the actual network state, is wifi connected or not. now when wifi drops we know it
    // dropped so we stop wasting time retrying. when wifi comes back we know it came back
    // so we destroy the old broken socket, create a new one, and reconnect immediately.
    // the retry timer now only runs when we have wifi but the server isnt responding.
    if (QNetworkInformation::loadDefaultBackend()) {
        QNetworkInformation *netInfo = QNetworkInformation::instance();
        connect(netInfo, &QNetworkInformation::reachabilityChanged, this, &cameraclient::onNetworkReachabilityChanged);

        // checking the current network status right now at startup
        // if we already have network start connecting immediately
        // if not we just wait then onNetworkReachabilityChanged will tell us when network comes back
        QNetworkInformation::Reachability current = netInfo->reachability();
        if (current == QNetworkInformation::Reachability::Online || current == QNetworkInformation::Reachability::Site) {
            hasNetwork = true;
            retryTimer->start();
            tcpSocket->connectToHost(IP_ADDRESS, TCP_PORT);
            qDebug() << "Network available! Connecting to server...";
        }
        else {
            hasNetwork = false;
            qDebug() << "No network available! Waiting for connection...";
        }
    }
}

// called when TCP connection to server succeeds
// stops the retry timer since we dont need to retry anymore
// sends our saved settings (resolution + color mode) to server so its UI matches
// also saves settings to make sure the JSON file exists
void cameraclient::connected()
{
    isConnected = true;
    retryTimer->stop();
    qDebug() << "Connected to server!";
    sendSettingsToServer();
    saveSettings();
}

// called when TCP connection drops it could be server shutting down or network failure
// stops and restarts retry timer to attempt reconnection every 5 seconds
void cameraclient::lostConnection()
{
    isConnected = false;
    retryTimer->stop();

    if (isServerClosed) {
        qDebug() << "Server is closed! Retrying...";
    }
    else {
        qDebug() << "Disconnected from network! Retrying...";
    }
    retryTimer->start();
}

// called by retry timer every 5 seconds
// creates a fresh socket (old one might be broken) and tries to connect again
void cameraclient::attemptConnectionAgain()
{
    isServerClosed = false;
    resetTcpSocket();
    tcpSocket->connectToHost(IP_ADDRESS, TCP_PORT);
}

// called when a socket error happens
// we check if the error was RemoteHostClosedError (server shut down intentionally) or something else
// this helps us show the correct message in lostConnection()
void cameraclient::socketError(QAbstractSocket::SocketError error)
{
    if (error == QAbstractSocket::RemoteHostClosedError)
        isServerClosed = true;
    else
        isServerClosed = false;
}

// called automatically by the operating system when network state changes (wifi on/off)
// this is the core of the reconnection logic:
// when network goes down → stop everything, dont waste time retrying without wifi
// when network comes back → destroy old broken socket, create new one, then reconnect
void cameraclient::onNetworkReachabilityChanged(QNetworkInformation::Reachability reachability)
{
    if (reachability == QNetworkInformation::Reachability::Online || reachability == QNetworkInformation::Reachability::Site) {

        // only reconnect if we were previously without network
        // if hasNetwork was already true this is a duplicate signal and we ignore it
        if (!hasNetwork) {
            hasNetwork = true;
            qDebug() << "Network is back! Reconnecting...";
            // when network drops the TCP socket can be left in a broken state
            // where connectToHost silently fails. so we destroy the old socket
            // completely and create a fresh one to guarantee reliable reconnection
            resetTcpSocket();
            tcpSocket->connectToHost(IP_ADDRESS, TCP_PORT);
        }
    }
    else {
        // stop retrying theres no point trying to connect without wifi
        if (hasNetwork) {
            hasNetwork = false;
            isConnected = false;
            retryTimer->stop();
            qDebug() << "Network lost! Stopped retry timer, waiting for reconnection...";
        }
    }
}

// saves current resolution and color mode to a JSON file next to the executable
// called when receiving RES: or MODE: from server, connecting to server, and when app is closing
// JSON format: { "width": 1280, "height": 720, "isColorMode": true }
void cameraclient::saveSettings()
{
    // create a JSON object and fill in the current settings
    QJsonObject json;
    json["width"] = CAMERA_WIDTH;
    json["height"] = CAMERA_HEIGHT;
    json["isColorMode"] = isColorMode;

    // open the file for writing and write the JSON text into it
    // QIODevice::WriteOnly means it overwrites the old file completely
    QFile file(settingsFilePath);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(json).toJson());
        file.close();
        qDebug() << "Settings saved:" << CAMERA_WIDTH << "x" << CAMERA_HEIGHT << (isColorMode ? "COLOR" : "GRAYSCALE");
    }
}

// loads previously saved resolution and color mode from JSON file
// called once at startup before the camera starts
// if the file doesnt exist or cant be opened we keep the default values
void cameraclient::loadSettings()
{
    QFile file(settingsFilePath);

    // if no settings file exists this is probably the first run use defaults.
    if (!file.exists()) {
        qDebug() << "No settings file found, using defaults.";
        return;
    }

    if(!file.open(QIODevice::ReadOnly)) {
        qDebug() << "File could not opened!";
        return;
    }
    // read the entire file content and parse it as JSON
    QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    file.close();
    QJsonObject json = document.object();

    // restore the saved resolution
    // toInt(0) means if the key doesnt exist or is invalid return 0
    int newWidth = json["width"].toInt(0);
    int newHeight = json["height"].toInt(0);

    if (newWidth > 0 && newHeight > 0) {
        CAMERA_WIDTH = newWidth;
        CAMERA_HEIGHT = newHeight;
    }

    // restore the saved color mode
    // toBool(false) means if the key doesnt exist default to false (grayscale)
    isColorMode = json["isColorMode"].toBool(false);

    qDebug() << "Loaded settings:" << CAMERA_WIDTH << "x" << CAMERA_HEIGHT << (isColorMode ? "COLOR" : "GRAYSCALE");
}

// sends current settings to the server over TCP so the server UI matches what the client is doing
// called every time the client connects or reconnects to the server
// server will receive "SETTINGS:1280x720:COLOR" and adjust its combo box and button
void cameraclient::sendSettingsToServer()
{
    if (!isConnected) return;

    QString mode = isColorMode ? "COLOR" : "GRAYSCALE";
    QString message = "SETTINGS:" + QString::number(CAMERA_WIDTH) + "x" + QString::number(CAMERA_HEIGHT) + ":" + mode;

    tcpSocket->write(message.toUtf8());
    qDebug() << "Sent settings to server:" << message;
}

// destroys the old TCP socket and creates a brand new one with fresh signal connections
// needed because when network drops the TCP socket can be left in a broken state
void cameraclient::resetTcpSocket()
{
    tcpSocket->disconnect();
    // abort any ongoing connection attempt or data transfer
    tcpSocket->abort();
    // schedule the old socket for deletion
    tcpSocket->deleteLater();

    // create a brand new clean socket
    tcpSocket = new QTcpSocket(this);

    // rewire all the signals to the new socket same connections as in the constructor
    connect(tcpSocket, &QTcpSocket::connected, this, &cameraclient::connected);
    connect(tcpSocket, &QTcpSocket::disconnected, this, &cameraclient::lostConnection);
    connect(tcpSocket, &QTcpSocket::errorOccurred, this, &cameraclient::socketError);
    connect(tcpSocket, &QTcpSocket::readyRead, this, &cameraclient::onTcpDataReceived);
}

// called when frameconverter produces a compressed JPEG frame
// splits the JPEG bytes into 1400 byte UDP packets each with an 8-byte header
// and sends them to the server
// why 1400 bytes? because network transmission unit is like 1500 bytes. if we send bigger packets
// they get split by the OS into fragments and losing any fragment kills the whole packet.
// at 1400 bytes each packet fits in one transmission unit so it either arrives whole or doesnt no fragmentation we will experience.
void cameraclient::sendFrameUdp(const QByteArray &framedata)
{
    if (!isConnected) return;

    // each frame gets a unique incrementing id so the server knows which packets belong together
    static int frameid = 0;
    frameid++;

    // calculating how many 1400 byte packets we need to send the entire JPEG
    // ceiling division: for example (10000 + 1400 - 1) / 1400 = 8 packets
    int totalpackets = (framedata.size() + BYTES_PER_UDP_PACKET - 1) / BYTES_PER_UDP_PACKET;

    // loop through each packet and build it with header + data
    // we use QDataStream to write the header because it ensures bytes are written
    // in the exact right size and order. BigEndian means the most significant byte comes first
    // which is the standard for network protocols

    // why we use quint instead of normal int?
    // normal int can vary in size on different computers meaning it might be 4 bytes on one machine
    // and different on another. in network programming this is a problem because if the client
    // writes a number as 2 bytes but the server reads it as 4 bytes everything breaks.
    // quint16 = always exactly 2 bytes, unsigned, range 0-65000ish
    // quint32 = always exactly 4 bytes, unsigned, range 0-4 billion
    // we use these so client and server always read and write the exact same byte sizes
    for (int i = 0; i < totalpackets; i++) {
        QByteArray packet;
        QDataStream header(&packet, QIODevice::WriteOnly);
        header.setByteOrder(QDataStream::BigEndian);

        // writing the 8-byte header that the server reads first to collect the frame:
        // bytes 0-1 (quint16): packet index meaning which packets this is (0, 1, 2...)
        // bytes 2-3 (quint16): total packets meaning how many packets this frame was split into
        // bytes 4-7 (quint32): frame id meaning which frame these packets belong to
        header << static_cast<quint16>(i);
        header << static_cast<quint16>(totalpackets);
        header << static_cast<quint32>(frameid);

        // after the 8-byte header append the actual JPEG data for this chunk
        // so final packet looks like: [8-byte header] + [up to 1400 bytes of JPEG data]
        packet.append(framedata.mid(i * BYTES_PER_UDP_PACKET, BYTES_PER_UDP_PACKET));
        udpSocket->writeDatagram(packet, QHostAddress(IP_ADDRESS), UDP_PORT);
    }

    qDebug() << "Frame:" << frameid << "Packets:" << totalpackets << "Bytes:" << framedata.size();
}

// called when TCP data arrives from the server
// handles two types of messages:
// RES:640x480 where server wants us to change camera resolution
// MODE:COLOR or MODE:GRAYSCALE where server wants us to change display mode
void cameraclient::onTcpDataReceived()
{
    // read all available TCP data and convert from raw bytes to readable string
    QByteArray data = tcpSocket->readAll();
    QString message = QString::fromUtf8(data);

    qDebug() << "TCP message received:" << message;

    // handle resolution change from server
    // server sends "RES:640x480" when user picks a new resolution from the combo box
    if (message.startsWith("RES:")) {
        // extracting the resolution part: "RES:640x480" to "640x480"
        QString res = message.mid(4);
        // spliting by "x" where "640x480" becomes ["640", "480"]
        QStringList parts = res.split("x");
        if (parts.size() == 2) {
            int newWidth = parts[0].toInt();
            int newHeight = parts[1].toInt();

            // ignore invalid values
            if (newWidth <= 0 || newHeight <= 0) {
                qDebug() << "Invalid resolution received, ignoring.";
                return;
            }

            qDebug() << "Changing resolution to" << newWidth << "x" << newHeight;

            // update the global resolution variables that all client files share
            CAMERA_WIDTH = newWidth;
            CAMERA_HEIGHT = newHeight;

            // stop camera then apply the new resolution then start again
            camera->stop();
            camera->cameraSettings();
            camera->start();

            // save so next startup uses this resolution
            saveSettings();
        }
    }
    // handle color mode change from server
    // server sends "MODE:COLOR" or "MODE:GRAYSCALE" when user clicks the button
    else if(message.startsWith("MODE:")) {
        // extract the mode part: from "MODE:COLOR" to "COLOR"
        QString mode = message.mid(5);
        if (mode == "COLOR") isColorMode = true;
        else isColorMode = false;
        saveSettings();
    }
}
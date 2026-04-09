#ifndef FRAMECOLLECTOR_H
#define FRAMECOLLECTOR_H

#include <QObject>
#include <QByteArray>
#include <QHash>

extern int CAM_WIDTH;
extern int CAM_HEIGHT;

class framecollector : public QObject
{
    Q_OBJECT

public:
    explicit framecollector(QObject *parent = nullptr);

    //server will call this function once for every UDP packet it receives
    void addpacket(quint16 packetindex, quint16 totalpackets,quint32 frameid,const QByteArray &packetdata);
    void clearBuffers();

signals:
    // fires when all packets of a frame have arrived successfully
    void frameComplete(const QByteArray &frameData);

private:

    // each framebuffer represents partially received frame and it holds 2 things
    // packets: hash map where key is the packet index and value is packets raw byte data
    // totalpackets:how many packets this frame should have in total
    struct FrameBuffer {
        QHash<quint16, QByteArray> packets;
        quint16 totalpackets = 0;
    };

    // Holds partially received frames key is frameid and value is framebuffer
    QHash<quint32, FrameBuffer> pendingFrames;

    void tryToCollect(quint32 frameid); //called after every new packet arrives
    void discardOldFrames(quint32 currentFrameid); //called before storing new packet
};
#endif // FRAMECOLLECTOR_H

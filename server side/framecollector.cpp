#include "framecollector.h"
#include <QDebug>

framecollector::framecollector(QObject *parent) : QObject(parent) { }

// the function addpacket() is called every time when server gets a udp packet
// there are 4 information coming with packet, which packet is this, how many total packets there are,
// which frame does the packet belong to and the data in the packet.
// at first we are cleaning the old frames that are old and will not completed with discardOldFrames()
// after we get the buffer for the frame, we are identifying how much packets we will be using and placing the packet in the correct position in hashmap
// finally with trytocollect() we will be checking wheter frame has all of its packets and collect them.
void framecollector::addpacket(quint16 packetindex, quint16 totalpackets, quint32 frameid, const QByteArray &packetdata)
{
    discardOldFrames(frameid);

    // pendingFrames[frameid] does two things automatically:
    // if this frameid was never seen before → creates a new empty FrameBuffer
    // if this frameid already exists → returns the existing one
    // the & means buffer is a reference to the original not a copy
    // so changes we make to buffer directly affect whats inside pendingFrames
    FrameBuffer &buffer = pendingFrames[frameid];
    buffer.totalpackets = totalpackets;

    // if we were not using hash map packets would arrive out of order on UDP (like 3, 0, 2, 1) but the hash map
    // makes each one in the right slot by index so order doesnt matter during collection we will map according to the correct order
    buffer.packets[packetindex] = packetdata;
    tryToCollect(frameid);
}

//clearBuffers() cleans all interrupted frames that are collected by the buffer
// called when resolution changes so old size frames dont mix with new size frames
void framecollector::clearBuffers()
{
    pendingFrames.clear();
}

// we call trytocollect() function every time a new packet arrives and checking wheter the frame is completed.
// if all packets are arrived, we will collect them in order into one big byte array.
// finally we use frameComplete signal for serverwindow can decode and display the image.
void framecollector::tryToCollect(quint32 frameid)
{
    FrameBuffer &buffer = pendingFrames[frameid];

    // if collected count doesnt match expected total meaning frame is not ready yet
    if (buffer.packets.size() != buffer.totalpackets) return;

    // all packets will be placed  in fullFrame so we will collect them in order.
    // JPEG size is variable so we dont know exact size to pre-allocate
    QByteArray fullFrame;

    // our loop from packet 0 to last packet and append each one in order
    // order matters because the JPEG data was split sequentially on the client side
    for (int i = 0; i < buffer.totalpackets; i++) {

        // what we do here is safety check if somehow a packet index is missing even though count matched
        // we will be discarding this frame entirely
        if (!buffer.packets.contains(i)) {
            qDebug() << "Frame" << frameid << "missing chunk:" << i;
            pendingFrames.remove(frameid);
            return;
        }
        fullFrame.append(buffer.packets[i]);
    }
    qDebug() << "Frame" << frameid << "complete" << fullFrame.size() << "bytes";

    // frame is collected so remove it from pending to free memory
    // without this pendingFrames would grow forever and will fill all RAM
    pendingFrames.remove(frameid);

    // firing the signal so serverwindow receives this and decodes the JPEG for display
    emit frameComplete(fullFrame);
}

// in discardOldFrames function we clean up old frames that are too old and will never complete
// if a frames id is more than 5 behind the current arriving frame it will be cleaned.
// for example if current frame is #100 and frame #94 is still sitting incomplete in the buffer
// those packets are clearly lost and the frame will never be whole so we throw it away
void framecollector::discardOldFrames(quint32 currentFrameid)
{
    quint32 MAX_FRAME_GAP = 5;
    QList<quint32> toremove;

    // we collect ids to remove first then remove in a separate loop
    // because we are using hash map deleting from a hash map while iterating through it is dangerous.
    // it can corrupt the iterator and crash the program
    for (auto iterator = pendingFrames.begin(); iterator != pendingFrames.end(); ++iterator) {
        // iterator.key() is the frame id of each pending frame
        // if the current frame is more than 5 ahead of it that frame is stale
        if (currentFrameid > iterator.key() + MAX_FRAME_GAP) {
            toremove.append(iterator.key());
        }
    }

    // now safely remove the stale frames outside of the iteration loop
    for (int i = 0; i < toremove.size(); i++) {
        quint32 id = toremove[i];
        qDebug() << "Discarding stale, incomplete frame" << id;
        pendingFrames.remove(id);
    }
}
# Single-Camera IP Streaming System

A real-time networked camera streaming system built with Qt and C++17, developed during
an internship at Turkish Technic R&D Department. A Linux-based Raspberry Pi captures and
streams live video to a macOS server over a local network, using TCP for control commands
and UDP for video data simultaneously.

---

## System Overview

```
[Raspberry Pi Client]                          [macOS Server]
 cameracapture                                  framecollector
     ↓ raw frame                                    ↓ assembled JPEG
 frameconverter  ── JPEG compress ──>           serverwindow
     ↓ JPEG bytes                                    ↓ decode + display
 cameraclient  ── UDP packets (port 4567) ──>   motion detection
     ↑                                              ↓
     └────────── TCP commands (port 3456) ──────────┘
                 RES: / MODE: / SETTINGS:
```

---

## Features

**Client side (Raspberry Pi / Linux)**
- Captures raw video frames using `QVideoSink` and JPEG-compresses them in memory via `QBuffer`
- Splits each JPEG frame into fixed 1,400-byte UDP packets to avoid IP fragmentation
- Each packet carries an 8-byte header: packet index, total packet count, and frame ID written in Big Endian byte order using `QDataStream`
- Receives resolution (`RES:`) and display mode (`MODE:`) commands from the server over TCP
- Persists last-used resolution and color mode to a JSON settings file, restoring them on next startup
- Handles WiFi disconnection and reconnection via `QNetworkInformation` — destroys and recreates the TCP socket on reconnect to avoid broken socket state
- Retries server connection every 5 seconds when network is available but server is unreachable

**Server side (macOS / any Qt platform)**
- Receives UDP packets and reassembles complete JPEG frames using a hash map keyed by frame ID
- Discards stale incomplete frames that are more than 5 frame IDs behind the current frame
- Decodes JPEG and displays the live feed scaled to the video label
- Supports color and grayscale display modes switchable at runtime
- Sends resolution and mode change commands to the client over TCP
- Synchronizes its UI combo box and color toggle with the client's saved settings on connect (without re-triggering commands back to the client via signal blocking)
- Real-time motion detection: downscales every frame to 160×120 before pixel comparison, reducing workload by ~100× vs full resolution; triggers a visual alert when more than 2% of pixels change by more than 25 brightness levels
- Live statistics panel: frames per second, average UDP packets per frame, average compressed frame size
- Frame timeout: if no frames arrive for 3 seconds, the client is considered disconnected and the UI resets

**Supported resolutions**

`360×240` `640×480` `800×600` `960×720` `1024×576` `1280×720` `1552×1552` `1920×1080`

---

## Project Structure

### Client (`clientCamera.pro`)

| File | Description |
|------|-------------|
| `cameracapture.h / .cpp` | Camera hardware controller using `QCamera` and `QVideoSink` |
| `frameconverter.h / .cpp` | Scales and JPEG-compresses raw video frames in memory |
| `cameraclient.h / .cpp` | TCP/UDP networking, settings persistence, reconnection logic |
| `main.cpp` | Entry point — creates `QCoreApplication` and `cameraclient` |

### Server (`ServerCamera.pro`)

| File | Description |
|------|-------------|
| `framecollector.h / .cpp` | Reassembles UDP packets into complete JPEG frames |
| `serverwindow.h / .cpp` | Main UI window — display, motion detection, statistics, TCP control |
| `serverwindow.ui` | Qt Designer UI layout |
| `main.cpp` | Entry point — creates `QApplication` and `serverwindow` |

---

## Qt Modules Used

**Client:** `core` `network` `multimedia`

**Server:** `core` `gui` `widgets` `network`

---

## Requirements

- Qt 6.x
- C++17
- Client: Linux (Raspberry Pi 5 recommended) with USB camera
- Server: macOS, Linux, or Windows
- Both devices on the same local network

---

## Configuration

In `cameraclient.cpp`, set the server's IP address and ports before building:

```cpp
QString IP_ADDRESS = "172.20.10.11";  // ← your server's IP
int TCP_PORT = 3456;
int UDP_PORT = 4567;
```

In `frameconverter.cpp`, adjust default resolution and JPEG quality if needed:

```cpp
int CAMERA_WIDTH  = 1280;
int CAMERA_HEIGHT = 720;
int JPEG_QUALITY  = 70;   // 0–100, lower = smaller packets, more reliable on WiFi
int BYTES_PER_UDP_PACKET = 1400;  // stay under MTU to avoid fragmentation
```

---

## How to Build

### Client (on Raspberry Pi)
```bash
git clone https://github.com/yourusername/single-camera-streaming.git
cd single-camera-streaming/client

qmake clientCamera.pro
make
./clientCamera
```

### Server (on macOS or Linux)
```bash
cd single-camera-streaming/server

qmake ServerCamera.pro
make
./ServerCamera
```

Or open either `.pro` file directly in **Qt Creator** and click Run.

---

## Key Technical Decisions

**Why TCP and UDP simultaneously**
TCP guarantees delivery, making it ideal for control commands where losing an instruction
would cause incorrect behaviour. UDP is used for video because low latency matters more
than guaranteed delivery — a lost frame is preferable to a delayed one that stalls the feed.

**Why 1,400-byte UDP packets**
Network MTU is typically 1,500 bytes. Packets larger than the MTU get fragmented by the
OS into sub-packets, and losing any single fragment discards the entire original packet.
By splitting JPEG frames into 1,400-byte chunks at the application level, each chunk
travels as one unfragmented datagram, dramatically improving reliability over WiFi.

**Why JPEG compression**
A raw 1280×720 RGB frame is ~2.7 MB. At 20–30 fps this would saturate a local network
and cause severe packet loss. JPEG at quality 70 reduces the same frame to ~8–15 KB
(~20× smaller), making transmission practical and reliable.

**Why motion detection runs at 160×120**
Running pixel comparison at full 1920×1080 resolution requires processing ~2 million
pixels per frame at 20–30 fps, which froze the server display thread. Downscaling to
160×120 first reduces the workload by ~100× while retaining enough detail to reliably
detect movement.

**WiFi reconnection via QNetworkInformation**
When WiFi drops, a TCP socket enters a broken state where subsequent `connectToHost`
calls silently fail. The solution monitors actual OS-level network reachability via
`QNetworkInformation`. When the network returns, the old broken socket is destroyed
completely and a fresh one is created before reconnecting.

---

## Context

This was the second of three progressive software deliverables developed during an
internship at Turkish Technic (Turk Hava Yollari Teknik A.S.), R&D Department, Software
Development Team. It established the client-server architecture and networking foundation
for the subsequent multi-camera IP streaming system.

---

## Author

Safiye Nur Narman
Computer Science and Engineering — Sabanci University
Internship at Turkish Technic R&D Department, 2026

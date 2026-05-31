#include "apps/handshake/handshake.hpp"

HandshakeApp::HandshakeApp(SerialIdentifier jack) : jack_(jack) {}

void HandshakeApp::start(SerialManager* serialManager) {
    if (serialManager) {
        serialManager->setOnBytesReceivedCallback(
            [this](const uint8_t* data, size_t len) { demuxer_.feed(data, len); }, jack_);
    }
}

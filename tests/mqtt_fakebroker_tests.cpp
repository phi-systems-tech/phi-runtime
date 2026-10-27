// The MQTT client against a broker that behaves, and several that do not
// (from phi-adapter-z2m, where the client lived until 2026-10-27).
//
// The broker here is a few hundred lines of MQTT 3.1.1 - enough to accept a
// CONNECT, refuse one, say nothing, echo a PUBLISH back, and go away - so the
// paths a person is going to hit (wrong password, broker down, broker gone
// mid-session) are exercised without a mosquitto process.

#include "phi/runtime/mqtt/client.h"
#include "test_support.h"

#include "phi/runtime/epollloop.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mqtt = phi::runtime::mqtt;
using namespace std::chrono_literals;

namespace {

class FakeBroker
{
public:
    enum class Manner {
        Accept,        ///< CONNACK 0, SUBACK, echo every PUBLISH back as retained
        Refuse,        ///< CONNACK 5: not authorised
        StaySilent,    ///< accepts TCP and never says a word
        GoAway,        ///< CONNACK 0, then closes the socket
    };

    explicit FakeBroker(Manner manner) : m_manner(manner)
    {
        m_listen = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        const int one = 1;
        ::setsockopt(m_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        m_ok = ::bind(m_listen, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0
            && ::listen(m_listen, 4) == 0;
        socklen_t length = sizeof(addr);
        if (m_ok && ::getsockname(m_listen, reinterpret_cast<sockaddr *>(&addr), &length) == 0)
            m_port = ::ntohs(addr.sin_port);
        m_thread = std::thread([this]() { serve(); });
    }

    ~FakeBroker()
    {
        m_done.store(true);
        ::shutdown(m_listen, SHUT_RDWR);
        ::close(m_listen);
        if (m_thread.joinable())
            m_thread.join();
    }

    bool ok() const { return m_ok; }
    std::uint16_t port() const { return m_port; }

    std::string clientId() const { std::lock_guard<std::mutex> l(m_mutex); return m_clientId; }
    std::string username() const { std::lock_guard<std::mutex> l(m_mutex); return m_username; }
    std::string password() const { std::lock_guard<std::mutex> l(m_mutex); return m_password; }
    std::vector<std::string> subscriptions() const { std::lock_guard<std::mutex> l(m_mutex); return m_subscriptions; }
    int disconnectsSeen() const { return m_disconnects.load(); }

private:
    static bool readAll(int fd, void *buffer, std::size_t length)
    {
        auto *out = static_cast<char *>(buffer);
        while (length > 0) {
            const ssize_t n = ::recv(fd, out, length, 0);
            if (n <= 0)
                return false;
            out += n;
            length -= static_cast<std::size_t>(n);
        }
        return true;
    }

    static bool readPacket(int fd, int *type, std::string *body)
    {
        unsigned char first = 0;
        if (!readAll(fd, &first, 1))
            return false;
        *type = first >> 4;
        std::size_t remaining = 0;
        std::size_t multiplier = 1;
        for (int i = 0; i < 4; ++i) {
            unsigned char byte = 0;
            if (!readAll(fd, &byte, 1))
                return false;
            remaining += (byte & 0x7F) * multiplier;
            multiplier *= 128;
            if ((byte & 0x80) == 0)
                break;
        }
        body->assign(remaining, '\0');
        return remaining == 0 || readAll(fd, body->data(), remaining);
    }

    static std::string field(const std::string &body, std::size_t *pos)
    {
        if (*pos + 2 > body.size())
            return {};
        const std::size_t length = (static_cast<unsigned char>(body[*pos]) << 8)
            | static_cast<unsigned char>(body[*pos + 1]);
        *pos += 2;
        if (*pos + length > body.size())
            return {};
        std::string out = body.substr(*pos, length);
        *pos += length;
        return out;
    }

    static std::string packet(unsigned char first, const std::string &body)
    {
        std::string out(1, static_cast<char>(first));
        std::size_t remaining = body.size();
        do {
            unsigned char byte = remaining % 128;
            remaining /= 128;
            if (remaining > 0)
                byte |= 0x80;
            out.push_back(static_cast<char>(byte));
        } while (remaining > 0);
        return out + body;
    }

    static void sendAll(int fd, const std::string &bytes)
    {
        ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
    }

    void handleConnect(int client, const std::string &body)
    {
        std::size_t pos = 0;
        field(body, &pos);           // protocol name
        pos += 1;                    // level
        const unsigned char flags = pos < body.size() ? static_cast<unsigned char>(body[pos]) : 0;
        pos += 1;
        pos += 2;                    // keepalive
        const std::string clientId = field(body, &pos);
        if (flags & 0x04) {          // will
            field(body, &pos);
            field(body, &pos);
        }
        const std::string username = (flags & 0x80) ? field(body, &pos) : std::string();
        const std::string password = (flags & 0x40) ? field(body, &pos) : std::string();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_clientId = clientId;
            m_username = username;
            m_password = password;
        }
        switch (m_manner) {
        case Manner::Refuse:
            sendAll(client, packet(0x20, std::string("\x00\x05", 2)));
            break;
        case Manner::StaySilent:
            break;
        case Manner::Accept:
        case Manner::GoAway:
            sendAll(client, packet(0x20, std::string("\x00\x00", 2)));
            break;
        }
    }

    void serve()
    {
        while (!m_done.load()) {
            const int client = ::accept(m_listen, nullptr, nullptr);
            if (client < 0)
                return;
            bool open = true;
            while (open && !m_done.load()) {
                int type = 0;
                std::string body;
                if (!readPacket(client, &type, &body))
                    break;
                switch (type) {
                case 1:
                    handleConnect(client, body);
                    if (m_manner == Manner::GoAway) {
                        std::this_thread::sleep_for(100ms);
                        open = false;
                    }
                    if (m_manner == Manner::Refuse)
                        open = false;
                    break;
                case 8: { // SUBSCRIBE
                    std::size_t pos = 2;
                    while (pos < body.size()) {
                        const std::string filter = field(body, &pos);
                        pos += 1; // qos
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_subscriptions.push_back(filter);
                    }
                    sendAll(client, packet(0x90, body.substr(0, 2) + std::string("\x00", 1)));
                    break;
                }
                case 3: // PUBLISH, qos 0: echoed back as a retained message
                    sendAll(client, packet(0x31, body));
                    break;
                case 12:
                    sendAll(client, packet(0xD0, {}));
                    break;
                case 14:
                    m_disconnects.fetch_add(1);
                    open = false;
                    break;
                default:
                    break;
                }
            }
            ::shutdown(client, SHUT_RDWR);
            ::close(client);
        }
    }

    Manner m_manner;
    int m_listen = -1;
    bool m_ok = false;
    std::uint16_t m_port = 0;
    std::thread m_thread;
    std::atomic_bool m_done{false};
    std::atomic_int m_disconnects{0};
    mutable std::mutex m_mutex;
    std::string m_clientId;
    std::string m_username;
    std::string m_password;
    std::vector<std::string> m_subscriptions;
};

mqtt::Settings settingsFor(std::uint16_t port)
{
    mqtt::Settings settings;
    settings.clientId = "phi-runtime-test";
    settings.host = "127.0.0.1";
    settings.port = port;
    settings.username = "zigbee2mqtt_test";
    settings.password = "geheim";
    settings.keepAliveSeconds = 5;
    settings.connectTimeout = 2000ms;
    return settings;
}

/// Runs the loop until `done` or the budget runs out.
void runUntil(phi::runtime::EpollLoop &loop, std::chrono::milliseconds budget)
{
    phi::runtime::Timer watchdog = loop.timerAfter(budget, [&loop]() { loop.stop(); });
    loop.run();
}

void testConnectsSubscribesAndHearsItself()
{
    FakeBroker broker(FakeBroker::Manner::Accept);
    CHECK(broker.ok());
    phi::runtime::EpollLoop loop;
    mqtt::Client client(loop);

    bool connected = false;
    std::string disconnectReason = "(none)";
    mqtt::Message heard;
    bool heardSomething = false;
    client.onConnected([&]() {
        connected = true;
        CHECK(client.subscribe("zigbee2mqtt/#"));
        CHECK(client.publish("zigbee2mqtt/Kitchen lamp", R"({"state":"ON"})"));
    });
    client.onDisconnected([&](const std::string &reason) { disconnectReason = reason; loop.stop(); });
    client.onMessage([&](const mqtt::Message &message) {
        heard = message;
        heardSomething = true;
        loop.stop();
    });

    std::string error;
    CHECK_MSG(client.connect(settingsFor(broker.port()), &error), "%s", error.c_str());
    CHECK(client.state() == mqtt::Client::State::Connecting);
    // A second connect while one is under way is refused, not stacked.
    CHECK(!client.connect(settingsFor(broker.port()), &error));
    runUntil(loop, 5s);

    CHECK(connected);
    CHECK(client.connected());
    CHECK_MSG(heardSomething, "the echoed publish never came back");
    CHECK(heard.topic == "zigbee2mqtt/Kitchen lamp");
    CHECK(heard.payload == R"({"state":"ON"})");
    CHECK(heard.retained);
    CHECK(disconnectReason == "(none)");
    // The account reached the wire.
    CHECK(broker.clientId() == "phi-runtime-test");
    CHECK(broker.username() == "zigbee2mqtt_test");
    CHECK(broker.password() == "geheim");
    CHECK(broker.subscriptions().size() == 1 && broker.subscriptions()[0] == "zigbee2mqtt/#");

    // Leaving on purpose is quiet, and tells the broker.
    client.disconnect();
    CHECK(client.state() == mqtt::Client::State::Disconnected);
    CHECK(!client.publish("zigbee2mqtt/x", "y"));
    runUntil(loop, 200ms);
    CHECK_MSG(disconnectReason == "(none)", "a deliberate disconnect was reported: %s",
                  disconnectReason.c_str());
    std::this_thread::sleep_for(100ms);
    CHECK(broker.disconnectsSeen() == 1);
}

void testARefusalIsNamed()
{
    FakeBroker broker(FakeBroker::Manner::Refuse);
    phi::runtime::EpollLoop loop;
    mqtt::Client client(loop);
    bool connected = false;
    std::string reason;
    client.onConnected([&]() { connected = true; });
    client.onDisconnected([&](const std::string &why) { reason = why; loop.stop(); });
    CHECK(client.connect(settingsFor(broker.port())));
    runUntil(loop, 5s);
    CHECK(!connected);
    CHECK(client.state() == mqtt::Client::State::Disconnected);
    CHECK_MSG(reason.find("refused") != std::string::npos
                      && reason.find("authori") != std::string::npos,
                  "reason was: %s", reason.c_str());
}

void testASilentBrokerIsGivenUpOn()
{
    FakeBroker broker(FakeBroker::Manner::StaySilent);
    phi::runtime::EpollLoop loop;
    mqtt::Client client(loop);
    std::string reason;
    client.onDisconnected([&](const std::string &why) { reason = why; loop.stop(); });
    mqtt::Settings settings = settingsFor(broker.port());
    settings.connectTimeout = 300ms;
    const auto started = std::chrono::steady_clock::now();
    CHECK(client.connect(settings));
    runUntil(loop, 5s);
    const auto waited = std::chrono::steady_clock::now() - started;
    CHECK_MSG(reason.find("did not answer") != std::string::npos, "reason was: %s", reason.c_str());
    CHECK(waited < 2s);
    CHECK(client.state() == mqtt::Client::State::Disconnected);
}

void testNobodyListening()
{
    // A port that was ours a moment ago and is closed now.
    std::uint16_t port = 0;
    {
        const int probe = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        ::bind(probe, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
        socklen_t length = sizeof(addr);
        ::getsockname(probe, reinterpret_cast<sockaddr *>(&addr), &length);
        port = ::ntohs(addr.sin_port);
        ::close(probe);
    }
    phi::runtime::EpollLoop loop;
    mqtt::Client client(loop);
    std::string reason;
    bool connected = false;
    client.onConnected([&]() { connected = true; });
    client.onDisconnected([&](const std::string &why) { reason = why; loop.stop(); });
    const auto started = std::chrono::steady_clock::now();
    std::string error;
    const bool started_ok = client.connect(settingsFor(port), &error);
    if (started_ok)
        runUntil(loop, 5s);
    else
        reason = error;
    const auto waited = std::chrono::steady_clock::now() - started;
    CHECK(!connected);
    CHECK_MSG(!reason.empty(), "a refused connection had no reason");
    CHECK_MSG(waited < 1500ms, "a refused connection took the whole deadline");
    CHECK(client.state() == mqtt::Client::State::Disconnected);
}

void testTheBrokerGoingAwayIsReported()
{
    FakeBroker broker(FakeBroker::Manner::GoAway);
    phi::runtime::EpollLoop loop;
    mqtt::Client client(loop);
    bool connected = false;
    std::string reason;
    client.onConnected([&]() { connected = true; });
    client.onDisconnected([&](const std::string &why) { reason = why; loop.stop(); });
    CHECK(client.connect(settingsFor(broker.port())));
    runUntil(loop, 5s);
    CHECK(connected);
    CHECK_MSG(reason.find("closed") != std::string::npos || reason.find("lost") != std::string::npos,
                  "reason was: %s", reason.c_str());
    CHECK(client.state() == mqtt::Client::State::Disconnected);

    // And it can go again from the same object.
    FakeBroker again(FakeBroker::Manner::Accept);
    connected = false;
    client.onConnected([&]() { connected = true; loop.stop(); });
    CHECK(client.connect(settingsFor(again.port())));
    runUntil(loop, 5s);
    CHECK(connected);
    client.disconnect();
}

} // namespace

int main()
{
    testConnectsSubscribesAndHearsItself();
    testARefusalIsNamed();
    testASilentBrokerIsGivenUpOn();
    testNobodyListening();
    testTheBrokerGoingAwayIsReported();
    return phitest::report("mqtt_fakebroker_tests");
}

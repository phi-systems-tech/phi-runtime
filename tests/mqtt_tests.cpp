// The MQTT client against a real broker: a mosquitto started on a free port
// for the run. Without the binary (a package build) the test is skipped.

#include "phi/runtime/epollloop.h"
#include "phi/runtime/mqtt/client.h"
#include "test_support.h"

#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

extern char **environ;

namespace {

using phi::runtime::EpollLoop;
using phi::runtime::Loop;
namespace mqtt = phi::runtime::mqtt;

constexpr const char *kBroker = "/usr/sbin/mosquitto";

int freePort()
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr);
    socklen_t len = sizeof addr;
    ::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len);
    ::close(fd);
    return ntohs(addr.sin_port);
}

bool waitFor(Loop &loop, const std::function<bool()> &predicate, int timeoutMs = 5000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return predicate();
        auto slice = loop.timerAfter(std::chrono::milliseconds(20), [&loop]() { loop.stop(); });
        loop.run();
    }
    return true;
}

mqtt::Settings settingsFor(int port, const std::string &clientId)
{
    mqtt::Settings s;
    s.clientId = clientId;
    s.host = "127.0.0.1";
    s.port = port;
    s.connectTimeout = std::chrono::milliseconds(3000);
    return s;
}

void testUnreachableBrokerSaysWhy(Loop &loop, int closedPort)
{
    mqtt::Client client(loop);
    bool connected = false;
    std::string reason;
    client.onConnected([&]() { connected = true; });
    client.onDisconnected([&](const std::string &r) { reason = r.empty() ? "?" : r; });
    std::string error;
    const bool started = client.connect(settingsFor(closedPort, "nobody"), &error);
    // Refused at once or reported through the callback: either way, a reason.
    CHECK(started ? waitFor(loop, [&]() { return !reason.empty(); }) : !error.empty());
    CHECK(!connected);
    CHECK(client.state() == mqtt::Client::State::Disconnected);
}

void testPublishSubscribeRetainAndWill(Loop &loop, int port)
{
    mqtt::Client listener(loop);
    std::vector<mqtt::Message> heard;
    listener.onMessage([&](const mqtt::Message &m) { heard.push_back(m); });
    CHECK(listener.connect(settingsFor(port, "listener")));
    CHECK(waitFor(loop, [&]() { return listener.connected(); }));
    CHECK(listener.subscribe("t/#"));

    const auto heardOn = [&](const std::string &topic) -> const mqtt::Message * {
        for (const mqtt::Message &m : heard)
            if (m.topic == topic) return &m;
        return nullptr;
    };

    {
        auto speaker = std::make_unique<mqtt::Client>(loop);
        mqtt::Settings s = settingsFor(port, "speaker");
        s.will = mqtt::Will{"t/will", "gone", 0, false};
        CHECK(speaker->connect(s));
        CHECK(waitFor(loop, [&]() { return speaker->connected(); }));
        CHECK(speaker->publish("t/hello", "world"));
        CHECK(speaker->publish("t/kept", "still here", 0, true));
        CHECK(waitFor(loop, [&]() { return heardOn("t/hello") != nullptr; }));
        CHECK(heardOn("t/hello") && heardOn("t/hello")->payload == "world" && !heardOn("t/hello")->retained);
        // Gone without a DISCONNECT: the broker speaks the will.
        speaker.reset();
    }
    CHECK(waitFor(loop, [&]() { return heardOn("t/will") != nullptr; }));
    CHECK(heardOn("t/will") && heardOn("t/will")->payload == "gone");

    {
        // A clean disconnect leaves no will behind.
        mqtt::Client polite(loop);
        mqtt::Settings s = settingsFor(port, "polite");
        s.will = mqtt::Will{"t/politewill", "gone", 0, false};
        bool told = false;
        polite.onDisconnected([&](const std::string &) { told = true; });
        CHECK(polite.connect(s));
        CHECK(waitFor(loop, [&]() { return polite.connected(); }));
        polite.disconnect();
        CHECK(!told); // quiet by contract
    }
    waitFor(loop, []() { return false; }, 300);
    CHECK(heardOn("t/politewill") == nullptr);

    // A late subscriber is handed the retained message, marked as such.
    mqtt::Client late(loop);
    std::vector<mqtt::Message> lateHeard;
    late.onMessage([&](const mqtt::Message &m) { lateHeard.push_back(m); });
    late.onConnected([&]() { late.subscribe("t/kept"); });
    CHECK(late.connect(settingsFor(port, "late")));
    CHECK(waitFor(loop, [&]() { return !lateHeard.empty(); }));
    CHECK(!lateHeard.empty() && lateHeard.front().payload == "still here" && lateHeard.front().retained);

    mqtt::Settings same = listener.settings();
    CHECK(same.sameConnection(listener.settings()));
    same.tls.enabled = true;
    CHECK(!same.sameConnection(listener.settings()));
}

void testBrokerGoneIsReported(Loop &loop, int port, pid_t broker)
{
    mqtt::Client client(loop);
    std::string reason;
    client.onDisconnected([&](const std::string &r) { reason = r.empty() ? "?" : r; });
    CHECK(client.connect(settingsFor(port, "survivor")));
    CHECK(waitFor(loop, [&]() { return client.connected(); }));
    ::kill(broker, SIGTERM);
    ::waitpid(broker, nullptr, 0);
    CHECK(waitFor(loop, [&]() { return !reason.empty(); }));
    CHECK(!client.connected());
}

} // namespace

int main()
{
    if (::access(kBroker, X_OK) != 0) {
        std::printf("mqtt_tests: no %s, skipped\n", kBroker);
        return 77;
    }
    const int port = freePort();
    const std::string portText = std::to_string(port);
    char *argv[] = {const_cast<char *>(kBroker), const_cast<char *>("-p"), const_cast<char *>(portText.c_str()), nullptr};
    pid_t broker = 0;
    if (posix_spawn(&broker, kBroker, nullptr, nullptr, argv, environ) != 0) {
        std::printf("mqtt_tests: cannot start %s\n", kBroker);
        return 1;
    }

    EpollLoop loop;
    // The broker needs a moment to listen; the first test probes a port
    // nobody listens on meanwhile.
    testUnreachableBrokerSaysWhy(loop, freePort());
    waitFor(loop, []() { return false; }, 300);
    testPublishSubscribeRetainAndWill(loop, port);
    testBrokerGoneIsReported(loop, port, broker);
    return phitest::report("mqtt_tests");
}

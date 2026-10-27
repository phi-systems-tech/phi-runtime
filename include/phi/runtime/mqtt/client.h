#pragma once

// An MQTT client driven by the caller's phi::runtime::Loop - the one copy of
// what phi-transport-voice, phi-adapter-voice, phi-adapter-z2m and
// phi-adapter-thinq each carried (2026-10-27). Its own library,
// libphi-runtime-mqtt, so libmosquitto comes only with the code that talks
// to a broker and never with the loop itself.
//
// One thread. The socket libmosquitto opens is watched by the loop, its
// keepalive runs off a timer, and a connect that goes nowhere is ended by a
// deadline of this client's choosing - libmosquitto's own network thread is
// never started.
//
// What the caller sees: a state, and three callbacks - connected, disconnected
// (with a reason a person can read), message. A refused password, a rejected
// certificate and an unreachable broker each say so.

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "phi/runtime/loop.h"

struct mosquitto;
struct mosquitto_message;

namespace phi::runtime::mqtt {

/// TLS towards the broker. The chain is always verified: an endpoint with a
/// self-signed certificate has a correct answer - name its CA in `caFile` -
/// and an option to accept any certificate would only be the easier way to
/// be insecure.
struct Tls {
    bool enabled = false;
    /// PEM file of the authority to trust; empty for the system's store.
    std::string caFile;
    /// Off for a broker dialled by IP, whose certificate cannot name the
    /// address. The chain is still checked.
    bool verifyHostname = true;

    bool operator==(const Tls &) const = default;
};

/// What the broker publishes for this client when it vanishes without a
/// DISCONNECT: a dead sidecar's "offline".
struct Will {
    std::string topic;
    std::string payload;
    int qos = 0;
    bool retain = true;

    bool operator==(const Will &) const = default;
};

struct Settings {
    std::string clientId;
    std::string host;
    int port = 1883;
    std::string username;
    std::string password;
    Tls tls;
    std::optional<Will> will;
    int keepAliveSeconds = 60;
    /// Resolve, connect, handshake and CONNACK, all within this.
    std::chrono::milliseconds connectTimeout{10000};

    /// Whether a connection made with `other` would be the same connection:
    /// everything but the deadline, which only matters while connecting.
    [[nodiscard]] bool sameConnection(const Settings &other) const
    {
        return clientId == other.clientId && host == other.host && port == other.port
            && username == other.username && password == other.password && tls == other.tls
            && will == other.will && keepAliveSeconds == other.keepAliveSeconds;
    }
};

struct Message {
    std::string topic;
    std::string payload;
    /// Replayed by the broker on subscribe rather than published just now.
    bool retained = false;
};

class Client
{
public:
    enum class State { Disconnected, Connecting, Connected };

    using ConnectedFn = std::function<void()>;
    using DisconnectedFn = std::function<void(const std::string &reason)>;
    using MessageFn = std::function<void(const Message &message)>;

    /// The client lives on `loop`'s thread: every call and every callback.
    explicit Client(Loop &loop);
    ~Client();
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    void onConnected(ConnectedFn fn) { m_onConnected = std::move(fn); }
    void onDisconnected(DisconnectedFn fn) { m_onDisconnected = std::move(fn); }
    void onMessage(MessageFn fn) { m_onMessage = std::move(fn); }

    /**
     * @brief Starts connecting. False, with `error`, when it cannot even start.
     *
     * A false return is final: neither callback follows. A true return is
     * followed by exactly one of connected() or disconnected(reason).
     *
     * The host is resolved synchronously - libmosquitto does that itself - so
     * a name that has to go out to a slow resolver holds the loop for as long
     * as the lookup takes. Brokers are addressed by IP or `localhost` in
     * practice, which is why this is noted rather than fixed.
     */
    bool connect(Settings settings, std::string *error = nullptr);

    /// Ends the connection quietly: no disconnected() callback, no will.
    void disconnect();

    [[nodiscard]] State state() const { return m_state; }
    [[nodiscard]] bool connected() const { return m_state == State::Connected; }

    /// What the client was last told to connect with.
    [[nodiscard]] const Settings &settings() const { return m_settings; }

    bool publish(const std::string &topic, std::string_view payload, int qos = 0, bool retain = false,
                 std::string *error = nullptr);
    bool subscribe(const std::string &topicFilter, int qos = 0, std::string *error = nullptr);

private:
    static void handleConnect(::mosquitto *, void *userdata, int rc);
    static void handleDisconnect(::mosquitto *, void *userdata, int rc);
    static void handleMessage(::mosquitto *, void *userdata, const ::mosquitto_message *message);
    static void handleLog(::mosquitto *, void *userdata, int level, const char *text);

    bool applyTls(std::string *error);
    void onReadable();
    void onWritable();
    void onTick();
    void syncWriteWatch();
    /// Ends whatever is in progress and, unless quiet, tells the owner why.
    void finish(std::string reason, bool quiet);
    void releaseHandle();

    Loop &m_loop;
    ::mosquitto *m_mosq = nullptr;
    Settings m_settings;
    State m_state = State::Disconnected;

    FdWatch m_readWatch;
    FdWatch m_writeWatch;
    Timer m_tick;
    Timer m_deadline;

    ConnectedFn m_onConnected;
    DisconnectedFn m_onDisconnected;
    MessageFn m_onMessage;

    /// The last error line libmosquitto logged. A rejected certificate is
    /// reported there and nowhere else.
    std::string m_lastLibraryError;
    /// Set when a CONNACK refused us, so the disconnect that follows carries
    /// the refusal rather than "connection closed".
    std::string m_pendingReason;
    int m_inCallback = 0;
};

} // namespace phi::runtime::mqtt

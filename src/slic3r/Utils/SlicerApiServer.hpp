#pragma once

#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>

// Opt-in, loopback-only WebSocket API through which local tools call the sw_Slicer* commands.
// This header has no wx or GUI dependency so it can be compiled into a test executable.
namespace Slic3r { namespace SlicerApi {

// Written to data_dir(): tells local clients the port and the bearer token.
constexpr const char* CONNECTION_FILE = "slicer_api.json";

enum class Verdict { Accept, NotUpgrade, OriginPresent, BadHost, BadToken };

struct HandshakeHeaders
{
    bool        is_upgrade{false};
    bool        has_origin{false}; // any Origin header, even "null" or empty
    std::string host;
    std::string authorization;
};

struct Envelope
{
    bool           ok{false};
    nlohmann::json header;   // set whenever the message carries one, also when !ok
    std::string    cmd;
    std::string    event_id;
    nlohmann::json params;   // empty object when absent
};

// Host must be exactly 127.0.0.1:<port> or localhost:<port> (case-insensitive), which rules out DNS rebinding.
bool host_matches(std::string_view host, unsigned short port);
// "Bearer <token>" with a case-insensitive scheme; the token is compared in constant time.
bool token_matches(std::string_view authorization, std::string_view token);
// Checks in this order: upgrade, Origin (any Origin means a browser), Host, token.
Verdict check_handshake(const HandshakeHeaders& h, unsigned short port, std::string_view token);

// Only sw_Slicer* commands from the project command list, and never with an event_id.
bool is_allowed_command(std::string_view cmd, std::string_view event_id,
                        const std::unordered_set<std::string>& project_cmds);

// Parses {"header": ..., "payload": {"cmd": ..., "params": ..., "event_id": ...}}.
Envelope    parse_envelope(const std::string& message);
// Same envelope as SSWCP_Instance::send_to_js: {header, payload: {code, message, data}}.
std::string make_error_reply(const nlohmann::json& header, int code, const std::string& msg);

// 32 random bytes from RAND_bytes as 64 lowercase hex characters. Throws std::runtime_error on failure.
std::string generate_token();

// Atomically writes the connection file, readable by the current user only.
bool write_connection_file(const boost::filesystem::path& file, unsigned short port,
                           const std::string& token, std::string& error);
// Removes the file unless another instance has replaced it with its own token. Never throws.
void remove_connection_file_if_ours(const boost::filesystem::path& file, const std::string& token);

// Sends a reply to the session that sent the request. Thread-safe; does nothing once the session
// is closed or the server is stopped.
using Reply = std::function<void(std::string)>;
// Called on the io thread for every text message. `alive` expires when the session closes or the
// server stops, so work queued elsewhere can tell that nobody is waiting for it any more.
using MessageHandler = std::function<void(std::string message, Reply reply, std::weak_ptr<void> alive)>;

struct Options
{
    unsigned short       port = 0; // 0 = let the OS choose
    std::string          token;
    std::size_t          max_message_bytes      = 8u << 20;
    std::size_t          max_sessions           = 4;
    std::size_t          max_pending_handshakes = 8;
    std::chrono::seconds handshake_timeout{5};
};

// WebSocket server on 127.0.0.1. All socket work runs asynchronously on a single io thread.
class Server
{
public:
    Server(Options options, MessageHandler handler);
    ~Server(); // calls stop()
    Server(const Server&)            = delete;
    Server& operator=(const Server&) = delete;

    // Binds 127.0.0.1 and starts the io thread. Can be called once.
    bool start(std::string& error);
    // Idempotent. Closes all sessions and joins the io thread. Called from the handler it only stops
    // the io loop; a later call from another thread, at the latest the destructor, finishes the job.
    // The server must not be destroyed from the handler.
    void stop();
    // The bound port, valid after a successful start().
    unsigned short port() const { return m_port; }

private:
    struct Core;
    class Session;
    std::shared_ptr<Core> m_core;
    std::thread           m_thread;
    std::mutex            m_stop_mutex;
    unsigned short        m_port{0};
    bool                  m_started{false};
};

}} // namespace Slic3r::SlicerApi

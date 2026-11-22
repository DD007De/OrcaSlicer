#include "SlicerApiServer.hpp"

// Asio and Beast first: they include winsock2.h, which must come before windows.h.
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/log/trivial.hpp>

#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs        = boost::filesystem;
namespace asio      = boost::asio;
namespace beast     = boost::beast;
namespace http      = beast::http;
namespace websocket = beast::websocket;
using tcp           = asio::ip::tcp;

namespace Slic3r { namespace SlicerApi {

namespace {

bool iequals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

#ifdef _WIN32
std::string win_error(const char* what, DWORD code) { return std::string(what) + " failed (error " + std::to_string(code) + ")"; }

// Creates the file with a protected DACL that grants access to the current user only, then writes it.
// A data directory outside the user profile may inherit broad permissions, so they are never inherited here.
bool write_private_file(const fs::path& path, const std::string& content, std::string& error)
{
    HANDLE process_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &process_token)) {
        error = win_error("OpenProcessToken", GetLastError());
        return false;
    }
    DWORD size = 0;
    GetTokenInformation(process_token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> user(size);
    const bool  got_user = size > 0 && GetTokenInformation(process_token, TokenUser, user.data(), size, &size);
    const DWORD user_err = GetLastError();
    CloseHandle(process_token);
    if (!got_user) {
        error = win_error("GetTokenInformation", user_err);
        return false;
    }

    EXPLICIT_ACCESS_W access{};
    access.grfAccessPermissions = FILE_ALL_ACCESS;
    access.grfAccessMode        = SET_ACCESS;
    access.grfInheritance       = NO_INHERITANCE;
    access.Trustee.TrusteeForm  = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType  = TRUSTEE_IS_USER;
    access.Trustee.ptstrName    = reinterpret_cast<LPWSTR>(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid);
    PACL        acl             = nullptr;
    const DWORD acl_err         = SetEntriesInAclW(1, &access, nullptr, &acl);
    if (acl_err != ERROR_SUCCESS) {
        error = win_error("SetEntriesInAcl", acl_err);
        return false;
    }

    // The file is created with this protected DACL, so it never has the inherited permissions.
    SECURITY_DESCRIPTOR sd;
    if (!InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) || !SetSecurityDescriptorDacl(&sd, TRUE, acl, FALSE) ||
        !SetSecurityDescriptorControl(&sd, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) {
        error = win_error("security descriptor", GetLastError());
        LocalFree(acl);
        return false;
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), &sd, FALSE};
    // No sharing while the file is written.
    HANDLE      file       = CreateFileW(path.wstring().c_str(), GENERIC_WRITE, 0, &sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    const DWORD create_err = GetLastError();
    LocalFree(acl);
    if (file == INVALID_HANDLE_VALUE) {
        error = win_error("CreateFile", create_err);
        return false;
    }
    bool  ok      = false;
    DWORD written = 0;
    if (!WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr))
        error = win_error("WriteFile", GetLastError());
    else if (written != content.size())
        error = "WriteFile wrote " + std::to_string(written) + " of " + std::to_string(content.size()) + " bytes";
    else if (!FlushFileBuffers(file))
        error = win_error("FlushFileBuffers", GetLastError());
    else
        ok = true;
    CloseHandle(file);
    return ok;
}

unsigned long current_pid() { return GetCurrentProcessId(); }
#else
bool write_private_file(const fs::path& path, const std::string& content, std::string& error)
{
    const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0) {
        error = std::string("open failed: ") + std::strerror(errno);
        return false;
    }
    bool ok = false;
    // The umask may have narrowed the mode further; make it exactly 0600.
    if (::fchmod(fd, 0600) != 0) {
        error = std::string("fchmod failed: ") + std::strerror(errno);
    } else {
        ok               = true;
        const char* data = content.data();
        size_t      left = content.size();
        while (ok && left > 0) {
            const ssize_t n = ::write(fd, data, left);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0) {
                error = n < 0 ? std::string("write failed: ") + std::strerror(errno) : std::string("write failed: no progress");
                ok    = false;
                break;
            }
            data += n;
            left -= static_cast<size_t>(n);
        }
        if (ok && ::fsync(fd) != 0) {
            error = std::string("fsync failed: ") + std::strerror(errno);
            ok    = false;
        }
    }
    ::close(fd);
    return ok;
}

unsigned long current_pid() { return static_cast<unsigned long>(::getpid()); }
#endif

} // namespace

bool host_matches(std::string_view host, unsigned short port)
{
    const size_t colon = host.rfind(':');
    if (colon == std::string_view::npos || host.substr(colon + 1) != std::to_string(port))
        return false;
    const std::string_view name = host.substr(0, colon);
    return name == "127.0.0.1" || iequals(name, "localhost");
}

bool token_matches(std::string_view authorization, std::string_view token)
{
    constexpr std::string_view scheme = "Bearer ";
    if (token.empty() || authorization.size() != scheme.size() + token.size() || !iequals(authorization.substr(0, scheme.size()), scheme))
        return false;
    return CRYPTO_memcmp(authorization.data() + scheme.size(), token.data(), token.size()) == 0;
}

Verdict check_handshake(const HandshakeHeaders& h, unsigned short port, std::string_view token)
{
    if (!h.is_upgrade)
        return Verdict::NotUpgrade;
    if (h.has_origin)
        return Verdict::OriginPresent;
    if (!host_matches(h.host, port))
        return Verdict::BadHost;
    if (!token_matches(h.authorization, token))
        return Verdict::BadToken;
    return Verdict::Accept;
}

bool is_allowed_command(std::string_view cmd, std::string_view event_id, const std::unordered_set<std::string>& project_cmds)
{
    constexpr std::string_view prefix = "sw_Slicer";
    return event_id.empty() && cmd.substr(0, prefix.size()) == prefix && project_cmds.count(std::string(cmd)) > 0;
}

Envelope parse_envelope(const std::string& message)
{
    Envelope             e;
    const nlohmann::json j = nlohmann::json::parse(message, nullptr, false);
    if (!j.is_object())
        return e;
    const auto header = j.find("header");
    if (header == j.end())
        return e;
    e.header = *header;

    const auto payload = j.find("payload");
    if (payload == j.end() || !payload->is_object())
        return e;
    const auto cmd = payload->find("cmd");
    if (cmd == payload->end() || !cmd->is_string())
        return e;
    const auto event_id = payload->find("event_id");
    if (event_id != payload->end() && !event_id->is_null()) {
        if (!event_id->is_string())
            return e;
        e.event_id = event_id->get<std::string>();
    }
    const auto params = payload->find("params");
    if (params == payload->end() || params->is_null())
        e.params = nlohmann::json::object();
    else if (params->is_object())
        e.params = *params;
    else
        return e;
    e.cmd = cmd->get<std::string>();
    e.ok              = true;
    return e;
}

std::string make_error_reply(const nlohmann::json& header, int code, const std::string& msg)
{
    nlohmann::json reply;
    reply["header"]  = header;
    reply["payload"] = {{"code", code}, {"message", msg}, {"data", nullptr}};
    return reply.dump(-1, ' ', true, nlohmann::json::error_handler_t::replace);
}

std::string generate_token()
{
    unsigned char bytes[32];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1)
        throw std::runtime_error("RAND_bytes failed");
    static const char hex[] = "0123456789abcdef";
    std::string       token;
    token.reserve(2 * sizeof(bytes));
    for (unsigned char b : bytes) {
        token += hex[b >> 4];
        token += hex[b & 0x0f];
    }
    OPENSSL_cleanse(bytes, sizeof(bytes));
    return token;
}

bool write_connection_file(const fs::path& file, unsigned short port, const std::string& token, std::string& error)
{
    const nlohmann::json content = {{"version", 1},
                                    {"url", "ws://127.0.0.1:" + std::to_string(port)},
                                    {"port", port},
                                    {"token", token},
                                    {"pid", current_pid()}};
    fs::path tmp = file;
    tmp += ".tmp";
    boost::system::error_code ec;
    // A leftover from a crash: the exclusive create below would refuse it.
    fs::remove(tmp, ec);
    if (!write_private_file(tmp, content.dump(), error)) {
        error = "cannot write connection file: " + error;
        fs::remove(tmp, ec);
        return false;
    }
    // Renaming within the directory keeps the permissions of the temporary file.
    fs::rename(tmp, file, ec);
    if (ec) {
        error = "cannot rename connection file: " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

// Reading and removing are two steps: an instance that rewrites the file in between loses it.
// The window is short and needs two instances on one data directory.
void remove_connection_file_if_ours(const fs::path& file, const std::string& token)
{
    try {
        nlohmann::json j;
        {
            fs::ifstream in(file, std::ios::binary);
            if (!in)
                return;
            j = nlohmann::json::parse(in, nullptr, false);
        }
        if (!j.is_object() || token.empty())
            return;
        const auto stored = j.find("token");
        if (stored != j.end() && stored->is_string() && stored->get<std::string>() == token) {
            boost::system::error_code ec;
            fs::remove(file, ec);
        }
    } catch (...) {
        // Cleanup on shutdown must never throw.
    }
}

// --- Server ---

namespace {
constexpr std::uint32_t request_header_limit = 8 * 1024;
constexpr std::size_t   max_queued_replies   = 64;
constexpr std::size_t   max_queued_bytes     = 64u << 20;
constexpr auto          accept_retry_delay   = std::chrono::milliseconds(100);
constexpr const char*   server_name          = "SlicerApi";

std::string header_value(const http::request<http::empty_body>& req, http::field field)
{
    // A repeated header is treated as missing.
    if (req.count(field) != 1)
        return {};
    const auto value = req[field];
    return std::string(value.data(), value.size());
}
} // namespace

struct Server::Core : std::enable_shared_from_this<Server::Core>
{
    Core(Options options, MessageHandler handler) : options(std::move(options)), handler(std::move(handler)) {}

    ~Core() { reset_io(); }

    const Options        options;
    const MessageHandler handler;
    unsigned short       port{0};

    // Guards `stopped`; a reply is only posted to `ioc` while it is false.
    std::mutex                    mutex;
    bool                          stopped{false};
    std::atomic<std::thread::id> io_thread{};

    // Used on the io thread only, or by stop() once that thread has ended. Declared before `ioc`,
    // because destroying the io_context destroys the sessions, which update the counts.
    std::size_t                         pending{0};   // connections still in the handshake
    std::size_t                         upgrading{0}; // authenticated, WebSocket accept running
    std::size_t                         active{0};    // open sessions
    bool                                accept_armed{false};
    std::vector<std::weak_ptr<Session>> sessions;

    std::unique_ptr<asio::io_context>   ioc;
    std::unique_ptr<tcp::acceptor>      acceptor;
    std::unique_ptr<asio::steady_timer> retry_timer;

    void do_accept();
    void on_connection(tcp::socket socket);
    // After an exception escaped a handler: make sure the server keeps accepting.
    void resume_accept();
    // Destroying the io_context also destroys the pending handlers and with them the sessions.
    void reset_io()
    {
        retry_timer.reset();
        acceptor.reset();
        ioc.reset();
    }
};

class Server::Session : public std::enable_shared_from_this<Server::Session>
{
public:
    Session(Core& core, tcp::socket socket) : m_core(core), m_ws(std::move(socket)) {}
    // Normally close() has run already; this keeps the counts right when a handler failed with an
    // exception and the session went away without it.
    ~Session()
    {
        try {
            set_state(State::Closed);
        } catch (...) {}
    }
    Session(const Session&)            = delete;
    Session& operator=(const Session&) = delete;

    void start();
    void send(std::string message);
    void close();

private:
    enum class State { New, Handshake, Rejecting, Upgrading, Open, Closed };

    void  set_state(State next);
    void  on_request(const beast::error_code& ec);
    void  reject(http::status status, const char* reason);
    void  on_accept(const beast::error_code& ec);
    void  do_read();
    void  on_read(const beast::error_code& ec);
    void  do_write();
    void  on_write(const beast::error_code& ec);
    Reply make_reply();

    Core&                                                 m_core;
    websocket::stream<beast::tcp_stream>                  m_ws;
    State                                                 m_state{State::New};
    beast::flat_buffer                                    m_buffer;
    std::optional<http::request_parser<http::empty_body>> m_parser;
    std::optional<http::response<http::empty_body>>       m_response;
    // Pending writes reference the front string, so the queue is never cleared while a write may run.
    std::deque<std::string> m_queue;
    std::size_t             m_queued_bytes{0};
    std::shared_ptr<void>   m_alive{std::make_shared<char>()};
};

void Server::Session::set_state(State next)
{
    const bool was_open = m_state == State::Open;
    switch (m_state) {
    case State::Handshake: --m_core.pending; break;
    case State::Upgrading: --m_core.upgrading; break;
    case State::Open: --m_core.active; break;
    default: break;
    }
    m_state = next;
    switch (next) {
    case State::Handshake: ++m_core.pending; break;
    case State::Upgrading: ++m_core.upgrading; break;
    case State::Open: ++m_core.active; break;
    default: break;
    }

    if (next == State::Open)
        BOOST_LOG_TRIVIAL(info) << "slicer API: session opened (" << m_core.active << " active)";
    else if (was_open)
        BOOST_LOG_TRIVIAL(info) << "slicer API: session closed (" << m_core.active << " active)";
}

void Server::Session::start()
{
    set_state(State::Handshake);
    // Also closes a connection that never sends a request.
    beast::get_lowest_layer(m_ws).expires_after(m_core.options.handshake_timeout);
    m_parser.emplace();
    m_parser->header_limit(request_header_limit);
    http::async_read(m_ws.next_layer(), m_buffer, *m_parser,
                     [self = shared_from_this()](const beast::error_code& ec, std::size_t) { self->on_request(ec); });
}

void Server::Session::on_request(const beast::error_code& ec)
{
    if (m_state != State::Handshake)
        return;
    if (ec) {
        if (ec == beast::error::timeout)
            BOOST_LOG_TRIVIAL(warning) << "slicer API: handshake rejected (timeout)";
        close();
        return;
    }

    const auto&      req = m_parser->get();
    HandshakeHeaders headers;
    headers.is_upgrade    = websocket::is_upgrade(req);
    headers.has_origin    = req.find(http::field::origin) != req.end();
    headers.host          = header_value(req, http::field::host);
    headers.authorization = header_value(req, http::field::authorization);
    const Verdict verdict = check_handshake(headers, m_core.port, m_core.options.token);
    headers.authorization.clear();
    switch (verdict) {
    case Verdict::NotUpgrade: reject(http::status::bad_request, "not an upgrade"); return;
    case Verdict::OriginPresent: reject(http::status::forbidden, "origin header"); return;
    case Verdict::BadHost: reject(http::status::forbidden, "host"); return;
    case Verdict::BadToken: reject(http::status::unauthorized, "token"); return;
    case Verdict::Accept: break;
    }
    if (m_core.upgrading + m_core.active >= m_core.options.max_sessions) {
        reject(http::status::service_unavailable, "too many sessions");
        return;
    }

    set_state(State::Upgrading);
    // From here the WebSocket timeouts apply instead of the stream deadline.
    beast::get_lowest_layer(m_ws).expires_never();
    auto timeouts              = websocket::stream_base::timeout::suggested(beast::role_type::server);
    timeouts.handshake_timeout = m_core.options.handshake_timeout;
    m_ws.set_option(timeouts);
    m_ws.set_option(websocket::stream_base::decorator([](websocket::response_type& res) { res.set(http::field::server, server_name); }));
    m_ws.read_message_max(m_core.options.max_message_bytes);
    m_ws.async_accept(req, [self = shared_from_this()](const beast::error_code& ec) { self->on_accept(ec); });
}

void Server::Session::reject(http::status status, const char* reason)
{
    BOOST_LOG_TRIVIAL(warning) << "slicer API: handshake rejected (" << reason << ")";
    set_state(State::Rejecting);
    m_response.emplace(status, m_parser->get().version());
    m_response->set(http::field::server, server_name);
    m_response->keep_alive(false);
    m_response->set(http::field::connection, "close");
    m_response->prepare_payload();
    // Still bounded by the handshake deadline.
    http::async_write(m_ws.next_layer(), *m_response,
                      [self = shared_from_this()](const beast::error_code&, std::size_t) { self->close(); });
}

void Server::Session::on_accept(const beast::error_code& ec)
{
    if (m_state != State::Upgrading)
        return;
    if (ec) {
        close();
        return;
    }
    m_parser.reset();
    m_buffer.consume(m_buffer.size());
    m_ws.text(true);
    set_state(State::Open);
    do_read();
}

void Server::Session::do_read()
{
    m_ws.async_read(m_buffer, [self = shared_from_this()](const beast::error_code& ec, std::size_t) { self->on_read(ec); });
}

void Server::Session::on_read(const beast::error_code& ec)
{
    if (m_state != State::Open)
        return;
    // Closed by the client, over read_message_max, idle timeout, or a binary frame.
    if (ec || !m_ws.got_text()) {
        close();
        return;
    }
    std::string message = beast::buffers_to_string(m_buffer.data());
    m_buffer.consume(m_buffer.size());
    try {
        m_core.handler(std::move(message), make_reply(), m_alive);
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "slicer API: message handler failed";
    }
    if (m_state == State::Open)
        do_read();
}

Reply Server::Session::make_reply()
{
    return [core = m_core.weak_from_this(), session = weak_from_this(), alive = std::weak_ptr<void>(m_alive)](std::string message) {
        if (alive.expired())
            return;
        const std::shared_ptr<Core> c = core.lock();
        if (!c)
            return;
        std::lock_guard<std::mutex> lock(c->mutex);
        if (c->stopped)
            return;
        asio::post(*c->ioc, [session, message = std::move(message)]() mutable {
            if (const auto s = session.lock())
                s->send(std::move(message));
        });
    };
}

void Server::Session::send(std::string message)
{
    if (m_state != State::Open)
        return;
    if (m_queue.size() >= max_queued_replies || m_queued_bytes + message.size() > max_queued_bytes) {
        BOOST_LOG_TRIVIAL(warning) << "slicer API: reply queue full, closing session";
        close();
        return;
    }
    m_queued_bytes += message.size();
    m_queue.push_back(std::move(message));
    if (m_queue.size() == 1)
        do_write();
}

void Server::Session::do_write()
{
    m_ws.async_write(asio::buffer(m_queue.front()),
                     [self = shared_from_this()](const beast::error_code& ec, std::size_t) { self->on_write(ec); });
}

void Server::Session::on_write(const beast::error_code& ec)
{
    if (m_state != State::Open)
        return;
    if (ec) {
        close();
        return;
    }
    m_queued_bytes -= m_queue.front().size();
    m_queue.pop_front();
    if (!m_queue.empty())
        do_write();
}

void Server::Session::close()
{
    if (m_state == State::Closed)
        return;
    set_state(State::Closed);
    m_alive.reset();
    beast::error_code ignored;
    auto&             stream = beast::get_lowest_layer(m_ws);
    stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
    stream.close();
}

void Server::Core::do_accept()
{
    acceptor->async_accept([this](const beast::error_code& ec, tcp::socket socket) {
        accept_armed = false;
        if (ec == asio::error::operation_aborted)
            return;
        if (ec) {
            // For example out of handles: try again a little later instead of spinning.
            BOOST_LOG_TRIVIAL(warning) << "slicer API: accept failed: " << ec.message();
            retry_timer->expires_after(accept_retry_delay);
            retry_timer->async_wait([this](const beast::error_code& wait_ec) {
                accept_armed = false;
                if (!wait_ec)
                    do_accept();
            });
            accept_armed = true;
            return;
        }
        try {
            on_connection(std::move(socket));
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "slicer API: accept failed";
        }
        // Should this throw, the io thread calls resume_accept().
        do_accept();
    });
    accept_armed = true;
}

void Server::Core::on_connection(tcp::socket socket)
{
    if (pending >= options.max_pending_handshakes) {
        BOOST_LOG_TRIVIAL(warning) << "slicer API: handshake rejected (too many pending handshakes)";
        beast::error_code ignored;
        socket.close(ignored);
        return;
    }
    sessions.erase(std::remove_if(sessions.begin(), sessions.end(), [](const std::weak_ptr<Session>& s) { return s.expired(); }),
                   sessions.end());
    auto session = std::make_shared<Session>(*this, std::move(socket));
    sessions.push_back(session);
    session->start();
}

void Server::Core::resume_accept()
{
    try {
        if (!accept_armed && acceptor && acceptor->is_open())
            do_accept();
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "slicer API: accept failed";
    }
}

Server::Server(Options options, MessageHandler handler) : m_core(std::make_shared<Core>(std::move(options), std::move(handler))) {}

Server::~Server() { stop(); }

bool Server::start(std::string& error)
{
    Core& core = *m_core;
    {
        std::lock_guard<std::mutex> lock(core.mutex);
        if (m_started || core.stopped) {
            error = "the server can only be started once";
            return false;
        }
    }
    m_started = true;
    if (core.options.token.empty()) {
        error = "no token";
        return false;
    }

    core.ioc      = std::make_unique<asio::io_context>(1);
    core.acceptor = std::make_unique<tcp::acceptor>(*core.ioc);
    const tcp::endpoint endpoint(asio::ip::address_v4::loopback(), core.options.port);
    beast::error_code   ec;
    core.acceptor->open(endpoint.protocol(), ec);
#ifdef _WIN32
    // Otherwise another process could bind the same port with SO_REUSEADDR and take connections.
    const BOOL exclusive = TRUE;
    if (!ec && ::setsockopt(core.acceptor->native_handle(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
                            sizeof(exclusive)) != 0)
        ec.assign(::WSAGetLastError(), boost::system::system_category());
#endif
    if (!ec)
        core.acceptor->bind(endpoint, ec);
    if (!ec)
        core.acceptor->listen(asio::socket_base::max_listen_connections, ec);
    if (!ec)
        core.port = core.acceptor->local_endpoint(ec).port();
    if (ec) {
        error = "cannot listen on 127.0.0.1:" + std::to_string(core.options.port) + ": " + ec.message();
        BOOST_LOG_TRIVIAL(error) << "slicer API: " << error;
        core.reset_io();
        return false;
    }

    core.retry_timer = std::make_unique<asio::steady_timer>(*core.ioc);
    core.do_accept();
    try {
        m_thread = std::thread([c = m_core] {
            c->io_thread = std::this_thread::get_id();
            // No exception may leave this thread: std::terminate would end the application.
            for (;;) {
                try {
                    c->ioc->run();
                    break;
                } catch (const std::exception& e) {
                    BOOST_LOG_TRIVIAL(error) << "slicer API: io error: " << e.what();
                } catch (...) {
                    BOOST_LOG_TRIVIAL(error) << "slicer API: io error";
                }
                c->resume_accept();
            }
        });
    } catch (const std::exception& e) {
        error = std::string("cannot start the io thread: ") + e.what();
        BOOST_LOG_TRIVIAL(error) << "slicer API: " << error;
        core.reset_io();
        return false;
    }
    m_port = core.port;
    BOOST_LOG_TRIVIAL(info) << "slicer API listening on 127.0.0.1:" << m_port;
    return true;
}

void Server::stop()
{
    {
        std::lock_guard<std::mutex> lock(m_core->mutex);
        m_core->stopped = true;
    }
    if (std::this_thread::get_id() == m_core->io_thread.load()) {
        // Called from the handler: the io thread cannot join itself. Only stop the io loop; the
        // next stop() from another thread, at the latest the destructor, joins and cleans up.
        m_core->ioc->stop();
        return;
    }
    std::lock_guard<std::mutex> lock(m_stop_mutex);
    if (!m_core->ioc) // never started, or already stopped
        return;
    m_core->ioc->stop();
    if (m_thread.joinable())
        m_thread.join();
    // A later thread may reuse the id; it must not take the "called from the handler" path.
    m_core->io_thread = std::thread::id{};
    // The io thread has ended, so the io objects may be used from this thread now.
    for (const auto& weak : m_core->sessions)
        if (const auto session = weak.lock())
            session->close();
    m_core->sessions.clear();
    m_core->reset_io();
    BOOST_LOG_TRIVIAL(info) << "slicer API stopped";
}

}} // namespace Slic3r::SlicerApi

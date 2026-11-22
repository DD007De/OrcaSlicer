#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include "slic3r/Utils/SlicerApiServer.hpp"

// Asio and Beast first: they include winsock2.h, which must come before windows.h.
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/trivial.hpp>

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#else
#include <sys/stat.h>
#endif

using namespace Slic3r::SlicerApi;
namespace fs = boost::filesystem;
using nlohmann::json;
using namespace std::chrono_literals;

namespace {

// The server's expected warnings would otherwise end up between the test results. Set when the
// run starts, after the static initialisers that set the default test log level.
struct QuietServerLog : Catch::EventListenerBase
{
    using Catch::EventListenerBase::EventListenerBase;
    void testRunStarting(Catch::TestRunInfo const&) override
    {
        boost::log::core::get()->set_filter(boost::log::trivial::severity >= boost::log::trivial::fatal);
    }
};
CATCH_REGISTER_LISTENER(QuietServerLog)

const unsigned short kPort  = 53817;
const std::string    kToken = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

HandshakeHeaders good_headers()
{
    return {true, false, "127.0.0.1:53817", "Bearer " + kToken};
}

struct TempDir
{
    fs::path path;
    TempDir() : path(fs::temp_directory_path() / fs::unique_path("slicer_api_test_%%%%-%%%%-%%%%")) { fs::create_directories(path); }
    ~TempDir()
    {
        boost::system::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string read_file(const fs::path& file)
{
    fs::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_file(const fs::path& file, const std::string& text)
{
    fs::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
}

// The file must be readable by the current user only.
void check_private_permissions(const fs::path& file)
{
#ifdef _WIN32
    HANDLE process_token = nullptr;
    REQUIRE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &process_token));
    DWORD size = 0;
    GetTokenInformation(process_token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> buf(size);
    REQUIRE(GetTokenInformation(process_token, TokenUser, buf.data(), size, &size));
    CloseHandle(process_token);
    PSID user_sid = reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid;

    PACL                 dacl = nullptr;
    PSECURITY_DESCRIPTOR sd   = nullptr;
    REQUIRE(GetNamedSecurityInfoW(file.wstring().c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr,
                                  &sd) == ERROR_SUCCESS);
    SECURITY_DESCRIPTOR_CONTROL control  = 0;
    DWORD                       revision = 0;
    REQUIRE(GetSecurityDescriptorControl(sd, &control, &revision));
    CHECK((control & SE_DACL_PROTECTED) != 0);
    REQUIRE(dacl != nullptr);
    CHECK(dacl->AceCount == 1);
    void* ace = nullptr;
    REQUIRE(GetAce(dacl, 0, &ace));
    auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
    CHECK(allowed->Header.AceType == ACCESS_ALLOWED_ACE_TYPE);
    CHECK(allowed->Mask == FILE_ALL_ACCESS);
    CHECK(EqualSid(reinterpret_cast<PSID>(&allowed->SidStart), user_sid));
    LocalFree(sd);
#else
    struct stat st{};
    REQUIRE(::stat(file.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);
#endif
}

} // namespace

TEST_CASE("host_matches accepts only loopback names with the bound port", "[slicer_api]")
{
    CHECK(host_matches("127.0.0.1:53817", kPort));
    CHECK(host_matches("localhost:53817", kPort));
    CHECK(host_matches("LOCALHOST:53817", kPort));

    CHECK_FALSE(host_matches("127.0.0.1", kPort));
    CHECK_FALSE(host_matches("localhost", kPort));
    CHECK_FALSE(host_matches("127.0.0.1:53818", kPort));
    CHECK_FALSE(host_matches("127.0.0.1:053817", kPort));
    CHECK_FALSE(host_matches("evil.example:53817", kPort));
    CHECK_FALSE(host_matches("127.0.0.1.evil.example:53817", kPort));
    CHECK_FALSE(host_matches("[::1]:53817", kPort));
    CHECK_FALSE(host_matches("", kPort));
}

TEST_CASE("token_matches requires a bearer token of the same value", "[slicer_api]")
{
    CHECK(token_matches("Bearer " + kToken, kToken));
    CHECK(token_matches("bearer " + kToken, kToken));
    CHECK(token_matches("BEARER " + kToken, kToken));

    CHECK_FALSE(token_matches("", kToken));
    CHECK_FALSE(token_matches("Bearer", kToken));
    CHECK_FALSE(token_matches("Bearer ", kToken));
    CHECK_FALSE(token_matches("Basic " + kToken, kToken));
    CHECK_FALSE(token_matches("Bearer " + kToken + "2", kToken));
    CHECK_FALSE(token_matches("Bearer " + kToken.substr(1), kToken));
    CHECK_FALSE(token_matches("Bearer  " + kToken, kToken));
    CHECK_FALSE(token_matches(kToken, kToken));
    std::string other = kToken;
    other.back()      = 'x';
    CHECK_FALSE(token_matches("Bearer " + other, kToken));
    // An empty server token never matches.
    CHECK_FALSE(token_matches("Bearer ", ""));
}

TEST_CASE("check_handshake checks upgrade, Origin, Host and token in that order", "[slicer_api]")
{
    CHECK(check_handshake(good_headers(), kPort, kToken) == Verdict::Accept);

    auto h       = good_headers();
    h.is_upgrade = false;
    h.has_origin = true;
    CHECK(check_handshake(h, kPort, kToken) == Verdict::NotUpgrade);

    // Origin wins over a valid token (a browser page must never get in).
    h            = good_headers();
    h.has_origin = true;
    CHECK(check_handshake(h, kPort, kToken) == Verdict::OriginPresent);
    h.host = "evil.example:53817";
    CHECK(check_handshake(h, kPort, kToken) == Verdict::OriginPresent);

    h      = good_headers();
    h.host = "evil.example:53817";
    CHECK(check_handshake(h, kPort, kToken) == Verdict::BadHost);
    h.authorization.clear();
    CHECK(check_handshake(h, kPort, kToken) == Verdict::BadHost);

    h = good_headers();
    h.authorization.clear();
    CHECK(check_handshake(h, kPort, kToken) == Verdict::BadToken);
    h.authorization = "Bearer wrong";
    CHECK(check_handshake(h, kPort, kToken) == Verdict::BadToken);

    // The port must be the one actually bound.
    CHECK(check_handshake(good_headers(), kPort + 1, kToken) == Verdict::BadHost);
}

TEST_CASE("is_allowed_command allows only listed sw_Slicer commands without event_id", "[slicer_api]")
{
    const std::unordered_set<std::string> project_cmds{"sw_SlicerPing", "sw_SlicerGetStatus", "sw_NewProject"};

    CHECK(is_allowed_command("sw_SlicerPing", "", project_cmds));
    CHECK(is_allowed_command("sw_SlicerGetStatus", "", project_cmds));

    CHECK_FALSE(is_allowed_command("sw_NewProject", "", project_cmds));
    CHECK_FALSE(is_allowed_command("sw_SlicerFoo", "", project_cmds));
    CHECK_FALSE(is_allowed_command("sw_MachinePrintStart", "", project_cmds));
    CHECK_FALSE(is_allowed_command("sw_SlicerPing", "event-1", project_cmds));
    CHECK_FALSE(is_allowed_command("", "", project_cmds));
}

TEST_CASE("parse_envelope reads header, cmd, event_id and params", "[slicer_api]")
{
    SECTION("valid")
    {
        auto e = parse_envelope(R"({"header":{"id":"a1"},"payload":{"cmd":"sw_SlicerPing","params":{"x":1}}})");
        CHECK(e.ok);
        CHECK(e.header == json({{"id", "a1"}}));
        CHECK(e.cmd == "sw_SlicerPing");
        CHECK(e.event_id.empty());
        CHECK(e.params == json({{"x", 1}}));
    }
    SECTION("event_id is passed on, null means none")
    {
        auto e = parse_envelope(R"({"header":{},"payload":{"cmd":"sw_SlicerPing","event_id":"e1"}})");
        CHECK(e.ok);
        CHECK(e.event_id == "e1");
        e = parse_envelope(R"({"header":{},"payload":{"cmd":"sw_SlicerPing","event_id":null}})");
        CHECK(e.ok);
        CHECK(e.event_id.empty());
    }
    SECTION("missing params become an empty object")
    {
        auto e = parse_envelope(R"({"header":{"id":"a4"},"payload":{"cmd":"sw_SlicerPing"}})");
        CHECK(e.ok);
        CHECK(e.params.is_object());
        CHECK(e.params.empty());
        e = parse_envelope(R"({"header":{"id":"a4"},"payload":{"cmd":"sw_SlicerPing","params":null}})");
        CHECK(e.ok);
        CHECK(e.params.is_object());
    }
    SECTION("params that are not an object keep the header for the error reply")
    {
        auto e = parse_envelope(R"({"header":{"id":"a6"},"payload":{"cmd":"sw_SlicerPing","params":[1]}})");
        CHECK_FALSE(e.ok);
        CHECK(e.header == json({{"id", "a6"}}));
        CHECK_FALSE(parse_envelope(R"({"header":{},"payload":{"cmd":"sw_SlicerPing","params":"x"}})").ok);
        CHECK_FALSE(parse_envelope(R"({"header":{},"payload":{"cmd":"sw_SlicerPing","params":3}})").ok);
    }
    SECTION("broken JSON")
    {
        auto e = parse_envelope(R"({"header":{"id":"a5"},)");
        CHECK_FALSE(e.ok);
        CHECK(e.header.is_null());
    }
    SECTION("not an object")
    {
        CHECK_FALSE(parse_envelope("[1,2]").ok);
        CHECK_FALSE(parse_envelope("\"text\"").ok);
    }
    SECTION("no payload keeps the header for the error reply")
    {
        auto e = parse_envelope(R"({"header":{"id":"a2"}})");
        CHECK_FALSE(e.ok);
        CHECK(e.header == json({{"id", "a2"}}));
    }
    SECTION("cmd is not a string")
    {
        auto e = parse_envelope(R"({"header":{"id":"a3"},"payload":{"cmd":5}})");
        CHECK_FALSE(e.ok);
        CHECK(e.header == json({{"id", "a3"}}));
        CHECK_FALSE(parse_envelope(R"({"header":{},"payload":{}})").ok);
    }
    SECTION("event_id is not a string")
    {
        CHECK_FALSE(parse_envelope(R"({"header":{},"payload":{"cmd":"sw_SlicerPing","event_id":7}})").ok);
    }
    SECTION("no header")
    {
        CHECK_FALSE(parse_envelope(R"({"payload":{"cmd":"sw_SlicerPing"}})").ok);
    }
}

TEST_CASE("make_error_reply uses the SSWCP envelope", "[slicer_api]")
{
    const json header{{"id", "r1"}};
    const json reply = json::parse(make_error_reply(header, -1, "command not available through the slicer API"));
    CHECK(reply["header"] == header);
    CHECK(reply["payload"]["code"] == -1);
    CHECK(reply["payload"]["message"] == "command not available through the slicer API");
    CHECK(reply["payload"]["data"].is_null());

    // Invalid UTF-8 must not throw.
    CHECK_NOTHROW(json::parse(make_error_reply(nullptr, -1, "bad \xff byte")));
}

TEST_CASE("generate_token returns 64 random lowercase hex characters", "[slicer_api]")
{
    const std::string a = generate_token();
    const std::string b = generate_token();
    REQUIRE(a.size() == 64);
    CHECK(a.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK(b.size() == 64);
    CHECK(a != b);
}

TEST_CASE("write_connection_file writes port and token for the current user only", "[slicer_api]")
{
    TempDir        dir;
    const fs::path file = dir.path / CONNECTION_FILE;
    std::string    error;

    REQUIRE(write_connection_file(file, kPort, kToken, error));
    CHECK(error.empty());
    CHECK_FALSE(fs::exists(file.string() + ".tmp"));

    const std::string text = read_file(file);
    CHECK(text.find('\n') == std::string::npos);
    const json j = json::parse(text);
    CHECK(j["version"] == 1);
    CHECK(j["url"] == "ws://127.0.0.1:53817");
    CHECK(j["port"] == kPort);
    CHECK(j["token"] == kToken);
    CHECK(j["pid"].is_number_integer());
    CHECK(j["pid"].get<long long>() > 0);

    SECTION("overwrite replaces the content and keeps the permissions")
    {
        const std::string other = generate_token();
        REQUIRE(write_connection_file(file, 40000, other, error));
        const json k = json::parse(read_file(file));
        CHECK(k["port"] == 40000);
        CHECK(k["token"] == other);
        check_private_permissions(file);
    }

    SECTION("permissions") { check_private_permissions(file); }
}

TEST_CASE("write_connection_file reports a missing directory", "[slicer_api]")
{
    TempDir     dir;
    std::string error;
    CHECK_FALSE(write_connection_file(dir.path / "missing" / CONNECTION_FILE, kPort, kToken, error));
    CHECK_FALSE(error.empty());
    CHECK(error.find(kToken) == std::string::npos);
}

TEST_CASE("remove_connection_file_if_ours removes only a file with our token", "[slicer_api]")
{
    TempDir        dir;
    const fs::path file = dir.path / CONNECTION_FILE;
    std::string    error;

    REQUIRE(write_connection_file(file, kPort, kToken, error));
    remove_connection_file_if_ours(file, generate_token());
    CHECK(fs::exists(file));

    remove_connection_file_if_ours(file, kToken);
    CHECK_FALSE(fs::exists(file));

    // Missing or broken files are ignored.
    CHECK_NOTHROW(remove_connection_file_if_ours(file, kToken));
    {
        fs::ofstream out(file);
        out << "not json";
    }
    CHECK_NOTHROW(remove_connection_file_if_ours(file, kToken));
    CHECK(fs::exists(file));
}

TEST_CASE("remove_connection_file_if_ours ignores an empty token and a file without a token", "[slicer_api]")
{
    TempDir        dir;
    const fs::path file = dir.path / CONNECTION_FILE;
    std::string    error;

    REQUIRE(write_connection_file(file, kPort, kToken, error));
    remove_connection_file_if_ours(file, "");
    CHECK(fs::exists(file));

    write_file(file, R"({"version":1,"port":53817})");
    remove_connection_file_if_ours(file, kToken);
    CHECK(fs::exists(file));

    write_file(file, R"({"version":1,"token":""})");
    remove_connection_file_if_ours(file, "");
    CHECK(fs::exists(file));
}

// ---------------------------------------------------------------------------------------------
// Server: real sockets on 127.0.0.1, port 0.

namespace {

namespace asio      = boost::asio;
namespace beast     = boost::beast;
namespace http      = beast::http;
namespace websocket = beast::websocket;
using tcp           = asio::ip::tcp;

Options test_options()
{
    Options options;
    options.token             = generate_token();
    options.max_message_bytes = 1024;
    options.handshake_timeout = std::chrono::seconds(1);
    return options;
}

void echo(std::string message, Reply reply, std::weak_ptr<void>) { reply(std::move(message)); }

template<class Pred> bool wait_until(Pred pred, std::chrono::milliseconds timeout = 3000ms)
{
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end)
            return false;
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

tcp::endpoint server_endpoint(unsigned short port) { return {asio::ip::address_v4::loopback(), port}; }

// A WebSocket client in which every operation has a 5 s deadline, so a broken server fails a test
// instead of hanging it.
class TestClient
{
public:
    http::status status{};

    beast::error_code open(unsigned short port, const std::string& host, const std::string& authorization, bool origin = false)
    {
        if (const auto ec = run([&](auto done) { beast::get_lowest_layer(m_ws).async_connect(server_endpoint(port), done); }))
            return ec;
        m_ws.set_option(websocket::stream_base::decorator([authorization, origin](websocket::request_type& req) {
            if (!authorization.empty())
                req.set(http::field::authorization, authorization);
            if (origin)
                req.set(http::field::origin, "null");
        }));
        websocket::response_type res;
        const auto               ec = run([&](auto done) { m_ws.async_handshake(res, host, "/", done); });
        status                      = res.result();
        return ec;
    }
    beast::error_code open(unsigned short port, const std::string& token)
    {
        return open(port, "127.0.0.1:" + std::to_string(port), "Bearer " + token);
    }
    beast::error_code send(const std::string& message, bool binary = false)
    {
        m_ws.binary(binary);
        return run([&](auto done) { m_ws.async_write(asio::buffer(message), done); });
    }
    beast::error_code receive(std::string& message)
    {
        beast::flat_buffer buffer;
        const auto         ec = run([&](auto done) { m_ws.async_read(buffer, done); });
        message               = beast::buffers_to_string(buffer.data());
        return ec;
    }
    beast::error_code close()
    {
        return run([&](auto done) { m_ws.async_close(websocket::close_code::normal, done); });
    }

private:
    template<class Start> beast::error_code run(Start start)
    {
        beast::error_code result = asio::error::timed_out;
        beast::get_lowest_layer(m_ws).expires_after(5s);
        start([&result](beast::error_code ec, auto&&...) { result = ec; });
        m_ioc.restart();
        m_ioc.run();
        return result;
    }

    asio::io_context                     m_ioc;
    websocket::stream<beast::tcp_stream> m_ws{m_ioc};
};

// Sends one request and expects it back from the echo handler.
void check_echo(unsigned short port, const std::string& token)
{
    TestClient client;
    REQUIRE_FALSE(client.open(port, token));
    REQUIRE_FALSE(client.send("ping"));
    std::string reply;
    REQUIRE_FALSE(client.receive(reply));
    CHECK(reply == "ping");
}

// Keeps the reply and alive pointer of the last request without answering it.
struct Recorder
{
    std::mutex          mutex;
    Reply               reply;
    std::weak_ptr<void> alive;
    int                 count{0};

    MessageHandler handler()
    {
        return [this](std::string, Reply r, std::weak_ptr<void> a) {
            std::lock_guard<std::mutex> lock(mutex);
            reply = std::move(r);
            alive = std::move(a);
            ++count;
        };
    }
    bool received()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return count > 0;
    }
    bool expired()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return alive.expired();
    }
    Reply last_reply()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return reply;
    }
};

} // namespace

TEST_CASE("Server answers a client with the right Host and token", "[slicer_api][integration]")
{
    const Options options = test_options();
    Server        server(options, echo);
    std::string   error;
    REQUIRE(server.start(error));
    REQUIRE(server.port() != 0);

    check_echo(server.port(), options.token);

    TestClient client;
    CHECK_FALSE(client.open(server.port(), "LocalHost:" + std::to_string(server.port()), "bearer " + options.token));
}

TEST_CASE("Server refuses browsers, other hosts and missing or wrong tokens", "[slicer_api][integration]")
{
    const Options        options = test_options();
    Server               server(options, echo);
    std::string          error;
    REQUIRE(server.start(error));
    const unsigned short port   = server.port();
    const std::string    host   = "127.0.0.1:" + std::to_string(port);
    const std::string    bearer = "Bearer " + options.token;

    SECTION("Origin, even with a valid token")
    {
        TestClient client;
        CHECK(client.open(port, host, bearer, true) == websocket::error::upgrade_declined);
        CHECK(client.status == http::status::forbidden);
    }
    SECTION("no token")
    {
        TestClient client;
        CHECK(client.open(port, host, "") == websocket::error::upgrade_declined);
        CHECK(client.status == http::status::unauthorized);
    }
    SECTION("wrong token")
    {
        TestClient client;
        CHECK(client.open(port, host, "Bearer " + generate_token()) == websocket::error::upgrade_declined);
        CHECK(client.status == http::status::unauthorized);
    }
    SECTION("wrong Host")
    {
        TestClient client;
        CHECK(client.open(port, "evil.example:" + std::to_string(port), bearer) == websocket::error::upgrade_declined);
        CHECK(client.status == http::status::forbidden);
    }
    SECTION("plain HTTP request")
    {
        asio::io_context ioc;
        tcp::socket      socket(ioc);
        socket.connect(server_endpoint(port));
        http::request<http::empty_body> req{http::verb::get, "/", 11};
        req.set(http::field::host, host);
        req.set(http::field::authorization, bearer);
        http::write(socket, req);
        beast::flat_buffer               buffer;
        http::response<http::empty_body> res;
        beast::error_code                ec;
        http::read(socket, buffer, res, ec);
        REQUIRE_FALSE(ec);
        CHECK(res.result() == http::status::bad_request);
        const auto connection = res[http::field::connection];
        CHECK(std::string(connection.data(), connection.size()) == "close");
    }

    // The server keeps working after a refusal.
    check_echo(port, options.token);
}

TEST_CASE("Server survives a bare TCP connect and close", "[slicer_api][integration]")
{
    const Options options = test_options();
    Server        server(options, echo);
    std::string   error;
    REQUIRE(server.start(error));

    for (int i = 0; i < 3; ++i) {
        asio::io_context ioc;
        tcp::socket      socket(ioc);
        socket.connect(server_endpoint(server.port()));
        socket.close();
    }
    check_echo(server.port(), options.token);
}

TEST_CASE("Server closes a connection that sends nothing after the handshake timeout", "[slicer_api][integration]")
{
    const Options options = test_options();
    Server        server(options, echo);
    std::string   error;
    REQUIRE(server.start(error));

    asio::io_context ioc;
    tcp::socket      socket(ioc);
    socket.connect(server_endpoint(server.port()));
    const auto        start = std::chrono::steady_clock::now();
    char              byte  = 0;
    bool              done  = false;
    beast::error_code ec;
    socket.async_read_some(asio::buffer(&byte, 1), [&](beast::error_code e, std::size_t) {
        ec   = e;
        done = true;
    });
    ioc.run_for(5s);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE(done);
    CHECK(ec);
    CHECK(elapsed >= 800ms);
    CHECK(elapsed < 3s);

    check_echo(server.port(), options.token);
}

TEST_CASE("Server closes only the session that sends a message over the limit or a binary frame", "[slicer_api][integration]")
{
    const Options options = test_options();
    Server        server(options, echo);
    std::string   error;
    REQUIRE(server.start(error));

    SECTION("too large")
    {
        TestClient client;
        REQUIRE_FALSE(client.open(server.port(), options.token));
        client.send(std::string(2 * options.max_message_bytes, 'x'));
        std::string reply;
        CHECK(client.receive(reply));
    }
    SECTION("binary")
    {
        TestClient client;
        REQUIRE_FALSE(client.open(server.port(), options.token));
        client.send("ping", true);
        std::string reply;
        CHECK(client.receive(reply));
    }
    check_echo(server.port(), options.token);
}

TEST_CASE("Server survives a handler that throws", "[slicer_api][integration]")
{
    const Options options = test_options();
    Server        server(options, [](std::string message, Reply reply, std::weak_ptr<void>) {
        if (message == "boom")
            throw std::runtime_error("handler failed");
        reply(std::move(message));
    });
    std::string error;
    REQUIRE(server.start(error));

    TestClient client;
    REQUIRE_FALSE(client.open(server.port(), options.token));
    REQUIRE_FALSE(client.send("boom"));
    REQUIRE_FALSE(client.send("ping"));
    std::string reply;
    REQUIRE_FALSE(client.receive(reply));
    CHECK(reply == "ping");
}

TEST_CASE("Server refuses a fifth session with 503", "[slicer_api][integration]")
{
    const Options options = test_options();
    Server        server(options, echo);
    std::string   error;
    REQUIRE(server.start(error));

    std::vector<std::unique_ptr<TestClient>> clients;
    for (std::size_t i = 0; i < options.max_sessions; ++i) {
        clients.push_back(std::make_unique<TestClient>());
        REQUIRE_FALSE(clients.back()->open(server.port(), options.token));
    }
    TestClient fifth;
    CHECK(fifth.open(server.port(), options.token) == websocket::error::upgrade_declined);
    CHECK(fifth.status == http::status::service_unavailable);

    // A closed session frees its slot.
    REQUIRE_FALSE(clients.front()->close());
    CHECK(wait_until([&] {
        TestClient client;
        return !client.open(server.port(), options.token);
    }));
}

TEST_CASE("Server sends a reply from another thread to the session that asked, and drops it once that session is gone",
          "[slicer_api][integration]")
{
    const Options options = test_options();
    Recorder      recorder;
    Server        server(options, recorder.handler());
    std::string   error;
    REQUIRE(server.start(error));

    {
        TestClient client;
        REQUIRE_FALSE(client.open(server.port(), options.token));
        REQUIRE_FALSE(client.send("request"));
        REQUIRE(wait_until([&] { return recorder.received(); }));
        CHECK_FALSE(recorder.expired());

        std::thread([&] { recorder.last_reply()("from a worker"); }).join();
        std::string reply;
        REQUIRE_FALSE(client.receive(reply));
        CHECK(reply == "from a worker");
        REQUIRE_FALSE(client.close());
    }
    REQUIRE(wait_until([&] { return recorder.expired(); }));
    CHECK_NOTHROW(recorder.last_reply()("too late"));

    // Another client gets nothing that was meant for the closed session.
    {
        TestClient client;
        REQUIRE_FALSE(client.open(server.port(), options.token));
        CHECK_NOTHROW(recorder.last_reply()("too late again"));
        REQUIRE_FALSE(client.send("next"));
        REQUIRE(wait_until([&] {
            std::lock_guard<std::mutex> lock(recorder.mutex);
            return recorder.count == 2;
        }));
        recorder.last_reply()("answer");
        std::string reply;
        REQUIRE_FALSE(client.receive(reply));
        CHECK(reply == "answer");
    }
}

TEST_CASE("Server stop returns quickly with open clients, is idempotent and expires alive", "[slicer_api][integration]")
{
    const Options options = test_options();
    Recorder      recorder;
    Server        server(options, recorder.handler());
    std::string   error;
    REQUIRE(server.start(error));

    TestClient client;
    REQUIRE_FALSE(client.open(server.port(), options.token));
    REQUIRE_FALSE(client.send("request"));
    REQUIRE(wait_until([&] { return recorder.received(); }));
    // A second connection that is still in the handshake.
    asio::io_context ioc;
    tcp::socket      socket(ioc);
    socket.connect(server_endpoint(server.port()));

    const auto start = std::chrono::steady_clock::now();
    server.stop();
    CHECK(std::chrono::steady_clock::now() - start < 2s);
    CHECK(recorder.expired());
    CHECK_NOTHROW(recorder.last_reply()("after stop"));
    CHECK_NOTHROW(server.stop());
}

TEST_CASE("Server reports a fixed port in use, an empty token and a second start", "[slicer_api][integration]")
{
    const Options options = test_options();
    Server        first(options, echo);
    std::string   error;
    REQUIRE(first.start(error));

    SECTION("port in use")
    {
        Options other = test_options();
        other.port    = first.port();
        Server second(other, echo);
        CHECK_FALSE(second.start(error));
        CHECK_FALSE(error.empty());
        CHECK(error.find(other.token) == std::string::npos);
    }
    SECTION("empty token")
    {
        Options other = test_options();
        other.token.clear();
        Server second(other, echo);
        CHECK_FALSE(second.start(error));
        CHECK_FALSE(error.empty());
    }
    SECTION("second start")
    {
        CHECK_FALSE(first.start(error));
        CHECK_FALSE(error.empty());
        check_echo(first.port(), options.token);
    }
}

namespace {

// True when the server closes the connection within `limit`.
bool closed_within(asio::io_context& ioc, tcp::socket& socket, std::chrono::milliseconds limit)
{
    auto              byte = std::make_shared<char>();
    bool              done = false;
    beast::error_code ec;
    socket.async_read_some(asio::buffer(byte.get(), 1), [&, byte](beast::error_code e, std::size_t) {
        ec   = e;
        done = true;
    });
    ioc.restart();
    ioc.run_for(limit);
    return done && ec;
}

} // namespace

TEST_CASE("Server closes a session whose replies pile up", "[slicer_api][integration]")
{
    const Options       options = test_options();
    std::mutex          mutex;
    bool                flooded = false;
    std::weak_ptr<void> flood_alive;
    Server              server(options, [&](std::string message, Reply reply, std::weak_ptr<void> alive) {
        if (message != "flood") {
            reply(std::move(message));
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            flood_alive = alive;
            flooded     = true;
        }
        const std::string big(1u << 20, 'x');
        for (int i = 0; i < 65; ++i)
            reply(big);
    });
    std::string error;
    REQUIRE(server.start(error));

    // This client never reads its replies.
    TestClient client;
    REQUIRE_FALSE(client.open(server.port(), options.token));
    REQUIRE_FALSE(client.send("flood"));
    CHECK(wait_until([&] {
        std::lock_guard<std::mutex> lock(mutex);
        return flooded && flood_alive.expired();
    }));
    check_echo(server.port(), options.token);
}

TEST_CASE("Server closes a connection whose request header is over 8 KB", "[slicer_api][integration]")
{
    const Options options = test_options();
    Server        server(options, echo);
    std::string   error;
    REQUIRE(server.start(error));

    asio::io_context ioc;
    tcp::socket      socket(ioc);
    socket.connect(server_endpoint(server.port()));
    const std::string request = "GET / HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(server.port()) +
                                "\r\nX-Padding: " + std::string(9000, 'a') + "\r\n\r\n";
    beast::error_code ignored;
    asio::write(socket, asio::buffer(request), ignored);
    // Well before the 1 s handshake timeout.
    CHECK(closed_within(ioc, socket, 700ms));
    check_echo(server.port(), options.token);
}

TEST_CASE("Server stop closes a connection in the middle of the handshake and an open session", "[slicer_api][integration]")
{
    Options options           = test_options();
    options.handshake_timeout = std::chrono::seconds(5); // only stop() may close the half handshake
    Server      server(options, echo);
    std::string error;
    REQUIRE(server.start(error));

    TestClient client;
    REQUIRE_FALSE(client.open(server.port(), options.token));

    asio::io_context ioc;
    tcp::socket      socket(ioc);
    socket.connect(server_endpoint(server.port()));
    const std::string partial = "GET / HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(server.port()) + "\r\nUpgrade: websocket\r\n";
    asio::write(socket, asio::buffer(partial));
    // Give the server time to read the partial request; it then waits for the rest.
    std::this_thread::sleep_for(200ms);

    const auto start = std::chrono::steady_clock::now();
    server.stop();
    CHECK(std::chrono::steady_clock::now() - start < 2s);
    CHECK(closed_within(ioc, socket, 2000ms));
    std::string reply;
    CHECK(client.receive(reply));
}

TEST_CASE("Server stop from the handler only stops the io loop", "[slicer_api][integration]")
{
    const Options     options = test_options();
    Server*           self    = nullptr;
    std::atomic<bool> stopped_in_handler{false};
    Server            server(options, [&](std::string, Reply, std::weak_ptr<void>) {
        self->stop();
        stopped_in_handler = true;
    });
    self = &server;
    std::string error;
    REQUIRE(server.start(error));

    TestClient client;
    REQUIRE_FALSE(client.open(server.port(), options.token));
    REQUIRE_FALSE(client.send("stop"));
    REQUIRE(wait_until([&] { return stopped_in_handler.load(); }));

    const auto start = std::chrono::steady_clock::now();
    server.stop();
    CHECK(std::chrono::steady_clock::now() - start < 2s);
    std::string reply;
    CHECK(client.receive(reply));
    CHECK_NOTHROW(server.stop());
}

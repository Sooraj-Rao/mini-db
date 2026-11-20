#include "storage.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <memory>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static constexpr socket_t invalid_socket = INVALID_SOCKET;
static void close_socket(socket_t s) { if (s != invalid_socket) closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t invalid_socket = -1;
static void close_socket(socket_t s) { if (s != invalid_socket) close(s); }
#endif

namespace {
#ifdef _WIN32
struct SocketRuntime {
    SocketRuntime() { WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("WSAStartup failed"); }
    ~SocketRuntime() { WSACleanup(); }
};
#else
struct SocketRuntime {};
#endif

bool send_all(socket_t socket, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
#ifdef _WIN32
        const int n = send(socket, data.data() + sent, static_cast<int>(data.size() - sent), 0);
#else
        const auto n = send(socket, data.data() + sent, data.size() - sent, 0);
#endif
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool receive_line(socket_t socket, std::string& line) {
    line.clear();
    char ch;
    while (true) {
#ifdef _WIN32
        const int n = recv(socket, &ch, 1, 0);
#else
        const auto n = recv(socket, &ch, 1, 0);
#endif
        if (n <= 0) return !line.empty();
        if (ch == '\n') {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }
        if (line.size() >= 1024 * 1024) return false;
        line.push_back(ch);
    }
}

std::string upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

std::string execute(minidb::Storage& db, const std::string& request, bool& quit) {
    std::istringstream input(request);
    std::string command;
    input >> command;
    command = upper(command);
    if (command.empty()) return "-ERR empty command\r\n";
    if (command == "PING") return "+PONG\r\n";
    if (command == "SET") {
        std::string key; input >> key;
        std::string value; std::getline(input >> std::ws, value);
        if (key.empty() || value.empty()) return "-ERR usage: SET key value [EX seconds]\r\n";
        try {
            const auto marker = value.rfind(" EX ");
            if (marker != std::string::npos) {
                const auto seconds_text = value.substr(marker + 4);
                std::size_t used = 0; const auto seconds = std::stoull(seconds_text, &used);
                if (used != seconds_text.size() || seconds == 0) return "-ERR invalid expiration seconds\r\n";
                value.resize(marker); if (value.empty()) return "-ERR value cannot be empty\r\n";
                db.set_with_ttl(key, value, seconds);
            } else db.set(key, value);
            return "+OK\r\n";
        } catch (const std::exception& e) { return std::string("-ERR ") + e.what() + "\r\n"; }
    }
    if (command == "EXPIRE") {
        std::string key, seconds_text; input >> key >> seconds_text;
        if (key.empty() || seconds_text.empty()) return "-ERR usage: EXPIRE key seconds\r\n";
        try { std::size_t used=0; auto seconds=std::stoull(seconds_text,&used); if(used!=seconds_text.size() || !seconds) return "-ERR invalid expiration seconds\r\n"; return db.expire(key,seconds) ? ":1\r\n" : ":0\r\n"; }
        catch (const std::exception& e) { return std::string("-ERR ") + e.what() + "\r\n"; }
    }
    if (command == "TTL") { std::string key; input >> key; if(key.empty()) return "-ERR usage: TTL key\r\n"; return ":" + std::to_string(db.ttl(key)) + "\r\n"; }
    if (command == "PERSIST") { std::string key; input >> key; if(key.empty()) return "-ERR usage: PERSIST key\r\n"; return db.persist(key) ? ":1\r\n" : ":0\r\n"; }
    if (command == "GET") {
        std::string key; input >> key;
        if (key.empty()) return "-ERR usage: GET key\r\n";
        const auto value = db.get(key);
        if (!value) return "$-1\r\n";
        return "$" + std::to_string(value->size()) + "\r\n" + *value + "\r\n";
    }
    if (command == "DELETE" || command == "DEL") {
        std::string key; input >> key;
        if (key.empty()) return "-ERR usage: DELETE key\r\n";
        return db.remove(key) ? ":1\r\n" : ":0\r\n";
    }
    if (command == "SIZE") return ":" + std::to_string(db.size()) + "\r\n";
    if (command == "QUIT") { quit = true; return "+BYE\r\n"; }
    return "-ERR unknown command\r\n";
}

unsigned short parse_port(const char* text) {
    const long value = std::strtol(text, nullptr, 10);
    if (value < 1 || value > 65535) throw std::runtime_error("port must be between 1 and 65535");
    return static_cast<unsigned short>(value);
}
}

namespace {
void handle_client(socket_t client, std::shared_ptr<minidb::Storage> db) {
    std::cout << "Client connected (worker " << std::this_thread::get_id() << ")\n";
    std::string request;
    try {
        while (receive_line(client, request)) {
            bool quit = false;
            std::string response;
            try {
                response = execute(*db, request, quit);
            } catch (const std::exception& e) {
                response = std::string("-ERR ") + e.what() + "\r\n";
            }
            if (!send_all(client, response) || quit) break;
        }
    } catch (const std::exception& e) {
        std::cerr << "Client handler error: " << e.what() << '\n';
    }
    close_socket(client);
    std::cout << "Client disconnected (worker " << std::this_thread::get_id() << ")\n";
}
}

int main(int argc, char** argv) {
    try {
        SocketRuntime runtime;
        const unsigned short port = argc > 1 ? parse_port(argv[1]) : 6380;
        const std::string db_path = argc > 2 ? argv[2] : "data/minidb.db";
        auto db = std::make_shared<minidb::Storage>(db_path);
        socket_t listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == invalid_socket) throw std::runtime_error("could not create listening socket");
        int reuse = 1;
#ifdef _WIN32
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(port);
        if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            close_socket(listener); throw std::runtime_error("bind failed; is the port already in use?");
        }
        if (listen(listener, SOMAXCONN) != 0) { close_socket(listener); throw std::runtime_error("listen failed"); }
        std::cout << "MiniDB TCP server listening on 0.0.0.0:" << port << "\n"
                  << "Database: " << db_path << " (" << db->size() << " keys loaded)\n"
                  << "Protocol: one UTF-8 command per line; each client runs on its own worker thread.\n"
                  << "TTL sweeper runs once per second. Press Ctrl+C to stop.\n" << std::flush;
        std::thread([db] { while (true) { std::this_thread::sleep_for(std::chrono::seconds(1)); try { db->sweep_expired(); } catch (const std::exception& e) { std::cerr << "TTL sweeper error: " << e.what() << '\n'; } } }).detach();

        while (true) {
            socket_t client = accept(listener, nullptr, nullptr);
            if (client == invalid_socket) continue;
            try {
                std::thread(handle_client, client, db).detach();
            } catch (...) {
                close_socket(client);
                throw;
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "MiniDB server error: " << error.what() << '\n';
        return 1;
    }
}

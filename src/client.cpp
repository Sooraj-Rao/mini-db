#include <iostream>
#include <stdexcept>
#include <string>

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
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t invalid_socket = -1;
static void close_socket(socket_t s) { if (s != invalid_socket) close(s); }
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) { std::cerr << "WSAStartup failed\n"; return 1; }
#endif
    const std::string host = argc > 1 ? argv[1] : "127.0.0.1";
    const std::string port = argc > 2 ? argv[2] : "6380";
    addrinfo hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    const int lookup = getaddrinfo(host.c_str(), port.c_str(), &hints, &results);
    if (lookup != 0) { std::cerr << "Could not resolve server address\n";
#ifdef _WIN32
        WSACleanup();
#endif
        return 1; }
    socket_t connection = invalid_socket;
    for (addrinfo* item = results; item; item = item->ai_next) {
        connection = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (connection == invalid_socket) continue;
        if (connect(connection, item->ai_addr, static_cast<int>(item->ai_addrlen)) == 0) break;
        close_socket(connection); connection = invalid_socket;
    }
    freeaddrinfo(results);
    if (connection == invalid_socket) {
        std::cerr << "Could not connect to " << host << ':' << port << '\n';
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }
    std::cout << "Connected to MiniDB at " << host << ':' << port << "\n"
              << "Enter PING, SET key value [EX seconds], GET, DELETE, EXPIRE, TTL, PERSIST, SIZE or QUIT.\n";
    std::string line;
    while (std::cout << "minidb-net> " && std::getline(std::cin, line)) {
        if (line.empty()) continue;
        std::string outgoing = line + "\r\n";
        std::size_t sent = 0;
        while (sent < outgoing.size()) {
#ifdef _WIN32
            int n = send(connection, outgoing.data() + sent, static_cast<int>(outgoing.size() - sent), 0);
#else
            auto n = send(connection, outgoing.data() + sent, outgoing.size() - sent, 0);
#endif
            if (n <= 0) { std::cerr << "Send failed\n"; close_socket(connection); return 1; }
            sent += static_cast<std::size_t>(n);
        }
        std::string response;
        char ch;
        bool header_done = false;
        std::size_t bulk_length = 0;
        while (true) {
#ifdef _WIN32
            int n = recv(connection, &ch, 1, 0);
#else
            auto n = recv(connection, &ch, 1, 0);
#endif
            if (n <= 0) { std::cerr << "Server disconnected\n"; close_socket(connection); return 1; }
            response.push_back(ch);
            if (response.size() >= 2 && response.substr(response.size()-2) == "\r\n") {
                if (!header_done && response[0] == '$' && response.size() > 3 && response[1] != '-') {
                    try { bulk_length = static_cast<std::size_t>(std::stoull(response.substr(1, response.size()-3))); }
                    catch (...) { break; }
                    header_done = true;
                    if (bulk_length == 0) break;
                } else if (header_done) {
                    if (response.size() >= (response.find("\r\n") + 2) + bulk_length + 2) break;
                } else break;
            }
            if (response.size() > 1024 * 1024 + 128) { std::cerr << "Response too large\n"; break; }
        }
        if (!response.empty()) std::cout << response;
        if (line == "QUIT" || line == "quit") break;
    }
    close_socket(connection);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}

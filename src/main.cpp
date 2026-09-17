#include <arpa/inet.h>
#include <csignal>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <netdb.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "logger.hpp"
#include "uci.hpp"
#include "usage.hpp"

namespace {

constexpr const char* APP_NAME = "tcpredir";
constexpr const char* APP_VERSION = "0.1.0";
constexpr int BUFFER_SIZE = 16384;
constexpr int UDP_REPLY_TIMEOUT_MS = 5000;

volatile std::sig_atomic_t running = 1;

struct redirect_t {
    std::string name;
    std::string listen_ip = "0.0.0.0";
    uint16_t listen_port = 0;
    std::string target_ip;
    uint16_t target_port = 0;
    bool tcp = true;
    bool udp = false;
    bool enabled = true;
};

void on_signal(int) {
    running = 0;
}

void close_fd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

uint16_t parse_port(const std::string& value, const std::string& what) {
    try {
        size_t pos = 0;
        long p = std::stol(value, &pos, 10);
        if (pos != value.size() || p < 1 || p > 65535)
            throw std::out_of_range("port out of range");
        return static_cast<uint16_t>(p);
    } catch (const std::exception&) {
        throw std::runtime_error("invalid " + what + ": '" + value + "'");
    }
}

std::string opt_string(const UCI::SECTION& section, const std::string& name, const std::string& def = "") {
    if (!section.contains(name)) return def;
    return section[name].to_string();
}

bool opt_bool(const UCI::SECTION& section, const std::string& name, bool def) {
    if (!section.contains(name)) return def;
    try { return section[name].to_bool(); }
    catch (...) {
        std::string v = section[name].to_string();
        for (char& c : v) c = static_cast<char>(::tolower(c));
        return v == "1" || v == "true" || v == "yes" || v == "on" || v == "enabled";
    }
}

std::vector<redirect_t> load_config(const std::string& config) {
    UCI::PACKAGE pkg(config);
    std::vector<redirect_t> redirects;

    if (!pkg.contains("redirect"))
        throw std::runtime_error("configuration has no 'redirect' sections");

    for (const auto& section : pkg["redirect"]) {
        redirect_t r;
        r.name = section.is_anonymous() ? ("redirect#" + std::to_string(section.index())) : section.name();
        r.enabled = opt_bool(section, "enabled", true);
        if (!r.enabled) continue;

        r.listen_ip = opt_string(section, "listen_ip", opt_string(section, "listen_addr", "0.0.0.0"));
        r.target_ip = opt_string(section, "target_ip", opt_string(section, "target_addr", ""));
        r.listen_port = parse_port(opt_string(section, "listen_port"), r.name + ".listen_port");
        r.target_port = parse_port(opt_string(section, "target_port"), r.name + ".target_port");

        std::string proto = opt_string(section, "proto", opt_string(section, "protocol", "tcp"));
        for (char& c : proto) c = static_cast<char>(::tolower(c));
        if (proto == "tcp") { r.tcp = true; r.udp = false; }
        else if (proto == "udp") { r.tcp = false; r.udp = true; }
        else if (proto == "both" || proto == "tcpudp" || proto == "tcp+udp") { r.tcp = true; r.udp = true; }
        else throw std::runtime_error("invalid protocol for " + r.name + ": " + proto);

        if (r.target_ip.empty())
            throw std::runtime_error("missing target_ip for " + r.name);

        redirects.push_back(r);
    }

    if (redirects.empty())
        throw std::runtime_error("no enabled redirects");

    return redirects;
}

int create_bound_socket(const std::string& ip, uint16_t port, int socktype, bool listen_socket) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = socktype;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* res = nullptr;
    int rc = ::getaddrinfo(ip.empty() ? nullptr : ip.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (rc != 0)
        throw std::runtime_error("getaddrinfo(" + ip + ":" + std::to_string(port) + "): " + gai_strerror(rc));

    int fd = -1;
    std::string last_error;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) { last_error = std::strerror(errno); continue; }

        int yes = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif
        if (::bind(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            if (!listen_socket || ::listen(fd, 128) == 0) break;
        }
        last_error = std::strerror(errno);
        close_fd(fd);
    }
    ::freeaddrinfo(res);

    if (fd < 0)
        throw std::runtime_error("bind " + ip + ":" + std::to_string(port) + " failed: " + last_error);
    return fd;
}

int connect_target(const redirect_t& r, int socktype) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = socktype;

    addrinfo* res = nullptr;
    int rc = ::getaddrinfo(r.target_ip.c_str(), std::to_string(r.target_port).c_str(), &hints, &res);
    if (rc != 0)
        throw std::runtime_error("target resolve " + r.target_ip + ":" + std::to_string(r.target_port) + ": " + gai_strerror(rc));

    int fd = -1;
    std::string last_error;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) { last_error = std::strerror(errno); continue; }
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        last_error = std::strerror(errno);
        close_fd(fd);
    }
    ::freeaddrinfo(res);

    if (fd < 0)
        throw std::runtime_error("connect target " + r.target_ip + ":" + std::to_string(r.target_port) + " failed: " + last_error);
    return fd;
}

bool send_all(int fd, const char* data, ssize_t len) {
    ssize_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, data + sent, static_cast<size_t>(len - sent), MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        sent += n;
    }
    return true;
}

void proxy_tcp(int client_fd, redirect_t r) {
    int target_fd = -1;
    try {
        target_fd = connect_target(r, SOCK_STREAM);
        logger::info[r.name] << "tcp client connected -> " << r.target_ip << ':' << r.target_port << std::endl;

        pollfd fds[2] = {{client_fd, POLLIN, 0}, {target_fd, POLLIN, 0}};
        char buf[BUFFER_SIZE];

        while (running) {
            int rc = ::poll(fds, 2, -1);
            if (rc < 0) {
                if (errno == EINTR) continue;
                break;
            }
            for (int i = 0; i < 2; ++i) {
                if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) { running = running; goto done; }
                if (fds[i].revents & POLLIN) {
                    int in = fds[i].fd;
                    int out = fds[i ^ 1].fd;
                    ssize_t n = ::recv(in, buf, sizeof(buf), 0);
                    if (n <= 0) goto done;
                    if (!send_all(out, buf, n)) goto done;
                }
            }
        }
    } catch (const std::exception& e) {
        logger::error[r.name] << e.what() << std::endl;
    }
done:
    close_fd(client_fd);
    close_fd(target_fd);
}

void tcp_listener(redirect_t r) {
    int fd = -1;
    try {
        fd = create_bound_socket(r.listen_ip, r.listen_port, SOCK_STREAM, true);
        logger::notice[r.name] << "tcp listening " << r.listen_ip << ':' << r.listen_port
                               << " -> " << r.target_ip << ':' << r.target_port << std::endl;
        while (running) {
            sockaddr_storage addr{};
            socklen_t len = sizeof(addr);
            int client = ::accept(fd, reinterpret_cast<sockaddr*>(&addr), &len);
            if (client < 0) {
                if (errno == EINTR) continue;
                if (running) logger::error[r.name] << "accept: " << std::strerror(errno) << std::endl;
                continue;
            }
            std::thread(proxy_tcp, client, r).detach();
        }
    } catch (const std::exception& e) {
        logger::error[r.name] << e.what() << std::endl;
        running = 0;
    }
    close_fd(fd);
}

void udp_request(int listen_fd, redirect_t r, std::vector<char> data, sockaddr_storage client_addr, socklen_t client_len) {
    int target = -1;
    try {
        target = connect_target(r, SOCK_DGRAM);
        if (::send(target, data.data(), data.size(), MSG_NOSIGNAL) < 0)
            throw std::runtime_error(std::string("udp send target: ") + std::strerror(errno));

        pollfd pfd{target, POLLIN, 0};
        int rc = ::poll(&pfd, 1, UDP_REPLY_TIMEOUT_MS);
        if (rc > 0 && (pfd.revents & POLLIN)) {
            std::vector<char> reply(65536);
            ssize_t n = ::recv(target, reply.data(), reply.size(), 0);
            if (n > 0)
                ::sendto(listen_fd, reply.data(), static_cast<size_t>(n), MSG_NOSIGNAL,
                         reinterpret_cast<sockaddr*>(&client_addr), client_len);
        }
    } catch (const std::exception& e) {
        logger::warning[r.name] << e.what() << std::endl;
    }
    close_fd(target);
}

void udp_listener(redirect_t r) {
    int fd = -1;
    try {
        fd = create_bound_socket(r.listen_ip, r.listen_port, SOCK_DGRAM, false);
        logger::notice[r.name] << "udp listening " << r.listen_ip << ':' << r.listen_port
                               << " -> " << r.target_ip << ':' << r.target_port << std::endl;
        while (running) {
            std::vector<char> buf(65536);
            sockaddr_storage client_addr{};
            socklen_t client_len = sizeof(client_addr);
            ssize_t n = ::recvfrom(fd, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (running) logger::error[r.name] << "recvfrom: " << std::strerror(errno) << std::endl;
                continue;
            }
            buf.resize(static_cast<size_t>(n));
            std::thread(udp_request, fd, r, std::move(buf), client_addr, client_len).detach();
        }
    } catch (const std::exception& e) {
        logger::error[r.name] << e.what() << std::endl;
        running = 0;
    }
    close_fd(fd);
}

} // namespace

int main(int argc, char** argv) {
    usage_t usage = {
        .args = { argc, argv },
        .info = {
            .name = APP_NAME,
            .version = APP_VERSION,
            .author = "Oskari Rauta / OpenAI Codex",
            .usage = "[options]",
            .description = "Simple TCP/UDP redirector using OpenWrt UCI style config."
        },
        .options = {
            { "config",  { .key = "c", .word = "config",  .desc = "UCI config name or path (default: tcpredir)", .flag = usage_t::REQUIRED, .name = "file" }},
            { "verbose", { .key = "V", .word = "verbose", .desc = "verbose logging" }},
            { "quiet",   { .key = "q", .word = "quiet",   .desc = "only errors" }},
            { "help",    { .key = "h", .word = "help",    .desc = "show help" }},
            { "version", { .key = "v", .word = "version", .desc = "show version" }}
        }
    };

    if (usage["help"]) {
        std::cout << usage << std::endl;
        return 0;
    }
    if (usage["version"]) {
        std::cout << APP_NAME << " " << APP_VERSION << std::endl;
        return 0;
    }
    if (!usage.errors().empty()) {
        std::cerr << usage.errors() << std::endl << usage << std::endl;
        return 2;
    }

    logger::prefix = APP_NAME;
    logger::log_level = usage["quiet"] ? logger::error.id() : (usage["verbose"] ? logger::verbose.id() : logger::notice.id());

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    try {
        const std::string config = usage["config"] ? usage["config"].stringValue() : "tcpredir";
        auto redirects = load_config(config);
        std::vector<std::thread> threads;

        for (const auto& r : redirects) {
            if (r.tcp) threads.emplace_back(tcp_listener, r);
            if (r.udp) threads.emplace_back(udp_listener, r);
        }

        for (auto& t : threads)
            if (t.joinable()) t.join();

    } catch (const std::exception& e) {
        logger::error << e.what() << std::endl;
        return 1;
    }

    logger::notice << "stopped" << std::endl;
    return 0;
}

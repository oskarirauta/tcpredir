#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <mutex>
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
#include "ubus.hpp"
#include "uloop.hpp"
#include "json.hpp"
#include "version.hpp"

namespace {

constexpr int BUFFER_SIZE = 16384;
constexpr int POLL_TICK_MS = 1000;

volatile std::sig_atomic_t stop_requested = 0;
volatile std::sig_atomic_t reload_requested = 0;

// logger_cpp keeps one global stream object per level and accumulates the tag and
// the message in it, so two threads logging at once corrupt each other's state -
// its internal mutex guards the message store, not the accumulation. uxcd never
// hits this because it is single-threaded; tcpredir has a listener thread per
// redirect and a thread per connection, all of which log. Two redirects were
// enough to abort in std::string::front(). Every log statement reachable from a
// worker thread goes through LOG(), which holds the lock for the WHOLE statement
// - not just one <<.
std::mutex log_mutex;
#define LOG(stmt) do { const std::lock_guard<std::mutex> _lk(log_mutex); stmt; } while (0)

// The redirects currently being served, and the listener threads serving them.
// Defined after redirect_t below, which it contains.
bool active_from_config = false;          // config file, as opposed to the command line
std::string active_config_name;
std::atomic<unsigned long> generation{1};

struct redirect_t {
    std::string name;
    std::string listen_ip = "0.0.0.0";
    uint16_t listen_port = 0;
    std::string target_ip;
    uint16_t target_port = 0;
    bool tcp = true;
    bool udp = false;
    bool enabled = true;
    int connect_timeout = 10;
    int idle_timeout = 300;
    int udp_timeout = 5;
    int max_connections = 0; // 0 = unlimited
    unsigned long generation = 0;
    // Shared with every thread serving this redirect (the listener, and each
    // connection), because they all hold copies of the struct. Set it and they
    // wind down within one poll tick, leaving other redirects untouched.
    std::shared_ptr<std::atomic<bool>> stop = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<int>> active_connections = std::make_shared<std::atomic<int>>(0);
};

// A redirect used to be stoppable only by bumping a global generation counter,
// which meant every change - adding one redirect, removing another - tore down
// ALL of them and dropped every connection in flight. Each redirect now carries
// its own stop flag, so listeners can be started and stopped one at a time,
// which is what the ubus write methods need.
//
// The registry is shared: ubus callbacks (uloop, main thread) read it while
// listener threads run, so it is guarded. Listener threads never touch it
// themselves - only the main thread adds and removes - but a reader must still
// not walk the vector while it is being resized.
struct listener_t {
    redirect_t r;
    std::vector<std::thread> threads;
};
std::mutex registry_mutex;
std::vector<std::shared_ptr<listener_t>> registry;   // guarded by registry_mutex

void on_stop_signal(int) {
    stop_requested = 1;
}

void on_reload_signal(int) {
    reload_requested = 1;
}

bool should_stop(const redirect_t& r) {
    return stop_requested || r.stop->load();
}

void close_fd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

int set_nonblock(int fd, bool enabled) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (enabled) flags |= O_NONBLOCK;
    else flags &= ~O_NONBLOCK;
    return ::fcntl(fd, F_SETFL, flags);
}

long parse_long(const std::string& value, const std::string& what, long min, long max) {
    try {
        size_t pos = 0;
        long n = std::stol(value, &pos, 10);
        if (pos != value.size() || n < min || n > max)
            throw std::out_of_range("out of range");
        return n;
    } catch (const std::exception&) {
        throw std::runtime_error("invalid " + what + ": '" + value + "'");
    }
}

uint16_t parse_port(const std::string& value, const std::string& what) {
    return static_cast<uint16_t>(parse_long(value, what, 1, 65535));
}

std::string opt_string(const UCI::SECTION& section, const std::string& name, const std::string& def = "") {
    if (!section.contains(name)) return def;
    return section[name].to_string();
}

int opt_int(const UCI::SECTION& section, const std::string& name, int def, int min, int max) {
    if (!section.contains(name)) return def;
    return static_cast<int>(parse_long(section[name].to_string(), name, min, max));
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

// Split on ':' at the top level only, so a bracketed IPv6 literal keeps its
// colons: "[fd00::1]:80" is two fields, not four.
std::vector<std::string> split_fields(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    for (char c : s) {
        if (c == '[') { depth++; continue; }
        if (c == ']') { if (depth > 0) depth--; continue; }
        if (c == ':' && depth == 0) { out.push_back(cur); cur.clear(); continue; }
        cur += c;
    }
    out.push_back(cur);
    return out;
}

// One command-line redirect spec:
//
//   [listen_ip:]listen_port:target_ip:target_port[/proto]
//
// listen_ip defaults to 0.0.0.0, proto to tcp. IPv6 literals must be bracketed
// ([::1]:1080:[fd00::2]:80) so their colons are not read as field separators.
redirect_t parse_spec(const std::string& spec, size_t index, unsigned long gen,
                      int connect_timeout, int idle_timeout, int udp_timeout, int max_connections) {
    redirect_t r;
    r.generation = gen;
    r.name = "redirect#" + std::to_string(index);
    r.connect_timeout = connect_timeout;
    r.idle_timeout = idle_timeout;
    r.udp_timeout = udp_timeout;
    r.max_connections = max_connections;

    std::string body = spec;
    std::string::size_type slash = body.rfind('/');
    if (slash != std::string::npos) {
        std::string proto = body.substr(slash + 1);
        body = body.substr(0, slash);
        for (char& c : proto) c = static_cast<char>(::tolower(c));
        if (proto == "tcp") { r.tcp = true; r.udp = false; }
        else if (proto == "udp") { r.tcp = false; r.udp = true; }
        else if (proto == "both" || proto == "tcpudp" || proto == "tcp+udp") { r.tcp = true; r.udp = true; }
        else throw std::runtime_error("invalid protocol in '" + spec + "': " + proto);
    }

    std::vector<std::string> f = split_fields(body);
    if (f.size() == 4) {
        r.listen_ip = f[0];
        r.listen_port = parse_port(f[1], "listen_port");
        r.target_ip = f[2];
        r.target_port = parse_port(f[3], "target_port");
    } else if (f.size() == 3) {
        r.listen_port = parse_port(f[0], "listen_port");
        r.target_ip = f[1];
        r.target_port = parse_port(f[2], "target_port");
    } else {
        throw std::runtime_error("malformed redirect '" + spec +
                                 "': expected [listen_ip:]listen_port:target_ip:target_port[/proto]");
    }

    if (r.listen_ip.empty()) r.listen_ip = "0.0.0.0";
    if (r.target_ip.empty())
        throw std::runtime_error("missing target address in '" + spec + "'");

    // a name that reads well in the log: "1080->10.102.2.2:80"
    r.name = std::to_string(r.listen_port) + "->" + r.target_ip + ":" + std::to_string(r.target_port);
    return r;
}

// Redirects given on the command line instead of in a UCI file. This is what a
// supervisor (uxcd) uses: it knows the container's address and the port to
// publish, and spawning `tcpredir 1080:10.102.2.2:80` needs no generated config
// file that two programs would then both own.
std::vector<redirect_t> load_specs(const std::vector<std::string>& specs, unsigned long gen,
                                   int connect_timeout, int idle_timeout, int udp_timeout, int max_connections) {
    std::vector<redirect_t> redirects;
    for (size_t i = 0; i < specs.size(); i++)
        redirects.push_back(parse_spec(specs[i], i, gen, connect_timeout, idle_timeout, udp_timeout, max_connections));
    return redirects;
}

std::vector<redirect_t> load_config(const std::string& config, unsigned long gen) {
    UCI::PACKAGE pkg(config);
    std::vector<redirect_t> redirects;

    if (!pkg.contains("redirect"))
        throw std::runtime_error("configuration has no 'redirect' sections");

    for (const auto& section : pkg["redirect"]) {
        redirect_t r;
        r.generation = gen;
        r.name = section.is_anonymous() ? ("redirect#" + std::to_string(section.index())) : section.name();
        r.enabled = opt_bool(section, "enabled", true);
        if (!r.enabled) continue;

        r.listen_ip = opt_string(section, "listen_ip", opt_string(section, "listen_addr", "0.0.0.0"));
        r.target_ip = opt_string(section, "target_ip", opt_string(section, "target_addr", ""));
        r.listen_port = parse_port(opt_string(section, "listen_port"), r.name + ".listen_port");
        r.target_port = parse_port(opt_string(section, "target_port"), r.name + ".target_port");
        r.connect_timeout = opt_int(section, "connect_timeout", 10, 1, 3600);
        r.idle_timeout = opt_int(section, "idle_timeout", 300, 1, 86400);
        r.udp_timeout = opt_int(section, "udp_timeout", 5, 1, 3600);
        r.max_connections = opt_int(section, "max_connections", 0, 0, 1000000);

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

        if (socktype == SOCK_STREAM) {
            set_nonblock(fd, true);
            rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
            if (rc == 0) {
                set_nonblock(fd, false);
                break;
            }
            if (errno == EINPROGRESS) {
                pollfd pfd{fd, POLLOUT, 0};
                rc = ::poll(&pfd, 1, r.connect_timeout * 1000);
                if (rc > 0 && (pfd.revents & POLLOUT)) {
                    int err = 0;
                    socklen_t len = sizeof(err);
                    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
                        set_nonblock(fd, false);
                        break;
                    }
                    errno = err;
                } else if (rc == 0) {
                    errno = ETIMEDOUT;
                }
            }
        } else if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }

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

struct connection_guard {
    std::shared_ptr<std::atomic<int>> counter;
    explicit connection_guard(std::shared_ptr<std::atomic<int>> c) : counter(std::move(c)) {}
    ~connection_guard() { if (counter) --(*counter); }
};

void proxy_tcp(int client_fd, redirect_t r) {
    connection_guard guard(r.active_connections);
    int target_fd = -1;
    try {
        target_fd = connect_target(r, SOCK_STREAM);
        LOG(logger::info[r.name] << "tcp client connected -> " << r.target_ip << ':' << r.target_port << std::endl);

        pollfd fds[2] = {{client_fd, POLLIN, 0}, {target_fd, POLLIN, 0}};
        char buf[BUFFER_SIZE];
        const int timeout_ms = std::min(r.idle_timeout * 1000, POLL_TICK_MS);
        int idle_ticks = 0;
        const int max_idle_ticks = std::max(1, (r.idle_timeout * 1000 + timeout_ms - 1) / timeout_ms);

        while (!should_stop(r)) {
            int rc = ::poll(fds, 2, timeout_ms);
            if (rc < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (rc == 0) {
                if (++idle_ticks >= max_idle_ticks) {
                    LOG(logger::verbose[r.name] << "tcp idle timeout" << std::endl);
                    break;
                }
                continue;
            }
            idle_ticks = 0;
            for (int i = 0; i < 2; ++i) {
                if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) goto done;
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
        LOG(logger::error[r.name] << e.what() << std::endl);
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
                               << " -> " << r.target_ip << ':' << r.target_port
                               << " max_connections=" << r.max_connections
                               << " idle_timeout=" << r.idle_timeout << "s" << std::endl;
        pollfd pfd{fd, POLLIN, 0};
        while (!should_stop(r)) {
            int rc = ::poll(&pfd, 1, POLL_TICK_MS);
            if (rc < 0) {
                if (errno == EINTR) continue;
                LOG(if (!should_stop(r)) logger::error[r.name] << "poll: " << std::strerror(errno) << std::endl);
                continue;
            }
            if (rc == 0 || !(pfd.revents & POLLIN)) continue;

            sockaddr_storage addr{};
            socklen_t len = sizeof(addr);
            int client = ::accept(fd, reinterpret_cast<sockaddr*>(&addr), &len);
            if (client < 0) {
                if (errno == EINTR) continue;
                LOG(if (!should_stop(r)) logger::error[r.name] << "accept: " << std::strerror(errno) << std::endl);
                continue;
            }

            int active = r.active_connections->load();
            if (r.max_connections > 0 && active >= r.max_connections) {
                LOG(logger::warning[r.name] << "connection limit reached (" << r.max_connections << ")" << std::endl);
                close_fd(client);
                continue;
            }
            ++(*r.active_connections);
            std::thread(proxy_tcp, client, r).detach();
        }
    } catch (const std::exception& e) {
        LOG(logger::error[r.name] << e.what() << std::endl);
        stop_requested = 1;
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
        int rc = ::poll(&pfd, 1, r.udp_timeout * 1000);
        if (rc > 0 && (pfd.revents & POLLIN)) {
            std::vector<char> reply(65536);
            ssize_t n = ::recv(target, reply.data(), reply.size(), 0);
            if (n > 0 && !should_stop(r))
                ::sendto(listen_fd, reply.data(), static_cast<size_t>(n), MSG_NOSIGNAL,
                         reinterpret_cast<sockaddr*>(&client_addr), client_len);
        }
    } catch (const std::exception& e) {
        LOG(logger::warning[r.name] << e.what() << std::endl);
    }
    close_fd(target);
}

void udp_listener(redirect_t r) {
    int fd = -1;
    try {
        fd = create_bound_socket(r.listen_ip, r.listen_port, SOCK_DGRAM, false);
        logger::notice[r.name] << "udp listening " << r.listen_ip << ':' << r.listen_port
                               << " -> " << r.target_ip << ':' << r.target_port
                               << " udp_timeout=" << r.udp_timeout << "s" << std::endl;
        pollfd pfd{fd, POLLIN, 0};
        while (!should_stop(r)) {
            int rc = ::poll(&pfd, 1, POLL_TICK_MS);
            if (rc < 0) {
                if (errno == EINTR) continue;
                LOG(if (!should_stop(r)) logger::error[r.name] << "poll: " << std::strerror(errno) << std::endl);
                continue;
            }
            if (rc == 0 || !(pfd.revents & POLLIN)) continue;

            std::vector<char> buf(65536);
            sockaddr_storage client_addr{};
            socklen_t client_len = sizeof(client_addr);
            ssize_t n = ::recvfrom(fd, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
            if (n < 0) {
                if (errno == EINTR) continue;
                LOG(if (!should_stop(r)) logger::error[r.name] << "recvfrom: " << std::strerror(errno) << std::endl);
                continue;
            }
            buf.resize(static_cast<size_t>(n));
            std::thread(udp_request, fd, r, std::move(buf), client_addr, client_len).detach();
        }
    } catch (const std::exception& e) {
        LOG(logger::error[r.name] << e.what() << std::endl);
        stop_requested = 1;
    }
    close_fd(fd);
}

// ubus `list`: what this daemon is serving right now. Read-only, and it reports
// only ITS OWN redirects - a port published by a container manager is that
// manager's to report, so a UI can show both sources separately instead of
// guessing which is which from a flat list.
void ubus_list(const std::string&, const JSON&, JSON& res) {
    JSON arr = JSON::Array();
    {
        const std::lock_guard<std::mutex> lk(registry_mutex);
        for (const auto& l : registry) {
            const redirect_t& r = l->r;
            JSON o = JSON::Object();
            o["name"] = r.name;
            o["proto"] = r.tcp && r.udp ? "both" : (r.udp ? "udp" : "tcp");
            o["listen_ip"] = r.listen_ip;
            o["listen_port"] = (long long)r.listen_port;
            o["target_ip"] = r.target_ip;
            o["target_port"] = (long long)r.target_port;
            o["connections"] = (long long)r.active_connections->load();
            o["max_connections"] = (long long)r.max_connections;
            o["idle_timeout"] = (long long)r.idle_timeout;
            o["connect_timeout"] = (long long)r.connect_timeout;
            arr.append(o);
        }
    }
    res["redirects"] = arr;
    res["source"] = active_from_config ? "config" : "arguments";
    if (active_from_config) res["config"] = active_config_name;
    res["version"] = APP_VERSION;
}

std::vector<ubus::method> ubus_methods() {
    return {
        { .name = "list", .cb = ubus_list }
    };
}

// Start one redirect's listeners and register it. Main thread only.
void start_listener(const redirect_t& r) {
    auto l = std::make_shared<listener_t>();
    l->r = r;
    if (l->r.tcp) l->threads.emplace_back(tcp_listener, l->r);
    if (l->r.udp) l->threads.emplace_back(udp_listener, l->r);
    const std::lock_guard<std::mutex> lk(registry_mutex);
    registry.push_back(l);
}

// Stop one redirect and wait for its threads. Must NOT be called with
// registry_mutex held: a listener can be mid-log or mid-accept, and joining while
// holding the registry lock would block every ubus read for as long as that takes.
void stop_listener(const std::shared_ptr<listener_t>& l) {
    l->r.stop->store(true);
    for (auto& t : l->threads)
        if (t.joinable()) t.join();
    l->threads.clear();
}

// Stop everything: take the registry away under the lock, then join outside it.
void stop_all_listeners() {
    std::vector<std::shared_ptr<listener_t>> taken;
    {
        const std::lock_guard<std::mutex> lk(registry_mutex);
        taken.swap(registry);
    }
    for (auto& l : taken)
        stop_listener(l);
}

// Load the redirects a fresh start (or a reload) should serve.
std::vector<redirect_t> current_redirects(bool argv_mode, const std::string& config,
                                          const std::vector<std::string>& specs,
                                          int connect_timeout, int idle_timeout,
                                          int udp_timeout, int max_connections) {
    if (argv_mode)
        return load_specs(specs, generation.load(), connect_timeout, idle_timeout, udp_timeout, max_connections);
    auto redirects = load_config(config, generation.load());
    if (redirects.empty())
        throw std::runtime_error("no enabled redirects");
    active_from_config = true;
    active_config_name = config;
    return redirects;
}

void run_redirects(std::vector<redirect_t> redirects) {
    for (const auto& r : redirects)
        start_listener(r);
}


} // namespace

int main(int argc, char** argv) {
    usage_t usage = {
        .args = { argc, argv },
        .info = {
            .name = APP_NAME,
            .version = APP_VERSION,
            .author = "Oskari Rauta / OpenAI Codex",
            .usage = "[options] [<redirect>...]",
            .description = "Simple TCP/UDP redirector. Redirects come from an OpenWrt UCI config, or\n"
                           "directly from the command line as [listen_ip:]listen_port:target_ip:target_port[/proto]\n"
                           "(e.g. 1080:10.102.2.2:80). Command-line redirects replace the config file."
        },
        .options = {
            { "config",  { .key = "c", .word = "config",  .desc = "UCI config name or path (default: tcpredir)", .flag = usage_t::REQUIRED, .name = "file" }},
            { "connect-timeout", { .word = "connect-timeout", .desc = "command-line redirects: connect timeout, seconds (default 10)", .flag = usage_t::REQUIRED, .name = "sec", .type = usage_t::INT }},
            { "idle-timeout",    { .word = "idle-timeout",    .desc = "command-line redirects: idle timeout, seconds (default 300)", .flag = usage_t::REQUIRED, .name = "sec", .type = usage_t::INT }},
            { "udp-timeout",     { .word = "udp-timeout",     .desc = "command-line redirects: udp reply timeout, seconds (default 5)", .flag = usage_t::REQUIRED, .name = "sec", .type = usage_t::INT }},
            { "max-connections", { .word = "max-connections", .desc = "command-line redirects: concurrent connections, 0 = unlimited", .flag = usage_t::REQUIRED, .name = "n", .type = usage_t::INT }},
            { "verbose", { .key = "V", .word = "verbose", .desc = "verbose logging" }},
            { "quiet",   { .key = "q", .word = "quiet",   .desc = "only errors" }},
            { "help",    { .key = "h", .word = "help",    .desc = "show help" }},
            { "version", { .key = "v", .word = "version", .desc = "show version" }}
        }
    };

    if (usage["help"]) {
        std::cout << usage << std::endl << usage.help() << std::endl;
        return 0;
    }
    if (usage["version"]) {
        std::cout << APP_NAME << " " << APP_VERSION << std::endl;
        return 0;
    }
    if (!usage.errors().empty()) {
        std::cerr << usage.errors() << std::endl << usage << std::endl << usage.help() << std::endl;
        return 2;
    }

    logger::prefix = APP_NAME;
    logger::log_level = usage["quiet"] ? logger::error.id() : (usage["verbose"] ? logger::verbose.id() : logger::notice.id());

    std::signal(SIGINT, on_stop_signal);
    std::signal(SIGTERM, on_stop_signal);
    std::signal(SIGHUP, on_reload_signal);
    std::signal(SIGPIPE, SIG_IGN);

    const std::string config = usage["config"] ? usage["config"].stringValue() : "tcpredir";

    // Command-line redirects take over completely: no UCI file is read, and
    // SIGHUP has nothing to re-read, so it only restarts the same listeners.
    const std::vector<std::string> specs = usage.remainder();
    const bool argv_mode = !specs.empty();
    if (argv_mode && usage["config"]) {
        logger::error << "--config and command-line redirects are mutually exclusive" << std::endl;
        return 2;
    }

    const int connect_timeout = usage["connect-timeout"] ? (int)usage["connect-timeout"].intValue() : 10;
    const int idle_timeout    = usage["idle-timeout"]    ? (int)usage["idle-timeout"].intValue()    : 300;
    const int udp_timeout     = usage["udp-timeout"]     ? (int)usage["udp-timeout"].intValue()     : 5;
    const int max_connections = usage["max-connections"] ? (int)usage["max-connections"].intValue() : 0;

    // ubus is optional: tcpredir forwards perfectly well without it, and it may
    // start before ubusd is up (or on a system that has none). A failure to
    // connect or register is a warning, never a reason to stop redirecting.
    //
    // Only the CONFIG-mode daemon registers the object. ubusd accepts duplicate
    // object names without complaint and then routes a call to whichever
    // instance it likes, so if every process registered, `ubus call tcpredir
    // list` would answer from a random one - and a supervisor that starts one
    // tcpredir per job (uxcd publishes a container port that way) would drown
    // out the administrator's own service. Redirects given as arguments belong
    // to whoever started the process; that parent reports them, not us.
    ubus* srv = nullptr;
    if (argv_mode) {
        logger::verbose << "command-line redirects: not registering a ubus object "
                        << "(it belongs to the configured service)" << std::endl;
    } else try {
        srv = new ubus();
        srv->add_object(APP_NAME, ubus_methods());
        logger::info << "serving ubus object '" << APP_NAME << "'" << std::endl;
    } catch (const ubus::exception& e) {
        logger::warning << "ubus unavailable (" << e.what() << ") - redirects run, but cannot be queried" << std::endl;
        delete srv;
        srv = nullptr;
    }

    // One uloop run for the whole lifetime. Reload happens INSIDE it: leaving
    // uloop and re-entering it dropped the ubus socket from its fd set, so the
    // object stayed registered with ubusd while the process no longer answered -
    // every call after the first SIGHUP timed out. Staying in uloop also means a
    // reload only replaces the listeners, which is the same operation the ubus
    // write methods will need.
    try {
        run_redirects(current_redirects(argv_mode, config, specs,
                                        connect_timeout, idle_timeout, udp_timeout, max_connections));
    } catch (const std::exception& e) {
        logger::error << e.what() << std::endl;
        delete srv;
        return 1;
    }

    uloop::task::add([&]() -> int {
        if (stop_requested) { uloop::exit(); return 0; }
        if (reload_requested) {
            reload_requested = 0;
            generation.fetch_add(1);
            logger::notice << ( argv_mode ? "restarting listeners" : "reloading configuration" ) << std::endl;
            stop_all_listeners();
            try {
                run_redirects(current_redirects(argv_mode, config, specs,
                                                connect_timeout, idle_timeout, udp_timeout, max_connections));
            } catch (const std::exception& e) {
                // A bad edit must not take the daemon down with it: say so and keep
                // running with nothing served, so fixing the file and sending
                // another HUP is all it takes to recover.
                logger::error << "reload failed: " << e.what() << std::endl;
            }
        }
        return 200;
    }, 200);
    uloop::run();

    stop_all_listeners();

    logger::notice << "stopped" << std::endl;
    delete srv;
    return 0;
}

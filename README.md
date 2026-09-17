[![License:MIT](https://img.shields.io/badge/License-MIT-blue?style=plastic)](LICENSE) [![CI build](https://img.shields.io/github/actions/workflow/status/oskarirauta/tcpredir/build.yml?style=plastic&label=build)](https://github.com/oskarirauta/tcpredir/actions/workflows/build.yml)

# tcpredir

`tcpredir` is a small TCP/UDP connection redirector written in C++17 for OpenWrt environments. It reads its settings from a UCI-style configuration file and can forward, for example, local port `1080` to `10.0.0.99:80`.

TCP is the primary use case. UDP support is included as a simple request/reply forwarder.

Finnish documentation is available in [`README.fi.md`](README.fi.md).

## Features

- Multiple simultaneous `redirect` rules from the same UCI configuration.
- Bidirectional TCP tunneling.
- Simple UDP request/reply forwarding.
- IPv4/IPv6 name resolution through `getaddrinfo()`.
- UCI configuration parsing with `uci_cpp`.
- Logging with `logger_cpp`.
- Help/version/argument handling with `usage_cpp`.
- No dependency on the netlink library.

## Building

```sh
make
```

Clean build artifacts:

```sh
make clean
```

By default, the build uses the `CXX` compiler from the environment or the Makefile default. For OpenWrt cross-compilation, provide the compiler through an environment variable:

```sh
make CXX=mips-openwrt-linux-musl-g++
```

Alternatively, use the toolchain variables exported by the OpenWrt SDK.

## Usage

```sh
./tcpredir [options]
```

Options:

```text
-c, --config <file>   UCI configuration name or path, default: tcpredir
-V, --verbose         Verbose logging
-q, --quiet           Errors only
-h, --help            Show help
-v, --version         Show version
```

If `--config` is a plain name, such as `tcpredir`, the UCI library resolves it in the OpenWrt style as `/etc/config/tcpredir`. If the value contains `/`, it is used as a path as-is.

Example:

```sh
./tcpredir -c /etc/config/tcpredir
```

## Configuration

An example configuration is also provided in [`tcpredir.uci.example`](tcpredir.uci.example).

```uci
config redirect 'web2'
        option enabled '1'
        option proto 'tcp'
        option listen_ip '0.0.0.0'
        option listen_port '1080'
        option target_ip '10.0.0.99'
        option target_port '80'
```

### Options

| Option | Required | Default | Description |
|---|---:|---|---|
| `enabled` | no | `1` | Disable the rule with `0`, `false`, `off` or `disabled`. |
| `proto` / `protocol` | no | `tcp` | `tcp`, `udp` or `both`. |
| `listen_ip` / `listen_addr` | no | `0.0.0.0` | Local address to bind to. |
| `listen_port` | yes | - | Local listening port. |
| `target_ip` / `target_addr` | yes | - | Target address. |
| `target_port` | yes | - | Target port. |

Multiple rules can be configured by adding several `config redirect` sections:

```uci
config redirect 'web'
        option proto 'tcp'
        option listen_port '1080'
        option target_ip '10.0.0.99'
        option target_port '80'

config redirect 'ssh'
        option proto 'tcp'
        option listen_port '2222'
        option target_ip '10.0.0.99'
        option target_port '22'
```

UDP example:

```uci
config redirect 'dns'
        option proto 'udp'
        option listen_port '1053'
        option target_ip '1.1.1.1'
        option target_port '53'
```

## OpenWrt installation sketch

Copy the binary and configuration to the device:

```sh
scp tcpredir root@router:/usr/sbin/tcpredir
scp tcpredir.uci.example root@router:/etc/config/tcpredir
```

Start it manually for testing:

```sh
ssh root@router /usr/sbin/tcpredir -c tcpredir
```

A proper OpenWrt package and init script should be maintained in an OpenWrt package feed if the application is to be installed with `opkg` and managed through `/etc/init.d/tcpredir`.

## Limitations

- Each TCP connection gets its own thread. This is simple and sufficient as a starting point for OpenWrt use, but a poll/epoll-based event loop would be better for very large connection counts.
- UDP support is stateless request/reply forwarding: for each datagram, a target socket is opened, a reply is waited for briefly, and the reply is forwarded back to the original sender. This is suitable for simple DNS-like use cases, but it is not a full UDP NAT/state-table implementation.
- The program does not daemonize itself and does not provide a pidfile.

## Development ideas

Useful future additions could include:

1. OpenWrt package directory and init script.
2. Daemon/pidfile support or procd integration.
3. Per-rule timeout and buffer settings.
4. Per-rule connection limits.
5. A better UDP state table if UDP should become production-grade generic tunneling.
6. Automated integration tests in the Makefile.

## License

See [`LICENSE`](LICENSE).

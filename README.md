[![License:MIT](https://img.shields.io/badge/License-MIT-blue?style=plastic)](LICENSE) [![CI build](https://img.shields.io/github/actions/workflow/status/oskarirauta/tcpredir/build.yml?style=plastic&label=build)](https://github.com/oskarirauta/tcpredir/actions/workflows/build.yml)

# tcpredir

`tcpredir` is a small TCP/UDP connection redirector written in C++17 for OpenWrt environments. It reads its settings from a UCI-style configuration file and can forward, for example, local port `1080` to `10.0.0.99:80`.

TCP is the primary use case. UDP support is included as a simple request/reply forwarder.

## Features

- Multiple simultaneous `redirect` rules from the same UCI configuration.
- Redirects can also be given directly as command-line arguments, with no configuration file.
- Bidirectional TCP tunneling.
- Simple UDP request/reply forwarding.
- IPv4/IPv6 name resolution through `getaddrinfo()`.
- UCI configuration parsing with `uci_cpp`.
- Logging with `logger_cpp`.
- A **ubus** interface: query the live redirects, and add, remove or reload them at runtime.
- A **LuCI application** for editing redirects and watching them run.
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
./tcpredir [options] [<redirect>...]
```

Options:

```text
-c, --config <file>       UCI configuration name or path, default: tcpredir
    --connect-timeout <s> command-line redirects: connect timeout (default 10)
    --idle-timeout <s>    command-line redirects: idle timeout (default 300)
    --udp-timeout <s>     command-line redirects: UDP reply timeout (default 5)
    --max-connections <n> command-line redirects: concurrent connections, 0 = unlimited
-V, --verbose             Verbose logging
-q, --quiet               Errors only
-h, --help                Show help
-v, --version             Show version
```

If `--config` is a plain name, such as `tcpredir`, the UCI library resolves it in the OpenWrt style as `/etc/config/tcpredir`. If the value contains `/`, it is used as a path as-is.

Example:

```sh
./tcpredir -c /etc/config/tcpredir
```

### Redirects on the command line

Redirects can also be given as arguments, in which case no configuration file is read:

```sh
./tcpredir 1080:10.0.0.99:80
./tcpredir 127.0.0.1:8443:10.0.0.99:443/tcp 1053:1.1.1.1:53/udp
```

The format is:

```text
[listen_ip:]listen_port:target_ip:target_port[/proto]
```

`listen_ip` defaults to `0.0.0.0` and `proto` to `tcp`. IPv6 literals must be bracketed so their colons are not read as field separators:

```sh
./tcpredir '[::1]:1080:[fd00::2]:80'
```

This mode exists for a supervisor that already knows the address and port it wants published - a container manager, for instance - so it can start a redirector without generating a configuration file that two programs would then both own. `--config` and command-line redirects are mutually exclusive. `SIGHUP` has nothing to re-read here, so it simply restarts the listeners.

**A redirect is a userspace proxy, not a firewall rule.** The connection to the target is opened by `tcpredir`, so the target sees *its* address as the client - not the original one. Services that log client addresses or make decisions based on them (access rules, rate limits, geolocation) will see the redirector instead. Where that matters, use a firewall redirect (`fw4` / nftables DNAT), which rewrites the packet and preserves the source address, or a protocol that carries the original address such as PROXY protocol.

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

## The ubus interface

When redirects come from a configuration file, `tcpredir` registers a `tcpredir`
object on ubus and answers `list` with what it is serving *right now* - including
the live connection count, which the configuration file cannot tell you:

```sh
ubus call tcpredir list
```

```json
{
	"redirects": [
		{
			"name": "web",
			"proto": "tcp",
			"listen_ip": "127.0.0.1",
			"listen_port": 18090,
			"target_ip": "10.0.0.99",
			"target_port": 80,
			"connections": 2,
			"max_connections": 0,
			"idle_timeout": 300,
			"connect_timeout": 10
		}
	],
	"source": "config",
	"config": "tcpredir",
	"version": "1.1.1"
}
```

ubus is **optional**: failing to reach `ubusd` or to register is logged as a
warning and nothing else. Redirecting works the same without it, which matters
because `tcpredir` may well start before `ubusd`, or run on a system that has no
ubus at all.

### Changing redirects at runtime

```sh
ubus call tcpredir add '{"redirect":"127.0.0.1:8080:10.0.0.99:80"}'
ubus call tcpredir remove '{"name":"8080->10.0.0.99:80"}'
ubus call tcpredir reload
```

`add` takes a redirect in the same form as the command line and starts it
immediately; `remove` stops one by the `name` that `list` reports. Neither
disturbs the other redirects, and connections already in flight through them are
untouched.

Redirects added this way are **not persistent**. They are gone when the daemon
restarts, and the configuration file remains the only description of its steady
state. That is deliberate: nothing owns a lease and nothing expires, so this
interface needs no ownership tracking or garbage collection to stay correct.

`reload` re-reads the configuration file. It replaces exactly the redirects that
came **from that file** - stopping and restarting them even if nothing changed,
because you asked it to - and leaves runtime redirects alone. A container manager
publishing a port has nothing to do with the administrator editing
`/etc/config/tcpredir`, and must not lose its redirect because they did.
`SIGHUP` does the same thing.

For the same reason, `remove` refuses a redirect that came from the file: it
would return on the next reload, and quietly serving something different from
what the file says is worse than saying no. `list` reports `"source"` per
redirect (`config` or `runtime`) so a user interface can tell them apart.

### Only the configured service registers

A `tcpredir` started with redirects **as arguments** does not register the
object. This is deliberate. `ubusd` accepts duplicate object names without
complaint and then routes a call to whichever instance it likes, so if every
process registered, `ubus call tcpredir list` would answer from a random one -
and a supervisor that starts one `tcpredir` per job would drown out the
administrator's own service.

The rule that follows is simple: **redirects given as arguments belong to
whoever started the process, and that parent reports them.** uxcd, for instance,
publishes a container's port by running `tcpredir` as its own supervised child,
and reports it through `ubus call uxcd list` / `info`. A user interface showing
"all redirects on this box" therefore reads two sources and labels them, rather
than trying to work out from one flat list which redirect came from where.

## OpenWrt package

`openwrt/Makefile` builds two packages, and is meant to be dropped into a feed
(or copied into `package/tcpredir/` in an OpenWrt tree):

* **`tcpredir`** - the daemon, the procd init script, and an empty
  `/etc/config/tcpredir` marked as a conffile so your redirects survive a
  sysupgrade. The shipped configuration is deliberately empty: a redirect starts
  listening the moment the service reads it, so installing a package must not
  create one. `tcpredir.uci.example` has a filled-in section to copy from.
* **`luci-app-tcpredir`** - the web interface below, `PKGARCH:=all`, depending on
  `luci-base` and `tcpredir`.

`PKG_HASH:=skip` and the release tarball URL assume the repository's release
workflow, which builds the tarball recursively so the vendored C++ libraries
travel with it.

For testing without a package, copy the pieces to the device by hand:

```sh
scp tcpredir root@router:/usr/sbin/tcpredir
scp tcpredir.uci.example root@router:/etc/config/tcpredir
scp openwrt.init root@router:/etc/init.d/tcpredir
ssh root@router '/etc/init.d/tcpredir enable; /etc/init.d/tcpredir start'
```

## The LuCI application

`luci/app-tcpredir/` holds a LuCI interface with two pages under
**Network -> Port Redirects**:

* **Redirects** edits `/etc/config/tcpredir`. Saving applies the file by calling
  `reload` rather than restarting the service, so redirects that were added at
  runtime keep serving and established connections are not dropped.
* **Status** shows what is actually running, refreshed every few seconds, and can
  add or remove runtime redirects. Each row says where it came from, because the
  first thing a reader needs to know is where to go to change it.

The Status page also lists the ports published by [uxcd](https://github.com/oskarirauta/uxcd)
containers, read-only and in a section of their own. Those are forwarded by
separate `tcpredir` processes that uxcd starts and supervises; this daemon does
not own them and removing one here would only have it return the next time the
container started. If uxcd is not installed the section is simply absent.

Install `luci-app-tcpredir`, or place the files by hand for testing:

```sh
scp -r luci/app-tcpredir/htdocs/* root@router:/www/
scp -r luci/app-tcpredir/root/*   root@router:/
ssh root@router 'rm -f /tmp/luci-indexcache*; /etc/init.d/rpcd restart'
```

## Limitations

- Each TCP connection gets its own thread. This is simple and sufficient as a starting point for OpenWrt use, but a poll/epoll-based event loop would be better for very large connection counts.
- UDP support is stateless request/reply forwarding: for each datagram, a target socket is opened, a reply is waited for briefly, and the reply is forwarded back to the original sender. This is suitable for simple DNS-like use cases, but it is not a full UDP NAT/state-table implementation.
- The program does not daemonize itself and does not provide a pidfile; procd supervises it instead.

## Development ideas

Useful future additions could include:

1. Per-rule buffer settings.
2. A better UDP state table if UDP should become production-grade generic tunneling.
3. Automated integration tests in the Makefile.

## License

See [`LICENSE`](LICENSE).

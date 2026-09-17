# tcpredir

`tcpredir` on pieni C++17:lla toteutettu TCP/UDP-yhteyksien uudelleenohjaaja OpenWrt-ympäristöön. Ohjelma lukee asetukset UCI-tyylisestä konfiguraatiosta ja voi ohjata esimerkiksi paikallisen portin `1080` laitteelle `10.0.0.99:80`.

TCP on ohjelman ensisijainen käyttötapa. UDP-tuki on mukana yksinkertaisena request/reply-forwarderina.

## Ominaisuudet

- Useita samanaikaisia `redirect`-sääntöjä samasta UCI-konfiguraatiosta.
- TCP-tunnelointi molempiin suuntiin.
- Yksinkertainen UDP request/reply -välitys.
- IPv4/IPv6-nimiresoluutio `getaddrinfo()`-rajapinnalla.
- UCI-konfiguraation luku `uci_cpp`-kirjastolla.
- Lokitus `logger_cpp`-kirjastolla.
- Help/version/argumenttien käsittely `usage_cpp`-kirjastolla.
- Ei riippuvuutta netlink-kirjastoon.

## Kääntäminen

```sh
make
```

Siivous:

```sh
make clean
```

Oletuksena käytetään ympäristöstä löytyvää `CXX`-kääntäjää tai Makefilen oletusta. OpenWrt-ristikäännössä voit antaa kääntäjän ympäristömuuttujalla:

```sh
make CXX=mips-openwrt-linux-musl-g++
```

Tai käytä OpenWrt SDK:n exporttaamia toolchain-muuttujia.

## Käyttö

```sh
./tcpredir [options]
```

Optiot:

```text
-c, --config <file>   UCI-konfiguraation nimi tai polku, oletus: tcpredir
-V, --verbose         Verbose-lokitus
-q, --quiet           Vain virheet
-h, --help            Näytä ohje
-v, --version         Näytä versio
```

Jos `--config` on pelkkä nimi, esimerkiksi `tcpredir`, UCI-kirjasto etsii tiedoston OpenWrt-tyyliin `/etc/config/tcpredir`. Jos arvossa on `/`, sitä käytetään polkuna sellaisenaan.

Esimerkki:

```sh
./tcpredir -c /etc/config/tcpredir
```

## Konfiguraatio

Esimerkkitiedosto löytyy myös tiedostosta [`tcpredir.uci.example`](tcpredir.uci.example).

```uci
config redirect 'web2'
        option enabled '1'
        option proto 'tcp'
        option listen_ip '0.0.0.0'
        option listen_port '1080'
        option target_ip '10.0.0.99'
        option target_port '80'
```

### Asetukset

| Optio | Pakollinen | Oletus | Kuvaus |
|---|---:|---|---|
| `enabled` | ei | `1` | Säännön voi poistaa käytöstä arvolla `0`, `false`, `off` tai `disabled`. |
| `proto` / `protocol` | ei | `tcp` | `tcp`, `udp` tai `both`. |
| `listen_ip` / `listen_addr` | ei | `0.0.0.0` | Paikallinen osoite johon bindataan. |
| `listen_port` | kyllä | - | Paikallinen kuunteluportti. |
| `target_ip` / `target_addr` | kyllä | - | Kohdeosoite. |
| `target_port` | kyllä | - | Kohdeportti. |

Useampi sääntö onnistuu lisäämällä useita `config redirect` -osioita:

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

UDP-esimerkki:

```uci
config redirect 'dns'
        option proto 'udp'
        option listen_port '1053'
        option target_ip '1.1.1.1'
        option target_port '53'
```

## OpenWrt-asennusluonnos

Kopioi binääri ja konfiguraatio laitteelle:

```sh
scp tcpredir root@router:/usr/sbin/tcpredir
scp tcpredir.uci.example root@router:/etc/config/tcpredir
```

Käynnistys testinä:

```sh
ssh root@router /usr/sbin/tcpredir -c tcpredir
```

Varsinainen OpenWrt-paketti ja init-skripti kannattaa lisätä myöhemmin, jos ohjelma halutaan asentaa `opkg`:llä ja hallita `/etc/init.d/tcpredir` kautta.

## Rajoitukset

- TCP-yhteydelle luodaan oma säie. Tämä on yksinkertainen ja OpenWrt-käyttöön riittävä lähtökohta, mutta erittäin suurella yhteysmäärällä poll/epoll-pohjainen event loop olisi parempi.
- UDP-tuki on stateless request/reply -mallinen: jokaiselle datagrammille avataan kohde-socket, odotetaan vastausta hetki ja välitetään vastaus takaisin alkuperäiselle lähettäjälle. Tämä sopii esimerkiksi yksinkertaisiin DNS-tyyppisiin käyttötapauksiin, mutta ei korvaa täyttä UDP-NAT/state-taulua.
- Ohjelma ei vielä daemonisoi itseään eikä tarjoa pidfileä.

## Kehityssuunnat

Hyödyllisimmät seuraavat lisäykset olisivat:

1. OpenWrt-pakettihakemisto ja init-skripti.
2. Daemon/pidfile-tuki tai procd-integraatio.
3. Per-sääntöiset timeoutit ja buffer-asetukset.
4. Yhteysmäärän rajoitus per sääntö.
5. Parempi UDP-state-taulu, jos UDP:stä halutaan tuotantokelpoinen yleiskäyttöinen tunnelointi.
6. Automaattiset integraatiotestit Makefileen.

## Lisenssi

Katso [`LICENSE`](LICENSE).

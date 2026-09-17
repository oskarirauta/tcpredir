# tcpredir

`tcpredir` on pieni C++17:lla toteutettu TCP/UDP-yhteyksien uudelleenohjaaja OpenWrt-ympäristöön. Ohjelma lukee asetukset UCI-tyylisestä konfiguraatiosta ja voi ohjata esimerkiksi paikallisen portin `1080` laitteelle `10.0.0.99:80`.

TCP on ohjelman ensisijainen käyttötapa. UDP-tuki on mukana yksinkertaisena request/reply-forwarderina.

## Ominaisuudet

- Useita samanaikaisia `redirect`-sääntöjä samasta UCI-konfiguraatiosta.
- TCP-tunnelointi molempiin suuntiin.
- Per-sääntöiset timeoutit ja yhteysmäärän rajoitus.
- SIGHUP-konfiguraatioreload.
- OpenWrt `procd` init-skripti.
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
        option connect_timeout '10'
        option idle_timeout '300'
        option max_connections '128'
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
| `connect_timeout` | ei | `10` | TCP-kohdeyhteyden muodostuksen timeout sekunteina. |
| `idle_timeout` | ei | `300` | TCP-yhteyden inaktiivisuustimeout sekunteina. |
| `udp_timeout` | ei | `5` | UDP-vastauksen odotusaika sekunteina. |
| `max_connections` | ei | `0` | Samanaikaisten TCP-yhteyksien raja per sääntö. `0` = ei rajaa. |

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

## Reload

Prosessi lukee konfiguraation uudelleen `SIGHUP`-signaalilla:

```sh
kill -HUP $(pidof tcpredir)
```

Reload sulkee kuuntelusocketit ja käynnistää ne uudella konfiguraatiolla. Olemassa olevat yhteydet päättyvät reloadin yhteydessä viimeistään seuraavan poll-herätyksen aikana.

## OpenWrt procd/init

Repossa on esimerkkiscripti [`openwrt.init`](openwrt.init). Se ei ole pakettireseptin korvike, vaan suoraan softan mukana pidettävä ajonaikainen init-esimerkki.

Asennus käsin:

```sh
cp openwrt.init /etc/init.d/tcpredir
chmod +x /etc/init.d/tcpredir
/etc/init.d/tcpredir enable
/etc/init.d/tcpredir start
```

Reload OpenWrt:ssä:

```sh
/etc/init.d/tcpredir reload
```

`procd` seuraa `/etc/config/tcpredir`-tiedostoa ja reload-trigger on määritelty init-skriptissä.

## OpenWrt-asennusluonnos

Kopioi binääri ja konfiguraatio laitteelle:

```sh
scp tcpredir root@router:/usr/sbin/tcpredir
scp tcpredir.uci.example root@router:/etc/config/tcpredir
scp openwrt.init root@router:/etc/init.d/tcpredir
```

Käynnistys testinä:

```sh
ssh root@router /usr/sbin/tcpredir -c tcpredir
```

OpenWrt-pakettiresepti kuuluu erilliseen pakettirepoon. Tämä repository sisältää vain ohjelman, esimerkkikonfiguraation ja procd-init-skriptin.

## Rajoitukset

- TCP-yhteydelle luodaan oma säie. Tämä on yksinkertainen ja OpenWrt-käyttöön riittävä lähtökohta, mutta erittäin suurella yhteysmäärällä poll/epoll-pohjainen event loop olisi parempi.
- UDP-tuki on stateless request/reply -mallinen: jokaiselle datagrammille avataan kohde-socket, odotetaan vastausta hetki ja välitetään vastaus takaisin alkuperäiselle lähettäjälle. Tämä sopii esimerkiksi yksinkertaisiin DNS-tyyppisiin käyttötapauksiin, mutta ei korvaa täyttä UDP-NAT/state-taulua.
- Ohjelma ei daemonisoi itseään eikä tarjoa pidfileä. OpenWrt:ssä prosessin valvonta on tarkoitus tehdä `procd`:llä.

## Kehityssuunnat

Mahdollisia myöhempiä lisäyksiä:

1. Parempi UDP-state-taulu, jos UDP:stä halutaan tuotantokelpoinen yleiskäyttöinen tunnelointi.
2. Automaattiset integraatiotestit Makefileen tai erilliseen testiscriptiin.
3. Bufferikokojen säätö konfiguraatiosta.
4. Valinnainen käyttäjän vaihto käynnistyksen jälkeen, jos prosessi aloitetaan rootina.

## Lisenssi

Katso [`LICENSE`](LICENSE).

[![License:MIT](https://img.shields.io/badge/License-MIT-blue?style=plastic)](LICENSE) [![CI build](https://img.shields.io/github/actions/workflow/status/oskarirauta/tcpredir/build.yml?style=plastic&label=build)](https://github.com/oskarirauta/tcpredir/actions/workflows/build.yml)

# tcpredir

`tcpredir` on pieni C++17:lla toteutettu TCP/UDP-yhteyksien uudelleenohjaaja OpenWrt-ympäristöön. Ohjelma lukee asetukset UCI-tyylisestä konfiguraatiosta ja voi ohjata esimerkiksi paikallisen portin `1080` laitteelle `10.0.0.99:80`.

TCP on ohjelman ensisijainen käyttötapa. UDP-tuki on mukana yksinkertaisena request/reply-forwarderina.

## Ominaisuudet

- Useita samanaikaisia `redirect`-sääntöjä samasta UCI-konfiguraatiosta.
- Ohjaukset voi antaa myös suoraan komentoriviargumentteina, ilman konfiguraatiotiedostoa.
- TCP-tunnelointi molempiin suuntiin.
- Yksinkertainen UDP request/reply -välitys.
- IPv4/IPv6-nimiresoluutio `getaddrinfo()`-rajapinnalla.
- UCI-konfiguraation luku `uci_cpp`-kirjastolla.
- Lokitus `logger_cpp`-kirjastolla.
- Vain luettava **ubus**-rajapinta (`tcpredir.list`) voimassa olevien ohjausten kyselyyn.
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
./tcpredir [options] [<redirect>...]
```

Optiot:

```text
-c, --config <file>       UCI-konfiguraation nimi tai polku, oletus: tcpredir
    --connect-timeout <s> komentoriviohjaukset: yhteyden aikakatkaisu (oletus 10)
    --idle-timeout <s>    komentoriviohjaukset: jouten-aikakatkaisu (oletus 300)
    --udp-timeout <s>     komentoriviohjaukset: UDP-vastauksen aikakatkaisu (oletus 5)
    --max-connections <n> komentoriviohjaukset: rinnakkaiset yhteydet, 0 = rajoittamaton
-V, --verbose             Verbose-lokitus
-q, --quiet               Vain virheet
-h, --help                Näytä ohje
-v, --version             Näytä versio
```

Jos `--config` on pelkkä nimi, esimerkiksi `tcpredir`, UCI-kirjasto etsii tiedoston OpenWrt-tyyliin `/etc/config/tcpredir`. Jos arvossa on `/`, sitä käytetään polkuna sellaisenaan.

Esimerkki:

```sh
./tcpredir -c /etc/config/tcpredir
```

### Ohjaukset komentoriviltä

Ohjaukset voi antaa myös argumentteina, jolloin konfiguraatiotiedostoa ei lueta lainkaan:

```sh
./tcpredir 1080:10.0.0.99:80
./tcpredir 127.0.0.1:8443:10.0.0.99:443/tcp 1053:1.1.1.1:53/udp
```

Muoto on:

```text
[listen_ip:]listen_port:target_ip:target_port[/proto]
```

`listen_ip` on oletuksena `0.0.0.0` ja `proto` on oletuksena `tcp`. IPv6-osoitteet on kirjoitettava hakasulkeisiin, jotta niiden kaksoispisteitä ei tulkita kenttäerottimiksi:

```sh
./tcpredir '[::1]:1080:[fd00::2]:80'
```

Tämä tila on tarkoitettu valvojalle, joka jo tietää osoitteen ja portin jonka haluaa julkaista – esimerkiksi konttien hallintaohjelmalle – jotta se voi käynnistää ohjauksen ilman generoitua konfiguraatiotiedostoa, jota kaksi ohjelmaa sitten omistaisi. `--config` ja komentoriviohjaukset ovat toisensa poissulkevia. `SIGHUP`:lla ei ole tässä tilassa mitään luettavaa uudelleen, joten se vain käynnistää kuuntelijat uudelleen.

**Ohjaus on userspace-proxy, ei palomuurisääntö.** `tcpredir` avaa yhteyden kohteeseen itse, joten kohde näkee asiakkaana *sen* osoitteen – ei alkuperäistä. Palvelut jotka kirjaavat asiakkaiden osoitteita tai tekevät niiden perusteella päätöksiä (pääsysäännöt, rajoitukset, paikannus) näkevät ohjaimen. Kun sillä on merkitystä, käytä palomuurin ohjausta (`fw4` / nftables DNAT), joka kirjoittaa paketin uudelleen ja säilyttää lähdeosoitteen, tai protokollaa joka kuljettaa alkuperäisen osoitteen mukanaan (PROXY protocol).

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

## ubus-rajapinta

Kun ohjaukset tulevat konfiguraatiotiedostosta, `tcpredir` rekisteröi ubusiin
`tcpredir`-objektin ja vastaa `list`-kutsuun sillä mitä se juuri nyt palvelee –
mukaan lukien elossa olevien yhteyksien määrä, jota konfiguraatiotiedostosta ei
näe:

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

ubus on **valinnainen**: jos `ubusd`:hen ei saada yhteyttä tai rekisteröinti ei
onnistu, siitä kirjataan varoitus eikä muuta. Ohjaukset toimivat täysin ilman –
mikä on olennaista, koska `tcpredir` voi hyvin käynnistyä ennen `ubusd`:tä tai
järjestelmässä jossa ubusia ei ole lainkaan.

### Vain konfiguroitu palvelu rekisteröi

Argumenteilla käynnistetty `tcpredir` **ei** rekisteröi objektia. Tämä on
tarkoituksellista. `ubusd` hyväksyy päällekkäiset objektinimet valittamatta ja
reitittää kutsun sitten jollekin instanssille – jos jokainen prosessi
rekisteröisi, `ubus call tcpredir list` vastaisi sattumanvaraisesti yhdestä
niistä, ja valvoja joka käynnistää yhden `tcpredir`:in per tehtävä hukuttaisi
ylläpitäjän oman palvelun alleen.

Sääntö joka tästä seuraa on yksinkertainen: **argumentteina annetut ohjaukset
kuuluvat sille joka prosessin käynnisti, ja se raportoi ne.** uxcd esimerkiksi
julkaisee kontin portin ajamalla `tcpredir`:iä omana valvottuna lapsenaan ja
raportoi sen `ubus call uxcd list` / `info` -kutsuilla. Käyttöliittymä joka
näyttää "kaikki laatikon ohjaukset" lukee siis kahta lähdettä ja merkitsee ne –
sen sijaan että yrittäisi päätellä yhdestä listasta mikä tuli mistäkin.

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
7. Kirjoittavat ubus-metodit (`add`, `remove`, `reload`), jotta LuCI-sovellus voi
   muuttaa ohjauksia ajon aikana. Ne kannattaa pitää **ei-persistoivina** – vain
   konfiguraatiotiedoston ohjaukset säilyvät uudelleenkäynnistyksen yli – jolloin
   ei tarvita omistajuuden seurantaa eikä vanhenemislogiikkaa, joihin tämän
   tyyppinen rajapinta yleensä mutkistuu.

## Lisenssi

Katso [`LICENSE`](LICENSE).

# dnsbench

[English](README.md) | [Русский](README_RU.md)

`dnsbench` is a cross-platform command-line benchmark for comparing DNS
resolver latency and reliability. It runs on Linux and native 64-bit Windows,
supports IPv4 and IPv6 resolver addresses, downloads live country lists,
accepts local lists, and can export sorted results to CSV.

## Features

- Native C with no third-party C libraries.
- One source file for Linux and Windows.
- Direct UDP DNS queries rather than timing `nslookup` or PowerShell.
- IPv4 and IPv6 resolvers; `A` and `AAAA` queries.
- Single, warmed-cache, and unique-name scenarios.
- Concurrent queries in bounded batches of up to 256 resolvers.
- Per-resolver averages, response counts, ranking, and optional CSV.

## Data source

Country names and codes are read from the Wirewiki
[DNS server index by country](https://www.wirewiki.com/dns-servers/by-country).
Resolver addresses are downloaded at runtime from Wirewiki endpoints of this
form:

```text
https://www.wirewiki.com/dns-servers/{country-code}.txt
```

For example, Kazakhstan comes from
[`kz.txt`](https://www.wirewiki.com/dns-servers/kz.txt).

The lists are not bundled with this repository. This project is independent of
and is not endorsed by Wirewiki. Availability, classification, accuracy, and
permission to use an individual resolver are controlled by third parties and
are not guaranteed by this project. Use moderate settings and comply with
applicable network policies.

## Test methodology

Every run performs three measured scenarios:

1. **Single** — one query for the selected name.
2. **Cached** — one unmeasured warm-up followed by `-n` queries for the same
   name.
3. **Uncached** — `-n` queries using unique names below the selected name.

The warm-up is excluded from statistics. With `-n 5`, each resolver receives
11 measured queries plus one warm-up query.

`Uncached` means that the exact generated name should not have a cached final
answer. It cannot guarantee a completely cold recursive lookup: delegation,
negative answers, and related records may remain cached.

The default query name is `example.com`, an IANA-reserved example domain. Use
`-q` to change it.

Resolvers are processed concurrently in batches of at most 256. This avoids
Linux file-descriptor limits and bounds simultaneous Windows sockets. A rough
worst-case duration in seconds is:

```text
ceil(resolver_count / 256) * (2 * n + 2) * timeout_ms / 1000
```

Execution may finish earlier when every resolver in a batch answers or fails
before the timeout.

## Requirements

- GCC, Clang, or MinGW-w64.
- GNU Make.
- `curl` in `PATH` for Wirewiki modes. Local lists do not require `curl`.

MinGW-w64 is needed only to build `dnsbench.exe`; users of a prebuilt binary do
not need the compiler, WSL, or Cygwin.

## Prebuilt Windows executable

The repository includes a ready-to-run 64-bit [`dnsbench.exe`](dnsbench.exe)
for users who do not want to install a compiler. Download it together with
[`SHA256SUMS`](SHA256SUMS) and verify it in PowerShell:

```powershell
Get-FileHash .\dnsbench.exe -Algorithm SHA256
```

The resulting hash must match the `dnsbench.exe` entry in `SHA256SUMS`.
The Makefile fixes `SOURCE_DATE_EPOCH` to remove build timestamps. Exact binary
reproducibility still requires the same compiler and linker versions; the
published checksum verifies the executable stored in this repository.

## Build

Linux:

```sh
make
```

Windows with MinGW-w64 and GNU Make:

```powershell
make
# or, depending on the installation:
mingw32-make
```

The Makefile detects `OS=Windows_NT`, links Winsock, and produces
`dnsbench.exe`.

Cross-compile Windows x64 on Ubuntu or Debian:

```sh
sudo apt-get update
sudo apt-get install gcc-mingw-w64-x86-64
make windows
```

Clean generated binaries with `make clean`.

## Quick start

Linux:

```sh
./dnsbench -L
./dnsbench kz
./dnsbench -n 5 -t 150 kz
./dnsbench -n 5 -t 150 -o fr.csv fr
```

Windows PowerShell:

```powershell
.\dnsbench.exe -L
.\dnsbench.exe kz
.\dnsbench.exe -n 5 -t 150 kz
.\dnsbench.exe -n 5 -t 150 -o fr.csv fr
```

Options may appear before or after the country code.

## Options

```text
Usage: dnsbench [options] SERVERS_FILE
       dnsbench [options] -c COUNTRY_CODE
       dnsbench [options] COUNTRY_CODE
       dnsbench -L

  -q NAME       query name (default: example.com)
  -n COUNT      requests in each series (default: 5)
  -t MS         timeout per request in milliseconds (default: 1000)
  -T TYPE       query type: A or AAAA (default: A)
  -p PORT       DNS destination port (default: 53)
  -o FILE       write sorted results to CSV
  -c CODE       download and test a Wirewiki country list
  -L            list countries found on Wirewiki
  -h            show help
```

`dnsbench kz` is equivalent to `dnsbench -c kz`. Do not confuse `-n` and
`-t`: `-n 150` sends 150 requests in each series, while `-t 150` sets a 150 ms
timeout.

## Reading the output

```text
Rank | DNS server      |    Single |    Cached |  Uncached |   Average |    OK/All
1    | 103.116.198.226 |     30.47 |     31.25 |     32.05 |     31.25 |   11/11
```

- **Single**, **Cached**, and **Uncached** are scenario averages in ms.
- **Average** is the mean across all successful measured queries.
- **OK/All** is valid replies divided by measured requests.
- A dash means that the scenario produced no valid reply.

Results are ranked by overall average among responding resolvers. A low average
with poor `OK/All` is not necessarily useful; consider reliability together
with latency.

## CSV

CSV is created only when `-o FILE` is supplied:

```sh
./dnsbench -n 10 -t 500 -o kz.csv kz
```

Columns:

```text
rank,dns_server,single_ms,cached_ms,uncached_ms,average_ms,successful,total
```

Missing timings are empty. Existing output files are overwritten.

## Local resolver lists

Pass a text file instead of a country code. Each line contains one literal IPv4
or IPv6 address and an optional display name. Blank lines and text after `#`
are ignored.

```text
# Public resolvers
1.1.1.1                 Cloudflare
8.8.8.8                 Google
2606:4700:4700::1111    Cloudflare IPv6
```

```sh
./dnsbench servers.example.txt
./dnsbench -n 10 -t 500 -o results.csv servers.example.txt
```

Only the IP address is currently shown in terminal and CSV results.

## Exit codes

- `0` — at least one resolver answered.
- `1` — argument, download, file, or local system error.
- `2` — no resolver answered.

## Platform implementation

DNS construction, scenarios, statistics, sorting, and CSV are shared. Linux
uses POSIX sockets, `poll`, and `clock_gettime`; Windows uses Winsock 2,
`WSAPoll`, and `QueryPerformanceCounter`.

Equivalent packets do not guarantee identical timings across systems: routes,
VPNs, firewalls, security software, system load, and cache state may differ.

## Repository layout

```text
dnsbench.c             Application source
Makefile               Linux, Windows, and cross-build targets
dnsbench.exe           Prebuilt Windows x64 executable
SHA256SUMS             Checksum for the prebuilt executable
servers.example.txt    Example local list
README.md              English documentation
README_RU.md           Russian documentation
```

The prebuilt Windows executable is intentionally tracked for convenient direct
downloads. The generated Linux binary, CSV results, and screenshots are
excluded by `.gitignore`. Release assets may additionally be attached to tagged
GitHub releases.

The GitHub Actions workflow builds Linux and Windows x64 artifacts.

## License

This project is distributed under the [MIT License](LICENSE).

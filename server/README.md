# RinCast Server

Accounts, headset sign-in codes, PC presence and the UDP rendezvous that
introduces a headset to its owner's PCs on other networks. The server never
forwards stream traffic: headset and PC always connect directly (same LAN,
IPv6, hole-punched IPv4 or a router port mapping), so it needs almost no
bandwidth. Local streaming never needs this server.

## Run

```sh
go run . -public-url https://rincast.example.com -rendezvous-host rincast.example.com
# or
docker build -t rincast-server . && docker run -p 8080:8080 -p 38720:38720/udp -v rincast:/data \
  -e RINCAST_PUBLIC_URL=https://rincast.example.com -e RINCAST_RENDEZVOUS_HOST=rincast.example.com rincast-server
```

Put HTTPS in front with a reverse proxy (Caddy, nginx, Apache; there is an Apache example in
[`deploy/apache-reverse-proxy.conf`](deploy/apache-reverse-proxy.conf)) and pass `-trust-proxy` so
rate limiting sees real client IPs. The server logs a warning if it sees `X-Forwarded-For` with
`-trust-proxy` off. Only set it behind a proxy you control, which must be the only way in. UDP 38720 must be reachable directly, not through a
proxy. Give the rendezvous host an AAAA record as well as an A record (and open UDP 38720
for IPv6 too): clients then register over both, and the server can tell each side where
the other is reachable over IPv6 as well.

| Flag / env | Default | Meaning |
|---|---|---|
| `-http` / `RINCAST_HTTP` | `:8080` | HTTP listen address |
| `-udp` / `RINCAST_UDP` | `:38720` | UDP rendezvous (IPv4 and IPv6) |
| `-data` / `RINCAST_DATA` | `./data` | bbolt database + signing key |
| `-public-url` / `RINCAST_PUBLIC_URL` | `http://localhost:8080` | Base URL shown to users (`/link`) |
| `-rendezvous-host` / `RINCAST_RENDEZVOUS_HOST` | `localhost` | Host clients use for UDP |
| `RINCAST_RENDEZVOUS_PORT` | bound port | Override if UDP is port-mapped |
| `-allow-signups` / `RINCAST_ALLOW_SIGNUPS` | `true` | Allow new accounts. Existing accounts and headset sign-in keep working |
| `-max-users` / `RINCAST_MAX_USERS` | `0` | Stop registration at this many accounts (`0` = no limit) |
| `-trust-proxy` / `RINCAST_TRUST_PROXY` | `false` | Take the client IP from the last `X-Forwarded-For` entry |
| `-privacy-url` / `RINCAST_PRIVACY_URL` | | Privacy policy link for the account site's footer |
| `-imprint-url` / `RINCAST_IMPRINT_URL` | | Legal notice link for the account site's footer |
| `-download-url` / `RINCAST_DOWNLOAD_URL` | | Where the website's download buttons go. A GitHub releases link (such as `https://github.com/TheReal-Flo/RinCast/releases/latest`) also adds links to the source code |
| `-reset-password <email>` | | Set a new random password, print it and exit (see below) |
| `-relay-max-mb` | | Deprecated and ignored (nothing is relayed); still accepted so old service files keep working |

## Resetting a password

The server doesn't send email, so a forgotten password goes through the operator. Stop the
server (it keeps the database locked), then run the command as the same user with the same
`-data` directory:

```sh
rincast-server -data /path/to/data -reset-password user@example.com
```

It prints a new random password once and signs out all of that account's devices. Check who
is asking before you hand it over, and have them change it in their account settings: until
they do, you know their password. In Docker, stop the container first, then run
`docker run --rm -v rincast:/data rincast-server -reset-password user@example.com`.

## Limits and stored data

Per client IP (one IPv6 /64 counts as one), per minute: 20 for register, login, refresh, device
code and account changes; 20 for entering a headset code; 120 for `/device/token`; 30 for
`/connect`; 240 for `/pcs` and everything under it; 120 UDP rendezvous datagrams. After 5 failed
sign-ins an email is blocked for 30 seconds, doubling up to 15 minutes, whether or not an account
exists for it. Someone guessing passwords can therefore delay new sign-ins to an account, but
devices that are already signed in aren't affected. An account can link 25 PCs and 25 headsets.

The database holds each account's email, name and password hash (argon2id), its PCs' and
headsets' names, models and last-seen times, and hashes of refresh tokens with their device names.
PC addresses are held in memory only, while the PC is online. Users can download their data and
delete their account from the account site.

## API summary

All endpoints are JSON under `/api/v1`.

- `POST /auth/register`, `/auth/login`, `/auth/refresh` (rotating refresh tokens), `/auth/logout`
- `GET/PATCH /me`. Changing the password signs out every other session.
- `DELETE /me {password}` removes the account, its tokens, PCs and headsets.
- `GET /me/export` returns the user's stored data, without password or token hashes.
- `GET /info` (public): server info, plus `privacyUrl` and `imprintUrl` when set.
- Headset sign-in (device code flow): `POST /device/code` → show `userCode`; user approves at
  `/link` (web) or in the PC streamer; headset polls `POST /device/token`.
- PCs: `POST /pcs` (register), `POST /pcs/{id}/heartbeat`, `GET /pcs/{id}/events?wait=25` (long-poll), `GET /pcs`, `DELETE /pcs/{id}`
- `POST /connect {pcId}` → session id, per-session secret, rendezvous token (`relayToken` for
  compatibility) and the PC's advertised addresses; the PC gets a `connect` event.

UDP rendezvous: datagrams prefixed with `RCRV` + JSON (`register`, `ping`). A `ping` is only
answered if the reply is no larger than the request, so pad it to 100 bytes with any extra JSON
field. A side
registers with its token and the addresses it can be reached on (LAN, global IPv6,
router port mapping). The server adds the address it saw the side send from and sends
each side the other's full list, so both punch toward every address at once. Any other
datagram is dropped: there is no relay.

## Test

```sh
go test ./...
```

# Moonlight / GameStream host protocol: implementation spec for a minimal C host

Research date 2026-10-12. Everything here was read from the current `master` source of
Sunshine, moonlight-common-c, moonlight-qt and moonlight-android, and the `stable` branch of
wolf. The box (192.168.1.50) was not contacted.

Markers: **[src]** = read in source code (file + function given), **[unverified]** = inference or
recollection, check before relying on it.

Source roots used below:

| Short name | URL |
|---|---|
| SUN | https://github.com/LizardByte/Sunshine/blob/master/src/ |
| MCC | https://github.com/moonlight-stream/moonlight-common-c/blob/master/src/ |
| MQT | https://github.com/moonlight-stream/moonlight-qt/blob/master/app/ |
| AND | https://github.com/moonlight-stream/moonlight-android/blob/master/app/src/main/java/com/limelight/ |
| WOLF | https://github.com/games-on-whales/wolf/blob/stable/src/ |

So `SUN nvhttp.cpp` means https://github.com/LizardByte/Sunshine/blob/master/src/nvhttp.cpp.

Byte order notation: **LE** = little endian, **BE** = big endian (network order). Every struct
is packed (`#pragma pack(1)`).

---

## 0. Big picture / recommended choices

```
client                              host (Cube)
------                              -----------
HTTP  GET :47989/serverinfo   --->  XML (unauthenticated)
HTTP  GET :47989/pair?...     --->  4-phase PIN pairing (AES-ECB + RSA-SHA256)
HTTPS GET :47984/pair?phrase=pairchallenge (mutual TLS, client cert now pinned)
HTTPS GET :47984/applist, /appasset, /launch, /resume, /cancel
          /launch?rikey=<16B AES key>&rikeyid=<int32>&mode=WxHxFPS&...
          <- sessionUrl0 = rtsp://<host>:48010
TCP   :48010  OPTIONS, DESCRIBE, SETUP audio, SETUP video, SETUP control, ANNOUNCE, PLAY
              (one request per TCP connection; host closes after each response)
UDP   :48000  client sends 20-byte ping (from its audio socket) every 500 ms; host sends RTP/Opus back to that addr:port
UDP   :47998  client sends 20-byte ping every 500 ms; host sends RTP/H.264 shards back to that addr:port
UDP   :47999  ENet connection (control): AES-128-GCM encrypted messages, input, IDR requests, pings, termination
```

Recommended for this project (all reasoning below):

- Advertise **`appversion` = `7.1.431.-1`** and **`GfeVersion` = `3.23.0.74`** (the exact
  values Sunshine and wolf use). The negative 4th field marks you as "Sunshine". The `7.1.431`
  triple selects TCP RTSP, the single `PLAY /`, multi-FEC video and the encrypted control stream.
- Return a **plain `rtsp://` session URL** (no `rtspenc://`). RTSP encryption is something the
  client opts into from the URL scheme. wolf does this with current clients.
- **Control-stream AES-GCM is mandatory** for this appversion. Advertise
  `encryptionSupported:5` (CONTROL_V2|AUDIO) and `encryptionRequested:5` so every client uses
  the 12-byte-IV "V2" control encryption *and* encrypted audio. That gives one audio code path:
  moonlight-qt and iOS encrypt audio anyway, and Android doesn't unless the host asks.
- **No video encryption** (don't set bit 0x02 in `encryptionSupported`). Video FEC at 20 %
  (nanors), or 0 % for a first prototype. Both are legal.
- **H.264, plus HEVC Main if the encoder can:** `ServerCodecModeSupport=0x101` (SCM_H264 |
  SCM_HEVC), `MaxLumaPixelsHEVC=1869449984`, `sprop-parameter-sets=AAAAAU` in DESCRIBE; the
  client picks with `x-nv-vqos[0].bitStreamFormat` (0/1). H.264 only: `1`, `0`, no sprop line.
  An HEVC keyframe must start with VPS (+ SPS, PPS); a CRA keyframe without leading pictures
  works with moonlight-qt **[tested 2026-10-15]**. Leave out `refPicInvalidation`, so the client asks for an IDR on any loss.
- Stereo Opus, CBR, 5 ms frames (whatever `x-nv-aqos.packetDuration` says), with audio FEC
  (RS 4+2). Audio FEC is optional for a first prototype.
- mDNS is optional: without it the user adds the host by IP. cube-stream announces
  `_nvstream._tcp` (instance name = hostname, port 47989, no TXT) through avahi-daemon.

---

## 1. Ports, discovery

Sunshine derives every port from a base port, by default **47989**: `net::map_port(offset)` =
base + offset **[src]** (SUN network.cpp `map_port`, nvhttp.h `PORT_HTTP = 0`,
`PORT_HTTPS = -5`, rtsp.h `RTSP_SETUP_PORT = 21`, stream.h `VIDEO_STREAM_PORT = 9`,
`CONTROL_PORT = 10`, `AUDIO_STREAM_PORT = 11`).

| Port | Proto | Purpose | Notes |
|---|---|---|---|
| 47989 | TCP | HTTP (nvhttp): `/serverinfo`, `/pair` | the port a user types/the client defaults to |
| 47984 | TCP | HTTPS (mutual TLS): all endpoints | client learns it from `<HttpsPort>` |
| 48010 | TCP | RTSP handshake | client takes the port from `sessionUrl0`, default 48010 (MCC Connection.c `parseRtspPortNumberFromUrl`) |
| 47998 | UDP | video RTP | client takes it from SETUP `Transport: server_port=`, default 47998 |
| 47999 | UDP | control (ENet) | from SETUP control, default 47999 |
| 48000 | UDP | audio RTP | from SETUP audio, default 48000 |
| (47990) | TCP | Sunshine web UI | not part of the protocol |

**mDNS:** Sunshine advertises `_nvstream._tcp` with port 47989. Clients only use it to
discover hosts. "Add PC manually" (moonlight-qt, Android, iOS) talks straight to
`http://<ip>:47989/serverinfo` (MQT backend/nvhttp.cpp `NvHTTP::getServerInfo`). **mDNS is not
required.** moonlight-qt also accepts `ip:port` for a non-default HTTP port **[unverified]**.
The other ports are learned from serverinfo, the launch response and RTSP SETUP.

Wake-on-LAN uses `<mac>`. Send `00:00:00:00:00:00` and the client ignores it (MQT
backend/nvcomputer.cpp ctor).

---

## 2. HTTP / HTTPS (nvhttp)

### 2.1 General rules [src]

- Only GET. Query string parameters. Responses are XML:
  `<?xml version="1.0" encoding="utf-8"?><root status_code="200">…</root>`.
  **The HTTP status line is always 200**. The real status is the `status_code` attribute on
  `<root>`, with an optional `status_message` attribute. MQT `NvHTTP::verifyResponseStatus`
  reads only that attribute. A non-2xx HTTP status would show up as a `QNetworkReply` error
  instead (SUN nvhttp.cpp `not_found` sends `HTTP/1.1 404` with an XML body).
- Every request from moonlight-qt carries `uniqueid=<client id>&uuid=<random>` and then the
  per-command arguments (MQT nvhttp.cpp `NvHTTP::openConnection`). moonlight-qt disables HTTP/2
  and keeps no persistent connections (keep-alive is fine, but closing after each response is
  what Sunshine does).
- Client timeouts (MQT nvhttp.cpp): 5 s normal, 2 s fast-fail for serverinfo polling, **120 s
  for /launch**, 30 s for /resume and /cancel. The first pairing request (`getservercert`) has
  **no timeout**: the host may hold it open until the user has typed the PIN on the host.

### 2.2 Endpoint table

| Endpoint | HTTP | HTTPS | Mandatory |
|---|---|---|---|
| `/serverinfo` | yes (unauthenticated) | yes (paired clients) | yes |
| `/pair` | yes (phases 1–4) | yes (phase 5 `pairchallenge`) | yes |
| `/applist` | – | yes | yes |
| `/appasset` | – | yes | no: a failed image read gives the client a placeholder (MQT `NvHTTP::getBoxArt`) |
| `/launch` | – | yes | yes |
| `/resume` | – | yes | yes (the client calls it when `currentgame != 0`) |
| `/cancel` | – | yes | yes ("Quit app") |
| `/unpair` | (client calls it on pairing failure) | – | no; a 404 XML reply is fine (Sunshine has no `/unpair`) |

Sunshine registers exactly these routes: SUN nvhttp.cpp `start()`, `https_server.resource[...]`
and `http_server.resource[...]`.

### 2.3 TLS / client certificate pinning [src]

- **Server cert:** self-signed X.509 v3, RSA 2048, SHA-256 signature, random 159-bit serial, CN
  e.g. "Sunshine Gamestream Host", valid 20 years (SUN crypto.cpp `gen_creds`). It must be
  **RSA**: moonlight-qt checks the pairing signature with `EVP_sha256` on the cert's public key
  (MQT nvpairingmanager.cpp `verifySignature`). Android picks `SHA256withRSA` or
  `SHA256withECDSA` from the key type (AND nvstream/http/PairingManager.java). RSA works everywhere.
- **The client pins the server cert.** It uses the PEM from `plaincert` during pairing and
  stores it. The TLS errors it ignores are only the ones for *that exact* cert (MQT
  `NvHTTP::handleSslErrors`). Never regenerate the host cert, or every client has to pair again.
- **Client auth:** the HTTPS server must *request* a client certificate and accept self-signed
  ones at TLS level. After the handshake, compare the peer cert with the stored paired certs
  (Sunshine uses `X509_cmp` after an X509_STORE check that tolerates expired/not-yet-valid certs,
  SUN crypto.cpp `cert_chain_t::verify`, nvhttp.cpp `https_server.verify`). If there is no match,
  reply HTTP 200 with `<root status_code="401" status_message="The client is not authorized.
  Certificate verification failed."/>` (SUN nvhttp.cpp `on_verify_failed`). The client takes
  401 from HTTPS `/serverinfo` to mean "not paired" and falls back to HTTP (MQT
  `NvHTTP::getServerInfo`). A failed TLS handshake is also mapped to 401 by moonlight-qt
  (`openConnection`: `SslHandshakeFailedError` → 401).
- Simplest C design: OpenSSL `SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, cb_return_1)`, then
  `SSL_get1_peer_certificate()` + `X509_cmp()` against the list of paired PEMs from a state file.

### 2.4 `/serverinfo` [src]

Sunshine (SUN nvhttp.cpp `serverinfo<T>`) sends these fields:

| Field | Sunshine value | Who reads it / what for |
|---|---|---|
| `hostname` | config name | display name (MQT nvcomputer.cpp ctor) |
| `appversion` | `7.1.431.-1` (SUN nvhttp.h `VERSION`) | **protocol gate** in moonlight-common-c (`AppVersionQuad`, `APP_VERSION_AT_LEAST`, `IS_SUNSHINE()` = 4th field < 0) and pairing hash choice (major ≥ 7 → SHA-256) |
| `GfeVersion` | `3.23.0.74` (`GFE_VERSION`) | moonlight-qt compares it with a downloaded "latest supported" value; empty or unparsable counts as supported (MQT settings/compatfetcher.cpp `isGfeVersionSupported`) |
| `uniqueid` | random UUID, persisted | host identity |
| `HttpsPort` | 47984 | HTTPS port (default 47984 if missing/0) |
| `ExternalPort` | 47989 | Sunshine extension, WAN HTTP port |
| `MaxLumaPixelsHEVC` | `0` or `1869449984` | 0 = no HEVC |
| `mac` | real MAC on HTTPS, `00:00:00:00:00:00` on HTTP | WoL only |
| `LocalIP` | local address of the connection | client stores it as the LAN address (`127.*` is ignored) |
| `ServerCodecModeSupport` | bitmask, `SCM_H264 = 0x1` | missing → H.264 assumed (MQT nvcomputer.cpp). Values in MCC Limelight.h `SCM_*` |
| `ExternalIP` | only if configured | optional |
| `PairStatus` | **0 over HTTP; 1 over HTTPS** (the TLS layer already checked the cert) | paired state |
| `currentgame` | running app id or 0 | only counts when `state` ends with `_SERVER_BUSY` (MQT `NvHTTP::getCurrentGame`) |
| `state` | `SUNSHINE_SERVER_BUSY` / `SUNSHINE_SERVER_FREE` | busy → resume/quit offered. Must **not** contain `MJOLNIR` (that marks real NVIDIA software) |

Optional: `<SupportedDisplayMode><DisplayMode><Width/><Height/><RefreshRate/></DisplayMode>…`
(wolf sends it, Sunshine doesn't) and `gputype`.

**Minimal working response** (HTTP, unpaired or polling):

```xml
<?xml version="1.0" encoding="utf-8"?>
<root status_code="200">
  <hostname>FireCube</hostname>
  <appversion>7.1.431.-1</appversion>
  <GfeVersion>3.23.0.74</GfeVersion>
  <uniqueid>3f0a7c52-0b6e-4f5e-9a1d-0c7a4d2e9b11</uniqueid>
  <HttpsPort>47984</HttpsPort>
  <ExternalPort>47989</ExternalPort>
  <MaxLumaPixelsHEVC>0</MaxLumaPixelsHEVC>
  <mac>00:00:00:00:00:00</mac>
  <LocalIP>192.168.1.50</LocalIP>
  <ServerCodecModeSupport>1</ServerCodecModeSupport>
  <PairStatus>0</PairStatus>
  <currentgame>0</currentgame>
  <state>SUNSHINE_SERVER_FREE</state>
</root>
```

Over HTTPS with a verified cert, send the same with `<PairStatus>1</PairStatus>`.

### 2.5 `/pair`: all phases [src]

Client side: MQT backend/nvpairingmanager.cpp `NvPairingManager::pair`. Host side: SUN nvhttp.cpp
`pair<T>`, `start_pairing_session`, `getservercert`, `clientchallenge`, `serverchallengeresp`,
`clientpairingsecret`. Cross-check: WOLF moonlight-protocol/moonlight.cpp `pair::*`.

The client generates the 4-digit PIN and shows it. The user types it **on the host**. Every
binary value travels as hex (moonlight sends lowercase, Sunshine sends uppercase, both sides
accept either). AES is **AES-128-ECB without padding**. "hash" is **SHA-256** when
`appversion` major ≥ 7, otherwise SHA-1 (MQT nvpairingmanager.cpp L214-225; AND
PairingManager.java `serverMajorVersion >= 7`). With `7.1.431.-1` it is always SHA-256.
"cert signature" means the raw signature bytes of an X.509 cert
(`X509_get0_signature` → `ASN1_STRING_get0_data`), 256 bytes for RSA-2048.

**Phase 1: `GET http://host:47989/pair?uniqueid=U&uuid=…&devicename=roth&updateState=1&phrase=getservercert&salt=<32 hex>&clientcert=<hex of client PEM>`**

- Host stores `clientcert` (the PEM text, hex-decoded) and `salt` (16 bytes) under `uniqueid`.
- Host **waits for the PIN** (Sunshine: web UI or stdin with `PIN_STDIN`, session timeout
  5 min, SUN nvhttp.h `PAIRING_SESSION_TIMEOUT`). For our design: block on a local CLI/FIFO/file,
  validate 4 digits, then answer. The client waits without a timeout.
- `key = SHA256(salt[16] || PIN as ASCII)[0..16)` (SUN crypto.cpp `gen_aes_key`).
- Response: `<root status_code="200"><paired>1</paired><plaincert>HEX(server cert PEM bytes)</plaincert></root>`.
  An empty `plaincert` makes the client think "already pairing", and it calls `/unpair`.

**Phase 2: `GET http://…/pair?uniqueid=U&…&clientchallenge=<hex 16 bytes>`**

- `challenge = AES_ECB_decrypt(key, clientchallenge)` (16 bytes).
- `serversecret = rand(16)`, `serverchallenge = rand(16)`.
- `h = SHA256(challenge || sig(server cert) || serversecret)` (32 bytes).
- Response: `<paired>1</paired><challengeresponse>HEX(AES_ECB_encrypt(key, h || serverchallenge))</challengeresponse>` (48 bytes → 96 hex).

**Phase 3: `GET …&serverchallengeresp=<hex 32 bytes>`**

- `clienthash = AES_ECB_decrypt(key, serverchallengeresp)` (32 bytes; the client zero-pads a
  SHA-1 hash to 32, MQT L295-296).
- Response: `<paired>1</paired><pairingsecret>HEX(serversecret[16] || RSA_SHA256_sign(server_key, serversecret))</pairingsecret>`
  (16 + 256 bytes). The client checks the signature with the server cert ("MITM detected" on
  failure). It then checks that `SHA256(its challenge || sig(server cert) || serversecret) == h`
  ("Incorrect PIN" on failure, followed by `/unpair`).

**Phase 4: `GET …&clientpairingsecret=<hex>`**

- Split into `clientsecret = first 16 bytes`, `clientsig = rest` (256 bytes).
- Check `SHA256(serverchallenge || sig(client cert) || clientsecret) == clienthash` **and**
  `RSA_SHA256_verify(client cert pubkey, clientsecret, clientsig)`.
- If both hold, add the client cert to the paired list and persist it.
- Response: `<root status_code="200"><paired>1</paired></root>` (or `<paired>0</paired>`;
  Sunshine still sends status 200).

**Phase 5: `GET https://host:47984/pair?uniqueid=U&…&phrase=pairchallenge`** (mutual TLS;
the client cert must already be trusted)

- Response: `<paired>1</paired>`, status 200. Sunshine does nothing else here.

Out-of-order phases → `paired=0, status_code=400`. Sunshine tracks `last_phase` per session.

### 2.6 `/applist` (HTTPS) [src]

```xml
<root status_code="200">
  <App><IsHdrSupported>0</IsHdrSupported><AppTitle>Kodi</AppTitle><ID>1</ID></App>
</root>
```
`AppTitle` and `ID` are required per `<App>` (MQT `NvHTTP::getAppList`, "Invalid applist XML"
if an App isn't initialized). `ID` > 0.

### 2.7 `/appasset?appid=N&AssetType=2&AssetIdx=0` (HTTPS)

Raw PNG body, `Content-Type: image/png` (SUN `appasset`). Optional (see 2.2). Box art is
portrait, Sunshine's default is 130×180 **[unverified]**.

### 2.8 `/launch` and `/resume` (HTTPS) [src]

Query parameters sent by moonlight-qt (MQT nvhttp.cpp `NvHTTP::startApp`, verb "launch"/"resume"):

| Param | Format | Meaning |
|---|---|---|
| `appid` | int | app to start |
| `mode` | `WIDTHxHEIGHTxFPS` | requested stream mode |
| `additionalStates` | `1` | ignore |
| `sops` | 0/1 | GFE "optimize game settings", ignore |
| `rikey` | 32 hex chars | **16-byte AES key** for this session (control/audio/RTSP/video crypto) |
| `rikeyid` | signed decimal int32 | first 4 bytes of the client's "remoteInputAesIv", read BE. Used as the audio IV base |
| `localAudioPlayMode` | 0/1 | 1 = keep playing audio on the host |
| `surroundAudioInfo` | int | `channelMask << 16 \| channelCount`; stereo = `0x30002` = 196610, 5.1 = `0x3F0006` |
| `remoteControllersBitmap`, `gcmap` | int | gamepad mask |
| `gcpersist` | 0/1 | keep virtual pads after disconnect |
| `hdrMode=1&clientHdrCap…` | | only if the client supports 10-bit |
| `corever` | `1` | added by `LiGetLaunchUrlQueryParameters()` (MCC Connection.c): v0 = video + control encryption v2, v1 = **RTSP encryption supported** |

Sunshine (SUN nvhttp.cpp `launch`, `make_launch_session`):
- Requires `rikey`, `rikeyid`, `localAudioPlayMode`, `appid`, else `status_code=400`. If an app
  is already running, launch → 400 "An app is already running" (the client should use /resume).
- If `corever >= 1`, Sunshine uses `rtspenc://` (encrypted RTSP). **This is the host's choice:**
  the client only encrypts RTSP when the URL starts with `rtspenc://` (MCC RtspConnection.c
  `performRtspHandshake`: `encryptedRtspEnabled = strstr(url, "rtspenc://")`). wolf always
  returns `rtsp://` (WOLF moonlight.cpp `launch_success`).
- Generates per session: `av_ping_payload` = 16 ASCII chars (hex of 8 random bytes) and
  `control_connect_data` = random uint32.

Response:
```xml
<root status_code="200"><sessionUrl0>rtsp://192.168.1.50:48010</sessionUrl0><gamesession>1</gamesession></root>
```
`/resume` is the same with `<resume>1</resume>` instead of `<gamesession>1</gamesession>`, and
Sunshine returns 503 if nothing runs. **Each /launch and /resume carries a new rikey/rikeyid.**
Re-key all ciphers per RTSP session. The host in `sessionUrl0` should be the local address the
HTTPS request arrived on (Sunshine `request->local_endpoint()`).

### 2.9 `/cancel` (HTTPS) [src]

Response `<root status_code="200"><cancel>1</cancel></root>`. Ends streaming sessions and the
app. Afterwards moonlight-qt re-reads serverinfo and reports error 599 if `currentgame` is
still ≠ 0 (MQT `NvHTTP::quitApp`). So `/cancel` must reset `currentgame` to 0 and state to FREE.

### 2.10 PIN entry on the host (design note)

Sunshine holds the phase-1 HTTP response (`async_insert_pin.response`) until the PIN arrives
via its web UI (`nvhttp::pin(pairing_id, pin, name)`), or reads stdin with `--pin-stdin`
(SUN nvhttp.cpp `start_pairing_session`). For the Cube: when a `getservercert` arrives, log
"pairing request from <devicename>/<ip>". Then wait (e.g. 5 min max) for a PIN on a control
socket, or in a file such as `/storage/cube-moonlight/moonlight/pin` written by
`ssh … 'echo 1234 > …'`. Delete the file after use.

---

## 3. RTSP (TCP 48010)

### 3.1 Transport rules [src]

- **One request per TCP connection.** The client connects, sends one request, and **reads
  until the host closes the socket** (MCC RtspConnection.c `transactRtspMessageTcp`; max 1 MiB,
  15 s receive timeout, connect retries every 500 ms for 10 s on ECONNREFUSED). Sunshine calls
  `sock.shutdown(both)` after every response (SUN rtsp.cpp `rtsp_server_t::handle_msg`).
  **If you don't close, the client hangs.**
- Sunshine only accepts RTSP connections while a launch session is pending, and drops the
  pending session if the client doesn't connect within `ping_timeout` (10 s)
  (SUN rtsp.cpp `handle_accept`, `session_raise`).
- Read requests until `\r\n\r\n`, then `Content-length` bytes of payload (ANNOUNCE only; the
  client writes `Content-length` with a lowercase l, so match case-insensitively).
- Response format (MCC RtspParser.c `serializeRtspMessage`):
  `RTSP/1.0 200 OK\r\n` + `Option: value\r\n` … + `\r\n` + payload. **The response needs at
  least one header** (always send `CSeq: <request CSeq>`): the client parser only detects the
  end of the header block after an option (MCC RtspParser.c `parseRtspMessage`), so a
  header-less response is "malformed". No `Content-Length` is needed on responses (read-to-close).
- Requests from a gen-7 client carry `CSeq`, `X-GS-ClientVersion: 14`, `Host: <addr>`
  (MCC `initializeRtspRequest`).

### 3.2 Sequence for appversion 7.1.431 [src]

From MCC RtspConnection.c `performRtspHandshake`:

1. `OPTIONS rtsp://<url> RTSP/1.0` → `200 OK`, `CSeq`.
2. `DESCRIBE rtsp://<url>` (`Accept: application/sdp`) → `200 OK` + SDP-ish payload (3.3).
   **A payload must be present** ("RTSP DESCRIBE no content in response").
3. `SETUP streamid=audio/0/0` (`Transport: unicast;X-GS-ClientPort=50000-50001`) → must have
   `Session:` (missing → fatal) and `Transport: server_port=48000`. Optional
   `X-SS-Ping-Payload: <16 chars>`. After this, **the client starts its audio ping thread**
   (`notifyAudioPortNegotiationComplete`).
4. `SETUP streamid=video/0/0` (+ `Session:`) → `Transport: server_port=47998`, `X-SS-Ping-Payload`.
5. `SETUP streamid=control/13/0` → `Transport: server_port=47999`, `X-SS-Connect-Data: <uint32 decimal>`.
6. `ANNOUNCE streamid=control/13/0` (`Session`, `Content-type: application/sdp`,
   `Content-length`) + SDP with the stream parameters (3.4) → `200 OK`. Sunshine starts the
   stream session here (SUN rtsp.cpp `cmd_announce` → `stream::session::alloc/start`).
7. `PLAY /` (single PLAY for ≥ 7.1.431) → `200 OK`.

The `rtsp://<url>` target is `sessionUrl0` if the bitrate ≥ 15 Mbps and the client is local or
stereo; otherwise it is `rtsp://0.0.0.0:48010`, and then the `Host:` header is `0.0.0.0`. Sunshine
uses that to choose low vs. high quality stereo (3.4).

SETUP target parsing (Sunshine `cmd_setup`): type = text after `=` up to `/` → audio | video |
control, anything else → 404.

### 3.3 DESCRIBE response payload [src]

Sunshine (SUN rtsp.cpp `cmd_describe`) emits lines separated by `\n`:

```
a=x-ss-general.featureFlags:<platform caps: 0x01 pen/touch, 0x02 controller touch>
a=x-ss-general.encryptionSupported:<flags>
a=x-ss-general.encryptionRequested:<flags>
[a=x-nv-video[0].refPicInvalidation:1]      ← only if the encoder supports RFI
[sprop-parameter-sets=AAAAAU]              ← only if HEVC is enabled (client looks for this exact string)
[a=rtpmap:98 AV1/90000]                     ← only if AV1 is enabled
a=fmtp:97 surround-params=<one line per audio config, see 5.6>
```

Encryption flags (MCC Limelight-internal.h): `SS_ENC_CONTROL_V2 = 0x01`, `SS_ENC_VIDEO = 0x02`,
`SS_ENC_AUDIO = 0x04`. Sunshine on LAN (default `lan_encryption_mode = NEVER`):
`supported = 0x05`, `requested = 0x01`. The client parses these with
`strstr` + `strtoul` (MCC `parseSdpAttributeToUInt`), so formatting is loose.

Client negotiation (MCC SdpGenerator.c `getAttributesList`, only when `IS_SUNSHINE()`):
- CONTROL_V2 is enabled whenever the host supports it.
- VIDEO if (host supports VIDEO and the client opted in) or (host requests VIDEO).
- AUDIO if (host supports AUDIO and the client opted in) or (host requests AUDIO).
- **Plus**, independent of host flags: if the client opted into audio encryption
  (`StreamConfig.encryptionFlags & ENCFLG_AUDIO`), it sets `NVFF_AUDIO_ENCRYPTION` (0x20) in
  `x-nv-general.featureFlags` and **will decrypt audio** (`addGen5Options`). moonlight-qt
  always opts into audio (and into video only with fast AES and >2 cores) (MQT
  streaming/session.cpp: `encryptionFlags = ENCFLG_ALL` or `ENCFLG_AUDIO`). iOS uses
  `ENCFLG_ALL`. Android never sets the field, so it is 0 **[src, AND StreamConfiguration.java
  has no setter call found]**.

**Minimal DESCRIBE response for this project (H.264, stereo + 5.1, no RFI, no video crypto):**

```
RTSP/1.0 200 OK
CSeq: 2

a=x-ss-general.featureFlags:0
a=x-ss-general.encryptionSupported:5
a=x-ss-general.encryptionRequested:5
a=fmtp:97 surround-params=21101
a=fmtp:97 surround-params=642012453
```

(The stereo line is ignored by clients but harmless. Without any `surround-params=6…` line, a
5.1 client falls back to a hardcoded 6-channel mapping; see 5.6.)

### 3.4 ANNOUNCE payload (client → host) [src]

Produced by MCC SdpGenerator.c `getSdpPayloadForStreamConfig`/`getAttributesList`/`addGen5Options`.
Format: `v=0\r\n o=android 0 14 IN IPv4 <addr>\r\n s=NVIDIA Streaming Client\r\n`, then
`a=<name>:<value> \r\n` lines (**note the trailing space before CRLF**; Sunshine strips it),
then `t=0 0\r\n m=video <videoPort>  \r\n`.

Attributes a gen-7 Sunshine-mode client sends, and what to do with them:

| Attribute | Typical value | Host use |
|---|---|---|
| `x-ml-general.featureFlags` | 3 = `ML_FF_FEC_STATUS(1) \| ML_FF_SESSION_ID_V1(2)` | if bit 2 is set, match the ENet connect by `X-SS-Connect-Data` and UDP pings by payload; otherwise match by client IP |
| `x-ss-general.encryptionEnabled` | negotiated SS_ENC_* | **use these bits**: CONTROL_V2 → 12-byte IV control crypto; AUDIO → encrypt audio; VIDEO → encrypt video |
| `x-ss-video[0].chromaSamplingType` | 0 | 1 = YUV444 (not offered) |
| `x-nv-video[0].clientViewportWd` / `Ht` | 1920 / 1080 | requested resolution |
| `x-nv-video[0].maxFPS` | 60 | requested fps |
| `x-nv-video[0].packetSize` | 1392 (1024 remote/VPN) | **video payload unit**; already reduced by 32 if video encryption is on |
| `x-nv-video[0].rateControlMode` | 4 | ignore |
| `x-nv-video[0].timeoutLengthMs` | 7000 | ignore |
| `x-nv-video[0].framesWithInvalidRefThreshold` | 0 | ignore |
| `x-nv-video[0].initialBitrateKbps`, `initialPeakBitrateKbps`, `x-nv-vqos[0].bw.minimumBitrateKbps`, `.maximumBitrateKbps` | 0.8 × user bitrate (−500 if remote, capped at 100000) | encoder bitrate |
| `x-ml-video.configuredBitrateKbps` | user bitrate | Sunshine uses this instead: × (100−FEC%)/100, minus audio (96 or 256 kbit/s per channel, cap 20 %), minus 500 (cap 10 %) (SUN `cmd_announce`) |
| `x-nv-vqos[0].fec.enable` | 1 | ignore |
| `x-nv-vqos[0].videoQualityScoreUpdateTime` | 5000 | ignore |
| `x-nv-vqos[0].qosTrafficType`, `x-nv-aqos.qosTrafficType` | 5/4 local, 0 remote | DSCP tagging hint |
| `x-nv-general.featureFlags` | `0x07 \| 0x80` (+`0x20` if audio crypto) | **bit 0x20 → encrypt audio** (Sunshine ORs it into `encryptionFlagsEnabled`) |
| `x-nv-general.useReliableUdp` | **13** | 13 = encrypted control protocol |
| `x-nv-vqos[0].fec.minRequiredFecPackets` | 2 | minimum parity shards per FEC block (if FEC% ≠ 0) |
| `x-nv-vqos[0].bllFec.enable` | 0 | ignore |
| `x-nv-vqos[0].drc.enable` | 0 | ignore |
| `x-nv-general.enableRecoveryMode` | 0 | ignore |
| `x-nv-video[0].videoEncoderSlicesPerFrame` | 1 (or decoder caps >> 24) | slices per frame |
| `x-nv-clientSupportHevc`, `x-nv-vqos[0].bitStreamFormat` | 0 / 0 | 0 = H.264, 1 = HEVC, 2 = AV1. Reject what you can't do (Sunshine → 400) |
| `x-nv-video[0].dynamicRangeMode` | 0 | 1 = HDR |
| `x-nv-video[0].maxNumReferenceFrames` | 0 or 1 | **1 when the decoder doesn't support RFI** → encode with 1 reference frame; 0 = "any" |
| `x-nv-video[0].clientRefreshRateX100` | e.g. 5994 | optional |
| `x-nv-audio.surround.numChannels` | 2 / 6 / 8 | channel count |
| `x-nv-audio.surround.channelMask` | 3 / 63 / 1599 | |
| `x-nv-audio.surround.enable` | 0/1 | |
| `x-nv-audio.surround.AudioQuality` | 0/1 | 1 = high-quality surround (only if advertised twice, 5.6) |
| `x-nv-aqos.packetDuration` | **5** (10 for slow decoders / low bitrate) | Opus frame length in ms |
| `x-nv-video[0].encoderCscMode` | `(colorSpace<<1) \| colorRange` | 0 = BT.601 limited, 2 = BT.709 limited, … (`COLORSPACE_*`, `COLOR_RANGE_*` in MCC Limelight.h) |

Sunshine requires (else 400): `x-nv-audio.surround.numChannels`, `.channelMask`,
`.AudioQuality`, `x-nv-video[0].packetSize`, `clientViewportWd/Ht`, `maxFPS`,
`x-nv-vqos[0].bw.maximumBitrateKbps`, `videoEncoderSlicesPerFrame`, `maxNumReferenceFrames`.
Defaults for the rest are in SUN `cmd_announce` (`args.try_emplace`).

**Stereo quality quirk (SUN `cmd_announce`):** for 2 channels, Sunshine uses high-quality
stereo (512 kbit/s) unless the request's `Host:` header contains `0.0.0.0` (then 96 kbit/s).
The client decodes any bitrate, so this is only bandwidth policy.

### 3.5 Encrypted RTSP (`rtspenc://`): optional, documented for completeness [src]

Each message, in both directions: `uint32 BE typeAndLength = 0x80000000 | plaintextLen`,
`uint32 BE sequenceNumber`, `uint8 tag[16]`, then the ciphertext. AES-128-GCM, key = rikey,
12-byte IV: bytes 0–3 = sequence number LE, bytes 10–11 = `'C','R'` (client→host) or
`'H','R'` (host→client), the rest zero. Sources: MCC RtspConnection.c
`sealRtspMessage`/`unsealRtspMessage`, SUN rtsp.cpp `respond()`. If the host returned
`rtspenc://`, it must reject unencrypted messages. Not needed when you return `rtsp://`.

---

## 4. Video stream (UDP 47998)

### 4.1 Client ping / hole punch [src]

- The client binds a UDP socket and sends a ping to host:47998 **every 500 ms** from the same
  socket it receives on (MCC VideoStream.c `VideoPingThreadProc`). With `X-SS-Ping-Payload`
  the ping is `SS_PING { char payload[16]; uint32 BE sequenceNumber }` (20 bytes, MCC Video.h).
  Without it, it is the 4 bytes `"PING"`.
- The host waits for a ping that **contains** the 16-byte payload (Sunshine `recv_ping`:
  `msg.find(expected_payload)`), then streams to the ping's source **addr:port**. Sunshine waits
  up to `ping_timeout` = 10 s. Keep updating the peer port if later pings come from elsewhere
  **[unverified, Sunshine latches the first]**.
- The client gives up after 10 s without video packets (`ML_ERROR_NO_VIDEO_TRAFFIC`), or 10 s
  without a complete frame (`ML_ERROR_NO_VIDEO_FRAME`) (MCC VideoStream.c
  `FIRST_FRAME_TIMEOUT_SEC`).

### 4.2 Packet layout (unencrypted) [src]

Each UDP datagram (SUN stream.cpp `video_packet_raw_t`; MCC Video.h `RTP_PACKET`,
`NV_VIDEO_PACKET`):

```
off size field
 0   1   rtp.header        = 0x90  (0x80 | FLAG_EXTENSION 0x10). The client expects the X bit.
 1   1   rtp.packetType    = 0     (not checked)
 2   2   rtp.sequenceNumber  BE16, +1 per datagram (data AND parity), continuous across frames, starts at 0
 4   4   rtp.timestamp       BE32, 90 kHz clock (capture time). 0 is accepted: the client then makes up PTS from receive time
 8   4   rtp.ssrc            0
12   4   reserved            0 (the "extension"; client skips 4 bytes when X is set)
16   4   streamPacketIndex   LE32 = packet counter << 8 (client uses bits 8..31 = 24 bits). The counter is
                             a wide int (Sunshine `int lowseq`); rtp.sequenceNumber is its low 16 bits.
                             **Not** the 16-bit seq: that wraps after 65536 packets and the client then
                             reports every later frame as corrupt [tested 2026-10-14]
20   4   frameIndex          LE32, first frame = 1, +1 per frame
24   1   flags             FLAG_CONTAINS_PIC_DATA 0x1 | FLAG_EOF 0x2 | FLAG_SOF 0x4
25   1   extraFlags        0 (0x1 = LTR frame, Sunshine RFI extension)
26   1   multiFecFlags     0x10
27   1   multiFecBlocks    (blockIndex << 4) | ((blockCount-1) << 6)    (2 bits each, max 4 blocks)
28   4   fecInfo           LE32 = shardIndex << 12 | dataShards << 22 | fecPercent << 4
32   …   payload           exactly packetSize-16 bytes, except the last data packet of a block may be shorter
```

- Datagram size = `packetSize + 16` (e.g. 1408 for 1392). The client asserts
  `(flags & FLAG_EOF) || payloadLen == packetSize` where payloadLen counts from offset 16
  (MCC RtpVideoQueue.c `RtpvAddPacket`). Sunshine pads every shard to full size.
- `fecInfo` fields: shardIndex 10 bits (0x3FF000), dataShards 10 bits (0xFFC00000),
  fecPercent 8 bits (0xFF0). The client computes parity count as
  `(dataShards * fecPercent + 99) / 100` (MCC `RtpvAddPacket`). **The host must produce exactly
  that many parity shards**, so when raising the parity count to `minRequiredFecPackets`, report
  `fecPercent = 100 * parity / data` (integer division), as Sunshine does (SUN stream.cpp
  `fec::encode`). Check that the client's formula gives your parity count back.
- `SOF` is set on the **first packet of every FEC block**, `EOF` on the last *data* packet of
  every block, `CONTAINS_PIC_DATA` on all (SUN `videoBroadcastThread`). Parity shards keep
  whatever these bytes come out as.
- `streamPacketIndex` must increase by exactly 1 between consecutive data packets inside a
  block. At SOF packets it may jump forward (over parity shards) but never go back (MCC
  VideoDepacketizer.c `processRtpPayload`, "Depacketizer detected corrupt frame").
- Frames older than `currentFrameNumber` (starts at **1**) are dropped (MCC
  `RtpvInitializeQueue`), so **do not start at frameIndex 0**. Sunshine's `frame_nr` starts at
  1 (SUN video.cpp).

### 4.3 Frame payload and the 8-byte frame header [src]

Per frame, the host builds `frame = header[8] || AnnexB access unit`, then cuts it into
`packetSize-16`-byte chunks (Sunshine `concat_and_insert` inserts 32 header bytes every
`packetSize-16` bytes).

`video_short_frame_header_t` (SUN stream.cpp, 8 bytes), as parsed by MCC VideoDepacketizer.c
`processRtpPayload`:

```
off size
 0   1  headerType = 0x01   (≥7.1.415: 0x01 → 8-byte header; 0x81 → longer GFE headers)
 1   2  LE16 frame processing latency in 0.1 ms units (Sunshine extension, shown in client stats; 0 = unknown)
 3   1  frameType: 1 = P, 2 = IDR, 4 = P with intra-refresh, 5 = P after RFI (104 tolerated)
 4   2  LE16 lastPayloadLen = (len(frame incl. header)) % (packetSize-16), or packetSize-16 if 0
        (only used for AV1; H.264 tolerates trailing zero padding)
 6   2  0
```

For H.264, the client does **not** trust `frameType == 2`. It parses the bitstream:
- After the header, the next bytes must be an Annex-B start code (3 or 4 bytes). AUD and SEI
  NALs at the start are skipped.
- **An IDR frame must begin with SPS, then PPS, then the slice(s)** (`isIdrFrameStart` = first
  NAL is SPS; debug builds assert `SPS → PPS → picture`). P frames before the first
  successfully decoded IDR are dropped. **Send SPS+PPS inline in front of every IDR.**
- A PPS in front of P-frames is skipped (Intel MF quirk).
- Trailing zero padding in the last packet is fine (Annex B).

SPS recommendations (SUN cbs.cpp `make_sps_h264`, used only if the encoder's SPS has no VUI;
`validate_sps` only checks `vui_parameters_present_flag`): `vui_parameters_present_flag=1`,
`video_signal_type`/colour description, `bitstream_restriction_flag=1`,
`max_num_reorder_frames=0`, `max_dec_frame_buffering=max_num_ref_frames`. Without
bitstream_restriction, some hardware decoders (Android MediaCodec in particular) buffer frames
and add latency **[unverified for the specific client decoders, but it is why Sunshine adds it]**.
Encode without B-frames, with 1 reference frame when ANNOUNCE says `maxNumReferenceFrames=1`.

Resolution: encode the requested `clientViewportWd×Ht`. Whether every client decoder copes with
a different resolution in the SPS is **[unverified]**: FFmpeg-based moonlight-qt probably does,
MediaCodec maybe not.

### 4.4 FEC (Reed-Solomon, nanors) and multi-FEC blocks [src]

Sunshine (SUN stream.cpp `videoBroadcastThread`, `fec::encode`):

```
blocksize          = packetSize + 16                 (whole datagram incl. RTP header)
max data shards    = (255 * 100) / (100 + fec%)      (DATA_SHARDS_MAX = 255; 212 at 20 %)
fec_blocks_needed  = ceil(frame_with_headers_bytes / (max_data_shards * blocksize)), max 4
                     (if >4: turn FEC off for this frame, fec% = 0, use 4 blocks)
split frame into fec_blocks_needed parts, each aligned to blocksize (last block takes the remainder)
per block:
  data_shards  = ceil(block_bytes / blocksize)   (last shard zero-padded)
  parity       = ceil(data_shards * fec% / 100); if parity < minRequiredFecPackets and fec% != 0:
                 parity = minRequiredFecPackets; fec% = 100*parity/data_shards
  rs = reed_solomon_new(data_shards, parity); reed_solomon_encode(rs, shards, data+parity, blocksize)
  then for every shard x (data and parity): write rtp.header=0x90, rtp.seq=BE16(lowseq+x),
    rtp.timestamp, fecInfo, multiFecBlocks, frameIndex   (AFTER encoding)
  lowseq += data_shards + parity
```

- **Encoding covers the whole datagram** (RTP + NV header + payload). Before encoding, the data
  shards already have streamPacketIndex/frameIndex/flags/multiFec set, while the RTP header
  bytes and `fecInfo` are still zero. These are written after encoding. The client rebuilds RTP
  header, `frameIndex` and `multiFecBlocks` of recovered packets from the queue state (MCC
  RtpVideoQueue.c `reconstructFrame`). Because RS works byte-position by byte-position, the
  payload bytes come back correct anyway. wolf encodes after filling the RTP headers (WOLF
  gst-plugin/video.hpp `generate_fec_packets`). Both work. Parity shards then get real headers.
  **Correction [src, verified 2026-10-14]:** not all of bytes 0-31 may be overwritten in a
  parity shard. A recovered data shard's `streamPacketIndex` (16-19), `flags` (24),
  `extraFlags` (25) and `multiFecFlags` (26) come out of the RS decode and are checked
  (`reconstructFrame`: SOF on shard 0, EOF on the last, only PIC/SOF/EOF bits; the
  depacketizer checks the index). So overwrite only what the client rebuilds: RTP 0-11,
  `frameIndex` 20-23, `multiFecBlocks` 27, and `fecInfo` 28-31 (never read from recovered
  shards). Bytes 0-11 have to be right in parity shards (the client reads the X bit and seq).
  The client reads with `bs = packetSize + 16` (`receiveSize`) and zero-pads short packets.
  cube-stream: encode after filling the data shards' full headers, then write those 4 fields.
- nanors commit: moonlight-common-c pins **b1e3c22** (submodule); newer nanors (2026) adds
  rs16 etc. Parity matrix row j, column i = `GF2_8_INV[(ps + i) ^ j]`, GF(2^8) poly 0x11d
  (enough to recompute parity in a test client: `tools/gs-test-client.py` `rs_parity`).
- `DATA_SHARDS_MAX` (255) limits data **+ parity**; `reed_solomon_new` fails above it.
- Library: **nanors** (`reed_solomon_new/encode/decode`, GF(2^8), max 255 total shards,
  MIT). moonlight-common-c uses the same library (submodule `nanors`), so the encodings match.
- **fec% = 0 is legal**: the client then needs all data packets and doesn't wait for parity.
  Sunshine itself sends 0 % for oversized frames. A first prototype can skip nanors entirely
  (`fecInfo` percent 0, still ≤ 255 data shards per block, up to 4 blocks).
- Size limits: at most 4 blocks and 1023 shards per block (10-bit fields). Sunshine logs an
  error above 1024 packets per block. At 1392/20 % that is about 4 × 212 × 1408 B ≈ 1.19 MB
  per frame with FEC.
- Pacing: Sunshine spreads a frame's packets at about 80 % of 1 Gbit/s
  (`ratecontrol_packets_in_1ms`) and uses batched sends (sendmmsg/GSO), ≤ 64 per batch. Not
  required, but it avoids bursts that overflow Wi-Fi/socket buffers. Sunshine sets SO_SNDBUF to
  1 MiB.
- moonlight-common-c's ANNOUNCE sends `x-nv-vqos[0].fec.minRequiredFecPackets` = 2 (so small
  frames get up to 200 % FEC) and `fec.repairPercent` 20 (5 for 4K); Sunshine ignores the
  latter and uses its own `fec_percentage` (default 20).

### 4.5 Video encryption: optional [src]

Only if the host advertised `SS_ENC_VIDEO` and the client enabled it (`encryptionEnabled & 2`).
Each datagram is then `ENC_VIDEO_HEADER { uint8 iv[12]; uint32 frameNumber; uint8 tag[16] }`
(32 bytes) followed by AES-128-GCM(rikey) of the full unencrypted datagram from 4.2. IV =
8-byte LE counter (per packet) with `iv[11] = 'V'`. The client already subtracted 32 from
`packetSize` (MCC Video.h, VideoStream.c, SUN `videoBroadcastThread`). **Not mandatory: don't
advertise bit 0x02.**

### 4.6 IDR requests, RFI, frame-loss handling [src]

- Without `refPicInvalidation` in DESCRIBE, the client reacts to any unrecoverable frame by
  sending **IDR request 0x0302** on the control stream (MCC ControlStream.c
  `queueFrameInvalidationTuple` → `LiRequestIdrFrame`, `requestIdrFrame`). **The first control
  message ("Start A") is also 0x0302**, so the session always starts with an IDR request.
- The host must send an IDR soon after each request (Sunshine raises `idr_events`). The Cube
  encoder can force an IDR, which is all that is needed. RFI (0x0301, `SS_RFI_REQUEST`) and LTR
  ACKs (0x0350) only arrive if you advertise RFI.

---

## 5. Audio stream (UDP 48000)

### 5.1 Ping [src]

Same as video: a 20-byte `SS_PING` (or `"PING"`) every 500 ms from the client's audio socket.
It starts **during the RTSP handshake** (after SETUP audio). The host sends audio to its source
addr:port (SUN `audioThread`/`recv_ping`; MCC AudioStream.c `AudioPingThreadProc`). The
client **drops the first ~500 ms** of received audio packets (`packetsToDrop = 500 /
AudioPacketDuration`, MCC `AudioReceiveThreadProc`).

### 5.2 RTP data packet [src]

```
off size
 0  1  0x80           (V=2, no extension)
 1  1  97             payload type (data)
 2  2  BE16 sequence  starts at 0, +1 per Opus packet
 4  4  BE32 timestamp starts at 0, += packetDuration **in milliseconds** (5), NOT 48 kHz samples
 8  4  ssrc 0
12  …  Opus packet (or its AES-CBC ciphertext, 5.4)
```
Sources: SUN stream.cpp `audioBroadcastThread` (`timestamp += config.audio.packetDuration`),
MCC RtpAudioQueue.c `getFecBlockForRtpPacket` (computes `baseTs = ts - (seq-baseSeq) *
AudioPacketDuration`, which confirms ms units). The client receive buffer is 1400 bytes
(`MAX_PACKET_SIZE`).

### 5.3 Opus parameters [src]

SUN audio.cpp `stream_configs`, `encodeThread`:

| Config | ch | streams | coupled | mapping (encoder) | Sunshine bitrate |
|---|---|---|---|---|---|
| stereo | 2 | 1 | 1 | 0 1 | 96 kbit/s |
| stereo HQ | 2 | 1 | 1 | 0 1 | 512 kbit/s |
| 5.1 | 6 | 4 | 2 | 0 1 2 3 4 5 | 256 kbit/s |
| 5.1 HQ | 6 | 6 | 0 | 0 1 2 3 4 5 | 1536 kbit/s |
| 7.1 | 8 | 5 | 3 | 0..7 | 450 kbit/s |
| 7.1 HQ | 8 | 8 | 0 | 0..7 | 2048 kbit/s |

- `opus_multistream_encoder_create(48000, ch, streams, coupled, mapping,
  OPUS_APPLICATION_RESTRICTED_LOWDELAY)`, `OPUS_SET_BITRATE`, **`OPUS_SET_VBR(0)` (CBR)**.
  Frame size = `packetDuration * 48` samples (240 at 5 ms).
- **CBR matters for audio FEC:** all 4 data packets of an FEC block must have the same length.
  Sunshine runs the RS encode with the last packet's byte count, and the client flags "Audio
  block size mismatch" and stops using FEC (MCC RtpAudioQueue.c `incompatibleServer`).
- Stereo: the client always decodes with a multistream decoder (streams 1, coupled 1, mapping
  0 1; MCC RtspConnection.c `parseOpusConfigurations`). A packet from a plain stereo
  `opus_encoder` is identical to a 1-stream multistream packet (the last stream uses normal
  framing) **[high confidence, from the Opus multistream format; not tested here]**. So the
  existing cube-stream stereo encoder works if it is switched to CBR + 5 ms frames +
  RESTRICTED_LOWDELAY.
- Input channel order (Sunshine/Moonlight): **FL FR FC LFE BL BR (SL SR)** (SUN
  platform/common.h `speaker_e`; MCC comment "we use FL FR C LFE RL RR SL SR"). The Cube's
  capture is FL FR **LFE FC** SL SR, so swap channels 2/3 and send SL/SR as BL/BR.

### 5.4 Audio encryption: needed in practice [src]

- Enabled when ANNOUNCE has `x-ss-general.encryptionEnabled & 0x04` **or**
  `x-nv-general.featureFlags & 0x20` (SUN `cmd_announce`). moonlight-qt and iOS always opt in,
  Android only if the host requests it.
- Algorithm: AES-128-CBC, key = rikey, **PKCS#7 padding** (Sunshine `cbc_t(key, true)`).
  IV (16 bytes) = `BE32(rikeyid + rtpSequenceNumber)` in bytes 0–3, the rest zero (SUN
  `audioBroadcastThread`, MCC AudioStream.c `decodeInputData`). `rikeyid` is the decimal
  launch parameter, as a uint32.
- The ciphertext replaces the Opus payload. With CBR, every ciphertext has the same length
  (rounded up to 16).

### 5.5 Audio FEC (RS 4+2, payload type 127): optional [src]

- Blocks of 4 consecutive data packets whose first sequence number is a **multiple of 4**
  (start seq at 0). After the 4th data packet of a block, the host sends 2 FEC packets
  (SUN `audioBroadcastThread`; MCC RtpAudioQueue.h).
- nanors `reed_solomon_new(4, 2)`, then **overwrite the parity matrix**:
  `memcpy(rs->p, {0x77,0x40,0x38,0x0e,0xc7,0xa7,0x0d,0x6c}, 8)` (both sides do this; it is
  the OpenFEC matrix GFE uses). Encode over the 4 *payloads* (after encryption, without the RTP
  header), block size = payload length.
- FEC packet:
  ```
  RTP: 0x80, 127, BE16 seq = lastDataSeq + 1 + shardIdx, ts 0, ssrc 0
  AUDIO_FEC_HEADER (12 bytes): uint8 fecShardIndex (0|1), uint8 payloadType = 97,
                               BE16 baseSequenceNumber (first data seq of the block),
                               BE32 baseTimestamp (its timestamp), BE32 ssrc = 0
  parity bytes (same length as the data payloads)
  ```
  The FEC packet sequence numbers overlap the next data packets' numbers. The client tells them
  apart by payload type.
- Without FEC packets, the client plays in-order data at once (the
  `RTPQ_RET_HANDLE_NOW` fast path in `RtpaAddPacket`), so FEC can wait.

### 5.6 Surround parameters in DESCRIBE [src]

The client takes the first `a=fmtp:97 surround-params=<ch>…` line with its channel count as
"normal quality" and an optional second one as "high quality" (MCC RtspConnection.c
`parseOpusConfigurations`). Value = digits `<channels><streams><coupled><mapping…>`.

GFE quirk: on the normal-quality line, the client **rotates the mapping**:
`final[3] = adv[ch-1]; final[4..ch-1] = adv[3..ch-2]`. To make the client's final mapping equal
your encoder mapping `m`, advertise `adv[0..2]=m[0..2], adv[3..ch-2]=m[4..ch-1], adv[ch-1]=m[3]`.
For 5.1 with identity mapping that is **`642012453`** (Sunshine's output; wolf does the same
rotation for GStreamer's Vorbis order). Sunshine rotates over a fixed 6-element range, which
looks wrong for 7.1 **[unverified, out of scope]**.

Verified 2026-10-15 (cube-stream, tone test): 5.1 `642012453` with encoder mapping 0..5 and
input order FL FR FC LFE BL BR decodes to the right channels; 7.1 `85301245673` (same rule,
`adv = m0 m1 m2 m4 m5 m6 m7 m3`) gives identity too. There is no rotation on the stereo line.
A requested channel count without a line → 5.1 falls back as below, **anything else fails**
(`parseOpusConfigurations` returns -4, the connection aborts).

If no 6-channel line is present, the client uses a hardcoded 5.1 config: 4 streams, 2 coupled,
mapping 0 4 1 5 2 3. Don't rely on that; advertise the line.

---

## 6. Control stream (UDP 47999, ENet)

### 6.1 ENet [src]

- moonlight-common-c and Sunshine use the **cgutman/enet** fork (MIT): MCC `.gitmodules`
  `enet → https://github.com/cgutman/enet.git`; Sunshine builds
  `third-party/moonlight-common-c/enet` (cmake/dependencies/common.cmake). The fork's 23
  commits (as of 2026-10-12) cover a protocol-agnostic sockaddr API (IPv6,
  `enet_address_set_address`), the address family at `enet_host_create`, a wake-up for
  retransmits, **default MTU lowered to 900**, a more lenient peer address check for multi-homed
  hosts, and QoS/DSCP/ECN tagging. None of them changes the ENet wire format as far as the
  commit titles show, so stock ENet 1.3.x is probably interoperable **[unverified]**. Use the
  fork anyway: Sunshine does, and the API differs (`enet_host_create(af, …)`).
- Host: `enet_host_create(AF_INET, {any, 47999}, peerCount ≥ 1, channelLimit 0 (= max), 0, 0)`
  (SUN network.cpp `host_create`, 128 peers).
- Client: `enet_host_connect(..., CTRL_CHANNEL_COUNT = 0x30 channels, data = ControlConnectData)`,
  peer timeout 10 s (MCC ControlStream.c `startControlStream`). On the host, the CONNECT
  event's `event.data` is the `X-SS-Connect-Data` value: use it to bind the peer to the session
  (SUN `control_server_t::get_session`).
- Client channels: 0 generic, 1 urgent (IDR/RFI/LTR), 2 keyboard, 3 mouse, 4 pen, 5 touch,
  6 UTF-8, 0x10+n gamepad n, 0x20+n sensors (MCC Limelight-internal.h). The host can send
  everything reliable on channel 0 (Sunshine does).

### 6.2 Message framing and encryption (mandatory for ≥ 7.1.431) [src]

`encryptedControlStream = APP_VERSION_AT_LEAST(7, 1, 431)` (MCC ControlStream.c
`initializeControlStream`). Sunshine expects `useReliableUdp == 13` and drops unencrypted
messages (SUN stream.cpp `control_server_t::call`). **Every** ENet packet in either direction
is:

```
off size
 0   2  LE16 encryptedHeaderType = 0x0001
 2   2  LE16 length = 4 + 16 + ciphertextLen        (counts seq, tag and ciphertext)
 4   4  LE32 seq                                     (sender's own counter, from 0)
 8  16  AES-GCM tag
24   …  ciphertext of:  LE16 type, LE16 payloadLength, payload[payloadLength]
```

- AES-128-GCM, key = rikey, no padding, no AAD.
- IV with `SS_ENC_CONTROL_V2` negotiated: 12 bytes, `iv[0..3] = seq LE`, `iv[4..9] = 0`,
  `iv[10] = 'C'` (client→host) or `'H'` (host→client), `iv[11] = 'C'`.
- IV without V2 (e.g. a host that doesn't advertise `encryptionSupported`, like wolf): 16-byte
  IV, all zero except `iv[0] = (uint8)seq`.
- Sources: MCC ControlStream.c `encryptControlMessage`, `decryptControlMessageToV1`,
  `sendMessageEnet`; SUN stream.cpp `encode_control`, `IDX_ENCRYPTED` handler.

### 6.3 Message types [src]

`packetTypesGen7Enc` (MCC ControlStream.c) / SUN stream.cpp `packetTypes`:

| Type | Dir | Payload | Host action |
|---|---|---|---|
| 0x0302 | C→H | 2 bytes `00 00` | **request IDR** (also sent as "Start A" right after connect) |
| 0x0307 | C→H | 1 byte `00` | "Start B", ignore |
| 0x0200 | C→H | 8 bytes: LE32 4, LE32 0 | **periodic ping every 100 ms** (≥7.1.415), reliable → keeps ENet RTT fresh. Use it as liveness |
| 0x5502 | C→H | `SS_FRAME_FEC_STATUS` (21 bytes, BE fields) | FEC stats (Sunshine-only, unsequenced), ignore |
| 0x0301 | C→H | `SS_RFI_REQUEST`: LE32 firstFrame, 4 reserved, LE32 lastFrame, 12 reserved (24 bytes) | only if RFI is advertised |
| 0x0350 | C→H | `SS_LTR_FRAME_ACK` LE32 frameIndex, 4 reserved | only with RFI |
| 0x0206 | C→H | input packet (6.4) | inject input |
| 0x0201 / 0x0204 | C→H | loss stats / frame stats | not sent by Sunshine-mode clients with periodic ping; ignore |
| **0x0109** | H→C | `BE32 errorCode` (payloadLength 4) | **termination**. `0x80030023` = graceful (client reports `ML_ERROR_GRACEFUL_TERMINATION` if it saw a frame, else "unexpected early termination"). `0x800e9403` = encoder frame conversion failed, `0x800e9302` = protected content. Anything else is passed through as-is |
| 0x010b | H→C | `LE32 unused(0xC0FFEE), LE16 controller, LE16 lowFreq, LE16 highFreq` | rumble (optional) |
| 0x010e | H→C | `uint8 enabled` + `SS_HDR_METADATA` (Sunshine) | HDR mode (optional; SDR → never send) |
| 0x5500 / 0x5501 / 0x5502 / 0x5503 / 0x5504 | H→C | trigger rumble / motion event / RGB LED / adaptive triggers / player LEDs | optional Sunshine extensions |

The client never waits for replies on ENet (`sendMessageAndDiscardReply` only reads replies on
the old TCP path). The host must accept an ENet CONNECT, ACK reliable packets (ENet does this),
decrypt and dispatch.

### 6.4 Input packets (inside type 0x0206) [src]

After decryption, the plaintext is `LE16 0x0206, LE16 len, <input packet>`. Sunshine strips the
first 4 bytes and parses the input packet (SUN stream.cpp `IDX_ENCRYPTED` handler →
`input::passthrough`). Every input packet starts with `NV_INPUT_HEADER { BE32 size = total −
4; LE32 magic }` (MCC Input.h). **Mixed endianness: mouse coordinates/deltas/scroll are BE,
keyboard and gamepad fields are LE.** Sources: MCC Input.h + InputStream.c, SUN input.cpp
`passthrough_helper`.

| Magic (LE32) | Struct after header | Notes |
|---|---|---|
| 0x00000003 / 0x00000004 | key down / up: `int8 flags; LE16 keyCode; int8 modifiers; int16 zero` | keyCode = Windows VK, moonlight-qt sends `0x8000\|VK`; Sunshine uses `keyCode & 0xFF`. modifiers: SHIFT 1, CTRL 2, ALT 4, META 8, EXTENDED 0x10. flags: `SS_KBE_FLAG_NON_NORMALIZED 1` |
| 0x00000007 | rel mouse move: `BE16 dx; BE16 dy` | Gen5+ magic |
| 0x00000005 | abs mouse: `BE16 x; BE16 y; int16 unused; BE16 width-1; BE16 height-1` | |
| 0x00000008 / 0x00000009 | mouse button down / up: `uint8 button` (1 L, 2 M, 3 R, 4 X1, 5 X2) | Gen5+ = BUTTON_ACTION + 1 |
| 0x0000000A | vertical scroll: `BE16 amt1; BE16 amt2; int16 0` | 120 = one notch |
| 0x55000001 | hscroll: `BE16 amount` | Sunshine extension |
| 0x00000017 | UTF-8 text: `char text[size-4]` (≤ 32 bytes) | |
| 0x0000000C | multi-controller (**34 bytes** total, all 16-bit fields + 2 trigger bytes, offsets from packet start: 8 headerB, 10 controllerNumber, 12 activeGamepadMask, 16 buttonFlags, 18 LT, 19 RT, 20 LSX, 22 LSY, 24 RSX, 26 RSY, 30 buttonFlags2 — checked against MCC Input.h and tested-compiled 2026-10-14; an earlier "38 bytes" here was wrong): `LE16 headerB; LE16 controllerNumber; LE16 activeGamepadMask; LE16 midB=0x0014; LE16 buttonFlags; u8 LT; u8 RT; LE16 LSX, LSY, RSX, RSY; LE16 tailA=0x009C; LE16 buttonFlags2 (bits 16-31 of buttons); LE16 tailB=0x0055` | buttons: UP 0x1 DOWN 0x2 LEFT 0x4 RIGHT 0x8 PLAY(Start) 0x10 BACK 0x20 LS 0x40 RS 0x80 LB 0x100 RB 0x200 SPECIAL(Guide) 0x400 A 0x1000 B 0x2000 X 0x4000 Y 0x8000, plus PADDLE1-4 0x10000-0x80000, TOUCHPAD 0x100000, MISC 0x200000 |
| 0x0000000D | "enable haptics": `LE16 enable` | sent once at start (≥7.1); ignore |
| 0x55000004 | controller arrival: `u8 num; u8 type; LE16 caps; LE32 supportedButtons` | |
| 0x55000002 / 03 / 05 / 06 / 07 | touch / pen / controller touch / motion / battery | only if advertised via `x-ss-general.featureFlags` (touch/pen) or controller caps; optional |

For Kodi: keyboard VK codes → Linux keycodes (uinput) or Kodi JSON-RPC `Input.*`, plus the
gamepad (Moonlight Android TV maps the remote to keys/gamepad) **[design note]**.

---

## 7. Session lifecycle [src]

1. **Launch:** HTTPS `/launch` → host stores `{rikey, rikeyid, ping payload, connect data,
   mode, surroundAudioInfo}` as the *pending* session and answers with `sessionUrl0`. App state
   becomes BUSY, `currentgame = appid`.
2. **RTSP:** within 10 s; ANNOUNCE creates the stream session, PLAY answers OK.
3. **Streams:** the client starts audio pings (during RTSP), then the control ENet connect
   (immediately sends 0x0302 + 0x0307), then the video receiver + pings (MCC Connection.c
   `LiStartConnection` stage order: audio init → RTSP → control init → video init → input init →
   control start → video start → audio start → input start). The host starts sending video on
   the first video ping (first frame IDR) and audio on the first audio ping.
   Sunshine clears the pending launch session once the control peer connects
   (`launch_session_clear` in `get_session`).
4. **Liveness:** Sunshine's `pingTimeout` (default **10 s**) is refreshed by any ENet event of
   the session. When it expires, the session stops (SUN `controlBroadcastThread`). The client's
   ENet peer timeout is also 10 s.
5. **Client disconnect without quitting** (back button / "disconnect"): ENet DISCONNECT →
   Sunshine stops the *stream* session but **keeps the app running** (`currentgame` ≠ 0, BUSY).
   The client then shows "Resume"/"Quit" and uses `/resume` (new rikey, full RTSP handshake
   again).
6. **Quit:** HTTPS `/cancel` → stop all sessions + app, `currentgame = 0`, FREE.
7. **Host-initiated end:** send 0x0109 with `0x80030023` to the peer, flush, then
   `enet_peer_disconnect_now` (SUN end of `controlBroadcastThread`). The client stops at once
   on the termination message, without waiting for an ENet disconnect.
8. If the app itself exits (n/a for Kodi), Sunshine ends the control loop and sends termination.

For the Cube, "app" = a dummy "Kodi" entry that runs all the time: `/launch` doesn't start
anything; `/cancel` just stops the stream and resets `currentgame`. You could also always
report FREE after a stream ends, so the client launches instead of resuming. Both are valid;
resume just costs one more round trip.

---

## 8. Minimum feature set, optional features, pitfalls

### 8.1 Mandatory for a working H.264 stereo stream with current clients

1. HTTP :47989 `/serverinfo` + `/pair` (phases 1–4), HTTPS :47984 with client-cert check:
   `/serverinfo`, `/pair?phrase=pairchallenge`, `/applist`, `/launch`, `/resume`, `/cancel`.
2. RSA-2048 self-signed host cert + key, persistent; persistent paired-client PEM list and
   uniqueid.
3. RTSP :48010, one request per connection with close: OPTIONS, DESCRIBE (with
   `encryptionSupported/Requested`), 3× SETUP (Session, Transport server_port, X-SS-Ping-Payload,
   X-SS-Connect-Data), ANNOUNCE parsing (packetSize, fps, resolution, bitrate, audio channels,
   packetDuration, encryptionEnabled, featureFlags 0x20, maxNumReferenceFrames), PLAY.
4. UDP video: ping → peer; RTP + NV_VIDEO_PACKET packetization, 8-byte frame header,
   frameIndex from 1, continuous sequence numbers, ≤ 255 data shards per block, ≤ 4 blocks,
   fecInfo consistent (fec% 0 is fine), SPS+PPS on every IDR, IDR on request.
5. UDP audio: ping → peer; RTP PT 97 Opus CBR with `packetDuration`-ms frames, ms timestamps,
   AES-128-CBC encryption (IV = rikeyid + seq) whenever enabled.
6. ENet control with AES-128-GCM wrapper 0x0001 (V2 IV), handling 0x0302 (IDR) and 0x0200
   (liveness), ignoring unknown types, sending 0x0109 on shutdown.

### 8.2 Optional / negotiable

| Feature | How to turn it off |
|---|---|
| mDNS `_nvstream._tcp` | just don't (manual add by IP); cube-stream does announce it via Avahi |
| `/appasset` box art | return a small PNG or a 404 XML |
| Encrypted RTSP | return `rtsp://` |
| Video encryption | don't advertise `SS_ENC_VIDEO` |
| Video FEC | fec% = 0 in `fecInfo` |
| Audio FEC | don't send PT 127 packets |
| RFI / LTR | don't advertise `refPicInvalidation` |
| HEVC / AV1 / YUV444 / HDR | `ServerCodecModeSupport=1`, no `sprop-parameter-sets`/`rtpmap:98` lines (cube-stream offers HEVC Main, not AV1/YUV444/HDR) |
| Surround (5.1) | advertise the `642012453` line and encode 6-ch multistream when `numChannels=6`; otherwise the client still asks, and failing politely needs care **[unverified: if the client asks for 5.1 and the host sends stereo Opus with a 6-ch decoder config, decoding fails]**, so implement 5.1 or tell users to choose stereo |
| Touch/pen/motion/rumble/LEDs | `x-ss-general.featureFlags:0`, never send H→C feedback |
| Frame processing latency, PTS | send 0s (the client makes up PTS) |
| `x-ml` FEC status, loss stats | ignore |

### 8.3 Pitfalls

- **The appversion string gates the protocol** (MCC Limelight-internal.h `APP_VERSION_AT_LEAST`,
  `IS_SUNSHINE`):
  - `7.x.y` with `y < 404` → RTSP over **ENet** (`useEnet`), not TCP.
  - `≥ 7.1.415` → periodic ping 0x0200 and frame-header type byte (0x01 = 8 bytes).
  - `≥ 7.1.431` → encrypted control stream mandatory, `streamid=control/13/0`, single `PLAY /`,
    multi-FEC, IDR request 0x0302.
  - `≥ 7.1.446` → DRC hacks for small resolutions.
  - `≥ 7.1.450` → 44-byte long headers when byte0 = 0x81.
  - 4th field < 0 → Sunshine extensions (x-ml featureFlags, configured bitrate, encryption
    negotiation, X-SS ping payload/connect data, FEC status, host latency).

  Use exactly **`7.1.431.-1`**. It is what Sunshine/wolf send, so it is what clients are tested
  against.
- The pairing hash depends on appversion major ≥ 7 → SHA-256. The client pads the hash to 32
  bytes before AES-ECB.
- **RTSP responses need ≥ 1 header line and the socket must be closed after each response.**
- `Session:` in SETUP audio is mandatory. Return the same token each time; the client keeps the
  part before `;`.
- `X-SS-Ping-Payload` must be exactly 16 characters, or the client sends legacy `PING` pings and
  you have to match by IP.
- **frameIndex must start at 1**; RTP sequence numbers continuous across frames and FEC blocks;
  streamPacketIndex = (wide packet counter) << 8, 24 bits — not the wrapping 16-bit seq.
- Non-final data packets must carry exactly `packetSize-16` payload bytes.
- IDR without inline SPS/PPS → the client never leaves "waiting for IDR".
- **Encode with exactly 1 reference frame** when ANNOUNCE says `maxNumReferenceFrames=1`
  (moonlight-qt with D3D11VA does). moonlight-qt applies an "H.264 SPS fixup" for low-delay
  output; a stream whose P frames reference 2 frames (Wave521 default preset IPP, SPS refs=2)
  then fails on every 3rd frame ("reference picture missing during reorder") → IDR request
  storm. **[tested 2026-10-14]** Fix on the Cube: `gop_pattern = 8` (IPP_SINGLE).
- Opus must be CBR if you send audio FEC; the audio RTP timestamp is in **ms**.
- The client discards the first 500 ms of audio. The client gives up after **10 s without
  video**.
- Audio encryption is effectively always on with moonlight-qt/iOS (featureFlags 0x20), even if
  the host never advertised it.
- Control GCM IV byte order: seq **LE** in bytes 0–3 (both sides), `'C','C'` / `'H','C'` in
  bytes 10–11.
- moonlight-qt treats a still-nonzero `currentgame` after `/cancel` as an error (599).
- Never regenerate the host cert or uniqueid (clients pin both).
- moonlight-qt requests `/serverinfo` every few seconds while the PC list is visible
  **[src: polling, interval unverified]**. Keep it cheap.

---

## 9. Reusable libraries, licenses, size estimate

| Component | Source | License | Use |
|---|---|---|---|
| ENet (moonlight fork) | https://github.com/cgutman/enet | MIT | control stream server; vendor as-is (~5 kLOC C) |
| nanors | https://github.com/sleepybishop/nanors | MIT | `reed_solomon_*` for video FEC and audio FEC (single `rs.c` + `oblas` headers, ~1–2 kLOC) |
| OpenSSL (1.1/3.x) | system (CoreELEC ships libssl) **[unverified on the box]** | Apache-2.0 | TLS server, X.509 gen/compare, RSA-SHA256 sign/verify, AES-ECB/CBC/GCM, SHA-256, RAND |
| libopus | already built for cube-stream | BSD | `opus_multistream_encoder_*` |
| moonlight-common-c | MCC | **GPL-3.0** | reference only. Its headers (`Video.h`, `Input.h`, `Limelight.h` constants, `RtpAudioQueue.h` `AUDIO_FEC_HEADER`) are the best spec; copying struct definitions makes our host GPL-3 **[licensing judgement, not legal advice]**. Writing our own packed structs from this spec avoids that |
| Sunshine | SUN | **GPL-3.0** | reference only (whole-project GPL). Also uses Boost/FFmpeg, so not practical to vendor |
| wolf | WOLF | **MIT** | cleaner, permissive reference; parts of `moonlight-protocol/` (pairing, XML, FEC wrapper) could be ported with attribution. It is C++/Boost/GStreamer, so port, don't link |
| Simple HTTP | write your own (~200 lines; GET-only, query parsing) or civetweb (MIT) | | |

Builds also contain Amlogic-SLA encoder code, so cube-moonlight publishes source only. That is a
separate constraint from GPL linking; the host is self-written from this spec (no copied GPL
code), which keeps it MIT.

**Estimate for a minimal C host** (excluding ENet, nanors, OpenSSL, libopus, and the existing
cube-stream capture/encode code):

| Module | LOC (approx.) |
|---|---|
| HTTP/HTTPS server (OpenSSL, GET + query parse, XML output) | 400–500 |
| Pairing state machine, cert generation, paired-cert store, PIN input | 350–450 |
| RTSP server + DESCRIBE/SETUP/ANNOUNCE/PLAY | 300–400 |
| Video packetizer + multi-block FEC + pacing/sendmmsg | 300–400 |
| Audio RTP + AES-CBC + RS 4+2 | 150–250 |
| ENet control: AES-GCM wrap/unwrap, dispatch, termination | 250–350 |
| Input decode → uinput / Kodi JSON-RPC | 250–500 |
| Session/lifecycle/config/glue | 300–400 |
| **Total** | **≈ 2,300–3,250 lines of C** (plus ~7 kLOC vendored MIT code) |

---

## Appendix A: worked byte examples

**Control stream: client IDR request (first message), V2 encryption, seq = 0:**
```
plaintext (6 bytes): 02 03 02 00 00 00          (LE16 0x0302, LE16 len=2, payload 00 00)
IV (12): 00 00 00 00 00 00 00 00 00 00 43 43                 ('C','C')
wire:    01 00 | 1a 00 | 00 00 00 00 | <16-byte tag> | <6 bytes ciphertext>
         type    len=4+16+6=26  seq
```

**Host termination (graceful), host seq = 7:**
```
plaintext: 09 01 04 00 80 03 00 23          (LE16 0x0109, LE16 4, BE32 0x80030023)
IV: 07 00 00 00 00 00 00 00 00 00 48 43     ('H','C')
wire: 01 00 1c 00 07 00 00 00 <tag16> <8 bytes ct>
```

**Video, frame 1, first datagram, packetSize 1392, 20 % FEC, 10 data shards → parity 2
(`(10*20+99)/100 = 2`, ≥ minRequired 2):**
```
90 00 00 00 | ts BE32 | 00 00 00 00 | 00 00 00 00 |
00 00 00 00 (spi = 0<<8) | 01 00 00 00 (frame 1) | 05 (PIC|SOF) | 00 | 10 | 00 |
fecInfo = 0<<12 | 10<<22 | 20<<4 = 0x02800140 → LE: 40 01 80 02 |
01 00 00 02 <lastPayloadLen LE16> 00 00 | 00 00 00 01 67 … (SPS) …
```

**Audio, seq 5, rikeyid = 0x12345678, encrypted:**
`IV = BE32(0x12345678 + 5) = 12 34 56 7D` followed by 12 zero bytes; AES-128-CBC/PKCS7 of the Opus bytes.

---

## Appendix B: source file index (for verification)

- Sunshine: `nvhttp.cpp` (`serverinfo`, `pair`, `getservercert`, `clientchallenge`,
  `serverchallengeresp`, `clientpairingsecret`, `applist`, `launch`, `resume`, `cancel`,
  `appasset`, `make_launch_session`, `start`), `nvhttp.h` (`VERSION`, `GFE_VERSION`, ports),
  `crypto.cpp` (`gen_aes_key`, `hash`, `signature`, `sign256`, `verify256`, `gen_creds`,
  `cert_chain_t::verify`, cipher classes), `rtsp.cpp` (`respond`, `cmd_option`, `cmd_describe`,
  `cmd_setup`, `cmd_announce`, `cmd_play`, `handle_msg`), `stream.cpp` (`packetTypes`,
  `video_short_frame_header_t`, `video_packet_raw_t`, `encode_control`, `control_server_t`,
  `controlBroadcastThread`, `recvThread`, `videoBroadcastThread`, `fec::encode`,
  `audioBroadcastThread`, `recv_ping`, `session::alloc`), `audio.cpp` (`stream_configs`,
  `encodeThread`), `cbs.cpp` (`make_sps_h264`, `validate_sps`), `video.cpp` (`frame_nr = 1`,
  SPS replacement), `input.cpp` (`passthrough_helper`, keyboard/mouse handlers),
  `network.cpp` (`map_port`, `host_create`), `config.cpp` (defaults: ping_timeout 10 s, FEC 20 %,
  LAN encryption NEVER).
- moonlight-common-c: `RtspConnection.c` (`performRtspHandshake`, `transactRtspMessageTcp`,
  `sealRtspMessage`, `parseOpusConfigurations`, `parseServerPortFromTransport`),
  `RtspParser.c`, `SdpGenerator.c` (`getAttributesList`, `addGen5Options`, `NVFF_*`),
  `ControlStream.c` (`packetTypesGen7Enc`, `encryptControlMessage`,
  `decryptControlMessageToV1`, `controlReceiveThreadFunc`, `lossStatsThreadFunc`,
  `startControlStream`), `Video.h`, `VideoStream.c`, `RtpVideoQueue.c` (`RtpvAddPacket`,
  `reconstructFrame`), `VideoDepacketizer.c` (`processRtpPayload`), `AudioStream.c`,
  `RtpAudioQueue.[ch]`, `Input.h`, `InputStream.c`, `Limelight.h`, `Limelight-internal.h`,
  `Connection.c` (`LiGetLaunchUrlQueryParameters`, stage order).
- moonlight-qt: `backend/nvhttp.cpp` (`openConnection`, `getServerInfo`, `startApp`,
  `quitApp`, `getAppList`, `verifyResponseStatus`), `backend/nvpairingmanager.cpp` (`pair`),
  `backend/nvcomputer.cpp` (serverinfo field parsing), `settings/compatfetcher.cpp`,
  `streaming/session.cpp` (encryptionFlags, packetSize 1392/1024).
- moonlight-android: `nvstream/http/PairingManager.java`, `nvstream/StreamConfiguration.java`.
- wolf: `moonlight-protocol/moonlight.cpp` (serverinfo/pair/launch XML),
  `moonlight-protocol/moonlight/protocol.hpp` (`M_VERSION = 7.1.431.-1`),
  `moonlight-server/rtsp/commands.hpp` (DESCRIBE/SETUP/ANNOUNCE),
  `moonlight-server/gst-plugin/video.hpp` (packetizer/FEC),
  `moonlight-protocol/moonlight/control.hpp`, `moonlight-server/control/control.cpp`;
  protocol notes in `docs/modules/protocols/pages/*.adoc`.

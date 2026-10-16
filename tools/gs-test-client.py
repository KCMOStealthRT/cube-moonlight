#!/usr/bin/env python3
"""gs-test-client — PC-side test of the Cube's Moonlight host HTTP part (gamestream.c).

Pairs like moonlight-qt (NvPairingManager: SHA-256, AES-128-ECB, RSA-SHA256), writes the PIN
to the box over ssh, then exercises HTTPS serverinfo / applist / launch / cancel.
Needs `cryptography` (pip). Client identity is kept in --state (default: ./gs-client; it is a
paired credential for your box - keep it out of git).

  python gs-test-client.py --host IP pair     # full pairing (+ PIN via ssh root@IP)
  python gs-test-client.py --host IP check    # HTTPS calls with the stored client cert
  python gs-test-client.py --host IP launch   # /launch, show serverinfo, /cancel
  python gs-test-client.py --host IP stream   # /launch, RTSP, receive + check RTP video and
                                              # audio (incl. FEC parity), /cancel
--host defaults to $CUBE_HOST; --ssh-key adds "-i KEY" to ssh.
"""
import argparse, datetime, hashlib, http.client, os, re, secrets, ssl, subprocess, sys, threading
import urllib.parse, uuid
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.x509.oid import NameOID

HOST = None          # set from --host
SSH = []
PIN_FILE = "/storage/cube-moonlight/moonlight/pin"


def aes_ecb(key, data, enc):
    c = Cipher(algorithms.AES(key), modes.ECB())
    o = c.encryptor() if enc else c.decryptor()
    return o.update(data) + o.finalize()


def gf_tables():
    exp, log, x = [0] * 512, [0] * 256, 1
    for i in range(255):
        exp[i] = exp[i + 255] = x
        log[x] = i
        x <<= 1
        if x & 0x100:
            x ^= 0x11d
    def mul(a, b):
        return 0 if a == 0 or b == 0 else exp[log[a] + log[b]]
    inv = [0] + [exp[255 - log[a]] for a in range(1, 256)]
    tables = [bytes(mul(c, v) for v in range(256)) for c in range(256)]
    return inv, tables


def rs_parity(data, ps, matrix=None):
    """parity shards of equal-length byte strings `data`, as nanors reed_solomon_encode"""
    inv, tables = GF
    ds, n = len(data), len(data[0])
    out = []
    for j in range(ps):
        acc = 0
        for i in range(ds):
            c = matrix[j * ds + i] if matrix else inv[(ps + i) ^ j]
            acc ^= int.from_bytes(data[i].translate(tables[c]), "little")
        out.append(acc.to_bytes(n, "little"))
    return out


GF = gf_tables()
AUDIO_FEC_MATRIX = bytes([0x77, 0x40, 0x38, 0x0e, 0xc7, 0xa7, 0x0d, 0x6c])


def tag(xml, name):
    m = re.search(r"<%s>(.*?)</%s>" % (name, name), xml, re.S)
    return m.group(1) if m else None


def client_identity(state):
    os.makedirs(state, exist_ok=True)
    kp, cp = os.path.join(state, "client.key"), os.path.join(state, "client.pem")
    if not os.path.exists(kp):
        key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "NVIDIA GameStream Client")])
        now = datetime.datetime.now(datetime.timezone.utc)
        cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name)
                .public_key(key.public_key()).serial_number(x509.random_serial_number())
                .not_valid_before(now).not_valid_after(now + datetime.timedelta(days=7300))
                .sign(key, hashes.SHA256()))
        open(kp, "wb").write(key.private_bytes(serialization.Encoding.PEM,
                                               serialization.PrivateFormat.PKCS8,
                                               serialization.NoEncryption()))
        open(cp, "wb").write(cert.public_bytes(serialization.Encoding.PEM))
    key = serialization.load_pem_private_key(open(kp, "rb").read(), None)
    pem = open(cp, "rb").read()
    return kp, cp, key, x509.load_pem_x509_certificate(pem), pem


class Client:
    def __init__(self, state):
        self.state = state
        self.kp, self.cp, self.key, self.cert, self.pem = client_identity(state)
        self.uniqueid = "0123456789ABCDEF"

    def get(self, https, path, **params):
        q = urllib.parse.urlencode(dict(uniqueid=self.uniqueid, uuid=uuid.uuid4().hex, **params))
        if https:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE         # pinned below, like moonlight-qt
            ctx.load_cert_chain(self.cp, self.kp)
            c = http.client.HTTPSConnection(HOST, 47984, context=ctx, timeout=330)
        else:
            c = http.client.HTTPConnection(HOST, 47989, timeout=330)
        c.request("GET", path + "?" + q)
        der = c.sock.getpeercert(binary_form=True) if https else None
        r = c.getresponse()
        body = r.read().decode(errors="replace")
        if https:
            pinned = os.path.join(self.state, "server.pem")
            if os.path.exists(pinned):
                srv = x509.load_pem_x509_certificate(open(pinned, "rb").read())
                assert der == srv.public_bytes(serialization.Encoding.DER), "server cert changed!"
        c.close()
        m = re.search(r'status_code="(\d+)"', body)
        return r.status, int(m.group(1)) if m else None, body

    def pair(self, pin_via_ssh=True):
        pin = "%04d" % secrets.randbelow(10000)
        salt = secrets.token_bytes(16)
        aes = hashlib.sha256(salt + pin.encode()).digest()[:16]
        print("PIN", pin)
        if pin_via_ssh:
            def send_pin():
                import time; time.sleep(1.5)
                subprocess.run(SSH + ["echo %s > %s" % (pin, PIN_FILE)],
                               stdin=subprocess.DEVNULL, check=True)
            threading.Thread(target=send_pin, daemon=True).start()
        else:
            print("write it on the box: echo %s > %s" % (pin, PIN_FILE))

        _, st, b = self.get(False, "/pair", devicename="gs-test", updateState=1,
                            phrase="getservercert", salt=salt.hex(), clientcert=self.pem.hex())
        assert tag(b, "paired") == "1", b
        srv_pem = bytes.fromhex(tag(b, "plaincert"))
        srv = x509.load_pem_x509_certificate(srv_pem)
        print("1 getservercert ok:", srv.subject.rfc4514_string(), srv.signature_hash_algorithm.name)

        challenge = secrets.token_bytes(16)
        _, st, b = self.get(False, "/pair", devicename="gs-test", updateState=1,
                            clientchallenge=aes_ecb(aes, challenge, True).hex())
        assert tag(b, "paired") == "1", b
        dec = aes_ecb(aes, bytes.fromhex(tag(b, "challengeresponse")), False)
        server_hash, server_challenge = dec[:32], dec[32:48]
        print("2 clientchallenge ok")

        client_secret = secrets.token_bytes(16)
        h = hashlib.sha256(server_challenge + self.cert.signature + client_secret).digest()
        _, st, b = self.get(False, "/pair", devicename="gs-test", updateState=1,
                            serverchallengeresp=aes_ecb(aes, h, True).hex())
        assert tag(b, "paired") == "1", b
        ps = bytes.fromhex(tag(b, "pairingsecret"))
        server_secret, server_sig = ps[:16], ps[16:]
        srv.public_key().verify(server_sig, server_secret, padding.PKCS1v15(), hashes.SHA256())
        assert hashlib.sha256(challenge + srv.signature + server_secret).digest() == server_hash, \
            "incorrect PIN (server hash mismatch)"
        print("3 serverchallengeresp ok: server signature + hash verified")

        sig = self.key.sign(client_secret, padding.PKCS1v15(), hashes.SHA256())
        _, st, b = self.get(False, "/pair", devicename="gs-test", updateState=1,
                            clientpairingsecret=(client_secret + sig).hex())
        assert tag(b, "paired") == "1", b
        print("4 clientpairingsecret ok")
        open(os.path.join(self.state, "server.pem"), "wb").write(srv_pem)

        _, st, b = self.get(True, "/pair", devicename="gs-test", updateState=1,
                            phrase="pairchallenge")
        assert tag(b, "paired") == "1", b
        print("5 pairchallenge (HTTPS, client cert) ok -> paired")

    def check(self):
        for https in (False, True):
            hs, st, b = self.get(https, "/serverinfo")
            print("serverinfo %s: http %d status %s PairStatus %s state %s appversion %s"
                  % ("https" if https else "http", hs, st, tag(b, "PairStatus"), tag(b, "state"),
                     tag(b, "appversion")))
        hs, st, b = self.get(True, "/applist")
        print("applist:", st, re.findall(r"<AppTitle>(.*?)</AppTitle><ID>(\d+)</ID>", b))

    def launch(self):
        rikey = secrets.token_bytes(16)
        hs, st, b = self.get(True, "/launch", appid=1, mode="1920x1080x60", additionalStates=1,
                             sops=0, rikey=rikey.hex(), rikeyid=12345, localAudioPlayMode=0,
                             surroundAudioInfo=196610, remoteControllersBitmap=0, gcmap=0,
                             gcpersist=0, corever=1)
        print("launch:", st, tag(b, "sessionUrl0"), "gamesession", tag(b, "gamesession"))
        hs, st, b = self.get(True, "/serverinfo")
        print("  serverinfo: currentgame", tag(b, "currentgame"), tag(b, "state"))
        hs, st, b = self.get(True, "/cancel")
        print("cancel:", st, tag(b, "cancel"))
        hs, st, b = self.get(True, "/serverinfo")
        print("  serverinfo: currentgame", tag(b, "currentgame"), tag(b, "state"))

    def stream(self, seconds, out, packet_size=1392, mode="1920x1080x60", kbps=20000, channels=2,
               codec="h264"):
        """/launch + RTSP handshake + video pings; receive and check RTP video (no ENet, so
        the host keeps the stream alive on our pings), write the H.264 to `out`, /cancel."""
        w, h, fps = mode.split("x")
        rikey = self.rikey = secrets.token_bytes(16)
        self.rikeyid = 12345
        hs, st, b = self.get(True, "/launch", appid=1, mode=mode, additionalStates=1, sops=0,
                             rikey=rikey.hex(), rikeyid=self.rikeyid, localAudioPlayMode=0,
                             surroundAudioInfo={2: 0x3, 6: 0x3f, 8: 0x63f}[channels] << 16 | channels,
                             remoteControllersBitmap=0, gcmap=0, gcpersist=0, corever=1)
        url = tag(b, "sessionUrl0")
        print("launch:", st, url)
        try:
            self._stream(url, seconds, out, packet_size, w, h, fps, kbps, channels, codec)
        finally:
            hs, st, b = self.get(True, "/cancel")
            print("cancel:", st, tag(b, "cancel"))

    def _stream(self, url, seconds, out, packet_size, w, h, fps, kbps, channels=2, codec="h264"):
        import socket, struct, time
        cseq = [0]

        def rtsp(method, target, headers=(), body=""):
            cseq[0] += 1
            s = socket.create_connection((HOST, 48010), timeout=5)
            msg = "%s %s RTSP/1.0\r\nCSeq: %d\r\nX-GS-ClientVersion: 14\r\nHost: %s\r\n" % (
                method, target, cseq[0], HOST)
            for hd in headers:
                msg += hd + "\r\n"
            if body:
                msg += "Content-type: application/sdp\r\nContent-length: %d\r\n" % len(body)
            s.sendall((msg + "\r\n" + body).encode())
            data = b""
            while True:                         # the host closes after its response
                d = s.recv(65536)
                if not d:
                    break
                data += d
            s.close()
            head, _, payload = data.decode().partition("\r\n\r\n")
            lines = head.split("\r\n")
            hdrs = dict(l.split(": ", 1) for l in lines[1:] if ": " in l)
            print("rtsp %-8s -> %s %s" % (method, lines[0], {k: v for k, v in hdrs.items() if k != "CSeq"}))
            assert lines[0].startswith("RTSP/1.0 200"), data
            return hdrs, payload

        rtsp("OPTIONS", url)
        _, sdp = rtsp("DESCRIBE", url, ["Accept: application/sdp"])
        print("  DESCRIBE payload:", sdp.strip().replace("\n", " | "))
        hevc = codec == "hevc"
        if hevc:   # moonlight-common-c falls back to H.264 without this exact string
            assert "sprop-parameter-sets=AAAAAU" in sdp, "host doesn't offer HEVC"
        if channels == 2:
            self.opus_layout = (2, 1, 1, [0, 1])
        else:
            pre = "a=fmtp:97 surround-params=%d" % channels
            assert pre in sdp, "no %d-channel surround-params line (client would fail)" % channels
            digits = sdp.split(pre, 1)[1]
            st_, cp_ = int(digits[0]), int(digits[1])
            adv = [int(c) for c in digits[2:2 + channels]]
            m = adv[:3] + [adv[channels - 1]] + adv[3:channels - 1]   # the client's rotation
            self.opus_layout = (channels, st_, cp_, m)
            print("  %d-channel layout: %d streams, %d coupled, client mapping %s%s" % (
                channels, st_, cp_, m, "" if m == list(range(channels)) else "  <-- NOT identity"))
        ha, _ = rtsp("SETUP", "streamid=audio/0/0", ["Transport: unicast;X-GS-ClientPort=50000-50001"])
        session = ha["Session"].split(";")[0]
        payload = ha["X-SS-Ping-Payload"].encode()
        assert len(payload) == 16
        hv, _ = rtsp("SETUP", "streamid=video/0/0", ["Session: " + session,
                                                     "Transport: unicast;X-GS-ClientPort=50000-50001"])
        rtsp("SETUP", "streamid=control/13/0", ["Session: " + session,
                                                "Transport: unicast;X-GS-ClientPort=50000-50001"])
        attrs = {"x-ml-general.featureFlags": 3, "x-ss-general.encryptionEnabled": 1,
                 "x-nv-video[0].clientViewportWd": w, "x-nv-video[0].clientViewportHt": h,
                 "x-nv-video[0].maxFPS": fps, "x-nv-video[0].packetSize": packet_size,
                 "x-nv-video[0].rateControlMode": 4, "x-nv-video[0].timeoutLengthMs": 7000,
                 "x-ml-video.configuredBitrateKbps": kbps,
                 "x-nv-vqos[0].bw.maximumBitrateKbps": int(kbps * 0.8),
                 "x-nv-general.featureFlags": 0xa7, "x-nv-general.useReliableUdp": 13,
                 "x-nv-video[0].videoEncoderSlicesPerFrame": 1, "x-nv-vqos[0].bitStreamFormat": int(hevc),
                 "x-nv-clientSupportHevc": int(hevc),
                 "x-nv-video[0].maxNumReferenceFrames": 0, "x-nv-audio.surround.numChannels": channels,
                 "x-nv-audio.surround.channelMask": {2: 0x3, 6: 0x3f, 8: 0x63f}[channels],
                 "x-nv-audio.surround.enable": int(channels > 2),
                 "x-nv-audio.surround.AudioQuality": 0, "x-nv-aqos.packetDuration": 5,
                 "x-nv-vqos[0].fec.minRequiredFecPackets": 2}
        sdpo = "v=0\r\no=android 0 14 IN IPv4 %s\r\ns=NVIDIA Streaming Client\r\n" % HOST
        sdpo += "".join("a=%s:%s \r\n" % kv for kv in attrs.items()) + "t=0 0\r\nm=video 47998  \r\n"
        rtsp("ANNOUNCE", "streamid=control/13/0", ["Session: " + session], sdpo)
        rtsp("PLAY", "/", ["Session: " + session])

        vs = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        vs.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)
        vs.bind(("", 0))
        vs.settimeout(0.5)
        as_ = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        as_.bind(("", 0))
        stop = threading.Event()

        def pinger():
            n = 0
            while not stop.is_set():
                n += 1
                vs.sendto(payload + struct.pack(">I", n), (HOST, 47998))
                as_.sendto(payload + struct.pack(">I", n), (HOST, 48000))
                stop.wait(0.5)
        threading.Thread(target=pinger, daemon=True).start()

        audio = []                               # (seq, ts, payload)
        audio_fec = []                           # (seq, payload incl. FEC header)

        def audio_rx():
            as_.settimeout(0.5)
            while not stop.is_set():
                try:
                    d, _ = as_.recvfrom(2048)
                except socket.timeout:
                    continue
                if len(d) > 12 and d[1] == 97:
                    seq, ts = struct.unpack(">HI", d[2:8])
                    audio.append((seq, ts, d[12:]))
                elif len(d) > 24 and d[1] == 127:
                    audio_fec.append((struct.unpack(">H", d[2:4])[0], d[12:]))
        threading.Thread(target=audio_rx, daemon=True).start()

        P = packet_size - 16
        frames, cur, bad = {}, None, []
        last_seq, last_spi, lost, pkts, nbytes = None, None, 0, 0, 0
        t0 = first_t = None
        t_end = time.time() + seconds
        while time.time() < t_end:
            try:
                d, _ = vs.recvfrom(4096)
            except socket.timeout:
                continue
            now = time.time()
            if first_t is None:
                first_t = now
                print("first video packet after ping start")
            pkts += 1
            nbytes += len(d)
            if len(d) != packet_size + 16 and not (d[24] & 2):
                bad.append("size %d" % len(d))
            hdr, ptype, seq, ts = struct.unpack(">BBHI", d[:8])
            spi, fidx, flags, extra, mff, mfb, fec = struct.unpack("<IIBBBBI", d[16:32])
            shard, data_shards, fecpct = fec >> 12 & 0x3ff, fec >> 22, fec >> 4 & 0xff
            parity = (data_shards * fecpct + 99) // 100   # the client's formula
            blk, nblk = mfb >> 4 & 3, (mfb >> 6) + 1
            if hdr != 0x90 or shard >= data_shards + parity or (parity and data_shards + parity > 255):
                bad.append("hdr %02x fec %x" % (hdr, fec))
            if shard < data_shards:
                # streamPacketIndex: 24-bit counter whose low 16 bits are the RTP seq; +1 inside
                # a block, may jump forward (over parity) at a block start
                sof = shard == 0
                if last_spi is None:
                    step_ok = True
                elif sof:
                    step_ok = 0 < ((spi >> 8) - last_spi) & 0xffffff < 0x800000
                else:
                    step_ok = spi >> 8 == (last_spi + 1) & 0xffffff
                if mff != 0x10 or spi & 0xff or (spi >> 8) & 0xffff != seq or not step_ok or \
                        bool(flags & 4) != sof or bool(flags & 2) != (shard == data_shards - 1):
                    bad.append("mff %02x spi %x seq %d flags %x" % (mff, spi, seq, flags))
                last_spi = spi >> 8
            if last_seq is not None and seq != (last_seq + 1) & 0xffff:
                lost += (seq - last_seq - 1) & 0xffff
            last_seq = seq
            f = frames.setdefault(fidx, {"parts": {}, "ts": ts, "t": now})
            f["parts"][(blk, shard)] = (d, flags, data_shards, nblk, parity, fecpct)
        stop.set()

        idxs = sorted(frames)
        fec_stats = {"blocks": 0, "data": 0, "parity": 0, "pct": set(), "verified": 0}
        complete, missing_frames, au = 0, 0, open(out, "wb")
        sizes, prev = [], None
        first_sps = None
        for i in idxs:
            if prev is not None and i != prev + 1:
                missing_frames += i - prev - 1
            prev = i
            parts = frames[i]["parts"]
            nblk = next(iter(parts.values()))[3]
            need = {}
            for (blk, shard), (_, flags, ds, nb, par, pct) in parts.items():
                need[blk] = (ds, par, pct)
            fec_stats["blocks"] += len(need)
            for blk, (ds, par, pct) in need.items():
                fec_stats["data"] += ds
                fec_stats["parity"] += par
                fec_stats["pct"].add(pct)
                if not par:
                    continue
                got = [parts.get((blk, x)) for x in range(ds + par)]
                if all(got):                     # recompute the parity from the data shards
                    calc = rs_parity([g[0] for g in got[:ds]], par)
                    for x in range(par):
                        # bytes the client rebuilds itself (RTP header, frameIndex, multiFecBlocks,
                        # fecInfo) carry the parity shard's own header
                        a, b = got[ds + x][0], calc[x]
                        if a[12:20] + a[24:27] + a[32:] != b[12:20] + b[24:27] + b[32:]:
                            bad.append("frame %d block %d parity %d wrong" % (i, blk, x))
                            break
                    else:
                        fec_stats["verified"] += 1
            data_keys = sorted(k for k in parts if k[1] < need[k[0]][0])
            if len(need) != nblk or sum(v[0] for v in need.values()) != len(data_keys):
                continue                         # incomplete frame
            data = b"".join(parts[k][0][32:] for k in data_keys)
            if data[0] != 1:
                bad.append("frame %d header type %02x" % (i, data[0]))
                continue
            ftype = data[3]
            body = data[8:]
            if first_sps is None:
                first_sps = body[:5]
            if ftype == 2:    # IDR: the client wants it to start with SPS (H.264) / VPS (HEVC)
                sc = 4 if body[:4] == b"\0\0\0\1" else 3 if body[:3] == b"\0\0\1" else 0
                nal = (body[sc] >> 1 & 0x3f) if hevc else body[sc] & 0x1f
                if not sc or nal != (32 if hevc else 7):
                    bad.append("frame %d IDR starts with NAL type %d" % (i, nal))
            sizes.append((len(body.rstrip(b"\0")), ftype, struct.unpack("<H", data[1:3])[0] / 10))
            au.write(body)
            complete += 1
        au.close()
        dur = (frames[idxs[-1]]["t"] - frames[idxs[0]]["t"]) if len(idxs) > 1 else 0
        print("received %d packets, %.1f MB, %d frames (%d complete, %d missing, %d seq lost) in %.1f s"
              % (pkts, nbytes / 1e6, len(idxs), complete, missing_frames, lost, dur))
        if idxs:
            print("frameIndex %d..%d, %.1f fps, %.0f kbit/s, IDR frames %d, first AU starts %s"
                  % (idxs[0], idxs[-1], (len(idxs) - 1) / dur if dur else 0,
                     nbytes * 8 / dur / 1000 if dur else 0, sum(1 for s in sizes if s[1] == 2),
                     first_sps.hex() if first_sps else None))
        if sizes:
            lat = sorted(s[2] for s in sizes)
            print("frame sizes max %d bytes (IDR max %d); host latency median %.1f ms, max %.1f ms"
                  % (max(s[0] for s in sizes), max([s[0] for s in sizes if s[1] == 2] or [0]),
                     lat[len(lat) // 2], lat[-1]))
        print("video FEC: %d blocks, %d data + %d parity shards, fec %% %s, %d blocks' parity verified"
              % (fec_stats["blocks"], fec_stats["data"], fec_stats["parity"],
                 sorted(fec_stats["pct"]), fec_stats["verified"]))
        print("problems:", bad[:10] if bad else "none")
        print("wrote", out)
        self._check_audio(audio, os.path.splitext(out)[0] + ".opus", audio_fec, self.opus_layout)

    def _check_audio(self, audio, out, audio_fec=(), layout=(2, 1, 1, [0, 1])):
        """decrypt (AES-128-CBC, IV = BE32(rikeyid + seq)), check RTP fields, write Ogg Opus"""
        import struct
        from cryptography.hazmat.primitives import padding as sympad
        if not audio:
            print("audio: no packets received")
            return
        bad, pkts = [], []
        for i, (seq, ts, p) in enumerate(audio):
            if i and seq != (audio[i - 1][0] + 1) & 0xffff:
                bad.append("seq gap %d->%d" % (audio[i - 1][0], seq))
            if i and ts != audio[i - 1][1] + 5:
                bad.append("ts step %d" % (ts - audio[i - 1][1]))
            iv = struct.pack(">I", (self.rikeyid + seq) & 0xffffffff) + bytes(12)
            d = Cipher(algorithms.AES(self.rikey), modes.CBC(iv)).decryptor()
            u = sympad.PKCS7(128).unpadder()
            try:
                op = u.update(d.update(p) + d.finalize()) + u.finalize()
            except ValueError:
                bad.append("decrypt seq %d" % seq)
                continue
            pkts.append(op)
        sizes = sorted(set(len(a[2]) for a in audio))
        tocs = sorted(set("%02x" % p[0] for p in pkts))
        print("audio: %d packets, seq %d..%d, ts %d..%d ms, ciphertext sizes %s, Opus TOC %s"
              % (len(audio), audio[0][0], audio[-1][0], audio[0][1], audio[-1][1], sizes, tocs))
        # FEC: 2 parity packets (seq = last data seq + 1 + shard) per block of 4 data packets
        by_seq = {seq: (ts, p) for seq, ts, p in audio}
        fec_ok = fec_blocks = 0
        for seq, f in audio_fec:
            shard, pt, base, bts, ssrc = struct.unpack(">BBHII", f[:12])
            if pt != 97 or base % 4 or shard > 1 or seq != (base + 4 + shard) & 0xffff or ssrc:
                bad.append("fec header seq %d: %s" % (seq, f[:12].hex()))
                continue
            data = [by_seq.get((base + k) & 0xffff) for k in range(4)]
            if not all(data):
                continue
            fec_blocks += 1
            if bts != data[0][0]:
                bad.append("fec base ts %d != %d" % (bts, data[0][0]))
            if rs_parity([d[1] for d in data], 2, AUDIO_FEC_MATRIX)[shard] == f[12:]:
                fec_ok += 1
            else:
                bad.append("fec parity seq %d wrong" % seq)
        print("audio FEC: %d packets, %d checked against their data blocks, %d correct"
              % (len(audio_fec), fec_blocks, fec_ok))
        print("audio problems:", bad[:10] if bad else "none")

        def crc(data):                           # Ogg CRC-32 (poly 0x04c11db7, no reflection)
            c = 0
            for b in data:
                c ^= b << 24
                for _ in range(8):
                    c = ((c << 1) ^ 0x04c11db7 if c & 0x80000000 else c << 1) & 0xffffffff
            return c

        def page(packets, granule, seqno, flags):
            lace = b""
            for p in packets:
                lace += b"\xff" * (len(p) // 255) + bytes([len(p) % 255])
            h = b"OggS" + bytes([0, flags]) + struct.pack("<qIII", granule, 1, seqno, 0) + \
                bytes([len(lace)]) + lace
            body = b"".join(packets)
            c = crc(h + body)
            return h[:22] + struct.pack("<I", c) + h[26:] + body

        with open(out, "wb") as f:
            ch, nst, ncp, m = layout
            if ch == 2:
                head = struct.pack("<BBHIhB", 1, 2, 0, 48000, 0, 0)
            else:   # Vorbis order 5.1: FL FC FR RL RR LFE; 7.1: FL FC FR SL SR RL RR LFE
                vorbis = [0, 2, 1, 4, 5, 3] if ch == 6 else [0, 2, 1, 6, 7, 4, 5, 3]
                head = struct.pack("<BBHIhBBB", 1, ch, 0, 48000, 0, 1, nst, ncp) + \
                    bytes(m[v] for v in vorbis)
            f.write(page([b"OpusHead" + head], 0, 0, 2))
            f.write(page([b"OpusTags" + struct.pack("<I", 2) + b"gs" + struct.pack("<I", 0)], 0, 1, 0))
            for i in range(0, len(pkts), 50):
                chunk = pkts[i:i + 50]
                last = i + 50 >= len(pkts)
                f.write(page(chunk, (i + len(chunk)) * 240, 2 + i // 50, 4 if last else 0))
        print("wrote", out)
        self._channel_report(out, layout[0])

    @staticmethod
    def _channel_report(path, ch):
        """decode with ffmpeg (Moonlight order FL FR FC LFE BL BR SL SR) and print each
        channel's level and zero-crossing frequency per second: shows which tone is where"""
        import array, math, shutil, subprocess
        if not shutil.which("ffmpeg"):
            return
        r = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-f", "f32le", "-ac", str(ch), "-"],
                           capture_output=True)
        a = array.array("f", r.stdout)
        names = ["FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR"][:ch] if ch > 2 else ["L", "R"]
        rate, n = 48000, len(a) // ch
        print("channels (dBFS @ Hz per second of audio, '-' = silent):", " ".join("%7s" % x for x in names))
        for sec in range(n // rate):
            row = []
            for c in range(ch):
                x = a[(sec * rate) * ch + c:((sec + 1) * rate) * ch:ch]
                rms = math.sqrt(sum(v * v for v in x[::4]) / len(x[::4]))
                zc = sum(1 for i in range(1, len(x)) if (x[i - 1] < 0) != (x[i] < 0)) / 2
                row.append("%3d@%-4d" % (20 * math.log10(rms), zc) if rms > 1e-3 else "      -")
            print("  %2d s" % sec, " ".join(row))


def main():
    global HOST, SSH
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=os.environ.get("CUBE_HOST"), help="the box's IP")
    ap.add_argument("--ssh-key", help="ssh identity file for root@host (PIN delivery)")
    ap.add_argument("cmd", choices=["pair", "check", "launch", "stream", "unpaired"])
    ap.add_argument("--state", default="gs-client")
    ap.add_argument("--manual-pin", action="store_true")
    ap.add_argument("--seconds", type=float, default=5, help="stream: receive time")
    ap.add_argument("--out", default="gs-stream.h264", help="stream: H.264/HEVC output file")
    ap.add_argument("--codec", default="h264", choices=["h264", "hevc"], help="stream: codec to ask for")
    ap.add_argument("--mode", default="1920x1080x60", help="stream: WxHxFPS")
    ap.add_argument("--kbps", type=int, default=20000, help="stream: configured bitrate")
    ap.add_argument("--channels", type=int, default=2, choices=[2, 6, 8],
                    help="stream: audio channels to request (6 = 5.1, 8 = 7.1)")
    ap.add_argument("--packet-size", type=int, default=1392,
                    help="stream: video packetSize (small = more FEC blocks per frame)")
    a = ap.parse_args()
    if not a.host:
        ap.error("--host (or $CUBE_HOST) is required")
    HOST = a.host
    SSH = ["ssh"] + (["-i", os.path.expanduser(a.ssh_key)] if a.ssh_key else []) + ["root@" + HOST]
    c = Client(a.state)
    if a.cmd == "pair":
        c.pair(not a.manual_pin)
        c.check()
    elif a.cmd == "check":
        c.check()
    elif a.cmd == "launch":
        c.launch()
    elif a.cmd == "stream":
        c.stream(a.seconds, a.out, packet_size=a.packet_size, mode=a.mode, kbps=a.kbps,
                 channels=a.channels, codec=a.codec)
    elif a.cmd == "unpaired":      # a fresh, unknown client cert must get 401 on HTTPS
        c2 = Client(a.state + "-unpaired")
        print("https serverinfo with unknown cert:", c2.get(True, "/serverinfo")[:2])


if __name__ == "__main__":
    main()

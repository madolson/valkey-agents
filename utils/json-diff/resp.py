"""Minimal RESP2/RESP3 client that returns each reply as its raw bytes.

Replies are compared byte for byte, so nothing here decodes or normalises a
reply beyond finding where it ends.
"""

import socket


class RespError(Exception):
    pass


def encode(args):
    out = [b"*%d\r\n" % len(args)]
    for a in args:
        if isinstance(a, str):
            a = a.encode("utf-8")
        out.append(b"$%d\r\n" % len(a))
        out.append(a)
        out.append(b"\r\n")
    return b"".join(out)


class Conn(object):
    def __init__(self, host, port, timeout=30.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.buf = b""
        self.pos = 0
        self.seq = 0

    def close(self):
        self.sock.close()

    def _fill(self):
        chunk = self.sock.recv(1 << 16)
        if not chunk:
            raise RespError("connection closed")
        if self.pos:
            self.buf = self.buf[self.pos:]
            self.pos = 0
        self.buf += chunk

    def _line(self):
        while True:
            i = self.buf.find(b"\r\n", self.pos)
            if i >= 0:
                line = self.buf[self.pos:i]
                self.pos = i + 2
                return line
            self._fill()

    def _exact(self, n):
        while len(self.buf) - self.pos < n + 2:
            self._fill()
        data = self.buf[self.pos:self.pos + n]
        self.pos += n + 2
        return data

    def _reply(self, out):
        line = self._line()
        out.append(line)
        out.append(b"\r\n")
        t = line[:1]
        if t in (b"+", b"-", b":", b"_", b",", b"#", b"("):
            return
        if t in (b"$", b"=", b"!"):
            n = int(line[1:])
            if n >= 0:
                out.append(self._exact(n))
                out.append(b"\r\n")
            return
        if t in (b"*", b"~", b">"):
            n = int(line[1:])
            for _ in range(max(n, 0)):
                self._reply(out)
            return
        if t in (b"%", b"|"):
            n = int(line[1:])
            for _ in range(n * 2):
                self._reply(out)
            if t == b"|":
                # An attribute precedes the actual reply.
                self._reply(out)
            return
        raise RespError("unknown RESP type byte %r" % line[:20])

    def read_raw(self):
        out = []
        self._reply(out)
        return b"".join(out)

    def call(self, args):
        """Send one command and return every byte it produced.

        Each command is followed by an ECHO sentinel and everything up to the
        sentinel's reply is returned. A server that emits more replies than it
        was asked for (valkey-json's legacy JSON.RESP on '..' does) then shows
        up as extra bytes on that command instead of shifting every later
        reply by one.
        """
        self.seq += 1
        marker = b"json-diff-sentinel-%d" % self.seq
        self.sock.sendall(encode(args) + encode([b"ECHO", marker]))
        want = b"$%d\r\n%s\r\n" % (len(marker), marker)
        first = self.read_raw()
        if first == want:
            raise RespError("no reply before the sentinel")
        extra = []
        while True:
            r = self.read_raw()
            if r == want:
                break
            extra.append(r)
        return first + b"".join(extra)


def is_error(raw):
    return raw[:1] in (b"-", b"!")

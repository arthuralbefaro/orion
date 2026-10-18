import argparse
import functools
import http.server
import os
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIG = 1 << 20


def big_data():
    return bytes(((i * 7 + (i >> 8)) & 0xFF) for i in range(BIG))


def prepare(path):
    os.makedirs(path, exist_ok=True)
    with open(os.path.join(path, 'index.html'), 'wb') as fh:
        fh.write(b'<html><body>ola do hospedeiro</body></html>\n')
    big = os.path.join(path, 'big.bin')
    data = big_data()
    if not os.path.exists(big) or os.path.getsize(big) != BIG:
        with open(big, 'wb') as fh:
            fh.write(data)
    return zlib.crc32(data) & 0xFFFFFFFF


class Quiet(http.server.SimpleHTTPRequestHandler):
    protocol_version = 'HTTP/1.0'

    def log_message(self, fmt, *args):
        sys.stderr.write('hostweb: ' + (fmt % args) + '\n')


def main():
    ap = argparse.ArgumentParser(description='Servidor HTTP do hospedeiro para os testes de TCP do Orion')
    ap.add_argument('--port', type=int, default=8088)
    ap.add_argument('--dir', default=os.path.join(ROOT, 'out', 'http'))
    a = ap.parse_args()
    crc = prepare(a.dir)
    handler = functools.partial(Quiet, directory=a.dir)
    srv = http.server.ThreadingHTTPServer(('127.0.0.1', a.port), handler)
    sys.stderr.write('hostweb: servindo %s em 127.0.0.1:%d (big.bin crc32 %08x)\n' % (a.dir, a.port, crc))
    sys.stderr.flush()
    srv.serve_forever()


if __name__ == '__main__':
    main()

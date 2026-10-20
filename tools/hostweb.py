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
    data = big_data()
    crc = 0
    for name, copies in (('big.bin', 1), ('big10.bin', 10)):
        full = os.path.join(path, name)
        if not os.path.exists(full) or os.path.getsize(full) != BIG * copies:
            with open(full, 'wb') as fh:
                for k in range(copies):
                    fh.write(bytes((b + k) & 0xFF for b in data) if k else data)
        if copies == 1:
            crc = zlib.crc32(data) & 0xFFFFFFFF
    return crc


class Quiet(http.server.SimpleHTTPRequestHandler):
    protocol_version = 'HTTP/1.0'

    def log_message(self, fmt, *args):
        sys.stderr.write('hostweb: ' + (fmt % args) + '\n')

    def do_PUT(self):
        left = int(self.headers.get('Content-Length', '0'))
        crc = 0
        total = 0
        while left > 0:
            chunk = self.rfile.read(min(left, 65536))
            if not chunk:
                break
            crc = zlib.crc32(chunk, crc)
            total += len(chunk)
            left -= len(chunk)
        body = ('recebido %d bytes, crc32 %08x\n' % (total, crc & 0xFFFFFFFF)).encode()
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    ap = argparse.ArgumentParser(description='Servidor HTTP do hospedeiro para os testes de TCP do Orion')
    ap.add_argument('--port', type=int, default=8088)
    ap.add_argument('--dir', default=os.path.join(ROOT, 'out', 'http'))
    a = ap.parse_args()
    crc = prepare(a.dir)
    esp = os.path.join(ROOT, 'out', 'esp')
    if os.path.exists(os.path.join(esp, 'bin', 'lua')):
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import mkrepo
        n = mkrepo.make_repo(esp, os.path.join(a.dir, 'repo'))
        sys.stderr.write('hostweb: repositorio de pacotes com %d pacote(s) em %s\n' % (n, os.path.join(a.dir, 'repo')))
    handler = functools.partial(Quiet, directory=a.dir)
    srv = http.server.ThreadingHTTPServer(('127.0.0.1', a.port), handler)
    sys.stderr.write('hostweb: servindo %s em 127.0.0.1:%d (big.bin crc32 %08x)\n' % (a.dir, a.port, crc))
    sys.stderr.flush()
    srv.serve_forever()


if __name__ == '__main__':
    main()

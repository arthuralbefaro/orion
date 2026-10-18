import hashlib
import os
import sys

B64 = './0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz'


def b64_from_24bit(b2, b1, b0, n):
    w = (b2 << 16) | (b1 << 8) | b0
    out = ''
    for _ in range(n):
        out += B64[w & 0x3f]
        w >>= 6
    return out


def sha512_crypt(password, salt, rounds=5000):
    p = password.encode()
    s = salt.encode()[:16]
    b = hashlib.sha512(p + s + p).digest()
    a = hashlib.sha512()
    a.update(p + s)
    n = len(p)
    while n > 64:
        a.update(b)
        n -= 64
    a.update(b[:n])
    n = len(p)
    while n:
        a.update(b if n & 1 else p)
        n >>= 1
    a = a.digest()
    dp = hashlib.sha512(p * len(p)).digest()
    pbytes = (dp * (len(p) // 64 + 1))[:len(p)]
    ds = hashlib.sha512(s * (16 + a[0])).digest()
    sbytes = (ds * (len(s) // 64 + 1))[:len(s)]
    c = a
    for i in range(rounds):
        h = hashlib.sha512()
        h.update(pbytes if i & 1 else c)
        if i % 3:
            h.update(sbytes)
        if i % 7:
            h.update(pbytes)
        h.update(c if i & 1 else pbytes)
        c = h.digest()
    order = [(0, 21, 42), (22, 43, 1), (44, 2, 23), (3, 24, 45), (25, 46, 4), (47, 5, 26),
             (6, 27, 48), (28, 49, 7), (50, 8, 29), (9, 30, 51), (31, 52, 10), (53, 11, 32),
             (12, 33, 54), (34, 55, 13), (56, 14, 35), (15, 36, 57), (37, 58, 16), (59, 17, 38),
             (18, 39, 60), (40, 61, 19), (62, 20, 41)]
    out = ''
    for x, y, z in order:
        out += b64_from_24bit(c[x], c[y], c[z], 4)
    out += b64_from_24bit(0, 0, c[63], 2)
    head = '$6$'
    if rounds != 5000:
        head += 'rounds=%d$' % rounds
    return head + salt[:16] + '$' + out


def main():
    if len(sys.argv) < 2 or len(sys.argv) > 3:
        raise SystemExit('uso: mkpasswd.py <senha> [sal]   (imprime o hash $6$ para o /etc/shadow)')
    salt = sys.argv[2] if len(sys.argv) > 2 else ''.join(B64[b % 64] for b in os.urandom(16))
    print(sha512_crypt(sys.argv[1], salt))


if __name__ == '__main__':
    main()

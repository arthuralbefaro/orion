import argparse
import array
import struct
import sys


def load(path):
    data = open(path, 'rb').read()
    if data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        raise SystemExit('wavcheck: %s nao e um WAV' % path)
    pos = 12
    fmt = None
    pcm = None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack_from('<I', data, pos + 4)[0]
        body = pos + 8
        if cid == b'fmt ':
            fmt = struct.unpack_from('<HHIIHH', data, body)
        elif cid == b'data':
            end = body + size if size and body + size <= len(data) else len(data)
            pcm = data[body:end]
            break
        pos = body + size + (size & 1)
    if not fmt or pcm is None:
        raise SystemExit('wavcheck: %s sem fmt ou data' % path)
    tag, channels, rate, _, _, bits = fmt
    if tag != 1 or bits != 16:
        raise SystemExit('wavcheck: so PCM de 16 bits (veio formato %d, %d bits)' % (tag, bits))
    samples = array.array('h')
    samples.frombytes(pcm[:len(pcm) - len(pcm) % (2 * channels)])
    if sys.byteorder != 'little':
        samples.byteswap()
    return rate, channels, samples


def main():
    ap = argparse.ArgumentParser(description='Confere um tom gravado pelo -audiodev wav do QEMU.')
    ap.add_argument('wav')
    ap.add_argument('--freq', type=float, default=1000.0)
    ap.add_argument('--dur', type=float, default=2.0)
    ap.add_argument('--tol-freq', type=float, default=20.0)
    ap.add_argument('--tol-dur', type=float, default=0.1)
    ap.add_argument('--threshold', type=int, default=1000)
    a = ap.parse_args()

    rate, channels, s = load(a.wav)
    frames = len(s) // channels
    left = s[0::channels]
    right = s[1::channels] if channels > 1 else array.array('h')
    print('wavcheck: %d Hz, %d canal(is), 16 bits, %.2f s gravados' % (rate, channels, frames / rate))
    ok = True

    loud = [i for i in range(frames) if abs(left[i]) > a.threshold]
    if not loud:
        print('wavcheck: canal esquerdo mudo: FALHA')
        print('wavcheck: FALHOU')
        return 1
    first, last = loud[0], loud[-1]
    dur = (last - first + 1) / rate
    good = abs(dur - a.dur) <= a.tol_dur
    ok &= good
    print('wavcheck: som no canal esquerdo por %.2f s (esperado %.2f +- %.2f): %s'
          % (dur, a.dur, a.tol_dur, 'ok' if good else 'FALHA'))

    crossings = 0
    sign = 0
    for i in range(first, last + 1):
        v = left[i]
        if v > a.threshold // 2:
            cur = 1
        elif v < -(a.threshold // 2):
            cur = -1
        else:
            continue
        if sign and cur != sign:
            crossings += 1
        sign = cur
    freq = crossings / 2 / dur
    good = abs(freq - a.freq) <= a.tol_freq
    ok &= good
    print('wavcheck: frequencia %.0f Hz pelos cruzamentos de zero (esperado %.0f +- %.0f): %s'
          % (freq, a.freq, a.tol_freq, 'ok' if good else 'FALHA'))

    peak = max((abs(v) for v in right), default=0)
    good = peak <= a.threshold // 10
    ok &= good
    print('wavcheck: canal direito em silencio (pico %d): %s' % (peak, 'ok' if good else 'FALHA'))

    window = max(rate // 200, 1)
    gap = 0
    worst = 0
    for w in range(first, last + 1, window):
        chunk = left[w:min(w + window, last + 1)]
        top = max(abs(v) for v in chunk)
        if top < a.threshold:
            gap += len(chunk)
            worst = max(worst, gap)
        else:
            gap = 0
    worst_ms = worst * 1000 / rate
    good = worst_ms <= 10
    ok &= good
    print('wavcheck: maior buraco dentro do som: %.0f ms: %s' % (worst_ms, 'ok' if good else 'FALHA'))
    print('wavcheck: TUDO OK' if ok else 'wavcheck: FALHOU')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())

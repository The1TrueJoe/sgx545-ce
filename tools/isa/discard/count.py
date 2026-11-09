import sys
W, H = 720, 480
for p in sys.argv[1:]:
    d = open(p, 'rb').read()
    if len(d) != W * H * 4:
        print(p, 'size', len(d)); continue
    L = R = 0; other = set()
    for y in range(H):
        row = d[y * W * 4:(y + 1) * W * 4]
        for x in range(W):
            px = row[4 * x:4 * x + 3]
            if px[1] > 200 and px[0] < 50:
                if x < W // 2: L += 1
                else: R += 1
            elif px != b'\x00\x00\x80' and px != b'\x00\x00\x7f':
                other.add(px.hex())
    print(f"{p.rsplit('/',1)[-1]:16} green left {L:6} right {R:6}  other {sorted(other)[:4]}")

# The span paths in displayio.Bitmap against a plain Python reference, over
# every bit depth Bitmap() can be asked for and every alignment within a byte.
# The width is deliberately neither a multiple of 8 nor of 32 so that the
# partial ends of a run are exercised as well as its whole bytes.
import displayio
import bitmaptools

W = 23
H = 5
DEPTHS = (1, 2, 4, 8, 16)


def blank():
    return [[0] * W for _ in range(H)]


def seed(bmp, ref, mask, salt=0):
    for y in range(H):
        for x in range(W):
            v = (x * 7 + y * 3 + salt) & mask
            bmp[x, y] = v
            ref[y][x] = v


def check(name, depth, bmp, ref):
    for y in range(H):
        row = [bmp[x, y] for x in range(W)]
        if row != ref[y]:
            print(name, depth, "MISMATCH row", y)
            print("  got", row)
            print("  exp", ref[y])
            return False
    return True


def test_fill(depth, mask):
    for x1 in range(9):
        for w in range(1, 10):
            for value in (0, mask):
                bmp = displayio.Bitmap(W, H, 1 << depth)
                ref = blank()
                seed(bmp, ref, mask)
                x2 = min(x1 + w, W)
                if x2 <= x1:
                    continue
                bitmaptools.fill_region(bmp, x1, 1, x2, 4, value)
                for y in range(1, 4):
                    for x in range(x1, x2):
                        ref[y][x] = value
                if not check("fill", depth, bmp, ref):
                    return False
    return True


def test_line(depth, mask):
    for x1 in range(9):
        for w in range(1, 10):
            bmp = displayio.Bitmap(W, H, 1 << depth)
            ref = blank()
            seed(bmp, ref, mask)
            x2 = min(x1 + w, W - 1)
            bitmaptools.draw_line(bmp, x1, 2, x2, 2, mask)
            for x in range(x1, x2 + 1):
                ref[2][x] = mask
            if not check("line", depth, bmp, ref):
                return False
    return True


def test_blit(depth, mask):
    for sx in range(9):
        for dx in range(9):
            for w in range(1, 10):
                if sx + w > W or dx + w > W:
                    continue
                src = displayio.Bitmap(W, H, 1 << depth)
                dst = displayio.Bitmap(W, H, 1 << depth)
                sref = blank()
                dref = blank()
                seed(src, sref, mask, 1)
                seed(dst, dref, mask, 2)
                bitmaptools.blit(dst, src, dx, 0, x1=sx, y1=0, x2=sx + w, y2=H)
                for y in range(H):
                    for i in range(w):
                        dref[y][dx + i] = sref[y][sx + i]
                if not check("blit", depth, dst, dref):
                    return False
    return True


def test_overlap(depth, mask):
    # Source and destination in one bitmap, moved both ways, so that a run has
    # to be read before the copy reaches it.
    for delta in (-5, -3, -1, 1, 3, 5):
        bmp = displayio.Bitmap(W, H, 1 << depth)
        ref = blank()
        seed(bmp, ref, mask)
        sx, w = 6, 10
        dx = sx + delta
        bitmaptools.blit(bmp, bmp, dx, 0, x1=sx, y1=0, x2=sx + w, y2=H)
        for y in range(H):
            row = list(ref[y])
            for i in range(w):
                ref[y][dx + i] = row[sx + i]
        if not check("overlap", depth, bmp, ref):
            return False
    return True


def test_replace(depth, mask):
    bmp = displayio.Bitmap(W, H, 1 << depth)
    ref = blank()
    seed(bmp, ref, mask)
    # The two have to differ at every depth, one bit included, or the call has
    # nothing to do and the run handling is never reached.
    old = 0
    new = mask
    bitmaptools.replace_color(bmp, old, new)
    for y in range(H):
        for x in range(W):
            if ref[y][x] == old:
                ref[y][x] = new
    return check("replace", depth, bmp, ref)


def test_boundary(depth, mask):
    bmp = displayio.Bitmap(W, H, 1 << depth)
    ref = blank()
    bmp.fill(0)
    bitmaptools.fill_region(bmp, 4, 1, 17, 4, mask)
    for y in range(H):
        for x in range(W):
            ref[y][x] = mask if (1 <= y < 4 and 4 <= x < 17) else 0
    if mask < 2:
        # One bit leaves no third value to fill the enclosed area with.
        return True
    fill_value = 1
    bitmaptools.boundary_fill(bmp, 8, 2, fill_value, mask)
    for y in range(1, 4):
        for x in range(4, 17):
            ref[y][x] = fill_value
    return check("boundary", depth, bmp, ref)


def ref_line(ref, x0, y0, x1, y1, value):
    # The same walk draw_line does, so that a mistake in the clipping shows up
    # as a difference rather than being reproduced on both sides.
    def plot(x, y):
        if 0 <= x < W and 0 <= y < H:
            ref[y][x] = value

    if x0 == x1:
        if y0 > y1:
            y0, y1 = y1, y0
        for y in range(max(0, y0), min(y1, H - 1) + 1):
            plot(x0, y)
        return
    if y0 == y1:
        if x0 > x1:
            x0, x1 = x1, x0
        for x in range(max(0, x0), min(x1, W - 1) + 1):
            plot(x, y0)
        return
    steep = abs(y1 - y0) > abs(x1 - x0)
    if steep:
        x0, y0 = y0, x0
        x1, y1 = y1, x1
    if x0 > x1:
        x0, x1 = x1, x0
        y0, y1 = y1, y0
    dx = x1 - x0
    dy = abs(y1 - y0)
    err = dx / 2
    ystep = 1 if y0 < y1 else -1
    for x in range(x0, x1 + 1):
        if steep:
            plot(y0, x)
        else:
            plot(x, y0)
        err -= dy
        if err < 0:
            y0 += ystep
            err += dx


def test_line_any(depth, mask):
    # Endpoints well outside the bitmap on every side, so a clip that is off by
    # one or missing shows up either as a wrong pixel or as a crash.
    cases = (
        (-9, -4, 30, 20),
        (30, -4, -9, 20),
        (-9, 20, 30, -4),
        (2, -30, 6, 40),
        (-30, 2, 40, 3),
        (0, 0, 22, 4),
        (22, 0, 0, 4),
        (5, -1, 5, 40),
        (-40, 2, 60, 2),
    )
    for x0, y0, x1, y1 in cases:
        bmp = displayio.Bitmap(W, H, 1 << depth)
        ref = blank()
        seed(bmp, ref, mask)
        bitmaptools.draw_line(bmp, x0, y0, x1, y1, mask)
        ref_line(ref, x0, y0, x1, y1, mask)
        if not check("line_any", depth, bmp, ref):
            return False
    return True


def test_circle(depth, mask):
    for cx, cy, r in ((11, 2, 3), (0, 0, 5), (22, 4, 6), (11, 2, 40), (5, 3, 0)):
        bmp = displayio.Bitmap(W, H, 1 << depth)
        ref = blank()
        seed(bmp, ref, mask)
        bitmaptools.draw_circle(bmp, cx, cy, r, mask)
        # draw_circle first clamps the centre into the bitmap, then walks.
        x = min(max(0, cx), W)
        y = min(max(0, cy), H)
        yb = r
        d = 3 - 2 * r
        xb = 0
        while xb <= yb:
            for px, py in (
                (xb + x, yb + y), (-xb + x, -yb + y), (-xb + x, yb + y), (xb + x, -yb + y),
                (yb + x, xb + y), (-yb + x, xb + y), (-yb + x, -xb + y), (yb + x, -xb + y),
            ):
                if 0 <= px < W and 0 <= py < H:
                    ref[py][px] = mask
            if d <= 0:
                d = d + (4 * xb) + 6
            else:
                d = d + 4 * (xb - yb) + 10
                yb -= 1
            xb += 1
        if not check("circle", depth, bmp, ref):
            return False
    return True


def test_blit_skip(depth, mask):
    # The binding validates the destination x against the bitmap, so only the
    # right hand overhang is reachable from Python.
    for dx in (0, 3, W - 5):
        for skip_src, skip_dst in ((0, None), (None, 0), (1 & mask, mask)):
            src = displayio.Bitmap(W, H, 1 << depth)
            dst = displayio.Bitmap(W, H, 1 << depth)
            sref = blank()
            dref = blank()
            seed(src, sref, mask, 1)
            seed(dst, dref, mask, 2)
            bitmaptools.blit(
                dst, src, dx, 0, x1=0, y1=0, x2=W, y2=H,
                skip_source_index=skip_src, skip_dest_index=skip_dst,
            )
            for y in range(H):
                for i in range(W):
                    xd = dx + i
                    if not (0 <= xd < W):
                        continue
                    v = sref[y][i]
                    if skip_src is not None and v == skip_src:
                        continue
                    if skip_dst is not None and dref[y][xd] == skip_dst:
                        continue
                    dref[y][xd] = v
            if not check("blit_skip", depth, dst, dref):
                return False
    return True


def test_arrayblit(depth, mask):
    bmp = displayio.Bitmap(W, H, 1 << depth)
    ref = blank()
    seed(bmp, ref, mask)
    x1, y1, x2, y2 = 3, 1, 19, 4
    data = bytearray((x * 5 + y) & mask for y in range(y2 - y1) for x in range(x2 - x1))
    bitmaptools.arrayblit(bmp, data, x1, y1, x2, y2)
    k = 0
    for y in range(y1, y2):
        for x in range(x1, x2):
            ref[y][x] = data[k]
            k += 1
    return check("arrayblit", depth, bmp, ref)


def test_rotozoom_bounds(depth, mask):
    # Nothing outside the destination clip window may be touched, which is what
    # the dropped per-pixel bounds test used to guarantee.
    src = displayio.Bitmap(W, H, 1 << depth)
    dst = displayio.Bitmap(W, H, 1 << depth)
    sref = blank()
    dref = blank()
    seed(src, sref, mask, 5)
    seed(dst, dref, mask, 6)
    bitmaptools.rotozoom(
        dst, src, ox=11, oy=2, dest_clip0=(4, 1), dest_clip1=(16, 4),
        px=W // 2, py=H // 2, angle=0.7, scale=1.5,
    )
    for y in range(H):
        for x in range(W):
            inside = 4 <= x < 16 and 1 <= y < 4
            if not inside and dst[x, y] != dref[y][x]:
                print("rotozoom", depth, "wrote outside the clip at", x, y)
                return False
    return True


for depth in DEPTHS:
    mask = (1 << depth) - 1
    for name, fn in (
        ("fill", test_fill),
        ("line", test_line),
        ("line_any", test_line_any),
        ("circle", test_circle),
        ("blit", test_blit),
        ("blit_skip", test_blit_skip),
        ("overlap", test_overlap),
        ("arrayblit", test_arrayblit),
        ("rotozoom", test_rotozoom_bounds),
        ("replace", test_replace),
        ("boundary", test_boundary),
    ):
        print(name, depth, "ok" if fn(depth, mask) else "FAILED")

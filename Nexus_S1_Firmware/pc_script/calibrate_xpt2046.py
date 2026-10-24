"""Calculate calibration from four real raw corner samples; never opens a port or edits firmware."""
import argparse
import json


def target_points(rotation=1, margin=0):
    if rotation not in range(4) or not 0 <= margin < 120:
        raise ValueError("rotation must be 0..3, margin 0..119")
    width, height = (320, 240) if rotation % 2 else (240, 320)
    return [(margin, margin), (width-1-margin, margin),
            (width-1-margin, height-1-margin), (margin, height-1-margin)]


def fit_calibration(corners, rotation=1, margin=0):
    """TL, TR, BR, BL raw (D0,90) medians at the requested logical pixel targets."""
    targets = target_points(rotation, margin)
    if len(corners) != 4 or any(len(p) != 2 for p in corners):
        raise ValueError("four raw x,y pairs required: TL TR BR BL")
    if any(not isinstance(v, int) or not 0 <= v <= 4095 for p in corners for v in p):
        raise ValueError("raw samples must be integer ADC counts in 0..4095")
    def inverse(x, y):
        return [(x, y), (y, 319-x), (239-x, 319-y), (239-y, x)][rotation]
    native = [inverse(x, y) for x, y in targets]
    def axis(values, coordinates, maximum):
        mean_c, mean_v = sum(coordinates)/4, sum(values)/4
        slope = sum((c-mean_c)*(v-mean_v) for c, v in zip(coordinates, values)) / sum(
            (c-mean_c)**2 for c in coordinates)
        origin = mean_v-slope*mean_c
        endpoints = [round(origin), round(origin+slope*maximum)]
        residual = max(abs(v-origin-slope*c) for c, v in zip(coordinates, values))
        if min(endpoints) < 0 or max(endpoints) > 4095 or abs(endpoints[1]-endpoints[0]) < 128:
            raise ValueError("calibration span invalid; repeat the samples at the stated targets")
        return endpoints, residual
    candidates = []
    for swap in (False, True):
        try:
            xs, rx = axis([p[1 if swap else 0] for p in corners], [p[0] for p in native], 239)
            ys, ry = axis([p[0 if swap else 1] for p in corners], [p[1] for p in native], 319)
            candidates.append((max(rx, ry), swap, xs, ys))
        except ValueError:
            continue
    if not candidates:
        raise ValueError("inconsistent/degenerate corner samples")
    residual, swap, xs, ys = min(candidates)
    if residual > 40:
        raise ValueError("corner residual exceeds 40 ADC counts; check order, targets and stability")
    return dict(confirmed=True, swap_xy=swap, x0=xs[0], x1=xs[1], y0=ys[0], y1=ys[1])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    def pair(text):
        try:
            values = tuple(int(v) for v in text.split(","))
            if len(values) != 2: raise ValueError()
            return values
        except ValueError:
            raise argparse.ArgumentTypeError("expected raw_x,raw_y")
    for name in ("tl", "tr", "br", "bl"):
        parser.add_argument("--"+name, required=True, type=pair)
    parser.add_argument("--rotation", type=int, default=1)
    parser.add_argument("--margin", type=int, default=0, help="pixel inset of actual sample targets (default: edges)")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    try:
        result = fit_calibration([args.tl, args.tr, args.br, args.bl], args.rotation, args.margin)
    except ValueError as error:
        parser.error(str(error))
    if args.json:
        print(json.dumps(result))
    else:
        values = ", ".join(str(result[k]).lower() for k in ("confirmed", "swap_xy", "x0", "x1", "y0", "y1"))
        print("// Computed from supplied observations; verify on the actual display before acceptance.")
        print("c.calibration = {"+values+"};")
        print(f"c.rotation = {args.rotation};")


if __name__ == "__main__":
    main()

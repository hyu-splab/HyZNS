# phase_canon.py — the paper's canonical resize-phase vocabulary and layout,
# SHARED by plot_ops_zoom.py and resize_matched_cdf.py so the figures cannot
# diverge: internal pipeline names map to the three paper phases and bands are
# laid out in the paper's conceptual order.

CANON = {
    "zenfs migration": "Migration", "f2fs drain (online)": "Migration",
    "dm force-GC + ABA": "Migration", "zenfs prep": "Migration",
    "zone reset": "Reset", "zenfs zone adopt": "Reset",
    "resize_cns (freeze+ckpt+ABA)": "UpdateABA",
    "resize_cns (drain+GC+ckpt+ABA)": "UpdateABA",
    "resize_cns (kernel)": "UpdateABA",
    "freeze (pass2+ckpt)": "UpdateABA", "finalize": "UpdateABA",
}
PHASE_C = {"Migration": "#ff7f0e", "Reset": "#9467bd", "UpdateABA": "#d62728"}
PAPER_ORDER = ("Migration", "Reset", "UpdateABA")


def snap(segs, x0, x1):
    """Cover [x0,x1] gaplessly: the first phase starts at the window start,
    each phase runs until the next one begins, the last ends at the window
    end. Keeps the INTERNAL boundaries (the measured ts) intact."""
    segs = sorted(segs, key=lambda s: s[0])
    out = []
    for i, (p0, p1, nm, info) in enumerate(segs):
        s = x0 if i == 0 else out[-1][1]
        e = segs[i + 1][0] if i + 1 < len(segs) else max(x1, p1)
        out.append((s, max(e, s), nm, info))
    return out


def paper_segments(segs, x0, x1):
    """The paper's CONCEPTUAL pipeline view: per-phase durations are the
    measured sums (from snap()), but the bands are laid out in the paper's
    order Migration -> Reset -> UpdateABA regardless of how the kernel
    interleaves them (e.g. shrink runs its dm move inside the frozen ioctl).
    Time attribution within the window, not literal interleaving."""
    dur, info = {}, {}
    for p0, p1, nm, inf in segs:
        dur[nm] = dur.get(nm, 0.0) + (p1 - p0)
        if inf and nm not in info:
            info[nm] = inf
    order = [n for n in PAPER_ORDER if dur.get(n, 0) > 0]
    order += [n for n in dur if n not in PAPER_ORDER and dur[n] > 0]
    out, cur = [], x0
    for nm in order:
        out.append((cur, cur + dur[nm], nm, info.get(nm, "")))
        cur += dur[nm]
    if out:
        out[-1] = (out[-1][0], x1, out[-1][2], out[-1][3])
    return out


def event_moved_zero(ev):
    """True when a resize event moved NOTHING (grow roll=0; shrink f2/dm/gcmig
    all 0) — its sub-0.1s 'Migration' sliver is trigger-poll/ABA command time,
    not data movement."""
    if ev.get("dir") == "shrink":
        return all(float(ev.get(c) or 0) == 0
                   for c in ("f2_mib", "dm_mib", "dm_gcmig_MiB"))
    return float(ev.get("roll_mib") or 0) == 0


def remap_zero_move(segs):
    """Fold the misleading Migration sliver of a zero-move event into
    UpdateABA (see event_moved_zero)."""
    return [(p0, p1, "UpdateABA" if nm == "Migration" and p1 - p0 < 0.1 else nm,
             "" if nm == "Migration" else info) for p0, p1, nm, info in segs]

#!/usr/bin/env python3
"""OpenDMOfw - worst-case stack depth from GCC call-graph info.

Compile every source with the firmware's flags plus `-fcallgraph-info=su`
(one .ci file per object; `make stack` does this), then:

    python3 tools/stack_depth.py [--limit BYTES] build/OP104/stack/*.ci

For each root (main, and every interrupt handler) it walks the static call
graph, adds the per-function frame sizes GCC reports, and prints the deepest
path. The total the stack must hold is the main-thread worst case plus the
deepest handler plus the Cortex-M0 exception frame (8 words = 32 bytes, plus
4 bytes of alignment): all interrupts in this firmware run at the reset
priority, so handlers never nest.

Limits, stated rather than hidden: calls through function pointers are not in
the graph (this firmware makes none), library functions GCC did not compile
here count as 0 bytes, and recursion is reported as an error.
"""
import re, sys

EXC_FRAME = 36
ROOTS_THREAD = ["Reset_Handler"]
HANDLER_RE = re.compile(r".*_(IRQHandler|Handler)$")

node_re = re.compile(r'node: \{ title: "([^"]+)" label: "([^"]*)"')
bytes_re = re.compile(r"\\n(\d+) bytes")
edge_re = re.compile(r'edge: \{ sourcename: "([^"]+)" targetname: "([^"]+)"')


def load(paths):
    size, calls = {}, {}
    for p in paths:
        text = open(p, encoding="utf-8", errors="replace").read()
        for name, label in node_re.findall(text):
            m = bytes_re.search(label)
            if m:
                size[name] = int(m.group(1))
            else:
                size.setdefault(name, None)
        for a, b in edge_re.findall(text):
            calls.setdefault(a, set()).add(b)
    return size, calls


def deepest(fn, size, calls, stack=()):
    if fn in stack:
        raise SystemExit("recursion: " + " -> ".join(stack + (fn,)))
    own = size.get(fn) or 0
    best, best_path = 0, ()
    for callee in sorted(calls.get(fn, ())):
        d, path = deepest(callee, size, calls, stack + (fn,))
        if d > best:
            best, best_path = d, path
    return own + best, (fn,) + best_path


def main():
    args = sys.argv[1:]
    limit = None
    if args[:1] == ["--limit"]:
        limit, args = int(args[1], 0), args[2:]
    if not args:
        sys.exit(__doc__)
    size, calls = load(args)
    unknown = sorted(n for n, s in size.items() if s is None and n not in calls)
    thread = max((deepest(r, size, calls) for r in ROOTS_THREAD if r in size),
                 key=lambda t: t[0])
    handlers = [h for h in size if HANDLER_RE.match(h) and h not in ROOTS_THREAD
                and h != "Default_Handler"]
    worst_h = max((deepest(h, size, calls) for h in handlers), key=lambda t: t[0],
                  default=(0, ()))

    def show(title, d):
        print(f"{title}: {d[0]} bytes")
        for fn in d[1]:
            print(f"    {size.get(fn) or 0:4d}  {fn}")
    show("thread (Reset_Handler -> main ...)", thread)
    show("deepest interrupt handler", worst_h)
    total = thread[0] + worst_h[0] + EXC_FRAME
    print(f"worst case: {thread[0]} + {worst_h[0]} + {EXC_FRAME} (exception frame) = {total} bytes")
    if unknown:
        print("counted as 0 bytes (not compiled here): " + ", ".join(unknown))
    if limit is not None:
        if total > limit:
            sys.exit(f"FAIL: worst case {total} bytes exceeds the {limit}-byte stack")
        print(f"ok: {limit - total} bytes of headroom in the {limit}-byte stack")


if __name__ == "__main__":
    main()

"""Re-root a standalone TOML document under a table prefix.

`[a.b]` becomes `[extras.a.b]` and the pairs before the first header fall under
an `[extras]` the caller writes, so a TOML file can be read as the extras of a
ryspec document. Only table headers are rewritten, so this walks just enough
TOML to know where one can appear -- past string bodies and comments, and at
bracket depth nought, a header being the one `[` that opens no array.

test_toml_conformance.py checks the result against `tomllib` for every
valid file in the suite, which is what makes the transform trustworthy.
"""


def reroot(src: bytes, prefix: bytes = b"extras") -> bytes:
    out = bytearray()
    i, n = 0, len(src)
    at_line_start = True
    in_header = False
    depth = 0
    while i < n:
        c = src[i : i + 1]
        if c == b"#":
            j = src.find(b"\n", i)
            j = n if j < 0 else j
            out += src[i:j]
            i = j
            continue
        if c == b'"' or c == b"'":
            escapes = c == b'"'
            if src[i : i + 3] == c * 3:
                j = _end_multiline(src, i + 3, c * 3, escapes)
            else:
                j = _end_single(src, i + 1, c, escapes)
            out += src[i:j]
            i = j
            at_line_start = False
            continue
        if c == b"\n":
            out += c
            i += 1
            at_line_start = True
            in_header = False
            continue
        if c in (b" ", b"\t", b"\r"):
            out += c
            i += 1
            continue
        if c == b"[" and at_line_start and depth == 0:
            if src[i : i + 2] == b"[[":
                out += b"[[" + prefix + b"."
                i += 2
            else:
                out += b"[" + prefix + b"."
                i += 1
            at_line_start = False
            in_header = True
            continue
        if not in_header:
            if c in (b"[", b"{"):
                depth += 1
            elif c in (b"]", b"}"):
                depth = max(0, depth - 1)
        out += c
        i += 1
        at_line_start = False
    return bytes(out)


def _end_single(src, i, quote, escapes):
    n = len(src)
    while i < n:
        c = src[i : i + 1]
        if escapes and c == b"\\":
            i += 2
            continue
        if c == quote:
            return i + 1
        if c == b"\n":  # unterminated; leave it for the parser to reject
            return i
        i += 1
    return n


def _end_multiline(src, i, delim, escapes):
    n = len(src)
    while i < n:
        if escapes and src[i : i + 1] == b"\\":
            i += 2
            continue
        if src[i : i + 3] == delim:
            i += 3
            while i < n and src[i : i + 1] == delim[:1]:
                i += 1
            return i
        i += 1
    return n

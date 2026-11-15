"""log_records.py - the single-record rule the standalone detectors share (lane C r17).

flicker_gate's strict halves validate their whole run log first (flicker_gate.log_integrity). The detectors also run on
their own (freeze_diff --exact, flicker_diff), and their inventory readers take dump / begin records out of the log.
After Astra 5fb5a2c r16 / conv16 P2 (an interleaved `[BROKEN] [INFO ] [BOOT] [SIM_FREEZE] #2 begin ...` after a dump
record hid that freeze's begin; a second [PRESENT_DUMP] / mark after a dump name was ignored), a line that carries a tag
such a reader takes records from is ONE whole record: a `[ts] [LEVEL] [CAT] ` envelope with a real time, no second
envelope anywhere after it, the tag only once, as the message's opening tag, and its body one of that tag's whole FORMS.

FORMS (r21, Astra 82f7b6d conv20 P2: a cut second record ` [2026-10-09 10:` after a dump name passed both standalone
detectors) is the ONE definition of the inventory tags' record forms: flicker_gate.RECORD_FORMS takes these three from
here, so the strict gate and the standalone readers accept exactly the same records. No form admits a bracket in its body:
archive survey 2026-10-23 (37 real FG run logs, 215,490 lines) - none of the 17,213 [PRESENT_DUMP], 10,269
[SIM_FREEZE_DUMP] and 616 [SIM_FREEZE] records carries one, so a bracket there is a fragment of another record.
"""
import datetime
import re

ENVELOPE = re.compile(r"^\[\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3}\] \[(?:INFO |WARN |ERROR|DEBUG|TRACE|FATAL)\] "
                      r"\[[A-Za-z0-9_]+\] ")
SECOND_ENVELOPE = re.compile(r"\] \[(?:INFO |WARN |ERROR|DEBUG|TRACE|FATAL)\] \[")

INVENTORY_TAGS = ("PRESENT_DUMP", "SIM_FREEZE", "SIM_FREEZE_DUMP")   # the tags the standalone readers take records from
_NUM = r"\d+(?:\.\d+)?"
_NB = r"[^\[\]]*"                                  # free text without a bracket
_KV_NB = r"(?: [^\s=\[\]]+=[^\s\[\]]+)*"           # trailing key=value fields, no bracket in a key or a value
FORMS = {   # each tag's record body (the message after `[TAG] `), matched from its start
    "SIM_FREEZE": [r"^#\d+ begin at " + _NUM + r" s: sim time " + _NUM + r" s, sim ticks \d+, render ticks \d+\s*$",
                   r"^#\d+ end: \d+ render ticks while held \(" + _NUM + r"/s\), shown stamp " + _NUM + r" s\s*$",
                   r"^eye adaptation " + _NB + r"$", r"^DIAGNOSTIC on: " + _NB + r"$", r"^shader time " + _NB + r"$"],
    "SIM_FREEZE_DUMP": [r"^freeze=\d+ k=\d+ present=\d+ held_ms=\d+ shown_stamp=" + _NUM + r" s" + _KV_NB + r"\s*$"],
    # r16: no bracket in the path or the tail; r22 (Astra fdc4cbd r21 P1: `^\S` let a cut `[2026-10-09 10:` stand BEFORE
    # the path, and the readers keep only its basename): nor as the path's first character
    "PRESENT_DUMP": [r"^[^\s\[\]]" + _NB + r"_p\d+_\d+x\d+\.bmp(?: " + _NB + r")?$",
                     r"^window opened by the harness dump mark at present \d+ \(cap \d+\)\s*$",
                     r"^WRITE FAILED [^\s\[\]]" + _NB + r"$"],
}
_FORMS_RX = {tag: [re.compile(p) for p in pats] for tag, pats in FORMS.items()}


# STRICT LOG INTEGRITY (lane C r13, after Astra 762d3d4 r12 / conv12: rounds 7-12 each found one more reader - the era
# markers, the gameplay / menu contexts, the movie windows, the dump records - that silently skipped a cut or corrupt
# record). A strict gate's halves first require their run log to be WELL-FORMED as a whole, so no reader ever sees a
# partial record: every line is `[ts] [LEVEL] [CAT] message` with no second timestamp inside it, the file ends with a
# newline (no cut last line), a message opens with a complete [TAG] (a dangling `[TAG` is a cut record), and every line of
# a VERDICT-BEARING tag is one of that tag's known record forms, whole (RECORD_FORMS); a [GPU_HANG] line belongs to a block
# opened by its trigger. Archive survey 2026-10-23 (37 real FG run logs, 215,490 lines): every line is of that form, none
# interleaved, every file ends with a newline, one untagged message (an ERROR text), every verdict-bearing line one of
# these forms. Non-strict grading (readonly, the ledger) keeps its reading - never a strict verdict.
LOG_LINE = re.compile(r"^\[\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3}\] \[(?:INFO |WARN |ERROR|DEBUG|TRACE|FATAL)\] "
                      r"\[[A-Za-z0-9_]+\] (.*)$")
INNER_TS = re.compile(r"\[\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3}\] \[")
# r23 (Astra a899ebb r22 P1: a cut `[2026-10-09 10:` inside a context record's mode hid it from the era reader): a record
# START's date - `[` and four digits and `-` - anywhere after a line's envelope is a cut record inside it (archive survey:
# no line of the 37 real logs carries one)
RECORD_START = re.compile(r"\[\d{4}-")
MSG_TAG = re.compile(r"^\[([A-Za-z0-9_:.\-]+)\](?: |$)")
_KV = r"(?: [^\s=]+=\S+)*"   # trailing key=value fields (a movie's `gaps<25/31/.../+=538/4/...` among them)
_NUM = r"\d+(?:\.\d+)?"
RECORD_FORMS = {tag: [re.compile(p) for p in pats] for tag, pats in {
    # r14 (Astra 263c082 r13 / conv13 P1: perf_report's skipped-movie era reader needs the fields after `config A`):
    # a context record is its WHOLE real form, through `poll=` (archive survey: every one of them)
    # r23 (Astra a899ebb r22 P1: `mode \S(?:.*\S)?` admitted a fragment perf_report's MOVIE_SKIP - `invX=\d+ invY=\d+
    # mode [a-z]` - does not read): the form implies every reader's match - no bracket in config / mode, invX / invY as
    # MOVIE_SKIP reads them, a mode `-` (the HUD request's) or a word (archive: `-`, `on-foot camera`, `flight`; inv 0/1)
    "PC_CONTROLS": [r"^context \w+ \(igc type \d+\) config [^\s\[\]]+ invX=\d+ invY=\d+ "
                    r"mode (?:-|[a-z](?:[^\[\]]*[^\s\[\]])?) poll=\d+\s*$",
                    r"^mouse-look ", r"^automated run: ", r"^game readers ON "],
    "MOVIE_PACE": [r"^movie#\d+ (?:EOM|GAP) wall=" + _NUM + r"s frames=\d+" + _KV + r"\s*$"],
    # r21 (Astra 82f7b6d conv20 P2): [SIM_FREEZE], [SIM_FREEZE_DUMP] and [PRESENT_DUMP] - the inventory tags - take their
    # forms from FORMS (above), the ONE definition the standalone detectors read them by too
    # r23 (Astra a899ebb r22): a request's path is what the era readers match - no bracket in it or its tail (archive: none)
    "APT_REQ": [r"^#\d+ rc=-?\d+\s*$", r"^#\d+ path='[^'\[\]]*'(?: [^\s\[\]][^\[\]]*)?$"],
    # r14: the in-game era's other start (perf_report.MOVIE_SKIP) and the roll-health readers' ALARM records (run_health
    # MARKERS, review_kernel_compiles, the host-work warning): a line of these tags is the reader's whole record or it is
    # refused (archive survey: none of the alarm tags appears in a real FG roll's log at all)
    "EV187": [r"^#\d+ slot=\d+ rc=\d+ (?:declined|ACCEPTED-KILL)(?: .*)?$"],
    "NB_AOT": [r"^compiled [^\s\[\]]+ - no shipped blob [^\s\[\]]+\s*$"],   # r23: the fields KERNEL_COMPILE reads, no bracket
    "CP_WAIT": [r"^stalled(?: .*)?$"],
    "NATIVE_RECORDING": [r"^FAILED(?: .*)?$"],
    "ICALL_FAIL": [r"^\S"],
    "CS_WEDGE": [r"^cs=\S"],
    "BUGCHECK": [r"^\S"],
    "CRASH": [r"^\S"],
    # r23: the fields run_health.HOST_WORK_STALL reads carry no bracket
    "HOST_WORK_STALL": [r"^\(a\) host work [^\s\[\]]+ on host tid \d+ \([^)\[\]]*\) for " + _NUM + r"s(?: .*)?$",
                        r"^\([b-z]\) "],
    # r15 (Astra fb6f4c6 r14 / conv14 P1: a cut orphan alarm passed health): run_health's orphaned-critical-section alarm,
    # whole through its ORPHANED word (archive survey: no [THREAD] line in a real FG roll's log)
    "THREAD": [r"^dying thread OWNS cs=\S+ .*\bORPHANED\b.*$"],
}.items()}
# r21: the inventory tags' whole forms, shared with freeze_diff --exact / flicker_diff (no bracket in their bodies)
RECORD_FORMS.update({tag: [re.compile(p) for p in pats] for tag, pats in FORMS.items()})
# r15 (Astra fb6f4c6 r14 P1: a record whose opening `[` was removed read as free text): an UNTAGGED message is one of the
# known untagged forms (archive survey, 215,490 lines: exactly one - the guard-page ERROR text); any other is refused
UNTAGGED_FORMS = [re.compile(r"^Failed to map guard page at 0x[0-9A-Fa-f]+\s*$")]
# r16 (Astra ed02b5a r15 / conv15 P1: a `[MOVIE_PACE] ...` text in another record's free tail was read by a whole-line
# search as a movie window): a verdict-bearing tag appears ONLY as a message's opening tag, and no line carries a second
# record envelope (`] [LEVEL] [`) anywhere - so a reader that searches a whole line can find only the line's own record
VERDICT_TAG = re.compile(r"\[(?:%s|GPU_HANG|PERF)\]" % "|".join(sorted(RECORD_FORMS)))
# r23 (the r23 sweep: a cut record's remainder spliced in after a record's opening `[` - `[IM_FREEZE] #9 ... ticks
# 20PC_CONTROLS] context gameplay ...` - turned the context record into an unknown tag's free text): a verdict-bearing
# tag's NAME and its `]` appear nowhere but as the message's opening tag - not after it, not inside another opening tag
# (archive survey: no such line in the 37 real logs)
VERDICT_NAMES = frozenset(RECORD_FORMS) | {"GPU_HANG", "PERF"}
VERDICT_NAME = re.compile(r"(?:%s)\]" % "|".join(sorted(VERDICT_NAMES)))
# r23 (the sweep: a cut record's remainder spliced INTO a record's opening tag - `[PIM_FREEZE] #9 ... 20C_CONTROLS]
# context gameplay ...`): a message's square brackets pair (archive survey, 215,490 lines: every message after its
# opening tag does, once the one half-open interval a real record writes - `[base, base+0x...)`, in `[PC_CONTROLS] game
# readers ON (... over [base, base+0xA0000000))` - is taken out whole) - so a cut record, which carries a lone `[` or `]`
# of its envelope / tag, cannot hide inside another record. The interval is removed, never counted as an open `[` (the
# full sweep: its `[` closed by a spliced record's `]` paired a line that hid a context record)
INTERVAL = re.compile(r"\[base, base\+0x[0-9A-Fa-f]+\)")


def brackets_pair(text):
    """True when every `[` in text (the real half-open interval removed) is closed by a later `]` and no `]` closes
    nothing."""
    depth = 0
    for c in INTERVAL.sub("", text):
        if c == "[":
            depth += 1
        elif c == "]":
            depth -= 1
            if depth < 0:
                return False
    return depth == 0


SECOND_ENVELOPE = re.compile(r"\] \[(?:INFO |WARN |ERROR|DEBUG|TRACE|FATAL)\] \[")
# r23 (the sweep: a cut record's `[INFO ] [BOOT] [BUGCHECK] code=0x1` spliced in after a time made another record): an
# envelope's LEVEL - `[INFO ]` or `INFO ] [` - inside a message is a cut record's envelope (archive survey: no message of
# the 37 real logs carries one)
LEVEL_PIECE = re.compile(r"\[(?:INFO |WARN |ERROR|DEBUG|TRACE|FATAL)\]|(?:INFO |WARN |ERROR|DEBUG|TRACE|FATAL)\] \[")
# ... and its CATEGORY with a tag after it - `[BOOT] [NATIVE_FLUSH]` - after a message's opening tag: a cut record's
# category and tag (archive survey: no message of the 37 real logs carries a `[NAME] [` after its opening tag)
CATEGORY_PIECE = re.compile(r"\[[A-Za-z0-9_]+\] \[")
# ... and a verdict-bearing record's body holds no TAG-like group - `[NATIVE_FLUSH]`: another record's tag, its record
# spliced in after this one's cut remainder (archive survey: no verdict-bearing record's body carries one; [KBPAD_OBS],
# [CS_WAIT], [LOAD_DONE] and [LOAD_OFF] - no verdict reader's - do)
TAG_LIKE = re.compile(r"\[(?=[^\]]*[A-Za-z])[A-Za-z0-9_:.\-]+\]")   # a tag's alphabet (`[ACQ:catopen]` too), a letter in it
LOG_BACKSTEP_S = 2.0   # r14: the largest backward step between consecutive lines a real log shows is 0.343 s (archive)


def log_integrity(log, data=None):
    """Raise ValueError unless the run log is well-formed as a whole (STRICT LOG INTEGRITY above); None when it is.
    data: the log's bytes, when the caller holds them (the parse-boundary sweep) - else the file is read."""
    if data is None:
        with open(log, "rb") as f:
            data = f.read()
    if data and not data.endswith(b"\n"):
        raise ValueError("log integrity: the run log's last line is cut (no final newline)")
    in_hang = False
    prev = None
    try:   # r15 (Astra fb6f4c6 r14 / conv14: a raw 0xFF in a record became a character the forms accepted): decoded strictly
        text = data.decode("utf-8")
    except UnicodeDecodeError as e:
        raise ValueError("log integrity: the run log is not UTF-8 (byte %d: %s)" % (e.start, e.reason))
    for n, line in enumerate(text.split("\n")[:-1] if data else [], 1):
        line = line.rstrip("\r")
        m = LOG_LINE.match(line)
        if not m:
            raise ValueError("log integrity: line %d is not a log record: %r" % (n, line[:120]))
        # r14 (Astra 263c082 r13 / conv13 P1: `10:99:30.000` passed by shape and world_floor compares times as strings):
        # every record's time is a real one, and the log runs forward (threads interleave by < LOG_BACKSTEP_S)
        try:
            t = datetime.datetime.strptime(line[1:24], "%Y-%m-%d %H:%M:%S.%f")
        except ValueError:
            raise ValueError("log integrity: line %d's time is not a time: %r" % (n, line[:40]))
        if prev is not None and (prev - t).total_seconds() > LOG_BACKSTEP_S:
            raise ValueError("log integrity: line %d's time runs %.1f s backwards: %r"
                             % (n, (prev - t).total_seconds(), line[:40]))
        prev = t if prev is None or t > prev else prev
        if INNER_TS.search(line, 1):
            raise ValueError("log integrity: line %d carries a second record inside it: %r" % (n, line[:120]))
        if RECORD_START.search(line, 1):   # r23: a cut record start (its date) inside the line
            raise ValueError("log integrity: line %d carries a cut record start inside it: %r" % (n, line[:140]))
        msg = m.group(1)
        if SECOND_ENVELOPE.search(msg):
            raise ValueError("log integrity: line %d carries a second record envelope: %r" % (n, line[:120]))
        if LEVEL_PIECE.search(msg):   # r23
            raise ValueError("log integrity: line %d carries a record envelope's level inside it: %r" % (n, line[:140]))
        t = MSG_TAG.match(msg)
        if VERDICT_TAG.search(msg, t.end() if t else 0):
            raise ValueError("log integrity: line %d carries a verdict-bearing tag inside its message: %r"
                             % (n, line[:140]))
        if VERDICT_NAME.search(msg, t.end() if (t and t.group(1) in VERDICT_NAMES) else 0):   # r23
            raise ValueError("log integrity: line %d carries a verdict-bearing tag's name outside its opening tag: %r"
                             % (n, line[:140]))
        rest = msg[t.end():] if t else msg
        if CATEGORY_PIECE.search(rest):   # r23
            raise ValueError("log integrity: line %d carries a record's category and tag inside it: %r"
                             % (n, line[:140]))
        if t and t.group(1) in VERDICT_NAMES and TAG_LIKE.search(rest):   # r23
            raise ValueError("log integrity: line %d is a verdict-bearing record carrying another record's tag: %r"
                             % (n, line[:140]))
        if not brackets_pair(rest):   # r23
            raise ValueError("log integrity: line %d's brackets do not pair (a cut record inside it): %r"
                             % (n, line[:140]))
        if not t:
            if msg.startswith("["):
                raise ValueError("log integrity: line %d opens with a cut tag: %r" % (n, line[:120]))
            if not any(rx.match(msg) for rx in UNTAGGED_FORMS):
                raise ValueError("log integrity: line %d is an untagged message of no known form: %r" % (n, line[:120]))
            continue
        tag, body = t.group(1), msg[t.end():]
        if tag == "GPU_HANG":
            if body.startswith("==== trigger"):
                in_hang = True
            elif not in_hang and not body.startswith("dump #"):   # the dump writer's note follows a block (archive)
                raise ValueError("log integrity: line %d is a [GPU_HANG] record outside a trigger's block: %r"
                                 % (n, line[:120]))
            elif body.startswith("==== end"):
                in_hang = False
            continue
        forms = RECORD_FORMS.get(tag)
        if forms is not None and not any(rx.match(body) for rx in forms):
            raise ValueError("log integrity: line %d is not a whole [%s] record: %r" % (n, tag, line[:140]))



def record_problem(line, tags):
    """Why `line` is not one whole record as far as `tags` (tag names, e.g. ("PRESENT_DUMP",)) go, or None (a line that
    carries none of them is no concern of the caller's)."""
    # r23: a tag is recognised by its NAME and `]` - a record whose opening `[` was taken by a spliced fragment (`[IM_FREEZE]
    # ... PRESENT_DUMP] C:...`) is still this tag's record, and not one whole record
    hits = list(re.finditer(r"(?:%s)\]" % "|".join(re.escape(t) for t in tags), line))
    if not hits:
        return None
    m = ENVELOPE.match(line)
    if not m:
        return "a record without its [ts] [LEVEL] [CAT] envelope"
    try:   # r20 (Astra b090fd7 r19 / conv19 P2: `2026-99-99 99:99:99.000` passed by shape): a real time
        datetime.datetime.strptime(line[1:24], "%Y-%m-%d %H:%M:%S.%f")
    except ValueError:
        return "a record whose time is not a time"
    if SECOND_ENVELOPE.search(line, m.end() - 1):
        return "a second record envelope inside the line (an interleaved record)"
    if len(hits) > 1 or hits[0].start() != m.end() + 1 or line[m.end()] != "[":
        return "the record's tag is not its message's one opening tag (an embedded record)"
    if not re.match(r" \S", line[hits[0].end():]):   # r19: the tag, ONE space, then a body (not bare, not a tab)
        return "the record's tag is not followed by a space and a body (a cut or corrupt record)"
    forms = _FORMS_RX.get(hits[0].group(0)[:-1])
    if forms is not None and not any(rx.match(line[hits[0].end() + 1:]) for rx in forms):   # r21: its whole form
        return "the record is not one of its tag's whole forms (a cut, corrupt or interleaved record)"
    return None

#!/usr/bin/env python3
"""
Log SQLite indexer: tails the harness log and indexes every line into SQLite
for fast queries. Run alongside a game session or post-hoc on a completed log.

Live tail (run while game is running):
    python scripts/log_to_sqlite.py --tail logs/latest_crash.log

Index an existing log file:
    python scripts/log_to_sqlite.py --index logs/iter149_err.log

Query the indexed log:
    python scripts/log_to_sqlite.py --query "SELECT * FROM events WHERE message LIKE '%8200094C%' LIMIT 20"
    python scripts/log_to_sqlite.py --query "SELECT * FROM events WHERE category='WATCHPOINT' ORDER BY ts"
    python scripts/log_to_sqlite.py --watchpoint 0x8200094C
    python scripts/log_to_sqlite.py --presents           # present timeline
    python scripts/log_to_sqlite.py --around 33.5        # events near t=33.5s

Start as MCP server:
    python scripts/log_to_sqlite.py --mcp
"""

import argparse
import json
import re
import sqlite3
import sys
import time
from pathlib import Path

ROOT = Path(__file__).parent.parent
DB_PATH = ROOT / ".cache" / "log.db"

# Log line format:
# [2026-05-15 12:34:56.789] [INFO] [CATEGORY] message...
# Or: [WATCHPOINT] guestLR=0x... addr=0x... val=0x...
# Or: [D3D_DIAG] present #N
LINE_RE = re.compile(
    r'\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)\]\s+'
    r'\[([A-Z]+)\]\s+'          # severity
    r'\[([^\]]+)\]\s+'          # category
    r'(.*)'                      # message
)
# Fallback for lines without full timestamp prefix (e.g. [WATCHPOINT], [D3D_DIAG])
BARE_RE = re.compile(r'\[([A-Z_]+)\]\s+(.*)')

PRESENT_RE = re.compile(r'present #(\d+)')
WATCHPOINT_RE = re.compile(r'guestLR=0x([0-9A-Fa-f]+).*?addr=0x([0-9A-Fa-f]+).*?val=0x([0-9A-Fa-f]+)')
GUEST_LR_RE = re.compile(r'guestLR=0x([0-9A-Fa-f]+)')
ADDR_RE = re.compile(r'\b(0x8[0-9A-Fa-f]{7})\b')


def init_db(con):
    con.executescript("""
        CREATE TABLE IF NOT EXISTS events (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            ts          TEXT,
            elapsed_s   REAL,
            severity    TEXT,
            category    TEXT,
            message     TEXT,
            guest_lr    INTEGER,
            addr        INTEGER,
            val         INTEGER,
            present_num INTEGER
        );
        CREATE INDEX IF NOT EXISTS ev_category ON events(category);
        CREATE INDEX IF NOT EXISTS ev_addr     ON events(addr);
        CREATE INDEX IF NOT EXISTS ev_guest_lr ON events(guest_lr);
        CREATE INDEX IF NOT EXISTS ev_present  ON events(present_num);
        CREATE INDEX IF NOT EXISTS ev_elapsed  ON events(elapsed_s);
    """)
    con.commit()


def parse_line(raw: str, session_start_ts: list) -> dict:
    raw = raw.rstrip()
    row = {"ts": None, "elapsed_s": None, "severity": None,
           "category": None, "message": raw,
           "guest_lr": None, "addr": None, "val": None, "present_num": None}

    m = LINE_RE.match(raw)
    if m:
        row["ts"] = m.group(1)
        row["severity"] = m.group(2)
        row["category"] = m.group(3)
        row["message"] = m.group(4)
        # Compute elapsed seconds from first line
        try:
            from datetime import datetime
            dt = datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f")
            if not session_start_ts:
                session_start_ts.append(dt)
            row["elapsed_s"] = (dt - session_start_ts[0]).total_seconds()
        except Exception:
            pass
    else:
        bm = BARE_RE.match(raw)
        if bm:
            row["category"] = bm.group(1)
            row["message"] = bm.group(2)

    msg = row["message"] or ""

    # Extract guest_lr
    glm = GUEST_LR_RE.search(msg)
    if glm:
        row["guest_lr"] = int(glm.group(1), 16)

    # Extract watchpoint addr + val
    wm = WATCHPOINT_RE.search(msg)
    if wm:
        row["addr"] = int(wm.group(2), 16)
        row["val"] = int(wm.group(3), 16)
    else:
        # Generic address extraction (first 0x82xxxxxx)
        am = ADDR_RE.search(msg)
        if am:
            row["addr"] = int(am.group(1), 16)

    # Present number
    pm = PRESENT_RE.search(msg)
    if pm:
        row["present_num"] = int(pm.group(1))

    return row


def index_file(log_path: Path, db_path: Path = DB_PATH):
    db_path.parent.mkdir(parents=True, exist_ok=True)
    con = sqlite3.connect(db_path)
    init_db(con)
    # Reset prior contents: "index this log" should leave the DB reflecting ONLY
    # this log. Without this, every re-index appended, so the table accumulated
    # rows from every previously-indexed log/run (millions of stale rows across
    # dates) and queries needed manual id-slicing to isolate the current run.
    con.execute("DELETE FROM events")
    # sqlite_sequence only exists once an AUTOINCREMENT table has been inserted into.
    if con.execute("SELECT 1 FROM sqlite_master WHERE type='table' AND name='sqlite_sequence'").fetchone():
        con.execute("DELETE FROM sqlite_sequence WHERE name='events'")
    session_start_ts = []
    rows = []

    with open(log_path, encoding="utf-8", errors="replace") as f:
        for line in f:
            r = parse_line(line, session_start_ts)
            rows.append((r["ts"], r["elapsed_s"], r["severity"], r["category"],
                          r["message"], r["guest_lr"], r["addr"], r["val"], r["present_num"]))

    con.executemany(
        "INSERT INTO events (ts,elapsed_s,severity,category,message,guest_lr,addr,val,present_num) VALUES (?,?,?,?,?,?,?,?,?)",
        rows
    )
    con.commit()
    con.close()
    # NOTE: status goes to stderr, never stdout — under --mcp stdout (fd 1) is the
    # JSON-RPC channel, and a stray print there both corrupts the protocol and (on
    # Windows' cp1252 console) raises UnicodeEncodeError on non-ASCII like "->".
    print(f"Indexed {len(rows)} lines from {log_path} -> {db_path}", file=sys.stderr)
    return len(rows)


def tail_file(log_path: Path, db_path: Path = DB_PATH):
    db_path.parent.mkdir(parents=True, exist_ok=True)
    con = sqlite3.connect(db_path)
    init_db(con)
    session_start_ts = []
    print(f"Tailing {log_path} -> {db_path}  (Ctrl-C to stop)", file=sys.stderr)

    with open(log_path, encoding="utf-8", errors="replace") as f:
        f.seek(0, 2)  # seek to end for live tail
        while True:
            line = f.readline()
            if line:
                r = parse_line(line, session_start_ts)
                con.execute(
                    "INSERT INTO events (ts,elapsed_s,severity,category,message,guest_lr,addr,val,present_num) VALUES (?,?,?,?,?,?,?,?,?)",
                    (r["ts"], r["elapsed_s"], r["severity"], r["category"],
                     r["message"], r["guest_lr"], r["addr"], r["val"], r["present_num"])
                )
                con.commit()
            else:
                time.sleep(0.05)


def run_query(sql: str, db_path: Path = DB_PATH):
    if not db_path.exists():
        print("No log database. Run --index or --tail first.")
        return
    con = sqlite3.connect(db_path)
    cur = con.execute(sql)
    cols = [d[0] for d in cur.description]
    rows = cur.fetchall()
    con.close()
    print("\t".join(cols))
    for r in rows:
        print("\t".join(str(x) for x in r))
    print(f"({len(rows)} rows)")


# ── MCP server ────────────────────────────────────────────────────────────────

MCP_TOOLS = [
    {
        "name": "log_query",
        "description": "Run a SQL query against the indexed harness log. Table: events(id, ts, elapsed_s, severity, category, message, guest_lr, addr, val, present_num).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "sql": {"type": "string", "description": "SQL SELECT statement"},
                "limit": {"type": "integer", "description": "Max rows (default 50)"}
            },
            "required": ["sql"]
        }
    },
    {
        "name": "log_watchpoint",
        "description": "Find all log events that reference a specific guest address.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "address": {"type": "string", "description": "Guest VA hex string, e.g. '0x8200094C'"}
            },
            "required": ["address"]
        }
    },
    {
        "name": "log_around",
        "description": "Show all log events within ±N seconds of a given elapsed time.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "elapsed_s": {"type": "number"},
                "window_s": {"type": "number", "description": "Half-window in seconds (default 1.0)"}
            },
            "required": ["elapsed_s"]
        }
    },
    {
        "name": "log_index",
        "description": "Index a harness log file into the SQLite database.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "log_path": {"type": "string"}
            },
            "required": ["log_path"]
        }
    }
]


def mcp_server():
    import io
    stdin = io.TextIOWrapper(sys.stdin.buffer, encoding="utf-8")
    stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8")

    def send(obj):
        stdout.write(json.dumps(obj) + "\n")
        stdout.flush()

    def handle(req):
        method = req.get("method", "")
        rid = req.get("id")

        if method == "initialize":
            return {"jsonrpc": "2.0", "id": rid, "result": {
                "protocolVersion": "2024-11-05",
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "log-sqlite", "version": "1.0"}
            }}

        if method == "tools/list":
            return {"jsonrpc": "2.0", "id": rid, "result": {"tools": MCP_TOOLS}}

        if method == "tools/call":
            name = req["params"]["name"]
            args = req["params"].get("arguments", {})
            text = ""

            try:
                if not DB_PATH.exists() and name != "log_index":
                    text = "No log database. Use log_index first."
                elif name == "log_query":
                    sql = args["sql"]
                    limit = args.get("limit", 50)
                    if "limit" not in sql.lower():
                        sql += f" LIMIT {limit}"
                    con = sqlite3.connect(DB_PATH)
                    cur = con.execute(sql)
                    cols = [d[0] for d in cur.description]
                    rows = cur.fetchall()
                    con.close()
                    text = "\t".join(cols) + "\n"
                    text += "\n".join("\t".join(str(x) for x in r) for r in rows)
                    text += f"\n({len(rows)} rows)"

                elif name == "log_watchpoint":
                    addr = int(args["address"], 16)
                    con = sqlite3.connect(DB_PATH)
                    rows = con.execute(
                        "SELECT elapsed_s, category, guest_lr, val, message FROM events "
                        "WHERE addr=? ORDER BY id LIMIT 100", (addr,)
                    ).fetchall()
                    con.close()
                    text = "\n".join(
                        f"t={r[0]:.3f}s [{r[1]}] lr={hex(r[2]) if r[2] else '?'} "
                        f"val={hex(r[3]) if r[3] else '?'} | {r[4][:120]}"
                        for r in rows
                    ) or "No events found for that address."

                elif name == "log_around":
                    t = args["elapsed_s"]
                    w = args.get("window_s", 1.0)
                    con = sqlite3.connect(DB_PATH)
                    rows = con.execute(
                        "SELECT elapsed_s, category, message FROM events "
                        "WHERE elapsed_s BETWEEN ? AND ? ORDER BY id LIMIT 200",
                        (t - w, t + w)
                    ).fetchall()
                    con.close()
                    text = "\n".join(
                        f"t={r[0]:.3f}s [{r[1]}] {r[2][:120]}" for r in rows
                    ) or "No events in that window."

                elif name == "log_index":
                    n = index_file(Path(args["log_path"]))
                    text = f"Indexed {n} lines from {args['log_path']}"

            except Exception as e:
                text = f"Error: {e}"

            return {"jsonrpc": "2.0", "id": rid, "result": {
                "content": [{"type": "text", "text": text}]
            }}

        if method == "notifications/initialized":
            return None

        return {"jsonrpc": "2.0", "id": rid, "error": {"code": -32601, "message": f"Unknown: {method}"}}

    for line in stdin:
        line = line.strip()
        if not line:
            continue
        try:
            resp = handle(json.loads(line))
            if resp is not None:
                send(resp)
        except Exception as e:
            send({"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": str(e)}})


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--index", metavar="LOG", help="Index a log file")
    parser.add_argument("--tail", metavar="LOG", help="Live-tail a log file")
    parser.add_argument("--query", metavar="SQL", help="Run a SQL query")
    parser.add_argument("--watchpoint", metavar="ADDR", help="Find events for guest addr (hex)")
    parser.add_argument("--around", metavar="T", type=float, help="Events near elapsed time T seconds")
    parser.add_argument("--presents", action="store_true", help="Show present timeline")
    parser.add_argument("--mcp", action="store_true", help="Run as MCP stdio server")
    args = parser.parse_args()

    if args.mcp:
        mcp_server()
    elif args.index:
        index_file(Path(args.index))
    elif args.tail:
        tail_file(Path(args.tail))
    elif args.query:
        run_query(args.query)
    elif args.watchpoint:
        addr = int(args.watchpoint, 16)
        run_query(f"SELECT elapsed_s, category, guest_lr, val, message FROM events WHERE addr={addr} ORDER BY id")
    elif args.around:
        run_query(f"SELECT elapsed_s, category, message FROM events WHERE elapsed_s BETWEEN {args.around-1} AND {args.around+1} ORDER BY id")
    elif args.presents:
        run_query("SELECT elapsed_s, present_num, message FROM events WHERE present_num IS NOT NULL ORDER BY present_num")
    else:
        parser.print_help()

#!/usr/bin/env python3
"""
Log SQLite Indexer — parse the Superman Returns runtime log into a SQLite
database for fast structured queries.

Usage:
  # Index a log file:
  python scripts/log_sqlite.py index superman_returns.log [--db logs/sr.db]

  # Query:
  python scripts/log_sqlite.py query --category D3D_DIAG [--db logs/sr.db]
  python scripts/log_sqlite.py query --text "0x8A260CC0"
  python scripts/log_sqlite.py query --category WATCHPOINT --around-present 5000
  python scripts/log_sqlite.py query --category KERNEL_IMPORT --guest-lr 0x825F70E0
  python scripts/log_sqlite.py stats [--db logs/sr.db]

Schema:
  log_lines(id, line_num, present_num, category, guest_lr, thread_id,
            source_loc, message, raw_line)
"""

import re
import sys
import sqlite3
import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_DB = ROOT / 'logs' / 'sr.db'

# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------

# Category tag patterns (all uppercase in log output)
_CAT_PATTERNS = [
    ('WATCHPOINT',     re.compile(r'\[WATCHPOINT\]')),
    ('KERNEL_IMPORT',  re.compile(r'\[KERNEL:IMPORT\]')),
    ('RENDER_PUMP',    re.compile(r'\[RENDER_PUMP\]')),
    ('WORKER_TRACE',   re.compile(r'\[WORKER_TRACE\]')),
    ('D3D_DIAG',       re.compile(r'\[D3D_DIAG\]')),
    ('CRASH',          re.compile(r'\[CRASH\]|Guest harness: exception|ExCreateThread.*exception')),
    ('INPUT_REC',      re.compile(r'\[INPUT_REC\]')),
    ('SPLASH',         re.compile(r'\[SPLASH\]')),
    ('SAVESTATE',      re.compile(r'\[SAVESTATE\]')),
    ('BOOT',           re.compile(r'\[BOOT\]')),
]

# Extract guestLR from lines like "guestLR=0x824DB158"
_GUEST_LR = re.compile(r'guest(?:LR|_lr|Lr)\s*[=:]\s*(0x[0-9A-Fa-f]+)', re.IGNORECASE)
# Extract thread ID from "[threadN]" or "thread=N"
_THREAD_ID = re.compile(r'\[thread\s*(\d+)\]|thread[=\s]+(\d+)', re.IGNORECASE)
# Extract source location "file.cpp:1234"
_SOURCE_LOC = re.compile(r'([\w/\\]+\.cpp):(\d+)')
# Extract present number from [D3D_DIAG] present #N
_PRESENT = re.compile(r'present\s+#(\d+)', re.IGNORECASE)

# Global running present counter (updated as we see D3D_DIAG lines)
_current_present = 0


def _classify(line: str) -> str:
    for name, pat in _CAT_PATTERNS:
        if pat.search(line):
            return name
    return 'OTHER'


def _extract(line: str, present_running: list) -> dict:
    """Parse a log line into structured fields. present_running is a 1-element list for state."""
    cat = _classify(line)

    # Update present counter
    pm = _PRESENT.search(line)
    if pm:
        present_running[0] = int(pm.group(1))

    m_lr = _GUEST_LR.search(line)
    guest_lr = m_lr.group(1) if m_lr else None

    m_th = _THREAD_ID.search(line)
    if m_th:
        thread_id = m_th.group(1) or m_th.group(2)
    else:
        thread_id = None

    m_sl = _SOURCE_LOC.search(line)
    source_loc = f"{m_sl.group(1)}:{m_sl.group(2)}" if m_sl else None

    return {
        'present_num': present_running[0],
        'category': cat,
        'guest_lr': guest_lr,
        'thread_id': thread_id,
        'source_loc': source_loc,
        'message': line.strip(),
    }


# ---------------------------------------------------------------------------
# DB helpers
# ---------------------------------------------------------------------------

def open_db(db_path: Path) -> sqlite3.Connection:
    db_path.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(db_path)
    conn.row_factory = sqlite3.Row
    conn.execute('PRAGMA journal_mode=WAL')
    conn.execute('PRAGMA synchronous=NORMAL')
    return conn


def create_schema(conn: sqlite3.Connection):
    conn.executescript("""
        CREATE TABLE IF NOT EXISTS log_lines (
            id          INTEGER PRIMARY KEY,
            line_num    INTEGER,
            present_num INTEGER,
            category    TEXT,
            guest_lr    TEXT,
            thread_id   TEXT,
            source_loc  TEXT,
            message     TEXT
        );
        CREATE INDEX IF NOT EXISTS idx_cat     ON log_lines(category);
        CREATE INDEX IF NOT EXISTS idx_present ON log_lines(present_num);
        CREATE INDEX IF NOT EXISTS idx_lr      ON log_lines(guest_lr);
        CREATE INDEX IF NOT EXISTS idx_thread  ON log_lines(thread_id);
    """)
    conn.commit()


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

def cmd_index(args):
    log_path = Path(args.log)
    if not log_path.exists():
        print(f"ERROR: log not found: {log_path}", file=sys.stderr)
        return 1

    db_path = Path(args.db)
    conn = open_db(db_path)
    create_schema(conn)

    # Clear existing data
    conn.execute('DELETE FROM log_lines')
    conn.commit()

    present_state = [0]
    rows = []
    try:
        lines = log_path.read_text(encoding='utf-8', errors='replace').splitlines()
    except OSError as e:
        print(f"ERROR reading log: {e}", file=sys.stderr)
        return 1

    for lineno, line in enumerate(lines, 1):
        if not line.strip():
            continue
        rec = _extract(line, present_state)
        rows.append((
            lineno,
            rec['present_num'],
            rec['category'],
            rec['guest_lr'],
            rec['thread_id'],
            rec['source_loc'],
            rec['message'],
        ))

    conn.executemany(
        'INSERT INTO log_lines(line_num,present_num,category,guest_lr,thread_id,source_loc,message) VALUES(?,?,?,?,?,?,?)',
        rows
    )
    conn.commit()
    conn.close()

    print(f"Indexed {len(rows)} lines from {log_path.name} -> {db_path}")
    return 0


def cmd_query(args):
    db_path = Path(args.db)
    if not db_path.exists():
        print(f"No database at {db_path}. Run: python scripts/log_sqlite.py index <logfile>", file=sys.stderr)
        return 1

    conn = open_db(db_path)
    where_parts = []
    params = []

    if args.category:
        where_parts.append('category = ?')
        params.append(args.category.upper())

    if args.guest_lr:
        lr = args.guest_lr if args.guest_lr.startswith('0x') else '0x' + args.guest_lr
        where_parts.append('guest_lr = ?')
        params.append(lr.lower())

    if args.thread:
        where_parts.append('thread_id = ?')
        params.append(str(args.thread))

    if args.text:
        where_parts.append('message LIKE ?')
        params.append(f'%{args.text}%')

    if args.around_present is not None:
        window = getattr(args, 'present_window', 200)
        where_parts.append('present_num BETWEEN ? AND ?')
        params.extend([args.around_present - window, args.around_present + window])

    where_sql = ('WHERE ' + ' AND '.join(where_parts)) if where_parts else ''
    limit = getattr(args, 'limit', 100)
    sql = f'SELECT * FROM log_lines {where_sql} ORDER BY line_num LIMIT {limit}'

    rows = conn.execute(sql, params).fetchall()
    conn.close()

    if not rows:
        print("No results.")
        return 0

    for row in rows:
        lr_tag = f" lr={row['guest_lr']}" if row['guest_lr'] else ''
        th_tag = f" th={row['thread_id']}" if row['thread_id'] else ''
        print(f"[{row['present_num']:>6}] {row['category']:<14}{lr_tag}{th_tag}")
        print(f"  {row['message'][:200]}")

    print(f"\n({len(rows)} results)")
    return 0


def cmd_stats(args):
    db_path = Path(args.db)
    if not db_path.exists():
        print(f"No database at {db_path}.", file=sys.stderr)
        return 1

    conn = open_db(db_path)

    total = conn.execute('SELECT COUNT(*) FROM log_lines').fetchone()[0]
    max_present = conn.execute('SELECT MAX(present_num) FROM log_lines').fetchone()[0]
    cats = conn.execute(
        'SELECT category, COUNT(*) as n FROM log_lines GROUP BY category ORDER BY n DESC'
    ).fetchall()
    unique_lrs = conn.execute(
        'SELECT COUNT(DISTINCT guest_lr) FROM log_lines WHERE guest_lr IS NOT NULL'
    ).fetchone()[0]
    crash_count = conn.execute(
        "SELECT COUNT(*) FROM log_lines WHERE category='CRASH'"
    ).fetchone()[0]
    conn.close()

    print(f"Log stats:")
    print(f"  Total lines  : {total:,}")
    print(f"  Max present  : {max_present}")
    print(f"  Unique LRs   : {unique_lrs}")
    print(f"  Crash events : {crash_count}")
    print(f"\nLines by category:")
    for row in cats:
        print(f"  {row['category']:<16} {row['n']:>8,}")
    return 0


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--db', default=str(DEFAULT_DB), help=f'SQLite database path (default: {DEFAULT_DB})')
    sub = ap.add_subparsers(dest='cmd', required=True)

    # index
    p_idx = sub.add_parser('index', help='Parse log file into database')
    p_idx.add_argument('log', help='Path to superman_returns.log')

    # query
    p_q = sub.add_parser('query', help='Query indexed log')
    p_q.add_argument('--category', '-c', help='Filter by category (e.g. WATCHPOINT, D3D_DIAG)')
    p_q.add_argument('--guest-lr', help='Filter by guest LR (e.g. 0x825F70E0)')
    p_q.add_argument('--thread', help='Filter by thread ID')
    p_q.add_argument('--text', '-t', help='Full-text search in message')
    p_q.add_argument('--around-present', type=int, metavar='N', help='Lines near present #N')
    p_q.add_argument('--present-window', type=int, default=200, help='Window around present (default: 200)')
    p_q.add_argument('--limit', type=int, default=100, help='Max results (default: 100)')

    # stats
    sub.add_parser('stats', help='Show statistics about indexed log')

    args = ap.parse_args()

    if args.cmd == 'index':
        sys.exit(cmd_index(args))
    elif args.cmd == 'query':
        sys.exit(cmd_query(args))
    elif args.cmd == 'stats':
        sys.exit(cmd_stats(args))


if __name__ == '__main__':
    main()

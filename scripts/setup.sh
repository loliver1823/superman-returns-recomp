#!/bin/bash
# setup.sh — One-command setup for the recomp pipeline
# Run from your project root: bash scripts/setup.sh

set -e

echo "=== Installing Python dependencies ==="
pip install -r requirements.txt

echo ""
echo "=== Initializing RAG index ==="
python scripts/setup_rag.py

echo ""
echo "=== Testing crash parser ==="
if [ -f "logs/superman_returns_crash.log" ]; then
    python scripts/parse_crash_log.py logs/superman_returns_crash.log | python -m json.tool | head -30
    echo "  ... (truncated)"
else
    echo "  No crash log found at logs/superman_returns_crash.log"
    echo "  Place your crash logs in logs/ and re-run, or run the parser manually:"
    echo "    python scripts/parse_crash_log.py <your_crash.log>"
fi

echo ""
echo "=== Setup complete ==="
echo ""
echo "Next steps:"
echo "  1. Place your crash logs in logs/"
echo "  2. Open Claude Code in this project directory"
echo "  3. Give it a crash log and say: 'Analyze this crash and fix it'"
echo "  4. Claude will follow the workflow in CLAUDE.md automatically"

"""Validate the actual replay child result independently of its shell wrapper."""
import hashlib
import json
import ntpath
from pathlib import Path

SCHEMA = 'sr-replay-process/1'


def read(path):
    try:
        return json.loads(Path(path).read_text(encoding='utf-8-sig'))
    except (OSError, ValueError, TypeError):
        return None


def errors(receipt, *, exe, trace, stderr=None, stdout=None):
    if not isinstance(receipt, dict) or receipt.get('schema') != SCHEMA:
        return ['missing or invalid replay child receipt']
    problems = []
    for field, value in [('terminal_observed', True), ('timed_out', False),
                         ('output_complete', True), ('frame_generated', True)]:
        if receipt.get(field) is not value:
            problems.append('replay receipt ' + field + ' is not ' + str(value))
    if type(receipt.get('exit_code')) is not int or receipt['exit_code'] != 0:
        problems.append('replay child did not exit 0')
    if type(receipt.get('pid')) is not int or receipt['pid'] <= 0:
        problems.append('replay child PID is invalid')
    for field, expected in [('exe', exe), ('trace', trace)]:
        actual = receipt.get(field)
        if (not isinstance(expected, str) or not expected or not isinstance(actual, str) or
                not ntpath.isabs(expected) or not ntpath.isabs(actual) or
                ntpath.normcase(ntpath.normpath(actual)) != ntpath.normcase(ntpath.normpath(expected))):
            problems.append('replay receipt ' + field + ' does not match the expected job')
    for field, path in [('stderr', stderr), ('stdout', stdout)]:
        if path is None:
            problems.append('missing retained replay ' + field)
            continue
        try:
            with Path(path).open('rb') as stream:
                actual = hashlib.file_digest(stream, 'sha256').hexdigest()
            if actual != receipt.get(field + '_sha256'):
                problems.append('retained replay ' + field + ' hash mismatch')
        except OSError:
            problems.append('missing retained replay ' + field)
    return problems

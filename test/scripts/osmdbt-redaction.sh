#!/bin/bash
#
#  Test use of osmdbt-get-log command when redactions present
#

set -e
set -x

. "$SRCDIR/setup.sh"

# If there is no data osmdbt-get-log does nothing
../src/osmdbt-get-log --config="$CONFIG"

# Load some test data
psql --quiet <"$SRCDIR/meta.sql"
psql --quiet <"$SRCDIR/testdata.sql"

# Redact two node versions
psql --quiet -c 'UPDATE nodes SET redaction_id = 10 WHERE version = 1;'

# Undo previous redaction
psql --quiet -c 'UPDATE nodes SET redaction_id = null WHERE version = 1;'

# Reading log
../src/osmdbt-get-log --config="$CONFIG" --catchup

# There should be exactly one log file
test $(ls -1 "$TESTDIR/log" | wc -l) -eq 1

# Determine name of log file
LOGFILE="$TESTDIR/log/"$(ls "$TESTDIR/log")

# Check content of log file
test $(wc -l <"$LOGFILE") -eq 13
test $(grep --count ' C$' "$LOGFILE") -eq 3
grep --quiet 'N n10 v1 c1$' "$LOGFILE"
grep --quiet 'N n11 v1 c1$' "$LOGFILE"
grep --quiet 'N n10 v2 c2$' "$LOGFILE"
grep --quiet 'N n11 v2 c2$' "$LOGFILE"
grep --quiet 'N w20 v1 c1$' "$LOGFILE"
grep --quiet 'N r30 v1 c1$' "$LOGFILE"

# Check redacted node versions along with redaction id
grep --quiet 'R n10 v1 c1 10$' "$LOGFILE"
grep --quiet 'R n11 v1 c1 10$' "$LOGFILE"

# Check un-redacted node versions along with NULL redaction id
grep --quiet 'R n10 v1 c1 NULL$' "$LOGFILE"
grep --quiet 'R n11 v1 c1 NULL$' "$LOGFILE"

../src/osmdbt-create-diff --config="$CONFIG" --sequence-number=42 --dry-run

zgrep --quiet 'node id="10" version="2"' "$TESTDIR/tmp/new-change.osc.gz"
zgrep --quiet 'node id="11" version="2"' "$TESTDIR/tmp/new-change.osc.gz"
zgrep --quiet 'way id="20" version="1"'  "$TESTDIR/tmp/new-change.osc.gz"
zgrep --quiet 'relation id="30" version="1"' "$TESTDIR/tmp/new-change.osc.gz"


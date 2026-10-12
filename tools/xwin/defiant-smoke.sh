#!/bin/bash
# Runs INSIDE ghoti-cross-mingw64:deb13 (through m1-win.sh). Starts the
# defiant program built for win64 under wine, in its memory mode, and speaks
# HTTP to it with bash's /dev/tcp: /alive, the sign-in page, a login that
# redirects, and a wrong password that does not. Prints "SMOKE ok" or why not.
set -u
EXE=/w/defiant-web/build/win64/release/apps/defiant.exe
[ -f "$EXE" ] || { echo "SMOKE no defiant.exe at $EXE"; exit 1; }
LOG=/w/logs/defiant.exe.log
export DEFIANT_LISTEN=127.0.0.1:0 DEFIANT_ADMIN_USER=ada DEFIANT_ADMIN_PASSWORD=smoke-pass-1
export PATH=/w/prefix/bin:$PATH
( cd /w/defiant-web/build/win64/release/apps && exec "$EXE" ) >/dev/null 2>"$LOG" &
PID=$!
trap 'kill $PID 2>/dev/null' EXIT
PORT=
for _ in $(seq 150); do
  PORT=$(tr -d '\r' < "$LOG" | sed -n 's/.*listening on port \([0-9]*\).*/\1/p')
  [ -n "$PORT" ] && break
  kill -0 $PID 2>/dev/null || break
  sleep 0.2
done
[ -n "$PORT" ] || { echo "SMOKE the program did not start: $(tr -d '\r' < $LOG | tail -3)"; exit 1; }
ask() { # ask <raw request>: prints the reply
  exec 3<>/dev/tcp/127.0.0.1/$PORT || return 1
  printf '%b' "$1" >&3
  cat <&3
  exec 3<&-
}
alive=$(ask 'GET /alive HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n' | head -1 | tr -d '\r')
[ "$alive" = "HTTP/1.1 200 OK" ] || { echo "SMOKE /alive said: $alive"; exit 1; }
page=$(ask 'GET /login HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n' | tr -d '\r')
echo "$page" | head -1 | grep -q '200' && echo "$page" | grep -q 'action="/login"' || { echo "SMOKE no sign-in page"; exit 1; }
body='username=ada&password=smoke-pass-1'
good=$(ask "POST /login HTTP/1.1\r\nHost: 127.0.0.1\r\nOrigin: http://127.0.0.1\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: ${#body}\r\nConnection: close\r\n\r\n$body" | tr -d '\r')
echo "$good" | head -1 | grep -q '303' && echo "$good" | grep -qi '^location: /admin' || { echo "SMOKE a good login did not redirect to /admin"; exit 1; }
body='username=ada&password=wrong'
bad=$(ask "POST /login HTTP/1.1\r\nHost: 127.0.0.1\r\nOrigin: http://127.0.0.1\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: ${#body}\r\nConnection: close\r\n\r\n$body" | tr -d '\r')
echo "$bad" | head -1 | grep -q '401' || { echo "SMOKE a bad login was not refused"; exit 1; }
echo "SMOKE ok: alive, sign-in page, login, refusal"

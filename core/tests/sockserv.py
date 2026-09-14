#!/usr/bin/env python3
"""Three listeners plus one guaranteed-closed port, for test_sock.

Prints: <echo_port> <silent_port> <eof_port> <closed_port>

  echo    accepts and echoes whatever it receives
  silent  accepts and never sends anything (drives the read timeout)
  eof     accepts and closes immediately (drives the clean-EOF path)
  closed  bound then released, so connecting to it is refused
"""
import socket, sys, threading


def listener(handler):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", 0))
    s.listen(8)
    port = s.getsockname()[1]

    def serve():
        while True:
            try:
                conn, _ = s.accept()
            except OSError:
                return
            threading.Thread(target=handler, args=(conn,), daemon=True).start()

    threading.Thread(target=serve, daemon=True).start()
    return port


def h_echo(conn):
    try:
        while True:
            d = conn.recv(4096)
            if not d:
                break
            conn.sendall(d)
    finally:
        conn.close()


def h_silent(conn):
    # Hold the connection open and send nothing at all.
    import time
    time.sleep(120)
    conn.close()


def h_eof(conn):
    conn.close()


def main():
    echo   = listener(h_echo)
    silent = listener(h_silent)
    eof    = listener(h_eof)

    tmp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    tmp.bind(("127.0.0.1", 0))
    closed = tmp.getsockname()[1]
    tmp.close()

    print("%d %d %d %d" % (echo, silent, eof, closed), flush=True)
    threading.Event().wait()


if __name__ == "__main__":
    main()

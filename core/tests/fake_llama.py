#!/usr/bin/env python3
"""Minimal stand-in for llama.cpp's /v1/chat/completions streaming endpoint.

Deliberately hostile framing: chunk boundaries land inside JSON strings, inside
SSE field names and between CR and LF, and tool-call arguments are split across
many events. That is what a real server under load looks like from the client
side, and it is precisely where a parser that keeps state in locals falls over.
"""
import json, socket, sys, threading, time

TEXT = ["Hello", " from", " llama.cpp,", " caf\u00e9", " \u25b6", " \U0001F680", "!"]
TOOL_ARGS = '{"path":"C:\\\\OS2\\\\CONFIG.SYS","limit":100}'


def sse_payloads():
    for frag in TEXT:
        yield json.dumps({"choices": [{"index": 0, "delta": {"content": frag}}]},
                         ensure_ascii=False)
    # Tool-call arguments arrive as fragments that only form valid JSON once
    # concatenated -- split at awkward points, including inside an escape.
    cuts = [0, 3, 9, 14, 15, 22, 30, len(TOOL_ARGS)]
    for a, b in zip(cuts, cuts[1:]):
        yield json.dumps({"choices": [{"index": 0, "delta": {"tool_calls": [
            {"index": 0, "function": {"arguments": TOOL_ARGS[a:b]}}]}}]},
            ensure_ascii=False)
    yield None  # [DONE]


def handle(conn):
    buf = b""
    while b"\r\n\r\n" not in buf:
        d = conn.recv(4096)
        if not d:
            conn.close()
            return
        buf += d
    head, _, rest = buf.partition(b"\r\n\r\n")
    clen = 0
    for line in head.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            clen = int(line.split(b":")[1])
    while len(rest) < clen:
        d = conn.recv(4096)
        if not d:
            break
        rest += d
    # The request body must be valid JSON or the client built it wrong.
    json.loads(rest.decode("utf-8"))

    conn.sendall(b"HTTP/1.1 200 OK\r\n"
                 b"Content-Type: text/event-stream\r\n"
                 b"Transfer-Encoding: chunked\r\n"
                 b"Cache-Control: no-cache\r\n"
                 b"\r\n")

    stream = b""
    for payload in sse_payloads():
        if payload is None:
            stream += b"data: [DONE]\n\n"
        else:
            stream += b"data: " + payload.encode("utf-8") + b"\n\n"

    # Dribble it out in small, uneven chunks with pauses, so the client sees
    # many short reads with events split across them.
    i, n = 0, 0
    sizes = [1, 7, 3, 29, 2, 13, 5, 61, 11]
    while i < len(stream):
        take = sizes[n % len(sizes)]
        piece = stream[i:i + take]
        conn.sendall(b"%x\r\n" % len(piece) + piece + b"\r\n")
        i += take
        n += 1
        if n % 5 == 0:
            time.sleep(0.001)
    conn.sendall(b"0\r\n\r\n")
    conn.close()


def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(4)
    print(srv.getsockname()[1], flush=True)
    while True:
        conn, _ = srv.accept()
        threading.Thread(target=handle, args=(conn,), daemon=True).start()


if __name__ == "__main__":
    main()

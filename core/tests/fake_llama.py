#!/usr/bin/env python3
"""Stand-in for llama.cpp's /v1/chat/completions, for the end-to-end test.

Two jobs.

1. Serve a deliberately hostile stream. Chunk boundaries land inside JSON
   strings, inside SSE field names and between CR and LF; tool-call arguments
   are split across several events, including mid-escape. That is what a real
   server under load looks like from the client side, and it is exactly where
   a parser that keeps state in locals falls over.

2. Assert what the client sent. The second request must replay the assistant
   turn WITH its tool_calls and the matching tool result, or the model would
   redo work it already did. Checking that here, on the wire, is stronger than
   checking it in the client's own tests.

Turn 1 asks for a file read. Turn 2 answers. Any failed assertion is reported
back as an HTTP 400 with the reason, so the C side fails loudly.
"""
import json, socket, sys, threading, time

TEXT = ["Reading", " that", " file", " now", " café", " ▶",
        " \U0001F680"]
TOOL_ARGS = '{"path":"probe.txt"}'
ANSWER = "The file says HELLO FROM OS2."

state = {"turn": 0, "errors": []}
lock = threading.Lock()


def sse(obj):
    return b"data: " + json.dumps(obj, ensure_ascii=False).encode() + b"\n\n"


def turn1_stream():
    out = b""
    for frag in TEXT:
        out += sse({"choices": [{"index": 0, "delta": {"content": frag}}]})
    cuts = [0, 1, 8, 9, 14, len(TOOL_ARGS)]
    first = True
    for a, b in zip(cuts, cuts[1:]):
        call = {"index": 0, "function": {"arguments": TOOL_ARGS[a:b]}}
        if first:
            call.update({"id": "call_probe", "type": "function"})
            call["function"]["name"] = "read"
            first = False
        out += sse({"choices": [{"index": 0,
                                 "delta": {"tool_calls": [call]}}]})
    out += sse({"choices": [{"index": 0, "delta": {},
                             "finish_reason": "tool_calls"}],
                "usage": {"prompt_tokens": 42, "completion_tokens": 17}})
    out += b"data: [DONE]\n\n"
    return out


def turn2_stream():
    out = b""
    for word in ANSWER.split(" "):
        out += sse({"choices": [{"index": 0,
                                 "delta": {"content": word + " "}}]})
    out += sse({"choices": [{"index": 0, "delta": {},
                             "finish_reason": "stop"}],
                "usage": {"prompt_tokens": 91, "completion_tokens": 8}})
    out += b"data: [DONE]\n\n"
    return out


def check_turn1(body):
    errs = []
    msgs = body.get("messages", [])
    if len(msgs) != 2:
        errs.append("turn 1 should carry system+user, got %d" % len(msgs))
    if msgs and msgs[0].get("role") != "system":
        errs.append("turn 1 message 0 is not the system prompt")
    names = [t["function"]["name"] for t in body.get("tools", [])]
    for want in ("read", "write", "edit", "ls", "grep", "cmd"):
        if want not in names:
            errs.append("tool %s not advertised" % want)
    if not isinstance(body.get("tools", [{}])[0]["function"]["parameters"],
                      dict):
        errs.append("tool schema was sent as a string, not an object")
    return errs


def check_turn2(body):
    """The replay assertion this whole file exists for."""
    errs = []
    msgs = body.get("messages", [])
    if len(msgs) != 4:
        errs.append("turn 2 should carry system+user+assistant+tool, got %d"
                    % len(msgs))
        return errs
    a = msgs[2]
    if a.get("role") != "assistant":
        errs.append("message 2 is %r, expected assistant" % a.get("role"))
    calls = a.get("tool_calls") or []
    if not calls:
        errs.append("assistant turn replayed WITHOUT its tool_calls; "
                    "the tool result below it is orphaned")
    else:
        if calls[0].get("id") != "call_probe":
            errs.append("tool call id not preserved: %r" % calls[0].get("id"))
        if calls[0]["function"]["name"] != "read":
            errs.append("tool call name not preserved")
        try:
            args = json.loads(calls[0]["function"]["arguments"])
        except Exception as e:
            errs.append("replayed arguments are not valid JSON: %s" % e)
        else:
            if args.get("path") != "probe.txt":
                errs.append("replayed arguments wrong: %r" % args)
    t = msgs[3]
    if t.get("role") != "tool":
        errs.append("message 3 is %r, expected tool" % t.get("role"))
    if t.get("tool_call_id") != "call_probe":
        errs.append("tool result not linked to its call: %r"
                    % t.get("tool_call_id"))
    if "HELLO FROM OS2" not in (t.get("content") or ""):
        errs.append("tool result does not carry what the tool returned: %r"
                    % t.get("content"))
    return errs


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

    errs = []
    if len(rest) != clen:
        errs.append("Content-Length said %d, body was %d" % (clen, len(rest)))
    try:
        body = json.loads(rest.decode("utf-8"))
    except Exception as e:
        errs.append("request body is not valid JSON: %s" % e)
        body = {}

    with lock:
        turn = state["turn"]
        state["turn"] += 1

    if not errs:
        errs = check_turn1(body) if turn == 0 else check_turn2(body)

    if errs:
        with lock:
            state["errors"].extend(errs)
        msg = ("; ".join(errs)).encode()
        conn.sendall(b"HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n"
                     b"Content-Length: %d\r\n\r\n" % len(msg) + msg)
        conn.close()
        return

    stream = turn1_stream() if turn == 0 else turn2_stream()

    conn.sendall(b"HTTP/1.1 200 OK\r\n"
                 b"Content-Type: text/event-stream\r\n"
                 b"Transfer-Encoding: chunked\r\n"
                 b"Cache-Control: no-cache\r\n"
                 b"\r\n")

    i, n = 0, 0
    sizes = [1, 7, 3, 29, 2, 13, 5, 61, 11]
    while i < len(stream):
        take = sizes[n % len(sizes)]
        piece = stream[i:i + take]
        conn.sendall(b"%x\r\n" % len(piece) + piece + b"\r\n")
        i += take
        n += 1
        if n % 7 == 0:
            time.sleep(0.001)
    conn.sendall(b"0\r\n\r\n")
    conn.close()


def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(8)
    print(srv.getsockname()[1], flush=True)
    while True:
        conn, _ = srv.accept()
        threading.Thread(target=handle, args=(conn,), daemon=True).start()


if __name__ == "__main__":
    main()

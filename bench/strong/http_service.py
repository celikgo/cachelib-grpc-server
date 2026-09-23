"""Disposable HTTP origin and HTTP-to-gRPC adapter for key-specific media bytes."""
import argparse
import http.client
import json
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import grpc
import cache_pb2 as pb
import cache_pb2_grpc as rpc


def payload(size, index):
    body = bytearray((bytes(range(256)) * (size // 256 + 1))[:size])
    body[:8] = index.to_bytes(8, "little")
    return bytes(body)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    count = 0
    lock = threading.Lock()
    cache = None
    mode = "origin"

    def fetch_origin(self):
        # A one-pass trace makes every request a miss. Reuse this handler's
        # origin connection just as the client and NGINX reuse connections;
        # opening a new TCP socket per object exhausts ephemeral ports in a
        # longer run and benchmarks the adapter's connection churn instead.
        conn = getattr(self, "origin_conn", None)
        if conn is None:
            conn = http.client.HTTPConnection("origin", 8080, timeout=10)
        for attempt in range(2):
            try:
                conn.request("GET", self.path)
                response = conn.getresponse()
                body = response.read()
                self.origin_conn = conn
                return response.status, body
            except (OSError, http.client.HTTPException):
                conn.close()
                if attempt:
                    raise
                conn = http.client.HTTPConnection("origin", 8080, timeout=10)

    def do_GET(self):
        if self.path == "/stats":
            with self.lock:
                body = json.dumps({"origin_requests": type(self).count}).encode()
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        match = re.fullmatch(r"/obj/(\d+)/(\d+)", self.path)
        if not match:
            self.send_error(404)
            return
        size, index = map(int, match.groups())
        if size < 8 or size > 1048576:
            self.send_error(413)
            return
        if self.mode == "origin":
            with self.lock:
                type(self).count += 1
            time.sleep(.005)  # disclosed simulated origin service time
            body, status = payload(size, index), "ORIGIN"
        else:
            key = "media:" + self.path
            r = self.cache.Get(pb.GetRequest(key=key), timeout=10)
            if r.found:
                body, status = r.value, "HIT"
            else:
                status_code, body = self.fetch_origin()
                if status_code != 200:
                    self.send_error(502)
                    return
                stored = self.cache.Set(pb.SetRequest(key=key, value=body,
                                                      ttl_seconds=3600), timeout=10)
                status = "MISS" if stored.success else "FILL_FAILED"
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "public, max-age=3600")
        self.send_header("X-Cache", status)
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        return


def main():
    p = argparse.ArgumentParser()
    p.add_argument("mode", choices=["origin", "adapter"])
    args = p.parse_args()
    Handler.mode = args.mode
    if args.mode == "adapter":
        Handler.cache = rpc.CacheServiceStub(grpc.insecure_channel(
            "cache:50051", options=[("grpc.max_receive_message_length", 8 * 1024 * 1024)]))
    ThreadingHTTPServer(("0.0.0.0", 8080), Handler).serve_forever()


if __name__ == "__main__":
    main()

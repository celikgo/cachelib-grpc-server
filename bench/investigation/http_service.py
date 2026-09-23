"""Disposable origin and HTTP->gRPC cache adapter for media-object experiments."""
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


def payload(size):
    block = bytes(range(256))
    return (block * (size // 256 + 1))[:size]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    count = 0
    lock = threading.Lock()
    cache = None
    origin = "origin"
    mode = "origin"

    def do_GET(self):
        if self.path == "/stats":
            with self.lock:
                body = json.dumps({"origin_requests": self.count}).encode()
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        match = re.fullmatch(r"/obj/(\d+)/(\d+)", self.path)
        if not match:
            self.send_error(404)
            return
        size = int(match.group(1))
        if size > 1048576:
            self.send_error(413)
            return
        if self.mode == "origin":
            with self.lock:
                type(self).count += 1
            time.sleep(0.005)  # explicitly simulated 5 ms origin service time
            body = payload(size)
            status = "ORIGIN"
        else:
            key = "media:" + self.path
            r = self.cache.Get(pb.GetRequest(key=key), timeout=10)
            if r.found:
                body, status = r.value, "HIT"
            else:
                conn = http.client.HTTPConnection(self.origin, 8080, timeout=10)
                conn.request("GET", self.path)
                response = conn.getresponse()
                body = response.read()
                if response.status != 200:
                    self.send_error(502)
                    return
                conn.close()
                stored = self.cache.Set(pb.SetRequest(key=key, value=body), timeout=10)
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
    p.add_argument("--origin", default="origin")
    p.add_argument("--cache", default="cache")
    args = p.parse_args()
    Handler.mode = args.mode
    Handler.origin = args.origin
    if args.mode == "adapter":
        Handler.cache = rpc.CacheServiceStub(grpc.insecure_channel(args.cache + ":50051",
                                                       options=[("grpc.max_receive_message_length", 8 * 1024 * 1024)]))
    ThreadingHTTPServer(("0.0.0.0", 8080), Handler).serve_forever()


if __name__ == "__main__":
    main()

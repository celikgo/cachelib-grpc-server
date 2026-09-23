"""Prove a gRPC service writes, reads, and expires actual Navy-resident data.

This proves logical flash-tier behavior. A Docker file can still be page-cached,
so it does not measure physical SSD latency, traffic, or endurance.
"""
import argparse
import json
import time

import grpc
import cache_pb2 as pb
import cache_pb2_grpc as rpc


def value(index, size):
    block = bytes((offset + index * 17) & 255 for offset in range(256))
    body = bytearray((block * (size // 256 + 1))[:size])
    body[:8] = index.to_bytes(8, "little")
    return bytes(body)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=50051)
    parser.add_argument("--objects", type=int, default=4096)
    parser.add_argument("--value-bytes", type=int, default=65536)
    args = parser.parse_args()
    assert args.objects > 1024 and 8 <= args.value_bytes <= 1048576
    channel = grpc.insecure_channel(f"{args.host}:{args.port}", options=[
        ("grpc.max_receive_message_length", 8 * 1024 * 1024),
        ("grpc.max_send_message_length", 8 * 1024 * 1024),
    ])
    client = rpc.CacheServiceStub(channel)
    initial = client.Stats(pb.StatsRequest(), timeout=10)
    assert initial.nvm_enabled, "NVM config flag absent"
    probe = "release:flash:00000000"
    probe_value = value(0, args.value_bytes)
    assert client.Set(pb.SetRequest(key=probe, value=probe_value, ttl_seconds=60), timeout=10).success
    for i in range(1, args.objects + 1):
        key = f"release:flash:{i:08d}"
        assert client.Set(pb.SetRequest(key=key, value=value(i, args.value_bytes)), timeout=10).success
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        state = client.Stats(pb.StatsRequest(), timeout=10)
        if state.eviction_count > initial.eviction_count and state.nvm_used > initial.nvm_used:
            break
        time.sleep(.1)
    else:
        raise AssertionError("no DRAM eviction plus Navy write traffic")
    hits_before = state.nvm_hit_count
    found_flash = 0
    for i in range(1, min(args.objects, 1024) + 1):
        key = f"release:flash:{i:08d}"
        got = client.Get(pb.GetRequest(key=key), timeout=10)
        if got.found:
            assert got.value == value(i, args.value_bytes), f"corrupt binary value {i}"
        updated = client.Stats(pb.StatsRequest(), timeout=10)
        if updated.nvm_hit_count > hits_before:
            found_flash += updated.nvm_hit_count - hits_before
            hits_before = updated.nvm_hit_count
            if found_flash >= 4:
                break
    assert found_flash >= 4, "no verified Navy hits after DRAM eviction"
    ttl_hits_before = client.Stats(pb.StatsRequest(), timeout=10).nvm_hit_count
    ttl_result = client.Get(pb.GetRequest(key=probe), timeout=10)
    assert ttl_result.found and ttl_result.value == probe_value, "TTL probe lost or corrupt"
    assert 0 < ttl_result.ttl_remaining <= 60, "TTL did not survive flash round trip"
    assert client.Stats(pb.StatsRequest(), timeout=10).nvm_hit_count > ttl_hits_before, (
        "TTL probe did not come from Navy")
    expiry_deadline = time.monotonic() + 65
    while time.monotonic() < expiry_deadline:
        if not client.Get(pb.GetRequest(key=probe), timeout=10).found:
            break
        time.sleep(.2)
    else:
        raise AssertionError("TTL probe failed to expire")
    final = client.Stats(pb.StatsRequest(), timeout=10)
    assert final.nvm_device_bytes_written > initial.nvm_device_bytes_written
    assert final.nvm_device_bytes_read > initial.nvm_device_bytes_read, (
        "Navy did not issue a device read for flash-resident objects")
    assert final.nvm_device_bytes_written == final.nvm_used, (
        "legacy nvm_used disagrees with device bytes written")
    print(json.dumps({"status": "passed", "objects_written": args.objects,
                      "value_bytes": args.value_bytes,
                      "dram_evictions_delta": final.eviction_count - initial.eviction_count,
                      "navy_written_bytes_delta": final.nvm_used - initial.nvm_used,
                      "navy_read_bytes_delta": final.nvm_device_bytes_read - initial.nvm_device_bytes_read,
                      "navy_hits_delta": final.nvm_hit_count - initial.nvm_hit_count,
                      "verified_flash_hits": found_flash,
                      "ttl_expired": True,
                      "physical_ssd_io_proven": False}, sort_keys=True))


if __name__ == "__main__":
    main()

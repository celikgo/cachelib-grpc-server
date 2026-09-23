// A common-interface, full-body-verifying HTTP media-object load generator.
package main

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"net/http"
	"os"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

type phaseResult struct {
	Requests int64            `json:"requests"`
	Errors   int64            `json:"errors"`
	Statuses map[string]int64 `json:"cache_status"`
	Latency  []float64        `json:"-"`
	Elapsed  float64          `json:"elapsed_s"`
}

func stats(origin string) int64 {
	client := &http.Client{Timeout: 5 * time.Second}
	r, e := client.Get("http://" + origin + ":8080/stats")
	if e != nil {
		panic(e)
	}
	defer r.Body.Close()
	var v struct {
		OriginRequests int64 `json:"origin_requests"`
	}
	if e = json.NewDecoder(r.Body).Decode(&v); e != nil {
		panic(e)
	}
	return v.OriginRequests
}

func cpuUS() int64 {
	raw, e := os.ReadFile("/sys/fs/cgroup/cpu.stat")
	if e != nil {
		return 0
	}
	for _, line := range strings.Split(string(raw), "\n") {
		fields := strings.Fields(line)
		if len(fields) == 2 && fields[0] == "usage_usec" {
			n, _ := strconv.ParseInt(fields[1], 10, 64)
			return n
		}
	}
	return 0
}

func memPeak() int64 {
	raw, _ := os.ReadFile("/sys/fs/cgroup/memory.peak")
	n, _ := strconv.ParseInt(strings.TrimSpace(string(raw)), 10, 64)
	return n
}

func validBody(body, canonical []byte, key int) bool {
	if len(body) != len(canonical) || len(body) < 8 {
		return false
	}
	return binary.LittleEndian.Uint64(body[:8]) == uint64(key) &&
		bytes.Equal(body[8:], canonical[8:])
}

func percentile(xs []float64, q float64) float64 {
	if len(xs) == 0 {
		return 0
	}
	at := float64(len(xs)-1) * q
	lo := int(at)
	hi := lo + 1
	if hi >= len(xs) {
		hi = lo
	}
	return xs[lo] + (xs[hi]-xs[lo])*(at-float64(lo))
}

func runPhase(target string, size, objects, concurrency int, pattern string, seconds int, expected []byte, offset *atomic.Uint64) phaseResult {
	start := time.Now()
	stop := start.Add(time.Duration(seconds) * time.Second)
	results := make([]phaseResult, concurrency)
	var wg sync.WaitGroup
	for worker := 0; worker < concurrency; worker++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()
			transport := &http.Transport{MaxIdleConns: 1, MaxIdleConnsPerHost: 1, MaxConnsPerHost: 1,
				IdleConnTimeout: 30 * time.Second}
			defer transport.CloseIdleConnections()
			client := &http.Client{Transport: transport, Timeout: 15 * time.Second}
			result := &results[id]
			result.Statuses = map[string]int64{}
			for time.Now().Before(stop) {
				n := offset.Add(1) - 1
				key := int(n % uint64(objects))
				if pattern == "one_pass" {
					key = objects + int(n)
				} else if pattern == "recent_replay" {
					// Readers switch to newer segments, then seek back to the
					// first half. Both sets are requested by many readers.
					width := max(1, objects/2)
					stage := min(2, int(time.Since(start)/(time.Duration(seconds)*time.Second/3)))
					base := 0
					if stage == 1 {
						base = width
					}
					key = base + int(n%uint64(width))
				}
				t0 := time.Now()
				resp, e := client.Get(fmt.Sprintf("http://%s:8080/obj/%d/%d", target, size, key))
				if e != nil {
					result.Errors++
					continue
				}
				body, e := io.ReadAll(resp.Body)
				resp.Body.Close()
				result.Latency = append(result.Latency, float64(time.Since(t0).Microseconds())/1000)
				if e != nil || resp.StatusCode != 200 || !validBody(body, expected, key) {
					result.Errors++
				} else {
					result.Requests++
				}
				result.Statuses[resp.Header.Get("X-Cache")]++
			}
		}(worker)
	}
	wg.Wait()
	out := phaseResult{Elapsed: time.Since(start).Seconds(), Statuses: map[string]int64{}}
	for _, r := range results {
		out.Requests += r.Requests
		out.Errors += r.Errors
		out.Latency = append(out.Latency, r.Latency...)
		for k, v := range r.Statuses {
			out.Statuses[k] += v
		}
	}
	sort.Float64s(out.Latency)
	return out
}

func main() {
	target := flag.String("target", "target", "HTTP target host")
	origin := flag.String("origin", "origin", "origin host")
	size := flag.Int("size", 262144, "binary object bytes")
	objects := flag.Int("objects", 64, "reusable objects")
	concurrency := flag.Int("concurrency", 8, "independent HTTP connections")
	pattern := flag.String("pattern", "repeated", "repeated, recent_replay, or one_pass")
	warmup := flag.Int("warmup-seconds", 15, "warmup seconds")
	measure := flag.Int("measure-seconds", 60, "measurement seconds")
	flag.Parse()
	if *size < 1 || *size > 1048576 || *objects < 1 || *concurrency < 1 || *measure < 1 ||
		(*pattern != "repeated" && *pattern != "recent_replay" && *pattern != "one_pass") {
		panic("invalid benchmark parameters")
	}
	expected := make([]byte, *size)
	for i := range expected {
		expected[i] = byte(i % 256)
	}
	if *pattern == "repeated" {
		client := &http.Client{Timeout: 15 * time.Second}
		for i := 0; i < *objects; i++ {
			r, e := client.Get(fmt.Sprintf("http://%s:8080/obj/%d/%d", *target, *size, i))
			if e != nil {
				panic(e)
			}
			body, e := io.ReadAll(r.Body)
			r.Body.Close()
			if e != nil || r.StatusCode != 200 || !validBody(body, expected, i) {
				panic("preload body mismatch")
			}
		}
	}
	var offset atomic.Uint64
	warmPattern, warmObjects := *pattern, *objects
	if *pattern == "recent_replay" {
		warmPattern, warmObjects = "repeated", max(1, *objects/2)
	}
	warm := runPhase(*target, *size, warmObjects, *concurrency, warmPattern, *warmup, expected, &offset)
	beforeOrigin, beforeCPU := stats(*origin), cpuUS()
	fmt.Println("MEASURE_START", time.Now().UTC().Format(time.RFC3339Nano))
	measured := runPhase(*target, *size, *objects, *concurrency, *pattern, *measure, expected, &offset)
	afterOrigin, afterCPU := stats(*origin), cpuUS()
	result := map[string]any{
		"size_bytes": *size, "objects": *objects, "concurrency": *concurrency, "pattern": *pattern,
		"warmup_seconds": *warmup, "warmup_success": warm.Requests, "warmup_errors": warm.Errors,
		"measure_seconds": *measure, "elapsed_s": measured.Elapsed,
		"successful_objects": measured.Requests, "errors": measured.Errors,
		"objects_s":     float64(measured.Requests) / measured.Elapsed,
		"payload_mib_s": float64(measured.Requests*int64(*size)) / 1048576 / measured.Elapsed,
		"p50_ms":        percentile(measured.Latency, .5), "p95_ms": percentile(measured.Latency, .95),
		"p99_ms":          percentile(measured.Latency, .99),
		"latency_samples": len(measured.Latency), "cache_status": measured.Statuses,
		"origin_requests_delta": afterOrigin - beforeOrigin,
		"origin_bytes_delta":    (afterOrigin - beforeOrigin) * int64(*size),
		"client_cpu_usage_usec": afterCPU - beforeCPU, "client_memory_peak_bytes": memPeak(),
	}
	if len(measured.Latency) >= 10000 {
		result["p999_ms"] = percentile(measured.Latency, .999)
	}
	encoded, _ := json.Marshal(result)
	fmt.Println(string(encoded))
	if measured.Errors > 0 {
		os.Exit(1)
	}
}

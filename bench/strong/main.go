package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"math"
	"os"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"cachebench/pb"
	"github.com/bradfitz/gomemcache/memcache"
	"github.com/redis/go-redis/v9"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
)

type options struct {
	backend, address, runID, pattern, workload, operation, model               string
	valueBytes, objects, concurrency, batchSize, warmupSeconds, measureSeconds int
	originMS, offeredRate                                                      int
	preload                                                                    bool
}

type backend struct {
	kind           string
	family         string
	grpcConn       *grpc.ClientConn
	grpcClient     pb.CacheServiceClient
	stream         pb.CacheService_PipelineClient
	streamSeq      uint64
	streamCreation time.Duration
	redis          *redis.Client
	mem            *memcache.Client
}

// family maps an engine identifier to the wire protocol used to drive it. The
// RESP engines share one client path; kind keeps the measured engine identity
// in the recorded result.
func family(kind string) string {
	switch {
	case strings.HasPrefix(kind, "grpc"):
		return "grpc"
	case strings.HasPrefix(kind, "memcached"):
		return "memcached"
	case kind == "redis" || kind == "valkey" || strings.HasPrefix(kind, "dragonfly") ||
		strings.HasPrefix(kind, "garnet") || strings.HasPrefix(kind, "kvrocks"):
		return "resp"
	}
	return ""
}

func newBackend(o options) (*backend, error) {
	b := &backend{kind: o.backend, family: family(o.backend)}
	switch b.family {
	case "grpc":
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		conn, err := grpc.DialContext(ctx, o.address, grpc.WithTransportCredentials(insecure.NewCredentials()),
			grpc.WithDefaultCallOptions(grpc.MaxCallRecvMsgSize(8<<20), grpc.MaxCallSendMsgSize(8<<20)))
		if err != nil {
			return nil, err
		}
		b.grpcConn = conn
		b.grpcClient = pb.NewCacheServiceClient(conn)
		if o.operation == "pipeline" {
			start := time.Now()
			stream, err := b.grpcClient.Pipeline(context.Background())
			if err != nil {
				conn.Close()
				return nil, err
			}
			b.stream = stream
			b.streamCreation = time.Since(start)
		}
	case "resp":
		b.redis = redis.NewClient(&redis.Options{Addr: o.address, PoolSize: 1, MinIdleConns: 1,
			DialTimeout: 10 * time.Second, ReadTimeout: 10 * time.Second, WriteTimeout: 10 * time.Second})
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		if err := b.redis.Ping(ctx).Err(); err != nil {
			return nil, err
		}
	case "memcached":
		b.mem = memcache.New(o.address)
		b.mem.MaxIdleConns = 1
		b.mem.Timeout = 10 * time.Second
	default:
		return nil, fmt.Errorf("unknown backend %q", o.backend)
	}
	return b, nil
}

func (b *backend) close() {
	if b.stream != nil {
		_ = b.stream.CloseSend()
	}
	if b.grpcConn != nil {
		_ = b.grpcConn.Close()
	}
	if b.redis != nil {
		_ = b.redis.Close()
	}
}

func (b *backend) get(key string) ([]byte, bool, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	switch b.family {
	case "grpc":
		r, e := b.grpcClient.Get(ctx, &pb.GetRequest{Key: key})
		if e != nil {
			return nil, false, e
		}
		return r.Value, r.Found, nil
	case "resp":
		v, e := b.redis.Get(ctx, key).Bytes()
		if e == redis.Nil {
			return nil, false, nil
		}
		return v, e == nil, e
	default:
		r, e := b.mem.Get(key)
		if e == memcache.ErrCacheMiss {
			return nil, false, nil
		}
		if e != nil {
			return nil, false, e
		}
		return r.Value, true, nil
	}
}

func (b *backend) set(key string, value []byte, ttl time.Duration) error {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	switch b.family {
	case "grpc":
		r, e := b.grpcClient.Set(ctx, &pb.SetRequest{Key: key, Value: value, TtlSeconds: int64(ttl.Seconds())})
		if e != nil {
			return e
		}
		if !r.Success {
			return errors.New("gRPC Set success=false")
		}
		return nil
	case "resp":
		return b.redis.Set(ctx, key, value, ttl).Err()
	default:
		return b.mem.Set(&memcache.Item{Key: key, Value: value, Expiration: int32(ttl.Seconds())})
	}
}

func (b *backend) batchGet(keys []string) ([][]byte, []bool, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	values := make([][]byte, len(keys))
	found := make([]bool, len(keys))
	switch b.family {
	case "grpc":
		r, e := b.grpcClient.MultiGet(ctx, &pb.MultiGetRequest{Keys: keys})
		if e != nil {
			return nil, nil, e
		}
		if len(r.Results) != len(keys) {
			return nil, nil, errors.New("MultiGet result count")
		}
		for i, x := range r.Results {
			if x.Key != keys[i] {
				return nil, nil, errors.New("MultiGet order")
			}
			values[i] = x.Value
			found[i] = x.Found
		}
	case "resp":
		cmds := make([]*redis.StringCmd, len(keys))
		_, e := b.redis.Pipelined(ctx, func(p redis.Pipeliner) error {
			for i, k := range keys {
				cmds[i] = p.Get(ctx, k)
			}
			return nil
		})
		if e != nil && e != redis.Nil {
			return nil, nil, e
		}
		for i, c := range cmds {
			v, e := c.Bytes()
			if e == redis.Nil {
				continue
			}
			if e != nil {
				return nil, nil, e
			}
			values[i] = v
			found[i] = true
		}
	default:
		r, e := b.mem.GetMulti(keys)
		if e != nil {
			return nil, nil, e
		}
		for i, k := range keys {
			if x, ok := r[k]; ok {
				values[i] = x.Value
				found[i] = true
			}
		}
	}
	return values, found, nil
}

func (b *backend) batchSet(keys []string, value []byte) error {
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	switch b.family {
	case "grpc":
		items := make([]*pb.SetRequest, len(keys))
		for i, k := range keys {
			items[i] = &pb.SetRequest{Key: k, Value: value}
		}
		r, e := b.grpcClient.MultiSet(ctx, &pb.MultiSetRequest{Items: items})
		if e != nil {
			return e
		}
		if int(r.SucceededCount) != len(keys) || r.FailedCount != 0 {
			return errors.New("MultiSet incomplete")
		}
		return nil
	case "resp":
		_, e := b.redis.Pipelined(ctx, func(p redis.Pipeliner) error {
			for _, k := range keys {
				p.Set(ctx, k, value, 0)
			}
			return nil
		})
		return e
	default:
		return errors.New("Memcached has no comparable native batch SET in this client")
	}
}

func (b *backend) pipelineGet(keys []string) ([][]byte, []bool, error) {
	if b.stream == nil {
		return nil, nil, errors.New("pipeline stream not initialized")
	}
	seq := make([]uint64, len(keys))
	for i, k := range keys {
		b.streamSeq++
		seq[i] = b.streamSeq
		if e := b.stream.Send(&pb.PipelineRequest{SequenceId: seq[i], Operation: &pb.PipelineRequest_Get{Get: &pb.GetRequest{Key: k}}}); e != nil {
			return nil, nil, e
		}
	}
	values := make([][]byte, len(keys))
	found := make([]bool, len(keys))
	for i := range keys {
		r, e := b.stream.Recv()
		if e != nil {
			return nil, nil, e
		}
		if r.SequenceId != seq[i] || r.Error != "" || r.GetGet() == nil {
			return nil, nil, errors.New("pipeline response correlation")
		}
		values[i] = r.GetGet().Value
		found[i] = r.GetGet().Found
	}
	return values, found, nil
}

type sample struct {
	latencyNS                            int64
	objects, gets, hits, origins, errors int
	bytes                                int64
}
type phase struct {
	latencies                                        []int64
	elapsed                                          time.Duration
	operations, batches, gets, hits, origins, errors int64
	payloadBytes                                     int64
	warmWindows                                      []int64
	dropped                                          int64
	schedulerLateNS                                  int64
}

// Keep one-pass keys unique across every warmup window and the measured phase.
var onePassCounter atomic.Uint64

func splitmix64(x uint64) uint64 {
	x += 0x9e3779b97f4a7c15
	x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9
	x = (x ^ (x >> 27)) * 0x94d049bb133111eb
	return x ^ (x >> 31)
}
func chooseIndex(o options, worker int, op uint64, elapsed time.Duration) int {
	if o.pattern == "one_pass" {
		return o.objects + worker*100000000 + int(op)
	}
	h := splitmix64(uint64(worker)*0x9e3779b97f4a7c15 + op + 20260923)
	if o.pattern == "skew" {
		f := float64(h>>11) / float64(uint64(1)<<53)
		return int(f * f * f * float64(o.objects))
	}
	if o.pattern == "hot_shift" {
		width := max(1, o.objects/10)
		base := 0
		if elapsed >= time.Duration(o.measureSeconds)*time.Second/2 {
			base = o.objects - width
		}
		return base + int(h%uint64(width))
	}
	return int(h % uint64(o.objects))
}
func keyFor(o options, index int) string { return "strong:" + o.runID + ":" + strconv.Itoa(index) }

func doOperation(b *backend, o options, value []byte, worker int, op uint64, elapsed time.Duration) sample {
	start := time.Now()
	if o.pattern == "one_pass" {
		op = onePassCounter.Add(1) - 1
	}
	n := 1
	if o.operation == "batch-get" || o.operation == "batch-set" || o.operation == "pipeline" {
		n = o.batchSize
	}
	keys := make([]string, n)
	for i := 0; i < n; i++ {
		keys[i] = keyFor(o, chooseIndex(o, worker, op*uint64(n)+uint64(i), elapsed))
	}
	result := sample{objects: n}
	if o.operation == "batch-set" {
		if e := b.batchSet(keys, value); e != nil {
			result.errors = n
		}
		result.latencyNS = time.Since(start).Nanoseconds()
		return result
	}
	if o.operation == "unary" && o.workload != "read-only" && ((o.workload == "read-heavy" && op%10 == 0) || (o.workload == "mixed" && op%2 == 0)) {
		if e := b.set(keys[0], value, 0); e != nil {
			result.errors = 1
		}
		result.latencyNS = time.Since(start).Nanoseconds()
		return result
	}
	result.gets = n
	var values [][]byte
	var found []bool
	var err error
	switch o.operation {
	case "unary":
		var v []byte
		var f bool
		v, f, err = b.get(keys[0])
		values = [][]byte{v}
		found = []bool{f}
	case "batch-get":
		values, found, err = b.batchGet(keys)
	case "pipeline":
		values, found, err = b.pipelineGet(keys)
	}
	if err != nil {
		result.errors = n
		result.latencyNS = time.Since(start).Nanoseconds()
		return result
	}
	for i := range keys {
		if found[i] {
			if !bytes.Equal(values[i], value) {
				result.errors++
			} else {
				result.hits++
				result.bytes += int64(len(value))
			}
		}
		if !found[i] {
			if o.workload == "origin" {
				time.Sleep(time.Duration(o.originMS) * time.Millisecond)
				result.origins++
				if e := b.set(keys[i], value, 0); e != nil {
					result.errors++
				}
			} else {
				result.errors++
			}
		}
	}
	result.latencyNS = time.Since(start).Nanoseconds()
	return result
}

func cgroupSnapshot() map[string]int64 {
	out := map[string]int64{}
	if data, e := os.ReadFile("/sys/fs/cgroup/cpu.stat"); e == nil {
		for _, line := range strings.Split(string(data), "\n") {
			p := strings.Fields(line)
			if len(p) == 2 {
				v, _ := strconv.ParseInt(p[1], 10, 64)
				out[p[0]] = v
			}
		}
	}
	for _, name := range []string{"memory.current", "memory.peak"} {
		if data, e := os.ReadFile("/sys/fs/cgroup/" + name); e == nil {
			v, _ := strconv.ParseInt(strings.TrimSpace(string(data)), 10, 64)
			out[name] = v
		}
	}
	return out
}

func runClosed(o options, bs []*backend, value []byte, duration time.Duration, record bool) phase {
	start := time.Now()
	end := start.Add(duration)
	out := phase{}
	parts := make([]phase, len(bs))
	var wg sync.WaitGroup
	for w, b := range bs {
		wg.Add(1)
		go func(w int, b *backend) {
			defer wg.Done()
			op := uint64(0)
			part := &parts[w]
			for time.Now().Before(end) {
				s := doOperation(b, o, value, w, op, time.Since(start))
				op++
				if record {
					part.latencies = append(part.latencies, s.latencyNS)
					part.operations += int64(s.objects)
					part.batches++
					part.gets += int64(s.gets)
					part.hits += int64(s.hits)
					part.origins += int64(s.origins)
					part.errors += int64(s.errors)
					part.payloadBytes += s.bytes
				} else {
					atomic.AddInt64(&out.operations, int64(s.objects))
				}
			}
		}(w, b)
	}
	wg.Wait()
	out.elapsed = time.Since(start)
	if record {
		for _, p := range parts {
			out.latencies = append(out.latencies, p.latencies...)
			out.operations += p.operations
			out.batches += p.batches
			out.gets += p.gets
			out.hits += p.hits
			out.origins += p.origins
			out.errors += p.errors
			out.payloadBytes += p.payloadBytes
		}
	}
	return out
}

type job struct {
	index uint64
	due   time.Time
}

func runOffered(o options, bs []*backend, value []byte, duration time.Duration, record bool) phase {
	start := time.Now()
	end := start.Add(duration)
	out := phase{}
	jobs := make(chan job, max(1024, o.offeredRate/2))
	parts := make([]phase, len(bs))
	var wg sync.WaitGroup
	for w, b := range bs {
		wg.Add(1)
		go func(w int, b *backend) {
			defer wg.Done()
			part := &parts[w]
			for j := range jobs {
				// Offered-load jobs use one global trace independent of which
				// connection happened to receive an arrival.
				s := doOperation(b, o, value, 0, j.index, time.Since(start))
				s.latencyNS = time.Since(j.due).Nanoseconds()
				if record {
					part.latencies = append(part.latencies, s.latencyNS)
					part.operations += int64(s.objects)
					part.batches++
					part.gets += int64(s.gets)
					part.hits += int64(s.hits)
					part.origins += int64(s.origins)
					part.errors += int64(s.errors)
					part.payloadBytes += s.bytes
				} else {
					atomic.AddInt64(&out.operations, int64(s.objects))
				}
			}
		}(w, b)
	}
	interval := time.Second / time.Duration(o.offeredRate)
	for i := uint64(0); ; i++ {
		due := start.Add(time.Duration(i) * interval)
		if !due.Before(end) {
			break
		}
		if wait := time.Until(due); wait > 0 {
			time.Sleep(wait)
		}
		lag := time.Since(due)
		if lag > 0 && record {
			out.schedulerLateNS += lag.Nanoseconds()
		}
		select {
		case jobs <- job{i, due}:
		default:
			out.dropped++
		}
	}
	close(jobs)
	wg.Wait()
	out.elapsed = time.Since(start)
	if record {
		for _, p := range parts {
			out.latencies = append(out.latencies, p.latencies...)
			out.operations += p.operations
			out.batches += p.batches
			out.gets += p.gets
			out.hits += p.hits
			out.origins += p.origins
			out.errors += p.errors
			out.payloadBytes += p.payloadBytes
		}
	}
	return out
}

func percentile(sorted []int64, q float64) float64 {
	if len(sorted) == 0 {
		return 0
	}
	at := q * float64(len(sorted)-1)
	lo := int(at)
	hi := min(lo+1, len(sorted)-1)
	return (float64(sorted[lo]) + (float64(sorted[hi])-float64(sorted[lo]))*(at-float64(lo))) / 1e6
}
func summarize(o options, p phase, warm []int64, stable bool, cpuBefore, cpuAfter map[string]int64, creation []float64) map[string]any {
	lat := p.latencies
	sort.Slice(lat, func(i, j int) bool { return lat[i] < lat[j] })
	quant := map[string]float64{"p50": percentile(lat, .5), "p95": percentile(lat, .95), "p99": percentile(lat, .99)}
	if len(lat) >= 10000 {
		quant["p999"] = percentile(lat, .999)
	}
	args := map[string]any{"backend": o.backend, "address": o.address, "run_id": o.runID, "pattern": o.pattern, "workload": o.workload, "operation": o.operation, "model": o.model, "value_bytes": o.valueBytes, "objects": o.objects, "concurrency": o.concurrency, "batch_size": o.batchSize, "warmup_seconds": o.warmupSeconds, "measure_seconds": o.measureSeconds, "origin_ms": o.originMS, "offered_rate": o.offeredRate, "preload": o.preload}
	return map[string]any{"schema": 1, "args": args, "elapsed_s": p.elapsed.Seconds(), "operations": p.operations, "batches": p.batches, "get_attempts": p.gets, "successful_ops_s": float64(p.operations-p.errors) / p.elapsed.Seconds(), "payload_mib_s": float64(p.payloadBytes) / 1048576 / p.elapsed.Seconds(), "latency_ms": quant, "latency_samples": len(lat), "errors": p.errors, "hits": p.hits, "origins": p.origins, "origin_bytes": p.origins * int64(o.valueBytes), "origin_bytes_avoided": p.hits * int64(o.valueBytes), "dropped_arrivals": p.dropped, "scheduler_lag_mean_ms": float64(p.schedulerLateNS) / math.Max(1, float64(p.batches)) / 1e6, "warmup_windows_ops": warm, "warmup_stable": stable, "client_cgroup_before": cpuBefore, "client_cgroup_after": cpuAfter, "pipeline_stream_creation_ms": creation, "definitions": map[string]string{"operation": "one key-value operation; batches count separately", "latency": "closed-loop request start to response, or offered scheduled arrival to response including queue", "payload": "verified cache-hit response bytes", "origin": "simulated fixed delay plus cache fill on miss"}}
}

func main() {
	o := options{}
	flag.StringVar(&o.backend, "backend", "grpc", "grpc|grpc_nvm|redis|valkey|dragonfly|dragonfly_tiered|garnet|garnet_storage|kvrocks|memcached|memcached_extstore")
	flag.StringVar(&o.address, "address", "", "host:port")
	flag.StringVar(&o.runID, "run-id", "run", "key namespace")
	flag.StringVar(&o.pattern, "pattern", "uniform", "uniform|skew|hot_shift|one_pass")
	flag.StringVar(&o.workload, "workload", "read-only", "read-only|read-heavy|mixed|origin")
	flag.StringVar(&o.operation, "operation", "unary", "unary|batch-get|batch-set|pipeline")
	flag.StringVar(&o.model, "model", "closed", "closed|offered")
	flag.IntVar(&o.valueBytes, "value-bytes", 1024, "value bytes")
	flag.IntVar(&o.objects, "objects", 1000, "logical objects")
	flag.IntVar(&o.concurrency, "concurrency", 1, "connections/workers")
	flag.IntVar(&o.batchSize, "batch-size", 16, "objects per batch")
	flag.IntVar(&o.warmupSeconds, "warmup-seconds", 15, "minimum warmup seconds")
	flag.IntVar(&o.measureSeconds, "measure-seconds", 60, "measurement seconds")
	flag.IntVar(&o.originMS, "origin-ms", 5, "simulated origin delay per miss")
	flag.IntVar(&o.offeredRate, "offered-rate", 1000, "arrival batches/s in offered mode")
	flag.BoolVar(&o.preload, "preload", true, "preload full logical set")
	flag.Parse()
	if o.address == "" || o.valueBytes <= 0 || o.objects <= 0 || o.concurrency <= 0 || o.batchSize <= 0 || o.measureSeconds <= 0 {
		fmt.Fprintln(os.Stderr, "invalid configuration")
		os.Exit(2)
	}
	if o.operation == "pipeline" && o.backend != "grpc" {
		fmt.Fprintln(os.Stderr, "pipeline is gRPC-only")
		os.Exit(2)
	}
	if o.operation == "batch-set" && strings.HasPrefix(o.backend, "memcached") {
		fmt.Fprintln(os.Stderr, "Memcached batch SET unsupported")
		os.Exit(2)
	}
	if o.model == "offered" && o.offeredRate <= 0 {
		fmt.Fprintln(os.Stderr, "offered-rate must be positive")
		os.Exit(2)
	}
	value := make([]byte, o.valueBytes)
	for i := range value {
		value[i] = byte(i % 256)
	}
	bs := make([]*backend, o.concurrency)
	for i := range bs {
		b, e := newBackend(o)
		if e != nil {
			fmt.Fprintln(os.Stderr, e)
			os.Exit(1)
		}
		bs[i] = b
	}
	defer func() {
		for _, b := range bs {
			b.close()
		}
	}()
	if o.preload {
		for i := 0; i < o.objects; i++ {
			if e := bs[0].set(keyFor(o, i), value, 0); e != nil {
				fmt.Fprintln(os.Stderr, "preload:", e)
				os.Exit(1)
			}
		}
	}
	runner := runClosed
	if o.model == "offered" {
		runner = runOffered
	}
	windows := []int64{}
	stable := false
	if o.warmupSeconds > 0 {
		minWindows := max(3, (o.warmupSeconds+4)/5)
		for i := 0; i < max(minWindows, 12); i++ {
			p := runner(o, bs, value, 5*time.Second, false)
			windows = append(windows, p.operations)
			if len(windows) >= minWindows && len(windows) >= 3 {
				a, b, c := float64(windows[len(windows)-3]), float64(windows[len(windows)-2]), float64(windows[len(windows)-1])
				mean := (a + b + c) / 3
				if mean > 0 && math.Max(math.Max(math.Abs(a-mean), math.Abs(b-mean)), math.Abs(c-mean))/mean <= .10 {
					stable = true
					break
				}
			}
		}
	}
	before := cgroupSnapshot()
	fmt.Fprintln(os.Stderr, "MEASURE_START")
	p := runner(o, bs, value, time.Duration(o.measureSeconds)*time.Second, true)
	after := cgroupSnapshot()
	creation := []float64{}
	for _, b := range bs {
		if b.stream != nil {
			creation = append(creation, float64(b.streamCreation.Microseconds())/1000)
		}
	}
	out := summarize(o, p, windows, stable, before, after, creation)
	encoder := json.NewEncoder(os.Stdout)
	if e := encoder.Encode(out); e != nil {
		fmt.Fprintln(os.Stderr, e)
		os.Exit(1)
	}
	if p.errors > 0 || p.dropped > 0 {
		os.Exit(3)
	}
}

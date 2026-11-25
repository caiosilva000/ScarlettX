// The vector kernel of bench_victory_prewarm's `vector` workload, written directly in Metal
// Shading Language and run on the native Metal device: a host-backend comparison on the same
// numbers. It says nothing about RDNA2 translation; nothing here is translated.
//
//   element i:  x = i + c0;  x ^= x << 13;  x ^= x >> 17;  x ^= x << 5;  out[i] = (x + c1) ^ k
//
// 32 threads per group, N / 32 groups, as the translated program runs it. `k` is a literal in
// the source, so each k is a different shader. One launch measures the cold path (source to
// library, pipeline state, first dispatch) and then the warm dispatch, each dispatch committed
// and waited for, and prints a hash of the output for fixed constants.
//
//   metal_vector_kernel [--elements N] [--k K] [--iterations N] [--launch N] [--series NAME]
//                       [--csv FILE] [--samples FILE]
//
// Exit codes: 0 pass, 1 an output was not exact, 2 a Metal call failed, 3 no device, 64 usage.
import Foundation
import Metal

func kernel(_ i: UInt32, _ c0: UInt32, _ c1: UInt32, _ k: UInt32) -> UInt32 {
    var x = i &+ c0
    x ^= x << 13
    x ^= x >> 17
    x ^= x << 5
    return (x &+ c1) ^ k
}

func ms(_ a: UInt64, _ b: UInt64) -> Double { Double(b - a) / 1_000_000.0 }
func now() -> UInt64 { DispatchTime.now().uptimeNanoseconds }

func percentile(_ values: [Double], _ p: Double) -> Double {
    if values.isEmpty { return -1 }
    let v = values.sorted()
    let rank = p * Double(v.count - 1)
    let lo = Int(rank)
    let hi = min(lo + 1, v.count - 1)
    return v[lo] + (v[hi] - v[lo]) * (rank - Double(lo))
}

var elements = 1 << 20
var k: UInt32 = 0
var iterations = 200
var launch = 0
var series = "default"
var csvPath: String? = nil
var samplesPath: String? = nil
var it = CommandLine.arguments.dropFirst().makeIterator()
while let a = it.next() {
    switch a {
    case "--elements": elements = Int(it.next() ?? "") ?? 0
    case "--k": k = UInt32(it.next() ?? "") ?? 0
    case "--iterations": iterations = Int(it.next() ?? "") ?? 0
    case "--launch": launch = Int(it.next() ?? "") ?? 0
    case "--series": series = it.next() ?? series
    case "--csv": csvPath = it.next()
    case "--samples": samplesPath = it.next()
    default:
        FileHandle.standardError.write("metal_vector_kernel: unknown argument \(a)\n".data(using: .utf8)!)
        exit(64)
    }
}
if elements <= 0 || elements % 32 != 0 || iterations <= 0 { exit(64) }

let shaderPath = "native Metal: MSL source -> Metal library -> compute pipeline (no translation)"
var csvRows = ""
var sampleRows = ""
var shaderFnv: UInt64 = 0
func metric(_ name: String, _ value: Double) {
    print(String(format: "[metal] %@=%.4f", name, value))
    csvRows += String(format: "%@,%d,vector,\"%@\",cold,%u,%016llx,%@,%.6f\n", series, launch, shaderPath, k, shaderFnv,
                      name, value)
}

guard let device = MTLCreateSystemDefaultDevice() else {
    print("[metal] no Metal device")
    exit(3)
}
print("[metal] workload=vector series=\(series) launch=\(launch) k=\(k) elements=\(elements) iterations=\(iterations)")
print("[metal] shader_path=\(shaderPath)")
print("[metal] backend=Metal device=\"\(device.name)\" unified_memory=\(device.hasUnifiedMemory) low_power=\(device.isLowPower)")

let source = """
#include <metal_stdlib>
using namespace metal;
struct Params { uint c0; uint c1; };
kernel void vector_kernel(device uint* out [[buffer(0)]], constant Params& p [[buffer(1)]],
                          uint3 gid [[thread_position_in_grid]]) {
    uint i = gid.x;
    uint x = i + p.c0;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    out[i] = (x + p.c1) ^ \(k)u;
}
"""
var h: UInt64 = 14695981039346656037
for b in source.utf8 {
    h ^= UInt64(b)
    h = h &* 1099511628211
}
shaderFnv = h

guard let queue = device.makeCommandQueue(),
      let buffer = device.makeBuffer(length: elements * 4, options: .storageModeShared)
else {
    print("[metal] FAILED api=makeCommandQueue/makeBuffer")
    exit(2)
}
let out = buffer.contents().bindMemory(to: UInt32.self, capacity: elements)
for i in 0..<elements { out[i] = 0xA5A5_A5A5 }

// Cold: source to library, pipeline state, the first dispatch.
var t0 = now()
let library: MTLLibrary
do { library = try device.makeLibrary(source: source, options: nil) } catch {
    print("[metal] FAILED api=makeLibrary \(error)")
    exit(2)
}
let libraryMs = ms(t0, now())
t0 = now()
let pipeline: MTLComputePipelineState
do { pipeline = try device.makeComputePipelineState(function: library.makeFunction(name: "vector_kernel")!) } catch {
    print("[metal] FAILED api=makeComputePipelineState \(error)")
    exit(2)
}
let pipelineMs = ms(t0, now())
print("[metal] threads: execution width \(pipeline.threadExecutionWidth), max per group \(pipeline.maxTotalThreadsPerThreadgroup)")

// One dispatch, committed and waited for. Returns (wall ms, GPU ms).
func dispatch(_ c0: UInt32, _ c1: UInt32) -> (Double, Double)? {
    var params = (c0, c1)
    let start = now()
    guard let cb = queue.makeCommandBuffer(), let enc = cb.makeComputeCommandEncoder() else { return nil }
    enc.setComputePipelineState(pipeline)
    enc.setBuffer(buffer, offset: 0, index: 0)
    enc.setBytes(&params, length: 8, index: 1)
    enc.dispatchThreadgroups(MTLSize(width: elements / 32, height: 1, depth: 1),
                             threadsPerThreadgroup: MTLSize(width: 32, height: 1, depth: 1))
    enc.endEncoding()
    cb.commit()
    cb.waitUntilCompleted()
    if cb.status != .completed { return nil }
    return (ms(start, now()), (cb.gpuEndTime - cb.gpuStartTime) * 1000.0)
}

var exact = 0
var total = 0
func validate(_ when: String, _ c0: UInt32, _ c1: UInt32) -> Bool {
    var ok = 0
    for i in 0..<elements where out[i] == kernel(UInt32(i), c0, c1, k) { ok += 1 }
    print("[metal] validate \(when): dwords exact \(ok)/\(elements)")
    exact += ok
    total += elements
    return ok == elements
}

guard let first = dispatch(1, 2) else {
    print("[metal] FAILED api=first dispatch")
    exit(2)
}
metric("elements", Double(elements))
metric("translate_ms", libraryMs)  // the front end: MSL source to a Metal library
metric("pipeline_create_ms", pipelineMs)
metric("first_submission_ms", first.0)
metric("first_gpu_ms", first.1)
metric("cold_compile_ms", libraryMs + pipelineMs + first.0)
if !validate("first dispatch", 1, 2) { exit(1) }

// Warm: the pipeline is in place; nothing is validated inside the loop.
var warm: [Double] = []
var gpu: [Double] = []
var c0: UInt32 = 0
var c1: UInt32 = 0
for i in 0..<iterations {
    c0 = 0x1000 &+ UInt32(i) &* 7919
    c1 = 0xABCD_0000 ^ UInt32(i)
    guard let t = dispatch(c0, c1) else {
        print("[metal] FAILED api=warm dispatch")
        exit(2)
    }
    warm.append(t.0)
    gpu.append(t.1)
    sampleRows += String(format: "%@,%d,vector,cold,%u,%d,0.000000,%.6f,%.6f\n", series, launch, k, i, t.0, t.1)
}
if !validate("after the warm loop", c0, c1) { exit(1) }
metric("warm_iterations", Double(warm.count))
metric("warm_dispatch_median_ms", percentile(warm, 0.5))
metric("warm_dispatch_p95_ms", percentile(warm, 0.95))
metric("warm_dispatch_min_ms", percentile(warm, 0.0))
metric("warm_gpu_median_ms", percentile(gpu, 0.5))
metric("warm_gpu_p95_ms", percentile(gpu, 0.95))

// The output for fixed constants, hashed as the other backend hashes its own.
guard dispatch(0x1234_5678, 0x9E37_79B9) != nil else { exit(2) }
if !validate("canonical constants", 0x1234_5678, 0x9E37_79B9) { exit(1) }
var oh: UInt64 = 14695981039346656037
for i in 0..<elements {
    let v = out[i]
    for b in 0..<4 {
        oh ^= UInt64((v >> UInt32(8 * b)) & 0xFF)
        oh = oh &* 1099511628211
    }
}
print(String(format: "[metal] canonical_output_fnv=%016llx first=%08x last=%08x", oh, out[0], out[elements - 1]))
metric("validated_exact", Double(exact))
metric("validated_total", Double(total))

func append(_ path: String?, _ text: String) {
    guard let path = path, let data = text.data(using: .utf8) else { return }
    if let handle = FileHandle(forWritingAtPath: path) {
        handle.seekToEndOfFile()
        handle.write(data)
        handle.closeFile()
    } else {
        FileManager.default.createFile(atPath: path, contents: data)
    }
}
append(csvPath, csvRows)
append(samplesPath, sampleRows)
print("[metal] PASS")

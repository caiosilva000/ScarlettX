//! Narrow C ABI around the pinned GPL-3.0-or-later `victory-shader` crate.
//!
//! The C++ host owns all input and output buffers.  No game material is
//! written by this crate: callers decide whether and where to cache results.

use std::ffi::c_char;
use std::slice;

use victory_shader::{Container, FourCC, HwStage, native_programs};

const MESSAGE_CAPACITY: usize = 192;

/// Stable inspection result for C++ callers.  `container_status` is zero
/// when the DXBC envelope parsed; `native_status` is zero only when the
/// pinned parser located and decoded native GFX10 program records.
#[repr(C)]
pub struct SxVictoryShaderInfo {
    pub container_status: i32,
    pub native_status: i32,
    pub part_count: u32,
    pub has_dxil: u32,
    pub has_xdat: u32,
    pub has_xbop: u32,
    pub native_program_count: u32,
    pub stage_mask: u32,
    pub wave64_mask: u32,
    pub message: [c_char; MESSAGE_CAPACITY],
}

#[repr(C)]
pub struct SxVictoryByteBuffer {
    pub data: *mut u8,
    pub size: usize,
}

fn clear(info: &mut SxVictoryShaderInfo) {
    *info = SxVictoryShaderInfo {
        container_status: 1,
        native_status: 1,
        part_count: 0,
        has_dxil: 0,
        has_xdat: 0,
        has_xbop: 0,
        native_program_count: 0,
        stage_mask: 0,
        wave64_mask: 0,
        message: [0; MESSAGE_CAPACITY],
    };
}

fn message(info: &mut SxVictoryShaderInfo, text: impl std::fmt::Display) {
    let bytes = text.to_string();
    let bytes = bytes.as_bytes();
    let count = bytes.len().min(MESSAGE_CAPACITY - 1);
    for (dst, src) in info.message[..count].iter_mut().zip(&bytes[..count]) {
        *dst = *src as c_char;
    }
}

fn stage_bit(stage: HwStage) -> u32 {
    match stage {
        HwStage::Ls => 1 << 0,
        HwStage::Hs => 1 << 1,
        HwStage::Es => 1 << 2,
        HwStage::Gs => 1 << 3,
        HwStage::Vs => 1 << 4,
        HwStage::Ps => 1 << 5,
        HwStage::Ms => 1 << 6,
        HwStage::Cs => 1 << 7,
    }
}

fn inspect(data: &[u8], info: &mut SxVictoryShaderInfo) {
    clear(info);
    let container = match Container::parse(data) {
        Ok(container) => container,
        Err(err) => {
            message(info, err);
            return;
        }
    };
    info.container_status = 0;
    info.part_count = container.parts.len() as u32;
    info.has_dxil = u32::from(container.part(FourCC::DXIL).is_some());
    if let Some(dxil) = container.part(FourCC::DXIL) {
        if let Ok(program) = victory_shader::dxil::DxilProgram::parse(dxil.data) {
            let extras = program.xbox_extras();
            info.has_xdat = u32::from(extras.xdat);
            info.has_xbop = u32::from(extras.xbop);
        }
    }
    let Some(dxil) = container.part(FourCC::DXIL) else {
        message(info, "no DXIL part in this container");
        return;
    };
    match native_programs(dxil.data) {
        Ok(programs) => {
            info.native_program_count = programs.len() as u32;
            if programs.is_empty() {
                message(info, "no native GFX10 programs in this container");
                return;
            }
            info.native_status = 0;
            for program in programs {
                let bit = stage_bit(program.stage);
                info.stage_mask |= bit;
                if program.wave64 {
                    info.wave64_mask |= bit;
                }
            }
        }
        Err(err) => message(info, err),
    }
}

/// Inspect a DXBC/Xbox shader envelope.  Returns -1 only for invalid FFI
/// arguments; parse and native-program outcomes are in `out`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sx_victory_shader_inspect(
    data: *const u8,
    size: usize,
    out: *mut SxVictoryShaderInfo,
) -> i32 {
    if data.is_null() || out.is_null() {
        return -1;
    }
    // SAFETY: both pointer ranges are validated by the C++ caller's ABI.
    let input = unsafe { slice::from_raw_parts(data, size) };
    let info = unsafe { &mut *out };
    inspect(input, info);
    0
}

/// Translate a native console shader to a host DXIL container.  The caller
/// owns the returned buffer and must release it with `sx_victory_buffer_free`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sx_victory_shader_translate(
    data: *const u8,
    size: usize,
    out: *mut SxVictoryByteBuffer,
    info: *mut SxVictoryShaderInfo,
) -> i32 {
    if data.is_null() || out.is_null() || info.is_null() {
        return -1;
    }
    let input = unsafe { slice::from_raw_parts(data, size) };
    let output = unsafe { &mut *out };
    output.data = std::ptr::null_mut();
    output.size = 0;
    let report = unsafe { &mut *info };
    inspect(input, report);
    if report.container_status != 0 || report.native_status != 0 {
        return 1;
    }
    match victory_shader::emit::translate(input) {
        Ok(shader) => {
            hand_over(output, shader.dxil);
            0
        }
        Err(err) => {
            message(report, err);
            2
        }
    }
}

/// Gives `bytes` to the C++ caller. A boxed slice's allocation is exactly its
/// length, so `sx_victory_buffer_free` can rebuild it from `data` and `size`
/// alone (a `Vec` may hold more capacity than its length).
fn hand_over(out: &mut SxVictoryByteBuffer, bytes: Vec<u8>) {
    let boxed = bytes.into_boxed_slice();
    out.size = boxed.len();
    out.data = Box::into_raw(boxed).cast::<u8>();
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn sx_victory_buffer_free(buffer: *mut SxVictoryByteBuffer) {
    if buffer.is_null() {
        return;
    }
    let buffer = unsafe { &mut *buffer };
    if !buffer.data.is_null() {
        // SAFETY: `hand_over` leaked a boxed slice of exactly `size` bytes at `data`.
        unsafe { drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(buffer.data, buffer.size))) };
    }
    buffer.data = std::ptr::null_mut();
    buffer.size = 0;
}

// ---------------------------------------------------------------------------
// Synthetic compute fixture
//
// A hand-assembled GFX10 compute program, built here from instruction
// encoders. Nothing in it comes from a title: the encoders and the program's
// shape (four root constants in s0..s3, one raw UAV reached through a
// descriptor table in s4, a single store) follow victory-shader's own
// synthetic VALU tests (`src/emit/valu_tests.rs`, GPL-3.0-or-later).
// ---------------------------------------------------------------------------

use victory_shader::native::{ComputeIo, StageIo};
use victory_shader::{NativeProgram, ResourceType, RootParam, RootType, UsedResource};

const SYNTHETIC_USER_DATA_CAPACITY: usize = 16;

/// One user-data dword the translated shader reads and what fills it.
/// `root`: 0 root constants, 1 descriptor table, 2 root descriptor,
/// 3 immediate sampler, 4 resource heap, 5 sampler heap, 255 other.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SxVictoryUserData {
    pub index: u32,
    pub slot: u32,
    pub root: u32,
    pub dword: u32,
}

/// What a host must bind to run a translated compute shader. `flags`: bit 0
/// title memory, 1 memory writes, 2 view addresses, 3 computed heap indices,
/// 4 heap writes, 5 view marks (each needs host tables this struct does not
/// describe further).
#[repr(C)]
pub struct SxVictoryComputeBinding {
    pub user_data_dwords: u32,
    pub user_data_count: u32,
    pub user_data: [SxVictoryUserData; SYNTHETIC_USER_DATA_CAPACITY],
    pub root_cbv_count: u32,
    pub root_srv_count: u32,
    pub root_uav_count: u32,
    pub sampler_count: u32,
    pub static_sampler_count: u32,
    pub flags: u32,
    pub threads: [u32; 3],
}

const S_ENDPGM: u32 = 0xBF81_0000;

/// SOP1 `s_mov_b32 sdst, src0`.
const fn s_mov_b32(sdst: u8, src0: u8) -> u32 {
    0xBE80_0000 | (sdst as u32) << 16 | 0x03 << 8 | src0 as u32
}

/// SMEM `s_load_dwordx4 s[dst:dst+3], s[base:base+1], 0`.
const fn s_load_dwordx4(dst: u8, base: u8) -> [u32; 2] {
    [
        0x3D << 26 | 0x02 << 18 | (dst as u32) << 6 | (base as u32 >> 1),
        125 << 25,
    ]
}

/// VOP1 `op vdst, src0` (`src0`: 0..105 an SGPR, 256..511 a VGPR).
const fn vop1(op: u16, vdst: u8, src0: u16) -> u32 {
    0x7E00_0000 | (vdst as u32) << 17 | (op as u32) << 9 | (src0 as u32 & 0x1FF)
}

/// VOP2 `op vdst, src0, vsrc1`.
const fn vop2(op: u8, vdst: u8, src0: u16, vsrc1: u8) -> u32 {
    (op as u32) << 25 | (vdst as u32) << 17 | (vsrc1 as u32) << 9 | (src0 as u32 & 0x1FF)
}

/// MUBUF `buffer_store_dword vdata, off, s[srsrc:srsrc+3], 0 offset:byte_off`.
const fn buffer_store_dword(vdata: u8, srsrc: u8, byte_off: u16) -> [u32; 2] {
    [
        0x38 << 26 | 0x1C << 18 | (byte_off as u32 & 0xFFF),
        0x80 << 24 | ((srsrc as u32 >> 2) & 0x1F) << 16 | (vdata as u32) << 8,
    ]
}

const fn vgpr(n: u8) -> u16 {
    256 + n as u16
}

const V_MOV_B32: u16 = 0x001;
const V_XOR_B32: u8 = 0x1D;
const V_ADD_NC_U32: u8 = 0x25;

/// One thread: `out[0] = ((c0 + c1) ^ c2) + c3` (32-bit, wrapping), the four
/// root constants in s0..s3, `out` the raw UAV at heap index 0.
fn synthetic_compute_program() -> NativeProgram {
    let mut words = Vec::new();
    // s[4:5] = the descriptor table's address (the high half is zero).
    words.push(s_mov_b32(5, 0x80));
    words.extend_from_slice(&s_load_dwordx4(8, 4));
    for i in 0..4u8 {
        words.push(vop1(V_MOV_B32, i + 1, u16::from(i))); // v<i+1> = s<i>
    }
    words.push(vop2(V_ADD_NC_U32, 0, vgpr(1), 2)); // v0 = v1 + v2
    words.push(vop2(V_XOR_B32, 0, vgpr(0), 3)); // v0 = v0 ^ v3
    words.push(vop2(V_ADD_NC_U32, 0, vgpr(0), 4)); // v0 = v0 + v4
    words.extend_from_slice(&buffer_store_dword(0, 8, 0));
    words.push(S_ENDPGM);

    NativeProgram {
        stage: HwStage::Cs,
        wave64: false,
        vgprs: 16,
        layout: vec![
            RootParam {
                root: RootType::Constants,
                table_offset_dwords: 0,
                sgprs: 4,
                base_register: 0,
                user_data: 0,
                slot: 0,
            },
            RootParam {
                root: RootType::Table,
                table_offset_dwords: 0,
                sgprs: 1,
                base_register: 0,
                user_data: 4,
                slot: 1,
            },
        ],
        used: vec![UsedResource {
            kind: ResourceType::Uav,
            root: RootType::Table,
            table_offset_dwords: 0,
            lower_bound: 0,
            stride: 0,
            dwords: 4,
            user_data: 4,
            slot: 1,
        }],
        io: StageIo::Cs(ComputeIo {
            rsrc2: 5 << 1, // five user SGPRs: s0..s4
            threads: [1, 1, 1],
        }),
        code: words.iter().flat_map(|w| w.to_le_bytes()).collect(),
    }
}

/// A DXBC envelope with no parts: a compute program has no signatures to carry.
fn empty_envelope() -> Vec<u8> {
    let mut out = b"DXBC".to_vec();
    out.extend_from_slice(&[0; 16]);
    out.extend_from_slice(&1u16.to_le_bytes());
    out.extend_from_slice(&0u16.to_le_bytes());
    out.extend_from_slice(&32u32.to_le_bytes());
    out.extend_from_slice(&0u32.to_le_bytes());
    out
}

fn root_code(root: RootType) -> u32 {
    match root {
        RootType::Constants => 0,
        RootType::Table => 1,
        RootType::Descriptor => 2,
        RootType::ImmediateSampler => 3,
        RootType::ResourceHeap => 4,
        RootType::SamplerHeap => 5,
        RootType::Other(_) => 255,
    }
}

fn translate_synthetic_compute() -> Result<(victory_shader::emit::HostShader, [u32; 3]), String> {
    let native = synthetic_compute_program();
    let envelope = empty_envelope();
    let container = Container::parse(&envelope).map_err(|e| e.to_string())?;
    let (program, structured) =
        victory_shader::range::build(&native.code, native.wave64, native.vgprs)
            .map_err(|e| e.to_string())?;
    let structured = structured.map_err(|e| e.to_string())?;
    let resources =
        victory_shader::resource::track(&program, &native.layout, native.stage, Some(&structured))
            .map_err(|e| e.to_string())?;
    let io = victory_shader::io::raise(&program, &native).map_err(|e| e.to_string())?;
    let shader = victory_shader::emit::emit(&container, &native, &program, &structured, &io, &resources)
        .map_err(|e| e.to_string())?;
    Ok((shader, [1, 1, 1]))
}

/// Translates the synthetic compute fixture to a host DXIL container and
/// reports what the host binds for it. Returns 0 on success, 2 when the
/// translator refuses it (reason in `info.message`), 3 when the shader reads
/// more user data than `binding` holds, -1 for invalid arguments. Release
/// `out` with `sx_victory_buffer_free`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sx_victory_synthetic_compute(
    out: *mut SxVictoryByteBuffer,
    binding: *mut SxVictoryComputeBinding,
    info: *mut SxVictoryShaderInfo,
) -> i32 {
    if out.is_null() || binding.is_null() || info.is_null() {
        return -1;
    }
    // SAFETY: the C++ caller passes three live, exclusive objects.
    let (output, binding, report) = unsafe { (&mut *out, &mut *binding, &mut *info) };
    output.data = std::ptr::null_mut();
    output.size = 0;
    clear(report);
    let (shader, threads) = match translate_synthetic_compute() {
        Ok(done) => done,
        Err(text) => {
            message(report, text);
            return 2;
        }
    };
    if shader.user_data.len() > SYNTHETIC_USER_DATA_CAPACITY {
        message(report, "the shader reads more user data than the binding holds");
        return 3;
    }
    report.container_status = 0;
    report.native_status = 0;
    report.native_program_count = 1;
    report.stage_mask = stage_bit(shader.stage);
    fill_binding(binding, &shader, threads);
    hand_over(output, shader.dxil);
    0
}

fn fill_binding(binding: &mut SxVictoryComputeBinding, shader: &victory_shader::emit::HostShader, threads: [u32; 3]) {
    binding.user_data_dwords = shader.user_data_dwords();
    binding.user_data_count = shader.user_data.len() as u32;
    binding.user_data = [SxVictoryUserData { index: 0, slot: 0, root: 0, dword: 0 };
        SYNTHETIC_USER_DATA_CAPACITY];
    for (dst, src) in binding.user_data.iter_mut().zip(&shader.user_data) {
        *dst = SxVictoryUserData {
            index: src.index,
            slot: u32::from(src.slot),
            root: root_code(src.root),
            dword: src.dword,
        };
    }
    binding.root_cbv_count = shader.root_cbvs.len() as u32;
    binding.root_srv_count = shader.root_srvs.len() as u32;
    binding.root_uav_count = shader.root_uavs.len() as u32;
    binding.sampler_count = shader.samplers.len() as u32;
    binding.static_sampler_count = shader.static_samplers.len() as u32;
    binding.flags = u32::from(shader.memory)
        | u32::from(shader.memory_writes) << 1
        | u32::from(shader.view_addresses) << 2
        | u32::from(shader.heap) << 3
        | u32::from(shader.heap_writes) << 4
        | u32::from(shader.view_marks) << 5;
    binding.threads = threads;
}

// ---------------------------------------------------------------------------
// Console-shaped containers
//
// A title hands pipeline creation a DXBC container whose `DXIL` part carries
// an `XDAT` index and, among its blobs, the native code: an `XBCH` wrapper
// around a zlib stream of an `FSSM` record set. The writer below produces
// that shape for a compute program, following the layout victory-shader's
// parser documents (`crates/victory-shader/src/native.rs`, module notes) and
// its test helpers. Nothing here is copied from a title's container.
// ---------------------------------------------------------------------------

const XDAT_OFFSET: usize = 0x18;
const KIND_NATIVE: u32 = 0x0d;
const FSSM_SLOT_CS: usize = 0x60;
const FSSM_ABSENT: u64 = 0xDEAD_BEEF_DEAD_BEEF;
const FSSM_RECORD_SIZE: usize = 0x54;
const FSSM_FLAG_WAVE32: u32 = 1 << 5;

fn adler32(data: &[u8]) -> u32 {
    let (mut a, mut b) = (1u32, 0u32);
    for &byte in data {
        a = (a + u32::from(byte)) % 65521;
        b = (b + a) % 65521;
    }
    b << 16 | a
}

/// A zlib stream of stored (uncompressed) deflate blocks.
fn zlib_stored(data: &[u8]) -> Vec<u8> {
    let mut out = vec![0x78, 0x01];
    let mut chunks = data.chunks(0xFFFF).peekable();
    if chunks.peek().is_none() {
        out.extend_from_slice(&[1, 0, 0, 0xFF, 0xFF]);
    }
    while let Some(chunk) = chunks.next() {
        out.push(u8::from(chunks.peek().is_none()));
        out.extend_from_slice(&(chunk.len() as u16).to_le_bytes());
        out.extend_from_slice(&(!(chunk.len() as u16)).to_le_bytes());
        out.extend_from_slice(chunk);
    }
    out.extend_from_slice(&adler32(data).to_be_bytes());
    out
}

fn raw_root_type(root: RootType) -> u32 {
    match root {
        RootType::Constants => 0,
        RootType::Descriptor => 1,
        RootType::Table => 3,
        RootType::ImmediateSampler => 4,
        RootType::ResourceHeap => 5,
        RootType::SamplerHeap => 6,
        RootType::Other(n) => n,
    }
}

fn raw_resource_type(kind: ResourceType) -> u8 {
    match kind {
        ResourceType::Srv => 0,
        ResourceType::Sampler => 1,
        ResourceType::Cbv => 2,
        ResourceType::Uav => 3,
        ResourceType::Other(n) => n,
    }
}

/// The `FSSM` record set of one compute program.
fn fssm_compute(native: &NativeProgram) -> Vec<u8> {
    let StageIo::Cs(io) = &native.io else {
        unreachable!("the writer is only given compute programs");
    };
    let mut payload = vec![0u8; FSSM_SLOT_CS + 8];
    payload[..8].copy_from_slice(&6u64.to_le_bytes()); // graphics slot count
    for at in [0x08usize, 0x10, 0x18, 0x20, 0x28, 0x30, 0x48, FSSM_SLOT_CS] {
        payload[at..at + 8].copy_from_slice(&FSSM_ABSENT.to_le_bytes());
    }
    let record = payload.len();
    payload[FSSM_SLOT_CS..FSSM_SLOT_CS + 8].copy_from_slice(&(record as u64).to_le_bytes());

    // Record, then the compute stage data, the two root tables, the code.
    let stage_data = record + FSSM_RECORD_SIZE;
    let layout_at = stage_data + 0x34;
    let used_at = layout_at + 20 * native.layout.len();
    let code_at = used_at + 20 * native.used.len();
    payload.resize(code_at, 0);
    let put32 = |p: &mut Vec<u8>, at: usize, v: u32| p[at..at + 4].copy_from_slice(&v.to_le_bytes());
    let put64 = |p: &mut Vec<u8>, at: usize, v: u64| p[at..at + 8].copy_from_slice(&v.to_le_bytes());
    put64(&mut payload, record, code_at as u64);
    put64(&mut payload, record + 8, native.code.len() as u64);
    put32(&mut payload, record + 0x14, if native.wave64 { 0 } else { FSSM_FLAG_WAVE32 });
    put64(&mut payload, record + 0x28, layout_at as u64);
    put64(&mut payload, record + 0x30, used_at as u64);
    put32(&mut payload, record + 0x38, native.layout.len() as u32);
    put32(&mut payload, record + 0x3c, native.used.len() as u32);
    put32(&mut payload, record + 0x50, u32::from(native.vgprs) | 31 << 20);
    put32(&mut payload, stage_data + 0x24, io.rsrc2);
    for (k, t) in io.threads.iter().enumerate() {
        put32(&mut payload, stage_data + 0x28 + 4 * k, *t);
    }
    for (k, param) in native.layout.iter().enumerate() {
        let at = layout_at + 20 * k;
        put32(&mut payload, at, raw_root_type(param.root));
        put32(&mut payload, at + 4, param.table_offset_dwords);
        put32(&mut payload, at + 8, param.sgprs);
        put32(&mut payload, at + 12, param.base_register);
        payload[at + 16] = param.user_data;
        payload[at + 17] = param.slot;
    }
    for (k, used) in native.used.iter().enumerate() {
        let at = used_at + 20 * k;
        payload[at] = raw_resource_type(used.kind);
        payload[at + 1] = raw_root_type(used.root) as u8;
        put32(&mut payload, at + 4, used.table_offset_dwords);
        put32(&mut payload, at + 8, used.lower_bound);
        put32(&mut payload, at + 12, used.stride);
        payload[at + 16] = used.dwords;
        payload[at + 17] = used.user_data;
        payload[at + 18] = used.slot;
    }
    payload.extend_from_slice(&native.code);

    let mut out = b"FSSM".to_vec();
    out.extend_from_slice(&0x101u32.to_le_bytes());
    out.extend_from_slice(&(payload.len() as u64).to_le_bytes());
    out.extend_from_slice(&payload);
    out
}

fn xbch(unpacked: &[u8]) -> Vec<u8> {
    let stream = zlib_stored(unpacked);
    let mut out = b"XBCH".to_vec();
    out.extend_from_slice(&3u32.to_le_bytes());
    out.extend_from_slice(&(stream.len() as u64).to_le_bytes());
    out.extend_from_slice(&(unpacked.len() as u64).to_le_bytes());
    out.extend_from_slice(&stream);
    out
}

/// A DXBC container with one `DXIL` part: a program header that carries no
/// bitcode (as a stripped shader's does), the `XDAT` index, then the blobs.
fn console_container(blobs: &[(u32, Vec<u8>)]) -> Vec<u8> {
    let mut part = vec![0u8; XDAT_OFFSET];
    part.extend_from_slice(b"XDAT");
    part.extend_from_slice(&(blobs.len() as u32).to_le_bytes());
    part.extend_from_slice(&0x24u32.to_le_bytes());
    let mut offset = part.len() + 12 * blobs.len();
    for (kind, blob) in blobs {
        for v in [*kind, offset as u32, blob.len() as u32] {
            part.extend_from_slice(&v.to_le_bytes());
        }
        offset += blob.len();
    }
    for (_, blob) in blobs {
        part.extend_from_slice(blob);
    }
    while part.len() % 4 != 0 {
        part.push(0);
    }
    // Program header: cs_6_0, the size in dwords, the DXIL magic and version.
    part[0..4].copy_from_slice(&((5u32 << 16) | (6 << 4)).to_le_bytes());
    let dwords = (part.len() / 4) as u32;
    part[4..8].copy_from_slice(&dwords.to_le_bytes());
    part[8..12].copy_from_slice(b"DXIL");
    part[12..16].copy_from_slice(&0x100u32.to_le_bytes());

    let mut out = b"DXBC".to_vec();
    out.extend_from_slice(&[0; 16]);
    out.extend_from_slice(&1u16.to_le_bytes());
    out.extend_from_slice(&0u16.to_le_bytes());
    out.extend_from_slice(&((32 + 4 + 8 + part.len()) as u32).to_le_bytes());
    out.extend_from_slice(&1u32.to_le_bytes());
    out.extend_from_slice(&36u32.to_le_bytes());
    out.extend_from_slice(b"DXIL");
    out.extend_from_slice(&(part.len() as u32).to_le_bytes());
    out.extend_from_slice(&part);
    out
}

/// A compute program that loads its result through a root descriptor in
/// s[4:5] and stores it to the raw UAV of a table in s6. The pinned translator
/// refuses it ("a descriptor read only as data"), which makes it the control
/// for a native program that cannot be translated.
fn synthetic_refused_program() -> NativeProgram {
    let mut words = Vec::new();
    words.extend_from_slice(&s_load_dwordx4(8, 4)); // s[8:11] = the constant buffer's first dwords
    words.push(s_mov_b32(7, 0x80));
    words.extend_from_slice(&s_load_dwordx4(12, 6)); // s[12:15] = the UAV descriptor
    words.push(vop1(V_MOV_B32, 0, 8)); // v0 = s8
    words.extend_from_slice(&buffer_store_dword(0, 12, 0));
    words.push(S_ENDPGM);
    NativeProgram {
        stage: HwStage::Cs,
        wave64: false,
        vgprs: 16,
        layout: vec![
            RootParam {
                root: RootType::Constants,
                table_offset_dwords: 0,
                sgprs: 4,
                base_register: 0,
                user_data: 0,
                slot: 0,
            },
            RootParam {
                root: RootType::Descriptor,
                table_offset_dwords: 0,
                sgprs: 2,
                base_register: 0,
                user_data: 4,
                slot: 1,
            },
            RootParam {
                root: RootType::Table,
                table_offset_dwords: 0,
                sgprs: 1,
                base_register: 0,
                user_data: 6,
                slot: 2,
            },
        ],
        used: vec![
            UsedResource {
                kind: ResourceType::Cbv,
                root: RootType::Descriptor,
                table_offset_dwords: 0,
                lower_bound: 0,
                stride: 0,
                dwords: 2,
                user_data: 4,
                slot: 1,
            },
            UsedResource {
                kind: ResourceType::Uav,
                root: RootType::Table,
                table_offset_dwords: 0,
                lower_bound: 0,
                stride: 0,
                dwords: 4,
                user_data: 6,
                slot: 2,
            },
        ],
        io: StageIo::Cs(ComputeIo {
            rsrc2: 7 << 1, // seven user SGPRs: s0..s6
            threads: [1, 1, 1],
        }),
        code: words.iter().flat_map(|w| w.to_le_bytes()).collect(),
    }
}

/// The synthetic containers the tests use. 0: the compute fixture. 1: the same
/// with its native stream cut short (malformed). 2: a console-shaped container
/// whose index lists no native code (missing). 3: a compute program that reads
/// through a root descriptor, which the translator refuses.
fn synthetic_container(variant: u32) -> Option<Vec<u8>> {
    match variant {
        0 => Some(console_container(&[(KIND_NATIVE, xbch(&fssm_compute(&synthetic_compute_program())))])),
        1 => {
            let mut blob = xbch(&fssm_compute(&synthetic_compute_program()));
            blob.truncate(blob.len() - 16);
            Some(console_container(&[(KIND_NATIVE, blob)]))
        }
        2 => Some(console_container(&[(0x10, vec![0u8; 16])])),
        3 => Some(console_container(&[(KIND_NATIVE, xbch(&fssm_compute(&synthetic_refused_program())))])),
        _ => None,
    }
}

/// Writes one of the synthetic console-shaped containers (see
/// `synthetic_container`). Returns 0, or 1 for an unknown variant, -1 for a
/// null argument. Release `out` with `sx_victory_buffer_free`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sx_victory_synthetic_container(variant: u32, out: *mut SxVictoryByteBuffer) -> i32 {
    if out.is_null() {
        return -1;
    }
    // SAFETY: the C++ caller passes a live, exclusive object.
    let output = unsafe { &mut *out };
    output.data = std::ptr::null_mut();
    output.size = 0;
    match synthetic_container(variant) {
        Some(bytes) => {
            hand_over(output, bytes);
            0
        }
        None => 1,
    }
}

/// What a container is, for a compute pipeline.
enum ComputeInput {
    /// No console index: a desktop shader, the host's as it is.
    Desktop,
    /// Console-shaped, with no native compute program in it.
    NoNativeCompute,
    Malformed(String),
    Refused(String),
    Translated(Box<victory_shader::emit::HostShader>, [u32; 3]),
}

fn translate_compute_container(data: &[u8]) -> ComputeInput {
    let Ok(container) = Container::parse(data) else {
        return ComputeInput::Desktop;
    };
    let Some(part) = container.part(FourCC::DXIL).or(container.part(FourCC::ACCC)) else {
        return ComputeInput::Desktop;
    };
    let console = part.data.starts_with(b"FSPX")
        || part.data.get(XDAT_OFFSET..XDAT_OFFSET + 4) == Some(b"XDAT");
    if !console {
        return ComputeInput::Desktop;
    }
    let programs = match native_programs(part.data) {
        Ok(programs) => programs,
        Err(err) => return ComputeInput::Malformed(err.to_string()),
    };
    let Some(threads) = programs.iter().find_map(|p| match (&p.io, p.stage) {
        (StageIo::Cs(io), HwStage::Cs) => Some(io.threads),
        _ => None,
    }) else {
        return ComputeInput::NoNativeCompute;
    };
    match victory_shader::emit::translate(data) {
        Ok(shader) if shader.stage == HwStage::Cs => ComputeInput::Translated(Box::new(shader), threads),
        Ok(_) => ComputeInput::Refused("the container's first program is not its compute program".into()),
        Err(err) => ComputeInput::Refused(err.to_string()),
    }
}

/// Translates the compute program of a console-shaped container and reports
/// what the host binds for it. Returns 0 translated (release `out` with
/// `sx_victory_buffer_free`), 1 not console-shaped (a desktop shader: nothing
/// to do), 2 console-shaped without a native compute program, 3 malformed
/// native data, 4 the translator refuses the program, 5 it reads more user
/// data than `binding` holds, -1 invalid arguments. The reason for 2 to 5 is
/// in `info.message`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sx_victory_shader_translate_compute(
    data: *const u8,
    size: usize,
    out: *mut SxVictoryByteBuffer,
    binding: *mut SxVictoryComputeBinding,
    info: *mut SxVictoryShaderInfo,
) -> i32 {
    if data.is_null() || out.is_null() || binding.is_null() || info.is_null() {
        return -1;
    }
    // SAFETY: the C++ caller passes a readable range and three live, exclusive objects.
    let input = unsafe { slice::from_raw_parts(data, size) };
    let (output, binding, report) = unsafe { (&mut *out, &mut *binding, &mut *info) };
    output.data = std::ptr::null_mut();
    output.size = 0;
    inspect(input, report);
    match translate_compute_container(input) {
        ComputeInput::Desktop => 1,
        ComputeInput::NoNativeCompute => {
            message(report, "a console-shaped container without a native compute program");
            2
        }
        ComputeInput::Malformed(text) => {
            message(report, text);
            3
        }
        ComputeInput::Refused(text) => {
            message(report, text);
            4
        }
        ComputeInput::Translated(shader, threads) => {
            if shader.user_data.len() > SYNTHETIC_USER_DATA_CAPACITY {
                message(report, "the shader reads more user data than the binding holds");
                return 5;
            }
            fill_binding(binding, &shader, threads);
            hand_over(output, shader.dxil);
            0
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_synthetic_container_parses_back_to_the_program_it_was_written_from() {
        let native = synthetic_compute_program();
        let bytes = synthetic_container(0).unwrap();
        let container = Container::parse(&bytes).expect("container");
        let part = container.part(FourCC::DXIL).expect("DXIL part");
        let parsed = native_programs(part.data).expect("native programs");
        assert_eq!(parsed, [native]);
    }

    #[test]
    fn a_console_container_translates_to_the_same_shader_as_the_program() {
        let (direct, threads) = translate_synthetic_compute().unwrap();
        match translate_compute_container(&synthetic_container(0).unwrap()) {
            ComputeInput::Translated(shader, t) => {
                assert_eq!(*shader, direct);
                assert_eq!(t, threads);
            }
            _ => panic!("the container did not translate"),
        }
    }

    #[test]
    fn the_control_containers_are_told_apart() {
        assert!(matches!(translate_compute_container(&synthetic_container(1).unwrap()), ComputeInput::Malformed(_)));
        assert!(matches!(translate_compute_container(&synthetic_container(2).unwrap()), ComputeInput::NoNativeCompute));
        // A translated shader is itself a desktop container.
        let (host, _) = translate_synthetic_compute().unwrap();
        assert!(matches!(translate_compute_container(&host.dxil), ComputeInput::Desktop));
        assert!(matches!(translate_compute_container(b"not a container"), ComputeInput::Desktop));
        assert!(synthetic_container(99).is_none());
    }

    #[test]
    fn the_refused_program_is_reported_with_the_translators_reason() {
        match translate_compute_container(&synthetic_container(3).unwrap()) {
            ComputeInput::Refused(text) => assert!(text.contains("is not supported"), "{text}"),
            _ => panic!("the translator was expected to refuse this program"),
        }
    }

    #[test]
    fn stored_zlib_streams_inflate() {
        // Through the parser's own inflate: a native blob of more than one stored block.
        let mut native = synthetic_compute_program();
        native.code.extend(std::iter::repeat_n(0u8, 70_000 * 4).collect::<Vec<_>>());
        let blob = xbch(&fssm_compute(&native));
        let bytes = console_container(&[(KIND_NATIVE, blob)]);
        let container = Container::parse(&bytes).unwrap();
        let parsed = native_programs(container.part(FourCC::DXIL).unwrap().data).unwrap();
        assert_eq!(parsed[0].code.len(), native.code.len());
        assert_eq!(adler32(b"Wikipedia"), 0x11E6_0398);
    }

    #[test]
    fn a_buffer_with_spare_capacity_is_handed_over_and_freed_by_its_length() {
        let mut bytes = Vec::with_capacity(4096);
        bytes.extend_from_slice(&[1u8, 2, 3, 4, 5]);
        let mut buffer = SxVictoryByteBuffer { data: std::ptr::null_mut(), size: 0 };
        hand_over(&mut buffer, bytes);
        assert_eq!(buffer.size, 5);
        // SAFETY: `hand_over` just filled the buffer.
        assert_eq!(unsafe { slice::from_raw_parts(buffer.data, buffer.size) }, [1, 2, 3, 4, 5]);
        unsafe { sx_victory_buffer_free(&mut buffer) };
        assert!(buffer.data.is_null() && buffer.size == 0);
        // Freeing twice, or freeing nothing, is harmless.
        unsafe { sx_victory_buffer_free(&mut buffer) };
        unsafe { sx_victory_buffer_free(std::ptr::null_mut()) };
    }

    #[test]
    fn synthetic_compute_translates_to_a_dxil_container() {
        let (shader, threads) = translate_synthetic_compute().expect("translate");
        assert_eq!(shader.stage, HwStage::Cs);
        assert_eq!(threads, [1, 1, 1]);
        assert_eq!(&shader.dxil[..4], b"DXBC");
        let container = Container::parse(&shader.dxil).expect("host container");
        assert!(container.part(FourCC::DXIL).is_some());
        // Four root constants and the table's heap offset: five dwords, rounded to eight.
        assert_eq!(shader.user_data_dwords(), 8);
        let constants = shader.user_data.iter().filter(|u| u.root == RootType::Constants).count();
        let tables = shader.user_data.iter().filter(|u| u.root == RootType::Table).count();
        assert_eq!((constants, tables), (4, 1));
        assert!(shader.root_cbvs.is_empty() && shader.root_srvs.is_empty() && shader.root_uavs.is_empty());
        assert!(!shader.memory && !shader.heap && !shader.view_addresses && !shader.view_marks);
    }
}

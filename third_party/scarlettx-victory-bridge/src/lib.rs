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

const FSSM_SLOT_VS: usize = 0x28;
const FSSM_SLOT_PS: usize = 0x30;
/// Stage data of a vertex or pixel program: the semantic tables, then eight
/// export patches of 404 bytes from 0x14c, each absent (code offset -1).
const GRAPHICS_STAGE_DATA: usize = 0x14c + 8 * 404;

/// The `FSSM` record set of one program: a compute program, or a vertex or
/// pixel program without attribute inputs, parameter outputs or export
/// patches (which is all the synthetic fixtures use).
fn fssm_program(native: &NativeProgram) -> Vec<u8> {
    let (slot, mut stage) = match &native.io {
        StageIo::Cs(io) => {
            let mut data = vec![0u8; 0x34];
            data[0x24..0x28].copy_from_slice(&io.rsrc2.to_le_bytes());
            for (k, t) in io.threads.iter().enumerate() {
                data[0x28 + 4 * k..0x2c + 4 * k].copy_from_slice(&t.to_le_bytes());
            }
            (FSSM_SLOT_CS, data)
        }
        StageIo::Vs { inputs, outputs } => {
            assert!(inputs.is_empty() && outputs.is_empty(), "the writer has no semantic tables");
            (FSSM_SLOT_VS, vec![0u8; GRAPHICS_STAGE_DATA])
        }
        StageIo::Ps { inputs, input_addr, db_shader_control, patches } => {
            assert!(inputs.is_empty() && patches.is_empty(), "the writer has no semantic tables or patches");
            let mut data = vec![0u8; GRAPHICS_STAGE_DATA];
            data[0x134..0x138].copy_from_slice(&input_addr.to_le_bytes());
            data[0x138..0x13c].copy_from_slice(&db_shader_control.to_le_bytes());
            (FSSM_SLOT_PS, data)
        }
        _ => unreachable!("the writer is given compute, vertex and pixel programs only"),
    };
    if slot != FSSM_SLOT_CS {
        for mrt in 0..8 {
            let at = 0x14c + mrt * 404;
            stage[at..at + 4].copy_from_slice(&u32::MAX.to_le_bytes());
        }
    }
    let mut payload = vec![0u8; FSSM_SLOT_CS + 8];
    payload[..8].copy_from_slice(&6u64.to_le_bytes()); // graphics slot count
    for at in [0x08usize, 0x10, 0x18, 0x20, 0x28, 0x30, 0x48, FSSM_SLOT_CS] {
        payload[at..at + 8].copy_from_slice(&FSSM_ABSENT.to_le_bytes());
    }
    let record = payload.len();
    payload[slot..slot + 8].copy_from_slice(&(record as u64).to_le_bytes());

    // Record, then the stage data, the two root tables, the code.
    let stage_data = record + FSSM_RECORD_SIZE;
    let layout_at = stage_data + stage.len();
    let used_at = layout_at + 20 * native.layout.len();
    let code_at = used_at + 20 * native.used.len();
    payload.resize(code_at, 0);
    payload[stage_data..layout_at].copy_from_slice(&stage);
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

/// The `FSSM` record set of one compute program.
fn fssm_compute(native: &NativeProgram) -> Vec<u8> {
    fssm_program(native)
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

/// The data of a console `DXIL` part: a program header that carries no
/// bitcode (as a stripped shader's does), the `XDAT` index, then the blobs.
fn console_dxil_part(blobs: &[(u32, Vec<u8>)], stage: HwStage) -> Vec<u8> {
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
    // Program header: the shader kind and model 6.0, the size in dwords, the DXIL magic and version.
    let kind: u32 = match stage {
        HwStage::Ps => 0,
        HwStage::Vs => 1,
        _ => 5,
    };
    part[0..4].copy_from_slice(&((kind << 16) | (6 << 4)).to_le_bytes());
    let dwords = (part.len() / 4) as u32;
    part[4..8].copy_from_slice(&dwords.to_le_bytes());
    part[8..12].copy_from_slice(b"DXIL");
    part[12..16].copy_from_slice(&0x100u32.to_le_bytes());
    part
}

/// A DXBC container with one console `DXIL` part (compute programs).
fn console_container(blobs: &[(u32, Vec<u8>)]) -> Vec<u8> {
    dxbc_envelope(&[(b"DXIL", console_dxil_part(blobs, HwStage::Cs))])
}

/// A DXBC envelope of these parts.
fn dxbc_envelope(parts: &[(&[u8; 4], Vec<u8>)]) -> Vec<u8> {
    let table = 32 + 4 * parts.len();
    let total = table + parts.iter().map(|(_, d)| 8 + d.len()).sum::<usize>();
    let mut out = b"DXBC".to_vec();
    out.extend_from_slice(&[0; 16]);
    out.extend_from_slice(&1u16.to_le_bytes());
    out.extend_from_slice(&0u16.to_le_bytes());
    out.extend_from_slice(&(total as u32).to_le_bytes());
    out.extend_from_slice(&(parts.len() as u32).to_le_bytes());
    let mut offset = table;
    for (_, d) in parts {
        out.extend_from_slice(&(offset as u32).to_le_bytes());
        offset += 8 + d.len();
    }
    for (fourcc, d) in parts {
        out.extend_from_slice(*fourcc);
        out.extend_from_slice(&(d.len() as u32).to_le_bytes());
        out.extend_from_slice(d);
    }
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

/// MUBUF `buffer_store_dword vdata, vaddr, s[srsrc:srsrc+3], 0 offen`: the
/// byte offset comes from a VGPR.
const fn buffer_store_dword_at(vdata: u8, vaddr: u8, srsrc: u8) -> [u32; 2] {
    [
        0x38 << 26 | 0x1C << 18 | 1 << 12,
        0x80 << 24 | (srsrc as u32 >> 2) << 16 | (vdata as u32) << 8 | vaddr as u32,
    ]
}

/// VOPC `op vcc, src0, vsrc1`.
const fn vopc(op: u16, src0: u16, vsrc1: u8) -> u32 {
    0x7C00_0000 | (op as u32) << 17 | (vsrc1 as u32) << 9 | (src0 as u32 & 0x1FF)
}

/// SOP1 `op sdst, src0`.
const fn sop1(op: u16, sdst: u8, src0: u16) -> u32 {
    0xBE80_0000 | (sdst as u32) << 16 | (op as u32) << 8 | (src0 as u32 & 0xFF)
}

/// An inline integer constant operand, 0 to 64.
const fn int(k: u16) -> u16 {
    128 + k
}

const VCC: u16 = 106;
const EXEC: u8 = 126;
const V_LSHLREV_B32: u8 = 0x1A;
const V_CMP_GT_U32: u16 = 0xC4;
const S_MOV_B32: u16 = 0x03;
const S_MOV_B64: u16 = 0x04;
const S_BCNT1_I32_B32: u16 = 0x0F;
const S_AND_SAVEEXEC_B64: u16 = 0x24;
const S_AND_SAVEEXEC_B32: u16 = 0x3C;

/// The root layout the conformance programs share: four root constants in
/// s0..s3, the table of one raw UAV in s4, `user` user SGPRs in all.
fn constants_and_table(user: u32, rsrc2_bits: u32, threads: [u32; 3], wave64: bool, words: &[u32]) -> NativeProgram {
    NativeProgram {
        stage: HwStage::Cs,
        wave64,
        vgprs: 24,
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
            rsrc2: user << 1 | rsrc2_bits,
            threads,
        }),
        code: words.iter().flat_map(|w| w.to_le_bytes()).collect(),
    }
}

/// Dispatch group ids and an offset store. One thread per group; the group
/// id arrives in s6 (x) and s7 (y) after six user SGPRs. Each group writes
/// `((c0 + x) ^ c1) + (y << 16)` to dword `2 + x + 3y`.
fn synthetic_group_id_program() -> NativeProgram {
    let mut w = vec![s_mov_b32(5, 0x80)];
    w.extend_from_slice(&s_load_dwordx4(8, 4));
    w.push(vop1(V_MOV_B32, 1, 0)); // v1 = c0
    w.push(vop1(V_MOV_B32, 2, 1)); // v2 = c1
    w.push(vop1(V_MOV_B32, 5, 6)); // v5 = group x
    w.push(vop1(V_MOV_B32, 6, 7)); // v6 = group y
    w.push(vop2(V_LSHLREV_B32, 7, int(1), 6)); // v7 = 2y
    w.push(vop2(V_ADD_NC_U32, 7, vgpr(7), 6)); // v7 = 3y
    w.push(vop2(V_ADD_NC_U32, 7, vgpr(7), 5)); // v7 = x + 3y
    w.push(vop2(V_LSHLREV_B32, 8, int(2), 7)); // v8 = 4 (x + 3y)
    w.push(vop2(V_ADD_NC_U32, 8, int(8), 8)); // v8 += 8: two dwords in
    w.push(vop2(V_ADD_NC_U32, 0, vgpr(1), 5)); // v0 = c0 + x
    w.push(vop2(V_XOR_B32, 0, vgpr(0), 2)); // v0 ^= c1
    w.push(vop2(V_LSHLREV_B32, 9, int(16), 6)); // v9 = y << 16
    w.push(vop2(V_ADD_NC_U32, 0, vgpr(0), 9)); // v0 += v9
    w.extend_from_slice(&buffer_store_dword_at(0, 8, 8));
    w.push(S_ENDPGM);
    // TGID_X_EN (bit 7) and TGID_Y_EN (bit 8).
    constants_and_table(6, 1 << 7 | 1 << 8, [1, 1, 1], false, &w)
}

/// A lane mask. `lanes` threads in one group, one wave: lane l (its thread
/// id, in v0) writes `l + c2` to dword l when `l < c0`, and `c1` otherwise.
/// The lanes under the compare are selected with EXEC (`s_and_saveexec`),
/// which is restored before the store. With `count`, the number of selected
/// lanes (`s_bcnt1` of the mask, a cross-lane value) also goes to dword
/// `lanes`.
fn synthetic_lane_mask_program(wave64: bool, count: bool) -> NativeProgram {
    let lanes: u32 = if wave64 { 64 } else { 32 };
    let mut w = vec![s_mov_b32(5, 0x80)];
    w.extend_from_slice(&s_load_dwordx4(8, 4));
    w.push(vop2(V_LSHLREV_B32, 16, int(2), 0)); // v16 = 4 lane
    w.push(vop1(V_MOV_B32, 3, 1)); // v3 = c1
    w.push(vopc(V_CMP_GT_U32, 0, 0)); // vcc = c0 > lane
    if count {
        w.push(sop1(S_BCNT1_I32_B32, 21, VCC)); // s21 = lanes selected
    }
    if wave64 {
        w.push(sop1(S_AND_SAVEEXEC_B64, 22, VCC)); // s[22:23] = exec; exec &= vcc
    } else {
        w.push(sop1(S_AND_SAVEEXEC_B32, 22, VCC)); // s22 = exec; exec &= vcc
    }
    w.push(vop2(V_ADD_NC_U32, 3, 2, 0)); // selected lanes: v3 = c2 + lane
    if wave64 {
        w.push(sop1(S_MOV_B64, EXEC, 22));
    } else {
        w.push(sop1(S_MOV_B32, EXEC, 22));
    }
    w.extend_from_slice(&buffer_store_dword_at(3, 16, 8));
    if count {
        w.push(vop1(V_MOV_B32, 4, 21)); // v4 = the count
        w.extend_from_slice(&buffer_store_dword(4, 8, (lanes * 4) as u16));
    }
    w.push(S_ENDPGM);
    constants_and_table(5, 0, [lanes, 1, 1], wave64, &w)
}

/// MUBUF `buffer_store_dword vdata, vaddr, s[srsrc:srsrc+3], 0 offen offset:byte_off`.
const fn buffer_store_dword_at_off(vdata: u8, vaddr: u8, srsrc: u8, byte_off: u16) -> [u32; 2] {
    [
        0x38 << 26 | 0x1C << 18 | 1 << 12 | (byte_off as u32 & 0xFFF),
        0x80 << 24 | (srsrc as u32 >> 2) << 16 | (vdata as u32) << 8 | vaddr as u32,
    ]
}

/// DS `ds_bpermute_b32 vdst, vaddr, vdata`: each lane reads `vdata` of the
/// lane its address names (bytes, four a lane, modulo the wave).
const fn ds_bpermute_b32(vdst: u8, vaddr: u8, vdata: u8) -> [u32; 2] {
    [
        0x36 << 26 | 0xB3 << 18,
        (vdst as u32) << 24 | (vdata as u32) << 8 | vaddr as u32,
    ]
}

const V_MUL_U32_U24: u8 = 0x0B;
const S_WAITCNT_LGKM: u32 = 0xBF8C_C07F;
/// A literal operand: the constant is the next word.
const LITERAL: u16 = 255;
/// What the lane exchange multiplies a lane's number by to make its value.
pub const LANE_EXCHANGE_MULTIPLIER: u32 = 0x0001_0203;

/// A lane exchange. One group of `lanes` threads, one console wave (32, or 64
/// with `wave64`). Lane l holds the value `(l * 0x10203) ^ c0`, different in
/// every lane. Two permutes move the values between lanes:
///
/// * dword `l` takes the value of lane `lanes - 1 - l` (the wave reversed:
///   in a wave of 64, lane 31 takes lane 32's value and lane 32 takes lane
///   31's, and every lane reads from the other half);
/// * dword `lanes + l` takes the value of lane `l ^ lanes/2` (the halves
///   exchanged).
///
/// Nothing else is written. The exchange only works when the host runs the
/// group as one wave of the console's size, which the translated shader
/// declares.
fn synthetic_lane_exchange_program(wave64: bool) -> NativeProgram {
    let lanes: u16 = if wave64 { 64 } else { 32 };
    let mut w = vec![s_mov_b32(5, 0x80)];
    w.extend_from_slice(&s_load_dwordx4(8, 4));
    w.push(vop2(V_LSHLREV_B32, 16, int(2), 0)); // v16 = 4 lane
    w.push(vop2(V_MUL_U32_U24, 2, LITERAL, 0)); // v2 = lane * 0x10203
    w.push(LANE_EXCHANGE_MULTIPLIER);
    w.push(vop2(V_XOR_B32, 2, 0, 2)); // v2 ^= c0: the lane's value
    w.push(vop2(V_XOR_B32, 1, int(lanes - 1), 0)); // v1 = lanes - 1 - lane
    w.push(vop2(V_LSHLREV_B32, 1, int(2), 1)); // its address
    w.extend_from_slice(&ds_bpermute_b32(3, 1, 2)); // v3 = the value of that lane
    w.push(vop2(V_XOR_B32, 4, int(lanes / 2), 0)); // v4 = the same lane of the other half
    w.push(vop2(V_LSHLREV_B32, 4, int(2), 4));
    w.extend_from_slice(&ds_bpermute_b32(5, 4, 2)); // v5 = the value of that lane
    w.push(S_WAITCNT_LGKM);
    w.extend_from_slice(&buffer_store_dword_at(3, 16, 8));
    w.extend_from_slice(&buffer_store_dword_at_off(5, 16, 8, lanes * 4));
    w.push(S_ENDPGM);
    constants_and_table(5, 0, [u32::from(lanes), 1, 1], wave64, &w)
}

// ---------------------------------------------------------------------------
// Synthetic draw: a vertex and a pixel program
// ---------------------------------------------------------------------------

/// `exp <target> v<a>, v<b>, v<c>, v<d>` with all four channels, `done`
/// (and `vm` for the pixel program's last colour export).
const fn exp(target: u32, vsrc: [u8; 4], vm: bool) -> [u32; 2] {
    [
        0xF800_0000 | 0xF | target << 4 | 1 << 11 | (vm as u32) << 12,
        vsrc[0] as u32 | (vsrc[1] as u32) << 8 | (vsrc[2] as u32) << 16 | (vsrc[3] as u32) << 24,
    ]
}

const EXP_MRT0: u32 = 0;
const EXP_POS0: u32 = 12;
const V_ADD_F32: u8 = 0x03;
const V_MUL_F32: u8 = 0x08;
const V_AND_B32: u8 = 0x1B;
const V_CVT_F32_U32: u16 = 0x06;
/// Inline float constants.
const F_ONE: u16 = 242;
const F_MINUS_ONE: u16 = 243;
const F_TWO: u16 = 244;

/// A vertex program without attributes: vertex n of a triangle that covers
/// the viewport, from the vertex id in v0 (x = 2 * ((n << 1) & 2) - 1,
/// y = 2 * (n & 2) - 1, z = 0, w = 1).
fn synthetic_vertex_program() -> NativeProgram {
    let mut w = vec![
        vop2(V_LSHLREV_B32, 1, int(1), 0),      // v1 = id << 1
        vop2(V_AND_B32, 1, int(2), 1),          // v1 &= 2
        vop2(V_AND_B32, 2, int(2), 0),          // v2 = id & 2
        vop1(V_CVT_F32_U32, 1, vgpr(1)),        // to float
        vop1(V_CVT_F32_U32, 2, vgpr(2)),
        vop2(V_MUL_F32, 1, F_TWO, 1),           // * 2
        vop2(V_MUL_F32, 2, F_TWO, 2),
        vop2(V_ADD_F32, 1, F_MINUS_ONE, 1),     // - 1
        vop2(V_ADD_F32, 2, F_MINUS_ONE, 2),
        vop1(V_MOV_B32, 3, int(0)),             // z = 0
        vop1(V_MOV_B32, 4, F_ONE),              // w = 1
    ];
    w.extend_from_slice(&exp(EXP_POS0, [1, 2, 3, 4], false));
    w.push(S_ENDPGM);
    NativeProgram {
        stage: HwStage::Vs,
        wave64: false,
        vgprs: 8,
        layout: vec![],
        used: vec![],
        io: StageIo::Vs { inputs: vec![], outputs: vec![] },
        code: w.iter().flat_map(|w| w.to_le_bytes()).collect(),
    }
}

/// A pixel program that writes its four root constants (s0..s3, float bits)
/// to render target 0.
fn synthetic_pixel_program() -> NativeProgram {
    let mut w = Vec::new();
    for i in 0..4u8 {
        w.push(vop1(V_MOV_B32, i, u16::from(i))); // v<i> = s<i>
    }
    w.extend_from_slice(&exp(EXP_MRT0, [0, 1, 2, 3], true));
    w.push(S_ENDPGM);
    NativeProgram {
        stage: HwStage::Ps,
        wave64: false,
        vgprs: 4,
        layout: vec![RootParam {
            root: RootType::Constants,
            table_offset_dwords: 0,
            sgprs: 4,
            base_register: 0,
            user_data: 0,
            slot: 0,
        }],
        used: vec![],
        io: StageIo::Ps {
            inputs: vec![],
            input_addr: 0,
            db_shader_control: 0,
            patches: vec![],
        },
        code: w.iter().flat_map(|w| w.to_le_bytes()).collect(),
    }
}

/// An `ISG1`/`OSG1` part: (name, D3D system value, component type, register, mask) per element.
fn signature_part(elements: &[(&str, u32, u32, u32, u8)]) -> Vec<u8> {
    let mut out = Vec::new();
    out.extend_from_slice(&(elements.len() as u32).to_le_bytes());
    out.extend_from_slice(&8u32.to_le_bytes());
    let mut name_at = 8 + 32 * elements.len();
    for (name, system_value, component, register, mask) in elements {
        for v in [0u32, name_at as u32, 0, *system_value, *component, *register] {
            out.extend_from_slice(&v.to_le_bytes());
        }
        out.extend_from_slice(&[*mask, *mask, 0, 0]);
        out.extend_from_slice(&0u32.to_le_bytes());
        name_at += name.len() + 1;
    }
    for (name, ..) in elements {
        out.extend_from_slice(name.as_bytes());
        out.push(0);
    }
    while out.len() % 4 != 0 {
        out.push(0);
    }
    out
}

fn envelope(parts: &[(&[u8; 4], Vec<u8>)]) -> Vec<u8> {
    dxbc_envelope(parts)
}

/// D3D system values and component types of signature parts.
const NAME_POSITION: u32 = 1;
const NAME_VERTEX_ID: u32 = 6;
const NAME_TARGET: u32 = 64;
const COMPONENT_UINT: u32 = 1;
const COMPONENT_FLOAT: u32 = 3;

fn translate_graphics(native: &NativeProgram, signatures: Vec<u8>) -> Result<victory_shader::emit::HostShader, String> {
    let container = Container::parse(&signatures).map_err(|e| e.to_string())?;
    let (program, structured) = victory_shader::range::build(&native.patched_code(), native.wave64, native.vgprs)
        .map_err(|e| e.to_string())?;
    let structured = structured.map_err(|e| e.to_string())?;
    let resources =
        victory_shader::resource::track(&program, &native.layout, native.stage, Some(&structured))
            .map_err(|e| e.to_string())?;
    let io = victory_shader::io::raise(&program, native).map_err(|e| e.to_string())?;
    victory_shader::emit::emit(&container, native, &program, &structured, &io, &resources).map_err(|e| e.to_string())
}

fn translate_synthetic_draw() -> Result<(victory_shader::emit::HostShader, victory_shader::emit::HostShader), String> {
    let vs = translate_graphics(
        &synthetic_vertex_program(),
        envelope(&[
            (b"ISG1", signature_part(&[("SV_VertexID", NAME_VERTEX_ID, COMPONENT_UINT, 0, 0x1)])),
            (b"OSG1", signature_part(&[("SV_Position", NAME_POSITION, COMPONENT_FLOAT, 0, 0xF)])),
        ]),
    )
    .map_err(|e| format!("vertex program: {e}"))?;
    let ps = translate_graphics(
        &synthetic_pixel_program(),
        envelope(&[
            (b"ISG1", signature_part(&[("SV_Position", NAME_POSITION, COMPONENT_FLOAT, 0, 0xF)])),
            (b"OSG1", signature_part(&[("SV_Target", NAME_TARGET, COMPONENT_FLOAT, 0, 0xF)])),
        ]),
    )
    .map_err(|e| format!("pixel program: {e}"))?;
    Ok((vs, ps))
}

/// Translates the synthetic vertex and pixel programs to host DXIL and
/// reports what the host binds for each. Returns 0, 2 when the translator
/// refuses one (reason in `info.message`), 3 when one reads more user data
/// than a binding holds, -1 for invalid arguments. Release both buffers with
/// `sx_victory_buffer_free`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sx_victory_synthetic_draw(
    vs_out: *mut SxVictoryByteBuffer,
    vs_binding: *mut SxVictoryComputeBinding,
    ps_out: *mut SxVictoryByteBuffer,
    ps_binding: *mut SxVictoryComputeBinding,
    info: *mut SxVictoryShaderInfo,
) -> i32 {
    if vs_out.is_null() || vs_binding.is_null() || ps_out.is_null() || ps_binding.is_null() || info.is_null() {
        return -1;
    }
    // SAFETY: the C++ caller passes five live, exclusive objects.
    let (vs_out, vs_binding, ps_out, ps_binding, report) =
        unsafe { (&mut *vs_out, &mut *vs_binding, &mut *ps_out, &mut *ps_binding, &mut *info) };
    for buffer in [&mut *vs_out, &mut *ps_out] {
        buffer.data = std::ptr::null_mut();
        buffer.size = 0;
    }
    clear(report);
    let (vs, ps) = match translate_synthetic_draw() {
        Ok(done) => done,
        Err(text) => {
            message(report, text);
            return 2;
        }
    };
    if vs.user_data.len() > SYNTHETIC_USER_DATA_CAPACITY || ps.user_data.len() > SYNTHETIC_USER_DATA_CAPACITY {
        message(report, "a shader reads more user data than the binding holds");
        return 3;
    }
    report.container_status = 0;
    report.native_status = 0;
    report.native_program_count = 2;
    report.stage_mask = stage_bit(vs.stage) | stage_bit(ps.stage);
    fill_binding(vs_binding, &vs, [0, 0, 0]);
    fill_binding(ps_binding, &ps, [0, 0, 0]);
    hand_over(vs_out, vs.dxil);
    hand_over(ps_out, ps.dxil);
    0
}

fn vertex_signatures() -> [(&'static [u8; 4], Vec<u8>); 2] {
    [
        (b"ISG1", signature_part(&[("SV_VertexID", NAME_VERTEX_ID, COMPONENT_UINT, 0, 0x1)])),
        (b"OSG1", signature_part(&[("SV_Position", NAME_POSITION, COMPONENT_FLOAT, 0, 0xF)])),
    ]
}

fn pixel_signatures() -> [(&'static [u8; 4], Vec<u8>); 2] {
    [
        (b"ISG1", signature_part(&[("SV_Position", NAME_POSITION, COMPONENT_FLOAT, 0, 0xF)])),
        (b"OSG1", signature_part(&[("SV_Target", NAME_TARGET, COMPONENT_FLOAT, 0, 0xF)])),
    ]
}

/// A vertex or pixel program as a title's pipeline carries it: a DXBC
/// container with the signature parts and a console `DXIL` part holding the
/// native code.
fn graphics_container(native: &NativeProgram, signatures: [(&'static [u8; 4], Vec<u8>); 2], blob: Vec<u8>) -> Vec<u8> {
    let [isg1, osg1] = signatures;
    dxbc_envelope(&[isg1, osg1, (b"DXIL", console_dxil_part(&[(KIND_NATIVE, blob)], native.stage))])
}

/// The pixel fixture, plus a store to the raw UAV of a descriptor table in
/// s4: a translated pixel shader whose bindings go beyond root constants.
fn synthetic_pixel_program_with_table() -> NativeProgram {
    let mut w = vec![s_mov_b32(5, 0x80)];
    w.extend_from_slice(&s_load_dwordx4(8, 4));
    for i in 0..4u8 {
        w.push(vop1(V_MOV_B32, i, u16::from(i)));
    }
    w.extend_from_slice(&buffer_store_dword(0, 8, 0));
    w.extend_from_slice(&exp(EXP_MRT0, [0, 1, 2, 3], true));
    w.push(S_ENDPGM);
    let mut native = synthetic_pixel_program();
    native.code = w.iter().flat_map(|w| w.to_le_bytes()).collect();
    native.vgprs = 16;
    native.layout.push(RootParam {
        root: RootType::Table,
        table_offset_dwords: 0,
        sgprs: 1,
        base_register: 0,
        user_data: 4,
        slot: 1,
    });
    native.used.push(UsedResource {
        kind: ResourceType::Uav,
        root: RootType::Table,
        table_offset_dwords: 0,
        lower_bound: 0,
        stride: 0,
        dwords: 4,
        user_data: 4,
        slot: 1,
    });
    native
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
        // Conformance programs (see their functions): 4 group ids and an offset store,
        // 5 a wave32 lane mask, 6 the same with a cross-lane count, 7 a wave64 lane mask.
        4 => Some(console_container(&[(KIND_NATIVE, xbch(&fssm_compute(&synthetic_group_id_program())))])),
        5 => Some(console_container(&[(KIND_NATIVE, xbch(&fssm_compute(&synthetic_lane_mask_program(false, false))))])),
        6 => Some(console_container(&[(KIND_NATIVE, xbch(&fssm_compute(&synthetic_lane_mask_program(false, true))))])),
        7 => Some(console_container(&[(KIND_NATIVE, xbch(&fssm_compute(&synthetic_lane_mask_program(true, false))))])),
        // The draw fixture as a title's pipeline carries it: 9 the vertex program, 10 the pixel
        // program, 11 the vertex program with its native stream cut short, 12 a pixel program
        // that also stores to a table's UAV (bindings beyond root constants).
        9 => {
            let native = synthetic_vertex_program();
            Some(graphics_container(&native, vertex_signatures(), xbch(&fssm_program(&native))))
        }
        10 => {
            let native = synthetic_pixel_program();
            Some(graphics_container(&native, pixel_signatures(), xbch(&fssm_program(&native))))
        }
        11 => {
            let native = synthetic_vertex_program();
            let mut blob = xbch(&fssm_program(&native));
            blob.truncate(blob.len() - 16);
            Some(graphics_container(&native, vertex_signatures(), blob))
        }
        12 => {
            let native = synthetic_pixel_program_with_table();
            Some(graphics_container(&native, pixel_signatures(), xbch(&fssm_program(&native))))
        }
        // The lane exchange (see its function): 13 in a wave of 32, 14 in a wave of 64.
        13 => Some(console_container(&[(KIND_NATIVE, xbch(&fssm_compute(&synthetic_lane_exchange_program(false))))])),
        14 => Some(console_container(&[(KIND_NATIVE, xbch(&fssm_compute(&synthetic_lane_exchange_program(true))))])),
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
    translate_stage_container(data, HwStage::Cs)
}

/// Classifies a container for a pipeline stage and translates its native
/// program of that stage. For a compute program the thread-group size comes
/// along; for the other stages it is zero.
fn translate_stage_container(data: &[u8], stage: HwStage) -> ComputeInput {
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
    let Some(threads) = programs.iter().find_map(|p| match &p.io {
        _ if p.stage != stage => None,
        StageIo::Cs(io) => Some(io.threads),
        _ => Some([0, 0, 0]),
    }) else {
        return ComputeInput::NoNativeCompute;
    };
    match victory_shader::emit::translate(data) {
        Ok(shader) if shader.stage == stage => ComputeInput::Translated(Box::new(shader), threads),
        Ok(_) => ComputeInput::Refused("the container's first program is of another stage".into()),
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
    translate_stage_into(input, HwStage::Cs, output, binding, report)
}

/// [`sx_victory_shader_translate_compute`] for a pipeline stage: `stage` 0
/// vertex, 1 pixel, 2 compute. The return codes are the same; 2 means the
/// container carries no native program of that stage, -1 also an unknown stage.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sx_victory_shader_translate_stage(
    data: *const u8,
    size: usize,
    stage: u32,
    out: *mut SxVictoryByteBuffer,
    binding: *mut SxVictoryComputeBinding,
    info: *mut SxVictoryShaderInfo,
) -> i32 {
    let stage = match stage {
        0 => HwStage::Vs,
        1 => HwStage::Ps,
        2 => HwStage::Cs,
        _ => return -1,
    };
    if data.is_null() || out.is_null() || binding.is_null() || info.is_null() {
        return -1;
    }
    // SAFETY: the C++ caller passes a readable range and three live, exclusive objects.
    let input = unsafe { slice::from_raw_parts(data, size) };
    let (output, binding, report) = unsafe { (&mut *out, &mut *binding, &mut *info) };
    output.data = std::ptr::null_mut();
    output.size = 0;
    translate_stage_into(input, stage, output, binding, report)
}

fn translate_stage_into(
    input: &[u8],
    stage: HwStage,
    output: &mut SxVictoryByteBuffer,
    binding: &mut SxVictoryComputeBinding,
    report: &mut SxVictoryShaderInfo,
) -> i32 {
    inspect(input, report);
    match translate_stage_container(input, stage) {
        ComputeInput::Desktop => 1,
        ComputeInput::NoNativeCompute => {
            message(report, "a console-shaped container without a native program of this stage");
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
    fn the_conformance_programs_translate() {
        for (variant, threads) in [(4u32, [1u32, 1, 1]), (5, [32, 1, 1]), (6, [32, 1, 1]), (7, [64, 1, 1])] {
            match translate_compute_container(&synthetic_container(variant).unwrap()) {
                ComputeInput::Translated(shader, t) => {
                    assert_eq!(t, threads, "variant {variant}");
                    assert_eq!(shader.user_data_dwords(), 8, "variant {variant}: {:?}", shader.user_data);
                    assert!(shader.root_cbvs.is_empty() && shader.root_srvs.is_empty() && shader.root_uavs.is_empty());
                    assert!(!shader.memory && !shader.heap, "variant {variant}");
                    eprintln!("variant {variant}: {} bytes of DXIL", shader.dxil.len());
                }
                ComputeInput::Refused(text) => panic!("variant {variant} refused: {text}"),
                ComputeInput::Malformed(text) => panic!("variant {variant} malformed: {text}"),
                _ => panic!("variant {variant} not translated"),
            }
        }
    }

    #[test]
    fn the_lane_exchanges_translate() {
        for (variant, threads) in [(13u32, [32u32, 1, 1]), (14, [64, 1, 1])] {
            match translate_compute_container(&synthetic_container(variant).unwrap()) {
                ComputeInput::Translated(shader, t) => {
                    assert_eq!(t, threads, "variant {variant}");
                    assert_eq!(shader.user_data_dwords(), 8, "variant {variant}: {:?}", shader.user_data);
                    assert!(!shader.memory && !shader.heap, "variant {variant}");
                    eprintln!("variant {variant}: {} bytes of DXIL", shader.dxil.len());
                }
                ComputeInput::Refused(text) => panic!("variant {variant} refused: {text}"),
                ComputeInput::Malformed(text) => panic!("variant {variant} malformed: {text}"),
                _ => panic!("variant {variant} not translated"),
            }
        }
    }

    #[test]
    fn the_graphics_containers_parse_back_and_translate_like_the_programs() {
        let (direct_vs, direct_ps) = translate_synthetic_draw().unwrap();
        for (variant, native, direct, stage) in [
            (9u32, synthetic_vertex_program(), &direct_vs, HwStage::Vs),
            (10, synthetic_pixel_program(), &direct_ps, HwStage::Ps),
        ] {
            let bytes = synthetic_container(variant).unwrap();
            let container = Container::parse(&bytes).unwrap();
            assert!(container.part(FourCC::ISG1).is_some() && container.part(FourCC::OSG1).is_some());
            let parsed = native_programs(container.part(FourCC::DXIL).unwrap().data).unwrap();
            assert_eq!(parsed, [native], "variant {variant}");
            match translate_stage_container(&bytes, stage) {
                ComputeInput::Translated(shader, threads) => {
                    assert_eq!(&*shader, direct, "variant {variant}");
                    assert_eq!(threads, [0, 0, 0]);
                }
                ComputeInput::Refused(text) => panic!("variant {variant} refused: {text}"),
                _ => panic!("variant {variant} not translated"),
            }
            // Asked for as another stage, it has no native program of that stage.
            let other = if stage == HwStage::Vs { HwStage::Ps } else { HwStage::Vs };
            assert!(matches!(translate_stage_container(&bytes, other), ComputeInput::NoNativeCompute));
        }
        assert!(matches!(
            translate_stage_container(&synthetic_container(11).unwrap(), HwStage::Vs),
            ComputeInput::Malformed(_)
        ));
        // A translated shader is a desktop container for every stage.
        assert!(matches!(translate_stage_container(&direct_ps.dxil, HwStage::Ps), ComputeInput::Desktop));
    }

    #[test]
    fn the_pixel_program_with_a_table_translates_with_a_table_binding() {
        match translate_stage_container(&synthetic_container(12).unwrap(), HwStage::Ps) {
            ComputeInput::Translated(shader, _) => {
                let tables = shader.user_data.iter().filter(|u| u.root == RootType::Table).count();
                assert_eq!(tables, 1, "{:?}", shader.user_data);
            }
            ComputeInput::Refused(text) => panic!("refused: {text}"),
            ComputeInput::Malformed(text) => panic!("malformed: {text}"),
            _ => panic!("not translated"),
        }
    }

    #[test]
    fn the_synthetic_draw_translates() {
        let (vs, ps) = translate_synthetic_draw().expect("translate");
        assert_eq!((vs.stage, ps.stage), (HwStage::Vs, HwStage::Ps));
        assert!(vs.user_data.is_empty(), "{:?}", vs.user_data);
        assert_eq!(ps.user_data_dwords(), 4, "{:?}", ps.user_data);
        for shader in [&vs, &ps] {
            assert!(Container::parse(&shader.dxil).unwrap().part(FourCC::DXIL).is_some());
            assert!(shader.root_cbvs.is_empty() && shader.samplers.is_empty() && !shader.memory && !shader.heap);
        }
        eprintln!("vs {} bytes, ps {} bytes", vs.dxil.len(), ps.dxil.len());
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

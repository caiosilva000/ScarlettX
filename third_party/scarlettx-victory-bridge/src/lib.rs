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

    hand_over(output, shader.dxil);
    0
}

#[cfg(test)]
mod tests {
    use super::*;

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

//! Narrow C ABI around the pinned GPL-3.0-or-later `victory-shader` crate.
//!
//! The C++ host owns all input and output buffers.  No game material is
//! written by this crate: callers decide whether and where to cache results.

use std::ffi::c_char;
use std::slice;

use victory_shader::{Container, FourCC, HwStage, container_programs};

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
    match container_programs(&container) {
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
            let mut dxil = shader.dxil;
            output.size = dxil.len();
            output.data = dxil.as_mut_ptr();
            std::mem::forget(dxil);
            0
        }
        Err(err) => {
            message(report, err);
            2
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn sx_victory_buffer_free(buffer: *mut SxVictoryByteBuffer) {
    if buffer.is_null() {
        return;
    }
    let buffer = unsafe { &mut *buffer };
    if !buffer.data.is_null() {
        // `translate` allocated exactly `size` bytes and transferred the Vec.
        unsafe { drop(Vec::from_raw_parts(buffer.data, buffer.size, buffer.size)) };
    }
    buffer.data = std::ptr::null_mut();
    buffer.size = 0;
}

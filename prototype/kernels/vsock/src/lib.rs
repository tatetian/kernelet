// SPDX-License-Identifier: MPL-2.0

//! A safe test kernel exposing a small socket lifecycle to one tenant process.
//!
//! Lifecycle calls use scalar arguments instead of Linux `sockaddr` layouts.
//! Data calls retain the `read`/`write` register convention and copy directly
//! between the tenant's translated frames and the channel ring.
#![no_std]
#![deny(unsafe_code)]

extern crate alloc;

use alloc::{sync::Arc, vec::Vec};
use ostd::{
    arch::cpu::context::UserContext,
    boot::boot_info,
    mm::{CachePolicy, FrameAllocOptions, PageFlags, PageProperty, VmIo, VmSpace, PAGE_SIZE},
    power::{poweroff, ExitCode},
    prelude::*,
    task::{disable_preempt, Task, TaskOptions},
    user::{DummyUserHooks, ReturnReason, UserMode},
    vsock::{Socket, SocketError, CAPACITY},
};

const BUFFER: usize = 0x1000_0000;

fn parameter(key: &str, default: usize) -> usize {
    boot_info()
        .kernel_cmdline
        .split_whitespace()
        .find_map(|s| s.strip_prefix(key).and_then(|v| v.parse().ok()))
        .unwrap_or(default)
}

fn map(space: &VmSpace, address: usize, contents: &[u8], size: usize) {
    let pages = FrameAllocOptions::new()
        .alloc_segment(size.div_ceil(PAGE_SIZE))
        .unwrap();
    pages.write_bytes(0, contents).unwrap();
    let guard = disable_preempt();
    let range = address..address + size.div_ceil(PAGE_SIZE) * PAGE_SIZE;
    let mut cursor = space.cursor_mut(&guard, &range).unwrap();
    for frame in pages {
        cursor.map(
            frame.into(),
            PageProperty::new_user(PageFlags::RWX, CachePolicy::Writeback),
        );
    }
}

#[ostd::main]
fn main() {
    let space = Arc::new(VmSpace::new());
    let binary = include_bytes!("../tenant");
    map(&space, 0x0040_0000, binary, binary.len());
    let bytes: Vec<u8> = (0..CAPACITY)
        .map(|i| (i.wrapping_mul(17).wrapping_add(3) % 251) as u8)
        .collect();
    map(&space, BUFFER, &bytes, CAPACITY);
    space.activate();
    Arc::new(TaskOptions::new(run).data(space).build().unwrap()).run();
}

fn run() {
    if matches!(parameter("role=", 0), 7 | 8 | 11) {
        run_api();
    }
    let task = Task::current().unwrap();
    let space = task.data().downcast_ref::<Arc<VmSpace>>().unwrap();
    let mut context = UserContext::default();
    context.set_rip(0x0040_1000);
    context.set_r12(parameter("size=", 4096));
    context.general_regs_mut().r13 = parameter("loops=", 1);
    context.general_regs_mut().r14 = parameter("role=", 0);
    context.general_regs_mut().r15 = parameter("peer=", 0);
    context.general_regs_mut().r8 = parameter("port=", 1234);
    let mut user = UserMode::new(context);
    let mut sockets: Vec<Option<Socket>> = (0..16).map(|_| None).collect();
    loop {
        assert_eq!(user.execute(&DummyUserHooks), ReturnReason::UserSyscall);
        let cx = user.context_mut();
        let nr = cx.rax();
        let fd = cx.rdi();
        if nr == 60 {
            println!("VSOCK-TENANT exit={} role={}", fd, parameter("role=", 0));
            drop(sockets);
            poweroff(if fd == 0 {
                ExitCode::Success
            } else {
                ExitCode::Failure
            });
        }
        let result: core::result::Result<usize, SocketError> = match nr {
            41 => Socket::new().and_then(|s| install(&mut sockets, s)),
            43 => match sockets.get_mut(fd).and_then(Option::as_mut) {
                Some(s) => s.accept().and_then(|s| install(&mut sockets, s)),
                None => Err(SocketError(9)),
            },
            3 => {
                if let Some(slot) = sockets.get_mut(fd) {
                    if slot.take().is_some() {
                        Ok(0)
                    } else {
                        Err(SocketError(9))
                    }
                } else {
                    Err(SocketError(9))
                }
            }
            _ => match sockets.get_mut(fd).and_then(Option::as_mut) {
                None => Err(SocketError(9)),
                Some(socket) => match nr {
                    0 if cx.rdx() <= CAPACITY => match space.writer(cx.rsi(), cx.rdx()) {
                        Ok(mut writer) => socket.buffer().read(&mut writer),
                        Err(_) => Err(SocketError(14)),
                    },
                    1 if cx.rdx() <= CAPACITY => match space.reader(cx.rsi(), cx.rdx()) {
                        Ok(mut reader) => socket.buffer().write(&mut reader),
                        Err(_) => Err(SocketError(14)),
                    },
                    42 => socket.connect(cx.rsi() as u64, cx.rdx() as u32).map(|_| {
                        if parameter("role=", 0) == 4 {
                            println!("VSOCK-TENANT recovery connected");
                        }
                        0
                    }),
                    49 => socket.bind(cx.rsi() as u32).map(|_| 0),
                    50 => socket.listen(cx.rsi() as u32).map(|_| {
                        println!("VSOCK-TENANT listening port={}", parameter("port=", 1234));
                        0
                    }),
                    48 => socket.shutdown(cx.rsi() as u32).map(|_| 0),
                    _ => Err(SocketError(22)),
                },
            },
        };
        cx.set_rax(match result {
            Ok(n) => n,
            Err(SocketError(e)) => (-(e as isize)) as usize,
        });
    }
}

fn run_api() -> ! {
    let size = parameter("size=", 4096);
    assert!((4096..=CAPACITY).contains(&size));
    let mut bytes = alloc::vec![0u8; size];
    let mut listener = Socket::new().unwrap();
    listener.bind(parameter("port=", 1234) as u32).unwrap();
    listener
        .listen(if parameter("role=", 0) == 11 { 1 } else { 8 })
        .unwrap();
    println!("VSOCK-TENANT listening port={}", parameter("port=", 1234));
    if parameter("role=", 0) == 11 {
        await_gate();
        let first = listener.accept().unwrap();
        let second = listener.accept().unwrap();
        drop(first);
        drop(second);
        drop(listener);
        println!("VSOCK-TENANT exit=0 role=11");
        poweroff(ExitCode::Success);
    }
    let mut stream = listener.accept().unwrap();
    stream.buffer().set_profiling(parameter("profile=", 0) != 0);
    if parameter("role=", 0) == 8 {
        await_gate();
    }
    for _ in 0..parameter("loops=", 1) {
        let mut done = 0;
        while done < size {
            let count = stream.buffer().read_bytes(&mut bytes[done..]).unwrap();
            assert!(count > 0);
            done += count;
        }
        done = 0;
        while done < size {
            let count = stream.buffer().write_bytes(&bytes[done..]).unwrap();
            assert!(count > 0);
            done += count;
        }
    }
    stream.shutdown(1).unwrap();
    drop(stream);
    drop(listener);
    println!("VSOCK-TENANT exit=0 role={}", parameter("role=", 0));
    poweroff(ExitCode::Success)
}

fn await_gate() {
    let mut gate = Socket::new().unwrap();
    gate.bind(4322).unwrap();
    gate.listen(1).unwrap();
    let mut signal = gate.accept().unwrap();
    assert_eq!(signal.buffer().read_bytes(&mut [0u8; 1]).unwrap(), 1);
}

fn install(
    sockets: &mut [Option<Socket>],
    mut socket: Socket,
) -> core::result::Result<usize, SocketError> {
    socket.buffer().set_profiling(parameter("profile=", 0) != 0);
    let fd = sockets
        .iter()
        .position(Option::is_none)
        .ok_or(SocketError(24))?;
    sockets[fd] = Some(socket);
    Ok(fd)
}

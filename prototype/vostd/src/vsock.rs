// SPDX-License-Identifier: MPL-2.0

//! Experimental socket channels with endovisor-owned storage.
//!
//! Safe callers get socket operations and copying readers/writers, never a
//! peer address, a shared `Arc`, or a reference into another image's heap.
//! Every operation may sleep on its carrier and asserts the existing sleeping
//! contract. In particular, dropping a socket requires a sleepable context.

use crate::{
    abi::svc,
    mm::{Fallible, FallibleVmRead, VmReader, VmWriter},
    sync::r2,
};

pub const CAPACITY: usize = 65536;
pub const HOST: u64 = 0;

const SOCKET: u32 = 0;
const BIND: u32 = 1;
const LISTEN: u32 = 2;
const CONNECT: u32 = 3;
const ACCEPT: u32 = 4;
const SHUTDOWN: u32 = 5;
const CLOSE: u32 = 6;
const RESERVE_TX: u32 = 7;
const COMMIT_TX: u32 = 8;
const RESERVE_RX: u32 = 9;
const COMMIT_RX: u32 = 10;

/// The fixed-layout experimental request, mirrored by `uapi/klet-vsock.h`.
#[repr(C)]
#[derive(Default)]
pub struct Request {
    handle: u64,
    arg0: u64,
    arg1: u64,
    addr: u64,
    len: u64,
    op: u32,
    nonblock: u32,
}

/// A positive Linux error number from the experimental broker.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SocketError(pub i32);

type Result<T> = core::result::Result<T, SocketError>;

fn call(r: &mut Request) -> Result<u64> {
    r2::assert_may_sleep("vsock");
    crate::mm::vm_space::retire::drain_if_safe();
    // SAFETY: `r` is a private request valid for the duration of the call.
    // The service resolves handles only within the current sandbox.
    let ret = unsafe { (svc().vsock)(r) };
    if ret < 0 {
        Err(SocketError(-ret as i32))
    } else {
        Ok(ret as u64)
    }
}

/// A uniquely owned socket endpoint with the usual connection lifecycle.
pub struct Socket {
    buffer: RingBuffer,
}

impl !Sync for Socket {}

impl Socket {
    /// Creates an unbound endpoint, allocating no ring until connection setup.
    pub fn new() -> Result<Self> {
        let handle = call(&mut Request {
            op: SOCKET,
            ..Default::default()
        })?;
        Ok(Self {
            buffer: RingBuffer {
                handle,
                nonblock: false,
                profile: None,
            },
        })
    }

    fn control(&mut self, op: u32, arg0: u64, arg1: u64) -> Result<u64> {
        call(&mut Request {
            handle: self.buffer.handle,
            op,
            arg0,
            arg1,
            nonblock: self.buffer.nonblock as u32,
            ..Default::default()
        })
    }

    /// Changes whether unavailable data, space, or backlog returns `EAGAIN`.
    ///
    /// Nonblocking operations still acquire a host mutex and may sleep there.
    pub fn set_nonblocking(&mut self, enabled: bool) {
        self.buffer.nonblock = enabled;
    }

    /// Binds a nonzero port within this sandbox's identity.
    pub fn bind(&mut self, port: u32) -> Result<()> {
        self.control(BIND, port as u64, 0).map(|_| ())
    }

    /// Starts listening with a bounded queue of pending connected endpoints.
    pub fn listen(&mut self, backlog: u32) -> Result<()> {
        self.control(LISTEN, backlog as u64, 0).map(|_| ())
    }

    /// Connects to an authorized listener and queues a new endpoint for accept.
    pub fn connect(&mut self, cid: u64, port: u32) -> Result<()> {
        self.control(CONNECT, cid, port as u64).map(|_| ())
    }

    /// Accepts a distinct connected endpoint, leaving this listener intact.
    pub fn accept(&mut self) -> Result<Self> {
        let handle = self.control(ACCEPT, 0, 0)?;
        Ok(Self {
            buffer: RingBuffer {
                handle,
                nonblock: false,
                profile: None,
            },
        })
    }

    /// Shuts down reads (`0`), writes (`1`), or both directions (`2`).
    pub fn shutdown(&mut self, how: u32) -> Result<()> {
        self.control(SHUTDOWN, how as u64, 0).map(|_| ())
    }

    /// Borrows the connected endpoint's stream storage for data transfer.
    pub fn buffer(&mut self) -> &mut RingBuffer {
        &mut self.buffer
    }
}

impl Drop for Socket {
    fn drop(&mut self) {
        if let Some(profiles) = self.buffer.profile.as_ref() {
            for (tx, p) in profiles.iter().enumerate() {
                if p.ops != 0 {
                    crate::console_println!(
                        "VSOCK-COPY tx={} ops={} bytes={} reserve_ns={} copy_ns={} commit_ns={}",
                        tx,
                        p.ops,
                        p.bytes,
                        p.reserve_ns,
                        p.copy_ns,
                        p.commit_ns
                    );
                }
            }
        }
        let _ = self.control(CLOSE, 0, 0);
    }
}

/// A capability for the two directional rings of one endpoint.
///
/// The endovisor owns the allocation and indexes. This vOSTD primitive reserves
/// a contiguous span, accesses its bytes directly, then publishes a prefix.
/// Exclusive borrowing serializes callers; the peer accesses only committed
/// bytes, and consumed bytes cannot be reused until the read commits.
pub struct RingBuffer {
    handle: u64,
    nonblock: bool,
    profile: Option<[CopyProfile; 2]>,
}

/// Elapsed diagnostic intervals, including preemption rather than just CPU work.
#[derive(Default, Clone, Copy)]
struct CopyProfile {
    ops: u64,
    bytes: u64,
    reserve_ns: u64,
    copy_ns: u64,
    commit_ns: u64,
}

impl !Sync for RingBuffer {}

impl RingBuffer {
    /// Enables optional diagnostic intervals for a separate profiling run.
    pub fn set_profiling(&mut self, enabled: bool) {
        self.profile = enabled.then(Default::default);
    }

    fn stamp(&self) -> u64 {
        if self.profile.is_some() {
            crate::time::now_ns()
        } else {
            0
        }
    }

    fn record(&mut self, tx: bool, done: usize, stamps: [u64; 4]) {
        if let Some(profiles) = self.profile.as_mut() {
            let p = &mut profiles[tx as usize];
            p.ops += 1;
            p.bytes += done as u64;
            p.reserve_ns += stamps[1] - stamps[0];
            p.copy_ns += stamps[2] - stamps[1];
            p.commit_ns += stamps[3] - stamps[2];
        }
    }
    fn reserve(&mut self, tx: bool, len: usize) -> Result<Request> {
        let mut r = Request {
            handle: self.handle,
            op: if tx { RESERVE_TX } else { RESERVE_RX },
            arg0: len as u64,
            nonblock: self.nonblock as u32,
            ..Default::default()
        };
        let count = call(&mut r)?;
        assert!(count <= len as u64 && count <= CAPACITY as u64);
        assert_eq!(count, r.len);
        Ok(r)
    }

    fn commit(&mut self, tx: bool, count: usize) -> Result<()> {
        call(&mut Request {
            handle: self.handle,
            op: if tx { COMMIT_TX } else { COMMIT_RX },
            arg0: count as u64,
            ..Default::default()
        })
        .map(|_| ())
    }

    /// Copies a prefix directly from a tenant or kernel reader into the ring.
    ///
    /// On a translation fault, a copied prefix is published and returned;
    /// a fault before any byte returns `EFAULT`. No staging buffer is used.
    pub fn write(&mut self, reader: &mut VmReader<'_, Fallible>) -> Result<usize> {
        let t0 = self.stamp();
        let r = self.reserve(true, reader.remain())?;
        let t1 = self.stamp();
        if r.len == 0 {
            return Ok(0);
        }
        // SAFETY: the service reserves these bytes exclusively for this
        // producer. Its endpoint pins both rings until close or until all its
        // carriers are gone, so a peer's death cannot invalidate the span.
        let mut writer = unsafe { VmWriter::from_kernel_space(r.addr as *mut u8, r.len as usize) };
        let (done, fault) = match reader.read_fallible(&mut writer) {
            Ok(n) => (n, false),
            Err((_, n)) => (n, true),
        };
        let t2 = self.stamp();
        self.commit(true, done)?;
        let t3 = self.stamp();
        self.record(true, done, [t0, t1, t2, t3]);
        if fault && done == 0 {
            Err(SocketError(14))
        } else {
            Ok(done)
        }
    }

    /// Copies committed bytes directly from the ring to a tenant or kernel writer.
    ///
    /// Bytes are reusable only after the copy finishes and its prefix commits.
    /// Buffered data drains before EOF or the peer-reset error is returned.
    pub fn read(&mut self, writer: &mut VmWriter<'_, Fallible>) -> Result<usize> {
        let t0 = self.stamp();
        let r = self.reserve(false, writer.avail())?;
        let t1 = self.stamp();
        if r.len == 0 {
            return Ok(0);
        }
        // SAFETY: the service reserves a committed prefix for this consumer;
        // the producer cannot reuse it until the commit below. The endpoint
        // pins the allocation independently of either sandbox heap.
        let mut reader =
            unsafe { VmReader::from_kernel_space(r.addr as *const u8, r.len as usize) };
        let (done, fault) = match reader.read_fallible(writer) {
            Ok(n) => (n, false),
            Err((_, n)) => (n, true),
        };
        let t2 = self.stamp();
        self.commit(false, done)?;
        let t3 = self.stamp();
        self.record(false, done, [t0, t1, t2, t3]);
        if fault && done == 0 {
            Err(SocketError(14))
        } else {
            Ok(done)
        }
    }

    /// Writes a kernel slice without exposing the backing ring address.
    pub fn write_bytes(&mut self, bytes: &[u8]) -> Result<usize> {
        self.write(&mut VmReader::from(bytes).to_fallible())
    }

    /// Reads into a kernel slice without exposing the backing ring address.
    pub fn read_bytes(&mut self, bytes: &mut [u8]) -> Result<usize> {
        self.read(&mut VmWriter::from(bytes).to_fallible())
    }
}

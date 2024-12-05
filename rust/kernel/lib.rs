// SPDX-License-Identifier: GPL-2.0

//! The `kernel` crate.
//!
//! This crate contains the kernel APIs that have been ported or wrapped for
//! usage by Rust code in the kernel and is shared by all of them.
//!
//! In other words, all the rest of the Rust code in the kernel (e.g. kernel
//! modules written in Rust) depends on [`core`], [`alloc`] and this crate.
//!
//! If you need a kernel C API that is not ported or wrapped yet here, then
//! do so first instead of bypassing this crate.

#![no_std]
#![feature(arbitrary_self_types)]
#![feature(coerce_unsized)]
#![feature(dispatch_from_dyn)]
#![feature(inline_const)]
#![feature(lint_reasons)]
#![feature(unsize)]

// Ensure conditional compilation based on the kernel configuration works;
// otherwise we may silently break things like initcall handling.
#[cfg(not(CONFIG_RUST))]
compile_error!("Missing kernel configuration for conditional compilation");

// Allow proc-macros to refer to `::kernel` inside the `kernel` crate (this crate).
extern crate self as kernel;

pub use ffi;

pub mod alloc;
#[cfg(CONFIG_BLOCK)]
pub mod block;
mod build_assert;
pub mod cred;
pub mod device;
pub mod error;
#[cfg(CONFIG_RUST_FW_LOADER_ABSTRACTIONS)]
pub mod firmware;
pub mod fs;
pub mod init;
pub mod ioctl;
pub mod jump_label;
#[cfg(CONFIG_KUNIT)]
pub mod kunit;
pub mod list;
pub mod miscdevice;
#[cfg(CONFIG_NET)]
pub mod net;
pub mod page;
pub mod pid_namespace;
pub mod prelude;
pub mod print;
pub mod rbtree;
pub mod security;
pub mod seq_file;
pub mod sizes;
mod static_assert;
#[doc(hidden)]
pub mod std_vendor;
pub mod str;
pub mod sync;
pub mod task;
pub mod time;
pub mod tracepoint;
pub mod transmute;
pub mod types;
pub mod uaccess;
pub mod workqueue;

#[doc(hidden)]
pub use bindings;
pub use macros;
pub use uapi;

#[doc(hidden)]
pub use build_error::build_error;

/// Prefix to appear before log messages printed from within the `kernel` crate.
const __LOG_PREFIX: &[u8] = b"rust_kernel\0";

/// The top level entrypoint to implementing a kernel module.
///
/// For any teardown or cleanup operations, your type may implement [`Drop`].
pub trait Module: Sized + Sync + Send {
    /// Called at module initialization time.
    ///
    /// Use this method to perform whatever setup or registration your module
    /// should do.
    ///
    /// Equivalent to the `module_init` macro in the C API.
    fn init(module: &'static ThisModule) -> error::Result<Self>;
}

/// A module that is pinned and initialised in-place.
pub trait InPlaceModule: Sync + Send {
    /// Creates an initialiser for the module.
    ///
    /// It is called when the module is loaded.
    fn init(module: &'static ThisModule) -> impl init::PinInit<Self, error::Error>;
}

impl<T: Module> InPlaceModule for T {
    fn init(module: &'static ThisModule) -> impl init::PinInit<Self, error::Error> {
        let initer = move |slot: *mut Self| {
            let m = <Self as Module>::init(module)?;

            // SAFETY: `slot` is valid for write per the contract with `pin_init_from_closure`.
            unsafe { slot.write(m) };
            Ok(())
        };

        // SAFETY: On success, `initer` always fully initialises an instance of `Self`.
        unsafe { init::pin_init_from_closure(initer) }
    }
}

/// Equivalent to `THIS_MODULE` in the C API.
///
/// C header: [`include/linux/init.h`](srctree/include/linux/init.h)
pub struct ThisModule(*mut bindings::module);

// SAFETY: `THIS_MODULE` may be used from all threads within a module.
unsafe impl Sync for ThisModule {}

impl ThisModule {
    /// Creates a [`ThisModule`] given the `THIS_MODULE` pointer.
    ///
    /// # Safety
    ///
    /// The pointer must be equal to the right `THIS_MODULE`.
    pub const unsafe fn from_ptr(ptr: *mut bindings::module) -> ThisModule {
        ThisModule(ptr)
    }

    /// Access the raw pointer for this module.
    ///
    /// It is up to the user to use it correctly.
    pub const fn as_ptr(&self) -> *mut bindings::module {
        self.0
    }
}

#[cfg(not(any(testlib, test)))]
#[panic_handler]
fn panic(info: &core::panic::PanicInfo<'_>) -> ! {
    pr_emerg!("{}\n", info);
    // SAFETY: FFI call.
    unsafe { bindings::BUG() };
}

/// Produces a pointer to an object from a pointer to one of its fields.
///
/// # Safety
///
/// The pointer passed to this macro, and the pointer returned by this macro, must both be in
/// bounds of the same allocation.
///
/// # Examples
///
/// ```
/// # use kernel::container_of;
/// struct Test {
///     a: u64,
///     b: u32,
/// }
///
/// let test = Test { a: 10, b: 20 };
/// let b_ptr = &test.b;
/// // SAFETY: The pointer points at the `b` field of a `Test`, so the resulting pointer will be
/// // in-bounds of the same allocation as `b_ptr`.
/// let test_alias = unsafe { container_of!(b_ptr, Test, b) };
/// assert!(core::ptr::eq(&test, test_alias));
/// ```
#[macro_export]
macro_rules! container_of {
    ($ptr:expr, $type:ty, $($f:tt)*) => {{
        let ptr = $ptr as *const _ as *const u8;
        let offset: usize = ::core::mem::offset_of!($type, $($f)*);
        ptr.sub(offset) as *const $type
    }}
}

/// Helper for `.rs.S` files.
#[doc(hidden)]
#[macro_export]
macro_rules! concat_literals {
    ($( $asm:literal )* ) => {
        ::core::concat!($($asm),*)
    };
}

/// Wrapper around `asm!` configured for use in the kernel.
///
/// Uses a semicolon to avoid parsing ambiguities, even though this does not match native `asm!`
/// syntax.
// For x86, `asm!` uses intel syntax by default, but we want to use at&t syntax in the kernel.
#[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
#[macro_export]
macro_rules! asm {
    ($($asm:expr),* ; $($rest:tt)*) => {
        ::core::arch::asm!( $($asm)*, options(att_syntax), $($rest)* )
    };
}

/// Wrapper around `asm!` configured for use in the kernel.
///
/// Uses a semicolon to avoid parsing ambiguities, even though this does not match native `asm!`
/// syntax.
// For non-x86 arches we just pass through to `asm!`.
#[cfg(not(any(target_arch = "x86", target_arch = "x86_64")))]
#[macro_export]
macro_rules! asm {
    ($($asm:expr),* ; $($rest:tt)*) => {
        ::core::arch::asm!( $($asm)*, $($rest)* )
    };
}

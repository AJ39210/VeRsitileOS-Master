#![no_std]

#[path = "panic/panic.rs"]
mod panic;

#[cfg(feature = "rust-mm")]
#[path = "mm/mod.rs"]
mod mm;

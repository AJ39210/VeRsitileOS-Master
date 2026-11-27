#![no_std]

#[cfg(test)]
extern crate std;

#[cfg(not(test))]
#[path = "panic/panic.rs"]
mod panic;

#[cfg(not(test))]
#[path = "security/security.rs"]
mod security;

#[cfg(feature = "rust-mm")]
#[path = "mm/mod.rs"]
mod mm;

#[cfg(test)]
#[path = "security_tests/mod.rs"]
mod security_tests;

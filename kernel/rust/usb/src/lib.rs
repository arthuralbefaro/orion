#![cfg_attr(not(test), no_std)]
#![allow(clippy::missing_safety_doc)]

extern crate alloc;

pub mod desc;
pub mod hid;

#[cfg(target_os = "none")]
mod xhci;

#[cfg(test)]
mod tests;

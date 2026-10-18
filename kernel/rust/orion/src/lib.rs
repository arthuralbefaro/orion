#![cfg_attr(not(test), no_std)]

extern crate orion_ext2;
extern crate orion_tcp;

#[cfg(not(test))]
mod kernel;

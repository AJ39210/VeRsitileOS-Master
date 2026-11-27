use core::slice;

#[path = "../../../security/crypto/ed25519.rs"]
mod ed25519;
#[path = "../../../security/cert/module_signature.rs"]
mod module_signature;
#[path = "../../../security/hash/sha256.rs"]
mod sha256;

const TRUSTED_MODULE_PUBLIC_KEY: &[u8; 32] =
    include_bytes!("../../../security/cert/module_signing_public_key.bin");

#[no_mangle]
pub extern "C" fn security_panic_write_protection_fault(
    fault_address: u64,
    error_code: u64,
    instruction_pointer: u64,
) -> ! {
    crate::panic::security_write_protection_panic(fault_address, error_code, instruction_pointer)
}

#[no_mangle]
pub unsafe extern "C" fn security_verify_signed_module(
    file_data: *const u8,
    file_size: u64,
    payload_size: *mut u64,
) -> bool {
    if file_data.is_null()
        || payload_size.is_null()
        || file_size > usize::MAX as u64
        || file_size == 0
    {
        return false;
    }

    let data = slice::from_raw_parts(file_data, file_size as usize);
    let Some(payload) =
        module_signature::verified_payload(data, TRUSTED_MODULE_PUBLIC_KEY, &sha256::digest)
    else {
        return false;
    };

    payload_size.write(payload.len() as u64);
    true
}

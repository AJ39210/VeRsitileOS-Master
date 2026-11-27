use super::ed25519;

pub const TRAILER_MAGIC: &[u8; 8] = b"VOSMOD01";
const SIGNATURE_SIZE: usize = 64;
const TRAILER_SIZE: usize = TRAILER_MAGIC.len() + SIGNATURE_SIZE;
const SIGNING_DOMAIN: &[u8] = b"VeRsitileOS kernel module signature v1\0";

pub fn verified_payload<'a, F>(
    signed_module: &'a [u8],
    public_key: &[u8; 32],
    hash: F,
) -> Option<&'a [u8]>
where
    F: Fn(&[u8]) -> [u8; 32],
{
    if signed_module.len() < TRAILER_SIZE {
        return None;
    }

    let payload_size = signed_module.len().checked_sub(TRAILER_SIZE)?;
    let trailer = signed_module.get(payload_size..)?;
    if trailer.get(..TRAILER_MAGIC.len())? != TRAILER_MAGIC {
        return None;
    }

    let signature: &[u8; SIGNATURE_SIZE] = trailer.get(TRAILER_MAGIC.len()..)?.try_into().ok()?;
    let payload = signed_module.get(..payload_size)?;
    let digest = hash(payload);
    let mut message = [0; SIGNING_DOMAIN.len() + 8 + 32];
    let domain_end = SIGNING_DOMAIN.len();
    message[..domain_end].copy_from_slice(SIGNING_DOMAIN);
    message[domain_end..domain_end + 8].copy_from_slice(&(payload.len() as u64).to_le_bytes());
    message[domain_end + 8..].copy_from_slice(&digest);

    if !ed25519::verify(public_key, &message, signature) {
        return None;
    }

    Some(payload)
}

#[cfg(test)]
mod tests {
    use super::{verified_payload, TRAILER_MAGIC};
    use std::vec::Vec;

    fn decode_hex<const N: usize>(input: &str) -> [u8; N] {
        let bytes = input.as_bytes();
        assert_eq!(bytes.len(), N * 2);
        let mut output = [0; N];
        for (index, byte) in output.iter_mut().enumerate() {
            let high = (bytes[index * 2] as char).to_digit(16).unwrap();
            let low = (bytes[index * 2 + 1] as char).to_digit(16).unwrap();
            *byte = ((high << 4) | low) as u8;
        }
        output
    }

    #[test]
    fn rejects_unsigned_and_truncated_module_trailers() {
        let key = [0; 32];
        assert!(verified_payload(b"plain elf", &key, super::super::sha256::digest).is_none());
        assert!(verified_payload(TRAILER_MAGIC, &key, super::super::sha256::digest).is_none());
    }

    #[test]
    fn verifies_signed_payload_and_rejects_tampering() {
        let public_key: [u8; 32] =
            decode_hex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
        let signature: [u8; 64] = decode_hex(concat!(
            "01a365d22bb4dc38dd16dfb985eac40c1a9c6da7c54e211fbbb455420e5daf36",
            "d5d4149439d70f44698a94cb0aac7e4dcdde2e5b26c08ee344f433b20e4fba00"
        ));
        let payload = b"abc";
        let mut signed_module = Vec::from(payload.as_slice());
        signed_module.extend_from_slice(TRAILER_MAGIC);
        signed_module.extend_from_slice(&signature);

        assert_eq!(
            verified_payload(&signed_module, &public_key, super::super::sha256::digest),
            Some(payload.as_slice())
        );

        signed_module[0] ^= 1;
        assert!(
            verified_payload(&signed_module, &public_key, super::super::sha256::digest).is_none()
        );
    }
}

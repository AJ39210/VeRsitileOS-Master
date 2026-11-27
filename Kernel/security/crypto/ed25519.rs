use ed25519_dalek::{Signature, VerifyingKey};

pub fn verify(public_key: &[u8; 32], message: &[u8], signature: &[u8; 64]) -> bool {
    let Ok(key) = VerifyingKey::from_bytes(public_key) else {
        return false;
    };
    key.verify_strict(message, &Signature::from_bytes(signature))
        .is_ok()
}

#[cfg(test)]
mod tests {
    use super::verify;

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
    fn verifies_rfc8032_vector_and_rejects_modified_message() {
        let public_key =
            decode_hex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
        let signature = decode_hex(concat!(
            "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155",
            "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"
        ));

        assert!(verify(&public_key, b"", &signature));
        assert!(!verify(&public_key, b"tampered", &signature));
    }
}

use sha2::{Digest, Sha256};

pub fn digest(data: &[u8]) -> [u8; 32] {
    let digest = Sha256::digest(data);
    let mut result = [0; 32];
    result.copy_from_slice(&digest);
    result
}

#[cfg(test)]
mod tests {
    use super::digest;

    fn decode_hex(input: &str, output: &mut [u8]) {
        let bytes = input.as_bytes();
        assert_eq!(bytes.len(), output.len() * 2);
        for (index, byte) in output.iter_mut().enumerate() {
            let high = (bytes[index * 2] as char).to_digit(16).unwrap();
            let low = (bytes[index * 2 + 1] as char).to_digit(16).unwrap();
            *byte = ((high << 4) | low) as u8;
        }
    }

    #[test]
    fn sha256_known_vectors() {
        let mut expected = [0; 32];
        decode_hex(
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            &mut expected,
        );
        assert_eq!(digest(b""), expected);

        decode_hex(
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            &mut expected,
        );
        assert_eq!(digest(b"abc"), expected);
    }
}

const std = @import("std");

const crypto = std.crypto;

pub const Error = error{
    invalidKeyLength,
    invalidNonceLength,
    DecryptionFailed,
};

pub fn encrypt(
    io: std.Io,
    key: []const u8,
    plaintext: []const u8,
    out_nonce: *[12]u8,
    out_tag: *[16]u8,
    out_ciphertext: []u8,
) Error!void {
    if (key.len != 32) {
        return Error.invalidKeyLength;
    }

    if (out_ciphertext.len != plaintext.len) {
        return Error.DecryptionFailed;
    }

    // Zig 0.16: obtain cryptographically secure entropy
    // through the Io interface.
    io.randomSecure(out_nonce[0..12]) catch {
        return Error.DecryptionFailed;
    };

    var key_array: [32]u8 = undefined;
    @memcpy(&key_array, key);

    crypto.aead.chacha_poly.ChaCha12Poly1305.encrypt(
        out_ciphertext,
        out_tag,
        plaintext,
        "",
        out_nonce.*,
        key_array,
    );
}

pub fn decrypt(
    key: []const u8,
    nonce: []const u8,
    tag: []const u8,
    ciphertext: []const u8,
    out_plaintext: []u8,
) Error!void {
    if (key.len != 32) {
        return Error.invalidKeyLength;
    }

    if (nonce.len != 12) {
        return Error.invalidNonceLength;
    }

    if (tag.len != 16) {
        return Error.DecryptionFailed;
    }

    if (out_plaintext.len != ciphertext.len) {
        return Error.DecryptionFailed;
    }

    var key_array: [32]u8 = undefined;
    var nonce_array: [12]u8 = undefined;
    var tag_array: [16]u8 = undefined;

    @memcpy(&key_array, key);
    @memcpy(&nonce_array, nonce);
    @memcpy(&tag_array, tag);

    crypto.aead.chacha_poly.ChaCha12Poly1305.decrypt(
        out_plaintext,
        ciphertext,
        tag_array,
        "",
        nonce_array,
        key_array,
    ) catch {
        return Error.DecryptionFailed;
    };
}

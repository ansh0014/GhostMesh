const std = @import("std");
const aead = @import("aead.zig");
const memory = @import("memory.zig");

/// Exported C-ABI Encrypt Function
/// Returns 0 on success, -1 on failure
export fn aether_encrypt(
    key: [*]const u8,
    plaintext: [*]const u8,
    len: usize,
    out_nonce: [*]u8,
    out_tag: [*]u8,
    out_ciphertext: [*]u8,
) c_int {
    const key_slice = key[0..32];
    const plaintext_slice = plaintext[0..len];
    const out_ciphertext_slice = out_ciphertext[0..len];

    var nonce_buf: [12]u8 = undefined;
    var tag_buf: [16]u8 = undefined;

    aead.encrypt(
        key_slice,
        plaintext_slice,
        &nonce_buf,
        &tag_buf,
        out_ciphertext_slice,
    ) catch {
        return -1;
    };

    @memcpy(out_nonce[0..12], &nonce_buf);
    @memcpy(out_tag[0..16], &tag_buf);

    return 0;
}

/// Exported C-ABI Decrypt Function
/// Returns 0 on success, -1 on failure (failed tag verification)
export fn aether_decrypt(
    key: [*]const u8,
    nonce: [*]const u8,
    tag: [*]const u8,
    ciphertext: [*]const u8,
    len: usize,
    out_plaintext: [*]u8,
) c_int {
    const key_slice = key[0..32];
    const nonce_slice = nonce[0..12];
    const tag_slice = tag[0..16];
    const ciphertext_slice = ciphertext[0..len];
    const out_plaintext_slice = out_plaintext[0..len];

    aead.decrypt(
        key_slice,
        nonce_slice,
        tag_slice,
        ciphertext_slice,
        out_plaintext_slice,
    ) catch {
        return -1;
    };

    return 0;
}

/// Exported C-ABI Secure Zero Function
export fn aether_secure_zero(ptr: ?*anyopaque, len: usize) void {
    if (ptr) |p| {
        const u8_ptr: [*]u8 = @ptrCast(p);
        memory.secureZero(u8_ptr, len);
    }
}

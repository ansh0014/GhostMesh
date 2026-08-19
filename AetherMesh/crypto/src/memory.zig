const std = @import("std");

// secure zero out a block of memory.
// we uses to volatile pointer operations to guarantee that the compiler will not
// we optimize away the zeroing operation, even if the memory is about to be freed
pub fn secureZero(ptr: [*]u8, len: usize) void {
    var i: usize = 0;
    while (i < len) : (i += 1) {
        const volatile_ptr: *volatile u8 = @ptrCast(&ptr[i]);
        volatile_ptr.* = 0;
    }
}

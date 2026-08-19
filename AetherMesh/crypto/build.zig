const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const lib = b.addSharedLibrary(.{
        .name = "aethercrypto",
        .root_source_file = b.path("src/lib.zig"),
        .target = target,
        .optimize = optimize,
        .version = .{ .major = 1, .minor = 0, .patch = 0 },
    });

    b.installArtifact(lib);
}

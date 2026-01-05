const std = @import("std");

const FILE_PATH = "/root/code/1brc/data/measurements.txt";

const LocationStat = struct {
    min: i32,
    max: i32,
    freq: usize,
    sum: i64,
};

fn printResults(m: *std.StringHashMap(LocationStat), arena_alloc: std.mem.Allocator) !void {
    var keys_arr = try std.ArrayList([]const u8).initCapacity(arena_alloc, 2 * 1024 * 1024);
    var keyIter = m.keyIterator();
    while (keyIter.next()) |location_slice| {
        try keys_arr.append(arena_alloc, location_slice.*);
    }

    std.mem.sort([]const u8, keys_arr.items, {}, struct {
        fn lessThan(_: void, a: []const u8, b: []const u8) bool {
            return std.mem.order(u8, a, b) == .lt;
        }
    }.lessThan);

    var stdout_buffer: [2 * 1024 * 1024]u8 = undefined;
    var stdout_writer = std.fs.File.stdout().writer(&stdout_buffer);
    const stdout = &stdout_writer.interface;

    var first = true;
    try stdout.writeAll("{");
    for (keys_arr.items) |location| {
        const stat = m.get(location).?;
        const mean = @as(f64, @floatFromInt(stat.sum)) / @as(f64, @floatFromInt(stat.freq));

        if (!first) {
            try stdout.writeAll(", ");
        }
        first = false;

        try stdout.print("{s}={d:.1}/{d:.1}/{d:.1}", .{
            location,
            @as(f64, @floatFromInt(stat.min)) / 10.0,
            mean / 10.0,
            @as(f64, @floatFromInt(stat.max)) / 10.0,
        });
    }
    try stdout.writeAll("}\n");
    try stdout.flush();
}

fn parseLine(line: []const u8) !struct { []const u8, i32 } {
    const semicol_pos = std.mem.indexOfScalar(u8, line, ';').?;
    const temperature = try std.fmt.parseFloat(f32, line[semicol_pos + 1 ..]);
    return .{ line[0..semicol_pos], @as(i32, @intFromFloat(temperature * 10.0)) };
}

fn updateMap(location_slice: []const u8, temperature: i32, m: *std.StringHashMap(LocationStat), arena_alloc: std.mem.Allocator) !void {
    const res = try m.getOrPut(location_slice);
    if (!res.found_existing) {
        const owned_ptr = try arena_alloc.dupe(u8, location_slice);
        res.key_ptr.* = owned_ptr;
        res.value_ptr.* = LocationStat{
            .min = temperature,
            .max = temperature,
            .freq = 1,
            .sum = temperature,
        };
    } else {
        res.value_ptr.min = @min(res.value_ptr.min, temperature);
        res.value_ptr.max = @max(res.value_ptr.max, temperature);
        res.value_ptr.sum += temperature;
        res.value_ptr.freq += 1;
    }
}

fn onebrc(file: std.fs.File, arena_alloc: std.mem.Allocator) !void {
    var file_buffer: [8192]u8 = undefined;
    var reader = file.reader(&file_buffer);

    var m = std.StringHashMap(LocationStat).init(arena_alloc);

    while (true) {
        const line = reader.interface.takeDelimiterExclusive('\n') catch |read_err| {
            if (read_err == error.EndOfStream) {
                break;
            }
            return read_err;
        };

        if (line.len == 0) continue;

        const location_slice, const temperature = try parseLine(line);
        try updateMap(location_slice, temperature, &m, arena_alloc);
    }

    try printResults(&m, arena_alloc);
}

pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    const allocator = gpa.allocator();

    var arena = std.heap.ArenaAllocator.init(allocator);
    defer arena.deinit();
    const arena_allocator = arena.allocator();

    const args = try std.process.argsAlloc(allocator);
    defer std.process.argsFree(allocator, args);

    var file_path: []const u8 = undefined;

    if (args.len < 2) {
        file_path = FILE_PATH;
    } else {
        file_path = args[1];
    }

    const file = try std.fs.cwd().openFile(file_path, .{});
    defer file.close();

    try onebrc(file, arena_allocator);
}

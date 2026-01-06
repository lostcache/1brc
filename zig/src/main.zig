const std = @import("std");

const FILE_PATH = "/root/code/1brc/data/measurements.txt";
const NUM_THREADS = 8;

const LocationStat = struct {
    min: i32,
    max: i32,
    freq: usize,
    sum: i64,
};

fn printResults(m: *const std.StringHashMap(LocationStat), allocator: std.mem.Allocator) !void {
    var keys = std.ArrayList([]const u8){};
    defer keys.deinit(allocator);

    var key_iter = m.keyIterator();
    while (key_iter.next()) |key| {
        try keys.append(allocator, key.*);
    }

    std.mem.sort([]const u8, keys.items, {}, struct {
        fn lessThan(_: void, a: []const u8, b: []const u8) bool {
            return std.mem.order(u8, a, b) == .lt;
        }
    }.lessThan);

    var stdout_buf: [2 * 1024 * 1024]u8 = undefined;
    var stdout_writer = std.fs.File.stdout().writer(&stdout_buf);
    const writer = &stdout_writer.interface;

    try writer.writeAll("{");
    for (keys.items, 0..) |location, i| {
        const stat = m.get(location).?;
        const mean = @as(f64, @floatFromInt(stat.sum)) / @as(f64, @floatFromInt(stat.freq));

        if (i > 0) try writer.writeAll(", ");

        try writer.print("{s}={d:.1}/{d:.1}/{d:.1}", .{
            location,
            @as(f64, @floatFromInt(stat.min)) / 10.0,
            mean / 10.0,
            @as(f64, @floatFromInt(stat.max)) / 10.0,
        });
    }
    try writer.writeAll("}\n");
    try writer.flush();
}

fn parseLine(line: []const u8) !struct { []const u8, i32 } {
    const semicol_pos = std.mem.indexOfScalar(u8, line, ';').?;
    const temperature = try std.fmt.parseFloat(f32, line[semicol_pos + 1 ..]);
    return .{ line[0..semicol_pos], @as(i32, @intFromFloat(temperature * 10.0)) };
}

fn updateMap(location: []const u8, temperature: i32, m: *std.StringHashMap(LocationStat), allocator: std.mem.Allocator) !void {
    const entry = try m.getOrPut(location);
    if (!entry.found_existing) {
        entry.key_ptr.* = try allocator.dupe(u8, location);
        entry.value_ptr.* = .{
            .min = temperature,
            .max = temperature,
            .freq = 1,
            .sum = temperature,
        };
    } else {
        const stat = entry.value_ptr;
        stat.min = @min(stat.min, temperature);
        stat.max = @max(stat.max, temperature);
        stat.sum += temperature;
        stat.freq += 1;
    }
}

fn skipToNextLine(file: std.fs.File, start_pos: usize) !u64 {
    if (start_pos == 0) return 0;

    var buf: [1]u8 = undefined;
    var reader = file.reader(&buf);
    try reader.seekTo(start_pos - 1);
    const prev_char = try reader.interface.takeByte();
    if (prev_char == '\n') return 0;

    try reader.seekTo(start_pos);
    var bytes_skipped: u64 = 0;
    while (true) {
        const char = try reader.interface.takeByte();
        bytes_skipped += 1;
        if (char == '\n') break;
    }

    return bytes_skipped;
}

fn processBatch(
    thread_idx: usize,
    start_pos: usize,
    batch_size: u64,
    m: *std.StringHashMap(LocationStat),
    file: std.fs.File,
    arena_alloc: std.mem.Allocator,
) !void {
    var processed_bytes: u64 = 0;
    if (thread_idx > 0) {
        processed_bytes = try skipToNextLine(file, start_pos);
    }

    var file_buffer: [8192]u8 = undefined;
    var reader = file.reader(&file_buffer);
    try reader.seekTo(start_pos + processed_bytes);

    while (processed_bytes < batch_size) {
        const line = reader.interface.takeDelimiterExclusive('\n') catch |read_err| {
            if (read_err == error.EndOfStream) {
                break;
            }
            return read_err;
        };

        const location_slice, const temperature = try parseLine(line);
        try updateMap(location_slice, temperature, m, arena_alloc);

        processed_bytes += line.len + 1;
    }
}

fn accumulateBatchResults(m: *std.StringHashMap(LocationStat), batch_results: *[NUM_THREADS]std.StringHashMap(LocationStat)) !void {
    for (batch_results) |batch_map| {
        var key_iter = batch_map.keyIterator();
        while (key_iter.next()) |location_slice| {
            const result = try m.getOrPut(location_slice.*);
            if (!result.found_existing) {
                result.key_ptr.* = location_slice.*;
                result.value_ptr.* = batch_map.get(location_slice.*).?;
            } else {
                const batch_stat = batch_map.get(location_slice.*).?;
                result.value_ptr.min = @min(result.value_ptr.min, batch_stat.min);
                result.value_ptr.max = @max(result.value_ptr.max, batch_stat.max);
                result.value_ptr.sum += batch_stat.sum;
                result.value_ptr.freq += batch_stat.freq;
            }
        }
    }
}

fn processParallelInMultipleBatches(file_size: u64, file_path: []const u8) !void {
    const batch_size = (file_size + NUM_THREADS - 1) / NUM_THREADS;
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    const gpa_alloc = gpa.allocator();

    var handles: [NUM_THREADS]std.Thread = undefined;
    var m: [NUM_THREADS]std.StringHashMap(LocationStat) = undefined;
    var arenas: [NUM_THREADS]std.heap.ArenaAllocator = undefined;
    var files: [NUM_THREADS]std.fs.File = undefined;

    for (0..NUM_THREADS) |i| {
        arenas[i] = std.heap.ArenaAllocator.init(gpa_alloc);
        m[i] = std.StringHashMap(LocationStat).init(arenas[i].allocator());
        files[i] = try std.fs.cwd().openFile(file_path, .{});
    }

    defer {
        for (0..NUM_THREADS) |i| {
            m[i].deinit();
            arenas[i].deinit();
            files[i].close();
        }
    }

    for (0..NUM_THREADS) |i| {
        const start_pos = i * batch_size;
        handles[i] = try std.Thread.spawn(.{}, processBatch, .{ i, @as(usize, start_pos), batch_size, &m[i], files[i], arenas[i].allocator() });
    }

    for (0..NUM_THREADS) |i| {
        handles[i].join();
    }

    var final_arena = std.heap.ArenaAllocator.init(gpa_alloc);
    defer final_arena.deinit();
    var final_map = std.StringHashMap(LocationStat).init(final_arena.allocator());
    defer final_map.deinit();

    try accumulateBatchResults(&final_map, &m);
    try printResults(&final_map, final_arena.allocator());
}

fn processInSingleBatch(batch_size: u64, file_path: []const u8) !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    const gpa_alloc = gpa.allocator();
    var arena = std.heap.ArenaAllocator.init(gpa_alloc);
    defer arena.deinit();
    const arena_alloc = arena.allocator();

    var m = std.StringHashMap(LocationStat).init(arena_alloc);

    const file = try std.fs.cwd().openFile(file_path, .{});
    defer file.close();

    try processBatch(0, 0, batch_size, &m, file, arena_alloc);
    try printResults(&m, arena_alloc);
}

fn onebrc(file_path: []const u8) !void {
    const file = try std.fs.cwd().openFile(file_path, .{});
    defer file.close();
    const file_size = (try file.stat()).size;

    if (file_size > 4 * 1024) {
        try processParallelInMultipleBatches(file_size, file_path);
    } else {
        try processInSingleBatch(file_size, file_path);
    }
}

pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    const allocator = gpa.allocator();

    const args = try std.process.argsAlloc(allocator);
    defer std.process.argsFree(allocator, args);

    const file_path = if (args.len < 2) FILE_PATH else args[1];

    try onebrc(file_path);
}

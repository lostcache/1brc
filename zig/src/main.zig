const std = @import("std");

const FILE_PATH = "/root/code/1brc/data/measurements.txt";
const NUM_THREADS = 8;

const LocationStat = struct {
    min: i32,
    max: i32,
    sum: i64,
    freq: u32,
    key: []const u8,
};

const FastMap = struct {
    const size: usize = 1 << 14;
    const mask: usize = @This().size - 1;
    entries: []LocationStat,
    allocator: std.mem.Allocator,

    fn init(allocator: std.mem.Allocator) !FastMap {
        const entries = try allocator.alloc(LocationStat, @This().size);
        for (0..@This().size) |i| {
            entries[i] = .{
                .min = std.math.maxInt(i32),
                .max = std.math.minInt(i32),
                .sum = 0,
                .freq = 0,
                .key = undefined,
            };
        }
        return FastMap{ .entries = entries, .allocator = allocator };
    }

    fn deinit(self: *FastMap) void {
        self.allocator.free(self.entries);
    }

    inline fn fast_hash(key: []const u8) u64 {
        var hash: u64 = 0;
        for (0..key.len) |index|
            hash = hash * @as(u64, 1315423911) + @as(u64, key[index]);
        return hash;
    }

    fn putOrUpdate(self: *FastMap, key: []const u8, temp: i32) void {
        var idx = fast_hash(key) & @This().mask;

        while (true) {
            var entry = &self.entries[idx];

            if (entry.freq == 0) {
                entry.* = .{
                    .min = temp,
                    .max = temp,
                    .sum = temp,
                    .freq = 1,
                    .key = key,
                };
                return;
            }

            if (std.mem.eql(u8, entry.key, key)) {
                entry.min = @min(entry.min, temp);
                entry.max = @max(entry.max, temp);
                entry.sum += temp;
                entry.freq += 1;
                return;
            }

            idx = (idx + 1) & @This().mask;
        }
    }

    fn putOrUpdateEntry(self: *FastMap, stat: *LocationStat) void {
        var idx = fast_hash(stat.key) & @This().mask;

        while (true) {
            var entry = &self.entries[idx];

            if (entry.freq == 0) {
                entry.* = stat.*;
                return;
            }

            if (std.mem.eql(u8, entry.key, stat.key)) {
                entry.min = @min(entry.min, stat.min);
                entry.max = @max(entry.max, stat.max);
                entry.sum += stat.sum;
                entry.freq += stat.freq;
                return;
            }

            idx = (idx + 1) & @This().mask;
        }
    }
};
fn printResults(m: *const FastMap) !void {
    std.mem.sort(LocationStat, m.entries, {}, struct {
        fn lessThan(_: void, a: LocationStat, b: LocationStat) bool {
            // Sort non-empty entries before empty ones, then by key
            if (a.freq == 0 and b.freq == 0) return false;
            if (a.freq == 0) return false;
            if (b.freq == 0) return true;
            return std.mem.order(u8, a.key, b.key) == .lt;
        }
    }.lessThan);

    var stdout_buf: [2 * 1024 * 1024]u8 = undefined;
    var stdout_writer = std.fs.File.stdout().writer(&stdout_buf);
    const writer = &stdout_writer.interface;

    try writer.writeAll("{");
    var first = true;
    for (m.entries) |stat| {
        if (stat.freq == 0) break; // All remaining entries are empty

        const mean = @as(f64, @floatFromInt(stat.sum)) / @as(f64, @floatFromInt(stat.freq));

        if (!first) try writer.writeAll(", ");
        first = false;

        try writer.print("{s}={d:.1}/{d:.1}/{d:.1}", .{
            stat.key,
            @as(f64, @floatFromInt(stat.min)) / 10.0,
            mean / 10.0,
            @as(f64, @floatFromInt(stat.max)) / 10.0,
        });
    }
    try writer.writeAll("}\n");
    try writer.flush();
}

fn parse_int(val_slice: []const u8) i32 {
    var is_negative = false;
    var start_pos: usize = 0;
    var val: i32 = undefined;

    if (val_slice[0] == '-') {
        is_negative = true;
        start_pos = 1;
    }

    if (val_slice[start_pos + 1] == '.') {
        val = @as(i32, val_slice[start_pos] - '0') * 10 + @as(i32, val_slice[start_pos + 2] - '0');
    } else {
        val = @as(i32, val_slice[start_pos] - '0') * 100 + @as(i32, val_slice[start_pos + 1] - '0') * 10 + @as(i32, val_slice[start_pos + 3] - '0');
    }

    return if (is_negative) -val else val;
}

fn parseLine(line: []const u8) !struct { []const u8, i32 } {
    const semicol_pos = std.mem.indexOfScalar(u8, line, ';').?;
    const temperature = parse_int(line[semicol_pos + 1 ..]);
    return .{ line[0..semicol_pos], temperature };
}

fn updateMap(location: []const u8, temperature: i32, m: *FastMap, allocator: std.mem.Allocator) !void {
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

fn skipToNextLine(mmap_file: []const u8, start_pos: usize) !u64 {
    if (start_pos == 0) return 0;

    const prev_char = mmap_file[start_pos - 1];
    if (prev_char == '\n') return 0;

    var bytes_skipped: u64 = 0;
    while (true) {
        const char = mmap_file[start_pos + @as(usize, bytes_skipped)];
        bytes_skipped += 1;
        if (char == '\n') break;
    }

    return bytes_skipped;
}

fn processBatch(
    thread_idx: usize,
    start_pos: usize,
    batch_size: u64,
    m: *FastMap,
    mmap_file: []const u8,
) !void {
    var processed_bytes: u64 = 0;
    if (thread_idx > 0) {
        processed_bytes = try skipToNextLine(mmap_file, start_pos);
    }

    while (processed_bytes < batch_size) {
        const current_pos = start_pos + @as(usize, processed_bytes);

        if (current_pos >= mmap_file.len) break; // last batch may exceed file size

        const relative_line_end_pos = std.mem.indexOfScalar(
            u8,
            mmap_file[current_pos..],
            '\n',
        ) orelse (mmap_file.len - current_pos); // Last line without newline

        const line = mmap_file[current_pos .. current_pos + relative_line_end_pos];

        const location_slice, const temperature = try parseLine(line);
        m.putOrUpdate(location_slice, temperature);

        processed_bytes += line.len + 1;
    }
}

fn accumulateBatchResults(m: *FastMap, batch_results: *[NUM_THREADS]FastMap) void {
    for (batch_results) |batch_map| {
        for (batch_map.entries) |*entry| {
            if (entry.freq == 0) continue; // Skip empty entries
            m.putOrUpdateEntry(entry);
        }
    }
}

fn processParallelInMultipleBatches(file_size: u64, mmap_file: []const u8) !void {
    const batch_size = (file_size + NUM_THREADS - 1) / NUM_THREADS;
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    const gpa_alloc = gpa.allocator();

    var thread_handles: [NUM_THREADS]std.Thread = undefined;
    var m: [NUM_THREADS]FastMap = undefined;
    var arenas: [NUM_THREADS]std.heap.ArenaAllocator = undefined;

    for (0..NUM_THREADS) |i| {
        arenas[i] = std.heap.ArenaAllocator.init(gpa_alloc);
        m[i] = try FastMap.init(arenas[i].allocator());
    }

    defer {
        for (0..NUM_THREADS) |i| {
            m[i].deinit();
            arenas[i].deinit();
        }
    }

    for (0..NUM_THREADS) |i| {
        const start_pos = i * batch_size;
        thread_handles[i] = try std.Thread.spawn(
            .{},
            processBatch,
            .{ i, @as(usize, start_pos), batch_size, &m[i], mmap_file },
        );
    }

    for (0..NUM_THREADS) |i| {
        thread_handles[i].join();
    }

    var final_arena = std.heap.ArenaAllocator.init(gpa_alloc);
    defer final_arena.deinit();
    var final_map = try FastMap.init(final_arena.allocator());
    defer final_map.deinit();

    accumulateBatchResults(&final_map, &m);
    try printResults(&final_map);
}

fn processInSingleBatch(batch_size: u64, mmap_file: []const u8) !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    const gpa_alloc = gpa.allocator();
    var arena = std.heap.ArenaAllocator.init(gpa_alloc);
    defer arena.deinit();
    const arena_alloc = arena.allocator();
    var m = try FastMap.init(arena_alloc);

    try processBatch(0, 0, batch_size, &m, mmap_file);
    try printResults(&m);
}

fn onebrc(file_path: []const u8) !void {
    const file = try std.fs.cwd().openFile(file_path, .{});
    defer file.close();
    const file_size = (try file.stat()).size;

    const mmap_file = try std.posix.mmap(null, file_size, std.posix.PROT.READ, .{ .TYPE = .PRIVATE }, file.handle, 0);

    if (file_size > 4 * 1024) {
        try processParallelInMultipleBatches(file_size, mmap_file);
    } else {
        try processInSingleBatch(file_size, mmap_file);
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

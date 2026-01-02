// ~95.536s

use std::{
    io::{BufRead, Read, Seek},
    thread::JoinHandle,
};

const NUM_THREADS: usize = 8;

struct LocationStats {
    min: i32,
    max: i32,
    sum: i64,
    freq: usize,
}

fn get_file_path() -> std::path::PathBuf {
    let args: std::vec::Vec<String> = std::env::args().collect();
    match args.len() {
        2 => std::path::PathBuf::from(&args[1]),
        _ => std::path::PathBuf::from("../data/measurements.txt"),
    }
}

fn print_result(
    m: &std::collections::BTreeMap<String, LocationStats>,
) -> Result<(), std::io::Error> {
    print!("{{");
    let mut it = m.iter().peekable();
    while let Some((location, stat)) = it.next() {
        let mut avg = stat.sum as f64 / stat.freq as f64 / 10.0;
        avg = (avg * 10.0).round() / 10.0;
        print!(
            "{}={:.1}/{:.1}/{:.1}",
            location,
            stat.min as f64 / 10.0,
            avg,
            stat.max as f64 / 10.0
        );

        if let Some(_) = it.peek() {
            print!(", ");
        }
    }
    print!("}}");

    Ok(())
}

fn parse_line(line: &str) -> Option<(String, i32)> {
    let line = line.trim();
    if line.is_empty() {
        return None;
    }

    let semicolon_pos = line.find(';')?;
    let location = &line[..semicolon_pos];
    let temp_str = &line[semicolon_pos + 1..];

    if location.is_empty() || temp_str.is_empty() {
        return None;
    }

    // Parse temperature from bytes
    let temperature: f64 = temp_str.parse().ok()?;
    let temp_int = (temperature * 10.0).round() as i32;

    // Convert location bytes to String
    let location_str = location.to_string();

    Some((location_str, temp_int))
}

fn update_map(
    main_map: &mut std::collections::BTreeMap<String, LocationStats>,
    batch_map: std::collections::BTreeMap<String, LocationStats>,
) {
    for (location, stats) in batch_map {
        let entry = main_map.entry(location).or_insert(LocationStats {
            min: std::i32::MAX,
            max: std::i32::MIN,
            sum: 0,
            freq: 0,
        });

        entry.min = std::cmp::min(entry.min, stats.min);
        entry.max = std::cmp::max(entry.max, stats.max);
        entry.sum += stats.sum;
        entry.freq += stats.freq;
    }
}

fn update_stats(
    m: &mut std::collections::BTreeMap<String, LocationStats>,
    location: String,
    temperature: i32,
) {
    let entry = m.entry(location).or_insert(LocationStats {
        min: std::i32::MAX,
        max: std::i32::MIN,
        sum: 0,
        freq: 0,
    });
    entry.min = std::cmp::min(entry.min, temperature);
    entry.max = std::cmp::max(entry.max, temperature);
    entry.sum += temperature as i64;
    entry.freq += 1;
}

fn skip_first_line(start: u64, fp: &std::path::Path) -> u64 {
    assert!(start > 0);

    let mut skipped_bytes = 0u64;

    let mut f = std::fs::File::open(fp).unwrap();

    f.seek(std::io::SeekFrom::Start(start - 1)).unwrap();
    let mut prev_char = [0u8; 1];
    f.read_exact(&mut prev_char).unwrap();

    if prev_char[0] != b'\n' {
        let mut buf_reader = std::io::BufReader::new(f);
        let mut buf = std::string::String::with_capacity(128);
        skipped_bytes += buf_reader.read_line(&mut buf).unwrap() as u64;
    }

    skipped_bytes
}

fn process_batch(
    thread_idx: usize,
    file_path: &std::path::Path,
    start: u64,
    batch_bytes: u64,
) -> std::collections::BTreeMap<String, LocationStats> {
    let mut f = std::fs::File::open(file_path).unwrap();

    let mut processed_bytes = 0u64;

    if thread_idx == 0 {
        f.seek(std::io::SeekFrom::Start(start)).unwrap();
    } else {
        processed_bytes += skip_first_line(start, file_path);
        f.seek(std::io::SeekFrom::Start(start + processed_bytes))
            .unwrap();
    }

    let mut buf_reader = std::io::BufReader::new(f);
    let mut m = std::collections::BTreeMap::<String, LocationStats>::new();
    let mut line = std::string::String::with_capacity(128);

    loop {
        if processed_bytes >= batch_bytes {
            break;
        }

        line.clear();
        let bytes_read = buf_reader.read_line(&mut line).unwrap() as u64;
        if bytes_read == 0 {
            break; // EOF
        }

        processed_bytes += bytes_read;

        if let Some((location, temperature)) = parse_line(&line) {
            update_stats(&mut m, location, temperature);
        }
    }

    m
}

fn process_in_batches(
    file_path: &std::path::Path,
    file_size: u64,
) -> std::collections::BTreeMap<String, LocationStats> {
    let batch_size = (file_size + NUM_THREADS as u64 - 1) / NUM_THREADS as u64;
    let mut handles: std::vec::Vec<JoinHandle<std::collections::BTreeMap<String, LocationStats>>> =
        Vec::with_capacity(NUM_THREADS);

    for i in 0..NUM_THREADS {
        let start = i as u64 * batch_size;
        let path = file_path.to_path_buf();

        handles.push(std::thread::spawn(move || {
            process_batch(i, &path, start, batch_size)
        }));
    }

    let mut m = std::collections::BTreeMap::<String, LocationStats>::new();
    for handle in handles {
        let batch_map = handle.join().unwrap();
        update_map(&mut m, batch_map);
    }

    m
}

fn process_in_single_batch(
    file_path: &std::path::Path,
    file_size: u64,
) -> std::collections::BTreeMap<String, LocationStats> {
    process_batch(0, file_path, 0, file_size)
}

fn process(file_path: &std::path::Path) -> std::collections::BTreeMap<String, LocationStats> {
    let f = std::fs::File::open(file_path).unwrap();
    let file_size = f.metadata().unwrap().len();

    if file_size > 4 * 1024 {
        process_in_batches(file_path, file_size)
    } else {
        process_in_single_batch(file_path, file_size)
    }
}

fn main() {
    let file_path = get_file_path();

    let m = process(&file_path);

    print_result(&m).unwrap();
}


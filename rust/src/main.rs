// ~98s

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

fn parse_line(line: &[u8]) -> Option<(String, i32)> {
    let semicolon_pos = line.iter().position(|&b| b == b';')?;
    let location = &line[..semicolon_pos];
    let temp_bytes = &line[semicolon_pos + 1..];

    if location.is_empty() || temp_bytes.is_empty() {
        return None;
    }

    // Parse temperature from bytes
    let temp_str = std::str::from_utf8(temp_bytes).ok()?.trim();
    let temperature: f64 = temp_str.parse().ok()?;
    let temp_int = (temperature * 10.0).round() as i32;

    // Convert location bytes to String
    let location_str = std::str::from_utf8(location).ok()?.to_string();

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

fn skip_first_line(start: u64, f: &mut std::fs::File) -> u64 {
    assert!(start > 0);

    let mut skipped_bytes = 0u64;
    f.seek(std::io::SeekFrom::Start(start - 1)).unwrap();
    let mut prev_char = [0u8; 1];
    f.read_exact(&mut prev_char).unwrap();

    if prev_char[0] != b'\n' {
        // Skip rest of partial line and count the bytes
        loop {
            let mut byte = [0u8; 1];
            match f.read_exact(&mut byte) {
                Ok(()) => {
                    skipped_bytes += 1;
                    if byte[0] == b'\n' {
                        break;
                    }
                }
                Err(_) => break,
            }
        }
    }

    skipped_bytes
}

fn process_batch(
    thread_idx: usize,
    file_path: &std::path::Path,
    start: u64,
    batch_bytes: u64,
) -> std::collections::BTreeMap<String, LocationStats> {
    let mut m = std::collections::BTreeMap::<String, LocationStats>::new();

    let mut f = std::fs::File::open(file_path).unwrap();

    let mut processed_bytes = 0u64;

    if thread_idx == 0 {
        f.seek(std::io::SeekFrom::Start(start)).unwrap();
    } else {
        processed_bytes += skip_first_line(start, &mut f);
    }

    let mut buf_reader = std::io::BufReader::new(f);

    loop {
        if processed_bytes >= batch_bytes {
            break;
        }

        let buf = match buf_reader.fill_buf() {
            Ok(buf) if buf.is_empty() => break, // EOF
            Ok(buf) => buf,
            Err(_) => break,
        };

        let line_end = buf.iter().position(|&b| b == b'\n');

        match line_end {
            Some(pos) => {
                let line = &buf[..pos];
                if !line.is_empty() {
                    if let Some((location, temperature)) = parse_line(line) {
                        update_stats(&mut m, location, temperature);
                    }
                }
                let bytes_consumed = pos + 1;
                buf_reader.consume(bytes_consumed);
                processed_bytes += bytes_consumed as u64;
            }
            // No newline
            //      case 1: when internal buffer ends with a line split
            //      case 2: EOF
            // just call read_until directly, it'll handle the buffer internally
            None => {
                let mut line_buf = Vec::new();

                match buf_reader.read_until(b'\n', &mut line_buf) {
                    Ok(0) | Ok(_) if line_buf.is_empty() => break,
                    Ok(n) => {
                        let line = if line_buf.ends_with(&[b'\n']) {
                            &line_buf[..line_buf.len() - 1]
                        } else {
                            &line_buf[..]
                        };

                        if !line.is_empty() {
                            if let Some((location, temperature)) = parse_line(line) {
                                update_stats(&mut m, location, temperature);
                            }
                        }

                        processed_bytes += n as u64;
                    }
                    Err(_) => break,
                }
            }
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


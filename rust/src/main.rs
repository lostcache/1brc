// ~95.536s
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

fn skip_first_line(start: u64, mmap_f: &[u8]) -> u64 {
    assert!(start > 0);

    let prev_char = mmap_f[(start - 1) as usize];

    if prev_char == b'\n' {
        return 0;
    }

    let mut new_line_char_pos = start as usize;
    loop {
        if new_line_char_pos >= mmap_f.len() || mmap_f[new_line_char_pos] == b'\n' {
            break;
        }
        new_line_char_pos += 1;
    }

    new_line_char_pos as u64 - start + 1
}

fn parse_line(line: &[u8]) -> Option<(String, i32)> {
    if line.is_empty() {
        return None;
    }

    let mut semicol_pos = 0 as usize;
    loop {
        if line[semicol_pos] == b';' {
            break;
        }
        semicol_pos += 1;
    }

    let location = &line[..semicol_pos];
    let temp_str = &line[semicol_pos + 1..];

    if location.is_empty() || temp_str.is_empty() {
        return None;
    }

    // Parse temperature from bytes
    let temperature: f64 = std::str::from_utf8(temp_str).unwrap().parse().unwrap();
    let temp_int = (temperature * 10.0).round() as i32;

    // Convert location bytes to String
    let location_str = std::str::from_utf8(location).unwrap().to_string();

    Some((location_str, temp_int))
}

fn process_batch(
    thread_idx: usize,
    mmap_f: &[u8],
    start: u64,
    batch_bytes: u64,
) -> std::collections::BTreeMap<String, LocationStats> {
    let mut processed_bytes = 0 as u64;

    if thread_idx != 0 {
        processed_bytes += skip_first_line(start, mmap_f);
    }

    let mut m = std::collections::BTreeMap::<String, LocationStats>::new();

    loop {
        if processed_bytes >= batch_bytes {
            break;
        }

        let line_start = (start + processed_bytes) as usize;
        if line_start >= mmap_f.len() {
            break;
        }

        let mut new_line_char_pos = start + processed_bytes;
        loop {
            if new_line_char_pos as usize >= mmap_f.len()
                || mmap_f[new_line_char_pos as usize] == b'\n'
            {
                break;
            }
            new_line_char_pos += 1;
        }

        let line_end = new_line_char_pos as usize;
        if let Some((location, temperature)) = parse_line(&mmap_f[line_start..line_end]) {
            update_stats(&mut m, location, temperature);
        }

        processed_bytes += (line_end - line_start + 1) as u64;
    }

    m
}

fn process_in_batches(
    file_path: &std::path::Path,
    file_size: u64,
) -> std::collections::BTreeMap<String, LocationStats> {
    // Create mmap once and share it across all threads
    let f = std::fs::File::open(file_path).unwrap();
    let mmap_f = unsafe { memmap2::Mmap::map(&f).unwrap() };
    let mmap_arc = std::sync::Arc::new(mmap_f);

    let batch_size = (file_size + NUM_THREADS as u64 - 1) / NUM_THREADS as u64;
    let mut handles: std::vec::Vec<
        std::thread::JoinHandle<std::collections::BTreeMap<String, LocationStats>>,
    > = Vec::with_capacity(NUM_THREADS);

    for i in 0..NUM_THREADS {
        let start = i as u64 * batch_size;
        let mmap_clone = mmap_arc.clone();

        handles.push(std::thread::spawn(move || {
            process_batch(i, &mmap_clone, start, batch_size)
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
    let f = std::fs::File::open(file_path).unwrap();
    let mmap_f = unsafe { memmap2::Mmap::map(&f).unwrap() };
    process_batch(0, &mmap_f, 0, file_size)
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


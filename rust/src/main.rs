use memchr::memchr;

const NUM_THREADS: usize = 8;

#[derive(Clone, PartialEq, Eq, PartialOrd, Ord)]
struct LocationEntry {
    location: String,
    min: i32,
    max: i32,
    sum: i64,
    freq: usize,
}

impl LocationEntry {
    #[inline]
    pub fn new() -> Self {
        Self {
            location: std::string::String::new(),
            min: std::i32::MAX,
            max: std::i32::MIN,
            sum: 0,
            freq: 0,
        }
    }
}

struct FastMap {
    data: std::vec::Vec<LocationEntry>,
    mask: usize,
}

impl FastMap {
    #[inline(always)]
    fn hash(key: &[u8]) -> usize {
        let mut hash: usize = 0;
        for &ch in key {
            hash = hash.wrapping_mul(1315423911).wrapping_add(ch as usize);
        }
        hash
    }

    #[inline(always)]
    fn get_idx(&self, key: &[u8]) -> usize {
        let hash = Self::hash(key);
        let mut idx = hash & self.mask;
        loop {
            let entry = unsafe { self.data.get_unchecked(idx) };
            if entry.freq == 0 || entry.location.as_bytes() == key {
                return idx;
            }
            idx = (idx + 1) & self.mask;
        }
    }

    fn sort_inplace(&mut self) {
        self.data.sort();
    }

    #[inline(always)]
    pub fn update(&mut self, location: &[u8], temperature: i32) {
        let idx = self.get_idx(location);
        let entry = unsafe { self.data.get_unchecked_mut(idx) };

        if entry.freq != 0 {
            // Hot path: existing entry (most common case)
            entry.freq += 1;
            entry.sum += temperature as i64;
            entry.min = std::cmp::min(entry.min, temperature);
            entry.max = std::cmp::max(entry.max, temperature);
        } else {
            // Cold path: new entry
            entry.location = unsafe { std::str::from_utf8_unchecked(location).to_string() };
            entry.freq = 1;
            entry.sum = temperature as i64;
            entry.min = temperature;
            entry.max = temperature;
        }
    }

    pub fn update_batch(&mut self, other: &Self) {
        for other_entry in &other.data {
            if other_entry.freq == 0 {
                continue;
            }

            let this_idx = self.get_idx(other_entry.location.as_bytes());
            let this_entry = unsafe { self.data.get_unchecked_mut(this_idx) };

            if this_entry.freq == 0 {
                this_entry.location = other_entry.location.clone();
                this_entry.min = other_entry.min;
                this_entry.max = other_entry.max;
                this_entry.sum = other_entry.sum;
                this_entry.freq = other_entry.freq;
            } else {
                this_entry.freq += other_entry.freq;
                this_entry.sum += other_entry.sum;
                this_entry.min = std::cmp::min(this_entry.min, other_entry.min);
                this_entry.max = std::cmp::max(this_entry.max, other_entry.max);
            }
        }
    }

    pub fn print_sorted(&mut self) {
        self.sort_inplace();

        let mut it = self.data.iter().peekable();

        print!("{{");
        while let Some(location_entry) = it.next() {
            let location = &location_entry.location;

            if location.len() <= 0 {
                continue;
            }

            let mut avg = location_entry.sum as f64 / location_entry.freq as f64 / 10.0;
            avg = (avg * 10.0).round() / 10.0;
            print!(
                "{}={:.1}/{:.1}/{:.1}",
                location,
                location_entry.min as f64 / 10.0,
                avg,
                location_entry.max as f64 / 10.0
            );

            if let Some(_) = it.peek() {
                print!(", ");
            }
        }
        print!("}}");
    }

    pub fn new() -> Self {
        Self {
            data: vec![LocationEntry::new(); 1 << 14],
            mask: (1 << 14) - 1,
        }
    }
}

fn get_file_path() -> std::path::PathBuf {
    let args: std::vec::Vec<String> = std::env::args().collect();
    match args.len() {
        2 => std::path::PathBuf::from(&args[1]),
        _ => std::path::PathBuf::from("../data/measurements.txt"),
    }
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

#[inline(always)]
fn parse_i32_from_byte_slice(slice: &[u8]) -> i32 {
    let (is_negative, start) = if slice[0] == b'-' {
        (true, 1)
    } else {
        (false, 0)
    };

    let num = if slice[start + 1] == b'.' {
        (slice[start] - b'0') as i32 * 10 + (slice[start + 2] - b'0') as i32
    } else {
        (slice[start] - b'0') as i32 * 100
            + (slice[start + 1] - b'0') as i32 * 10
            + (slice[start + 3] - b'0') as i32
    };

    if is_negative { -num } else { num }
}

#[inline(always)]
fn parse_line(line: &[u8]) -> Option<(&[u8], i32)> {
    if line.len() < 4 {
        return None;
    }

    // Semicolon is always at len - 4, len - 5, or len - 6
    // Temperature format: [-]D[D].D (3-5 chars + semicolon)
    let len = line.len();
    let semicol_pos = if line[len - 4] == b';' {
        len - 4
    } else if line[len - 5] == b';' {
        len - 5
    } else {
        len - 6
    };

    let location = &line[..semicol_pos];
    let temperature_slice = &line[semicol_pos + 1..];

    let temperature = parse_i32_from_byte_slice(temperature_slice);

    Some((location, temperature))
}

fn process_batch(thread_idx: usize, mmap_f: &[u8], start: u64, batch_bytes: u64) -> FastMap {
    // Pin thread to CPU core (Linux-specific)
    unsafe {
        let mut cpu_set: libc::cpu_set_t = std::mem::zeroed();
        libc::CPU_SET(thread_idx % NUM_THREADS, &mut cpu_set);
        libc::sched_setaffinity(0, std::mem::size_of::<libc::cpu_set_t>(), &cpu_set);
    }

    let mut processed_bytes: u64 = 0;

    if thread_idx != 0 {
        processed_bytes += skip_first_line(start, mmap_f);
    }

    let mut m = FastMap::new();
    let mut pos = (start + processed_bytes) as usize;
    let end_target = (start + batch_bytes) as usize;

    while pos < mmap_f.len() && (pos as u64) < (start + batch_bytes) {
        // Use SIMD-optimized memchr for newline search
        let remaining = &mmap_f[pos..];
        let line_end = match memchr(b'\n', remaining) {
            Some(offset) => pos + offset,
            None => mmap_f.len(),
        };

        let line = &mmap_f[pos..line_end];
        if let Some((location, temperature)) = parse_line(line) {
            m.update(location, temperature);
        }

        pos = line_end + 1;
        
        if pos >= end_target {
            break;
        }
    }

    m
}

fn process_in_batches(file_path: &std::path::Path, file_size: u64) -> FastMap {
    let f = std::fs::File::open(file_path).unwrap();
    let mmap_f = unsafe { memmap2::Mmap::map(&f).unwrap() };

    unsafe {
        libc::madvise(
            mmap_f.as_ptr() as *mut libc::c_void,
            file_size as usize,
            libc::MADV_SEQUENTIAL,
        );
        libc::madvise(
            mmap_f.as_ptr() as *mut libc::c_void,
            file_size as usize,
            libc::MADV_WILLNEED,
        );
    }
    
    let mmap_arc = std::sync::Arc::new(mmap_f);

    let batch_size = (file_size + NUM_THREADS as u64 - 1) / NUM_THREADS as u64;
    let mut handles: std::vec::Vec<std::thread::JoinHandle<FastMap>> =
        Vec::with_capacity(NUM_THREADS);

    for i in 0..NUM_THREADS {
        let start = i as u64 * batch_size;
        let mmap_clone = mmap_arc.clone();

        handles.push(std::thread::spawn(move || {
            process_batch(i, &mmap_clone, start, batch_size)
        }));
    }

    let mut m = FastMap::new();
    for handle in handles {
        let batch_map = handle.join().unwrap();
        m.update_batch(&batch_map);
    }

    m
}

fn process_in_single_batch(file_path: &std::path::Path, file_size: u64) -> FastMap {
    let f = std::fs::File::open(file_path).unwrap();
    let mmap_f = unsafe { memmap2::Mmap::map(&f).unwrap() };
    process_batch(0, &mmap_f, 0, file_size)
}

fn process(file_path: &std::path::Path) -> FastMap {
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

    let mut m = process(&file_path);

    m.print_sorted();
}


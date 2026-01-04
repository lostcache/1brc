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
    fn hash(&self, key: &[u8]) -> usize {
        let mut hash = 0 as usize;

        for ch in key {
            hash = hash * 1315423911 as usize + *ch as usize;
        }
        hash
    }

    fn get_idx(&self, key: &[u8]) -> usize {
        let hash = self.hash(key);
        let mut idx = hash & self.mask;
        loop {
            let location = &self.data[idx].location;
            if location.len() <= 0 || location.as_bytes() == key {
                return idx;
            }
            idx = (idx + 1) & self.mask;
        }
    }

    fn sort_inplace(&mut self) {
        self.data.sort();
    }

    pub fn update(&mut self, location: &[u8], temperature: i32) {
        let idx = self.get_idx(location);

        if self.data[idx].location.len() <= 0 {
            self.data[idx].location = unsafe { std::str::from_utf8_unchecked(location).to_string() }
        }
        self.data[idx].freq += 1;
        self.data[idx].sum += temperature as i64;
        self.data[idx].min = std::cmp::min(self.data[idx].min, temperature);
        self.data[idx].max = std::cmp::max(self.data[idx].max, temperature);
    }

    pub fn update_batch(&mut self, other: &Self) {
        for i in 0..other.data.len() {
            let other_location = &other.data[i].location;

            if other_location.len() <= 0 {
                continue;
            }

            let this_idx = self.get_idx(other_location.as_bytes());

            if self.data[this_idx].location.len() <= 0 {
                self.data[this_idx].location = other_location.to_string();
            }
            self.data[this_idx].freq += other.data[i].freq;
            self.data[this_idx].sum += other.data[i].sum;
            self.data[this_idx].min = std::cmp::min(self.data[this_idx].min, other.data[i].min);
            self.data[this_idx].max = std::cmp::max(self.data[this_idx].max, other.data[i].max);
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

fn parse_i32_from_byte_slice(slice: &[u8]) -> i32 {
    let mut pos = 0 as usize;
    let is_negative = if slice[0] == b'-' {
        pos += 1;
        true
    } else {
        false
    };

    let num = if slice[pos + 1] == b'.' {
        (slice[pos] - b'0') as i32 * 10 + (slice[pos + 2] - b'0') as i32
    } else {
        (slice[pos] - b'0') as i32 * 100
            + (slice[pos + 1] - b'0') as i32 * 10
            + (slice[pos + 3] - b'0') as i32
    };

    if is_negative {
        -num
    } else {
        num
    }
}

fn parse_line(line: &[u8]) -> Option<(&[u8], i32)> {
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
    let temperature_slice = &line[semicol_pos + 1..];

    if location.is_empty() || temperature_slice.is_empty() {
        return None;
    }

    // saves ~3s in ~34s total runtime.
    let temperature = parse_i32_from_byte_slice(temperature_slice);

    Some((location, temperature))
}

fn process_batch(thread_idx: usize, mmap_f: &[u8], start: u64, batch_bytes: u64) -> FastMap {
    let mut processed_bytes = 0 as u64;

    if thread_idx != 0 {
        processed_bytes += skip_first_line(start, mmap_f);
    }

    let mut m = FastMap::new();

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
            m.update(location, temperature);
        }

        processed_bytes += (line_end - line_start + 1) as u64;
    }

    m
}

fn process_in_batches(file_path: &std::path::Path, file_size: u64) -> FastMap {
    // Create mmap once and share it across all threads
    let f = std::fs::File::open(file_path).unwrap();
    let mmap_f = unsafe { memmap2::Mmap::map(&f).unwrap() };
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


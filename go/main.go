package main

import (
	"bufio"
	"bytes"
	"fmt"
	"io"
	"math"
	"os"
	"sort"
	"sync"
)

const NumThreads = 16
const MapSize = 1 << 14
const MapMask = MapSize - 1

type LocationStat struct {
	key  []byte
	min  int32
	max  int32
	sum  int64
	freq uint32
}

type FastMap struct {
	entries []LocationStat
}

func NewFastMap() *FastMap {
	m := &FastMap{
		entries: make([]LocationStat, MapSize),
	}
	for i := range m.entries {
		m.entries[i].min = math.MaxInt32
		m.entries[i].max = math.MinInt32
	}
	return m
}

func fastHash(key []byte) uint64 {
	var hash uint64 = 0
	for _, b := range key {
		hash = hash*1315423911 + uint64(b)
	}
	return hash
}

func (m *FastMap) Put(key []byte, temp int32) {
	idx := fastHash(key) & MapMask
	for {
		entry := &m.entries[idx]
		if entry.freq == 0 {
			keyCopy := make([]byte, len(key))
			copy(keyCopy, key)
			entry.key = keyCopy
			entry.min = temp
			entry.max = temp
			entry.sum = int64(temp)
			entry.freq = 1
			return
		}
		if bytes.Equal(entry.key, key) {
			if temp < entry.min {
				entry.min = temp
			}
			if temp > entry.max {
				entry.max = temp
			}
			entry.sum += int64(temp)
			entry.freq++
			return
		}
		idx = (idx + 1) & MapMask
	}
}

func (m *FastMap) PutEntry(stat *LocationStat) {
	idx := fastHash(stat.key) & MapMask
	for {
		entry := &m.entries[idx]
		if entry.freq == 0 {
			entry.key = stat.key
			entry.min = stat.min
			entry.max = stat.max
			entry.sum = stat.sum
			entry.freq = stat.freq
			return
		}
		if bytes.Equal(entry.key, stat.key) {
			if stat.min < entry.min {
				entry.min = stat.min
			}
			if stat.max > entry.max {
				entry.max = stat.max
			}
			entry.sum += stat.sum
			entry.freq += stat.freq
			return
		}
		idx = (idx + 1) & MapMask
	}
}

func getFileSize(filePath string) (int64, error) {
	file, err := os.Open(filePath)
	if err != nil {
		return -1, err
	}
	defer file.Close()
	stat, err := file.Stat()
	if err != nil {
		return -1, err
	}
	return stat.Size(), nil
}

func parseInt32(temperatureBytes []byte) int32 {
	var pos = 0
	var isNegative = false
	var val int32 = 0

	if temperatureBytes[0] == '-' {
		isNegative = true
		pos += 1
	}

	if temperatureBytes[pos+1] == '.' {
		val = int32(temperatureBytes[pos]-'0')*10 + int32(temperatureBytes[pos+2]-'0')
	} else {
		val = 100*int32(temperatureBytes[pos]-'0') + 10*int32(temperatureBytes[pos+1]-'0') + int32(temperatureBytes[pos+3]-'0')
	}

	if isNegative {
		val = -val
	}

	return val
}

func parseLine(line []byte) ([]byte, int32) {
	locationBytes, temperatureBytes, _ := bytes.Cut(line, []byte(";"))
	temperature := parseInt32(temperatureBytes)
	return locationBytes, temperature
}

func processBatch(i int, start int64, end int64, filePath string, m *FastMap) error {
	file, err := os.Open(filePath)
	if err != nil {
		return err
	}
	defer file.Close()

	if _, err := file.Seek(start, 0); err != nil {
		return err
	}

	reader := bufio.NewReaderSize(file, 1024*1024)
	currentOffset := start

	if i > 0 {
		b := make([]byte, 1)
		if _, err := file.ReadAt(b, start-1); err != nil {
			return err
		}
		if b[0] != '\n' {
			skipped, err := reader.ReadBytes('\n')
			if err != nil {
				if err == io.EOF {
					return nil
				}
				return err
			}
			currentOffset += int64(len(skipped))
		}
	}

	for currentOffset < end {
		line, err := reader.ReadSlice('\n')
		if err != nil {
			if err == io.EOF {
				if len(line) > 0 {
					if line[len(line)-1] == '\r' {
						line = line[:len(line)-1]
					}
					l, t := parseLine(line)
					m.Put(l, t)
				}
				break
			}
			return err
		}

		currentOffset += int64(len(line))

		lineContent := line[:len(line)-1]
		if len(lineContent) > 0 && lineContent[len(lineContent)-1] == '\r' {
			lineContent = lineContent[:len(lineContent)-1]
		}

		l, t := parseLine(lineContent)
		m.Put(l, t)
	}

	return nil
}

func accumulateResults(maps []*FastMap) *FastMap {
	finalMap := NewFastMap()
	for _, m := range maps {
		for i := range m.entries {
			if m.entries[i].freq > 0 {
				finalMap.PutEntry(&m.entries[i])
			}
		}
	}
	return finalMap
}

func printResults(m *FastMap) error {
	var validEntries []LocationStat
	for i := range m.entries {
		if m.entries[i].freq > 0 {
			validEntries = append(validEntries, m.entries[i])
		}
	}
	sort.Slice(validEntries, func(i, j int) bool {
		return string(validEntries[i].key) < string(validEntries[j].key)
	})

	fmt.Print("{")
	for i, stat := range validEntries {
		min := float64(stat.min) / 10.0
		mean := math.Round(float64(stat.sum)/float64(stat.freq)) / 10.0
		max := float64(stat.max) / 10.0
		if i > 0 {
			fmt.Print(", ")
		}
		fmt.Printf("%s=%.1f/%.1f/%.1f", stat.key, min, mean, max)
	}
	fmt.Println("}")
	return nil
}

func processInBatches(filePath string, fileSize int64) error {
	var m = make([]*FastMap, NumThreads)
	for i := range NumThreads {
		m[i] = NewFastMap()
	}

	batchSize := (fileSize + NumThreads - 1) / NumThreads
	var wg sync.WaitGroup

	for i := range NumThreads {
		start := int64(i) * batchSize
		end := min(start+batchSize, fileSize)
		wg.Add(1)
		go func(i int, start int64, end int64) {
			defer wg.Done()
			processBatch(i, start, end, filePath, m[i])
		}(i, start, end)
	}
	wg.Wait()

	finalMap := accumulateResults(m)
	printResults(finalMap)
	return nil
}

func processInSingleBatch(filePath string, fileSize int64) error {
	m := NewFastMap()
	err := processBatch(0, 0, fileSize, filePath, m)
	if err != nil {
		return err
	}
	printResults(m)
	return nil
}

func onebrc(filePath string) error {
	fileSize, err := getFileSize(filePath)
	if err != nil {
		return err
	}
	if fileSize > 4*1024 {
		return processInBatches(filePath, fileSize)
	}
	return processInSingleBatch(filePath, fileSize)
}

func main() {
	var filePath = "../data/measurements.txt"
	if len(os.Args) > 1 {
		filePath = os.Args[1]
	}
	err := onebrc(filePath)
	if err != nil {
		fmt.Println(err)
		os.Exit(1)
	}
}


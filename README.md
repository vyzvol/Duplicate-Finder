# Duplicate Finder

Ultra-fast tool for detecting duplicate lines between two massive text files.

## 🚀 Features
- Full multi-core utilization (tested on Ryzen 5 7600, 12 threads)
- Handles files up to **80 GB+** efficiently with memory limit at **16 GB**
- Optimized with **xxHash3**, **mmap**, and **parallel chunked processing**
- Uses **robin_hood hashing** for ultra-low lookup overhead
- Perfect for comparing large password lists (e.g. *rockyou.txt*, *hashmob.net* dumps)

## 🧠 Usage
```bash
./duplicate_finder file1.txt file2.txt
````

## ⚙️ Build

```bash
g++ duplicate_finder.cpp -O3 -march=znver4 -flto -funroll-loops -fomit-frame-pointer -pipe -pthread -lxxhash
```

## 📈 Performance

**Test setup:**
Ryzen 5 7600 + 32 GB DDR5 + average nvme

**Benchmark:**
`rockyou.txt (136 MB)` vs `hashmob.net_2025-10-19.large.found (542 MB)`  
→ **Completed in ~6 seconds**

**Estimated scaling:**
≈ 60 GB of total data → **~9 minutes** of processing **IM NOT SURE**

## 🧩 Requirements

* Linux with POSIX (mmap, madvise, setrlimit)
* `xxHash` library (`sudo pacman -S xxhash`)
* `robin_hood.h` header (from [https://github.com/martinus/robin-hood-hashing](https://github.com/martinus/robin-hood-hashing))
* 16 GB+ of RAM (tool self-limits to 16 GB)

## 📝 License

MIT

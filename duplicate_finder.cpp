#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <algorithm>
#include <thread>
#include <chrono>
#include <memory>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/resource.h>
#include <cassert>
#include <mutex>
#include <atomic>
#include <queue>
#include <functional>
#include <condition_variable>
#include <future>
#include <unordered_set>

// Хэш-функция для оптимизации производительности
#include <xxhash.h>
#include <robin_hood.h>

// Ограничение памяти до 16 ГБ
#define MAX_MEMORY_LIMIT (16ULL * 1024 * 1024 * 1024)
#define CHUNK_SIZE (2ULL * 1024 * 1024 * 1024) // Увеличенный размер блока до 2 ГБ

// Класс для работы с маппингом файлов с использованием huge pages
class FileMapper {
private:
    int fd;
    void* mapped_data;
    size_t file_size;
    
public:
    FileMapper(const std::string& filename) : fd(-1), mapped_data(nullptr), file_size(0) {
        fd = open(filename.c_str(), O_RDONLY);
        if (fd == -1) {
            throw std::runtime_error("Не удалось открыть файл: " + filename);
        }
        
        // Добавляем подсказку для последовательного чтения
        posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
        
        struct stat sb;
        if (fstat(fd, &sb) == -1) {
            close(fd);
            throw std::runtime_error("Не удалось получить информацию о файле: " + filename);
        }
        
        file_size = sb.st_size;
        
        // Используем MAP_POPULATE для предзагрузки страниц
        mapped_data = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
        if (mapped_data == MAP_FAILED) {
            close(fd);
            throw std::runtime_error("Ошибка маппинга файла: " + filename);
        }
        
        // Используем sequential advise для лучшей производительности
        if (madvise(mapped_data, file_size, MADV_SEQUENTIAL) != 0) {
            // Если не удалось использовать sequential advise, продолжаем без него
        }
    }
    
    ~FileMapper() {
        if (mapped_data != nullptr && mapped_data != MAP_FAILED) {
            munmap(mapped_data, file_size);
        }
        if (fd != -1) {
            close(fd);
        }
    }
    
    const char* data() const { return static_cast<const char*>(mapped_data); }
    size_t size() const { return file_size; }
};

// Основной класс для сравнения файлов
class DuplicateFinder {
private:
    std::string file1_path;
    std::string file2_path;
    
    // Счетчики без атомиков
    size_t file1_total_lines = 0;
    size_t file2_total_lines = 0;
    size_t file1_unique_lines = 0;
    size_t file2_unique_lines = 0;
    size_t duplicate_count = 0;
    
    // Используем более быструю хэш-таблицу
    robin_hood::unordered_flat_set<uint64_t> file1_hashes;
    robin_hood::unordered_flat_set<uint64_t> file2_hashes;
    
public:
    DuplicateFinder(const std::string& path1, const std::string& path2) 
        : file1_path(path1), file2_path(path2) {}
    
    void run() {
        // Отключаем синхронизацию I/O для повышения скорости
        std::ios::sync_with_stdio(false);
        std::cin.tie(nullptr);
        
        std::cout << "Начинаю анализ файлов...\n";
        std::cout << "Файл 1: " << file1_path << "\n";
        std::cout << "Файл 2: " << file2_path << "\n";
        
        // Устанавливаем лимит памяти
        set_memory_limit();
        
        // Анализируем оба файла
        analyze_files();
        
        // Выводим результаты
        print_results();
    }
    
private:
    void set_memory_limit() {
        struct rlimit rl;
        rl.rlim_cur = MAX_MEMORY_LIMIT;
        rl.rlim_max = MAX_MEMORY_LIMIT;
        if (setrlimit(RLIMIT_AS, &rl) == -1) {
            std::cerr << "Предупреждение: не удалось установить лимит памяти\n";
        }
    }
    
    void analyze_files() {
        // Анализ первого файла
        analyze_file(file1_path, true);
        
        // Анализ второго файла
        analyze_file(file2_path, false);
    }
    
    void analyze_file(const std::string& filename, bool is_file1) {
        FileMapper mapper(filename);
        const char* data = mapper.data();
        size_t file_size = mapper.size();
        
        std::cout << "Анализирую файл: " << filename << " (" << file_size << " байт)\n";
        
        // Предварительное резервирование памяти для хэш-таблицы
        size_t estimated_lines = file_size / 100; // Примерная оценка
        if (is_file1) {
            file1_hashes.reserve(estimated_lines);
        } else {
            file2_hashes.reserve(estimated_lines);
        }
        
        // Разбиваем файл на блоки для обработки
        std::vector<std::pair<size_t, size_t>> chunks;
        size_t start = 0;
        while (start < file_size) {
            size_t end = start + CHUNK_SIZE;
            if (end > file_size) end = file_size;
            chunks.emplace_back(start, end);
            start = end;
        }
        
        // Параллельная обработка блоков с использованием std::thread
        std::vector<std::thread> threads;
        size_t num_threads = std::min(static_cast<size_t>(std::thread::hardware_concurrency()), chunks.size());
        
        for (size_t i = 0; i < num_threads; ++i) {
            threads.emplace_back([this, data, &chunks, is_file1, num_threads, i]() {
                // Устанавливаем привязку к ядру процессора
                cpu_set_t cpuset;
                CPU_ZERO(&cpuset);
                CPU_SET(i % std::thread::hardware_concurrency(), &cpuset);
                pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
                
                // Локальные счётчики для каждого потока
                size_t local_total = 0;
                size_t local_unique = 0;
                
                // Обрабатываем только свои блоки
                for (size_t j = i; j < chunks.size(); j += num_threads) {
                    analyze_file_chunk(data, chunks[j].first, chunks[j].second, is_file1, local_total, local_unique);
                }
                
                // Суммируем в общие переменные
                if (is_file1) {
                    file1_total_lines += local_total;
                    file1_unique_lines += local_unique;
                } else {
                    file2_total_lines += local_total;
                    file2_unique_lines += local_unique;
                }
            });
        }
        
        // Ждем завершения всех потоков
        for (auto& t : threads) {
            t.join();
        }
        
        std::cout << "Обработано строк: " << (is_file1 ? file1_total_lines : file2_total_lines) << "\n";
    }
    
    void analyze_file_chunk(const char* data, size_t start, size_t end, bool is_file1, 
                           size_t& local_total, size_t& local_unique) {
        // Используем memchr для поиска новых строк
        const char* pos = data + start;
        const char* end_pos = data + end;
        const char* line_start = pos;
        robin_hood::unordered_flat_set<uint64_t>* hashes = is_file1 ? &file1_hashes : &file2_hashes;
        
        while (pos < end_pos) {
            // Используем memchr для поиска следующего перевода строки
            const char* newline = static_cast<const char*>(memchr(pos, '\n', end_pos - pos));
            
            if (newline == nullptr) {
                // Последняя строка в блоке
                size_t line_length = end_pos - line_start;
                if (line_length > 0) {
                    uint64_t hash = XXH3_64bits(line_start, line_length);
                    local_total++;
                    if (hashes->insert(hash).second) {
                        local_unique++;
                    }
                }
                break;
            }
            
            // Подсказка процессору для предзагрузки данных
            __builtin_prefetch(line_start + 64, 0, 1);
            
            // Обрабатываем строку
            size_t line_length = newline - line_start;
            if (line_length > 0) {
                uint64_t hash = XXH3_64bits(line_start, line_length);
                local_total++;
                if (hashes->insert(hash).second) {
                    local_unique++;
                }
            }
            
            line_start = newline + 1;
            pos = newline + 1;
        }
    }
    
    void print_results() {
        // Подсчитываем пересечения
        size_t duplicates = 0;
        if (file1_hashes.size() <= file2_hashes.size()) {
            for (const auto& hash : file1_hashes) {
                if (file2_hashes.find(hash) != file2_hashes.end()) {
                    duplicates++;
                }
            }
        } else {
            for (const auto& hash : file2_hashes) {
                if (file1_hashes.find(hash) != file1_hashes.end()) {
                    duplicates++;
                }
            }
        }
        
        size_t unique_to_file1 = file1_unique_lines - duplicates;
        size_t unique_to_file2 = file2_unique_lines - duplicates;
        
        std::cout << "\n=== РЕЗУЛЬТАТЫ АНАЛИЗА ===\n";
        std::cout << "\nФайл 1: " << file1_path << "\n";
        std::cout << "  Всего строк: " << file1_total_lines << "\n";
        std::cout << "  Уникальных строк: " << file1_unique_lines << "\n";
        std::cout << "  Уникальных строк только в файле 1: " << unique_to_file1 << "\n";
        
        std::cout << "\nФайл 2: " << file2_path << "\n";
        std::cout << "  Всего строк: " << file2_total_lines << "\n";
        std::cout << "  Уникальных строк: " << file2_unique_lines << "\n";
        std::cout << "  Уникальных строк только в файле 2: " << unique_to_file2 << "\n";
        
        std::cout << "\n=== ПОВТОРЯЮЩИЕСЯ СТРОКИ ===\n";
        std::cout << "  Повторяющихся строк: " << duplicates << "\n";
        std::cout << "  Общее количество повторений: " << duplicates << "\n";
    }
};

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Использование: " << argv[0] << " <файл1> <файл2>\n";
        return 1;
    }
    
    try {
        DuplicateFinder finder(argv[1], argv[2]);
        finder.run();
    } catch (const std::exception& e) {
        std::cerr << "Ошибка: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}
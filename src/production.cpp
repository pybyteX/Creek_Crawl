#include <iostream>
#include <string>
#include <vector>
#include <queue>
#include <atomic>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <fstream>
#include <cstdio>
#include <curl/curl.h>

class DiskBackedCrawler {
private:
    struct CrawlTask {
        std::string url;
        int depth;
    };
    
    // File-based URL storage (uses almost no RAM)
    class UrlStore {
    private:
        FILE* data_file;
        FILE* index_file;
        std::shared_mutex mtx;
        
        struct IndexEntry {
            uint64_t hash;
            uint64_t offset;
            uint32_t length;
        };
        
        // Tiny RAM cache for recent URLs (optional, 1MB max)
        std::unordered_map<std::string, bool> recent_cache;
        static constexpr size_t CACHE_SIZE = 10000;
        
    public:
        UrlStore(const std::string& prefix) {
            data_file = fopen((prefix + "_urls.dat").c_str(), "a+b");
            index_file = fopen((prefix + "_index.dat").c_str(), "a+b");
            
            if (!data_file || !index_file) {
                throw std::runtime_error("Cannot open URL database files");
            }
        }
        
        ~UrlStore() {
            if (data_file) fclose(data_file);
            if (index_file) fclose(index_file);
        }
        
        bool contains(const std::string& url) {
            // Check RAM cache first (fast path)
            {
                std::shared_lock lock(mtx);
                auto it = recent_cache.find(url);
                if (it != recent_cache.end()) {
                    return it->second;
                }
            }
            
            // Check on disk
            uint64_t hash = std::hash<std::string>{}(url);
            
            std::shared_lock lock(mtx);
            fseek(index_file, 0, SEEK_SET);
            
            IndexEntry entry;
            while (fread(&entry, sizeof(IndexEntry), 1, index_file) == 1) {
                if (entry.hash == hash) {
                    // Verify by reading actual URL
                    char* buffer = new char[entry.length + 1];
                    fseek(data_file, entry.offset, SEEK_SET);
                    fread(buffer, 1, entry.length, data_file);
                    buffer[entry.length] = '\0';
                    
                    bool found = (url == buffer);
                    delete[] buffer;
                    
                    if (found) {
                        // Cache for future
                        std::unique_lock write_lock(mtx);
                        if (recent_cache.size() < CACHE_SIZE) {
                            recent_cache[url] = true;
                        }
                        return true;
                    }
                }
            }
            
            return false;
        }
        
        bool add_url(const std::string& url) {
            if (contains(url)) {
                return false;
            }
            
            uint64_t hash = std::hash<std::string>{}(url);
            
            std::unique_lock lock(mtx);
            
            // Write URL data
            fseek(data_file, 0, SEEK_END);
            uint64_t offset = ftell(data_file);
            fwrite(url.c_str(), 1, url.size(), data_file);
            
            // Write index entry
            IndexEntry entry{hash, offset, (uint32_t)url.size()};
            fwrite(&entry, sizeof(IndexEntry), 1, index_file);
            
            fflush(data_file);
            fflush(index_file);
            
            // Update cache
            if (recent_cache.size() < CACHE_SIZE) {
                recent_cache[url] = true;
            }
            
            return true;
        }
        
        size_t count() {
            std::shared_lock lock(mtx);
            fseek(index_file, 0, SEEK_END);
            return ftell(index_file) / sizeof(IndexEntry);
        }
    };
    
    UrlStore url_store;
    std::queue<CrawlTask> task_queue;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::atomic<bool> running{true};
    std::atomic<size_t> active_tasks{0};
    std::vector<std::thread> workers;
    
    // Configuration
    size_t max_depth;
    size_t max_concurrent;
    size_t urls_processed{0};
    
    // Fetch URL (streaming to disk)
    bool fetch_to_disk(const std::string& url, const std::string& output_file) {
        CURL* curl = curl_easy_init();
        if (!curl) return false;
        
        FILE* out = fopen(output_file.c_str(), "wb");
        if (!out) {
            curl_easy_cleanup(curl);
            return false;
        }
        
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, 
            [](char* ptr, size_t size, size_t nmemb, void* stream) {
                return fwrite(ptr, size, nmemb, (FILE*)stream);
            });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, 
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
        
        CURLcode res = curl_easy_perform(curl);
        
        fclose(out);
        curl_easy_cleanup(curl);
        
        return res == CURLE_OK;
    }
    
    // Extract links from HTML file (no RAM storage)
    std::vector<std::string> extract_links_from_file(const std::string& filepath) {
        std::vector<std::string> links;
        std::ifstream file(filepath);
        if (!file.is_open()) return links;
        
        std::string content;
        content.reserve(1024 * 1024);  // 1MB buffer
        
        // Read in chunks
        char buffer[8192];
        while (file.read(buffer, sizeof(buffer))) {
            content.append(buffer, sizeof(buffer));
        }
        content.append(buffer, file.gcount());
        
        // Parse links (simple regex)
        std::regex href_regex(R"(href=\"(https?://[^\"]+)\")");
        std::sregex_iterator it(content.begin(), content.end(), href_regex);
        std::sregex_iterator end;
        
        for (; it != end; ++it) {
            std::string link = (*it)[1];
            if (link.find("javascript:") == std::string::npos &&
                link.find("mailto:") == std::string::npos) {
                links.push_back(std::move(link));
            }
        }
        
        // Delete file immediately to free disk space
        file.close();
        std::remove(filepath.c_str());
        
        return links;
    }
    
    void worker_function() {
        while (running) {
            std::unique_lock<std::mutex> lock(queue_mutex);
            queue_cv.wait(lock, [this] { 
                return !task_queue.empty() || !running; 
            });
            
            if (!running && task_queue.empty()) break;
            
            CrawlTask task = std::move(task_queue.front());
            task_queue.pop();
            lock.unlock();
            
            active_tasks++;
            
            // Process URL with minimal RAM
            std::string temp_file = "/tmp/crawl_" + std::to_string(std::hash<std::string>{}(task.url)) + ".html";
            
            if (fetch_to_disk(task.url, temp_file)) {
                auto links = extract_links_from_file(temp_file);
                
                for (auto& link : links) {
                    if (task.depth + 1 <= max_depth) {
                        if (url_store.add_url(link)) {
                            std::lock_guard<std::mutex> qlock(queue_mutex);
                            task_queue.push({std::move(link), task.depth + 1});
                            queue_cv.notify_one();
                        }
                    }
                }
            }
            
            active_tasks--;
            urls_processed++;
        }
    }
    
public:
    DiskBackedCrawler(size_t max_depth = 3, size_t max_concurrent = 50)
        : url_store("./crawler_data"), max_depth(max_depth), max_concurrent(max_concurrent) {
        
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }
    
    ~DiskBackedCrawler() {
        running = false;
        queue_cv.notify_all();
        for (auto& worker : workers) {
            if (worker.joinable()) worker.join();
        }
        curl_global_cleanup();
    }
    
    void start(const std::string& seed_url) {
        auto start_time = std::chrono::steady_clock::now();
        
        // Add seed URL
        url_store.add_url(seed_url);
        task_queue.push({seed_url, 0});
        
        // Start workers
        for (size_t i = 0; i < max_concurrent; i++) {
            workers.emplace_back(&DiskBackedCrawler::worker_function, this);
        }
        
        // Monitor progress
        while (urls_processed < 1000000 && !task_queue.empty()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            
            // Print stats
            std::cout << "\rURLs processed: " << urls_processed 
                      << " | Unique: " << url_store.count()
                      << " | Queue: " << task_queue.size()
                      << " | Active: " << active_tasks << std::flush;
        }
        
        running = false;
        queue_cv.notify_all();
        
        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time);
        
        std::cout << "\n\n=== FINAL STATISTICS ===" << std::endl;
        std::cout << "URLs processed: " << urls_processed << std::endl;
        std::cout << "Unique URLs: " << url_store.count() << std::endl;
        std::cout << "Time: " << duration.count() << " seconds" << std::endl;
        std::cout << "Throughput: " << (urls_processed / duration.count()) << " URLs/sec" << std::endl;
        
        // Memory usage
        struct rusage usage;
        getrusage(RUSAGE_SELF, &usage);
        std::cout << "RAM used: " << usage.ru_maxrss / 1024 << " MB" << std::endl;
        std::cout << "Disk used: ~" << (url_store.count() * 100 / 1024 / 1024) << " MB for URLs" << std::endl;
    }
};

int main() {
    DiskBackedCrawler crawler(3, 50);  // Max depth 3, 50 concurrent bots
    crawler.start("https://example.com");
    return 0;
}

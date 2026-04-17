#include <memory>
#include <queue>
#include <list>
#include <chrono>

class WebCrawler {
private:
    // Use shared_ptr to manage Bot lifecycle
    using BotPtr = std::shared_ptr<struct Bot>;
    
    // Limit queue size
    static constexpr size_t MAX_QUEUE_SIZE = 1000;
    static constexpr size_t MAX_VISITED_URLS = 10000;
    
    // LRU cache for visited URLs (evicts old entries)
    class VisitedLRU {
    private:
        std::unordered_set<std::string> urlSet;
        std::list<std::string> lruList;
        size_t maxSize;
        std::mutex mtx;
        
    public:
        VisitedLRU(size_t max) : maxSize(max) {}
        
        bool contains(const std::string& url) {
            std::lock_guard<std::mutex> lock(mtx);
            return urlSet.find(url) != urlSet.end();
        }
        
        void insert(const std::string& url) {
            std::lock_guard<std::mutex> lock(mtx);
            if (urlSet.size() >= maxSize) {
                // Evict oldest
                std::string oldest = lruList.back();
                lruList.pop_back();
                urlSet.erase(oldest);
            }
            urlSet.insert(url);
            lruList.push_front(url);
        }
        
        size_t size() { return urlSet.size(); }
    } visitedUrls;
    
    // Use move semantics to avoid copies
    void botWorker(int botId, std::string&& url, int depth) {
        // Fetch with streaming to avoid full HTML storage
        std::string html = fetchUrlStreaming(url);  // Custom streaming version
        
        // Reuse string buffers
        static thread_local std::vector<std::string> links;
        links.clear();
        links.reserve(100);  // Pre-allocate
        
        extractLinks(html, url, links);  // Pass by reference
        
        // Use emplace_back to construct in-place
        for (auto& link : links) {
            if (depth + 1 <= maxDepth && !visitedUrls.contains(link)) {
                visitedUrls.insert(link);
                
                // Move link into queue (no copy)
                {
                    std::lock_guard<std::mutex> lock(queueMutex);
                    if (taskQueue.size() < MAX_QUEUE_SIZE) {
                        taskQueue.emplace(++totalBotsSpawned, 
                                        std::move(link),  // MOVE, not copy!
                                        depth + 1);
                        activeBots++;
                    }
                }
            }
        }
        
        // Clear strings immediately
        html.clear();
        html.shrink_to_fit();  // Force memory release
    }
    
    // Streaming fetch (doesn't store entire response)
    std::string fetchUrlStreaming(const std::string& url) {
        // Use curl's write callback that limits size
        std::string response;
        response.reserve(65536);  // Reserve 64KB initially
        
        // Set max receive size (e.g., 2MB)
        curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, 2 * 1024 * 1024L);
        
        // ... rest of fetch
        return response;
    }
    
    // Reuse regex objects (expensive to compile)
    class RegexCache {
        std::regex hrefPattern{R"(href=\"(https?://[^\"]+)\")", 
                               std::regex::icase | std::regex::optimize};
        std::regex hrefPattern2{R"(href='(https?://[^']+)')", 
                                std::regex::icase | std::regex::optimize};
    public:
        void extract(const std::string& html, std::vector<std::string>& out) {
            out.clear();
            // Use regex iterators with pre-compiled patterns
            std::sregex_iterator it(html.begin(), html.end(), hrefPattern);
            // ...
        }
    };
    
    // Object pooling for Bot structures
    class BotPool {
        std::queue<BotPtr> pool;
        std::mutex mtx;
    public:
        BotPtr acquire() {
            std::lock_guard<std::mutex> lock(mtx);
            if (pool.empty()) {
                return std::make_shared<Bot>();
            }
            auto bot = pool.front();
            pool.pop();
            return bot;
        }
        
        void release(BotPtr bot) {
            std::lock_guard<std::mutex> lock(mtx);
            bot->url.clear();
            bot->url.shrink_to_fit();
            if (pool.size() < 100) {
                pool.push(bot);
            }
        }
    };
};

// Memory-efficient queue (fixed capacity)
template<typename T>
class BoundedQueue {
private:
    std::queue<T> queue;
    size_t maxSize;
    std::mutex mtx;
    
public:
    BoundedQueue(size_t max) : maxSize(max) {}
    
    bool push(T&& item) {
        std::lock_guard<std::mutex> lock(mtx);
        if (queue.size() >= maxSize) {
            return false;  // Queue full
        }
        queue.push(std::forward<T>(item));
        return true;
    }
};

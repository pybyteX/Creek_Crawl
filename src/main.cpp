#include <iostream>
#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <regex>
#include <curl/curl.h>
#include <unordered_set>

// Callback for libcurl to write response data
size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* output) {
    size_t totalSize = size * nmemb;
    output->append((char*)contents, totalSize);
    return totalSize;
}

struct Bot {
    int id;
    std::string url;
    int depth;
};

class WebCrawler {
private:
    std::queue<Bot> taskQueue;
    std::unordered_set<std::string> visitedUrls;
    std::mutex queueMutex;
    std::mutex visitedMutex;
    std::condition_variable cv;
    std::atomic<int> activeBots{0};
    std::atomic<bool> stopFlag{false};
    int maxDepth;
    int maxConcurrentBots;
    int totalBotsSpawned{0};
    
    // Extract all HTTP/HTTPS links from HTML
    std::vector<std::string> extractLinks(const std::string& html, const std::string& baseUrl) {
        std::vector<std::string> links;
        std::regex linkRegex(R"(href=\"(https?://[^\"]+)\")", std::regex::icase);
        std::regex linkRegex2(R"(href='(https?://[^']+)')", std::regex::icase);
        
        auto processMatches = [&](const std::regex& regex) {
            std::sregex_iterator it(html.begin(), html.end(), regex);
            std::sregex_iterator end;
            for (; it != end; ++it) {
                std::string link = (*it)[1];
                // Clean up common garbage
                if (link.find("javascript:") == std::string::npos &&
                    link.find("mailto:") == std::string::npos &&
                    link.find("#") != 0) {
                    links.push_back(link);
                }
            }
        };
        
        processMatches(linkRegex);
        processMatches(linkRegex2);
        
        return links;
    }
    
    // Fetch a URL and return HTML content
    std::string fetchUrl(const std::string& url) {
        CURL* curl = curl_easy_init();
        std::string response;
        
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
            
            // Pretend to be Chrome
            curl_easy_setopt(curl, CURLOPT_USERAGENT, 
                "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
            curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip, deflate, br");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
            
            struct curl_slist* headers = nullptr;
            headers = curl_slist_append(headers, "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8");
            headers = curl_slist_append(headers, "Accept-Language: en-US,en;q=0.9");
            headers = curl_slist_append(headers, "Cache-Control: no-cache");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            
            CURLcode res = curl_easy_perform(curl);
            
            if (res != CURLE_OK) {
                std::cerr << "Bot fetch error: " << curl_easy_strerror(res) << std::endl;
                response.clear();
            }
            
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
        }
        
        return response;
    }
    
    // Check if URL already visited
    bool isVisited(const std::string& url) {
        std::lock_guard<std::mutex> lock(visitedMutex);
        return visitedUrls.find(url) != visitedUrls.end();
    }
    
    void markVisited(const std::string& url) {
        std::lock_guard<std::mutex> lock(visitedMutex);
        visitedUrls.insert(url);
    }
    
    // Bot worker function - each bot spawns ONE new bot when it finds links
    void botWorker(int botId, std::string url, int depth) {
        std::cout << "[Bot " << botId << "] Crawling: " << url << " (Depth: " << depth << ")" << std::endl;
        
        // Fetch the page
        std::string html = fetchUrl(url);
        if (html.empty()) {
            std::cout << "[Bot " << botId << "] Failed to fetch or empty response" << std::endl;
            activeBots--;
            cv.notify_one();
            return;
        }
        
        std::cout << "[Bot " << botId << "] Downloaded " << html.length() << " bytes" << std::endl;
        
        // Extract links
        std::vector<std::string> links = extractLinks(html, url);
        std::cout << "[Bot " << botId << "] Found " << links.size() << " links" << std::endl;
        
        // Filter unvisited links
        std::vector<std::string> newLinks;
        for (const auto& link : links) {
            if (!isVisited(link) && depth + 1 <= maxDepth) {
                newLinks.push_back(link);
                markVisited(link);
            }
        }
        
        // Spawn ONE new bot from the first unvisited link (if any)
        if (!newLinks.empty() && !stopFlag) {
            std::string spawnUrl = newLinks[0];  // Each bot spawns exactly one new bot
            int newBotId = ++totalBotsSpawned;
            
            std::cout << "[Bot " << botId << "] 🎯 Spawning new bot " << newBotId 
                      << " to crawl: " << spawnUrl << std::endl;
            
            // Create new task
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                taskQueue.push({newBotId, spawnUrl, depth + 1});
                activeBots++;  // Increment for new bot
            }
            cv.notify_one();
            
            // Optional: Queue remaining links for other bots to pick up
            // This allows for exponential growth
            for (size_t i = 1; i < newLinks.size() && i < 3; i++) {
                int queuedBotId = ++totalBotsSpawned;
                {
                    std::lock_guard<std::mutex> lock(queueMutex);
                    taskQueue.push({queuedBotId, newLinks[i], depth + 1});
                    activeBots++;
                }
                std::cout << "[Bot " << botId << "] 📋 Queued bot " << queuedBotId 
                          << " for: " << newLinks[i] << std::endl;
                cv.notify_one();
            }
        } else {
            std::cout << "[Bot " << botId << "] No new links to spawn bots" << std::endl;
        }
        
        // Bot finishes its work
        activeBots--;
        cv.notify_one();
        
        std::cout << "[Bot " << botId << "] Completed. Active bots: " << activeBots << std::endl;
    }
    
public:
    WebCrawler(int maxDepth = 3, int maxConcurrent = 10) 
        : maxDepth(maxDepth), maxConcurrentBots(maxConcurrent) {}
    
    void start(const std::string& seedUrl) {
        std::cout << "🚀 Starting crawler with Chrome user-agent simulation" << std::endl;
        std::cout << "Max depth: " << maxDepth << ", Max concurrent: " << maxConcurrentBots << std::endl;
        
        // Initialize with first bot
        markVisited(seedUrl);
        taskQueue.push({0, seedUrl, 0});
        activeBots = 1;
        totalBotsSpawned = 0;
        
        std::vector<std::thread> workers;
        
        // Create worker threads that pull from queue
        for (int i = 0; i < maxConcurrentBots; i++) {
            workers.emplace_back([this]() {
                while (!stopFlag) {
                    std::unique_lock<std::mutex> lock(queueMutex);
                    cv.wait(lock, [this]() { 
                        return !taskQueue.empty() || (activeBots == 0 && taskQueue.empty()) || stopFlag;
                    });
                    
                    if (stopFlag || (taskQueue.empty() && activeBots == 0)) {
                        break;
                    }
                    
                    if (taskQueue.empty()) {
                        continue;
                    }
                    
                    Bot bot = taskQueue.front();
                    taskQueue.pop();
                    lock.unlock();
                    
                    // Execute bot
                    botWorker(bot.id, bot.url, bot.depth);
                }
            });
        }
        
        // Wait for all bots to complete
        for (auto& worker : workers) {
            worker.join();
        }
        
        std::cout << "\n✅ Crawling completed!" << std::endl;
        std::cout << "Total unique URLs visited: " << visitedUrls.size() << std::endl;
        std::cout << "Total bots spawned: " << totalBotsSpawned + 1 << std::endl;
    }
    
    void stop() {
        stopFlag = true;
        cv.notify_all();
    }
    
    void printStats() {
        std::cout << "\n📊 Crawler Statistics:" << std::endl;
        std::cout << "Visited URLs:" << std::endl;
        for (const auto& url : visitedUrls) {
            std::cout << "  - " << url << std::endl;
        }
    }
};

int main() {
    // Initialize libcurl globally
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    // Create crawler: max depth 2, max 5 concurrent bots
    WebCrawler crawler(2, 5);
    
    // Start crawling from a test URL (replace with your target)
    // For testing, using a safe example
    std::string seedUrl = "https://example.com";
    
    std::cout << "Press Ctrl+C to stop...\n" << std::endl;
    
    crawler.start(seedUrl);
    
    // Optional: print all visited URLs
    crawler.printStats();
    
    curl_global_cleanup();
    return 0;
}

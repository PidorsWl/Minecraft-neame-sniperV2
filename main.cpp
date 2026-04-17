#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <sstream>
#include <memory>
#include <iomanip>
#include <cctype>
#include <curl/curl.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "wldap32.lib")

const int MAX_THREADS = 1000;
const int TIMEOUT_SEC = 8;
const int SAVE_SEC = 30;
const int MAX_FAIL = 3;
const int DELAY_MS = 10;
const int REACTIVATE_SEC = 30;
const int IDLE_SLEEP_MIN = 60;

std::mutex console_mtx;
std::mutex file_mtx;

std::string url_encode(const std::string& s) {
    std::ostringstream esc;
    esc.fill('0'); esc << std::hex;
    for(char c : s) {
        if(isalnum((unsigned char)c) || c=='-' || c=='_' || c=='.' || c=='~')
            esc << c;
        else
            esc << std::uppercase << '%' << std::setw(2) << int((unsigned char)c) << std::nouppercase;
    }
    return esc.str();
}

struct Proxy {
    std::string host, user, pass;
    int port;
    std::atomic<int> fail{0}, ok{0};
    std::atomic<bool> active{true};
    std::chrono::steady_clock::time_point deactivated_at;
    std::mutex mtx;
    Proxy() : deactivated_at(std::chrono::steady_clock::now()) {}
};

class Queue {
    std::queue<std::string> q;
    std::mutex m;
public:
    void push(const std::string& s) { std::lock_guard<std::mutex> l(m); q.push(s); }
    bool pop(std::string& s) { std::lock_guard<std::mutex> l(m); if(q.empty()) return false; s=q.front(); q.pop(); return true; }
    size_t size() { std::lock_guard<std::mutex> l(m); return q.size(); }
    bool empty() { std::lock_guard<std::mutex> l(m); return q.empty(); }
    void clear() { std::lock_guard<std::mutex> l(m); while(!q.empty()) q.pop(); }
    void save(const std::string& f) {
        std::lock_guard<std::mutex> l(m);
        std::ofstream o(f); auto t=q; while(!t.empty()){ o<<t.front()<<"\n"; t.pop(); }
    }
    void load(const std::string& f) {
        std::lock_guard<std::mutex> l(m);
        std::ifstream i(f); std::string s; while(std::getline(i,s)) if(!s.empty()) q.push(s);
    }
};

class State {
    std::mutex m;
    std::vector<std::string> checked, avail;
    std::atomic<size_t> chk{0}, ava{0};
public:
    void add(const std::string& u, bool taken) {
        std::lock_guard<std::mutex> l(m); checked.push_back(u); ++chk;
        if(!taken) { avail.push_back(u); ++ava; }
    }
    size_t checked_cnt() const { return chk; }
    size_t avail_cnt() const { return ava; }
    void save_progress(const std::string& base) {
        std::lock_guard<std::mutex> l(m);
        std::ofstream p(base+".progress"); for(auto& s: checked) p<<s<<"\n";
        std::ofstream a(base+".available", std::ios::app); for(auto& s: avail) a<<s<<"\n";
        avail.clear();
    }
    void load_progress(const std::string& base) {
        std::lock_guard<std::mutex> l(m);
        std::ifstream p(base+".progress"); std::string s;
        while(std::getline(p,s)) if(!s.empty()) { checked.push_back(s); ++chk; }
    }
    void clear() {
        std::lock_guard<std::mutex> l(m);
        checked.clear(); avail.clear(); chk=0; ava=0;
    }
    void print(size_t qs, size_t act) {
        std::lock_guard<std::mutex> l(console_mtx);
        std::cout<<"\rchecked:"<<chk<<" avail:"<<ava<<" queue:"<<qs<<" active:"<<act<<"   "<<std::flush;
    }
};

std::vector<std::string> gen_names() {
    static const std::string c="abcdefghijklmnopqrstuvwxyz0123456789_";
    std::vector<std::string> v; v.reserve(c.size()*c.size()*c.size());
    for(char a:c) for(char b:c) for(char d:c) v.push_back(std::string()+a+b+d);
    return v;
}

static size_t write_cb(void* p, size_t s, size_t n, void* u) {
    ((std::string*)u)->append((char*)p, s*n); return s*n;
}

enum class R { Taken, Available, Error };

R check(const std::string& name, Proxy* px) {
    CURL* c=curl_easy_init(); if(!c) return R::Error;
    std::string url="https://api.mojang.com/users/profiles/minecraft/"+name, body;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, (long)TIMEOUT_SEC);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    std::string pxy=px->host+":"+std::to_string(px->port);
    curl_easy_setopt(c, CURLOPT_PROXY, pxy.c_str());
    curl_easy_setopt(c, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5_HOSTNAME);
    if(!px->user.empty()) {
        std::string cred = url_encode(px->user) + ":" + url_encode(px->pass);
        curl_easy_setopt(c, CURLOPT_PROXYUSERPWD, cred.c_str());
    }
    CURLcode r=curl_easy_perform(c); long http=0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http); curl_easy_cleanup(c);
    if(r!=CURLE_OK) return R::Error;
    if(http==200) return R::Taken;
    if(http==204||http==404) return R::Available;
    return R::Error;
}

void worker(Proxy* px, Queue* q, State* st, std::atomic<bool>* run, std::atomic<int>* act) {
    bool was_active = true;
    while(run->load()) {
        if(!px->active.load()) {
            auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> l(px->mtx);
            if(std::chrono::duration_cast<std::chrono::seconds>(now - px->deactivated_at).count() >= REACTIVATE_SEC) {
                px->active = true;
                px->fail = 0;
                ++(*act);
                std::lock_guard<std::mutex> lc(console_mtx);
                std::cout<<"\n[reactivated] "<<px->host<<":"<<px->port<<"\n";
                was_active = true;
            }
        }
        if(!px->active.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        std::string u;
        if(!q->pop(u)) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); continue; }

        R res = check(u, px);
        if(res == R::Error) {
            if(++px->fail >= MAX_FAIL) {
                px->active = false;
                --(*act);
                {
                    std::lock_guard<std::mutex> l(px->mtx);
                    px->deactivated_at = std::chrono::steady_clock::now();
                }
                q->push(u);
                if(was_active) {
                    std::lock_guard<std::mutex> lc(console_mtx);
                    std::cout<<"\n[deactivated] "<<px->host<<":"<<px->port<<"\n";
                    was_active = false;
                }
            } else {
                q->push(u);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        px->fail = 0;
        ++px->ok;
        bool taken = (res == R::Taken);
        st->add(u, taken);
        if(!taken) {
            std::lock_guard<std::mutex> l(file_mtx);
            std::ofstream f("available.txt", std::ios::app);
            f << u << "\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(DELAY_MS));
    }
}

void saver(State* st, Queue* q, std::atomic<bool>* run) {
    while(run->load()) {
        std::this_thread::sleep_for(std::chrono::seconds(SAVE_SEC));
        st->save_progress("state");
        q->save("state.queue");
        std::lock_guard<std::mutex> l(console_mtx);
        std::cout<<"\n[saved]\n";
    }
}

int main(int argc, char* argv[]) {
    curl_global_init(CURL_GLOBAL_ALL);
    bool resume = false;
    std::string pf = "proxies.txt";
    for(int i=1; i<argc; ++i) {
        if(std::string(argv[i]) == "/resume") resume = true;
        else pf = argv[i];
    }

    std::vector<std::unique_ptr<Proxy>> proxies;
    {
        std::ifstream f(pf);
        std::string l;
        while(std::getline(f, l)) {
            if(l.empty() || l[0]=='#') continue;
            std::vector<std::string> p;
            std::stringstream ss(l);
            std::string x;
            while(std::getline(ss, x, ':')) p.push_back(x);
            if(p.size() >= 4) {
                auto px = std::make_unique<Proxy>();
                px->host = p[0];
                px->port = std::stoi(p[1]);
                px->user = p[2];
                px->pass = p[3];
                for(size_t i=4; i<p.size(); ++i) px->pass += ":" + p[i];
                proxies.push_back(std::move(px));
            }
        }
    }
    if(proxies.empty()) { std::cerr<<"no proxies\n"; system("pause"); return 1; }

    auto all_names = gen_names();
    State st;
    Queue q;

    if(resume) {
        st.load_progress("state");
        q.load("state.queue");
        std::cout<<"resumed checked:"<<st.checked_cnt()<<" queue:"<<q.size()<<"\n";
    } else {
        for(auto& n : all_names) q.push(n);
        std::cout<<"new run total:"<<all_names.size()<<"\n";
    }

    if(q.empty()) {
        for(auto& n : all_names) q.push(n);
        st.clear();
        std::cout<<"restarting fresh\n";
    }

    std::cout<<"proxies:"<<proxies.size()<<" threads:"<<proxies.size()<<"\n";

    std::atomic<bool> run(true);
    std::atomic<int> act((int)proxies.size());
    std::vector<std::thread> workers;
    std::thread sav(saver, &st, &q, &run);

    for(auto& px : proxies)
        workers.emplace_back(worker, px.get(), &q, &st, &run, &act);

    while(run.load()) {
        st.print(q.size(), act.load());
        if(q.empty()) {
            std::cout<<"\nqueue empty, sleeping "<<IDLE_SLEEP_MIN<<" min...\n";
            st.save_progress("state"); q.save("state.queue");
            std::this_thread::sleep_for(std::chrono::minutes(IDLE_SLEEP_MIN));
            for(auto& n : all_names) q.push(n);
            st.clear();
            std::cout<<"restarting fresh\n";
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    run = false;
    for(auto& t : workers) if(t.joinable()) t.join();
    sav.join();

    st.save_progress("state");
    q.save("state.queue");
    std::cout<<"\ndone checked:"<<st.checked_cnt()<<" avail:"<<st.avail_cnt()<<"\n";
    curl_global_cleanup();
    return 0;
}
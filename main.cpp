#include <iostream>
#include <fstream>
#include <string>
#include <unistd.h>
#include <dirent.h>
#include <vector>
#include <algorithm>
#include <sstream>
#include <unordered_map>
#include <cctype>
#include <csignal>
#include <sys/types.h>
#include <ncurses.h>
#include <cstdio>

struct ProcessInfo {
    int pid;
    std::string name;
    float cpu;
    float mem;
};

static inline unsigned long long readTotalCPU() {
    std::ifstream file("/proc/stat");
    std::string line;
    if (!std::getline(file, line)) return 0;
    unsigned long long u,n,s,i,io,iq,si,st;
    if (sscanf(line.c_str(),"cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               &u,&n,&s,&i,&io,&iq,&si,&st) != 8) return 0;
    return u+n+s+i+io+iq+si+st;
}

static inline unsigned long long readProcCPU(int pid) {
    std::string path = "/proc/" + std::to_string(pid) + "/stat";
    std::ifstream f(path);
    if (!f) return 0;
    std::string line;
    std::getline(f, line);
    if (line.empty()) return 0;
    // comm is in parentheses; find last ')'
    size_t rparen = line.rfind(')');
    if (rparen == std::string::npos) return 0;
    std::string after = line.substr(rparen + 2); // skip ") "

    // In "after": tokens start at stat field 3; we need utime(14) & stime(15)
    // relative to whole. Within "after", utime is 12th, stime is 13th.
    std::istringstream iss(after);
    std::string tok;
    unsigned long long ut=0, st=0;
    for (int i=1; i<=13; ++i) {
        if (!(iss >> tok)) return 0;
        if (i==12) ut = strtoull(tok.c_str(), nullptr, 10);
        if (i==13) st = strtoull(tok.c_str(), nullptr, 10);
    }
    return ut + st;
}

static inline std::string readProcName(int pid) {
    std::string path = "/proc/" + std::to_string(pid) + "/comm";
    std::ifstream f(path);
    std::string name;
    std::getline(f, name);
    if (!name.empty() && (name.back()=='\n' || name.back()=='\r')) name.pop_back();
    if (name.empty()) name = std::to_string(pid);
    return name;
}

static inline long totalMemKB() {
    std::ifstream f("/proc/meminfo");
    std::string key, unit;
    long val;
    while (f >> key >> val >> unit) {
        if (key == "MemTotal:") return val;
    }
    return 1;
}
static inline float memUsagePercent() {
    // Use MemAvailable for realistic usage %
    std::ifstream f("/proc/meminfo");
    std::string key, unit;
    long memTotal=0, memAvail=0, val=0;
    while (f >> key >> val >> unit) {
        if (key == "MemTotal:") memTotal = val;
        if (key == "MemAvailable:") memAvail = val;
        if (memTotal && memAvail) break;
    }
    if (memTotal <= 0) return 0.f;
    long used = memTotal - memAvail;
    return (float)used / (float)memTotal * 100.0f;
}
static inline float procMemPercent(int pid, long memTotalKB_) {
    std::string path = "/proc/" + std::to_string(pid) + "/statm";
    std::ifstream f(path);
    if (!f) return 0.f;
    unsigned long rssPages=0; f >> rssPages;
    long pageKB = sysconf(_SC_PAGESIZE)/1024;
    float usedKB = (float)rssPages * (float)pageKB;
    if (memTotalKB_ <= 0) return 0.f;
    return usedKB / (float)memTotalKB_ * 100.0f;
}

static inline std::vector<int> listPIDs() {
    std::vector<int> out;
    DIR* dir = opendir("/proc");
    if (!dir) return out;
    dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (std::isdigit(ent->d_name[0])) out.push_back(atoi(ent->d_name));
    }
    closedir(dir);
    return out;
}

struct Snapshot {
    unsigned long long totalCPU_prev{0};
    std::unordered_map<int, unsigned long long> procCPU_prev;
};

static inline std::vector<ProcessInfo> collectProcesses(long memKB,
    Snapshot &snap, unsigned long long &totalCPU_now)
{
    std::vector<ProcessInfo> plist;
    totalCPU_now = readTotalCPU();
    auto pids = listPIDs();
    unsigned long long totalDiff = (totalCPU_now > snap.totalCPU_prev) ?
                                   (totalCPU_now - snap.totalCPU_prev) : 1ULL;

    std::unordered_map<int, unsigned long long> nextPrev = snap.procCPU_prev;

    for (int pid : pids) {
        std::string name = readProcName(pid);
        if (name.empty()) continue;

        unsigned long long cur = readProcCPU(pid);
        unsigned long long prev = 0;
        auto it = snap.procCPU_prev.find(pid);
        if (it != snap.procCPU_prev.end()) prev = it->second;

        nextPrev[pid] = cur;

        float cpu = 0.f;
        if (cur >= prev && totalDiff > 0) {
            cpu = (float)(cur - prev) / (float)totalDiff * 100.0f;
        }
        float mem = procMemPercent(pid, memKB);
        plist.push_back({pid, name, cpu, mem});
    }

    snap.procCPU_prev.swap(nextPrev);
    snap.totalCPU_prev = totalCPU_now;
    return plist;
}


static inline int colorCPU(float cpu) {
    if (cpu >= 70.f) return 3;     // red
    if (cpu >= 25.f) return 2;     // yellow
    return 1;                      // green
}
static inline int colorMEM(float mem) {
    if (mem >= 15.f) return 3;     // red
    if (mem >= 4.f)  return 2;     // yellow
    return 1;                      // green
}

static inline float cpuUsagePercent(float &lastCPU, unsigned long long &lastTotal) {
    static unsigned long long prevIdle=0, prevTotal=0;
    std::ifstream f("/proc/stat");
    std::string line; std::getline(f, line);
    unsigned long long u,n,s,i,io,iq,si,st;
    if (sscanf(line.c_str(),"cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               &u,&n,&s,&i,&io,&iq,&si,&st)!=8) return 0.f;
    unsigned long long idle = i + io;
    unsigned long long total = u+n+s+i+io+iq+si+st;
    unsigned long long dIdle = idle - prevIdle;
    unsigned long long dTotal = total - prevTotal;
    prevIdle = idle; prevTotal = total;
    if (dTotal == 0) return 0.f;
    float pct = (float)(dTotal - dIdle) / (float)dTotal * 100.0f;
    lastCPU = pct; lastTotal = total;
    return pct;
}


static inline void drawHeader(bool sortByCPU, int rows, int cols,
                              float cpuPct, float memPct, bool alert) {
    attron(A_BOLD);
    mvprintw(0, 0, " SMART SYSTEM MONITOR (Day 4 - ncurses) ");
    attroff(A_BOLD);
    mvprintw(0, 40, "%s", sortByCPU ? "[Sorting: CPU%%]" : "[Sorting: MEM%%]");
    mvprintw(1, 0, "[q] quit  [t] toggle sort  [k] kill PID  [c] color/self-test");
    mvprintw(2, 0, "CPU: %5.1f%%   MEM: %5.1f%%", cpuPct, memPct);

    
    if (alert) {
        attron(COLOR_PAIR(3) | A_BOLD); // red
        mvprintw(2, 24, "  ALERT: High usage detected!  ");
        attroff(COLOR_PAIR(3) | A_BOLD);
    }

    
    attron(COLOR_PAIR(4) | A_BOLD); // cyan
    mvprintw(4, 0,  "PID");
    mvprintw(4, 8,  "NAME");
    mvprintw(4, 32, "CPU%%");
    mvprintw(4, 40, "MEM%%");
    attroff(COLOR_PAIR(4) | A_BOLD);

    
    mvprintw(rows-1, 0, "Legend: ");
    attron(COLOR_PAIR(1)); printw("Green=Normal "); attroff(COLOR_PAIR(1));
    attron(COLOR_PAIR(2)); printw("Yellow=Medium "); attroff(COLOR_PAIR(2));
    attron(COLOR_PAIR(3)); printw("Red=High"); attroff(COLOR_PAIR(3));
}


static inline void colorSelfTest(int cols) {
    int row = 6;
    attron(A_BOLD); mvprintw(row, 0, "Color/Self-Test:"); attroff(A_BOLD);
    row += 1;
    attron(COLOR_PAIR(1)); mvprintw(row++, 0, "Green OK"); attroff(COLOR_PAIR(1));
    attron(COLOR_PAIR(2)); mvprintw(row++, 0, "Yellow OK"); attroff(COLOR_PAIR(2));
    attron(COLOR_PAIR(3)); mvprintw(row++, 0, "Red OK"); attroff(COLOR_PAIR(3));
    attron(COLOR_PAIR(4)); mvprintw(row++, 0, "Cyan Header OK"); attroff(COLOR_PAIR(4));
    mvprintw(row, 0, "Press any key to continue...");
    nodelay(stdscr, FALSE); getch(); nodelay(stdscr, TRUE);
}

int main() {
    
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);  
    curs_set(0);

    if (has_colors()) {
        start_color();
        // 1=green, 2=yellow, 3=red, 4=cyan
        init_pair(1, COLOR_GREEN, COLOR_BLACK);
        init_pair(2, COLOR_YELLOW, COLOR_BLACK);
        init_pair(3, COLOR_RED, COLOR_BLACK);
        init_pair(4, COLOR_CYAN, COLOR_BLACK);
    }

    bool sortByCPU = true;
    Snapshot snap{};
    
    snap.totalCPU_prev = readTotalCPU();
    usleep(300*1000);

    long memKB = totalMemKB();
    float lastCpuPct=0.f; unsigned long long lastTotal=0;

    while (true) {
        int rows, cols; getmaxyx(stdscr, rows, cols);
        clear();

        float cpuPct = cpuUsagePercent(lastCpuPct, lastTotal);
        float memPct = memUsagePercent();

        unsigned long long totalNow=0;
        auto plist = collectProcesses(memKB, snap, totalNow);

        if (sortByCPU) {
            std::sort(plist.begin(), plist.end(),
                [](const ProcessInfo& a, const ProcessInfo& b){ return a.cpu > b.cpu; });
        } else {
            std::sort(plist.begin(), plist.end(),
                [](const ProcessInfo& a, const ProcessInfo& b){ return a.mem > b.mem; });
        }

        bool alert = false;
        for (const auto& p : plist) {
            if (p.cpu >= 70.f || p.mem >= 15.f) { alert = true; break; }
        }

        drawHeader(sortByCPU, rows, cols, cpuPct, memPct, alert);

        int startRow = 6;
        int maxRows = rows - startRow - 2;
        if (maxRows < 1) maxRows = 1;
        int shown = 0;
        for (const auto& p : plist) {
            if (shown >= maxRows) break;
            int row = startRow + shown;
            mvprintw(row, 0,  "%-7d", p.pid);

            
            std::string name = p.name;
            if ((int)name.size() > 22) name = name.substr(0, 22);
            mvprintw(row, 8,  "%-22s", name.c_str());

            int c1 = colorCPU(p.cpu);
            attron(COLOR_PAIR(c1));
            mvprintw(row, 32, "%6.1f", p.cpu);
            attroff(COLOR_PAIR(c1));

            int c2 = colorMEM(p.mem);
            attron(COLOR_PAIR(c2));
            mvprintw(row, 40, "%6.1f", p.mem);
            attroff(COLOR_PAIR(c2));

            shown++;
        }

        
        int ch = getch();
        if (ch != ERR) {
            if (ch=='q' || ch=='Q') {
                endwin();
                return 0;
            }
            if (ch=='t' || ch=='T') {
                sortByCPU = !sortByCPU;
            }
            if (ch=='k' || ch=='K') {
                
                echo();
                nodelay(stdscr, FALSE);
                curs_set(1);
                char buf[32] = {0};
                mvprintw(rows-3, 0, "Enter PID to kill (SIGTERM): ");
                getnstr(buf, 31);
                noecho();
                nodelay(stdscr, TRUE);
                curs_set(0);
                int pid = 0;
                try { pid = std::stoi(std::string(buf)); } catch (...) { pid = 0; }
                if (pid > 1) {
                    int rc = kill(pid, SIGTERM);
                    if (rc == 0) {
                        attron(COLOR_PAIR(1));
                        mvprintw(rows-2, 0, "Sent SIGTERM to PID %d", pid);
                        attroff(COLOR_PAIR(1));
                    } else {
                        attron(COLOR_PAIR(3));
                        mvprintw(rows-2, 0, "kill(%d) failed: %m", pid);
                        attroff(COLOR_PAIR(3));
                    }
                } else {
                    attron(COLOR_PAIR(2));
                    mvprintw(rows-2, 0, "Invalid PID.");
                    attroff(COLOR_PAIR(2));
                }
            }
            if (ch=='c' || ch=='C') {
                colorSelfTest(cols);
            }
        }

        
        // Here we do a single nap; ncurses keeps it smooth without flicker.
        usleep(1800 * 1000);
        refresh();
    }
    endwin();
    return 0;
}

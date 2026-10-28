#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <random>
#include <ctime>
#include <cstdint>
#include <cstdlib>
#include <filesystem>

using namespace std;

// how long one cpu tick lasts in real time (milliseconds)
const int TICK_MS = 100;

const string DEVELOPERS = "Justin David, Kryster Limpin, Railey Singson";
const string LAST_UPDATED = "10-28-2026";

// Settings loaded from config.txt
struct Config {
    int numCpu = 4;
    string scheduler = "rr";
    long long quantum = 5;
    long long batchFreq = 1;
    long long minIns = 1000;
    long long maxIns = 2000;
    long long delay = 0;
};

// One instruction that a process can run
struct Instruction {
    string type;               // PRINT, DECLARE, ADD, SUBTRACT, SLEEP or FOR
    vector<string> args;       // the operands of the instruction
    vector<Instruction> body;  // only used by FOR
    int repeats = 0;           // only used by FOR
};

// A process and everything it needs while running
struct Process {
    string name;
    int id = 0;
    string createdAt;
    string finishedAt;
    vector<Instruction> code;    // FOR loops are already unrolled here
    int pc = 0;                  // the current instruction line
    map<string, uint16_t> vars;  // the process memory
    vector<string> logs;         // output of PRINT
    int core = -1;               // -1 means it's not on a core
    bool finished = false;
    long long wakeTick = 0;      // tick when a sleeping process can run again
    long long delayLeft = 0;     // busy waiting ticks before the next instruction
};

Config config;
mutex mtx;                    // guards everything below
vector<Process*> processes;   // every process ever made
deque<Process*> readyQueue;
vector<Process*> sleeping;
vector<Process*> cores;       // what each core is running, nullptr if idle
vector<long long> coreTicks;  // how long the current process has been on each core
long long cpuTicks = 0;
bool generating = false;
int nextId = 1;
int nextBatchNum = 1;

atomic<bool> running(true);
bool initialized = false;
thread schedulerThread;
mt19937 rng((unsigned)time(nullptr));

// Returns a random number from lo to hi
long long randRange(long long lo, long long hi) {
    uniform_int_distribution<long long> dist(lo, hi);
    return dist(rng);
}

// Returns the current time like 01/18/2024 09:15:22AM
string timestamp() {
    time_t now = time(nullptr);
    char buf[32];
    strftime(buf, sizeof(buf), "%m/%d/%Y %I:%M:%S%p", localtime(&now));
    return buf;
}

// Keeps a value inside the uint16 range
uint16_t clamp16(long long value) {
    if (value < 0) return 0;
    if (value > 65535) return 65535;
    return (uint16_t)value;
}

// Clears the console
void clearScreen() {
#ifdef _WIN32
    system("cls");
#else
    system("clear");
#endif
}

// Prints the CSOPESY banner
void printHeader() {
    cout << R"ART(
  ____ ____   ___  ____  _____ ______   __
 / ___/ ___| / _ \|  _ \| ____/ ___\ \ / /
| |   \___ \| | | | |_) |  _| \___ \\ V /
| |___ ___) | |_| |  __/| |___ ___) || |
 \____|____/ \___/|_|   |_____|____/ |_|
)ART";
    cout << "------------------------------------------\n";
    cout << "Welcome to CSOPESY Emulator!\n\n";
    cout << "Developers:\n" << DEVELOPERS << "\n\n";
    cout << "Last updated: " << LAST_UPDATED << "\n";
    cout << "------------------------------------------\n\n";
}

// Reads config.txt, returns false if the file is missing
bool loadConfig() {
    ifstream file("config.txt");
    if (!file) return false;

    string key, value;
    while (file >> key >> value) {
        // take off the quotes around values like "rr"
        if (value.size() >= 2 && value.front() == '"') value = value.substr(1, value.size() - 2);

        if (key == "num-cpu") config.numCpu = stoi(value);
        else if (key == "scheduler") config.scheduler = value;
        else if (key == "quantum-cycles") config.quantum = stoll(value);
        else if (key == "batch-process-freq") config.batchFreq = stoll(value);
        else if (key == "min-ins") config.minIns = stoll(value);
        else if (key == "max-ins") config.maxIns = stoll(value);
        else if (key == "delay-per-exec" || key == "delays-per-exec") config.delay = stoll(value);
    }

    // keep the values in their allowed ranges
    if (config.numCpu < 1) config.numCpu = 1;
    if (config.numCpu > 128) config.numCpu = 128;
    if (config.quantum < 1) config.quantum = 1;
    if (config.batchFreq < 1) config.batchFreq = 1;
    if (config.minIns < 1) config.minIns = 1;
    if (config.maxIns < config.minIns) config.maxIns = config.minIns;
    if (config.delay < 0) config.delay = 0;
    return true;
}

// Finds a process by name, nullptr if it doesn't exist
Process* findProcess(const string& name) {
    for (Process* p : processes)
        if (p->name == name) return p;
    return nullptr;
}

// Makes a random instruction, FOR loops can nest up to 3 deep
Instruction randomInstruction(const string& procName, int depth) {
    const string varNames[] = {"x", "y", "z"};
    Instruction ins;

    // only allow FOR if we haven't hit the nesting limit yet
    int pick = (int)randRange(0, depth < 3 ? 5 : 4);

    if (pick == 0) {
        ins.type = "PRINT";
        ins.args = {"Hello world from " + procName + "!"};
    } else if (pick == 1) {
        ins.type = "DECLARE";
        ins.args = {varNames[randRange(0, 2)], to_string(randRange(0, 100))};
    } else if (pick == 2 || pick == 3) {
        ins.type = (pick == 2) ? "ADD" : "SUBTRACT";
        // second operand is a variable, third one is a plain number
        ins.args = {varNames[randRange(0, 2)], varNames[randRange(0, 2)], to_string(randRange(0, 100))};
    } else if (pick == 4) {
        ins.type = "SLEEP";
        ins.args = {to_string(randRange(1, 5))};
    } else {
        ins.type = "FOR";
        ins.repeats = (int)randRange(1, 3);
        int bodySize = (int)randRange(1, 3);
        for (int i = 0; i < bodySize; i++)
            ins.body.push_back(randomInstruction(procName, depth + 1));
    }
    return ins;
}

// Unrolls FOR loops into a flat list so the cpu can run them one at a time
void flatten(const Instruction& ins, vector<Instruction>& out) {
    if (ins.type == "FOR") {
        for (int r = 0; r < ins.repeats; r++)
            for (const Instruction& inner : ins.body) flatten(inner, out);
    } else {
        out.push_back(ins);
    }
}

// Creates a process with random instructions and puts it in the ready queue
Process* createProcess(const string& name) {
    Process* p = new Process();
    p->name = name;
    p->id = nextId++;
    p->createdAt = timestamp();

    // keep adding random instructions until we reach the target count
    long long count = randRange(config.minIns, config.maxIns);
    while ((long long)p->code.size() < count)
        flatten(randomInstruction(name, 0), p->code);
    p->code.resize((size_t)count);  // cut off extra lines from the last loop

    processes.push_back(p);
    readyQueue.push_back(p);
    return p;
}

// Gives the next free name like p01, p02 and so on
string nextBatchName() {
    while (true) {
        stringstream ss;
        ss << "p" << setw(2) << setfill('0') << nextBatchNum++;
        if (findProcess(ss.str()) == nullptr) return ss.str();
    }
}

// Reads a number or a variable, unknown variables become 0
long long getValue(Process* p, const string& s) {
    if (isdigit((unsigned char)s[0])) return stoll(s);
    return p->vars[s];  // map makes it 0 if it's not declared yet
}

// Runs the current instruction of a process on the given core
void execute(Process* p, int coreId) {
    Instruction& ins = p->code[p->pc];

    if (ins.type == "PRINT") {
        string msg = ins.args[0];
        // tack on the variable's value if one was given
        if (ins.args.size() > 1) msg += to_string(getValue(p, ins.args[1]));
        p->logs.push_back("(" + timestamp() + ") Core:" + to_string(coreId) + " \"" + msg + "\"");
    } else if (ins.type == "DECLARE") {
        p->vars[ins.args[0]] = clamp16(stoll(ins.args[1]));
    } else if (ins.type == "ADD") {
        p->vars[ins.args[0]] = clamp16(getValue(p, ins.args[1]) + getValue(p, ins.args[2]));
    } else if (ins.type == "SUBTRACT") {
        p->vars[ins.args[0]] = clamp16(getValue(p, ins.args[1]) - getValue(p, ins.args[2]));
    } else if (ins.type == "SLEEP") {
        p->wakeTick = cpuTicks + stoll(ins.args[0]);
    }

    p->pc++;
}

// Takes a process off its core
void removeFromCore(int c) {
    cores[c]->core = -1;
    cores[c] = nullptr;
}

// Background loop that ticks the cpu and runs the processes on each core
void schedulerLoop() {
    while (running) {
        {
            lock_guard<mutex> lock(mtx);

            // make a new dummy process every batch-process-freq ticks
            if (generating && cpuTicks % config.batchFreq == 0)
                createProcess(nextBatchName());

            // move processes that are done sleeping back to the ready queue
            for (size_t i = 0; i < sleeping.size();) {
                if (sleeping[i]->wakeTick <= cpuTicks) {
                    readyQueue.push_back(sleeping[i]);
                    sleeping.erase(sleeping.begin() + i);
                } else {
                    i++;
                }
            }

            for (int c = 0; c < config.numCpu; c++) {
                // idle core grabs the next process in line
                if (cores[c] == nullptr && !readyQueue.empty()) {
                    cores[c] = readyQueue.front();
                    readyQueue.pop_front();
                    cores[c]->core = c;
                    coreTicks[c] = 0;
                }

                Process* p = cores[c];
                if (p == nullptr) continue;
                coreTicks[c]++;

                // busy waiting, the process keeps the core but does nothing
                if (p->delayLeft > 0) {
                    p->delayLeft--;
                } else {
                    execute(p, c);
                    p->delayLeft = config.delay;
                }

                if (p->pc >= (int)p->code.size()) {
                    // no more instructions left
                    p->finished = true;
                    p->finishedAt = timestamp();
                    removeFromCore(c);
                } else if (p->wakeTick > cpuTicks) {
                    // it just went to sleep so it gives up the core
                    sleeping.push_back(p);
                    removeFromCore(c);
                } else if (config.scheduler == "rr" && coreTicks[c] >= config.quantum) {
                    // time slice is used up, back of the line
                    readyQueue.push_back(p);
                    removeFromCore(c);
                }
            }

            cpuTicks++;
        }
        this_thread::sleep_for(chrono::milliseconds(TICK_MS));
    }
}

// Builds the cpu usage and process list for screen -ls and report-util
string buildReport() {
    lock_guard<mutex> lock(mtx);
    stringstream ss;

    int used = 0;
    for (Process* p : cores)
        if (p != nullptr) used++;

    ss << "CPU utilization: " << used * 100 / config.numCpu << "%\n";
    ss << "Cores used: " << used << "\n";
    ss << "Cores available: " << config.numCpu - used << "\n\n";
    ss << "------------------------------------------\n";

    ss << "Running processes:\n";
    for (Process* p : processes) {
        if (p->core == -1) continue;
        ss << left << setw(12) << p->name << "(" << p->createdAt << ")    Core: "
           << setw(6) << p->core << p->pc << " / " << p->code.size() << "\n";
    }

    ss << "\nFinished processes:\n";
    for (Process* p : processes) {
        if (!p->finished) continue;
        ss << left << setw(12) << p->name << "(" << p->finishedAt << ")    Finished    "
           << p->pc << " / " << p->code.size() << "\n";
    }
    ss << "------------------------------------------\n";
    return ss.str();
}

// Prints the process details for process-smi
void printProcessSmi(Process* p) {
    lock_guard<mutex> lock(mtx);
    cout << "\nProcess name: " << p->name << "\n";
    cout << "ID: " << p->id << "\n";
    cout << "Logs:\n";
    for (const string& log : p->logs) cout << log << "\n";
    cout << "\n";

    if (p->finished) {
        cout << "Finished!\n\n";
    } else {
        cout << "Current instruction line: " << p->pc << "\n";
        cout << "Lines of code: " << p->code.size() << "\n\n";
    }
}

// Opens a process screen until the user types exit
void openScreen(Process* p) {
    clearScreen();
    printProcessSmi(p);

    string cmd;
    while (true) {
        cout << "root:\\> ";
        if (!getline(cin, cmd)) break;

        if (cmd == "process-smi") printProcessSmi(p);
        else if (cmd == "exit") break;
        else if (!cmd.empty()) cout << "Command not recognized\n";
    }

    // go back to the main menu
    clearScreen();
    printHeader();
}

int main() {
    printHeader();

    string line;
    while (true) {
        cout << "root:\\> ";
        if (!getline(cin, line)) break;

        // split the line into command, flag and name
        stringstream ss(line);
        string cmd, flag, name;
        ss >> cmd >> flag >> name;

        if (cmd.empty()) continue;
        if (cmd == "exit") break;

        if (cmd == "initialize") {
            if (initialized) {
                cout << "Already initialized\n";
            } else if (!loadConfig()) {
                cout << "Could not open config.txt\n";
            } else {
                cores.assign(config.numCpu, nullptr);
                coreTicks.assign(config.numCpu, 0);
                initialized = true;
                schedulerThread = thread(schedulerLoop);
                cout << "Initialized with " << config.numCpu << " CPU(s) using " << config.scheduler << " scheduler\n";
            }
        } else if (!initialized) {
            cout << "Please type initialize first\n";
        } else if (cmd == "screen" && flag == "-s" && !name.empty()) {
            Process* p = nullptr;
            {
                lock_guard<mutex> lock(mtx);
                if (findProcess(name) == nullptr) p = createProcess(name);
            }
            if (p == nullptr) cout << "Process " << name << " already exists\n";
            else openScreen(p);
        } else if (cmd == "screen" && flag == "-r" && !name.empty()) {
            Process* p = nullptr;
            {
                lock_guard<mutex> lock(mtx);
                p = findProcess(name);
                // finished processes can't be opened anymore
                if (p != nullptr && p->finished) p = nullptr;
            }
            if (p == nullptr) cout << "Process " << name << " not found.\n";
            else openScreen(p);
        } else if (cmd == "screen" && flag == "-ls") {
            cout << buildReport();
        } else if (cmd == "scheduler-start") {
            lock_guard<mutex> lock(mtx);
            if (generating) cout << "Scheduler is already generating processes\n";
            else { generating = true; cout << "Started generating processes\n"; }
        } else if (cmd == "scheduler-stop") {
            lock_guard<mutex> lock(mtx);
            if (!generating) cout << "Scheduler is not generating processes\n";
            else { generating = false; cout << "Stopped generating processes\n"; }
        } else if (cmd == "report-util") {
            ofstream out("csopesy-log.txt");
            out << buildReport();
            // show the full path so the user knows where the file went
            cout << "Report generated at " << filesystem::absolute("csopesy-log.txt").string() << "!\n";
        } else {
            cout << "Command not recognized\n";
        }
    }

    // stop the scheduler thread before closing
    running = false;
    if (schedulerThread.joinable()) schedulerThread.join();
    return 0;
}

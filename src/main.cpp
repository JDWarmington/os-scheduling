#include <iostream>
#include <string>
#include <list>
#include <vector>
#include <chrono>
#include <thread>
#include <mutex>
#include <algorithm>
#include <cstdlib>
#include <ncurses.h>
#include "configreader.h"
#include "process.h"

// Shared data for all cores
typedef struct SchedulerData {
    std::mutex queue_mutex;
    ScheduleAlgorithm algorithm;
    uint32_t context_switch;
    uint32_t time_slice;
    std::list<Process*> ready_queue;
    bool all_terminated;
} SchedulerData;

// Forward declarations
void insertReadyQueue(SchedulerData *data, Process *process);
void coreRunProcesses(uint8_t core_id, SchedulerData *data);
void printProcessOutput(std::vector<Process*>& processes);
std::string makeProgressString(double percent, uint32_t width);
uint64_t currentTime();
std::string processStateToString(Process::State state);

// ---------------------------------------------------------------------------
// insertReadyQueue
//   Inserts a process into the ready queue per the scheduling algorithm.
//   MUST be called while queue_mutex is already held (no internal locking).
// ---------------------------------------------------------------------------
void insertReadyQueue(SchedulerData *data, Process *process)
{
    // FCFS and RR: simply append to the back of the queue
    if (data->algorithm == FCFS || data->algorithm == RR)
    {
        data->ready_queue.push_back(process);
        return;
    }

    // SJF: sorted by total remaining CPU time (shortest first)
    // PP:  sorted by priority number (0 = highest priority)
    //      ties broken by FCFS order
    std::list<Process*>::iterator it = data->ready_queue.begin();
    for (; it != data->ready_queue.end(); ++it)
    {
        if (data->algorithm == SJF)
        {
            if (process->getRemainingTime() < (*it)->getRemainingTime())
                break;
        }
        else if (data->algorithm == PP)
        {
            if (process->getPriority() < (*it)->getPriority())
                break;
        }
    }

    data->ready_queue.insert(it, process);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        std::cerr << "Error: must specify configuration file" << std::endl;
        exit(EXIT_FAILURE);
    }

    int i;
    SchedulerData *shared_data = new SchedulerData();
    std::vector<Process*> processes;

    SchedulerConfig *config = scr::readConfigFile(argv[1]);

    uint8_t num_cores = config->cores;

    shared_data->algorithm = config->algorithm;
    shared_data->context_switch = config->context_switch;
    shared_data->time_slice = config->time_slice;
    shared_data->all_terminated = false;

    uint64_t start = currentTime();

    // Build process list; immediately-ready processes go into the ready queue
    for (i = 0; i < config->num_processes; i++)
    {
        Process *p = new Process(config->processes[i], start);
        processes.push_back(p);

        if (p->getState() == Process::Ready)
        {
            std::lock_guard<std::mutex> lock(shared_data->queue_mutex);
            insertReadyQueue(shared_data, p);
        }
    }

    scr::deleteConfig(config);

    // Launch one scheduling thread per CPU core
    std::thread *schedule_threads = new std::thread[num_cores];
    for (i = 0; i < num_cores; i++)
    {
        schedule_threads[i] = std::thread(coreRunProcesses, i, shared_data);
    }

    // Ncurses setup
    initscr();
    cbreak();
    noecho();
    curs_set(0);

    // Main monitor loop
    while (!(shared_data->all_terminated))
    {
        uint64_t now = currentTime();
        std::vector<Process*> newly_ready;

        {
            std::lock_guard<std::mutex> lock(shared_data->queue_mutex);
            bool all_done = true;

            for (Process *p : processes)
            {
                Process::State old_state = p->getState();

                switch (old_state)
                {
                    case Process::NotStarted:
                        if (now - start >= p->getStartTime())
                        {
                            p->setState(Process::Ready, now);
                            p->setBurstStartTime(now);
                            insertReadyQueue(shared_data, p);
                            newly_ready.push_back(p);
                        }
                        all_done = false;
                        break;

                    case Process::Ready:
                        p->updateProcess(now);
                        all_done = false;
                        break;

                    case Process::Running:
                        all_done = false;
                        break;

                    case Process::IO:
                        p->updateProcess(now);
                        if (p->getState() == Process::Ready)
                        {
                            insertReadyQueue(shared_data, p);
                            newly_ready.push_back(p);
                        }
                        else if (p->getState() == Process::IO)
                        {
                            all_done = false;
                        }
                        break;

                    case Process::Terminated:
                        break;
                }

                if (p->getState() == Process::Ready ||
                    p->getState() == Process::Running ||
                    p->getState() == Process::IO ||
                    p->getState() == Process::NotStarted)
                {
                    all_done = false;
                }
            }

            // Round Robin: time-slice interrupt
            if (shared_data->algorithm == RR)
            {
                for (Process *p : processes)
                {
                    if (p->getState() == Process::Running &&
                        !p->isInterrupted() &&
                        now - p->getBurstStartTime() >= shared_data->time_slice)
                    {
                        p->interrupt();
                    }
                }
            }
            // Preemptive Priority: interrupt a lower-priority running process
            else if (shared_data->algorithm == PP)
            {
                for (Process *ready_p : newly_ready)
                {
                    Process *victim = nullptr;

                    for (Process *running_p : processes)
                    {
                        if (running_p->getState() == Process::Running &&
                            !running_p->isInterrupted() &&
                            ready_p->getPriority() < running_p->getPriority())
                        {
                            if (victim == nullptr ||
                                running_p->getPriority() > victim->getPriority())
                            {
                                victim = running_p;
                            }
                        }
                    }

                    if (victim != nullptr)
                    {
                        victim->interrupt();
                    }
                }
            }

            shared_data->all_terminated = all_done;
        }

        erase();
        printProcessOutput(processes);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // Wait for all worker threads
    for (i = 0; i < num_cores; i++)
    {
        schedule_threads[i].join();
    }

    // Final statistics
    erase();

    double total_cpu_time = 0.0;
    double total_turnaround = 0.0;
    double total_wait = 0.0;
    std::vector<double> completion_times;

    for (Process *p : processes)
    {
        total_cpu_time += p->getCpuTime();
        total_turnaround += p->getTurnaroundTime();
        total_wait += p->getWaitTime();
        completion_times.push_back(((double)p->getStartTime() / 1000.0) + p->getTurnaroundTime());
    }

    std::sort(completion_times.begin(), completion_times.end());

    double total_elapsed = (double)(currentTime() - start) / 1000.0;
    size_t n = completion_times.size();
    size_t first_half_count = (n + 1) / 2;
    size_t second_half_count = n - first_half_count;

    double first_half_end = (n > 0) ? completion_times[first_half_count - 1] : 0.0;
    double second_half_span = total_elapsed - first_half_end;

    double cpu_utilization = (total_elapsed > 0.0)
        ? (100.0 * total_cpu_time / ((double)num_cores * total_elapsed))
        : 0.0;

    double throughput_first = (first_half_end > 0.0)
        ? ((double)first_half_count / first_half_end)
        : 0.0;

    double throughput_second = (second_half_count > 0 && second_half_span > 0.0)
        ? ((double)second_half_count / second_half_span)
        : 0.0;

    double throughput_overall = (total_elapsed > 0.0)
        ? ((double)n / total_elapsed)
        : 0.0;

    printw("CPU utilization: %.2f%%\n", cpu_utilization);
    printw("Throughput (first 50%%): %.2f processes/sec\n", throughput_first);
    printw("Throughput (second 50%%): %.2f processes/sec\n", throughput_second);
    printw("Throughput (overall): %.2f processes/sec\n", throughput_overall);
    printw("Average turnaround time: %.2f sec\n", total_turnaround / n);
    printw("Average waiting time: %.2f sec\n", total_wait / n);
    printw("\nPress any key to exit...");
    refresh();
    getch();

    // Cleanup
    for (Process *p : processes)
    {
        delete p;
    }
    processes.clear();

    delete[] schedule_threads;
    delete shared_data;

    endwin();
    return 0;
}

// ---------------------------------------------------------------------------
// coreRunProcesses
//   Worker thread for one CPU core.
// ---------------------------------------------------------------------------
void coreRunProcesses(uint8_t core_id, SchedulerData *shared_data)
{
    const uint32_t tick_ms = 5;
    const uint32_t switch_half = shared_data->context_switch / 2;

    while (!(shared_data->all_terminated))
    {
        Process *p = nullptr;

        // Pull next ready process
        {
            std::lock_guard<std::mutex> lock(shared_data->queue_mutex);
            if (!shared_data->ready_queue.empty())
            {
                p = shared_data->ready_queue.front();
                shared_data->ready_queue.pop_front();
            }
        }

        if (p == nullptr)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(tick_ms));
            continue;
        }

        // Context switch in
        if (switch_half > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(switch_half));
        }

        uint64_t now = currentTime();
        {
            std::lock_guard<std::mutex> lock(shared_data->queue_mutex);
            p->interruptHandled();
            p->setCpuCore(core_id);
            p->setState(Process::Running, now);
            p->setBurstStartTime(now);
        }

        // Run until completion of burst or interruption
        while (true)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(tick_ms));
            now = currentTime();
            p->updateProcess(now);

            if (p->getState() != Process::Running)
            {
                break;
            }

            if (p->isInterrupted())
            {
                break;
            }
        }

        now = currentTime();
        {
            std::lock_guard<std::mutex> lock(shared_data->queue_mutex);
            p->setCpuCore(-1);

            // Interrupted while still running -> put back into ready queue
            if (p->getState() == Process::Running && p->isInterrupted())
            {
                p->interruptHandled();
                p->setState(Process::Ready, now);
                p->setBurstStartTime(now);
                insertReadyQueue(shared_data, p);
            }
            else
            {
                p->interruptHandled();

                // If it naturally transitioned to Ready (e.g., IO finished later),
                // ensure it's reinserted.
                if (p->getState() == Process::Ready)
                {
                    insertReadyQueue(shared_data, p);
                }
            }
        }

        // Context switch out
        if (switch_half > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(switch_half));
        }
    }
}

// ---------------------------------------------------------------------------
// printProcessOutput
// ---------------------------------------------------------------------------
void printProcessOutput(std::vector<Process*>& processes)
{
    printw("|   PID | Priority |    State    | Core |               Progress               |\n");
    printw("+-------+----------+-------------+------+--------------------------------------+\n");

    for (size_t i = 0; i < processes.size(); i++)
    {
        if (processes[i]->getState() != Process::NotStarted)
        {
            uint16_t pid = processes[i]->getPid();
            uint8_t priority = processes[i]->getPriority();
            std::string process_state = processStateToString(processes[i]->getState());
            int8_t core = processes[i]->getCpuCore();
            std::string cpu_core = (core >= 0) ? std::to_string(core) : "--";

            double total_time = processes[i]->getTotalRunTime();
            double completed_time = total_time - processes[i]->getRemainingTime();
            double percent = (total_time > 0.0) ? (completed_time / total_time) : 1.0;

            std::string progress = makeProgressString(percent, 36);

            printw("| %5u | %8u | %11s | %4s | %36s |\n",
                   pid,
                   priority,
                   process_state.c_str(),
                   cpu_core.c_str(),
                   progress.c_str());
        }
    }

    refresh();
}

// ---------------------------------------------------------------------------
// makeProgressString
// ---------------------------------------------------------------------------
std::string makeProgressString(double percent, uint32_t width)
{
    if (percent < 0.0) percent = 0.0;
    if (percent > 1.0) percent = 1.0;

    uint32_t n_chars = (uint32_t)(percent * width);
    std::string progress_bar(n_chars, '#');
    progress_bar.resize(width, ' ');
    return progress_bar;
}

// ---------------------------------------------------------------------------
// currentTime
// ---------------------------------------------------------------------------
uint64_t currentTime()
{
    uint64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return ms;
}

// ---------------------------------------------------------------------------
// processStateToString
// ---------------------------------------------------------------------------
std::string processStateToString(Process::State state)
{
    std::string str;
    switch (state)
    {
        case Process::NotStarted:
            str = "not started";
            break;
        case Process::Ready:
            str = "ready";
            break;
        case Process::Running:
            str = "running";
            break;
        case Process::IO:
            str = "i/o";
            break;
        case Process::Terminated:
            str = "terminated";
            break;
        default:
            str = "unknown";
            break;
    }
    return str;
}
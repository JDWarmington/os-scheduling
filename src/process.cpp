#include "process.h"
#include <algorithm>
#include <unordered_map>

static std::unordered_map<const Process*, uint64_t> last_update_times;

// Process class methods
Process::Process(ProcessDetails details, uint64_t current_time)
{
    int i;
    pid = details.pid;
    start_time = details.start_time;
    num_bursts = details.num_bursts;
    current_burst = 0;

    burst_times = new uint32_t[num_bursts];
    for (i = 0; i < num_bursts; i++)
    {
        burst_times[i] = details.burst_times[i];
    }

    priority = details.priority;
    state = (start_time == 0) ? Ready : NotStarted;

    burst_start_time = current_time;
    launch_time = (state == Ready) ? current_time : 0;

    is_interrupted = false;
    core = -1;
    turn_time = 0;
    wait_time = 0;
    cpu_time = 0;
    remain_time = 0;
    total_time = 0;

    for (i = 0; i < num_bursts; i += 2)
    {
        total_time += burst_times[i];
    }

    remain_time = total_time;
    last_update_times[this] = current_time;
}

Process::~Process()
{
    delete[] burst_times;
    last_update_times.erase(this);
}

uint16_t Process::getPid() const
{
    return pid;
}

uint32_t Process::getStartTime() const
{
    return start_time;
}

uint8_t Process::getPriority() const
{
    return priority;
}

uint64_t Process::getBurstStartTime() const
{
    return burst_start_time;
}

Process::State Process::getState() const
{
    return state;
}

bool Process::isInterrupted() const
{
    return is_interrupted;
}

int8_t Process::getCpuCore() const
{
    return core;
}

double Process::getTurnaroundTime() const
{
    return (double)turn_time / 1000.0;
}

double Process::getWaitTime() const
{
    return (double)wait_time / 1000.0;
}

double Process::getCpuTime() const
{
    return (double)cpu_time / 1000.0;
}

double Process::getRemainingTime() const
{
    return (double)remain_time / 1000.0;
}

double Process::getTotalRunTime() const
{
    return (double)total_time / 1000.0;
}

void Process::setBurstStartTime(uint64_t current_time)
{
    burst_start_time = current_time;
}

void Process::setState(State new_state, uint64_t current_time)
{
    if (state == NotStarted && new_state == Ready)
    {
        launch_time = current_time;
    }

    state = new_state;
    last_update_times[this] = current_time;
}

void Process::setCpuCore(int8_t core_num)
{
    core = core_num;
}

void Process::interrupt()
{
    is_interrupted = true;
}

void Process::interruptHandled()
{
    is_interrupted = false;
}

void Process::updateProcess(uint64_t current_time)
{
    uint64_t last_time = last_update_times[this];

    if (current_time <= last_time)
    {
        return;
    }

    uint64_t delta = current_time - last_time;
    uint64_t time_cursor = last_time;

    while (delta > 0)
    {
        switch (state)
        {
            case NotStarted:
            case Terminated:
                delta = 0;
                break;

            case Ready:
                wait_time += (int32_t)delta;
                turn_time += (int32_t)delta;
                time_cursor += delta;
                delta = 0;
                break;

            case Running:
            {
                if (current_burst >= num_bursts)
                {
                    state = Terminated;
                    delta = 0;
                    break;
                }

                uint64_t burst_remaining = burst_times[current_burst];
                uint64_t used = std::min<uint64_t>(delta, burst_remaining);

                turn_time += (int32_t)used;
                cpu_time += (int32_t)used;
                remain_time -= (int32_t)used;
                burst_times[current_burst] -= (uint32_t)used;

                delta -= used;
                time_cursor += used;

                if (burst_times[current_burst] == 0)
                {
                    current_burst++;

                    if (current_burst >= num_bursts)
                    {
                        state = Terminated;
                    }
                    else
                    {
                        state = IO;
                        burst_start_time = time_cursor;
                    }
                }

                break;
            }

            case IO:
            {
                if (current_burst >= num_bursts)
                {
                    state = Terminated;
                    delta = 0;
                    break;
                }

                uint64_t burst_remaining = burst_times[current_burst];
                uint64_t used = std::min<uint64_t>(delta, burst_remaining);

                turn_time += (int32_t)used;
                burst_times[current_burst] -= (uint32_t)used;

                delta -= used;
                time_cursor += used;

                if (burst_times[current_burst] == 0)
                {
                    current_burst++;

                    if (current_burst >= num_bursts)
                    {
                        state = Terminated;
                    }
                    else
                    {
                        state = Ready;
                        burst_start_time = time_cursor;
                    }
                }

                break;
            }
        }
    }

    last_update_times[this] = current_time;
}

void Process::updateBurstTime(int burst_idx, uint32_t new_time)
{
    if (burst_idx >= 0 && burst_idx < num_bursts)
    {
        burst_times[burst_idx] = new_time;
    }
}
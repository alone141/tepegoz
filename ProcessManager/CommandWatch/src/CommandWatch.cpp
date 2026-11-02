#include "CommandWatch.hpp"
#include "StatusTable.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace command_watch
{

namespace
{

std::string Pid(int pid)
{
    return "pid " + std::to_string(pid);
}

std::string State(status_table::ServiceState state)
{
    return std::string{status_table::StateName(state)};
}

} // namespace

std::optional<status_table::ServiceSample> FindService(const std::vector<status_table::ServiceSample>& report,
                                                       std::string_view name)
{
    for (const status_table::ServiceSample& sample : report)
    {
        if (sample.name == name)
        {
            return sample;
        }
    }
    return std::nullopt;
}

CommandWatch::CommandWatch(Command command, std::string_view target,
                           const std::vector<status_table::ServiceSample>& before)
    : command{command}
    , all{target == all_services}
{
    for (const status_table::ServiceSample& sample : before)
    {
        if (all || sample.name == target)
        {
            const bool alive = status_table::IsAlive(sample.state) && sample.pid > 0;
            tracks.push_back(Track{sample.name, alive ? sample.pid : 0, alive, sample.pid, sample.state});
        }
    }
    if (!all && tracks.empty())
    {
        tracks.push_back(Track{std::string{target}});
    }
}

WatchCode CommandWatch::Update(const std::vector<status_table::ServiceSample>& after, std::vector<std::string>& lines)
{
    // --start all and --restart all build every service again from the configuration, disabled ones too.
    const bool rebuilt = all && (command == Command::Start || command == Command::Restart);
    if (rebuilt)
    {
        for (const status_table::ServiceSample& sample : after)
        {
            const bool tracked = std::any_of(tracks.begin(), tracks.end(),
                                             [&sample](const Track& track) { return track.name == sample.name; });
            if (!tracked)
            {
                tracks.push_back(Track{sample.name});
            }
        }
    }

    for (Track& track : tracks)
    {
        if (!track.settled)
        {
            Follow(track, FindService(after, track.name), lines);
        }
    }

    const bool settled = std::all_of(tracks.begin(), tracks.end(), [](const Track& track) { return track.settled; });
    if (!settled)
    {
        return WatchCode::Waiting;
    }

    const bool failed = std::any_of(tracks.begin(), tracks.end(), [](const Track& track) { return track.failed; });
    return failed ? WatchCode::Failed : WatchCode::Done;
}

void CommandWatch::DescribeWaiting(std::chrono::seconds waited, std::vector<std::string>& lines) const
{
    const std::string after = " after " + std::to_string(waited.count()) + "s";
    for (const Track& track : tracks)
    {
        if (track.settled)
        {
            continue;
        }

        if (command == Command::Disable)
        {
            lines.push_back(track.name + ": still listed" + after);
        }
        else if (command == Command::Stop)
        {
            lines.push_back(track.name + ": still " + State(track.lastState) + after + ", " + Pid(track.lastPid) +
                            "; a stop can take twice its ForceKillTimeoutMs");
        }
        else if (track.started)
        {
            const bool starting = track.lastState == status_table::ServiceState::Starting;
            lines.push_back(track.name + ": still " + State(track.lastState) + after + ", " + Pid(track.lastPid) +
                            (starting ? ", no heartbeat yet" : ""));
        }
        else
        {
            lines.push_back(track.name + ": not started" + after +
                            "; it waits while a service ahead of it in the start order is in its grace period "
                            "or being stopped");
        }
    }
}

void CommandWatch::Follow(Track& track, const std::optional<status_table::ServiceSample>& sample,
                          std::vector<std::string>& lines) const
{
    if (command == Command::Disable)
    {
        if (!sample.has_value())
        {
            track.settled = true;
            lines.push_back(track.name + ": disabled; it comes back when the manager restarts");
        }
        return;
    }

    if (!sample.has_value())
    {
        return; // left the reports for a moment, as a service being rebuilt does
    }

    const status_table::ServiceSample& now = sample.value();
    const bool alive = status_table::IsAlive(now.state) && now.pid > 0;
    track.lastPid = now.pid;
    track.lastState = now.state;

    if (command == Command::Stop)
    {
        if (!alive)
        {
            track.settled = true;
            lines.push_back(track.name + (track.aliveBefore ? ": Stopped" : ": was not running"));
        }
        return;
    }

    // Start leaves a running service as it is; Restart, and Start for all, want a new process.
    const bool keepsRunning = command == Command::Start && !all && track.aliveBefore;
    if (keepsRunning)
    {
        if (now.state == status_table::ServiceState::Running && now.pid == track.pidBefore)
        {
            track.settled = true;
            lines.push_back(track.name + ": Running, " + Pid(now.pid) + " (it was already running)");
            return;
        }
        if (alive && now.pid == track.pidBefore)
        {
            return; // Starting or Unhealthy: wait for it to come round
        }
    }

    const bool newProcess = alive && (now.pid != track.pidBefore || !track.aliveBefore);
    if (newProcess)
    {
        track.started = true;
    }
    if (!track.started)
    {
        return;
    }

    if (!alive)
    {
        track.settled = true;
        track.failed = true;
        lines.push_back(track.name + ": started, then stopped again; its log says why");
        return;
    }

    const bool replaced = track.aliveBefore && now.pid != track.pidBefore;
    const std::string was = replaced ? " (was " + std::to_string(track.pidBefore) + ")" : "";
    if (now.state == status_table::ServiceState::Running)
    {
        track.settled = true;
        lines.push_back(track.name + ": Running, " + Pid(now.pid) + was);
    }
    else if (now.state == status_table::ServiceState::Unhealthy)
    {
        track.settled = true;
        track.failed = true;
        lines.push_back(track.name + ": Unhealthy, " + Pid(now.pid) + "; it sends no heartbeats");
    }
    else if (!track.startingShown)
    {
        track.startingShown = true;
        lines.push_back(track.name + ": Starting, " + Pid(now.pid) + was + ", waiting for its first heartbeat");
    }
}

} // namespace command_watch

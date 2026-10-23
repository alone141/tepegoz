#!/usr/bin/env python3
"""
Mock process-manager for testing the GUI.

PUB  (bind)  – broadcasts array of DetailedHealthReport, GPU use included
               (--no-gpu: as a manager on a machine without the NVIDIA driver)
ROUTER (bind) – accepts DEALER commands with:
      identity = b"GUI" (or the CLI's b"PMC")
      frame0   = b"BPM"
      frame1   = packed CommandMessage (65 B)
    and answers the GUI identity with [b"BPM"][b"1" | b"0"], as the manager does.
PUB  (bind, --network) – the network manager's status and events (network_status):
      BPM is connected; running processes connect and send heartbeats, unhealthy
      ones go quiet, stopped ones leave; the GUI joins with its first command.
      --missing NAME keeps a heartbeating process off the network manager.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import random
import signal
import sys
import time
from typing import Dict, List

import zmq

from network_status import STATUS_VERSION, format_age_ms

from health_structs import (
    COMMAND_SIZE,
    GUI_IDENTITY,
    CommandEnum,
    CommandMessage,
    DetailedHealthReport,
    GpuState,
    REPORT_SIZE,
    RuntimeState,
    make_command_message,   # only for size check
)


PROCESSES = [
    "sensor_fusion",
    "path_planner",
    "vision_pipeline",
    "control_loop",
    "telemetry_bridge",
    "logger_daemon",
]
# The processes that use a GPU: (VRAM, utilization range in percent).
GPU_USERS = {
    "vision_pipeline": (2_100 * 1024 * 1024, (35, 90)),
    "sensor_fusion":   (600 * 1024 * 1024, (5, 30)),
}


def make_report(
    name: str,
    pid: int,
    state: RuntimeState,
    mem: int,
    cpu_usec: int,
    start_ns: int,
    last_seen_ns: int,
    missed: int,
    restarts: int,
    snap_ns: int,
    gpu_state: GpuState = GpuState.SAMPLED,
    gpu_util: int = 0,
    vram: int = 0,
) -> DetailedHealthReport:
    r = DetailedHealthReport()
    name_bytes = name.encode("utf-8")[:63]
    r.processName = name_bytes + b"\x00" * (64 - len(name_bytes))
    r.pid = pid
    r.memoryUsageInBytes = mem
    r.cpuUsageInUsec = cpu_usec
    r.runtimeHealth.state = int(state)
    r.runtimeHealth.start_time = start_ns
    r.runtimeHealth.lastSeen = last_seen_ns
    r.runtimeHealth.missedBeats = missed
    r.runtimeHealth.restartCount = restarts
    r.snapshotTime = snap_ns
    r.gpuUsage.state = int(gpu_state)
    r.gpuUsage.utilizationPercent = gpu_util
    r.gpuUsage.memoryUsageInBytes = vram
    return r


def reports_to_bytes(reports: List[DetailedHealthReport]) -> bytes:
    size = len(reports) * REPORT_SIZE
    buf = (ctypes.c_char * size)()
    for i, rep in enumerate(reports):
        ctypes.memmove(
            ctypes.addressof(buf) + i * REPORT_SIZE,
            ctypes.addressof(rep),
            REPORT_SIZE,
        )
    return bytes(buf)


class MockNetwork:
    """The network manager's side: who is connected, and the status and events it publishes."""

    def __init__(self, sock: zmq.Socket, endpoint: str) -> None:
        self.sock = sock
        self.endpoint = endpoint
        self.started_ms = int(time.time() * 1000)
        self.dealers: Dict[str, dict] = {}
        self.next_port = 40000
        self.forwarded = 0
        self.not_delivered = 0
        self.last_status_ms = 0
        self.join("BPM")

    def join(self, name: str) -> None:
        now = int(time.time() * 1000)
        address = f"127.0.0.1:{self.next_port}"
        self.next_port += 1
        self.dealers[name] = {"address": address, "since": now, "messages": 1, "lastMessage": now}
        self.event("info", "joined", name, f"joined from {address}")

    def leave(self, name: str) -> None:
        dealer = self.dealers.pop(name)
        age = format_age_ms(int(time.time() * 1000) - dealer["since"]).replace(" ", "")
        self.event("info", "left", name,
                   f"left {dealer['address']} after {age}, {dealer['messages']} messages")

    def sent(self, name: str) -> None:
        dealer = self.dealers[name]
        dealer["messages"] += 1
        dealer["lastMessage"] = int(time.time() * 1000)
        self.forwarded += 1

    def event(self, level: str, kind: str, name: str, text: str) -> None:
        self.publish(b"event", {"version": STATUS_VERSION, "time": int(time.time() * 1000),
                                "level": level, "kind": kind, "name": name, "text": text})

    def publish_status_if_due(self) -> None:
        now = int(time.time() * 1000)
        if now - self.last_status_ms < 1000:
            return
        self.last_status_ms = now
        self.publish(b"status", {
            "version": STATUS_VERSION, "endpoint": self.endpoint, "started": self.started_ms, "now": now,
            "totals": {"forwarded": self.forwarded, "notDelivered": self.not_delivered, "rejected": 0},
            "dealers": [{"name": name, **d} for name, d in sorted(self.dealers.items())],
            "unnamed": [],
        })

    def publish(self, topic: bytes, payload: dict) -> None:
        self.sock.send_multipart([topic, json.dumps(payload).encode()])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pub", default="tcp://*:6667")
    parser.add_argument("--router", default="tcp://*:5557")
    parser.add_argument("--network", default="tcp://*:6669",
                        help="PUB endpoint of the network manager's status ('' for none)")
    parser.add_argument("--missing", action="append", default=[],
                        help="a process that sends heartbeats but never connects (repeatable)")
    parser.add_argument("--interval", type=float, default=0.5)
    parser.add_argument(
        "--no-gpu", action="store_true",
        help="report that the manager could not load the NVIDIA driver library",
    )
    args = parser.parse_args()

    ctx = zmq.Context()

    pub = ctx.socket(zmq.PUB)
    pub.bind(args.pub)
    print(f"[PUB]    bound to {args.pub}")

    network = None
    if args.network:
        net = ctx.socket(zmq.PUB)
        net.bind(args.network)
        network = MockNetwork(net, args.router.replace("*", "0.0.0.0"))
        print(f"[NET]    bound to {args.network}")

    router = ctx.socket(zmq.ROUTER)
    router.bind(args.router)
    print(f"[ROUTER] bound to {args.router}")
    print("Expecting DEALER identity = 'GUI' (or the CLI's 'PMC'), frames: 'BPM' + CommandMessage(65 B)\n")

    # simulated state
    states     = {n: RuntimeState.RUNNING for n in PROCESSES}
    pids       = {n: 10000 + i for i, n in enumerate(PROCESSES)}
    start_times = {
        n: time.time_ns() - random.randint(60, 3600) * 1_000_000_000
        for n in PROCESSES
    }
    cpu_base   = {n: random.randint(1_000_000, 50_000_000) for n in PROCESSES}
    mem_base   = {n: random.randint(20, 400) * 1024 * 1024 for n in PROCESSES}
    restarts   = {n: random.randint(0, 3) for n in PROCESSES}
    missed     = {n: 0 for n in PROCESSES}

    last_snap = time.time_ns()
    running = True

    def handle_sig(*_):
        nonlocal running
        running = False

    signal.signal(signal.SIGINT, handle_sig)
    signal.signal(signal.SIGTERM, handle_sig)

    poller = zmq.Poller()
    poller.register(router, zmq.POLLIN)

    print("Publishing health reports… (Ctrl+C to stop)\n")

    while running:
        # ── handle commands ──────────────────────────────────────────────
        try:
            events = dict(poller.poll(10))
        except zmq.ZMQError:
            break

        if router in events:
            try:
                # ROUTER frames: [identity][empty?][…]  we take last two meaningful
                frames = router.recv_multipart(flags=zmq.NOBLOCK)
                identity = frames[0]
                # remaining frames after identity
                payload_frames = frames[1:]
                if not payload_frames:
                    continue

                # Expect: identity | "BPM" | CommandMessage
                # (some stacks put an empty delimiter, so we are flexible)
                topic = None
                payload = None
                for f in payload_frames:
                    if f == b"BPM":
                        topic = f
                    elif len(f) == COMMAND_SIZE:
                        payload = f

                print(f"← from identity={identity!r}  topic={topic!r}  "
                      f"payload={len(payload) if payload else 0} B")

                if identity not in (GUI_IDENTITY, b"PMC"):
                    print(f"   WARNING: expected identity 'GUI' or 'PMC', got {identity!r}")
                if topic != b"BPM":
                    print(f"   WARNING: expected topic 'BPM', got {topic!r}")

                if payload and len(payload) == COMMAND_SIZE:
                    cmd_msg = CommandMessage.from_buffer_copy(payload)
                    try:
                        cmd = CommandEnum(cmd_msg.command)
                    except ValueError:
                        cmd = cmd_msg.command
                    svc  = cmd_msg.ServiceName.split(b"\x00", 1)[0].decode(errors="replace")
                    args = cmd_msg.Args.split(b"\x00", 1)[0].decode(errors="replace")
                    print(f"   CMD={cmd}  ServiceName={svc!r}  Args={args!r}")

                    accepted = svc in states and cmd in (
                        CommandEnum.START, CommandEnum.STOP, CommandEnum.RESTART
                    )
                    if svc in states:
                        if cmd == CommandEnum.START:
                            states[svc] = RuntimeState.STARTING
                            print(f"   → starting {svc}")
                        elif cmd == CommandEnum.STOP:
                            states[svc] = RuntimeState.STOPPED
                            pids[svc] = 0
                            print(f"   → stopped {svc}")
                        elif cmd == CommandEnum.RESTART:
                            states[svc] = RuntimeState.STARTING
                            restarts[svc] += 1
                            print(f"   → restarting {svc}")
                    if identity == GUI_IDENTITY:
                        # Like the manager: [BPM][b"1" accepted | b"0" refused], GUI only.
                        router.send_multipart([identity, b"BPM", b"1" if accepted else b"0"])
                    if network is not None:
                        sender = identity.decode(errors="replace")
                        if sender not in network.dealers:
                            network.join(sender)
                        network.sent(sender)
                else:
                    print(f"   payload size mismatch "
                          f"(got {len(payload) if payload else 0}, expected {COMMAND_SIZE})")
                    if payload:
                        print(f"   hex: {payload[:32].hex(' ')}…")
            except zmq.Again:
                pass
            except Exception as e:
                print(f"   command handling error: {e}")

        # ── simulate state changes ───────────────────────────────────────
        for name in PROCESSES:
            if states[name] == RuntimeState.STARTING and random.random() < 0.3:
                states[name] = RuntimeState.RUNNING
                pids[name] = random.randint(10000, 20000)
                start_times[name] = time.time_ns()
                restarts[name] += 1
            elif states[name] == RuntimeState.RUNNING and random.random() < 0.01:
                states[name] = RuntimeState.UNHEALTHY
                missed[name] += 1
            elif states[name] == RuntimeState.UNHEALTHY and random.random() < 0.05:
                states[name] = RuntimeState.STOPPED
                pids[name] = 0

        # ── publish health array ─────────────────────────────────────────
        now = time.time_ns()
        reports = []
        for name in PROCESSES:
            elapsed_us = (now - last_snap) / 1000.0
            load = random.uniform(0.05, 0.6) if states[name] == RuntimeState.RUNNING else 0.0
            cpu_base[name] += int(elapsed_us * load)

            mem = mem_base[name] + random.randint(-2_000_000, 2_000_000)
            mem = max(10 * 1024 * 1024, mem)

            last_seen = now - random.randint(0, 200_000_000)
            if states[name] in (
                RuntimeState.UNKNOWN,
                RuntimeState.STOPPED,
                RuntimeState.UNHEALTHY,
            ):
                last_seen = start_times[name]

            gpu_state = GpuState.NO_DRIVER if args.no_gpu else GpuState.SAMPLED
            gpu_util = vram = 0
            if gpu_state != GpuState.SAMPLED:
                gpu_util = -1  # what the manager sends without GPU data (memory stays 0)
            elif name in GPU_USERS and states[name] == RuntimeState.RUNNING:
                vram_base, (low, high) = GPU_USERS[name]
                vram = vram_base + random.randint(-50, 50) * 1024 * 1024
                gpu_util = random.randint(low, high)

            rep = make_report(
                name=name,
                pid=pids[name],
                state=states[name],
                mem=mem,
                cpu_usec=cpu_base[name],
                start_ns=start_times[name],
                last_seen_ns=last_seen,
                missed=missed[name],
                restarts=restarts[name],
                snap_ns=now,
                gpu_state=gpu_state,
                gpu_util=gpu_util,
                vram=vram,
            )
            reports.append(rep)

        last_snap = now
        pub.send(reports_to_bytes(reports))

        # ── network manager: heartbeats of the running, joins and leaves ─
        if network is not None:
            for name in PROCESSES:
                connected = states[name] in (RuntimeState.RUNNING, RuntimeState.UNHEALTHY)
                connected = connected and name not in args.missing
                if connected and name not in network.dealers:
                    network.join(name)
                elif not connected and name in network.dealers:
                    network.leave(name)
                if name in network.dealers and states[name] == RuntimeState.RUNNING:
                    network.sent(name)  # unhealthy ones send no heartbeat: they go quiet
            if random.random() < 0.02:
                network.not_delivered += 1
                network.event("warning", "notDelivered", "GUI -> PMC", "not delivered: PMC is not connected")
            network.publish_status_if_due()

        time.sleep(args.interval)

    print("\nShutting down…")
    pub.close(0)
    router.close(0)
    if network is not None:
        network.sock.close(0)
    ctx.term()


if __name__ == "__main__":
    main()

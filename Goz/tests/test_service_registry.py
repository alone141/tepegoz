"""Tests of registering services through the GUI: the request logic, the registry worker, and the dialog.

Run from the repository root (standard library unittest; PyQt6 and pyzmq installed):
    QT_QPA_PLATFORM=offscreen python3 -m unittest discover -s Goz/tests -v
Every socket is on 127.0.0.1 at a port the system picks, so a manager on this machine is never reached.
"""

from __future__ import annotations

import json
import os
import socket
import sys
import threading
import time
import unittest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
GUI_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if GUI_DIR not in sys.path:
    sys.path.insert(0, GUI_DIR)

import zmq  # noqa: E402
from PyQt6.QtCore import QThread, Qt  # noqa: E402
from PyQt6.QtWidgets import QApplication, QMessageBox  # noqa: E402

import mock_publisher  # noqa: E402
import service_registry as sr  # noqa: E402
from health_structs import RuntimeState  # noqa: E402

APP = None


def setUpModule():
    global APP
    APP = QApplication.instance() or QApplication([])


def free_port() -> int:
    """A port nothing listens on: the system picks it, and it is closed again at once."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def closed_endpoint() -> str:
    return f"tcp://127.0.0.1:{free_port()}"


def wait_for(condition, timeout: float = 10.0) -> bool:
    """Run the Qt event loop until condition() is true, or the time is up."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        APP.processEvents()
        if condition():
            return True
        time.sleep(0.01)
    APP.processEvents()
    return condition()


class StandIn:
    """mock_publisher's registry on a ROUTER at a random port, answered on a thread of its own.

    ``delay`` holds back the answer to the first request that many seconds; ``silent`` answers nothing.
    """

    def __init__(self, delay: float = 0.0, silent: bool = False):
        main_file, registered = mock_publisher.mock_definitions(mock_publisher.PROCESSES)
        self.sim = mock_publisher.MockServices(mock_publisher.PROCESSES + sorted(registered))
        self.registry = mock_publisher.MockRegistry(None, self.sim, main_file, registered)
        self.requests = []
        self._delay = delay
        self._silent = silent
        self._ready = threading.Event()
        self._stop = threading.Event()
        self.endpoint = ""
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        self._ready.wait(5)

    def _run(self):
        sock = zmq.Context.instance().socket(zmq.ROUTER)
        sock.setsockopt(zmq.LINGER, 0)
        sock.bind("tcp://127.0.0.1:*")
        self.endpoint = sock.getsockopt(zmq.LAST_ENDPOINT).decode()
        self._ready.set()
        held = []
        while not self._stop.is_set():
            if sock.poll(20, zmq.POLLIN):
                frames = sock.recv_multipart()
                self.requests.append([f.decode() for f in frames[1:]])
                if self._silent:
                    continue
                if self._delay and len(self.requests) == 1:
                    held.append((time.monotonic() + self._delay, frames))
                    continue
                self._answer(sock, frames)
            for item in [h for h in held if h[0] <= time.monotonic()]:
                held.remove(item)
                self._answer(sock, item[1], late=True)
        sock.close(0)

    def _answer(self, sock, frames, late=False):
        status, body = self.registry.handle(frames[1:])
        if late:
            body = dict(body, message="late answer")
        sock.send_multipart([frames[0], status.encode(), json.dumps(body).encode()])

    def close(self):
        self._stop.set()
        self._thread.join(5)


# ── pure logic ───────────────────────────────────────────────────────────────


class ArgumentsTest(unittest.TestCase):
    def test_splits_like_a_shell(self):
        self.assertEqual(
            sr.split_args("--name 'two words' \"say \\\"hi\\\"\" a\\ b --port 8080"),
            ["--name", "two words", 'say "hi"', "a b", "--port", "8080"],
        )

    def test_open_quote_is_a_problem(self):
        with self.assertRaises(ValueError):
            sr.split_args("--name 'two words")

    def test_join_gives_the_same_arguments_back(self):
        args = ["--name", "two words", "it's", "", "$HOME", "plain"]
        self.assertEqual(sr.split_args(sr.join_args(args)), args)


class BuildDefinitionTest(unittest.TestCase):
    def form(self, **values) -> sr.ServiceForm:
        form = sr.ServiceForm()
        form.name = "camera"
        form.values[sr.EXECUTABLE] = "/opt/cam/bin/server"
        form.values.update(values)
        return form

    def test_sends_only_the_keys_set(self):
        definition, problems = sr.build_definition(self.form())
        self.assertEqual(problems, {})
        self.assertEqual(definition, {"ExecutablePath": "/opt/cam/bin/server"})

    def test_typed_values(self):
        form = self.form(Args="--port 8080 --title 'Front door'", Cwd="/var/lib/cam", WorkMode="Manual",
                         RestartMode="Always", MaxRestarts="3", BackoffMultiplier="1.5", CheckHealth="true",
                         StdioAppend="false", HeartbeatPeriodMs="500")
        form.depends = ["sensor_fusion"]
        form.environment = [("MODE", "fast"), ("", "")]
        definition, problems = sr.build_definition(form)
        self.assertEqual(problems, {})
        self.assertEqual(definition, {
            "ExecutablePath": "/opt/cam/bin/server",
            "Args": ["--port", "8080", "--title", "Front door"],
            "Cwd": "/var/lib/cam",
            "WorkMode": "Manual",
            "RestartMode": "Always",
            "MaxRestarts": 3,
            "BackoffMultiplier": 1.5,
            "CheckHealth": True,
            "StdioAppend": False,
            "HeartbeatPeriodMs": 500,
            "DependsOn": ["sensor_fusion"],
            "EnvironmentVariables": {"MODE": "fast"},
        })
        self.assertIsInstance(definition["BackoffMultiplier"], float)
        self.assertEqual(json.loads(json.dumps(definition)), definition)

    def test_problems_name_their_field(self):
        form = self.form(ExecutablePath="", Args="'open", MaxRestarts="-1", BackoffMultiplier="fast")
        form.environment = [("A", "1"), ("A", "2")]
        _definition, problems = sr.build_definition(form)
        self.assertEqual(set(problems), {"ExecutablePath", "Args", "MaxRestarts", "BackoffMultiplier",
                                         "EnvironmentVariables"})

    def test_edit_keeps_every_key_the_form_does_not_show(self):
        base = {
            "ProcessName": "map_server", "ExecutablePath": "/opt/maps/bin/map_server",
            "Args": "--port 8090", "UseDisplay": True, "Display": ":0", "XAuthority": "/home/u/.Xauthority",
            "InheritParentEnvironment": False, "CreateProcessGroup": False, "Enabled": True,
            "EnviromentVariables": ["MAP_CACHE=/var/cache/maps"], "ForceKillTimeoutMs": 2000,
            "DependsOn": "sensor_fusion path_planner", "MaxRestarts": "5", "SomethingNew": {"nested": [1, 2]},
        }
        self.assertEqual(sr.other_keys(base), ["UseDisplay", "Display", "XAuthority", "InheritParentEnvironment",
                                               "CreateProcessGroup", "Enabled", "ForceKillTimeoutMs", "SomethingNew"])
        form = sr.form_from_definition(base, "map_server")
        self.assertEqual(form.values[sr.ARGS], "--port 8090")
        self.assertEqual(form.depends, ["sensor_fusion", "path_planner"])
        self.assertEqual(form.environment, [("MAP_CACHE", "/var/cache/maps")])
        definition, problems = sr.build_definition(form, base)
        self.assertEqual(problems, {})
        expected = dict(base)
        del expected["ProcessName"]  # the manager sets it from the name
        self.assertEqual(definition, expected)

    def test_edit_writes_a_changed_key_and_drops_a_cleared_one(self):
        base = {"ProcessName": "x", "ExecutablePath": "/bin/x", "Args": "a b", "Cwd": "/tmp",
                "EnviromentVariables": {"A": "1"}, "UseDisplay": True}
        form = sr.form_from_definition(base, "x")
        form.values[sr.ARGS] = "a 'b c'"
        form.values[sr.CWD] = ""
        form.environment.append(("B", "2"))
        definition, problems = sr.build_definition(form, base)
        self.assertEqual(problems, {})
        self.assertEqual(definition, {"ExecutablePath": "/bin/x", "Args": ["a", "b c"],
                                      "EnvironmentVariables": {"A": "1", "B": "2"}, "UseDisplay": True})

    def test_duplicate_drops_the_source_name(self):
        base = {"ProcessName": "map_server", "ExecutablePath": "/opt/maps/bin/map_server", "UseDisplay": False}
        form = sr.form_from_definition(base)
        self.assertEqual(form.name, "")
        definition, _problems = sr.build_definition(form, base)
        self.assertNotIn("ProcessName", definition)
        self.assertEqual(definition["UseDisplay"], False)

    def test_requests(self):
        self.assertEqual(sr.add_request("cam", {"ExecutablePath": "/x"}, True),
                         ("add", {"name": "cam", "definition": {"ExecutablePath": "/x"}, "start": True}))
        self.assertEqual(sr.add_request("cam", {}, False, dry_run=True)[1]["dryRun"], True)
        self.assertEqual(sr.replace_request("cam", {}, True),
                         ("replace", {"name": "cam", "definition": {}, "restart": True}))
        self.assertEqual(sr.remove_request("cam"), ("remove", {"name": "cam"}))
        self.assertEqual(sr.remove_request("cam", force=True), ("remove", {"name": "cam", "force": True}))

    def test_browse_only_for_a_manager_on_this_linux_machine(self):
        from unittest import mock

        with mock.patch.object(sr.sys, "platform", "linux"):
            self.assertTrue(sr.browse_allowed("tcp://127.0.0.1:6671"))
            self.assertTrue(sr.browse_allowed("tcp://localhost:6671"))
            self.assertTrue(sr.browse_allowed("tcp://[::1]:6671"))
            self.assertFalse(sr.browse_allowed("tcp://10.0.0.5:6671"))
        with mock.patch.object(sr.sys, "platform", "win32"):  # a manager reached from Windows runs in WSL
            self.assertFalse(sr.browse_allowed("tcp://127.0.0.1:6671"))

    def test_names(self):
        self.assertEqual(sr.check_name("camera-2.front_left"), "")
        for bad in ("", ".hidden", "a/b", "two words", "pm", "GUI", "x" * 32):
            self.assertNotEqual(sr.check_name(bad), "", bad)


class RepliesTest(unittest.TestCase):
    def test_ok(self):
        reply = sr.parse_reply([b"ok", b'{"version": 1, "operation": "add", "message": "\'cam\' is registered"}'])
        self.assertTrue(reply.ok)
        self.assertEqual(reply.operation, "add")
        self.assertEqual(reply.message, "'cam' is registered")

    def test_refused_with_problems(self):
        body = {"version": 1, "message": "the change would bring 1 problem", "problems": ["cam Cwd does not exists"]}
        reply = sr.parse_reply([b"refused", json.dumps(body).encode()], "add")
        self.assertEqual(reply.status, sr.REFUSED)
        self.assertEqual(reply.problems, ["cam Cwd does not exists"])
        self.assertEqual(reply.operation, "add")

    def test_malformed(self):
        for frames in ([b"ok"], [b"ok", b"not json"], [b"maybe", b"{}"], [b"ok", b"[1]"], [b"a", b"b", b"c"]):
            reply = sr.parse_reply(frames)
            self.assertEqual(reply.status, sr.ERROR, frames)
            self.assertTrue(reply.message)

    def test_list(self):
        body = {"services": [
            {"name": "a", "origin": "main", "file": "/etc/pm.yaml", "enabled": True, "loaded": True,
             "state": "Running", "pid": 12, "workMode": "Auto", "restartToApply": False},
            {"name": "b", "origin": "registered", "file": "/etc/services.d/b.yaml", "restartToApply": True},
            {"name": "c", "origin": "registered", "file": "/etc/services.d/c.yaml", "problem": "not YAML"},
        ]}
        services = sr.parse_services(sr.RegistryReply(sr.OK, body))
        self.assertEqual(sorted(services), ["a", "b", "c"])
        self.assertFalse(services["a"].registered)
        self.assertTrue(services["b"].restart_to_apply)
        self.assertEqual(sr.origin_text(services["a"]), ("Defined in the main file · /etc/pm.yaml", False))
        self.assertEqual(sr.origin_text(services["b"]), ("Registered · /etc/services.d/b.yaml", False))
        self.assertTrue(sr.origin_text(services["c"])[1])
        self.assertEqual(sr.origin_text(None), ("", False))

    def test_dependents(self):
        self.assertEqual(sr.dependents_of(["path_planner depends on 'map'", "viewer depends on 'map'"], "map"),
                         ["path_planner", "viewer"])
        self.assertEqual(sr.dependents_of(["map ExecutablePath does not exists"], "map"), [])
        self.assertEqual(sr.dependents_of([], "map"), [])

    def test_problems_go_next_to_their_fields(self):
        fields = sr.problem_fields("the change would bring 3 problems", [
            "cam ExecutablePath does not exists",
            "missing dependency: cam depends on 'nothing'",
            "service 'cam': MaxRestarts: 2000 is out of range; clamped to 1000",
            "something else entirely",
        ], "cam")
        self.assertEqual(sorted(fields), ["", "DependsOn", "ExecutablePath", "MaxRestarts"])
        self.assertEqual(fields[""], ["something else entirely"])
        self.assertEqual(sr.problem_fields("'pm' is reserved: pm, all, BPM, PMC and GUI cannot name a service",
                                           [], "pm"), {"name": ["'pm' is reserved: pm, all, BPM, PMC and GUI "
                                                                "cannot name a service"]})
        self.assertEqual(sr.problem_fields("the configuration cannot be read", [], "cam"), {})


class ActionsTest(unittest.TestCase):
    services = {
        "main_svc": sr.ServiceEntry({"name": "main_svc", "origin": "main", "file": "/etc/pm.yaml"}),
        "reg_svc": sr.ServiceEntry({"name": "reg_svc", "origin": "registered", "file": "/d/reg_svc.yaml"}),
        "broken": sr.ServiceEntry({"name": "broken", "origin": "registered", "file": "/d/broken.yaml",
                                   "problem": "not a mapping"}),
    }

    def test_all_off_while_the_registry_does_not_answer(self):
        why = sr.unreachable_reason("tcp://h:6671")
        for name in (None, "reg_svc"):
            actions = sr.registry_actions(name, None, why)
            self.assertEqual(set(actions), set(sr.ACTIONS))
            self.assertTrue(all(enabled is False and tip == why for enabled, tip in actions.values()))
        self.assertIn("RemoteRegistration: true", why)

    def test_before_the_first_answer(self):
        actions = sr.registry_actions("reg_svc", None, "")
        self.assertFalse(any(enabled for enabled, _tip in actions.values()))

    def test_main_file_service(self):
        actions = sr.registry_actions("main_svc", self.services, "")
        self.assertTrue(actions[sr.ADD][0])
        self.assertTrue(actions[sr.DUPLICATE][0])
        self.assertFalse(actions[sr.EDIT][0])
        self.assertFalse(actions[sr.REMOVE][0])
        self.assertIn("main file", actions[sr.EDIT][1])

    def test_registered_service(self):
        actions = sr.registry_actions("reg_svc", self.services, "")
        self.assertTrue(all(enabled for enabled, _tip in actions.values()))
        self.assertIn("/d/reg_svc.yaml", actions[sr.REMOVE][1])

    def test_skipped_file_and_unknown_service(self):
        actions = sr.registry_actions("broken", self.services, "")
        self.assertTrue(actions[sr.EDIT][0] and actions[sr.REMOVE][0])
        self.assertFalse(actions[sr.DUPLICATE][0])
        actions = sr.registry_actions("not_listed", self.services, "")
        self.assertTrue(actions[sr.ADD][0])
        self.assertFalse(any(actions[a][0] for a in (sr.EDIT, sr.DUPLICATE, sr.REMOVE)))


# ── the client and the worker against a stand-in ─────────────────────────────


class ClientTest(unittest.TestCase):
    def test_answers(self):
        stand_in = StandIn()
        self.addCleanup(stand_in.close)
        client = sr.RegistryClient(stand_in.endpoint)
        self.addCleanup(client.close)
        reply = client.request("list")
        self.assertTrue(reply.ok, reply)
        self.assertIn("map_server", sr.parse_services(reply))
        self.assertEqual(stand_in.requests[0], ["list"])
        reply = client.request(*sr.show_request("map_server"))
        self.assertEqual(reply.get("origin"), "registered")
        self.assertEqual(json.loads(stand_in.requests[1][1]), {"name": "map_server"})

    def test_nothing_listening_is_unreachable(self):
        client = sr.RegistryClient(closed_endpoint())
        self.addCleanup(client.close)
        started = time.monotonic()
        reply = client.request("list", connect_timeout_ms=500)
        self.assertEqual(reply.status, sr.UNREACHABLE)
        self.assertIn("not reachable", reply.message)
        self.assertLess(time.monotonic() - started, 3.0)

    def test_no_answer_in_time(self):
        stand_in = StandIn(silent=True)
        self.addCleanup(stand_in.close)
        client = sr.RegistryClient(stand_in.endpoint)
        self.addCleanup(client.close)
        reply = client.request("list", timeout_ms=300)
        self.assertEqual(reply.status, sr.UNREACHABLE)
        self.assertIn("no answer", reply.message)

    def test_a_late_answer_is_not_taken_for_the_next(self):
        stand_in = StandIn(delay=0.6)
        self.addCleanup(stand_in.close)
        client = sr.RegistryClient(stand_in.endpoint)
        self.addCleanup(client.close)
        self.assertEqual(client.request("version", timeout_ms=200).status, sr.UNREACHABLE)
        time.sleep(0.8)  # the late answer goes out to the closed socket
        reply = client.request("list")
        self.assertTrue(reply.ok)
        self.assertEqual(reply.operation, "list")
        self.assertNotEqual(reply.message, "late answer")


class WorkerTest(unittest.TestCase):
    def start_worker(self, endpoint: str):
        from goz import RegistryWorker, stop_workers

        thread = QThread()
        worker = RegistryWorker(endpoint)
        worker.moveToThread(thread)
        thread.started.connect(worker.start)
        replies = {}
        worker.reply_ready.connect(lambda request_id, reply: replies.__setitem__(request_id, reply))
        thread.start()
        self.addCleanup(lambda: stop_workers([(worker, thread)], 3000))
        return worker, replies

    def test_requests_in_order_and_endpoint_change(self):
        stand_in = StandIn()
        self.addCleanup(stand_in.close)
        worker, replies = self.start_worker(closed_endpoint())
        first = worker.request("list")
        self.assertTrue(wait_for(lambda: first in replies, 8))
        self.assertEqual(replies[first].status, sr.UNREACHABLE)
        worker.set_endpoint(stand_in.endpoint)
        ids = [worker.request("version"), worker.request(*sr.show_request("sensor_fusion")),
               worker.request(*sr.remove_request("map_server"))]
        self.assertTrue(wait_for(lambda: all(i in replies for i in ids)))
        self.assertEqual([replies[i].operation for i in ids], ["version", "show", "remove"])
        self.assertTrue(replies[ids[1]].ok)
        self.assertEqual(replies[ids[2]].status, sr.REFUSED)
        self.assertEqual(sr.dependents_of(replies[ids[2]].problems, "map_server"), ["path_planner"])

    def test_the_ui_thread_is_not_held_up(self):
        stand_in = StandIn(silent=True)
        self.addCleanup(stand_in.close)
        worker, replies = self.start_worker(stand_in.endpoint)
        started = time.monotonic()
        worker.request("list")
        self.assertLess(time.monotonic() - started, 0.1)
        self.assertFalse(wait_for(lambda: bool(replies), 0.5))


# ── the window and the dialog, offscreen ─────────────────────────────────────


class WindowTest(unittest.TestCase):
    def open_window(self, registry_endpoint: str):
        from goz import GozWindow

        window = GozWindow(
            closed_endpoint(), closed_endpoint(), closed_endpoint(),
            journal_endpoint=closed_endpoint(), registry_endpoint=registry_endpoint,
        )
        self.addCleanup(window.close)
        window.show()
        return window

    def test_add_check_and_register(self):
        stand_in = StandIn()
        self.addCleanup(stand_in.close)
        window = self.open_window(stand_in.endpoint)
        self.assertTrue(wait_for(lambda: window._registry_services is not None))
        self.assertTrue(window.btn_add_service.isEnabled())

        window.btn_add_service.click()
        dialog = window._service_dialog
        self.assertIsNotNone(dialog)
        self.assertTrue(dialog.isVisible())
        dialog.edit_name.setText("camera")
        dialog.edit_executable.setText("/opt/cam/bin/missing_server")
        dialog.edit_args.setText("--port 8080 --title 'Front door'")
        dialog.btn_check.click()
        self.assertTrue(wait_for(lambda: not dialog.busy()))
        self.assertIn("ExecutablePath does not exists", dialog.problem_text(sr.EXECUTABLE))
        self.assertIn("would bring 1 problem", dialog.result_text())

        dialog.edit_executable.setText("/opt/cam/bin/server")
        dialog.btn_advanced.setChecked(True)
        dialog._choices["RestartMode"].setCurrentIndex(dialog._choices["RestartMode"].findData("Always"))
        dialog._numbers["MaxRestarts"].setText("7")
        for i in range(dialog.list_depends.count()):
            item = dialog.list_depends.item(i)
            if item.text() == "map_server":
                item.setCheckState(Qt.CheckState.Checked)
        dialog._add_env_row("MODE", "fast")
        dialog.btn_check.click()
        self.assertTrue(wait_for(lambda: not dialog.busy()))
        self.assertEqual(dialog.problem_text(sr.EXECUTABLE), "")
        preview = dialog.preview_text()
        self.assertIn("ProcessName: camera", preview)
        self.assertIn("ExecutablePath: /opt/cam/bin/server", preview)
        self.assertIn("Front door]", preview)  # one argument, with its space
        self.assertNotIn("camera", stand_in.registry.services)  # a check writes nothing

        dialog.check_now.setChecked(False)
        dialog.btn_register.click()
        self.assertTrue(wait_for(lambda: window._service_dialog is None))
        origin, definition = stand_in.registry.services["camera"]
        self.assertEqual(origin, "registered")
        self.assertEqual(definition, {
            "ProcessName": "camera", "ExecutablePath": "/opt/cam/bin/server",
            "Args": ["--port", "8080", "--title", "Front door"], "RestartMode": "Always", "MaxRestarts": 7,
            "DependsOn": ["map_server"], "EnvironmentVariables": {"MODE": "fast"},
        })
        self.assertEqual(stand_in.sim.states["camera"], RuntimeState.UNKNOWN)  # held: Start it now was off
        self.assertIn("'camera' is registered and held", window.lbl_log.text())
        self.assertTrue(wait_for(lambda: "camera" in (window._registry_services or {})))

    def test_edit_keeps_unshown_keys_and_marks_restart_to_apply(self):
        stand_in = StandIn()
        self.addCleanup(stand_in.close)
        window = self.open_window(stand_in.endpoint)
        self.assertTrue(wait_for(lambda: window._registry_services is not None))
        before = dict(stand_in.registry.services["map_server"][1])

        window._on_registry_action(sr.EDIT, "map_server")
        self.assertTrue(wait_for(lambda: window._service_dialog is not None))
        dialog = window._service_dialog
        self.assertTrue(dialog.edit_name.isReadOnly())
        self.assertIn("UseDisplay", dialog.lbl_kept.text())
        self.assertEqual(dialog.edit_args.text(), "--port 8090 --tiles /var/lib/maps/tiles")
        dialog.edit_args.setText("--port 8091 --tiles /var/lib/maps/tiles")
        dialog.btn_register.click()
        self.assertTrue(wait_for(lambda: window._service_dialog is None))
        after = stand_in.registry.services["map_server"][1]
        self.assertEqual(after["Args"], ["--port", "8091", "--tiles", "/var/lib/maps/tiles"])
        self.assertEqual({k: v for k, v in after.items() if k != "Args"},
                         {k: v for k, v in before.items() if k != "Args"})  # UseDisplay, Cwd, … as they were
        self.assertTrue(stand_in.sim.restart_to_apply["map_server"])
        self.assertIn("keeps running", window.lbl_log.text())

        # The mark, as a report with restartToApply shows it.
        window.sidebar.set_processes({"map_server": {"state": RuntimeState.RUNNING, "restartToApply": True}})
        window.sidebar.select_process("map_server")
        self.assertTrue(wait_for(lambda: window.detail.name == "map_server"))
        window.detail.update_report({"state": RuntimeState.RUNNING, "restartToApply": True})
        self.assertFalse(window.detail.pill_apply.isHidden())
        self.assertIn("next start", window.detail.pill_apply.toolTip())
        self.assertTrue(window.detail.origin().startswith("Registered · /etc/tepegoz/services.d/map_server"))
        self.assertTrue(window.detail.registry_buttons[sr.EDIT].isEnabled())

    def test_duplicate_and_remove_refused(self):
        stand_in = StandIn()
        self.addCleanup(stand_in.close)
        window = self.open_window(stand_in.endpoint)
        self.assertTrue(wait_for(lambda: window._registry_services is not None))

        window._on_registry_action(sr.DUPLICATE, "sensor_fusion")
        self.assertTrue(wait_for(lambda: window._service_dialog is not None))
        dialog = window._service_dialog
        self.assertEqual(dialog.edit_name.text(), "")
        self.assertEqual(dialog.edit_executable.text(), "/opt/app/bin/sensor_fusion")
        dialog.reject()
        self.assertTrue(wait_for(lambda: window._service_dialog is None))

        replies = []
        window._ask_registry(*sr.remove_request("map_server"), replies.append)
        self.assertTrue(wait_for(lambda: bool(replies)))
        box, anyway = window.removal_refused_box("map_server", False, replies[0])
        self.assertIsNotNone(anyway)
        self.assertIn("path_planner", box.text())
        box, anyway = window.removal_refused_box("map_server", True, replies[0])
        self.assertIsNone(anyway)

        window._send_remove("map_server", force=True)
        self.assertTrue(wait_for(lambda: "map_server" not in stand_in.registry.services))
        self.assertTrue(wait_for(lambda: "map_server" not in (window._registry_services or {"map_server": 0})))

    def test_actions_off_without_a_registry(self):
        endpoint = closed_endpoint()
        window = self.open_window(endpoint)
        self.assertTrue(wait_for(lambda: window._registry_unavailable != "", 8))
        self.assertFalse(window.btn_add_service.isEnabled())
        self.assertIn(endpoint, window.btn_add_service.toolTip())
        self.assertIn("does not answer", window.btn_add_service.toolTip())
        menu, _commands, registry = window.sidebar.context_menu(None)
        self.assertEqual([a.isEnabled() for a in registry], [False])
        self.assertIn("RemoteRegistration", next(iter(registry)).toolTip())
        menu.deleteLater()
        # Nothing else changes: the status bar does not talk about the registry.
        self.assertNotIn("registry", window.lbl_log.text())

    def test_registry_follows_host(self):
        window = self.open_window(closed_endpoint())
        window.edit_host.setText("10.1.2.3")
        window._apply_host()
        self.assertEqual(window.registry_endpoint.rsplit(":", 1)[0], "tcp://10.1.2.3")
        self.assertEqual(window._machine(), "10.1.2.3")
        self.assertIn("REGISTRY tcp://10.1.2.3:", window.lbl_machine.toolTip())


class DialogTest(unittest.TestCase):
    def test_browse_buttons_follow_where_the_manager_is(self):
        from registry_view import ADD_MODE, ServiceDialog

        here = ServiceDialog(ADD_MODE, sr.ServiceForm(), ["a"], browse=True)
        elsewhere = ServiceDialog(ADD_MODE, sr.ServiceForm(), ["a"], browse=False)
        self.addCleanup(here.deleteLater)
        self.addCleanup(elsewhere.deleteLater)
        self.assertIsNotNone(here.btn_browse_executable)
        self.assertIsNotNone(here.btn_browse_cwd)
        self.assertIsNone(elsewhere.btn_browse_executable)
        self.assertIsNone(elsewhere.btn_browse_cwd)

    def test_problems_found_before_asking(self):
        from registry_view import ADD_MODE, ServiceDialog

        dialog = ServiceDialog(ADD_MODE, sr.ServiceForm(), [])
        self.addCleanup(dialog.deleteLater)
        sent = []
        dialog.request_wanted.connect(lambda *args: sent.append(args))
        dialog.edit_name.setText("pm")
        dialog.edit_args.setText("'open")
        dialog.btn_check.click()
        self.assertEqual(sent, [])
        self.assertIn("reserved", dialog.problem_text("name"))
        self.assertIn("required", dialog.problem_text(sr.EXECUTABLE))
        self.assertIn("quotation", dialog.problem_text(sr.ARGS))
        self.assertFalse(dialog.busy())

    def test_an_unchanged_edit_sends_the_definition_back(self):
        from registry_view import EDIT_MODE, ServiceDialog

        base = {"ProcessName": "x", "ExecutablePath": "/bin/x", "Args": ["a", "b c"], "MaxRestarts": 4,
                "BackoffMultiplier": 3.0, "CheckHealth": True, "StdioMode": "Pipe", "WorkMode": "auto",
                "DependsOn": ["gone"], "EnvironmentVariables": {"K": "v"}, "UseDisplay": True}
        dialog = ServiceDialog(EDIT_MODE, sr.form_from_definition(base, "x"), ["x", "y"], base=base)
        self.addCleanup(dialog.deleteLater)
        request, problems = dialog.request(dry_run=True)
        self.assertEqual(problems, {})
        operation, params = request
        self.assertEqual(operation, "replace")
        expected = dict(base)
        del expected["ProcessName"]
        self.assertEqual(params["definition"], expected)  # "auto" and the missing "gone" too: as written
        self.assertEqual(params["restart"], False)
        self.assertEqual(params["dryRun"], True)


class MessageBoxesDoNotBlock(unittest.TestCase):
    def test_remove_confirmation_names_the_file(self):
        from goz import GozWindow

        window = GozWindow(closed_endpoint(), closed_endpoint(), closed_endpoint(),
                                      journal_endpoint=closed_endpoint(), registry_endpoint=closed_endpoint())
        self.addCleanup(window.close)
        window._registry_services = {"cam": sr.ServiceEntry(
            {"name": "cam", "origin": "registered", "file": "/etc/tepegoz/services.d/cam.yaml"})}
        seen = []
        original = QMessageBox.question

        def answer_no(_parent, title, text, *_args):
            seen.append((title, text))
            return QMessageBox.StandardButton.No

        QMessageBox.question = staticmethod(answer_no)
        try:
            window._remove_service("cam")
        finally:
            QMessageBox.question = original
        self.assertEqual(len(seen), 1)
        self.assertIn("/etc/tepegoz/services.d/cam.yaml", seen[0][1])
        self.assertIn("<b>cam</b>", seen[0][1])


if __name__ == "__main__":
    unittest.main()

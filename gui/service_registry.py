"""Client of the manager's service registry (port 6671), and the logic of the GUI's service dialogs. No Qt.

The registry registers, changes and removes services (docs/ARCHITECTURE.md, "Service registry"):
  [operation, parameters JSON]  ->  [status, object JSON]
    operation: version | list | show | add | replace | update | remove
    status:    ok | refused (nothing was written: message, problems) | error (not understood: message)
A manager from before the registry, or one on another machine without RemoteRegistration: true, does not
answer; the client reports that as UNREACHABLE.

The form functions turn what the dialog shows into a definition and back. A definition holds only the keys the
user set, so a registered file stays short, and an edited one keeps every key the form does not show.
"""

from __future__ import annotations

import copy
import json
import re
import shlex
import sys
import time
from typing import Any, Callable, Dict, List, Optional, Sequence, Tuple

import zmq

from remote_journal import LOCAL_HOSTS, endpoint_host

REGISTRY_PORT = 6671
REGISTRY_ENDPOINT = f"tcp://127.0.0.1:{REGISTRY_PORT}"
PROTOCOL_VERSION = 1
# A request the registry has not answered by then is reported as not answered.
REPLY_TIMEOUT_MS = 10000
# A request that finds no connection to send on by then is reported as not reachable, without the reply wait.
CONNECT_TIMEOUT_MS = 3000

OK = "ok"
REFUSED = "refused"
ERROR = "error"
UNREACHABLE = "unreachable"  # the GUI's own: no connection, or no answer in time

MAIN = "main"
REGISTERED = "registered"

# Registry actions, in the order of the menus.
ADD, EDIT, DUPLICATE, REMOVE = "add", "edit", "duplicate", "remove"
ACTIONS = (EDIT, DUPLICATE, REMOVE, ADD)

NAME_PATTERN = re.compile(r"^[A-Za-z0-9_.-]{1,31}$")
RESERVED_NAMES = ("pm", "all", "BPM", "PMC", "GUI")


# ── replies ──────────────────────────────────────────────────────────────────


class RegistryReply:
    """One answer of the registry, or the GUI's UNREACHABLE when none came."""

    def __init__(self, status: str, body: Optional[dict] = None, operation: str = ""):
        self.status = status
        self.body: dict = body if isinstance(body, dict) else {}
        self.operation = operation or str(self.body.get("operation", ""))

    @property
    def ok(self) -> bool:
        return self.status == OK

    @property
    def message(self) -> str:
        text = self.body.get("message")
        if isinstance(text, str) and text:
            return text
        return {OK: "done", REFUSED: "refused", ERROR: "not understood"}.get(self.status, "no answer")

    @property
    def problems(self) -> List[str]:
        problems = self.body.get("problems")
        if not isinstance(problems, list):
            return []
        return [str(problem) for problem in problems]

    def get(self, key: str, default: Any = None) -> Any:
        return self.body.get(key, default)

    def __repr__(self) -> str:
        return f"RegistryReply({self.status!r}, {self.body!r})"


def unreachable(message: str, operation: str = "") -> RegistryReply:
    return RegistryReply(UNREACHABLE, {"message": message}, operation)


def parse_reply(frames: Sequence[bytes], operation: str = "") -> RegistryReply:
    """[status, JSON object] as the registry sends it. Anything else is an ERROR that says what came."""
    if len(frames) != 2:
        return RegistryReply(ERROR, {"message": f"the registry answered with {len(frames)} frames instead of 2"},
                             operation)
    status = frames[0].decode("utf-8", errors="replace")
    if status not in (OK, REFUSED, ERROR):
        return RegistryReply(ERROR, {"message": f"the registry answered with the unknown status {status!r}"},
                             operation)
    try:
        body = json.loads(frames[1].decode("utf-8", errors="replace"))
    except ValueError as e:
        return RegistryReply(ERROR, {"message": f"the registry's answer is not JSON: {e}"}, operation)
    if not isinstance(body, dict):
        return RegistryReply(ERROR, {"message": "the registry's answer is not a JSON object"}, operation)
    return RegistryReply(status, body, operation)


def request_frames(operation: str, params: Optional[dict] = None) -> List[bytes]:
    frames = [operation.encode("utf-8")]
    if params is not None:
        frames.append(json.dumps(params).encode("utf-8"))
    return frames


# ── the client ───────────────────────────────────────────────────────────────


class RegistryClient:
    """A DEALER to the registry, for one request at a time. Use it from one thread.

    A request left without an answer closes the socket, so a late answer can never be taken for the next one's.
    """

    def __init__(self, endpoint: str, context: Optional[zmq.Context] = None):
        self.endpoint = endpoint
        self._context = context or zmq.Context.instance()
        self._socket: Optional[zmq.Socket] = None

    def request(
        self,
        operation: str,
        params: Optional[dict] = None,
        timeout_ms: int = REPLY_TIMEOUT_MS,
        cancelled: Optional[Callable[[], bool]] = None,
        connect_timeout_ms: int = CONNECT_TIMEOUT_MS,
    ) -> RegistryReply:
        """Send one request and wait for its answer; while waiting, give up when ``cancelled`` returns True."""
        try:
            socket = self._open()
            if not self._send(socket, request_frames(operation, params), connect_timeout_ms, cancelled):
                self.close()
                return unreachable(f"the service registry at {self.endpoint} is not reachable", operation)
            deadline = time.monotonic() + timeout_ms / 1000.0
            while not socket.poll(100, zmq.POLLIN):
                if time.monotonic() >= deadline or (cancelled is not None and cancelled()):
                    self.close()
                    return unreachable(
                        f"no answer from the service registry at {self.endpoint} in {timeout_ms / 1000:.0f} s",
                        operation,
                    )
            frames = socket.recv_multipart()
        except zmq.ZMQError as e:
            self.close()
            return unreachable(f"service registry at {self.endpoint}: {e}", operation)
        return parse_reply(frames, operation)

    def close(self) -> None:
        if self._socket is not None:
            self._socket.close(linger=0)
            self._socket = None

    def _open(self) -> zmq.Socket:
        if self._socket is None:
            socket = self._context.socket(zmq.DEALER)
            socket.setsockopt(zmq.LINGER, 0)
            # Refuse to send while there is no connection, instead of queueing the request to go whenever one comes:
            # an absent registry is then told apart from a slow one.
            socket.setsockopt(zmq.IMMEDIATE, 1)
            try:
                socket.connect(self.endpoint)
            except zmq.ZMQError:
                socket.close(linger=0)
                raise
            self._socket = socket
        return self._socket

    @staticmethod
    def _send(socket: zmq.Socket, frames: List[bytes], timeout_ms: int,
              cancelled: Optional[Callable[[], bool]]) -> bool:
        deadline = time.monotonic() + timeout_ms / 1000.0
        while True:
            try:
                socket.send_multipart(frames, flags=zmq.NOBLOCK)
                return True
            except zmq.Again:
                pass
            if time.monotonic() >= deadline or (cancelled is not None and cancelled()):
                return False
            socket.poll(100, zmq.POLLOUT)


# ── what the manager has ─────────────────────────────────────────────────────


class ServiceEntry:
    """One service of a ``list`` answer: where it is defined, and how it runs."""

    def __init__(self, item: dict):
        self.name = str(item.get("name", ""))
        self.origin = str(item.get("origin", ""))
        self.file = str(item.get("file", ""))
        self.enabled = bool(item.get("enabled", True))
        self.loaded = bool(item.get("loaded", False))
        self.state = str(item.get("state", ""))
        self.pid = item.get("pid", -1)
        self.work_mode = str(item.get("workMode", ""))
        self.restart_to_apply = bool(item.get("restartToApply", False))
        self.problem = str(item.get("problem", "") or "")

    @property
    def registered(self) -> bool:
        return self.origin == REGISTERED

    def __eq__(self, other: object) -> bool:
        return isinstance(other, ServiceEntry) and vars(self) == vars(other)

    def __repr__(self) -> str:
        return f"ServiceEntry({self.name!r}, {self.origin!r}, {self.file!r})"


def parse_services(reply: RegistryReply) -> Dict[str, ServiceEntry]:
    """name -> ServiceEntry from a ``list`` answer."""
    services: Dict[str, ServiceEntry] = {}
    items = reply.get("services")
    for item in items if isinstance(items, list) else []:
        if isinstance(item, dict) and item.get("name"):
            entry = ServiceEntry(item)
            services.setdefault(entry.name, entry)  # a skipped file of a name the main file has comes second
    return services


def origin_text(entry: Optional[ServiceEntry]) -> Tuple[str, bool]:
    """Where a service is defined, for its page: (text, whether it reports a problem)."""
    if entry is None:
        return "", False
    if entry.problem:
        return f"Registered · {entry.file} · skipped: {entry.problem}", True
    if entry.registered:
        return f"Registered · {entry.file}", False
    return f"Defined in the main file · {entry.file}", False


def unreachable_reason(endpoint: str) -> str:
    """Why the registry actions are off when the registry does not answer."""
    return (
        f"The service registry at {endpoint} does not answer: the manager may be older than the registry, or it "
        "runs on another machine and its configuration does not have RemoteRegistration: true"
    )


def list_failed_reason(reply: RegistryReply) -> str:
    if reply.status == UNREACHABLE:
        return ""
    return f"The service registry cannot list the services: {reply.message}"


def registry_actions(
    name: Optional[str], services: Optional[Dict[str, ServiceEntry]], unavailable: str
) -> Dict[str, Tuple[bool, str]]:
    """Each registry action for the service ``name`` (None: no service): (enabled, tooltip saying why)."""
    if services is None or unavailable:
        why = unavailable or "Waiting for the service registry's answer"
        return {action: (False, why) for action in ACTIONS}
    actions = {ADD: (True, "Register a new service with the manager")}
    entry = services.get(name) if name else None
    if not name:
        actions.update({action: (False, "Select a service first") for action in (EDIT, DUPLICATE, REMOVE)})
        return actions
    if entry is None:
        why = f"The service registry does not list {name}"
        actions.update({action: (False, why) for action in (EDIT, DUPLICATE, REMOVE)})
        return actions
    if entry.problem:
        actions[DUPLICATE] = (False, f"{entry.file} was skipped, so there is no definition to copy: {entry.problem}")
    else:
        actions[DUPLICATE] = (True, f"A new service, filled from the definition of {name}")
    if entry.registered:
        actions[EDIT] = (True, f"Change the definition of {name} in {entry.file}")
        actions[REMOVE] = (True, f"Remove {name}: the manager deletes {entry.file} and stops the service")
    else:
        why = f"{name} is defined in the main file {entry.file}; only an edit of that file changes it"
        actions[EDIT] = (False, why)
        actions[REMOVE] = (False, why)
    return actions


# ── requests ─────────────────────────────────────────────────────────────────


def add_request(name: str, definition: dict, start: bool, dry_run: bool = False) -> Tuple[str, dict]:
    params = {"name": name, "definition": definition, "start": bool(start)}
    if dry_run:
        params["dryRun"] = True
    return "add", params


def replace_request(name: str, definition: dict, restart: bool, dry_run: bool = False) -> Tuple[str, dict]:
    params = {"name": name, "definition": definition, "restart": bool(restart)}
    if dry_run:
        params["dryRun"] = True
    return "replace", params


def remove_request(name: str, force: bool = False) -> Tuple[str, dict]:
    params: Dict[str, Any] = {"name": name}
    if force:
        params["force"] = True
    return "remove", params


def show_request(name: str) -> Tuple[str, dict]:
    return "show", {"name": name}


def dependents_of(problems: Sequence[str], name: str) -> List[str]:
    """The services a refused ``remove`` names as needing ``name``; empty when it was refused for anything else."""
    suffix = f" depends on '{name}'"
    dependents = [problem[: -len(suffix)] for problem in problems if problem.endswith(suffix)]
    return dependents if dependents and len(dependents) == len(problems) else []


def check_name(name: str) -> str:
    """Why ``name`` cannot name a registered service, or "" when it can. The manager checks it again."""
    if not name:
        return "A name is required"
    if not NAME_PATTERN.match(name):
        return "1 to 31 letters, digits, _, - and ."
    if name.startswith("."):
        return "A name cannot start with a dot"
    if name in RESERVED_NAMES:
        return "pm, all, BPM, PMC and GUI are reserved"
    return ""


def browse_allowed(endpoint: str) -> bool:
    """Whether a file chooser on this machine picks paths the manager sees: the manager runs here, on Linux.

    The manager checks the executable and folders on its own machine, and it runs on Linux only; a GUI on
    Windows that reaches a manager through localhost (WSL) would offer paths the manager does not have.
    """
    return sys.platform.startswith("linux") and endpoint_host(endpoint) in LOCAL_HOSTS


# ── the form ─────────────────────────────────────────────────────────────────

NAME = "ProcessName"
EXECUTABLE = "ExecutablePath"
ARGS = "Args"
CWD = "Cwd"
DEPENDS = "DependsOn"
ENVIRONMENT = "EnvironmentVariables"
# Older spellings the manager still reads; the form reads them and writes the new one only when it changes the value.
ALIASES: Dict[str, Tuple[str, ...]] = {ENVIRONMENT: ("EnviromentVariables",)}


class Field:
    """A single-value key of the Advanced part: its kind, its choices and the manager's default (docs/USAGE.md)."""

    def __init__(self, key: str, label: str, kind: str, default: str, choices: Tuple[str, ...] = (), tip: str = ""):
        self.key = key
        self.label = label
        self.kind = kind  # "text", "int", "float", "bool" or "choice"
        self.default = default
        self.choices = choices
        self.tip = tip


ADVANCED_FIELDS: Tuple[Field, ...] = (
    Field("WorkMode", "Work mode", "choice", "the manager's Mode", ("Auto", "Manual"),
          "Auto: started with the manager and restarted by the restart mode. Manual: waits for a start command"),
    Field("RestartMode", "Restart mode", "choice", "OnFailure", ("Never", "OnFailure", "Always", "UnlessStopped"),
          "Which exits are restarted"),
    Field("MaxRestarts", "Max restarts", "int", "5", tip="Restarts in a row before the manager gives up (0-1000)"),
    Field("RestartDelayMs", "Restart delay (ms)", "int", "1000", tip="Wait after an exit before the first restart"),
    Field("BackoffMultiplier", "Backoff multiplier", "float", "2.0",
          tip="Each further restart in a row waits this many times longer (1.0-10.0), at most 5 minutes"),
    Field("RestartResetMs", "Reset after (ms)", "int", "60000",
          tip="A run at least this long starts the count of restarts in a row again"),
    Field("CheckHealth", "Health check", "bool", "off",
          tip="The service sends heartbeats, and missing ones mark it Unhealthy"),
    Field("GracePeriodMs", "Grace period (ms)", "int", "5000", tip="After a start, heartbeats are not checked for this long"),
    Field("HeartbeatPeriodMs", "Heartbeat period (ms)", "int", "1000", tip="How often the service sends a heartbeat"),
    Field("MissedHeartbeatThreshold", "Missed beats allowed", "int", "5",
          tip="Heartbeat periods without one before the service is Unhealthy (1-1000)"),
    Field("StdioMode", "Output", "choice", "Inherit", ("Inherit", "Pipe", "File", "NullDevice"),
          "Where stdout and stderr go"),
    Field("StdioFilePath", "Output file", "text", "/tmp/output.txt",
          tip="The file, with output File, on the manager's machine. Its folder must exist"),
    Field("StdioAppend", "Append to the file", "bool", "off",
          tip="With output File: add to the file instead of starting it again at each start"),
)
FIELDS_BY_KEY: Dict[str, Field] = {field.key: field for field in ADVANCED_FIELDS}
# Every key the form shows; the dialog keeps every other key of an edited definition as it was.
FORM_KEYS: Tuple[str, ...] = (EXECUTABLE, ARGS, CWD) + tuple(f.key for f in ADVANCED_FIELDS) + (DEPENDS, ENVIRONMENT)


class ServiceForm:
    """What the service dialog shows, as texts: "" for a key the user did not set."""

    def __init__(self) -> None:
        self.name = ""
        # EXECUTABLE, ARGS (one line, shell quoting), CWD, and every ADVANCED_FIELDS key.
        self.values: Dict[str, str] = {key: "" for key in (EXECUTABLE, ARGS, CWD)}
        self.values.update({field.key: "" for field in ADVANCED_FIELDS})
        self.depends: List[str] = []
        self.environment: List[Tuple[str, str]] = []

    def advanced_set(self) -> int:
        """How many keys of the Advanced part are set."""
        count = sum(1 for field in ADVANCED_FIELDS if self.values.get(field.key))
        return count + (1 if self.depends else 0) + (1 if self.environment else 0)


def value_text(value: Any) -> str:
    """A definition's scalar as the form shows it."""
    if value is None:
        return ""
    if value is True or value is False:
        return "true" if value else "false"
    return str(value)


def split_args(text: str) -> List[str]:
    """The arguments typed on one line, split as a POSIX shell splits them. Raises ValueError for open quotes."""
    try:
        return shlex.split(text, posix=True)
    except ValueError as e:
        raise ValueError(f"cannot split the arguments: {e}") from None


def join_args(args: Sequence[str]) -> str:
    """One line that split_args turns back into ``args``."""
    return " ".join(shlex.quote(arg) for arg in args)


def _args_list(value: Any) -> List[str]:
    if isinstance(value, list):
        return [value_text(item) for item in value]
    # A text is split at spaces by the manager.
    return [part for part in value_text(value).split(" ") if part]


def _names_list(value: Any) -> List[str]:
    if isinstance(value, list):
        return [value_text(item) for item in value]
    return [part for part in value_text(value).split(" ") if part]


def _environment_pairs(value: Any) -> List[Tuple[str, str]]:
    if isinstance(value, dict):
        return [(str(key), value_text(item)) for key, item in value.items()]
    if isinstance(value, list):
        pairs = []
        for item in value:
            key, _sep, text = value_text(item).partition("=")
            pairs.append((key, text))
        return pairs
    return []


def _find(definition: dict, key: str) -> Tuple[Optional[str], Any]:
    """(the spelling the definition uses for ``key``, its value), or (None, None)."""
    for spelling in (key,) + ALIASES.get(key, ()):
        if spelling in definition:
            return spelling, definition[spelling]
    return None, None


def form_from_definition(definition: Optional[dict], name: str = "") -> ServiceForm:
    """The form filled from a definition that ``show`` returned."""
    form = ServiceForm()
    form.name = name
    definition = definition if isinstance(definition, dict) else {}
    for key in (EXECUTABLE, CWD) + tuple(f.key for f in ADVANCED_FIELDS):
        _spelling, value = _find(definition, key)
        form.values[key] = value_text(value)
    _spelling, args = _find(definition, ARGS)
    form.values[ARGS] = join_args(_args_list(args)) if args is not None else ""
    _spelling, depends = _find(definition, DEPENDS)
    form.depends = _names_list(depends) if depends is not None else []
    _spelling, environment = _find(definition, ENVIRONMENT)
    form.environment = _environment_pairs(environment) if environment is not None else []
    return form


def other_keys(definition: Optional[dict]) -> List[str]:
    """The keys of a definition the form does not show; Edit and Duplicate keep them as they are."""
    shown = set(FORM_KEYS) | {NAME} | {alias for aliases in ALIASES.values() for alias in aliases}
    return [key for key in (definition or {}) if key not in shown]


def _scalar(field_kind: str, text: str) -> Any:
    """The JSON value of a typed text. Raises ValueError with a short reason."""
    if field_kind == "int":
        if not text.isdigit():
            raise ValueError("a whole number, 0 or more")
        return int(text)
    if field_kind == "float":
        try:
            number = float(text)
        except ValueError:
            raise ValueError("a number, like 2.0") from None
        if number != number or number in (float("inf"), float("-inf")):
            raise ValueError("a number, like 2.0")
        return number
    if field_kind == "bool":
        if text not in ("true", "false"):
            raise ValueError("true or false")
        return text == "true"
    return text


def _same(key: str, kind: str, old: Any, new: Any) -> bool:
    """Whether a value the form produced says what the definition's value says, in whatever form that has."""
    if key == ARGS:
        return _args_list(old) == new
    if key == DEPENDS:
        return _names_list(old) == new
    if key == ENVIRONMENT:
        return dict(_environment_pairs(old)) == new and len(_environment_pairs(old)) == len(new)
    if kind == "float":
        try:
            return float(value_text(old)) == new
        except ValueError:
            return False
    if kind == "bool":
        return value_text(old) == value_text(new)
    return value_text(old) == value_text(new)


def build_definition(form: ServiceForm, base: Optional[dict] = None) -> Tuple[dict, Dict[str, str]]:
    """The definition to send, and the problems found in the form (key -> reason; "name" for the name).

    ``base`` is the definition the form was filled from (Edit, Duplicate): every key the form does not show is kept as
    it was, and a key the form shows but the user did not change keeps its value as written. ProcessName is left out:
    the manager sets it from the name.
    """
    problems: Dict[str, str] = {}
    produced: Dict[str, Any] = {}  # key -> new value, None for unset

    for key in (EXECUTABLE, CWD):
        produced[key] = form.values.get(key, "").strip() or None
    if produced[EXECUTABLE] is None:
        problems[EXECUTABLE] = "The executable is required"

    args_text = form.values.get(ARGS, "").strip()
    try:
        args = split_args(args_text) if args_text else []
        produced[ARGS] = args or None
    except ValueError as e:
        problems[ARGS] = str(e)
        produced[ARGS] = None

    for field in ADVANCED_FIELDS:
        text = form.values.get(field.key, "").strip()
        if not text:
            produced[field.key] = None
            continue
        _spelling, old = _find(base if isinstance(base, dict) else {}, field.key)
        if old is not None and value_text(old) == text:
            produced[field.key] = old  # shown and left as it was: kept as written, even a value the form would refuse
            continue
        try:
            produced[field.key] = _scalar(field.kind, text)
        except ValueError as e:
            problems[field.key] = str(e)
            produced[field.key] = None

    produced[DEPENDS] = [name for name in form.depends if name] or None

    environment: Dict[str, str] = {}
    for key, value in form.environment:
        key = key.strip()
        if not key and not value:
            continue
        if not key:
            problems[ENVIRONMENT] = "A variable needs a name"
        elif "=" in key or any(c.isspace() for c in key):
            problems[ENVIRONMENT] = f"{key!r}: a name has no '=' and no spaces"
        elif key in environment:
            problems[ENVIRONMENT] = f"{key} is set twice"
        else:
            environment[key] = value
    produced[ENVIRONMENT] = environment or None

    definition = copy.deepcopy(base) if isinstance(base, dict) else {}
    definition.pop(NAME, None)
    for key in FORM_KEYS:
        new = produced[key]
        kind = FIELDS_BY_KEY[key].kind if key in FIELDS_BY_KEY else "text"
        spelling, old = _find(definition, key)
        if spelling is not None and new is not None and _same(key, kind, old, new):
            continue  # unchanged: keep it as it was written
        for alias in (key,) + ALIASES.get(key, ()):
            definition.pop(alias, None)
        if new is not None:
            definition[key] = new
    return definition, problems


# ── problems next to the fields ──────────────────────────────────────────────

_KEY_PATTERNS: Tuple[Tuple[str, Any], ...] = tuple(
    (key, re.compile(r"\b(" + "|".join(re.escape(s) for s in (key,) + ALIASES.get(key, ())) + r")\b"))
    for key in FORM_KEYS
) + ((DEPENDS, re.compile(r"\bdependency\b|\bdepends on\b", re.IGNORECASE)),)


def problem_fields(message: str, problems: Sequence[str], name: str = "") -> Dict[str, List[str]]:
    """The manager's problems, by the form key each one names ("name" for the name, "" for the rest)."""
    fields: Dict[str, List[str]] = {}
    for problem in problems:
        keys = [key for key, pattern in _KEY_PATTERNS if pattern.search(problem)]
        for key in dict.fromkeys(keys) or [""]:
            fields.setdefault(key, []).append(problem)
    if not problems and name and _names_the_name(message, name):
        fields.setdefault("name", []).append(message)
    return fields


def _names_the_name(message: str, name: str) -> bool:
    """A refusal without problems that is about the name itself: missing, invalid, reserved, or taken already."""
    if message == "the service needs a name":
        return True
    words = ("is reserved", "is registered already", "defines", "is not a name", "bytes")
    return f"'{name}'" in message and any(word in message for word in words)

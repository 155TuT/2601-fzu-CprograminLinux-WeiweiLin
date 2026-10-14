"""Discover only compatible healthy loopback llama services.

The owned runtime registry stores transport location, not inference settings.
Every candidate is checked against live /health and /props. An explicit URL
always bypasses discovery. TASK4_MODEL_SERVICE_FILE isolates tests/custom runs.
"""

import http.client
import json
import os
from pathlib import Path
import stat
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_REGISTRY = ROOT / ".runtime" / "model-service.json"
LEGACY_ENDPOINT = "http://127.0.0.1:8081"
MAX_REGISTRY = 65536


def registry_path():
    return Path(os.environ.get("TASK4_MODEL_SERVICE_FILE", str(DEFAULT_REGISTRY))).absolute()


def owned_regular(descriptor, label):
    info = os.fstat(descriptor)
    if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid()
            or info.st_nlink != 1 or info.st_mode & 0o077):
        raise ValueError(f"{label} must be a private regular file owned by this user")


def runtime_directory(path):
    path = Path(path)
    path.mkdir(parents=True, exist_ok=True, mode=0o700)
    info = path.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o022:
        raise ValueError("model discovery directory must be owned and not writable by other users")
    return path


def registry_records(document):
    """Read the original single-service format and the multi-service format."""
    if document is None:
        return []
    return [document] if document["version"] == 1 else document["services"]


def read_registry(path=None):
    path = Path(path) if path is not None else registry_path()
    try:
        descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK)
    except FileNotFoundError:
        return None
    try:
        owned_regular(descriptor, "model service registry")
        with os.fdopen(descriptor, "rb", closefd=False) as source:
            content = source.read(MAX_REGISTRY + 1)
        if len(content) > MAX_REGISTRY:
            raise ValueError("model service registry is oversized")
        document = json.loads(content.decode("utf-8"))
        if not isinstance(document, dict) or document.get("version") not in (1, 2):
            raise ValueError("unsupported model service registry")
        if document["version"] == 2 and not isinstance(document.get("services"), list):
            raise ValueError("invalid model service registry list")
        from transport import normalize_config
        endpoints = set()
        for record in registry_records(document):
            if not isinstance(record, dict):
                raise ValueError("invalid model service registry record")
            endpoint = normalize_config({"endpoint": record.get("endpoint")})["endpoint"]
            if endpoint == "auto":
                raise ValueError("registry endpoint must be a concrete loopback URL")
            if endpoint in endpoints:
                raise ValueError("duplicate model service registry endpoint")
            record["endpoint"] = endpoint
            endpoints.add(endpoint)
        return document
    finally:
        os.close(descriptor)


def write_registry(path, document):
    path = Path(path)
    runtime_directory(path.parent)
    # Never follow/overwrite an unsafe pre-existing entry.
    read_registry(path)
    descriptor, temporary = tempfile.mkstemp(prefix=path.name + ".tmp-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write((json.dumps(document, ensure_ascii=False, sort_keys=True,
                                     indent=2, allow_nan=False) + "\n").encode("utf-8"))
            output.flush(); os.fsync(output.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try: os.fsync(directory)
        finally: os.close(directory)
    finally:
        try: os.unlink(temporary)
        except FileNotFoundError: pass


def compatible_properties(config, properties):
    if not isinstance(properties, dict) or properties.get("model_alias") != config["model"]:
        return False
    path = properties.get("model_path")
    if not isinstance(path, str) or not path or not config["model_file"]:
        return False
    expected = Path(config["model_file"])
    if not expected.is_absolute(): expected = ROOT / expected
    if Path(path).resolve() != expected.resolve():
        return False
    settings = properties.get("default_generation_settings", {})
    capacity = settings.get("n_ctx") if isinstance(settings, dict) else None
    slots = properties.get("total_slots")
    return (type(capacity) is int and capacity >= config["context_tokens"]
            and type(slots) is int and slots >= config["slots"]
            and isinstance(properties.get("chat_template"), str)
            and bool(properties["chat_template"]))


def probe_service(endpoint, config, timeout=1.0):
    from transport import LocalServer, normalize_config
    concrete = normalize_config({**config, "endpoint": endpoint})
    if concrete["endpoint"] == "auto":
        raise ValueError("service probe needs a concrete loopback endpoint")
    server = LocalServer(concrete, timeout=timeout)
    try:
        if server.json_request("/health").get("status") != "ok": return None
        properties = server.json_request("/props")
        return properties if compatible_properties(config, properties) else None
    except (OSError, ValueError, RuntimeError, http.client.HTTPException):
        return None


def candidates(config, path=None, preferred=LEGACY_ENDPOINT):
    document = read_registry(path)
    endpoints = [record["endpoint"] for record in registry_records(document)]
    endpoints += [preferred] if preferred else []
    return list(dict.fromkeys(endpoints))


def resolve_endpoint(config, path=None, preferred=LEGACY_ENDPOINT):
    if config["endpoint"] != "auto":
        return config["endpoint"]
    for endpoint in candidates(config, path, preferred):
        if probe_service(endpoint, config) is not None:
            return endpoint
    raise RuntimeError("no compatible healthy local model service found; run bash scripts/model/serve.sh gpu")

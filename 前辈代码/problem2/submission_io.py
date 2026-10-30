"""Local package loading and path-free diagnostic messages."""
import importlib.util
from pathlib import Path
import re
import sys


def load_local_package(directory, name):
    """Load only this directory, without importing from its parent."""
    if name not in sys.modules:
        spec = importlib.util.spec_from_file_location(
            name, directory / "__init__.py", submodule_search_locations=[str(directory)])
        module = importlib.util.module_from_spec(spec)
        sys.modules[name] = module
        spec.loader.exec_module(module)
    return name


def safe_message(value):
    """Keep error details and filenames while omitting absolute directories."""
    text = str(value)
    # Quoted paths may contain spaces; unquoted traceback paths do not.
    text = re.sub(r"([\"'])(/[^\"'\n]+)\1",
                  lambda m: m[1] + Path(m[2]).name + m[1], text)
    text = re.sub(r"(?<![\w])/(?:[^\s\"'<>]+/)*[^\s\"'<>]*",
                  lambda m: Path(m[0]).name or "<directory>", text)
    return text


def install_error_handler():
    def handle(kind, error, traceback):
        print(f"[SOLVER ERROR] {kind.__name__}: {safe_message(error)}", file=sys.stderr)
    sys.excepthook = handle

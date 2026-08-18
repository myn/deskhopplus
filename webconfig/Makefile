# The render needs jinja2 and Python 3.10 or newer — form.py uses `int | None`,
# which the system python3 on macOS (3.9) cannot parse. `make venv` builds a
# local interpreter that satisfies both; CMake looks for it there first and
# skips the render step when it is absent, so this is only needed by someone
# actually changing the config UI.

VENV    := venv
PYTHON  := $(VENV)/bin/python3

render: $(PYTHON)
	$(PYTHON) -B render.py

# Any Python 3.10+ will do. python3.13 is named first because it is what the
# macOS development machine has outside the system one.
$(PYTHON):
	@for p in python3.13 python3.12 python3.11 python3.10 python3; do \
	    if command -v $$p >/dev/null 2>&1 && \
	       $$p -c 'import sys; sys.exit(sys.version_info < (3,10))' 2>/dev/null; then \
	        echo "==> $(VENV) from $$p"; $$p -m venv $(VENV) || exit 1; \
	        $(PYTHON) -m pip install --quiet --upgrade pip || exit 1; \
	        $(PYTHON) -m pip install --quiet -r requirements.txt || exit 1; \
	        exit 0; \
	    fi; \
	done; \
	echo "no python 3.10+ found; install one to render the config UI" >&2; exit 1

venv: $(PYTHON)

clean:
	rm -rf $(VENV) __pycache__

.PHONY: render venv clean

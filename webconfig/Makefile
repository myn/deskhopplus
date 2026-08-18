# Rendering needs jinja2 and Python 3.10 or newer — form.py annotates with
# `int | None`, which the system python on macOS (3.9) cannot parse.
#
# An interpreter that already satisfies both is used as-is. The repo's own
# reproducible-build container installs py3-jinja2 for exactly that, and most
# distributions package it, so building a venv there would ignore what is
# already present and reach for the network. A venv is built only when nothing
# on the machine qualifies.

VENV   := venv
STAMP  := $(VENV)/.installed

# Prints a usable interpreter, or nothing. Never builds anything, so CMake can
# ask "is rendering possible here?" without side effects.
FIND_PYTHON = for c in python3 python3.13 python3.12 python3.11 python3.10 $(VENV)/bin/python3; do \
                  command -v $$c >/dev/null 2>&1 || continue; \
                  $$c -c 'import sys, jinja2; sys.exit(0 if sys.version_info >= (3,10) else 1)' \
                      >/dev/null 2>&1 && { echo $$c; break; }; \
              done

render:
	@p="$$($(FIND_PYTHON))"; \
	if [ -z "$$p" ]; then \
	    $(MAKE) --no-print-directory $(STAMP) || exit 1; \
	    p=$(VENV)/bin/python3; \
	fi; \
	echo "==> render with $$p"; \
	"$$p" -B render.py

# Exits 0 when the page can be rendered without building anything.
can-render:
	@test -n "$$($(FIND_PYTHON))"

# The stamp, not the interpreter, is the target: a venv whose pip step failed
# still has bin/python3, so keying off that would leave make convinced the
# environment was ready and every later render failing on a missing jinja2.
$(STAMP):
	@for c in python3.13 python3.12 python3.11 python3.10 python3; do \
	    command -v $$c >/dev/null 2>&1 || continue; \
	    $$c -c 'import sys; sys.exit(0 if sys.version_info >= (3,10) else 1)' 2>/dev/null || continue; \
	    echo "==> building $(VENV) from $$c"; \
	    rm -rf $(VENV); \
	    if $$c -m venv $(VENV) \
	       && $(VENV)/bin/python3 -m pip install --quiet -r requirements.txt; then \
	        touch $@; exit 0; \
	    fi; \
	    rm -rf $(VENV); \
	    echo "failed to build $(VENV) from $$c" >&2; exit 1; \
	done; \
	echo "no python 3.10+ found; install one to render the config UI" >&2; exit 1

venv: $(STAMP)

clean:
	rm -rf $(VENV) __pycache__

.PHONY: render venv can-render clean

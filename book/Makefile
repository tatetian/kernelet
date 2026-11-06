# Build and check Building Kernelets on Linux from this directory.
# Generated HTML goes to build/; the companion implementation is ../prototype/.
MDBOOK ?= mdbook
PYTHON ?= python3

.PHONY: all build check render vsock-plots serve clean

all: check build

build:
	$(MDBOOK) build

check:
	$(PYTHON) tools/check.py

# Optional: regenerate checked-in SVGs; normal book builds need no plotting packages.
vsock-plots:
	$(PYTHON) tools/plot-vsock.py

render: build
	tools/render.sh $(PAGE) $(OUT)

serve:
	$(MDBOOK) serve --hostname 0.0.0.0

clean:
	$(MDBOOK) clean
	rm -rf .cache/render tools/__pycache__

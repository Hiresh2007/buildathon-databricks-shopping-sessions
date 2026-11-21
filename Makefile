PYTHON ?= python3
CXX ?= c++
CARGO ?= cargo
CXXFLAGS ?= -O3
KIT := .cache/starter-duckdb
BASE := https://www.joinbuilders.org/buildathon/tracks/shopping-sessions/files
PY := .venv/bin/python

.PHONY: fixtures check-dev
fixtures: sessions .venv/.installed $(KIT)/scripts/run_fixtures.py
	$(PY) $(KIT)/scripts/run_fixtures.py $(KIT)/fixtures -- ./sessions

check-dev: sessions .venv/.installed $(KIT)/scripts/run_fixtures.py .cache/dev/.generated .cache/dev-answer/user_daily.csv
	./sessions .cache/dev .cache/dev-output
	$(PY) $(KIT)/scripts/compare.py .cache/dev-output .cache/dev-answer

sessions: sessions.cpp input.hpp identity.hpp parquet_output.hpp build.py
	CXX="$(CXX)" CXXFLAGS="$(CXXFLAGS)" $(PYTHON) build.py

.venv/.installed: requirements.txt
	$(PYTHON) -m venv .venv
	$(PY) -m pip install -r requirements.txt
	@touch $@

$(KIT)/scripts/run_fixtures.py: .cache/starter-duckdb.zip
	unzip -oq $< -d .cache
	@touch $@

.cache/dev-answer/user_daily.csv: .cache/dev-answer.zip
	unzip -oq $< -d .cache
	@touch $@

.cache/dev/.generated: .cache/generator.zip
	unzip -oq $< -d .cache
	$(CARGO) build --release --locked --manifest-path .cache/generator/Cargo.toml
	.cache/generator/target/release/generate --seed 1 --events 1000000 --clicks 250000 --output .cache/dev
	@touch $@

.cache/%.zip:
	@mkdir -p .cache
	curl -fsSL --retry 3 "$(BASE)/$*.zip" -o "$@.tmp"
	mv "$@.tmp" "$@"

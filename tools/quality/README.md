# Quality checks

The pre-commit hooks in `.pre-commit-config.yaml` use the rules in this folder:
AStyle (`astyle.yml`), Ruff (`ruff.toml`) and the project policy
(`policy.json`). From the repository root, with Python 3.8 or later:

```sh
python -m venv .venv && source .venv/bin/activate
python -m pip install -r tools/quality/requirements.txt
python -m pre_commit install
python -m pre_commit run --all-files
```

Hooks download their pinned tools on first use. The four `mosaico-*` hooks run
the ESP-Mosaico toolkit's project checks from the workspace
(`../../submodule/esp-mosaico-utils`), so this repository has to sit in an
esp-mosaico-vibe workspace's `projects/` folder for them. Formatting hooks may
change files and fail that run: review, stage and run again.

For Ruff on its own, pass `--config tools/quality/ruff.toml`.

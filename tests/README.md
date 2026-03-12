Run the regression suite with:

```bash
conda run -n websockets_working python -m unittest discover -s tests -v
```

The tests intentionally lock down behavior rather than implementation details. They cover:

- length-prefixed packet decoding
- telemetry frame parsing
- tracked joint pose validity
- CLI argument parsing
- wired vs wireless mode selection
- transport setup registration behavior
- latest-frame queue behavior
- viewer hand pose selection, formatting, and math helpers

The viewer tests are mandatory in this environment and rely on `matplotlib` being installed in `websockets_working`.

This is the safety net for later simplification work.
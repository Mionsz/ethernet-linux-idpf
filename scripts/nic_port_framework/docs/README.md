# NIC Port Framework V3.3

Manifest-driven, evidence-backed framework for semantic NIC-driver re-hosting (for example Linux IDPF -> FreeBSD).

Step-by-step commands for a full run: [../QUICK_START.md](../QUICK_START.md).
Staged runner with logging: `../run_framework.sh`.

## Stable entrypoint

```bash
python3 nic_port_pipeline.py --manifest examples/idpf.full.manifest.json validate-manifest
python3 nic_port_pipeline.py --manifest examples/idpf.full.manifest.json prepare
python3 nic_port_pipeline.py --manifest examples/idpf.full.manifest.json diagnose-extraction
```

If extraction fails and you corrected the problem, prefer:

```bash
python3 nic_port_pipeline.py --manifest examples/idpf.full.manifest.json resume
```

`resume` skips bootstrap and reuses matching successful per-TU cache records.

The canonical run exitpoint is:

```text
<output.root>/output_manifest.json
```

Start there rather than guessing artifact paths.

See `nic_port_pipeline.md` for the full architecture and V3.3 extraction/recovery model.

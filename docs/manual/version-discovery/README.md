# Background version discovery: a maintainer's guide

This implementation reference explains Launcher’s background version discovery
from configuration through scanning, presentation, actions, and shutdown.
The chapters describe the October 2026 implementation. Source, code, and
figure directives point to examples from that edition. Consult the current
source for later changes.

| Chapter | Subject |
| --- | --- |
| [About this guide](chapters/00-reading.md) | Scope, baseline and reading routes |
| [1. Architecture](chapters/01-architecture.md) | Search, attempt, catalog and presentation |
| [2. Configuration](chapters/02-configuration.md) | Wildcards, normalization and shared identity |
| [3. Scanning](chapters/03-scanning.md) | Native traversal, metadata, coverage and budgets |
| [4. Service](chapters/04-service.md) | Visits, admission, retention, pins and tombstones |
| [5. Runtime](chapters/05-runtime.md) | Ownership, atomic publication and the completion Event |
| [6. Presentation](chapters/06-presentation.md) | COM apartments, generations, pixels and blinds |
| [7. Actions](chapters/07-actions.md) | Warm/cold launches and exact-file version menus |
| [8. Diagnostics](chapters/08-errors.md) | External-error journal and Windows notifications |
| [9. Shutdown](chapters/09-shutdown.md) | Native thread exit, process boundary and restart |
| [10. Testing](chapters/10-testing.md) | Injection seams, controlled scenarios and evidence limits |
| [11. Maintenance](chapters/11-maintenance.md) | Investigation and change-review guidance |
| [12. Reference](chapters/12-reference.md) | Glossary, source map, evidence and native API references |

See the [design record](../../version-discovery-design.md) for the original
design and implementation checkpoints. The [preview instructions](../../preview-images.md)
explain how to generate a new overlay gallery.

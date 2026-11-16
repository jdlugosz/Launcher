# 11. Maintaining the implementation

## Trace a symptom to its boundary

| Symptom | First boundary to inspect |
| --- | --- |
| One installation never appears | Expanded pattern, native component matching, final-file kind and metadata coverage diagnostics. |
| A failed refresh removes known versions | Complete-only catalog replacement and absence classification at the drive/share anchor. |
| Repeated hover starts repeated scans | Current activation token, attempted activation and the binding's canonical search key. |
| A slow network prevents unrelated progress | Active capacity, attention reserve, independent jobs and whether a shared Adobe catalog is the actual bottleneck. |
| The icon describes one file but opens another | Presentation generation, coherent adoption and the warm gesture's copied prepared action. |
| A cold click executes after a later retry | Pending intention's captured terminal attempt, release of its pin and terminal-state lifetime. |
| A busy overlay remains after completion | Collection/adoption invalidation, adopted activity and interaction holds. A changed catalog is not required to clear it. |
| Exit hangs or crashes after Window cleanup | Native thread handles, the last process owner and the explicit final boundary. Shared-pointer counts alone cannot settle this. |
| A notification appears but cannot be found later | Native history, identity/registration, Windows presentation and activation as separate checks. |

Begin with the application log's attempt and retained-catalog messages, then follow the matching search key. The filesystem journal supplies the external path/code details. Add diagnostics at the responsible boundary rather than polling every icon or rebuilding all drawing resources to see whether the symptom changes.

## Reviewing a scanner change

A provider must distinguish normal end from incomplete enumeration and distinguish optional metadata absence from an unreadable file. Preserve native error domains. A new limit should report named exhaustion without truncating an action path. Directory links and namespace prefixes must keep their documented interpretation.

Ask whether plausible partial results could accidentally be labelled Complete. Also check resources retained across phases. In generic traversal, metadata reads and the USB test's metadata gate occur after directory cursors close. Adobe currently reads metadata while its layout cursor remains alive. Changing either provider's ordering changes ownership and the conditions for removable-device testing.

## Reviewing an ownership change

List everything captured by the accepted work closure. It must own each value it will use after the caller returns. A copied scanner callable is safe only if its own captures satisfy the same rule. A `shared_ptr` to one result does not repair a borrowed configuration pointer elsewhere in the closure.

Keep the service map and UI bindings on their owner thread. Do not add a mutex around the whole map merely to permit a worker callback into the Window. The current design narrows shared mutation to each job's private publication state. That is easier to reason about than locking unrelated scheduling, menus and graphics together.

When transferring a new presentation resource, identify its COM apartment and destruction point. Owned pixels are a portable result. A Shell menu interface is a live native object with a different lifetime and message-routing contract. Moving it to a thread pool without marshaling and ownership design does not make its provider safe.

## Reviewing a UI integration change

Write down the user-visible snapshot at the beginning of a gesture. If a completion races that gesture, decide whether the gesture keeps the old snapshot or becomes an explicit pending intention. It must not accidentally do both. Do not read a fresh “latest” catalog after the user has selected an older row.

Verify generation handling when a page hides, an item changes and a newer presentation request supersedes an older one. A byte-identical image can retain its graphics resources while the associated catalog/status changes. Conversely, reusing pixels must not prevent the action, capabilities or tooltip from adopting the intended generation.

## Extending the feature

A new discovery provider can implement the source/provider boundary and return the existing structured observation. It needs a typed, canonical identity that captures every option affecting discovery. Per-icon selection and launch options should continue to live in bindings.

Persisting catalogs would require a separate trust design: identity, age, installation changes and startup behavior. Increasing capacity changes thread/stack and provider-resource exposure. Native asynchronous I/O would change execution ownership and cancellation, rather than simply replace one function name. These are future choices to justify with evidence, not assumptions already built into the current implementation.

The remaining code is not all background scanning. The presentation pipeline, interaction snapshots, diagnostic channels and process boundary each protect a distinct contract. Refactoring can reduce the size of `main.cpp`, but it should preserve those boundaries rather than collapse them into one general cache object.

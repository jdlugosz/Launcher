# 7. Actions that retain the file the user selected

Background discovery makes an old assumption unsafe: the program's current default can change between mouse-down and execution. Launcher therefore treats a gesture, a deferred cold launch, and an open menu as owned actions with fixed interpretation. New information may prepare the next view; it must not silently redirect an action already being chosen.

@figure action | A warm action copies the displayed target. A cold action attaches to one attempt and captures its terminal outcome exactly once.

## Warm actions copy the displayed preparation

An ordinary click begins by copying the icon's prepared action and label. The gesture then holds presentation adoption while it requests attention and collects discoveries. If a newer version finished in that interval, the copied action still names the file the user saw. The launch worker receives owned values and performs native Shell activation outside the drawing thread.

@source src/main.cpp | // displayed target frozen through attention, collection and dispatch.
@source src/main.cpp | // Shell activation can wait for another app or extension. Keep it

Resolving a version changes the handler to `shell` and replaces its target with the selected executable. It retains configured arguments, working directory, and show mode. The final `NativeLaunchAction` pins the concrete target and converted options. If the file disappears before Windows opens it, that exact operation fails; discovery does not reinterpret the gesture as permission to open another file.

@source src/main.cpp | std::optional<launcher::LaunchAction> resolved_item_action
@source src/launch.cpp | LaunchResult launch_prepared_action

The worker creates the STA required by `ShellExecuteExW` and releases its native owners there. A plain completion result crosses back to the UI. Error presentation and drawing recovery wait for safe interaction boundaries too, including recovery after an accepted or canceled elevation transition.

## Cold actions belong to one attempt

A first click can arrive before any complete catalog exists. Launcher cannot launch a partial observation or send an unresolved wildcard back through synchronous preparation. Instead, `PendingVersionLaunch` copies the item, requested verb, reveal choice, search key, and previous attempt identity. A retained pending reference pins currently queued or running discovery across hiding or a tab switch. A queued pin also receives attention admission priority.

@source src/main.cpp | struct PendingVersionLaunch final
@source include/launcher/version_discovery.hpp | bool retain_pending

Repeating the same pending action on the same binding does not queue another launch. An explicitly different action, such as administrator execution, has its own identity. Retaining an already terminal search never begins an implicit retry. That visit's successful complete result can resolve an action immediately; otherwise the action terminates with an appropriate failure or no-matching-installation result.

When a retained attempt finishes, the collection phase captures its terminal outcome before another visit can replace it. Complete success selects from that exact snapshot. Empty, incomplete, failed, or exceptional results cannot turn partial evidence into an automatic launch. The pin is released and the intent becomes terminal, even if a held menu delays its eventual dispatch.

@code src/main.cpp | // A terminal attempt is consumed now, even if a menu/press defers | 7 | The original intention is resolved against its own terminal attempt.

A later successful scan cannot revive an earlier failed intention. This is a behavior guarantee with a lifetime consequence: hiding may discard speculative work, but it does not discard a launch the user already requested. Destruction or removal of its binding cancels that ownership explicitly.

## A menu is a frozen session

The icon context menu copies its item, version vector, selected default, and freshness wording into `VersionMenuSession`. Its entries come from the icon's adopted catalog, preserving agreement with the displayed action and artwork. Each native row carries a stable index in `MIIM_DATA`; command ranges separately distinguish Launcher, configured, version, and Shell commands.

@source src/main.cpp | struct VersionMenuSession final
@source src/main.cpp | row.dwItemData = index + 1;

Up to 50 versions appear inline. Additional entries use lazily initialized pages and bounded grouping rather than a recursively growing chain. The scanner's accepted catalog and the menu's native allocation limits remain different concepts: menu construction failure is reported as limited availability, never disguised as a complete filesystem scan.

Right-clicking an installed-version row opens commands for that exact file. Apps and Shift+F10 reach the same operation through a thread-local native menu filter, since a tracking menu does not route keys like an ordinary application window. Disabled headings, separators, and grouping rows cannot masquerade as installations.

The child menu provides Open, Run as administrator where applicable, Reveal in Explorer, Properties, configured verbs, and a lazy Windows commands submenu. The child stores an accepted choice, returns from its recursive tracking call, and then closes the parent. Dispatch happens after the outer menu unwinds, so an elevated program, Properties dialog, or another application is not started inside a fragile nested menu callback.

@source src/main.cpp | void version_row_context_menu
@source src/main.cpp | EndMenu(); // Child tracking has returned; close the parent before dispatch.
@source src/main.cpp | void dispatch_version_menu_choice

## Exact-file preparation and the Shell exception

`prepare_exact_target_action` converts an already selected absolute catalog path and its configured options without checking existence or resolving a new latest target. It also avoids resolving a shortcut to its destination. Properties deliberately addresses the selected file itself and passes no configured program arguments. Open and administrator commands preserve the launch options; reveal selects that file in Explorer.

@source src/launch.cpp | PreparedLaunch prepare_exact_target_action
@source src/launch.cpp | LaunchResult show_target_properties

Native Shell extensions impose a deliberate boundary. Launcher constructs the Windows commands submenu only when the user enters it. Its `IContextMenu` interfaces remain in their owning UI STA through population, callback forwarding, and invocation. Forwarding uses the active menu frames and their own command ranges, so a child extension's offsets cannot be interpreted against its parent.

@source include/launcher/shell_context_menu.hpp | // Keep this object alive on the UI thread through menu tracking and invocation.
@source src/main.cpp | // commands explicitly is the only operation that binds them.

Canonical open, runas, and properties commands route through Launcher's exact-file helpers. Arbitrary third-party commands retain their extension's semantics; they are not promised to obey configured show mode. Such an extension can block while its submenu is entered or invoked. Lazy construction keeps that cost away from ordinary discovery and initial Launcher commands, but does not remove the Windows apartment requirement or make an extension asynchronous.

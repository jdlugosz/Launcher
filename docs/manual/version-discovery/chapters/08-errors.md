# 8. External errors and useful diagnostics

## A failure has more than one audience

The service needs to know whether a result can replace its catalog. A maintainer needs to know what the application was doing. An operator investigating an unreliable disk needs the affected path, the native error number and Windows' explanation. These are different information needs.

The main application log records chronology, attempts, retained catalogs, worker failures and notification failures. The separate filesystem journal records external filesystem errors as independent UTF-8 JSON lines. Its entries contain a UTC timestamp, the full affected path or pattern, an error domain, the numeric code and its text. Internal normalization, allocation and budget diagnostics do not masquerade as operating-system disk failures.

```json
// Illustrative journal record, formatted for reading
{
  "path": "I:\\Version 2\\Guide.txt",
  "error_category": "Win32",
  "error_code": 32,
  "error_text": "The process cannot access the file ..."
}
```

The actual journal writes one complete object per line and includes the timestamp. Windows error values retain the system error domain. Generic adapter errors retain their own domain rather than being converted into a misleading Win32 message. Formatting native messages uses the appropriate Windows lookup and ignores insert placeholders.

@source src/filesystem_error_log.cpp | bool is_external_filesystem_error(
@source src/filesystem_error_log.cpp | std::string record_for(

## Logging happens before UI adoption

The scanner closure owns a shared diagnostics object. It appends relevant errors on the worker before returning its result for publication. A hidden tab, held menu or destroyed Window therefore does not prevent an already accepted worker from recording the failure.

Healthy scans return from the journal's fast path before acquiring its mutex. They do not wait behind another worker's slow journal append. Actual error appends serialize within the shared journal instance. This is a best-effort diagnostic channel, so a write, rotation or allocation failure returns failure to the caller without discarding an otherwise valid discovery observation.

The UI notices the shared write-failure indication and records a compact internal-log message. It does not synchronously retry filesystem writes while painting or handling pointer motion. The native error remains an error observation even when its diagnostic destination is unavailable.

@code src/filesystem_error_log.cpp | bool FilesystemErrorLog::append( | 10 | Healthy scans bypass journal serialization
@source src/main.cpp | void capture_version_results(

The journal follows the normal log's parent directory and filename stem. The default name is `launcher.filesystem-errors.log`. Rotation uses a nominal 256 KiB limit and keeps complete records, including a single oversized record. The newest backup has a `.1` suffix. Earlier segments stay available for at least 48 hours from their last write. A journal destination on a slow device can itself block an error-reporting worker. The runtime's ownership and shutdown rules still apply to that work.

## Notification suppression is per search

The UI-owned `PerSearchNoticeState` compares sorted, distinct sets of external error signatures. A signature contains the full path, native-versus-generic domain and numeric code. Enumeration order and duplicate diagnostics do not create a new error set.

A successful complete scan clears suppression, allowing a later recurrence to notify again. An internal-only failure or a result that omitted diagnostics does not establish filesystem recovery. The policy stages a changed set, requests native delivery, and commits suppression only after the request succeeds. Failed delivery remains eligible on a later observation.

Multiple newly observed faults in one collection batch share one notification request. The persistent native adapter also uses one fixed tag/group entry, so later accepted notifications replace that entry rather than accumulate a row for every failing file.

@source src/filesystem_error_notice.cpp | bool PerSearchNoticeState::observe(
@source src/filesystem_error_notice.cpp | void PerSearchNoticeState::mark_shown()

@figure errors | Worker-side recording and UI-owned notification policy share the observation without sharing a Window pointer.

## Persistent Windows app notifications

`AppNotification` uses Windows SDK ABI interfaces and WRL to create a silent `ToastGeneric` notification. Normal and instrumented applications have distinct AUMIDs and COM activation CLSIDs. Per-user registration records the display identity, custom activator and a quoted local-server command. The adapter does not clear notification history merely because the application exits.

Activation validates the application identity and an absolute filesystem-journal path, then stores that path in an owned bounded inbox. It signals its activation Event and, while the dock runs, a duplicated completion Event. The callback retains no Window pointer. The UI drains the inbox and opens the validated journal through the fixed Windows Notepad executable on owned worker work.

After Launcher exits, COM can start the executable with `--notification-activated` and its appended `-Embedding` argument. This small startup mode registers and pumps its activator before loading any personal configuration or scenario fixture. It opens the journal without creating another dock. The callback payload supplies the path through COM rather than through a loosely interpreted command line.

@source src/app_notification.cpp | HRESULT AppNotification::register_server(
@source src/app_notification.cpp | HRESULT AppNotification::show(
@source src/main.cpp | int activate_filesystem_notification()

Windows controls whether and when a popup appears and how Notification Center presents it. An accepted API request does not prove that an operator saw a banner. The retained evidence verifies history persistence and direct warm/cold COM activation to the exact journal. An actual click on the replacement notification has not been exercised. The operator also reported difficulty opening the Windows notification panel. That remains a visible-interaction verification gap, rather than a reason to claim an end-to-end click test passed.

Ordinary smoke and `--test-ui` runs do not register or display normal notifications. An explicitly opted-in instrumented scenario uses its separate test identity. This isolation prevents a temporary build-output executable from replacing the installed application's activation command.

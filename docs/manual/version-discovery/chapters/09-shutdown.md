# 9. Shutdown, restart, and providers that never return

Shared ownership solves the lifetime of a job's inputs and result. It does not by itself make process shutdown safe. A blocked worker can still be executing C++ or a provider after its window, service, and logging owners have gone away. Ordinary CRT and static teardown must not run concurrently with that worker merely because no application object still wants its answer.

@figure shutdown | Normal cleanup finishes first. Native exit observation then decides whether ordinary process teardown is safe.

## Publication is earlier than thread exit

A worker publishes its result before the executor's thread function finishes. The executor clears the `Work` object and its arbitrary captures, signals the completion Event, and then returns. Even after collection sees ready and releases a service slot, Windows may not yet have marked that thread handle signaled.

The runtime therefore keeps every accepted `std::thread` registered. Reaping uses a zero-time native wait and joins only a thread already observed exited. It never joins live provider work on the UI thread. Scheduling counts active discovery jobs, not a strict instantaneous count of native thread handles; finished threads can briefly remain registered while their exits are observed.

@source src/version_discovery_runtime.cpp | std::size_t NativeVersionDiscoveryExecutor::reap_exited()
@source include/launcher/version_discovery_runtime.hpp | // A published job need not have exited its native thread yet.

## Explicit cleanup, then a process boundary

Closing the window first stops new admission, cancels pending launch intentions, deactivates discovery, and releases UI-owned work. The window removes its hooks, tooltips, graphics, notification registration, and other native owners. The application also performs its normal persistence, logging, COM, and restart cleanup before the final runtime decision.

`BackgroundLifetime` is constructed before these owners and destroyed after them, including when the outer exception boundary unwinds. It carries the intended exit code. Its final call reaps exited threads; if none remain, ordinary C++ and process teardown can proceed.

@source src/main.cpp | class BackgroundLifetime final
@source src/main.cpp | case WM_DESTROY:

If native work is still live, the runtime instead terminates its own entire process with `TerminateProcess`. This intentionally skips later CRT/static teardown. It neither detaches threads and hopes for the best nor forcibly terminates one worker while continuing to use its process. The explicit cleanup boundary is the reason settings and owned UI resources can still be handled before that final choice.

@code src/version_discovery_runtime.cpp | void NativeVersionDiscoveryExecutor::finish_process_if_busy | 11 | The final process decision runs after explicit application-owner cleanup.

Completion infrastructure failures have their own emergency exit codes. A failed `SetEvent`, for example, cannot silently leave a permanently pending icon. That emergency worker path has no promise to flush logs or execute the normal UI cleanup. Destruction of a runtime still owning live threads without first using the explicit final boundary is an invariant failure.

@source src/version_discovery_runtime.cpp | [[noreturn]] void end_process

## Restart is a handoff, not an early exit

Restart begins on its own worker while the current dock remains usable. The replacement initializes hidden with inherited parent, ready, cancel, and error channels. Successful startup means it is ready and waiting for the old process to exit; it has not yet revealed a second dock or started sharing the parent's log.

@source include/launcher/restart.hpp | // Success means the replacement initialized while hidden and is waiting for
@source src/main.cpp | void start_restart()

When the parent receives readiness, it commits the handoff and closes normally. The replacement waits for the parent's actual process exit before resuming its log and revealing. If preparation fails or cancellation wins, the parent remains and reports the failure. The restart worker is separately stop-aware and joined during window destruction. Its transient startup-monitor timer is unrelated to version discovery's timer-free idle behavior.

@source src/restart.cpp | LaunchResult complete_restart_handoff
@source src/main.cpp | void finish_restart()

## What the bounds do not promise

Sixteen admitted discovery jobs are finite protection against blocked providers, not a recovery mechanism for them. If all slots are held, further work remains honestly queued. Hiding a tab cannot cancel an in-progress `FindNext`, metadata operation, image decoder, or Shell call. The thread-local critical-error policy helps removed drives return errors without Windows critical-error dialogs, but cannot make every network provider respond.

The final process boundary avoids racing language-runtime teardown; it does not promise bounded kernel or provider reaping time. A future requirement to recover capacity from a permanently stuck provider while continuing the same application would call for a stronger isolation boundary, such as a helper process. That is a different architecture from the delivered worker design.

@source src/version_discovery_runtime.cpp | struct ThreadFilesystemErrorMode final
@source include/launcher/version_discovery_runtime.hpp | // process before CRT/static teardown; it does not terminate one thread or
